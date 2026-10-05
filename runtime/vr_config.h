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
    // head movement, so it sets the apparent size of the world.
    float units_per_metre = 100.0f;

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

    // Fraction of the field of view the flat 2D elements (the HUD) are shrunk into
    // when rendering in stereo.
    float hud_scale = 0.55f;

    // Trace the theater path one display frame at a time (adb logcat -s waverace).
    bool log_frames = false;

    // Start in stereo rather than theater, for testing.
    bool start_in_stereo = false;
};

// Reads `dir`/vr.txt. Missing file or missing keys keep the defaults above.
VrConfig vr_config_load(const std::string& dir);
