// Immersive OpenXR frontend: the game on a floating screen, with the controllers ours.
//
// Theater mode, built on XrCompositionLayerQuad. The compositor is handed one flat
// texture and places it in space, reprojecting it at display rate -- so head tracking
// stays smooth no matter how slowly the game itself renders, and no per-eye rendering
// is needed yet. Full 3D later replaces this single quad with a projection layer and
// per-eye u_proj/u_view; the session, swapchain and input code below are unchanged by
// that.
//
// The disc image is read from the app's external files directory:
//   /sdcard/Android/data/<package>/files/game.iso
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "vr_config.h"
#include <sys/stat.h>

// Non-zero while the player is on the course: set as the countdown starts and cleared
// when the race ends. Zero through boot, the menus, course select, loading, the course
// overview flyover, the pre-race rider cinematic and the results screen. It holds the
// course's wave height as a float -- 3.0 on Dolphin Park -- which is simply the race
// parameter that happens to be live exactly when a race is. Specific to the supported
// disc (see the Supported version section).
static constexpr uint32_t kRaceActiveAddr = 0x806193BC;
#include "gx/render.h"
#include "gx/render_gl.h"
#include "gx/gl.h"
#include "hw/pad.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define TAG "waverace"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

uint32_t boot_load(const char* iso_path);

// The quad's texture. 4:3 to match the game, large enough that the panel is legible.
static constexpr int SWAP_W = 1024, SWAP_H = 768;

static bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    LOGE("%s failed: %d", what, (int)r);
    return false;
}
#define XR_TRY(expr) do { if (!xr_ok((expr), #expr)) return false; } while (0)

// ---------------------------------------------------------------------------
// Pipe printf/fprintf into logcat; they go nowhere in an Android app.
// ---------------------------------------------------------------------------
static void* log_pump(void*) {
    int fds[2];
    if (pipe(fds) != 0) return nullptr;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf) - 1)) > 0) {
        if (buf[n - 1] == '\n') n--;
        buf[n] = 0;
        __android_log_write(ANDROID_LOG_INFO, TAG, buf);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// EGL. Immersive VR renders into swapchain images, so the surface only has to exist.
// ---------------------------------------------------------------------------
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLConfig g_cfg;

static void* gl_proc(const char* name) {
    static void* lib = [] {
        void* h = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        return h;
    }();
    if (lib) {
        if (void* p = dlsym(lib, name)) return p;
    }
    return (void*)eglGetProcAddress(name);
}

static bool egl_init() {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, nullptr, nullptr)) { LOGE("eglInitialize failed"); return false; }
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) { LOGE("no ES3 config"); return false; }
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g_surf = eglCreatePbufferSurface(g_dpy, g_cfg, pb);
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) { LOGE("eglMakeCurrent failed"); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// OpenXR
// ---------------------------------------------------------------------------
struct Xr {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;          // the theater quad
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;

    // One swapchain per eye for the stereo projection layer.
    struct Eye {
        XrSwapchain handle = XR_NULL_HANDLE;
        int32_t w = 0, h = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> fbos;
    } eyes[2];
    GLuint eye_depth = 0;   // shared: the eyes render one after the other
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;

    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction a_btn, b_btn, x_btn, y_btn, menu, trig_l, trig_r, grip_r, stick_l, stick_r;
    XrAction toggle;   // right thumbstick click: force between theater and stereo
};
static Xr g_xr;
static VrConfig g_vrcfg;

// ---------------------------------------------------------------------------
// Matrices, column-major for glUniformMatrix4fv with transpose = GL_FALSE.
// ---------------------------------------------------------------------------
static void mat_proj(const XrFovf& fov, float nearZ, float farZ, float* m) {
    const float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    const float u = tanf(fov.angleUp), d = tanf(fov.angleDown);
    const float w = r - l, h = u - d;
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / w;
    m[5] = 2.0f / h;
    m[8] = (r + l) / w;
    m[9] = (u + d) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

// World-to-eye for a view-space vertex. The game's camera is treated as the origin of
// the reference space, so head rotation looks around from wherever the chase camera
// is, and the eye offset gives the stereo separation. Positions are converted from
// metres into game units on the way in.
static void mat_view(const XrPosef& pose, const VrConfig& c, float* m) {
    const XrQuaternionf& q = pose.orientation;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    // R in column-major: r[col * 3 + row].
    const float r[9] = {
        1 - 2 * (yy + zz), 2 * (xy + wz),     2 * (xz - wy),
        2 * (xy - wz),     1 - 2 * (xx + zz), 2 * (yz + wx),
        2 * (xz + wy),     2 * (yz - wx),     1 - 2 * (xx + yy),
    };
    const float t[3] = {
        pose.position.x * c.units_per_metre + c.offset_x,
        pose.position.y * c.units_per_metre + c.offset_y,
        pose.position.z * c.units_per_metre + c.offset_z,
    };
    // m = transpose(R) * translate(-t), i.e. the inverse of the eye's pose.
    m[0] = r[0]; m[1] = r[3]; m[2] = r[6]; m[3] = 0;
    m[4] = r[1]; m[5] = r[4]; m[6] = r[7]; m[7] = 0;
    m[8] = r[2]; m[9] = r[5]; m[10] = r[8]; m[11] = 0;
    m[12] = -(m[0] * t[0] + m[4] * t[1] + m[8] * t[2]);
    m[13] = -(m[1] * t[0] + m[5] * t[1] + m[9] * t[2]);
    m[14] = -(m[2] * t[0] + m[6] * t[1] + m[10] * t[2]);
    m[15] = 1;
}

static bool xr_create_instance(android_app* app) {
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                        (PFN_xrVoidFunction*)&xrInitializeLoaderKHR)) ||
        !xrInitializeLoaderKHR) {
        LOGE("xrInitializeLoaderKHR unavailable");
        return false;
    }
    XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    init.applicationVM = app->activity->vm;
    init.applicationContext = app->activity->clazz;
    XR_TRY(xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR*)&init));

    const char* exts[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                          XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = app->activity->vm;
    android_info.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_info;
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = exts;
    strcpy(ci.applicationInfo.applicationName, "Wave Race");
    strcpy(ci.applicationInfo.engineName, "bluestorm-recomp");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XR_TRY(xrCreateInstance(&ci, &g_xr.instance));

    XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
    sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_TRY(xrGetSystem(g_xr.instance, &sys, &g_xr.system));
    return true;
}

