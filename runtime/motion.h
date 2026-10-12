// Motion telemetry for this game: the first-person eye's motion, sent to a motion platform
// through gcn-recomp's telemetry (runtime/telemetry.h). See motion.cpp.
#pragma once

namespace wr {

// Game units to the metre for the headset: how big the world looks (vr_waverace.cpp). The
// telemetry has a metre of its own, the game physics' (motion.cpp).
constexpr float kUnitsPerMetre = 50.0f;

// Whether anything is listening: the eye has to be followed through every frame if so.
bool motion_enabled();

// The first-person eye after this game frame's step (first_person.cpp): where it is, in the
// world's own frame and game units, and its eased heading, pitch and roll in radians, the
// pitch and roll already scaled the way the eye takes them; and the hull's own heading,
// pitch and roll this frame, neither eased nor scaled, for the `raw` blend. `valid` is false
// while there is no ski on screen.
struct EyePose {
    bool valid;
    float pos[3];
    float yaw, pitch, roll;
    float raw_yaw, raw_pitch, raw_roll;
};
void motion_frame(const EyePose& eye);

}  // namespace wr
