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
#include "gx/gl_msrtt.h"
#include "hw/pad.h"
#include <EGL/egl.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

uint32_t boot_load(const char* iso_path);
bool audio_open();
void debug_dump_threads();
bool write_png(const char* path, const uint8_t* rgba, int w, int h);
static void* gl_proc(const char* name);

// ---------------------------------------------------------------------------
// --eye renders through render_execute_eye into an offscreen target and dumps that,
// instead of the flat path. It exists so the stereo renderer can be looked at without
// a headset: an idle headset will not launch a 6DoF app, and shipping VR changes that
// cannot be checked first has already cost a regression.
//
// The view is the identity, so the eye sits exactly where the game's camera is and the
// result should closely match the flat render. Anything that differs -- geometry in the
// wrong place, missing render-to-texture results -- is a fault in the eye path.
// ---------------------------------------------------------------------------
static bool g_eye_mode = false;
static float g_eye_yaw = 0.0f;  // --eye-yaw: degrees of head turn, for spotting head-locked draws
static GLuint g_eye_fbo, g_eye_tex, g_eye_depth;
static int g_eye_w = 960, g_eye_h = 720;

// --eyes=2 renders the second eye as well, as the headset does; --eye-size=WxH renders at
// the headset's size rather than this harness's; --msaa=N multisamples the eye the way the
// headset can. Together with WR_EYE_GPU=1, which times the eye passes on the GPU, they are
// what answers "what does a higher eye resolution or antialiasing cost" without a headset.
static int g_eye_count = 1, g_eye_msaa = 0;

static void eye_init() {
    glGenTextures(1, &g_eye_tex);
    glBindTexture(GL_TEXTURE_2D, g_eye_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_eye_w, g_eye_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const gx::Msrtt ms = g_eye_msaa > 1 ? gx::msrtt_load(gl_proc) : gx::Msrtt{};
    if (g_eye_msaa > 1 && ms.max_samples < g_eye_msaa) {
        fprintf(stderr, "--msaa=%d unavailable (max %d); rendering without\n", g_eye_msaa,
                ms.max_samples);
        g_eye_msaa = 0;
    }
    glGenRenderbuffers(1, &g_eye_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_eye_depth);
    if (g_eye_msaa > 1)
        ms.renderbuffer_storage(GL_RENDERBUFFER, g_eye_msaa, GL_DEPTH_COMPONENT24, g_eye_w, g_eye_h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_eye_w, g_eye_h);
    glGenFramebuffers(1, &g_eye_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo);
    if (g_eye_msaa > 1)
        ms.framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_eye_tex, 0,
                                  g_eye_msaa);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_eye_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_eye_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "eye fbo incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// The HUD frame has no vr.txt to read here, so these match its defaults at the default
// 100 game units per metre: 4 m out, 1.3 m up, leaned 23.2 degrees back towards the
// viewer to sit parallel to the game world's vertical rather than the room's. The point
// of the harness is to show what the headset shows.
static const float kHudDist = 400.0f;
static const float kHudScale = 0.5f;
static const float kHudHeight = -36.0f;
static const float kHudPitch = 0.0f;
static const float kWorldPitch = 23.2f * 3.14159265f / 180.0f;

// Column-major, matching glUniformMatrix4fv with transpose = GL_FALSE. The game's view
// space is -Z forward, so this is an ordinary GL perspective in game units.
static void eye_matrices(float* proj, float* view, float* hud) {
    const float fov = 1.0f;          // tan(45 deg): a 90 degree vertical field
    const float aspect = (float)g_eye_w / (float)g_eye_h;
    const float n = 10.0f, f = 500000.0f;
    memset(proj, 0, 16 * sizeof(float));
    proj[0] = 1.0f / (fov * aspect);
    proj[5] = 1.0f / fov;
    proj[10] = -(f + n) / (f - n);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * f * n) / (f - n);
    // --eye-yaw turns the head. With the view left at identity nothing in the image can
    // ever be seen to be head-locked, which is how a change that pinned the ocean and a
    // copy of the racer to the viewer's face got through this harness looking correct.
    // Dump the same frame at two yaws: whatever does not move with the world is locked.
    const float a = g_eye_yaw * 3.14159265f / 180.0f;
    memset(view, 0, 16 * sizeof(float));
    view[0] = cosf(a);  view[2] = -sinf(a);
    view[8] = sinf(a);  view[10] = cosf(a);
    view[5] = view[15] = 1.0f;
    // The HUD frame is anchored in front of the game's camera, not the head, so --eye-yaw
    // swings it out of view exactly as turning to look away does in the headset. A HUD
    // that sits still under a yaw is one that is still locked to the viewer's face.
    gx::render_hud_frame(kHudDist, fov, kHudScale, kHudHeight, kHudPitch, hud);
}

