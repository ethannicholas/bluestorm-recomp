// The game's frame rate, and the experiment of running it at 60 (WR_FPS=60, or `fps 60`
// in vr.txt). See docs/dev/performance.md, "60 frames per second".
//
// The game paces itself on the retrace. fn_80007A94 ends a frame: it spins until at least
// N retraces have passed since the mark it took at the end of the last frame (N is 1 in
// play, 2 in mode 0), takes a new mark, hands the video interface the next frame buffer
// and waits for one more retrace. So a frame that fits in a field still takes two, and
// the game is 30 frames a second by construction; with N = 0 the spin is skipped and a
// frame that fits in a field takes one. The patches in recomp/patches.txt at 0x80007B3C
// and 0x80007B44 route N through wr_fields_per_frame.
//
// The game keeps a clock in seconds at 0x806919F0, advanced once a frame by the constant
// at 0x80692400 (1/30) in the mode runner fn_80006D20 (0x80006F28), and a frame counter at
// 0x80690FF8 (0x80006F14), both read all over the game's logic. At 60 the clock advances
// by half the constant, so that anything timed by it keeps its pace (wr_frame_dt). The
// counter has to go on advancing every frame: advanced every other frame the starting
// lights never finish counting down (performance.md), so it is the frame's identity to
// the game, not a timer, and wr_frame_count_step leaves it alone unless asked. What none
// of this touches is anything the game steps per frame with a constant of its own, which
// is what the replay comparison in performance.md is for finding.
//
// The virtual clock charges a frame's work two fields (gcn-recomp/docs/diagnostics.md,
// "The clock"), so at 60 the guest also has to be a CPU twice as fast: clock_set_cpu_scale.
#include "runtime.h"
#include "pace.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int g_fps = 30;
// WR_PACE_KEEP=<bits>, for finding what a 60 fps run trips over: 1 advances the frame
// counter every other frame (so it counts the game's own seconds; the start sequence then
// never finishes, see performance.md), 2 keeps the clock's full 1/30 step, 4 keeps the
// two-field pacing (then only the CPU scale is in effect).
int g_keep = 0;

const bool installed = [] {
    if (const char* e = getenv("WR_PACE_KEEP")) g_keep = atoi(e);
    if (const char* e = getenv("WR_FPS")) wr::set_fps(atoi(e));
    return true;
}();
}  // namespace

namespace wr {
void set_fps(int fps) {
    if (fps != 30 && fps != 60) return;
    g_fps = fps;
    clock_set_cpu_scale(fps == 60 ? 2.0 : 1.0);
    fprintf(stderr, "[pace] %d frames per second\n", fps);
}
int fps() { return g_fps; }
float frame_dt() { return 1.0f / (float)g_fps; }
}  // namespace wr

extern "C" uint32_t wr_fields_per_frame(uint32_t game) { return g_fps == 60 && !(g_keep & 4) ? 0u : game; }
extern "C" double wr_frame_dt(double game) { return g_fps == 60 && !(g_keep & 2) ? game * 0.5 : game; }
extern "C" uint32_t wr_frame_count_step() {
    static uint32_t calls;
    return g_fps == 60 && (g_keep & 1) ? (calls++ & 1u) : 1u;
}
