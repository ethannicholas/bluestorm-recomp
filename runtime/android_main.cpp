// Android NativeActivity frontend: renders the game to the app window.
//
// On Horizon OS this appears under "Unknown Sources" and runs in a flat panel. It is
// deliberately not a VR app yet -- it is the smallest thing that puts the game on the
// headset's display, and the same EGL/GL ES setup is what an OpenXR frontend would sit
// on top of.
//
// The disc image is not shipped in the APK (it is the user's own, and far too large).
// It is read from the app's external files directory, which needs no runtime
// permission:  /sdcard/Android/data/<package>/files/game.iso
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "gx/render.h"
#include "gx/render_gl.h"
#include "gx/gl.h"
#include "hw/pad.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <string>

#define TAG "waverace"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

uint32_t boot_load(const char* iso_path);

static int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// The runtime logs with printf and fprintf(stderr), which go nowhere in an Android
// app. Pump both through a pipe into logcat so `adb logcat -s waverace` shows them.
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
// EGL
// ---------------------------------------------------------------------------
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig g_cfg;
static int g_win_w, g_win_h;

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

static bool egl_create(ANativeWindow* win) {
    if (g_dpy == EGL_NO_DISPLAY) {
        g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(g_dpy, nullptr, nullptr)) { LOGE("eglInitialize failed"); return false; }
        const EGLint cfg_attr[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE,
        };
        EGLint n = 0;
        if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) {
            LOGE("no ES3 window config"); return false;
        }
        const EGLint ctx_attr[] = {
            EGL_CONTEXT_MAJOR_VERSION, WR_GL_MAJOR,
            EGL_CONTEXT_MINOR_VERSION, WR_GL_MINOR,
            EGL_NONE,
        };
        g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
        if (g_ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
    }
    // The window's native visual has to match the chosen config.
    EGLint vis = 0;
    eglGetConfigAttrib(g_dpy, g_cfg, EGL_NATIVE_VISUAL_ID, &vis);
    ANativeWindow_setBuffersGeometry(win, 0, 0, vis);

    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, win, nullptr);
    if (g_surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return false; }
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) { LOGE("eglMakeCurrent failed"); return false; }
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &g_win_w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &g_win_h);
    LOGI("surface %dx%d", g_win_w, g_win_h);
    return true;
}

static void egl_destroy_surface() {
    if (g_surf != EGL_NO_SURFACE) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_dpy, g_surf);
        g_surf = EGL_NO_SURFACE;
    }
}

// ---------------------------------------------------------------------------
// Input: whatever gamepad-ish events reach us, mapped onto GC pad 1.
// ---------------------------------------------------------------------------
static PadState g_pad;

static int32_t on_input(android_app*, AInputEvent* ev) {
    const int32_t type = AInputEvent_getType(ev);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(ev);
        const bool down = AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN;
        uint16_t bit = 0;
        switch (code) {
        case AKEYCODE_BUTTON_A: case AKEYCODE_DPAD_CENTER: bit = PAD_A; break;
        case AKEYCODE_BUTTON_B: bit = PAD_B; break;
        case AKEYCODE_BUTTON_X: bit = PAD_X; break;
        case AKEYCODE_BUTTON_Y: bit = PAD_Y; break;
        case AKEYCODE_BUTTON_R1: bit = PAD_Z; break;
        case AKEYCODE_BUTTON_L1: bit = PAD_L; break;
        case AKEYCODE_BUTTON_START: case AKEYCODE_ENTER: bit = PAD_START; break;
        case AKEYCODE_DPAD_UP: bit = PAD_UP; break;
        case AKEYCODE_DPAD_DOWN: bit = PAD_DOWN; break;
        case AKEYCODE_DPAD_LEFT: bit = PAD_LEFT; break;
        case AKEYCODE_DPAD_RIGHT: bit = PAD_RIGHT; break;
        default: return 0;
        }
        if (down) g_pad.buttons |= bit; else g_pad.buttons &= ~bit;
        if (bit == PAD_L) g_pad.trig_l = down ? 255 : 0;
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(ev) & AINPUT_SOURCE_JOYSTICK)) {
        auto axis = [&](int32_t a) { return AMotionEvent_getAxisValue(ev, a, 0); };
        auto to_u8 = [](float f) {
            int v = 128 + (int)(f * 100.0f);
            return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        };
        g_pad.stick_x = to_u8(axis(AMOTION_EVENT_AXIS_X));
        g_pad.stick_y = to_u8(-axis(AMOTION_EVENT_AXIS_Y));   // Android Y is down
        g_pad.cstick_x = to_u8(axis(AMOTION_EVENT_AXIS_Z));
        g_pad.cstick_y = to_u8(-axis(AMOTION_EVENT_AXIS_RZ));
        float rt = axis(AMOTION_EVENT_AXIS_RTRIGGER);
        g_pad.trig_r = (uint8_t)(rt * 255.0f);
        if (rt > 0.9f) g_pad.buttons |= PAD_R; else g_pad.buttons &= ~PAD_R;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
static std::atomic<bool> g_window_ready{false};
static bool g_game_started = false;

static void on_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        LOGI("APP_CMD_INIT_WINDOW");
        if (app->window && egl_create(app->window)) g_window_ready = true;
        break;
    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");
        g_window_ready = false;
        egl_destroy_surface();
        break;
    case APP_CMD_GAINED_FOCUS: LOGI("focus gained"); break;
    case APP_CMD_LOST_FOCUS:   LOGI("focus lost"); break;
    default:
        break;
    }
}

