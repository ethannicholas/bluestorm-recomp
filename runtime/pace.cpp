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
#include <cmath>
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
// WR_SNAP=<dir>:<p>,<p>-<q>,...: at those presented frames (a range is every frame of it),
// write the guest's writable data
// (0x80340000-0x80692400: .bss, .sdata and .sbss) to <dir>/snap_<p>.bin, for finding the
// word that flips at an event by comparing snapshots from before and after it.
// WR_SNAP_RANGE=<lo>-<hi> (hex) snapshots another range instead: the heap, where the
// course's objects live, is the arena from 0x806A48C0 to 0x817F9E20.
std::string g_snap_dir;
uint32_t g_snap_lo = 0x80340000u, g_snap_hi = 0x80692400u;
std::vector<uint32_t> g_snap_at = [] {
    std::vector<uint32_t> v;
    if (const char* e = getenv("WR_SNAP_RANGE")) {
        char* end;
        uint32_t lo = (uint32_t)strtoul(e, &end, 16);
        if (*end == '-') {
            uint32_t hi = (uint32_t)strtoul(end + 1, &end, 16);
            if (hi > lo) { g_snap_lo = lo; g_snap_hi = hi; }
        }
    }
    if (const char* e = getenv("WR_SNAP")) {
        const char* colon = strchr(e, ':');
        if (colon) {
            g_snap_dir.assign(e, colon - e);
            for (const char* p = colon + 1; *p;) {
                char* end;
                uint32_t a = (uint32_t)strtoul(p, &end, 10), b = a;
                if (end == p) break;
                if (*end == '-') b = (uint32_t)strtoul(end + 1, &end, 10);  // <p>-<q>: every frame of the range
                for (uint32_t f = a; f <= b && f < a + 4096; f++) v.push_back(f);
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
            fwrite(HOST(g_snap_lo), 1, g_snap_hi - g_snap_lo, f);
            fclose(f);
            fprintf(stderr, "[snap] p%u %08X-%08X -> %s\n", p, g_snap_lo, g_snap_hi, path.c_str());
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
// sites unscaled, 16 keeps the riders' four physics passes a paced frame (wr_rider_passes).
int g_keep = 0;
// WR_FPS_AT=<presented frame>: switch to 60 at that frame rather than at boot, so that a
// scripted route can run its menus at 30 (where the script's frame counts hold) and only
// the race at 60; a 60 fps run that goes wrong is bisected many times faster that way.
uint32_t g_fps_at = 0;
// A switch asked for from another thread (the headset's left thumbstick click), applied at
// the next frame's end so that the rate only ever changes between frames, on the guest thread.
std::atomic<int> g_fps_request{0};
// The rate asked for (WR_FPS, `fps` in vr.txt, the thumbstick); g_fps is the rate in force,
// which is the wanted one in a race and 30 everywhere else (wr_fields_per_frame).
int g_wanted = 30;
// Whether the game is in a race: the same guest state that puts the headset in stereo
// (runtime/vr_waverace.cpp, docs/dev/vr.md "Theater and stereo"), less the starting-light
// rig, which is a draw test. The on-course count at 0x80602160 is positive from the course
// intro to 170 frames past the finish and clears the moment a race is quit; the wave height
// at 0x806193BC is non-zero from the starting lights to the finish. So this is true from
// the lights (up to a second and a half into the countdown) to the chequered flag.
bool in_race() { return (int32_t)mem_r32(0x80602160u) > 0 && mem_r32(0x806193BCu) != 0; }
void apply_fps(int fps) {
    g_fps = fps;
    clock_set_cpu_scale(fps == 60 ? 2.0 : 1.0);
    step_set_scale(fps == 60 && !(g_keep & 8) ? 0.5 : 1.0);
    fprintf(stderr, "[pace] %d frames per second\n", fps);
}
// The pacing log's counters (WR_PACE_LOG, below wr_spray_gate).
uint32_t g_c_frames = 0, g_c_counter = 0, g_c_spray = 0, g_c_spray_forced = 0, g_c_water = 0;
uint64_t g_c_last_ns = 0;

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
    g_wanted = fps;
    fprintf(stderr, "[pace] %d frames per second wanted (in a race)\n", fps);
}
int fps() { return g_fps; }
int wanted_fps() { return g_wanted; }
void request_fps(int fps) { g_fps_request.store(fps, std::memory_order_relaxed); }
void toggle_fps() { request_fps(g_wanted == 60 ? 30 : 60); }
float frame_dt() { return 1.0f / (float)g_fps; }
}  // namespace wr

// Called once a frame from fn_80007A94, the frame's end, at the retrace wait: every frame
// the game presents passes here, the logo and title screens' as much as a race's. This is
// where the step scale learns that a frame has passed and where a change of rate is
// applied. It used to be the mode runner's advance of the frame counter (below), which
// the logo and title screens never reach: there the step clock stood still, so nothing was
// gated or halved and the whole title sequence ran at double speed, the title's water
// included, patches and tables notwithstanding (docs/dev/performance.md, "The title
// sequence").
//
// `game` is the game's own count: 2 in play (mode 5, the attract demo's 9) and 1 on the
// logo and title screens and during a course load (mode -1 and 0), as the pacing log below
// reads it. At 60 every frame is one field. For an afternoon on 2026-10-11 this returned
// the game's count for `game == 2`, taken for mode 0 from a reading of the branch in
// fn_80007A94 that was backwards, with the step override at 1 for those frames: in a race
// at 60 that paced at the game's own rate with the frame counter and the clock at full
// size over half-stepped physics, which showed on the headset as a crawling, flooded race
// with the frame rate gone. A course load that never finished at 60 was what that change
// was for; its cause was the spray drawer (wr_spray_gate), not the field count.
//
// 60 is the race's only (2026-10-11 night, docs/dev/performance.md "The race only"). The
// rate wanted is applied while in_race() holds and 30 is restored the frame it stops, so
// the title, the menus, the loads, the videos and the results run as the game runs them,
// whatever the thumbstick says, and only the race, the part the headset shows in stereo,
// is paced at 60.
extern "C" uint32_t wr_fields_per_frame(uint32_t game) {
    if (g_fps_at && gx::g_frames_submitted.load(std::memory_order_relaxed) >= g_fps_at) {
        g_fps_at = 0;
        wr::set_fps(60);
    }
    if (int r = g_fps_request.exchange(0, std::memory_order_relaxed)) wr::set_fps(r);
    if (const int want = in_race() ? g_wanted : 30; want != g_fps) apply_fps(want);
    step_frame();
    g_c_frames++;
    {
        const uint64_t now = host_ns();
        if (now - g_c_last_ns >= 1000000000ull) {
            static const bool on = getenv("WR_PACE_LOG") != nullptr
#ifdef __ANDROID__
                                   || true
#endif
                ;
            if (on && g_c_last_ns)
                fprintf(stderr, "[pace] %d fps (%d wanted): frame ends %u, counter steps %u, spray runs %u (forced %u), water runs %u, mode %d, scale %.2f%s%s\n",
                        g_fps, g_wanted, g_c_frames, g_c_counter, g_c_spray, g_c_spray_forced, g_c_water,
                        (int)mem_r32(0x80690FF4u), step_scale(), game == 2 ? ", game 2" : "", in_race() ? ", race" : "");
            g_c_last_ns = now;
            g_c_frames = g_c_counter = g_c_spray = g_c_spray_forced = g_c_water = 0;
        }
    }
    return g_fps == 60 && !(g_keep & 4) ? 0u : game;
}
extern "C" double wr_frame_dt(double game) { return g_fps == 60 && !(g_keep & 2) ? game * 0.5 : game; }
// Called once a game frame in play, from the mode runner's advance of the frame counter,
// with the counter's value before the advance. The counter steps like any other integer:
// every other frame at 60.
extern "C" uint32_t gcn_step_int(uint32_t pc, double p);
extern "C" uint32_t wr_frame_count_step(uint32_t frame) {
    watch_print(frame);
    const uint32_t step = g_fps == 60 && !(g_keep & 1) ? gcn_step_int(0x80006F14u, 1.0) : 1u;
    g_c_counter += step;
    return step;
}


// The riders' physics (docs/dev/performance.md, "The steering, seventh pass"): the riders'
// frame fn_80091C04 runs the whole of a player's physics -- the rider update fn_80091F5C
// with its forces, the water fn_800BE4E0, the damping fn_800BDFC0, the integrator
// fn_80093858 with its constraints -- in a loop of four passes a frame (the AI riders get
// one), each pass a complete step of a fixed size. At 60 the loop runs two passes of that
// size per paced frame with every step site inside it unscaled (gcn-recomp/docs/
// diagnostics.md, "Finding a game's per-frame steps"), so the hull's dynamics are the
// game's own; the control law and the frame's measurements, outside the loop, keep the
// table's scaling. The patches at 0x80091D4C and 0x80091DC4 set the count and the
// override, the one at 0x80091E00 (the first instruction after both loops) ends it.
extern "C" uint32_t wr_rider_passes(CPU*) {
    double s = step_scale();
    if (g_keep & 16) return 4u;
    step_override(1.0);
    return (uint32_t)llround(4.0 * s);
}
extern "C" void wr_rider_passes_end(CPU*) { step_override(NAN); }

// The spray (recomp/patches.txt, the spawner's gate). The spawner's update sets each
// rider's draw stride at 0x804EB080 (1, 2 or -1; a rider without particles keeps 0), and
// the drawer, which runs every frame, steps through the particles by it, so a frame that
// draws after a course reset cleared the table and before the spawner has run again loops
// forever. The gate may skip the spawner only while the table still holds what the
// spawner last left there: wr_spray_ran keeps a copy after each run, and a table that
// differs from it (cleared by a reset) runs the spawner whatever the frame. Earlier
// versions tested the entries themselves (all nonzero, then the player's nonzero) and
// were wrong twice: the AI riders' entries stay 0 through a race, and in a Championship
// the player need not be rider 0, so on the headset the spawner ran every frame, which
// doubled the spray and its cost (long streaks behind every ski, the frame rate gone).
namespace {
uint8_t g_spray_strides[16];
bool g_spray_ran = false;
}
// WR_PACE_LOG=1 (or the Android build, where stderr is logcat): a line a second with what ran
// -- frame ends, frame-counter steps taken, the spray spawner's runs (and how many of
// them the stride check forced), the water update's runs -- against the rate and the
// scale, for seeing from a headset log what the gates are doing.
extern "C" uint32_t wr_count_water() { g_c_water++; return 1u; }
// WR_WATER_FREEZE=1 (gcn_env.txt on the headset) or `water_freeze 1` in vr.txt: the
// water-and-effects update never runs, at either rate, so the sea stands still -- the
// wave field, the hull pushes, the keyframe-animated course objects and the wake and foam
// builders inside it all freeze. A test switch: with the surface fixed, whatever still
// changes on a switch of rate is not the water's own motion.
namespace { bool g_water_freeze = false; }
namespace wr {
void set_water_freeze(bool on) { g_water_freeze = on; if (on) fputs("[pace] water frozen", stderr), fputc(10, stderr); }
}
extern "C" uint32_t wr_water_frozen() {
    static const bool env = [] { const char* e = getenv("WR_WATER_FREEZE"); return e && atoi(e) != 0; }();
    return env || g_water_freeze ? 1u : 0u;
}
extern "C" uint32_t wr_spray_gate(uint32_t pc) {
    if (gcn_step_int(pc, 1.0)) { g_c_spray++; return 1u; }
    if (g_spray_ran && memcmp(mem_ptr(0x804EB080u), g_spray_strides, sizeof g_spray_strides) == 0) return 0u;
    g_c_spray++;
    g_c_spray_forced++;
    return 1u;
}
extern "C" void wr_spray_ran() {
    memcpy(g_spray_strides, mem_ptr(0x804EB080u), sizeof g_spray_strides);
    g_spray_ran = true;
}