static void dump_fbo(GLuint fbo, int w, int h, const char* path) {
    std::vector<uint8_t> px((size_t)w * h * 4), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const size_t stride = (size_t)w * 4;
    for (int y = 0; y < h; y++)
        memcpy(&fl[y * stride], &px[(size_t)(h - 1 - y) * stride], stride);
    for (size_t i = 3; i < fl.size(); i += 4) fl[i] = 255;
    write_png(path, fl.data(), w, h);
}

static void eye_dump(const char* dir, uint32_t n, const char* suffix = "") {
    char path[512];
    snprintf(path, sizeof(path), "%s/eye_%05u%s.png", dir, n, suffix);
    dump_fbo(g_eye_fbo, g_eye_w, g_eye_h, path);
}

// WR_EYE_MORPH=0,0.5,1 renders every dumped frame once per value, as eye_NNNNN_mXXXX.png
// (thousandths),
// with the stereo view folded that far back towards theater -- see render_set_vr_morph --
// plus the flat frame as flat_NNNNN.png. The panel hangs where theater hangs it, at this
// harness's 100 game units per metre, so the identity view sees it 2.5 m ahead and 3.2 m
// wide: at 0 the middle of the eye image should be flat_NNNNN.png, and nothing else.
static std::vector<float> g_morphs;
static GLuint g_flat_fbo, g_flat_tex;
static void morph_panel(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 160.0f;
    m[5] = 120.0f;
    m[14] = -250.0f;
    m[15] = 1.0f;
}

// ---------------------------------------------------------------------------
// WR_EYE_GPU=1: how long the eye passes take on the GPU, from GL_EXT_disjoint_timer_query,
// and on the CPU to issue, averaged and printed every 2 s. The GPU figure covers both eyes
// and the flat pass the first eye runs to produce the frame's copies. Queries are read a few
// frames late from a ring, so measuring does not itself stall the pipeline.
static bool g_gpu_timing = false;
static void (*g_query_u64)(GLuint, GLenum, uint64_t*) = nullptr;
static GLuint g_queries[8];
static int g_q_head = 0, g_q_live = 0;
static double g_gpu_ms_sum = 0, g_cpu_ms_sum = 0;
static int g_gpu_n = 0, g_cpu_n = 0;
static constexpr GLenum kTimeElapsed = 0x88BF;   // GL_TIME_ELAPSED_EXT

static void gpu_timer_init() {
    g_gpu_timing = getenv("WR_EYE_GPU") != nullptr;
    if (!g_gpu_timing) return;
    g_query_u64 = (void (*)(GLuint, GLenum, uint64_t*))gl_proc("glGetQueryObjectui64vEXT");
    if (!g_query_u64) {
        fprintf(stderr, "WR_EYE_GPU: no GL_EXT_disjoint_timer_query; CPU times only\n");
        return;
    }
    glGenQueries(8, g_queries);
}

static void gpu_timer_collect(bool all) {
    // A disjoint event -- the GPU changing clock, say -- makes the queries in flight
    // meaningless; they came back as hours. Drop them. The flag alone did not catch every
    // such sample on a Quest 3, so anything over a second goes too.
    GLint disjoint = 0;
    glGetIntegerv(0x8FBB /* GL_GPU_DISJOINT_EXT */, &disjoint);
    while (g_q_live > 0) {
        const GLuint q = g_queries[(g_q_head - g_q_live + 8) % 8];
        GLuint ready = 0;
        if (!all) glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!all && !ready) break;
        uint64_t ns = 0;
        g_query_u64(q, GL_QUERY_RESULT, &ns);
        if (!disjoint && ns < 1000000000ull) {
            g_gpu_ms_sum += ns * 1e-6;
            g_gpu_n++;
        }
        g_q_live--;
    }
}

static void gpu_timer_begin() {
    if (!g_query_u64) return;
    if (g_q_live == 8) gpu_timer_collect(true);
    glBeginQuery(kTimeElapsed, g_queries[g_q_head]);
}

static void gpu_timer_end(double cpu_ms) {
    if (!g_gpu_timing) return;
    g_cpu_ms_sum += cpu_ms;
    g_cpu_n++;
    if (!g_query_u64) return;
    glEndQuery(kTimeElapsed);
    g_q_head = (g_q_head + 1) % 8;
    g_q_live++;
    gpu_timer_collect(false);
}

