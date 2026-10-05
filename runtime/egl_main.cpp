// Headless EGL frontend for Android: runs the real renderer offscreen and writes frames
// out as PNGs.
//
// No window, no OpenXR, no APK. The point is to validate the OpenGL ES renderer on a
// device over adb -- shader compilation, the TEV pipeline, textures, EFB copies -- before
// building a VR frontend on top of it. The renderer draws into its own framebuffer
// object and the frame dump reads that back, so the EGL surface only exists to make a
// context current and can be 16x16.
//
// Usage: waverace_egl [--scale=N] [--frames=N] [--seconds=N]
//                     [--dump-dir=DIR] [--dump-every=N] [path/to/game.iso]
#include "runtime.h"
#include "platform.h"
#include "gx/render.h"
#include "gx/render_gl.h"
#include "input_script.h"
#include "gx/gl.h"
#include "hw/pad.h"
#include <EGL/egl.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();

// ---------------------------------------------------------------------------
static void on_interrupt() {
    plat_watchdog(2, 2);
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    uintptr_t a = (uintptr_t)fault_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n",
                (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (code %d)\n", fault_addr, code);
    plat_watchdog(2, 2);
    plat_backtrace_print();
    debug_dump_threads();
    plat_exit_now(1);
}

// ---------------------------------------------------------------------------
// GL entry points. Android's eglGetProcAddress does resolve core ES functions, but not
// on every driver, so prefer dlsym on the ES library and fall back to EGL.
// ---------------------------------------------------------------------------
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

static EGLDisplay g_dpy = EGL_NO_DISPLAY;

static bool egl_init() {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return false; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed (0x%X)\n", eglGetError());
        return false;
    }
    printf("EGL %d.%d  %s\n", major, minor, eglQueryString(g_dpy, EGL_VENDOR));

    // A pbuffer config rather than a window config: there is no surface to present to.
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "eglChooseConfig found no ES3 pbuffer config (0x%X)\n", eglGetError());
        return false;
    }

    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, WR_GL_MAJOR,
        EGL_CONTEXT_MINOR_VERSION, WR_GL_MINOR,
        EGL_NONE,
    };
    EGLContext ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext for ES %d.%d failed (0x%X)\n",
                WR_GL_MAJOR, WR_GL_MINOR, eglGetError());
        return false;
    }

    const EGLint pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(g_dpy, cfg, pb_attr);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreatePbufferSurface failed (0x%X)\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g_dpy, surf, surf, ctx)) {
        fprintf(stderr, "eglMakeCurrent failed (0x%X)\n", eglGetError());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string iso = WR_DEFAULT_ISO;
    int scale = 1, frames_wanted = 0, seconds = 120;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--frames=", 9)) frames_wanted = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--seconds=", 10)) seconds = atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--dump-dir=", 11)) gx::g_dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) gx::g_dump_every = atoi(argv[i] + 13);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    if (!plat_readable(iso.c_str())) iso = plat_find_file("rom", ".iso");
    if (!plat_readable(iso.c_str())) {
        fprintf(stderr, "No game image found. Pass the path to your .iso.\n");
        return 1;
    }
    if (gx::g_dump_dir) plat_make_dirs(gx::g_dump_dir);

    plat_install_crash_handlers(on_interrupt, on_fault);

    mem_init();
    timing_init();
    input_script_init();

    if (!egl_init()) return 1;
    int glver = gl_load_with(gl_proc);
    if (glver < WR_GL_VERSION_MIN) {
        fatal("OpenGL ES %d.%d is too old (need %d.%d)", glver / 10, glver % 10,
              WR_GL_MAJOR, WR_GL_MINOR);
    }
    printf("GL_VERSION  %s\nGL_RENDERER %s\nGL_VENDOR   %s\n",
           (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
           (const char*)glGetString(GL_VENDOR));
    fflush(stdout);

    gx::render_init(scale);
    gx::render_set_window_size(640 * scale, 480 * scale);

    uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);

    printf("rendering (scale %d)%s...\n", scale,
           gx::g_dump_dir ? "" : "  [no --dump-dir, nothing will be written]");
    fflush(stdout);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto t_mark = t0;
    uint32_t presented = 0, presented_at_mark = 0;
    for (;;) {
        PadState p;
        p.connected = true;
        input_script_apply(p);
        pad_set_state(0, p);

        if (auto b = gx::take_batch(4)) {
            if (gx::render_execute(*b)) presented++;
        }

        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        if (std::chrono::duration<double>(now - t_mark).count() >= 2.0) {
            printf("  %4.0fs  presented %u (+%u)\n", elapsed, presented, presented - presented_at_mark);
            fflush(stdout);
            presented_at_mark = presented;
            t_mark = now;
        }
        if (frames_wanted && presented >= (uint32_t)frames_wanted) break;
        if (elapsed >= seconds) break;
    }

    printf("done: %u frames presented in %.1f s\n", presented,
           std::chrono::duration<double>(clock::now() - t0).count());
    fflush(stdout);

    // Guest threads are parked in longjmp-based contexts; don't unwind them.
    plat_exit_now(0);
}
