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
#include "input_log.h"
#include "vr_config.h"
#include <sys/stat.h>

// Non-zero while the game is on a course: set as the course intro begins, and cleared
// when the race ends -- or the moment the player quits out of one. Zero through boot, the
// menus, course select, loading, the results screen and the title screen.
//
// Traced every frame it has exactly two transitions in a 6,663-frame session that races to
// the finish (1 at 1996, 0 at 3797) and exactly two in a 5,837-frame session that retires
// to the main menu (1 at 1996, 0 at 3539, against a quit confirmed at 3520).
//
// It replaces two variables that each got half of this wrong. 0x806193BC, the course's
// wave height, came up only as the countdown reached the line -- about a second after the
// starting lights are on screen, so a race began flat and popped into 3D once it was
// already under way -- and, worse, it is never cleared on the quit path: it still read 3.0
// back on the title screen, which left the headset stuck in stereo over the menus. The
// start-sequence state at 0x80625A54 covered the lights but went out mid-race.
//
// This one is a signed count of what the course loaded, not a flag -- see where it is
// read. Treating it as a flag, and aborting on anything but 0 or 1, is what it used to
// do; it read 0 and 1 in every session measured and then aborted in someone's headset on
// a session that counted differently. Being wrong about which view to present does not
// degrade gracefully, but aborting degrades worse, so an unexpected value now presents
// the flat view and says so once.
static constexpr uint32_t kOnCourseAddr = 0x80602160;

// The course's wave height: non-zero from the starting lights (up to 45 game frames after
// the rig appears) until the race ends. It cannot end stereo on its own, because nothing
// resets it when a course is abandoned, but it does know when a race *finishes*:
// kOnCourseAddr stays up for another 170 frames past the chequered flag, through the
// results, which is too long to sit in stereo.
//
// What starts stereo is the starting-light rig itself, found in the frame
// (gx::batch_shows_start_rig). The start sequence's phase at 0x80625A54 used to, on the
// belief that its 5 meant "countdown". It does not only mean that: Championship ->
// Exhibition -> Dolphin Park opens on a screen that cycles views of the course until A is
// pressed, and five seconds into it the phase goes to 5 and stays there -- so the headset
// switched to stereo on a screen with no race on it, every time. In Time Attack the phase
// never moves at all. No guest variable found so far means "the lights are up", so the
// lights are looked for instead.
static constexpr uint32_t kRaceActiveAddr = 0x806193BC;
#include "gx/render.h"
#include "gx/render_gl.h"
#include "start_rig.h"
#include "gx/gl.h"
#include "gx/gl_msrtt.h"
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
#include <ctime>
#include <string>
#include <vector>

#define TAG "waverace"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

uint32_t boot_load(const char* iso_path);
bool audio_open();

// Where the theater panel hangs and how big it is: a cinema-sized 4:3 screen, matching
// the game's own shape, 2.5 m ahead of where the viewer started.
static constexpr float kQuadDist = 2.5f;
static constexpr float kQuadW = 3.2f, kQuadH = 2.4f;

// Rendering an eye at the headset's own resolution and then dropping back to a panel
// drawn at the GameCube's is the jarring part of leaving a race, so the panel is given as
// many texels as the headset can actually resolve across it -- see xr_create_swapchain.
// Starts at what it used to be fixed at, in case the sizing finds nothing to work from.
static int g_swap_w = 1024, g_swap_h = 768;

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
    XrAction toggle;   // right thumbstick click: chase camera or first person, in stereo
};
static Xr g_xr;
static VrConfig g_vrcfg;

// Where the head was when first person began, in metres in the reference space; the eye
// is placed relative to that rather than to the space's own origin. The space is LOCAL,
// whose origin is wherever the head was at launch, so without this the viewer's posture
// since then lands in the view -- multiplied by units_per_metre. Behind the chase camera
// a 30 cm lean is lost in the distance; on a rider whose head is 15 units above his hands
// it put the eye between them. Re-zeroed every time first person is entered.
static float g_head_zero[3];
static bool g_head_zeroed = false;

// The first-person anchor, with vr.txt's metre nudges converted to game units.
static void set_first_person(bool on) {
    const float u = g_vrcfg.units_per_metre;
    gx::render_set_first_person(on, g_vrcfg.fp_x, g_vrcfg.fp_y + g_vrcfg.fp_up_m * u,
                                g_vrcfg.fp_z + g_vrcfg.fp_forward_m * u);
}