// Optional: a WR_INPUT-style script in the external files directory drives the game
// without a controller, which is how to see a race if no gamepad is paired.
static std::string read_script(const std::string& dir) {
    std::string path = dir + "/wr_input.txt";
    FILE* f = fopen(path.c_str(), "rb");
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
    app->onInputEvent = on_input;

    const std::string dir = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    const std::string iso = dir + "/game.iso";
    LOGI("looking for %s", iso.c_str());

    // Wait for a window before touching GL.
    while (!g_window_ready && !app->destroyRequested) {
        int events;
        android_poll_source* src;
        if (ALooper_pollOnce(-1, nullptr, &events, (void**)&src) >= 0 && src) src->process(app, src);
    }
    if (app->destroyRequested) return;

    if (!plat_readable(iso.c_str())) {
        LOGE("no game image at %s -- push one with:", iso.c_str());
        LOGE("  adb push game.iso %s", iso.c_str());
        // Keep the activity alive so the message is readable in logcat.
        while (!app->destroyRequested) {
            int events;
            android_poll_source* src;
            if (ALooper_pollOnce(250, nullptr, &events, (void**)&src) >= 0 && src) src->process(app, src);
        }
        return;
    }

    int glver = gl_load_with(gl_proc);
    if (glver < WR_GL_VERSION_MIN) { LOGE("OpenGL ES %d.%d too old", glver / 10, glver % 10); return; }
    LOGI("GL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    mem_init();
    timing_init();
    input_script_init(read_script(dir).c_str());

    gx::render_init(1);
    gx::render_set_window_size(g_win_w, g_win_h);

    uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);
    g_game_started = true;
    LOGI("game started");

    uint32_t presented = 0, presented_at_report = 0;
    int64_t last_report = now_ms();

    while (!app->destroyRequested) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(0, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) return;
        }
        // Report before the window check: an idle headset tears the window down, and a
        // silent log is indistinguishable from a hang.
        const int64_t now = now_ms();
        if (now - last_report >= 5000) {
            LOGI("presented %u frames (%.1f fps)%s", presented,
                 (presented - presented_at_report) * 1000.0 / (now - last_report),
                 g_window_ready ? "" : "  [no window: headset idle or app backgrounded]");
            presented_at_report = presented;
            last_report = now;
        }
        if (!g_window_ready) { usleep(16000); continue; }

        PadState p = g_pad;
        p.connected = true;
        input_script_apply(p);
        pad_set_state(0, p);

        gx::render_set_window_size(g_win_w, g_win_h);
        if (auto b = gx::take_batch(4)) {
            if (gx::render_execute(*b)) {
                eglSwapBuffers(g_dpy, g_surf);
                presented++;
            }
        }
    }
    (void)g_game_started;
}
