// This game's answers to the headset frontend's questions (gcn-recomp/runtime/vr_game.h):
// how big its world is and where the HUD reads well, when to be in stereo, and how to put
// the eye on the ski. The frontends themselves are gcn-recomp/android/; everything that
// knows this is a racing game with a start line is here. See docs/dev/vr.md.
//
// When to be in stereo. The game's own state gates it, and what it draws starts it:
//
//   0x80602160  non-zero while the game is on a course: set as the course intro begins,
//               cleared when the race ends or the moment the player quits out of one. Zero
//               through boot, the menus, course select, loading, the results and the title.
//               It is a signed count of what the course loaded, not a flag -- field +0x20
//               of the descriptor at 0x80602140, a loop counter, or -1 from a failed load
//               -- and a build that insisted it was a flag aborted in someone's headset.
//               The only signal that clears when a race is *quit*, so it gates.
//   0x806193BC  the course's wave height: non-zero from the starting lights (up to 45
//               frames after the rig appears) until the race ends. Never reset on the quit
//               path, so it cannot end stereo alone, but it is what notices a race
//               *finishing*: the count above stays up another 170 frames, through the
//               results, which is too long to sit in stereo.
//   the rig     the starting-light rig on screen (gx::batch_shows_start_rig, start_rig.cpp)
//               is what brings stereo up with the lights. No guest variable found so far
//               means "the lights are up": the start sequence's phase at 0x80625A54 reads 5
//               on Dolphin Park's course-view screen too, and never moves in Time Attack.
//
// Stereo is on_course > 0 && (wave_height != 0 || rig on screen). The frontend holds a
// change in the answer for two frames before following it, which drops the single-frame
// transients a value written on the guest thread and read on the render thread shows.
#include "runtime.h"
#include "vr_game.h"
#include "first_person.h"
#include "pace.h"
#include "start_rig.h"
#include "gx/render_gl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kOnCourseAddr = 0x80602160;
constexpr uint32_t kRaceActiveAddr = 0x806193BC;

bool wants_stereo(const gx::Batch& b) {
    const int32_t on_course = (int32_t)mem_r32(kOnCourseAddr);
    if (on_course < 0) {
        static bool said = false;
        if (!said) {
            said = true;
            fprintf(stderr, "on-course count at %#x reads %d; treating as off-course\n", kOnCourseAddr, on_course);
        }
    }
    const uint32_t racing = mem_r32(kRaceActiveAddr);
    const bool rig = gx::batch_shows_start_rig(b);
    static const bool riglog = getenv("GCN_RIGLOG") != nullptr;
    if (riglog) gx::rig_log(b, rig);
    return on_course > 0 && (racing != 0 || rig);
}

// First person: the eye on the ski (first_person.cpp), from vr.txt's keys. The anchor is
// where the eye sits relative to the hull's origin, in game units in the ski's own frame --
// x to the right, y up, z forward -- and fp_up_m/fp_forward_m nudge it in metres, for
// finding by eye where it should be. The rider's eyes, standing at rest, are at 45 up and
// 9 back (his head's centre 43.5 and 11); the default is 25 cm above and 75 cm behind
// that, which is where it was put by eye in the headset: his own eyes put the viewer too
// low and too far forward to ride. The rest is how the eye follows the ski: fp_yaw_s and
// fp_tilt_s are the time constants, in seconds, over which its heading and its pitch and
// roll follow the hull's, fp_tilt how much of that pitch and roll it takes (1 rides with the
// hull, 0 keeps the horizon level, which felt like flying a camera rather than riding), and
// fp_height_s eases the height too -- but any lag lets the ski rise through the viewer on a
// crest, so it is 0.
void set_first_person(bool on, const VrConfig& c) {
    const float u = c.units_per_metre;
    const float anchor[3] = {c.get("fp_x", 0.0f), c.get("fp_y", 57.5f) + c.get("fp_up_m", 0.0f) * u,
                             c.get("fp_z", -46.5f) + c.get("fp_forward_m", 0.0f) * u};
    wr::first_person_configure(anchor, c.get("fp_height_s", 0.0f), c.get("fp_yaw_s", 0.1f), c.get("fp_tilt", 0.6f),
                               c.get("fp_tilt_s", 0.15f));
    gx::render_set_first_person(on);
}

// `fps 60` in vr.txt runs the game at 60 frames a second (pace.cpp); 30 is the game's own.
void config_loaded(const VrConfig& c) { wr::set_fps((int)c.get("fps", 30.0f)); }

void config_defaults(VrConfig& c) {
    // Game units to the metre. 100 was a guess and left the racers looking about half the
    // size they should; 50 is where it was judged from inside the headset, which is the
    // only place the question can be answered.
    c.units_per_metre = 50.0f;
    // The chase camera looks down at the ski at this angle; taking it back out of the
    // world in stereo puts the sea level with the room and leaves the viewer's own neck to
    // supply the downward look. With the world level the game's vertical is the room's, so
    // the HUD frame wants no tilt of its own, and a small negative height hangs it just
    // under the horizon -- 5.2 degrees under, where it was tuned to sit while wearing it.
    // Levelling off (world_pitch_deg 0) wants hud_pitch_deg -23.2 and hud_height_m 1.3.
    c.world_pitch_deg = 23.2f;
    c.hud_height_m = -0.36f;
    // The theater panel stays one image. As a stereo pair (theater_stereo) the water's
    // reflections went wrong: the game samples its reflection copy by screen position, and
    // the pair's second pass is drawn from a camera the copy was not made from. See
    // docs/dev/vr.md.
    c.theater_stereo = false;
    // The first-person eye's keys, so vr.txt can be read over them (see set_first_person).
    c.extra["fp_x"] = 0.0f;
    c.extra["fp_y"] = 57.5f;
    c.extra["fp_z"] = -46.5f;
    c.extra["fp_up_m"] = 0.0f;
    c.extra["fp_forward_m"] = 0.0f;
    c.extra["fp_height_s"] = 0.0f;
    c.extra["fp_yaw_s"] = 0.1f;
    c.extra["fp_tilt"] = 0.6f;
    c.extra["fp_tilt_s"] = 0.15f;
    c.extra["fps"] = (float)wr::fps();  // what WR_FPS set, unless vr.txt says otherwise
}

const bool installed = [] {
    vr::GameHooks h;
    h.config_defaults = config_defaults;
    h.config_loaded = config_loaded;
    h.wants_stereo = wants_stereo;
    h.set_first_person = set_first_person;
    // Left thumbstick click: 30 or 60 frames a second, for comparing the two in the headset.
    h.left_click = wr::toggle_fps;
    vr::set_game_hooks(h);
    return true;
}();

}  // namespace
