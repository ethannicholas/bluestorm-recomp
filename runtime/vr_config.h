// Runtime-tunable VR parameters.
//
// These exist as a file rather than constants because the right values cannot be known
// from the source: `a_pos` reaches the renderer in the game's own units, and nothing
// says how many of those make a metre. Getting that wrong makes the world giant or
// doll-sized, and it is only judgeable by wearing the headset. So they are read from
//   /sdcard/Android/data/<package>/files/vr.txt
// and can be changed with `adb push` between runs, with no rebuild.
//
// Format is one `key value` per line; blank lines and lines starting with # ignored.
#pragma once
#include <string>

struct VrConfig {
    // Game units per real-world metre. Scales both the interpupillary distance and
    // head movement, so it sets the apparent size of the world: raising it makes the
    // viewer bigger and the world smaller. 100 was a guess and left the racers looking
    // about half the size they should; 50 is where it was judged from inside the headset,
    // which is the only place the question can be answered.
    float units_per_metre = 50.0f;

    // Where the viewpoint sits relative to the game's camera, in game units:
    // x right, y up, z back. The game's own chase camera is already over the
    // shoulder, so this nudges it rather than replacing it.
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;

    // Near and far planes, in metres. Their ratio sets depth precision, so widen only
    // as far as the scene needs: too large a range shows up as z-fighting.
    float near_m = 0.1f;
    float far_m = 2000.0f;

    // Where the frame the HUD is painted on hangs, and how big it is. The distance is
    // what makes the HUD fuse: it gives the two eyes one thing at one depth to agree on,
    // which a fixed position in each eye's own NDC never does. The rest is comfort, and
    // comfort is only judgeable by wearing the headset -- hence four knobs rather than
    // four constants.
    //
    //   scale       fraction of the vertical field of view the frame fills
    //   distance    how far ahead of the game's camera it stands
    //   height      how far above the forward axis its centre sits, in metres
    //   pitch       tilt about the frame's horizontal axis. 0 stands the frame vertical
    //               in the *room*; negative leans its top towards the viewer.
    //
    // These go with world_pitch_deg below, which levels the sea. With the world level,
    // the game world's vertical is the room's, so the frame wants no tilt of its own, and
    // the forward axis is the horizon, so a small negative height hangs the HUD just under
    // it -- 5.2 degrees under, which is where it was tuned to sit while wearing it.
    //
    // Turning the levelling off (world_pitch_deg 0) puts the tilt back in the world, and
    // the HUD has to lean to match it again: hud_pitch_deg -23.2 and hud_height_m 1.3.
    float hud_scale = 0.5f;
    float hud_distance_m = 4.0f;
    float hud_height_m = -0.36f;
    float hud_pitch_deg = 0.0f;

    // Degrees of the game's chase-camera pitch to take back out of the world in stereo.
    // Vertices arrive in the camera's own frame and are handed to the headset as though
    // that frame were gravity-aligned, so the sea is rendered as a slope of this angle and
    // the horizon rides high. Rotating the world back by it puts the sea level with the
    // room and leaves the viewer's own neck to supply the downward look -- at the cost of
    // the ski sitting well below the forward axis, since the camera was aimed down at it.
    //
    // 0 renders what the game draws, tilt and all. Whether the honest horizon or the
    // familiar framing is the better trade is a question for the headset, which is why
    // this is a knob rather than a constant. Note it moves the world and not the HUD, so
    // levelling the world wants hud_pitch_deg back at 0.
    float world_pitch_deg = 23.2f;

    // How many samples the EFB keeps per hardware pixel, in each axis, in each view.
    // 1 is the GameCube's own 640x528, which is what the theater panel used to be
    // stretched from and why leaving a race looked as soft as it did.
    //
    // The two views are separate because the EFB means different things in each. In
    // theater it *is* the picture. In stereo the eyes are drawn at the headset's own
    // resolution and the EFB is only scratch space for what they sample out of it --
    // the water reflection, the sheet the spray is cut from -- filled by a third full
    // scene pass that already costs more than it returns; leave that at 1 unless the
    // reflections look coarse.
    //
    // 3 draws 1920x1584 behind a panel 1260 texels across, so the extra samples are
    // filtered down into it rather than thrown away: antialiasing, not just detail. It
    // is close to free -- this path is bound by the recompiled guest code, and sixteen
    // times the pixels measured within noise of one -- but the measurement is a 150 s
    // run, which says nothing about a headset that has been warm for an hour, so there
    // is a step left in hand. Capped at gx::kMaxInternalScale.
    int theater_scale = 3;
    int stereo_scale = 1;

    // Trace the theater path one display frame at a time (adb logcat -s waverace).
    bool log_frames = false;


    // Write the EFB to <files>/frames every N presented frames. The headless harness can
    // already dump, but it is a different frontend: when the two disagree about the same
    // scene on the same device, the only way to see what the app itself rendered is to
    // have the app dump it. 0 is off.
    int dump_every = 0;


    // Start in stereo rather than theater, for testing.
    bool start_in_stereo = false;
};

// Reads `dir`/vr.txt. Missing file or missing keys keep the defaults above.
VrConfig vr_config_load(const std::string& dir);