static bool xr_create_session() {
    // Required before xrCreateSession, even though the result is only advisory here.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(g_xr.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&getReq);
    if (getReq) {
        XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        getReq(g_xr.instance, g_xr.system, &req);
    }

    XrGraphicsBindingOpenGLESAndroidKHR bind{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    bind.display = g_dpy;
    bind.config = g_cfg;
    bind.context = g_ctx;

    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &bind;
    ci.systemId = g_xr.system;
    XR_TRY(xrCreateSession(g_xr.instance, &ci, &g_xr.session));

    XrReferenceSpaceCreateInfo sp{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    sp.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    sp.poseInReferenceSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateReferenceSpace(g_xr.session, &sp, &g_xr.space));
    return true;
}

static bool xr_create_swapchain() {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_xr.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(g_xr.session, n, &n, formats.data());
    // The renderer writes linear RGBA, so prefer a linear format over sRGB to avoid a
    // second gamma encode.
    int64_t chosen = formats.empty() ? 0 : formats[0];
    for (int64_t f : formats) if (f == GL_RGBA8) { chosen = f; break; }
    LOGI("swapchain format 0x%llx", (unsigned long long)chosen);

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = chosen;
    ci.sampleCount = 1;
    ci.width = SWAP_W;
    ci.height = SWAP_H;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_TRY(xrCreateSwapchain(g_xr.session, &ci, &g_xr.swapchain));

    xrEnumerateSwapchainImages(g_xr.swapchain, 0, &n, nullptr);
    g_xr.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    XR_TRY(xrEnumerateSwapchainImages(g_xr.swapchain, n, &n,
                                      (XrSwapchainImageBaseHeader*)g_xr.images.data()));

    // One framebuffer per swapchain image, so the renderer can blit straight in.
    g_xr.fbos.resize(n);
    glGenFramebuffers(n, g_xr.fbos.data());
    for (uint32_t i = 0; i < n; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_xr.fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               g_xr.images[i].image, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOGE("swapchain fbo %u incomplete", i);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("quad swapchain %dx%d, %u images", SWAP_W, SWAP_H, n);

    // Per-eye swapchains for the stereo projection layer, at whatever the runtime
    // recommends for this headset.
    uint32_t nv = 0;
    xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
    std::vector<XrViewConfigurationView> vcs(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_TRY(xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             nv, &nv, vcs.data()));
    if (nv < 2) { LOGE("expected 2 views, got %u", nv); return false; }

    for (int e = 0; e < 2; e++) {
        auto& eye = g_xr.eyes[e];
        eye.w = (int32_t)vcs[e].recommendedImageRectWidth;
        eye.h = (int32_t)vcs[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo ec{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ec.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ec.format = chosen;
        ec.sampleCount = 1;
        ec.width = eye.w;
        ec.height = eye.h;
        ec.faceCount = 1;
        ec.arraySize = 1;
        ec.mipCount = 1;
        XR_TRY(xrCreateSwapchain(g_xr.session, &ec, &eye.handle));

        uint32_t en = 0;
        xrEnumerateSwapchainImages(eye.handle, 0, &en, nullptr);
        eye.images.assign(en, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_TRY(xrEnumerateSwapchainImages(eye.handle, en, &en,
                                          (XrSwapchainImageBaseHeader*)eye.images.data()));
        eye.fbos.resize(en);
        glGenFramebuffers(en, eye.fbos.data());
    }

    // One depth buffer, shared: the eyes are rendered in sequence, and it is cleared
    // for each. Both eyes use the same recommended size.
    glGenRenderbuffers(1, &g_xr.eye_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_xr.eye_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_xr.eyes[0].w, g_xr.eyes[0].h);
    for (int e = 0; e < 2; e++) {
        for (size_t i = 0; i < g_xr.eyes[e].fbos.size(); i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_xr.eyes[e].fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   g_xr.eyes[e].images[i].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                      g_xr.eye_depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                LOGE("eye %d fbo %zu incomplete", e, i);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("eye swapchains %dx%d", g_xr.eyes[0].w, g_xr.eyes[0].h);
    return true;
}

// ---------------------------------------------------------------------------
// Input. Touch controllers -> GC pad 1.
// ---------------------------------------------------------------------------
static XrPath xr_path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_xr.instance, s, &p);
    return p;
}

static XrAction make_action(const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, label);
    XrAction a = XR_NULL_HANDLE;
    xrCreateAction(g_xr.action_set, &ci, &a);
    return a;
}

static bool xr_create_actions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "gameplay");
    strcpy(si.localizedActionSetName, "Gameplay");
    XR_TRY(xrCreateActionSet(g_xr.instance, &si, &g_xr.action_set));

    g_xr.a_btn   = make_action("a_button", "A", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.b_btn   = make_action("b_button", "B", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.x_btn   = make_action("x_button", "X", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.y_btn   = make_action("y_button", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.menu    = make_action("menu", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.trig_l  = make_action("trigger_l", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    g_xr.trig_r  = make_action("trigger_r", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    g_xr.grip_r  = make_action("grip_r", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT);
    g_xr.stick_l = make_action("stick_l", "Left stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_xr.stick_r = make_action("stick_r", "Right stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_xr.toggle  = make_action("view_toggle", "Toggle view", XR_ACTION_TYPE_BOOLEAN_INPUT);

    const XrActionSuggestedBinding binds[] = {
        {g_xr.a_btn,   xr_path("/user/hand/right/input/a/click")},
        {g_xr.b_btn,   xr_path("/user/hand/right/input/b/click")},
        {g_xr.x_btn,   xr_path("/user/hand/left/input/x/click")},
        {g_xr.y_btn,   xr_path("/user/hand/left/input/y/click")},
        {g_xr.menu,    xr_path("/user/hand/left/input/menu/click")},
        {g_xr.trig_l,  xr_path("/user/hand/left/input/trigger/value")},
        {g_xr.trig_r,  xr_path("/user/hand/right/input/trigger/value")},
        {g_xr.grip_r,  xr_path("/user/hand/right/input/squeeze/value")},
        {g_xr.stick_l, xr_path("/user/hand/left/input/thumbstick")},
        {g_xr.stick_r, xr_path("/user/hand/right/input/thumbstick")},
        // Not bound to anything in the game, so it is free for switching views.
        {g_xr.toggle,  xr_path("/user/hand/right/input/thumbstick/click")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = xr_path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = binds;
    sb.countSuggestedBindings = sizeof(binds) / sizeof(binds[0]);
    XR_TRY(xrSuggestInteractionProfileBindings(g_xr.instance, &sb));

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_xr.action_set;
    XR_TRY(xrAttachSessionActionSets(g_xr.session, &ai));
    return true;
}

static bool action_bool(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(g_xr.session, &gi, &st))) return false;
    return st.isActive && st.currentState;
}

static float action_float(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(g_xr.session, &gi, &st))) return 0.0f;
    return st.isActive ? st.currentState : 0.0f;
}

static XrVector2f action_vec2(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(g_xr.session, &gi, &st))) return {0, 0};
    return st.isActive ? st.currentState : XrVector2f{0, 0};
}

static void read_pad(PadState& p) {
    XrActiveActionSet active{g_xr.action_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(g_xr.session, &si))) return;

    if (action_bool(g_xr.a_btn)) p.buttons |= PAD_A;
    if (action_bool(g_xr.b_btn)) p.buttons |= PAD_B;
    if (action_bool(g_xr.x_btn)) p.buttons |= PAD_X;
    if (action_bool(g_xr.y_btn)) p.buttons |= PAD_Y;
    if (action_bool(g_xr.menu))  p.buttons |= PAD_START;

    const float lt = action_float(g_xr.trig_l), rt = action_float(g_xr.trig_r);
    p.trig_l = (uint8_t)(lt * 255.0f);
    p.trig_r = (uint8_t)(rt * 255.0f);
    if (lt > 0.85f) p.buttons |= PAD_L;
    if (rt > 0.85f) p.buttons |= PAD_R;
    if (action_float(g_xr.grip_r) > 0.5f) p.buttons |= PAD_Z;

    auto to_u8 = [](float f) {
        int v = 128 + (int)(f * 100.0f);
        return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    const XrVector2f l = action_vec2(g_xr.stick_l), r = action_vec2(g_xr.stick_r);
    p.stick_x = to_u8(l.x);
    p.stick_y = to_u8(l.y);
    p.cstick_x = to_u8(r.x);
    p.cstick_y = to_u8(r.y);
}

// ---------------------------------------------------------------------------
static void handle_session_state(XrSessionState s) {
    g_xr.state = s;
    LOGI("session state %d", (int)s);
    if (s == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(g_xr.session, &bi))) {
            g_xr.running = true;
            LOGI("session begun");
        }
    } else if (s == XR_SESSION_STATE_STOPPING) {
        xrEndSession(g_xr.session);
        g_xr.running = false;
    }
}

static void poll_xr_events() {
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(g_xr.instance, &ev) != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            handle_session_state(((XrEventDataSessionStateChanged*)&ev)->state);
        else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            LOGE("instance loss pending");
    }
}

static void on_cmd(android_app*, int32_t) {}

static std::string read_script(const std::string& dir) {
    FILE* f = fopen((dir + "/wr_input.txt").c_str(), "rb");
    if (!f) return {};
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    LOGI("input script: %s", s.c_str());
    return s;
}

void android_main(android_app* app) {
    pthread_t t;
    pthread_create(&t, nullptr, log_pump, nullptr);
    pthread_detach(t);
    app->onAppCmd = on_cmd;

    const std::string dir = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    const std::string iso = dir + "/game.iso";
    g_vrcfg = vr_config_load(dir);
    gx::render_set_frame_marker(g_vrcfg.frame_marker);
    static std::string dump_dir;
    if (g_vrcfg.dump_every > 0) {
        dump_dir = dir + "/frames";
        mkdir(dump_dir.c_str(), 0777);
        gx::g_dump_dir = dump_dir.c_str();
        gx::g_dump_every = g_vrcfg.dump_every;
        LOGI("dumping every %d frames to %s", g_vrcfg.dump_every, dump_dir.c_str());
    }

    if (!egl_init()) return;
    int glver = gl_load_with(gl_proc);
    if (glver < WR_GL_VERSION_MIN) { LOGE("GL ES %d.%d too old", glver / 10, glver % 10); return; }
    LOGI("GL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    if (!xr_create_instance(app)) { LOGE("no OpenXR instance"); return; }
    if (!xr_create_session()) return;
    if (!xr_create_swapchain()) return;
    if (!xr_create_actions()) return;

    if (!plat_readable(iso.c_str())) {
        LOGE("no game image at %s", iso.c_str());
        return;
    }

    mem_init();
    timing_init();
    input_script_init(read_script(dir).c_str());
    gx::render_init(1);
    gx::render_set_window_size(SWAP_W, SWAP_H);

    uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);
    LOGI("game started");

    uint32_t xr_frames = 0, game_frames = 0, skipped = 0;
    uint64_t disp_frames = 0;  // monotonic, unlike xr_frames which the stats line resets
    bool have_content = false;
    bool stereo = g_vrcfg.start_in_stereo, manual_override = false, toggle_was_down = false;
    // Kept across frames: a display frame with no new game frame re-submits these
    // rather than re-rendering. They carry the pose each image was rendered for, so
    // the compositor reprojects them for the current head pose.
    XrCompositionLayerProjectionView proj_views[2]{};
    bool have_proj = false;
    XrTime last_report = 0;

    while (!app->destroyRequested) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(0, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) return;
        }
        poll_xr_events();
        if (!g_xr.running) { usleep(10000); continue; }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fw{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(g_xr.session, &fw, &fs))) continue;
        XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(g_xr.session, &fb);

        PadState p;
        p.connected = true;
        read_pad(p);
        input_script_apply(p);
        pad_set_state(0, p);

        // Which view to present. The game's own frames decide: a race submits several
        // hundred perspective draws, menus submit a handful. Held for a number of
        // frames so a brief spike cannot flap the view back and forth. Clicking the
        // right thumbstick pins it manually.
        if (action_bool(g_xr.toggle)) {
            if (!toggle_was_down) {
                manual_override = true;
                stereo = !stereo;
                LOGI("view pinned to %s", stereo ? "stereo" : "theater");
            }
            toggle_was_down = true;
        } else {
            toggle_was_down = false;
        }

        // Take at most one game frame per display frame, before choosing a view: both
        // the auto-switch and both render paths need it, and a backlog drawn and
        // thrown away would be wasted work.
        std::unique_ptr<gx::Batch> batch = gx::take_batch(0);
        if (batch) {
            game_frames++;
            if (!manual_override) {
                // The game's own "on the course" flag: 1 from the moment the countdown
                // starts until the race ends, 0 for everything else -- boot, menus,
                // course select, loading, the course overview and the results screen.
                // Found by diffing guest RAM across labelled snapshots; see the Tools
                // section of README.md. It is exact, so no hysteresis is needed.
                //
                // Counting perspective draws instead, as this used to, puts the overview
                // flyover over the threshold (it is a full 3D scene) and dips below it
                // during a race whenever the view is sparse, which is what made the view
                // flap. That heuristic is kept only as a fallback for a disc this address
                // does not suit: if it ever holds anything but 0 or 1, it is not the flag.
                const uint32_t bits = mem_r32(kRaceActiveAddr);
                float wave;
                memcpy(&wave, &bits, 4);
                // Zero, or a plausible wave height. Anything else means this is not that
                // variable -- a different disc, or a layout change. There is deliberately
                // no fallback: counting draws, which this replaced, does not work, and
                // guessing wrong does not degrade gracefully. It drops someone into
                // stereo over a menu or flips the view mid-race, which is unpleasant
                // enough in a headset to be worth refusing to run at all.
                if (bits != 0 && !(wave > 0.0f && wave < 100.0f))
                    fatal("race-state variable at %#x reads %#010x (%g), which is neither "
                          "zero nor a plausible wave height. This build only knows the "
                          "disc named in README.md; refusing to guess which view to "
                          "present.", kRaceActiveAddr, bits, (double)wave);
                const bool want_stereo = bits != 0;
                if (want_stereo != stereo) {
                    stereo = want_stereo;
                    LOGI("switching to %s", stereo ? "stereo" : "theater");
                }
            }
        }

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

        if (fs.shouldRender && stereo) {
            // --- stereo: the world through each eye, as a projection layer ---
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t nv = 0;
            XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = g_xr.space;
            // Only redraw the eyes when the game produced a frame. Acquiring and
            // releasing an image without drawing into it hands the compositor whatever
            // that image held several frames ago, which reads as a hard strobe.
            if (batch && XR_SUCCEEDED(xrLocateViews(g_xr.session, &li, &vs, 2, &nv, views)) && nv == 2) {
                // Both eyes share one batch: the CPU-side transform and the vertex
                // buffer are produced once, only the uniforms and draws repeat.
                gx::Batch* b = batch.get();
                bool both_eyes = true;
                for (int e = 0; e < 2; e++) {
                    auto& eye = g_xr.eyes[e];
                    uint32_t ei = 0;
                    XrSwapchainImageAcquireInfo eai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    if (XR_FAILED(xrAcquireSwapchainImage(eye.handle, &eai, &ei))) {
                        both_eyes = false;
                        continue;
                    }
                    XrSwapchainImageWaitInfo ewi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    ewi.timeout = XR_INFINITE_DURATION;
                    if (XR_SUCCEEDED(xrWaitSwapchainImage(eye.handle, &ewi))) {
                        float P[16], V[16];
                        const float n = g_vrcfg.near_m * g_vrcfg.units_per_metre;
                        const float f = g_vrcfg.far_m * g_vrcfg.units_per_metre;
                        mat_proj(views[e].fov, n, f, P);
                        mat_view(views[e].pose, g_vrcfg, V);
                        gx::render_set_vr_eye(P, V, g_vrcfg.hud_scale);
                        if (b) gx::render_execute_eye(*b, eye.fbos[ei], eye.w, eye.h, e == 0);
                        glFlush();
                    }
                    XrSwapchainImageReleaseInfo eri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(eye.handle, &eri);

                    proj_views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    proj_views[e].pose = views[e].pose;
                    proj_views[e].fov = views[e].fov;
                    proj_views[e].subImage.swapchain = eye.handle;
                    proj_views[e].subImage.imageRect = {{0, 0}, {eye.w, eye.h}};
                    proj_views[e].subImage.imageArrayIndex = 0;
                }
                // A half-filled projection layer is invalid, so only keep it when both
                // eyes were acquired.
                if (both_eyes) have_proj = true;
            }
            if (have_proj) {
                proj_layer.space = g_xr.space;
                proj_layer.viewCount = 2;
                proj_layer.views = proj_views;
                layers.push_back((XrCompositionLayerBaseHeader*)&proj_layer);
            }
        } else if (fs.shouldRender) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_xr.swapchain, &ai, &idx))) {
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_SUCCEEDED(xrWaitSwapchainImage(g_xr.swapchain, &wi))) {
                    gx::render_set_output_fbo(g_xr.fbos[idx]);
                    // Consume at most one game frame per display frame; otherwise a
                    // backlog would be drawn and thrown away.
                    bool drew = false;
                    if (batch && gx::render_execute(*batch)) drew = true;
                    // The compositor needs an image every display frame, and the game
                    // produces far fewer, so repaint the last one otherwise.
                    if (!drew) gx::render_repaint();
                    gx::render_set_output_fbo(0);
                    // `log_frames 1` in vr.txt traces the theater path one display frame
                    // at a time: which swapchain image was written, whether it got a new
                    // game frame or a repaint, and which game frame it is showing. The
                    // headless harness renders the same pixels but cannot reproduce the
                    // compositor, so flicker that is not in the EFB has to be caught here.
                    if (g_vrcfg.log_frames)
                        LOGI("[t] disp=%llu img=%u %s present=%u batch=%d",
                             (unsigned long long)disp_frames, idx, drew ? "drew" : "rept",
                             gx::present_count(), batch ? 1 : 0);
                    // Issue the blit before handing the image back. A full fence wait
                    // here was tried against the character-select flicker and changed
                    // nothing, at about a tenth of the game's frame rate.
                    glFlush();
                    have_content = true;
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_xr.swapchain, &ri);
            }
        } else {
            skipped++;
        }

        // Submit the quad on every frame that has content, including ones the runtime
        // told us not to render. Dropping the layer shows the user an empty frame,
        // which strobes against the frames that do carry it; re-submitting it just
        // re-displays the last released image.
        if (have_content && !stereo) {
            quad.layerFlags = 0;
            quad.space = g_xr.space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_xr.swapchain;
            quad.subImage.imageRect = {{0, 0}, {SWAP_W, SWAP_H}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation = {0, 0, 0, 1};
            quad.pose.position = {0, 0, -2.5f};   // 2.5 m ahead
            quad.size = {3.2f, 2.4f};             // 4:3, a cinema-sized panel
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }

        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = (uint32_t)layers.size();
        fe.layers = layers.data();
        xrEndFrame(g_xr.session, &fe);

        xr_frames++;
        disp_frames++;
        if (fs.predictedDisplayTime - last_report > 5000000000LL) {  // 5 s in ns
            LOGI("compositor %u frames, game %u frames, %u not rendered",
                 xr_frames, game_frames, skipped);
            xr_frames = game_frames = skipped = 0;
            last_report = fs.predictedDisplayTime;
        }
    }
}