static void gpu_timer_report() {
    if (!g_gpu_timing || !g_cpu_n) return;
    printf("  eyes: cpu %.2f ms", g_cpu_ms_sum / g_cpu_n);
    if (g_gpu_n) printf("  gpu %.2f ms", g_gpu_ms_sum / g_gpu_n);
    printf("  (%d frames)\n", g_cpu_n);
    g_gpu_ms_sum = g_cpu_ms_sum = 0;
    g_gpu_n = g_cpu_n = 0;
}

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
        else if (!strcmp(argv[i], "--eye")) g_eye_mode = true;
        else if (!strncmp(argv[i], "--eye-yaw=", 10)) g_eye_yaw = (float)atof(argv[i] + 10);
        else if (!strncmp(argv[i], "--eye-size=", 11)) sscanf(argv[i] + 11, "%dx%d", &g_eye_w, &g_eye_h);
        else if (!strncmp(argv[i], "--eyes=", 7)) g_eye_count = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--msaa=", 7)) g_eye_msaa = atoi(argv[i] + 7);
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

    gx::render_set_shader_cache("shaders.bin");
    gx::render_init(scale);
    if (g_eye_mode) {
        eye_init();
        gpu_timer_init();
        gx::render_set_world_pitch(kWorldPitch);
        if (const char* s = getenv("WR_EYE_MORPH")) {
            for (const char* q = s; *q;) {
                g_morphs.push_back((float)atof(q));
                q = strchr(q, ',');
                if (!q) break;
                q++;
            }
            glGenTextures(1, &g_flat_tex);
            glBindTexture(GL_TEXTURE_2D, g_flat_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGenFramebuffers(1, &g_flat_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_flat_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_flat_tex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    // The flat frame WR_EYE_MORPH dumps is blitted at 640x480 whatever --scale says.
    gx::render_set_window_size(g_morphs.empty() ? 640 * scale : 640,
                               g_morphs.empty() ? 480 * scale : 480);

    // WR_AUDIO=1 opens the device here too. Off by default because this harness exists
    // to be run over adb on a device somebody may be wearing, but it is the only way to
    // exercise the audio path without the VR frontend -- and with WR_WAV it records what
    // the device was handed, which is checkable afterwards rather than by listening.
    if (getenv("WR_AUDIO") && !audio_open())
        fprintf(stderr, "audio unavailable; continuing without sound\n");

    uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);

    printf("rendering (scale %d)%s...\n", scale,
           gx::g_dump_dir ? "" : "  [no --dump-dir, nothing will be written]");
    fflush(stdout);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto t_mark = t0;
    uint32_t presented = 0, presented_at_mark = 0, last_snap = 0;
    for (;;) {
        PadState p;
        p.connected = true;
        input_script_apply(p);
        pad_set_state(0, p);

        // WR_RAMSNAP=dir dumps guest RAM once a second. Diffing snapshots taken in known
        // game states is how a variable like "the race is running" gets found; the draw
        // count the stereo switch uses now is a guess that fires on the course overview.
        static const char* ramsnap = getenv("WR_RAMSNAP");
        static const uint32_t snap_every = getenv("WR_RAMSNAP_EVERY")
                                               ? atoi(getenv("WR_RAMSNAP_EVERY")) : 150;
        // The low 8 MB: enough to hold the game's own state without writing 24 MB a shot.
        static const uint32_t snap_bytes = 8u << 20;
        // WR_RAMSNAP_RANGE=a-b bounds it to a window of frames. A transition that takes a
        // second needs a snapshot every few frames to bracket, and a whole run at that
        // interval is gigabytes; the states either side of one boundary are all that a
        // diff for that boundary needs.
        static int snap_lo = -1, snap_hi = -1;
        static bool snap_range_parsed = false;
        if (!snap_range_parsed) {
            snap_range_parsed = true;
            if (const char* r = getenv("WR_RAMSNAP_RANGE")) {
                snap_lo = atoi(r);
                const char* dash = strchr(r, '-');
                snap_hi = dash ? atoi(dash + 1) : snap_lo;
            }
        }
        const bool snap_in_range = snap_lo < 0 ||
                                   ((int)presented >= snap_lo && (int)presented <= snap_hi);
        if (ramsnap && snap_in_range && presented && presented % snap_every == 0 &&
            presented != last_snap) {
            last_snap = presented;
            char path[512];
            snprintf(path, sizeof(path), "%s/ram_%05u.bin", ramsnap, presented);
            if (FILE* f = fopen(path, "wb")) {
                fwrite(mem_ptr(0x80000000), 1, snap_bytes, f);
                fclose(f);
            }
        }
        if (auto b = gx::take_batch(4)) {
            if (g_eye_mode) {
                float P[16], V[16], H[16];
                eye_matrices(P, V, H);
                gx::render_set_vr_morph(1.0f, nullptr);
                gpu_timer_begin();
                const auto t_eyes = clock::now();
                if (g_eye_count > 1) {
                    // The other eye first, into the same target, so what is dumped is the
                    // left one. 64 mm apart at this harness's 100 units to the metre.
                    float V2[16];
                    memcpy(V2, V, sizeof(V2));
                    V2[12] -= 6.4f;
                    gx::render_set_vr_eye(P, V2, H);
                    gx::render_execute_eye(*b, g_eye_fbo, g_eye_w, g_eye_h, true);
                }
                gx::render_set_vr_eye(P, V, H);
                gx::render_execute_eye(*b, g_eye_fbo, g_eye_w, g_eye_h, g_eye_count < 2);
                gpu_timer_end(std::chrono::duration<double, std::milli>(clock::now() - t_eyes).count());
                // As the headset does after each eye. Nothing else here hands the GPU its
                // work, and a driver left to queue frames up stalls the next frame's vertex
                // upload until they drain: without this the flat pass measured 17-21 ms a
                // race frame, almost all of it waiting in glBufferData, against 6 ms with it.
                glFlush();
                presented++;
                // WR_DUMP_COPIES=N dumps whenever a frame holds at least N EFB copies,
                // which is how a frame thick with spray is caught: the faults that only
                // appear at speed are in exactly those frames, and a fixed interval
                // almost never lands on one.
                static const int want_copies = getenv("WR_DUMP_COPIES")
                                                   ? atoi(getenv("WR_DUMP_COPIES")) : 0;
                int ncopies = 0;
                if (want_copies)
                    for (auto& c : b->cmds) ncopies += c.type == gx::CmdType::EfbCopy;
                // WR_DUMP_RANGE=a-b narrows dumping to a window of frames, as it does in
                // the flat path, so a short stretch can be caught every few frames
                // without writing a gigabyte. Something that is only on screen for three
                // seconds -- the countdown -- is otherwise missed by any interval coarse
                // enough to run a whole race with.
                static int range_lo = -1, range_hi = -1;
                static bool range_parsed = false;
                if (!range_parsed) {
                    range_parsed = true;
                    if (const char* r = getenv("WR_DUMP_RANGE")) {
                        range_lo = atoi(r);
                        const char* dash = strchr(r, '-');
                        range_hi = dash ? atoi(dash + 1) : range_lo;
                    }
                }
                const bool in_range = range_lo < 0 ||
                                      ((int)presented >= range_lo && (int)presented <= range_hi);
                if (gx::g_dump_dir && in_range &&
                    ((gx::g_dump_every && presented % gx::g_dump_every == 0) ||
                     (want_copies && ncopies >= want_copies))) {
                    eye_dump(gx::g_dump_dir, presented);
                    if (!g_morphs.empty()) {
                        float panel[16];
                        morph_panel(panel);
                        for (float m : g_morphs) {
                            gx::render_set_vr_morph(m, panel);
                            gx::render_execute_eye(*b, g_eye_fbo, g_eye_w, g_eye_h, false);
                            char suffix[16];
                            snprintf(suffix, sizeof(suffix), "_m%04d", (int)lroundf(m * 1000.0f));
                            eye_dump(gx::g_dump_dir, presented, suffix);
                        }
                        gx::render_set_vr_morph(1.0f, nullptr);
                        // The flat frame, last: it runs the batch again, copies and all.
                        gx::render_set_output_fbo(g_flat_fbo);
                        gx::render_execute(*b);
                        gx::render_set_output_fbo(0);
                        char path[512];
                        snprintf(path, sizeof(path), "%s/flat_%05u.png", gx::g_dump_dir, presented);
                        dump_fbo(g_flat_fbo, 640, 480, path);
                    }
                }
            } else {
                if (gx::render_execute(*b)) presented++;
                glFlush();  // as the eye path does; see there
            }
        }

        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        if (std::chrono::duration<double>(now - t_mark).count() >= 2.0) {
            printf("  %4.0fs  presented %u (+%u)\n", elapsed, presented, presented - presented_at_mark);
            gpu_timer_report();
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