// The viewer's choice of chase camera or first person, kept across sessions in
// <files>/view.txt, which the app writes whenever the choice changes. It is a separate file
// from vr.txt so that the app never rewrites what a person edits by hand; vr.txt's
// first_person is only the choice before one has been made.
static std::string g_view_path;

static bool view_pref_load(bool fallback) {
    FILE* f = fopen(g_view_path.c_str(), "r");
    if (!f) return fallback;
    int v = fallback ? 1 : 0;
    if (fscanf(f, "first_person %d", &v) != 1) v = fallback ? 1 : 0;
    fclose(f);
    return v != 0;
}

static void view_pref_save(bool first_person) {
    if (FILE* f = fopen(g_view_path.c_str(), "w")) {
        fprintf(f, "first_person %d\n", first_person ? 1 : 0);
        fclose(f);
    }
}

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
// metres into game units on the way in, after taking off `zero`, a head position in
// metres that counts as the origin instead -- see g_head_zero.
static void mat_view(const XrPosef& pose, const VrConfig& c, const float zero[3], float* m) {
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
        (pose.position.x - zero[0]) * c.units_per_metre + c.offset_x,
        (pose.position.y - zero[1]) * c.units_per_metre + c.offset_y,
        (pose.position.z - zero[2]) * c.units_per_metre + c.offset_z,
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

    // What the runtime recommends per eye. Needed before the panel as well as after it:
    // it is the one honest statement this headset makes about its own pixel density.
    uint32_t nv = 0;
    xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
    std::vector<XrViewConfigurationView> vcs(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_TRY(xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             nv, &nv, vcs.data()));
    if (nv < 2) { LOGE("expected 2 views, got %u", nv); return false; }

    // How many texels the panel is worth.
    //
    // It covers 2*atan(1.6/2.5) = 65 degrees of the viewer's horizontal field, and an eye
    // swapchain spans that whole field -- call it a hundred degrees, which is roughly the
    // part of a headset's advertised figure that is in front of one eye. So the panel is
    // worth about two thirds of an eye's width at one texel per display pixel. A little
    // over that, because the compositor samples the panel at whatever angle the head
    // happens to be holding and texels that line up with pixels nowhere are better
    // filtered down than invented.
    //
    // Taken from the recommendation rather than fixed, so a denser headset gets a denser
    // panel: a Quest 3's 1680-wide eyes give 1260x945, against the 1024x768 this was.
    // Width is kept a multiple of four so that three quarters of it stays whole and the
    // panel stays exactly 4:3 -- it is submitted as a 4:3 rectangle, and any other shape
    // would be stretched into it.
    constexpr float kNominalEyeFovDeg = 100.0f;
    constexpr float kPanelOversample = 1.15f;
    const float quad_fov_deg = 2.0f * atanf(0.5f * kQuadW / kQuadDist) * 180.0f / 3.14159265f;
    const int panel_w = (int)(vcs[0].recommendedImageRectWidth *
                              (quad_fov_deg / kNominalEyeFovDeg) * kPanelOversample) & ~3;
    if (panel_w > g_swap_w) {
        g_swap_w = panel_w;
        g_swap_h = panel_w * 3 / 4;
    }

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = chosen;
    ci.sampleCount = 1;
    ci.width = g_swap_w;
    ci.height = g_swap_h;
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
    LOGI("quad swapchain %dx%d, %u images", g_swap_w, g_swap_h, n);

    // Per-eye swapchains for the stereo projection layer, at what the runtime recommends
    // for this headset times `eye_scale`.
    for (int e = 0; e < 2; e++) {
        auto& eye = g_xr.eyes[e];
        // `eye_scale` multiplies the recommendation, within what the runtime will take.
        const float es = g_vrcfg.eye_scale > 0.0f ? g_vrcfg.eye_scale : 1.0f;
        eye.w = (int32_t)fminf(vcs[e].recommendedImageRectWidth * es,
                               (float)vcs[e].maxImageRectWidth);
        eye.h = (int32_t)fminf(vcs[e].recommendedImageRectHeight * es,
                               (float)vcs[e].maxImageRectHeight);
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

    // Antialiasing, if asked for and available: see gl_msrtt.h for why this way.
    int samples = g_vrcfg.msaa > 1 ? g_vrcfg.msaa : 0;
    const gx::Msrtt ms = samples ? gx::msrtt_load(gl_proc) : gx::Msrtt{};
    if (samples && ms.max_samples < samples) {
        LOGE("msaa %d unavailable (max %d); eyes are not antialiased", samples, ms.max_samples);
        samples = 0;
    }

    // One depth buffer, shared: the eyes are rendered in sequence, and it is cleared
    // for each. Both eyes use the same recommended size.
    glGenRenderbuffers(1, &g_xr.eye_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_xr.eye_depth);
    if (samples)
        ms.renderbuffer_storage(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, g_xr.eyes[0].w,
                                g_xr.eyes[0].h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_xr.eyes[0].w, g_xr.eyes[0].h);
    for (int e = 0; e < 2; e++) {
        for (size_t i = 0; i < g_xr.eyes[e].fbos.size(); i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_xr.eyes[e].fbos[i]);
            if (samples)
                ms.framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                          g_xr.eyes[e].images[i].image, 0, samples);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       g_xr.eyes[e].images[i].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                      g_xr.eye_depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                LOGE("eye %d fbo %zu incomplete", e, i);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("eye swapchains %dx%d, msaa %d", g_xr.eyes[0].w, g_xr.eyes[0].h, samples);
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
    g_xr.toggle  = make_action("view_toggle", "First person", XR_ACTION_TYPE_BOOLEAN_INPUT);

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
        // Not bound to anything in the game, so it is free for switching the view.
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
    FILE* f = fopen((dir + "/gcn_input.txt").c_str(), "rb");
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

// The runtime's GCN_* diagnostics are environment variables, and an APK launched from the
// headset's own launcher has no environment to speak of. `gcn_env.txt` beside `vr.txt`, one
// KEY=VALUE per line, stands in for it: each line is put into the environment before the
// runtime starts, so GCN_HEAP=1 there does what GCN_HEAP=1 on a desktop shell does. Only
// variables read lazily see it -- a static initializer that called getenv at library load
// has already run -- which is most of them.
static void apply_env_file(const std::string& dir) {
    FILE* f = fopen((dir + "/gcn_env.txt").c_str(), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos || eq == 0) continue;
        setenv(l.substr(0, eq).c_str(), l.substr(eq + 1).c_str(), 1);
        LOGI("env: %s", l.c_str());
    }
    fclose(f);
}

// A NativeActivity can be destroyed and re-created inside one process, and that calls
// android_main a second time. Nothing here survives it: the EGL context, the OpenXR
// instance and session, the recompiled game and the threads it booted are all global and
// are built exactly once. The second call then deadlocks on the first call's leftovers,
// which is what a hung launch is -- from outside it shows up as two log_pump pipes and
// two threads parked in its read(fd, buf, 511).
//
// Unwinding all of that on the way out is not on offer: the game is a recompiled
// executable with no shutdown path, and the runtime around it was written to be set up
// once. So the process goes instead and the next launch gets a clean one, which is what
// the system expects by the time the activity is gone. The memory card is safe to leave
// this way -- it is rewritten at the end of every command that dirties it, so there is
// nothing buffered to lose.
[[noreturn]] static void app_exit(const char* why) {
    LOGI("exiting: %s", why);
    fflush(nullptr);
    plat_exit_now(0);
}

void android_main(android_app* app) {
    // Belt and braces for the same thing: if the glue ever starts a second android_main
    // while the first is still in its loop, the process still only runs one.
    static bool entered;
    if (entered) app_exit("android_main re-entered; this process has already run a game");
    entered = true;

    pthread_t t;
    pthread_create(&t, nullptr, log_pump, nullptr);
    pthread_detach(t);
    app->onAppCmd = on_cmd;

    const std::string dir = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    const std::string iso = dir + "/game.iso";
    g_vrcfg = vr_config_load(dir);
    g_view_path = dir + "/view.txt";
    gx::render_set_world_pitch(g_vrcfg.world_pitch_deg * 3.14159265f / 180.0f);
    gx::render_set_first_person_smoothing(g_vrcfg.fp_height_s, g_vrcfg.fp_yaw_s,
                                         g_vrcfg.fp_tilt, g_vrcfg.fp_tilt_s);
    static std::string dump_dir;
    if (g_vrcfg.dump_every > 0) {
        dump_dir = dir + "/frames";
        mkdir(dump_dir.c_str(), 0777);
        gx::g_dump_dir = dump_dir.c_str();
        gx::g_dump_every = g_vrcfg.dump_every;
        LOGI("dumping every %d frames to %s", g_vrcfg.dump_every, dump_dir.c_str());
    }

    if (!egl_init()) app_exit("EGL init failed");
    int glver = gl_load_with(gl_proc);
    if (glver < GCN_GL_VERSION_MIN) { LOGE("GL ES %d.%d too old", glver / 10, glver % 10); app_exit("GL too old"); }
    LOGI("GL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    if (!xr_create_instance(app)) { LOGE("no OpenXR instance"); app_exit("no OpenXR instance"); }
    if (!xr_create_session()) app_exit("no OpenXR session");
    if (!xr_create_swapchain()) app_exit("no swapchain");
    if (!xr_create_actions()) app_exit("no actions");

    if (!plat_readable(iso.c_str())) {
        LOGE("no game image at %s", iso.c_str());
        app_exit("no game image");
    }

    apply_env_file(dir);
    mem_init();
    timing_init();
    input_script_init(read_script(dir).c_str());
    static const std::string shader_cache = dir + "/shaders.bin";
    gx::render_set_shader_cache(shader_cache.c_str());
    gx::render_init(g_vrcfg.start_in_stereo ? g_vrcfg.stereo_scale : g_vrcfg.theater_scale);
    gx::render_set_window_size(g_swap_w, g_swap_h);

    // The app's own files directory is the only place it can write, and the memory card
    // has to land there or the game starts from a blank one every launch.
    static std::string save_dir = dir;
    if (!save_dir.empty()) g_save_dir = save_dir.c_str();

    // Every run records what the guest read from the controllers, into
    // <files>/inputs/<stamp>/ with a snapshot of the memory card, so that a route to
    // wherever something went wrong can be played back: on a desktop with
    // --replay=<dir> after an adb pull, or here with GCN_REPLAY=<dir> in gcn_env.txt
    // (relative to the files directory). GCN_NO_INPUT_LOG=1 turns the recording off.
    // See gcn-recomp/docs/diagnostics.md; the replay runs on a scratch copy of the
    // logged card, never the real one.
    static std::string replay_card;
    if (const char* r = getenv("GCN_REPLAY")) {
        const std::string replay_dir = r[0] == '/' ? std::string(r) : dir + "/" + r;
        if (!input_replay_load(replay_dir, replay_card)) LOGE("no replay at %s", replay_dir.c_str());
        else if (!replay_card.empty()) g_memcard_path = replay_card.c_str();
    }
    if (!getenv("GCN_NO_INPUT_LOG")) {
        char stamp[64];
        time_t now = time(nullptr);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
        input_log_start(dir + "/inputs/" + stamp, replay_card.empty() ? dir + "/memcard_a.raw" : replay_card);
    }

    // Before the guest runs, so the AI DMA has somewhere to go from its first block.
    // A device that will not open is not fatal: the game is still playable silently, and
    // the backend says why in the log.
    if (!audio_open()) LOGE("audio unavailable; continuing without sound");

    uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);
    LOGI("game started");

    uint32_t xr_frames = 0, game_frames = 0, skipped = 0;
    uint64_t disp_frames = 0;  // monotonic, unlike xr_frames which the stats line resets
    bool have_content = false;
    bool stereo = g_vrcfg.start_in_stereo, toggle_was_down = false;
    // `first_person` is the viewer's choice; it is *in effect* only in stereo. Leaving
    // stereo drops back to the chase camera at once, before the morph to theater begins:
    // the morph folds the world onto the panel the game's own camera drew, and starting it
    // from the rider's seat swung the whole scene across to the chase camera's view on the
    // way. Entering stereo picks the choice up again.
    bool first_person = view_pref_load(g_vrcfg.first_person);
    LOGI("view preference: %s", first_person ? "first person" : "chase camera");
    auto apply_view = [&]() {
        set_first_person(first_person && stereo);
        g_head_zeroed = false;
    };
    apply_view();
    // Kept across frames: a display frame with no new game frame re-submits these
    // rather than re-rendering. They carry the pose each image was rendered for, so
    // the compositor reprojects them for the current head pose.
    XrCompositionLayerProjectionView proj_views[2]{};
    bool have_proj = false;
    // Where the presentation stands between theater (0) and stereo (1). `stereo` says where
    // it is going; this follows it over `transition_s`. Anywhere above 0 the eyes are
    // rendered, with the stereo view folded part way back onto the theater panel -- see
    // render_set_vr_morph -- so the panel is only shown again once it is all the way home.
    float morph = stereo ? 1.0f : 0.0f;
    XrTime last_display = 0;
    // Whether the panel's swapchain holds a frame drawn since the eyes last took over.
    // Until it does, the eyes' last image stays up: it was rendered almost exactly flat,
    // and the panel's alternative is whatever it showed before the race.
    bool quad_fresh = !stereo;
    // The panel in the eye's space, in game units: where theater hangs it, seen from where
    // mat_view puts the eye. It is in the headset's room, not the game's, so it takes the
    // viewpoint offset and nothing of the world's pitch.
    float panel[16] = {0};
    {
        const float u = g_vrcfg.units_per_metre;
        panel[0] = 0.5f * kQuadW * u;
        panel[5] = 0.5f * kQuadH * u;
        panel[12] = g_vrcfg.offset_x;
        panel[13] = g_vrcfg.offset_y;
        panel[14] = g_vrcfg.offset_z - kQuadDist * u;
        panel[15] = 1.0f;
    }
    XrTime last_report = 0;

    while (!app->destroyRequested) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(0, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) app_exit("activity destroyed");
        }
        poll_xr_events();
        if (!g_xr.running) { usleep(10000); continue; }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fw{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(g_xr.session, &fw, &fs))) continue;
        XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(g_xr.session, &fb);

        // Where the frame hangs is a question about the viewer, so `log_frames 1` reports
        // the viewer: the frame sits on the forward axis of the LOCAL space, and that axis
        // is eye level only if the runtime fixed the space while the headset was being
        // worn. A head well above y=0 slides the frame down the view without anything in
        // the frame's own geometry being wrong, and from inside the headset the two look
        // identical. This runs wherever the app is -- a menu will do -- so the number does
        // not cost a drive to the start line.
        if (g_vrcfg.log_frames) {
            static uint32_t nlog;
            if ((nlog++ % 72) == 0) {
                XrViewState lvs{XR_TYPE_VIEW_STATE};
                uint32_t lnv = 0;
                XrView lv[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                XrViewLocateInfo lli{XR_TYPE_VIEW_LOCATE_INFO};
                lli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                lli.displayTime = fs.predictedDisplayTime;
                lli.space = g_xr.space;
                if (XR_SUCCEEDED(xrLocateViews(g_xr.session, &lli, &lvs, 2, &lnv, lv)) && lnv == 2) {
                    const XrPosef& h = lv[0].pose;
                    const float pitch = asinf(fmaxf(-1.0f, fminf(1.0f,
                        2.0f * (h.orientation.w * h.orientation.x -
                                h.orientation.y * h.orientation.z))));
                    float tan_h = 0.0f;
                    for (int e = 0; e < 2; e++) {
                        tan_h = fmaxf(tan_h, fabsf(tanf(lv[e].fov.angleUp)));
                        tan_h = fmaxf(tan_h, fabsf(tanf(lv[e].fov.angleDown)));
                    }
                    // The frame's half-height as an angle is the number to hold against
                    // what it looks like: it is where the top edge of the HUD sits above
                    // the forward axis, and the game's own top row is at 0.83 of that.
                    const float half_m = g_vrcfg.hud_distance_m * tan_h * g_vrcfg.hud_scale;
                    LOGI("head y=%.2fm pitch=%+.1fdeg | eye fov up=%+.1f down=%+.1f deg | "
                         "frame %.2fm ahead %+.2fm up, half-height %.2fm = %.1fdeg, "
                         "HUD top row %.1fdeg",
                         h.position.y, pitch * 57.2958f,
                         lv[0].fov.angleUp * 57.2958f, lv[0].fov.angleDown * 57.2958f,
                         g_vrcfg.hud_distance_m, g_vrcfg.hud_height_m, half_m,
                         atanf(half_m / g_vrcfg.hud_distance_m) * 57.2958f,
                         atanf(0.83f * half_m / g_vrcfg.hud_distance_m) * 57.2958f);
                }
            }
        }

        PadState p;
        p.connected = true;
        read_pad(p);
        input_script_apply(p);
        pad_set_state(0, p);

        // Clicking the right thumbstick switches between the chase camera and first
        // person. Only in stereo: theater shows the game's own frame, which has no other
        // view to offer. It used to pin the view between theater and stereo, which the
        // game's own state now decides well enough that nobody needed the override.
        if (action_bool(g_xr.toggle)) {
            if (!toggle_was_down && stereo) {
                first_person = !first_person;
                apply_view();
                view_pref_save(first_person);
                LOGI("view: %s", first_person ? "first person" : "chase camera");
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
            // Which view to present. The game's own state decides, held for a number of
            // frames so a transient cannot flap the view back and forth.
            //
            // The game's own "on a course" value; see kOnCourseAddr. Found by
            // diffing guest RAM across labelled snapshots and then tracing every
            // frame, which is the only way to tell a steady value from one that
            // blinks -- see docs/dev/vr.md.
            //
            // It is a *count*, not a flag, which cost a crash to learn. It is field
            // +0x20 of the descriptor at 0x80602140, written once at 0x8004561C
            // (`stw r3,0x20(r30)`) from whatever fn_80047320 returns: a loop counter,
            // zeroed at 0x80047348 and incremented per entry at 0x800473CC, or -1
            // from the error path at 0x80047578. Every session traced while this was
            // being worked out counted exactly one entry, so it read 0 and 1 and
            // looked boolean. It is not, and a build that insisted it was aborted in
            // someone's headset mid-session.
            //
            // Positive is on a course; -1 is a load that failed, which is not.
            const int32_t on_course = (int32_t)mem_r32(kOnCourseAddr);
            if (on_course < 0) {
                static bool said = false;
                if (!said) {
                    said = true;
                    LOGE("on-course count at %#x reads %d; treating as off-course",
                         kOnCourseAddr, on_course);
                }
            }
            // kOnCourseAddr is the only signal that clears when a race is quit, so it
            // gates. Within it, the starting-light rig on screen brings stereo up with the
            // lights, and the wave height carries it from there to the chequered flag --
            // it is what notices a race *finishing*, which kOnCourseAddr does not do until
            // 170 frames later, well into the results.
            const uint32_t racing = mem_r32(kRaceActiveAddr);
            const bool rig = gx::batch_shows_start_rig(*batch);
            const bool want_stereo = on_course > 0 && (racing != 0 || rig);
            // The flag is written by the guest thread and read here, so a sample can
            // land on a transient: tracing the old race flag from this side caught it
            // reading non-zero for a single frame on the results screen and twice
            // before a race. One game frame of agreement drops those, and costs 33 ms
            // on a switch that used to be a second late anyway.
            static bool pending = false;
            static int agree = 0;
            if (want_stereo != stereo) {
                agree = (want_stereo == pending) ? agree + 1 : 1;
                pending = want_stereo;
                if (agree >= 2) {
                    stereo = want_stereo;
                    agree = 0;
                    apply_view();
                    LOGI("switching to %s", stereo ? "stereo" : "theater");
                }
            } else {
                agree = 0;
            }
        }

        // Advance the morph on the compositor's clock. The eyes only take it up when the
        // game delivers a frame, so it moves at the game's rate, not this one -- but
        // stepping it here keeps its duration honest however unevenly frames arrive.
        {
            const float target = stereo ? 1.0f : 0.0f;
            if (g_vrcfg.transition_s <= 0.0f || last_display == 0) {
                morph = target;
            } else {
                const float step = (float)(fs.predictedDisplayTime - last_display) * 1e-9f /
                                   g_vrcfg.transition_s;
                morph = target > morph ? fminf(target, morph + step) : fmaxf(target, morph - step);
            }
            last_display = fs.predictedDisplayTime;
        }
        const bool eyes = morph > 0.0f;
        if (eyes) quad_fresh = false;

        if (batch) {
            // The two views want the EFB at different sizes -- see theater_scale. This is
            // the moment to change it: a whole frame is about to be drawn into it, and a
            // display frame without a new game frame repaints the panel from what is
            // already there, which a re-scale would have thrown away. A no-op otherwise.
            // It follows the path that will draw, which mid-morph is the eyes'.
            gx::render_set_internal_scale(eyes ? g_vrcfg.stereo_scale
                                               : g_vrcfg.theater_scale);
        }

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

        if (fs.shouldRender && eyes) {
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
                // One HUD frame for both eyes, built outside the loop. Each eye's own
                // frustum is asymmetric and the two differ; sizing the frame per eye
                // would give the viewer two different HUDs to fuse. The widest half-field
                // either eye sees is what `hud_scale` is a fraction of.
                float tan_half = 0.0f;
                for (int e = 0; e < 2; e++) {
                    tan_half = fmaxf(tan_half, fabsf(tanf(views[e].fov.angleUp)));
                    tan_half = fmaxf(tan_half, fabsf(tanf(views[e].fov.angleDown)));
                }
                float H[16];
                gx::render_hud_frame(g_vrcfg.hud_distance_m * g_vrcfg.units_per_metre,
                                     tan_half, g_vrcfg.hud_scale,
                                     g_vrcfg.hud_height_m * g_vrcfg.units_per_metre,
                                     g_vrcfg.hud_pitch_deg * 3.14159265f / 180.0f, H);
                // Eased at both ends, so the world neither lurches out of the panel nor
                // slams into place.
                gx::render_set_vr_morph(morph * morph * (3.0f - 2.0f * morph), panel);
                if (!g_head_zeroed) {
                    g_head_zeroed = true;
                    if (first_person && stereo) {
                        g_head_zero[0] = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
                        g_head_zero[1] = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
                        g_head_zero[2] = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
                    } else {
                        g_head_zero[0] = g_head_zero[1] = g_head_zero[2] = 0.0f;
                    }
                }
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
                        mat_view(views[e].pose, g_vrcfg, g_head_zero, V);
                        gx::render_set_vr_eye(P, V, H);
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
        } else if (fs.shouldRender) {
            // Touch the swapchain only when there is a new game frame to put in it.
            //
            // The compositor does not need a new image every display frame: a quad layer
            // keeps showing the last image released to it, reprojected at display rate,
            // which is what makes head tracking smooth however slowly the game renders --
            // and is exactly what the re-submission below relies on. Re-blitting identical
            // pixels into a freshly acquired image 72 times a second bought nothing and
            // meant cycling the swapchain under the compositor while it sampled, at the
            // one rate where a fade makes any mismatch visible. It also cost a full-screen
            // blit per display frame.
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (batch && XR_SUCCEEDED(xrAcquireSwapchainImage(g_xr.swapchain, &ai, &idx))) {
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_SUCCEEDED(xrWaitSwapchainImage(g_xr.swapchain, &wi))) {
                    gx::render_set_output_fbo(g_xr.fbos[idx]);
                    // Consume at most one game frame per display frame; otherwise a
                    // backlog would be drawn and thrown away.
                    const bool drew = gx::render_execute(*batch);
                    gx::render_set_output_fbo(0);
                    // `log_frames 1` in vr.txt traces the theater path one display frame
                    // at a time: which swapchain image was written, whether it got a new
                    // game frame or a repaint, and which game frame it is showing. The
                    // headless harness renders the same pixels but cannot reproduce the
                    // compositor, so flicker that is not in the EFB has to be caught here.
                    if (g_vrcfg.log_frames)
                        LOGI("[t] disp=%llu img=%u %s present=%u", (unsigned long long)disp_frames,
                             idx, drew ? "drew" : "no-present", gx::present_count());
                    // Issue the blit before handing the image back. A full fence wait
                    // here was tried against the character-select flicker and changed
                    // nothing, at about a tenth of the game's frame rate.
                    glFlush();
                    if (drew) have_content = quad_fresh = true;
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_xr.swapchain, &ri);
            }
        } else {
            skipped++;
        }

        // Once the panel has a frame of its own, the eyes' last images are history.
        // Forgetting them keeps the next race from opening on the end of the last one
        // should its first eye frame fail to render.
        if (quad_fresh) have_proj = false;

        // Submit whichever layer is current on every frame, including ones the runtime
        // told us not to render. Dropping the layer shows the user an empty frame,
        // which strobes against the frames that do carry it; re-submitting it just
        // re-displays the last released image -- reprojected for the current head pose.
        if (have_proj && !quad_fresh) {
            proj_layer.space = g_xr.space;
            proj_layer.viewCount = 2;
            proj_layer.views = proj_views;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj_layer);
        }
        // The panel also holds until the eyes have drawn something: a manual toggle can
        // land on a display frame with no game frame to render them from.
        if (have_content && (quad_fresh || !have_proj)) {
            quad.layerFlags = 0;
            quad.space = g_xr.space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_xr.swapchain;
            quad.subImage.imageRect = {{0, 0}, {g_swap_w, g_swap_h}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation = {0, 0, 0, 1};
            quad.pose.position = {0, 0, -kQuadDist};
            quad.size = {kQuadW, kQuadH};
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
    app_exit("activity destroyed");
}
