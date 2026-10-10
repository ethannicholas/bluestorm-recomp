// The game's frame rate, and running it at 60 (WR_FPS=60, or `fps 60` in vr.txt). See
// docs/dev/performance.md, "60 frames per second".
//
// The game paces itself on the retrace. fn_80007A94 ends a frame: it spins until at least
// N retraces have passed since the mark it took at the end of the last frame (N is 1 in
// play, 2 in mode 0), takes a new mark, hands the video interface the next frame buffer
// and waits for one more retrace. So a frame that fits in a field still takes two, and
// the game is 30 frames a second by construction; with N = 0 the spin is skipped and a
// frame that fits in a field takes one. The patches in recomp/patches.txt at 0x80007B3C
// and 0x80007B44 route N through wr_fields_per_frame.
//
// The simulation then steps everything per frame: the game keeps a clock in seconds at
// 0x806919F0, advanced once a frame by the constant at 0x80692400 (1/30) in the mode
// runner fn_80006D20 (0x80006F28), a frame counter at 0x80690FF8 (0x80006F14), and some
// hundreds of other per-frame steps scattered through its logic, each with a constant of
// its own. At 60 every one of them has to take half a step. The clock and the counter are
// patched by hand (wr_frame_dt, wr_frame_count_step); the rest are listed in
// recomp/steps.txt, found by tools/rate_sites.py, and emitted by the recompiler through
// the shared runtime's step scale (gcn-recomp/runtime/step.cpp): x += t becomes
// x += t/2, x *= k becomes x *= sqrt(k), and an integer counter steps on every other
// frame, the frame counter among them.
//
// The virtual clock charges a frame's work two fields (gcn-recomp/docs/diagnostics.md,
// "The clock"), so at 60 the guest also has to be a CPU twice as fast: clock_set_cpu_scale.
#include "runtime.h"
#include "pace.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

namespace gx { extern std::atomic<uint32_t> g_frames_submitted; }

namespace {
int g_fps = 30;
// WR_WATCH=<hex>,<hex>,... prints those guest words once a game frame, as hex and as a
// float, keyed by the game's frame counter and the presented frame (the count GCN_INPUT and
// GCN_STORE_HIST use): `[ww] f<frame> p<presented> <addr>=<hex>/<float> ...`. It
// hangs off the frame counter's advance, so it needs no renderer (a headless --fast run)
// and no debug build; tools/compare_runs.py reads it beside the eye hook's [fp] lines.
std::vector<uint32_t> g_watch = [] {
    std::vector<uint32_t> v;
    if (const char* e = getenv("WR_WATCH")) {
        for (const char* p = e; *p;) {
            char* end;
            uint32_t a = (uint32_t)strtoul(p, &end, 16);
            if (end == p) break;
            v.push_back(a);
            p = *end == ',' ? end + 1 : end;
        }
    }
    return v;
}();
// WR_SNAP=<dir>:<p>,<p>,...: at those presented frames, write the guest's writable data
// (0x80340000-0x80692400: .bss, .sdata and .sbss) to <dir>/snap_<p>.bin, for finding the
// word that flips at an event by comparing snapshots from before and after it.
std::string g_snap_dir;
std::vector<uint32_t> g_snap_at = [] {
    std::vector<uint32_t> v;
    if (const char* e = getenv("WR_SNAP")) {
        const char* colon = strchr(e, ':');
        if (colon) {
            g_snap_dir.assign(e, colon - e);
            for (const char* p = colon + 1; *p;) {
                char* end;
                uint32_t a = (uint32_t)strtoul(p, &end, 10);
                if (end == p) break;
                v.push_back(a);
                p = *end == ',' ? end + 1 : end;
            }
        }
    }
    return v;
}();
void snap_check() {
    if (g_snap_at.empty()) return;
    uint32_t p = gx::g_frames_submitted.load(std::memory_order_relaxed);
    for (size_t i = 0; i < g_snap_at.size(); i++) {
        if (p < g_snap_at[i]) continue;
        std::string path = g_snap_dir + "/snap_" + std::to_string(g_snap_at[i]) + ".bin";
        if (FILE* f = fopen(path.c_str(), "wb")) {
            fwrite(HOST(0x80340000u), 1, 0x80692400u - 0x80340000u, f);
            fclose(f);
            fprintf(stderr, "[snap] p%u -> %s\n", p, path.c_str());
        }
        g_snap_at.erase(g_snap_at.begin() + i);
        i--;
    }
}
void watch_print(uint32_t frame) {
    snap_check();
    if (g_watch.empty()) return;
    fprintf(stderr, "[ww] f%u p%u", frame, gx::g_frames_submitted.load(std::memory_order_relaxed));
    for (uint32_t a : g_watch) {
        uint32_t w = mem_r32(a);
        float f;
        memcpy(&f, &w, 4);
        fprintf(stderr, " %08X=%08X/%g", a, w, f);
    }
    fputc('\n', stderr);
}
// WR_PACE_KEEP=<bits>, for finding what a 60 fps run trips over: 1 advances the frame
// counter every frame instead of every other, 2 keeps the clock's full 1/30 step, 4 keeps
// the two-field pacing (then only the CPU scale is in effect), 8 leaves the steps.txt
// sites unscaled.
int g_keep = 0;
// WR_FPS_AT=<presented frame>: switch to 60 at that frame rather than at boot, so that a
// scripted route can run its menus at 30 (where the script's frame counts hold) and only
// the race at 60; a 60 fps run that goes wrong is bisected many times faster that way.
uint32_t g_fps_at = 0;

const bool installed = [] {
    if (const char* e = getenv("WR_PACE_KEEP")) g_keep = atoi(e);
    if (const char* e = getenv("WR_FPS")) wr::set_fps(atoi(e));
    if (const char* e = getenv("WR_FPS_AT")) g_fps_at = (uint32_t)atoi(e);
    return true;
}();
}  // namespace

namespace wr {
void set_fps(int fps) {
    if (fps != 30 && fps != 60) return;
    g_fps = fps;
    clock_set_cpu_scale(fps == 60 ? 2.0 : 1.0);
    step_set_scale(fps == 60 && !(g_keep & 8) ? 0.5 : 1.0);
    fprintf(stderr, "[pace] %d frames per second\n", fps);
}
int fps() { return g_fps; }
float frame_dt() { return 1.0f / (float)g_fps; }
}  // namespace wr

extern "C" uint32_t wr_fields_per_frame(uint32_t game) { return g_fps == 60 && !(g_keep & 4) ? 0u : game; }
extern "C" double wr_frame_dt(double game) { return g_fps == 60 && !(g_keep & 2) ? game * 0.5 : game; }
// Called once a game frame, from the mode runner's advance of the frame counter, with the
// counter's value before the advance. This is where the step scale learns that a frame has
// passed, and the counter itself steps like any other integer: every other frame at 60.
extern "C" uint32_t gcn_step_int(uint32_t pc, double p);
extern "C" uint32_t wr_frame_count_step(uint32_t frame) {
    watch_print(frame);
    if (g_fps_at && gx::g_frames_submitted.load(std::memory_order_relaxed) >= g_fps_at) {
        g_fps_at = 0;
        wr::set_fps(60);
    }
    step_frame();
    return g_fps == 60 && !(g_keep & 1) ? gcn_step_int(0x80006F14u, 1.0) : 1u;
}
