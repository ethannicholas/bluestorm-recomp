// Motion telemetry: the first-person eye's motion, for a motion platform.
//
// The platform is driven by the eye on the ski (first_person.cpp) rather than by the hull
// itself, in every view and not only in first person, because the eye is already what
// riding the ski feels like: its pitch and roll follow the hull's eased over fp_tilt_s and
// scaled by fp_tilt, so the 30 Hz slap of each wave is taken out and the swell comes through.
// The rig tilts as the eye does.
//
// Its position follows the hull exactly, so the accelerations are taken from it with
// smoothing of their own: velocity is differenced from position each game frame and eased
// over `ski_velocity_s`, and acceleration differenced from that and eased over `ski_accel_s`
// (telemetry.txt; 0.05 s and 0.15 s by default, the second the eye's own fp_tilt_s). Both
// are then turned into the ski's heading: surge along it, sway across it, heave up the
// world's vertical.
//
// That is the motion as the eye has it, and on a rig it is on the tame side. `raw` (0 to 1,
// 1 by default) blends the eye's angles toward the hull's own: its pitch and roll at full size rather than
// fp_tilt's, and unsmoothed, and its heading. A dive off a ramp pitches the hull 30 degrees
// in a tenth of a second, and the eye takes the edge off that.
//
// The accelerations are not blended the same way. Differenced frame to frame with no easing
// they are the hull bouncing on the chop, 100-200 m/s^2 back and forth ten or twenty times
// a second at speed, far faster than a platform moves; sent, they held the old rate limit
// four frames in five and buried everything else. Heave is the worst of it, so it is eased
// again over `heave_s` (0.15 s) on top of `ski_accel_s`. What was buried is impacts: a
// collision is 200-350 m/s^2 for a few frames after the velocity's own easing, a landing
// 150-220, well clear of the chop's 20-40. So an impact is that fast acceleration's excess
// over the eased one beyond `impact_threshold` (50 m/s^2), scaled by `impact` (0.25), held
// at its peak and let go over `impact_s` (0.2 s): a knock, in the right direction, long
// enough for a platform to make. Whatever the motion, it goes through the same safeguards
// (telemetry.cpp): the ease to rest, the crossfades and the range and rate limits.
//
// A difference across a discontinuity is a spike, and a spike is a blow to whoever is on the
// platform, so the track is careful about what it differences:
//
// - A pause freezes the ski where it is. Differenced, a ski at 90 km/h stopping dead in one
//   frame is some 700 m/s^2 of deceleration, and unpausing the same forwards. So a ski that
//   has not moved at all for kPauseFrames frames is paused: the platform eases to rest (the
//   frame is sent inactive) and the velocity is kept, not decayed, so that it picks up where
//   it left off. Nothing on water is still to the last bit for that long otherwise.
// - Shorter stills are a game that moves the ski on some frames and not others: the next
//   movement is divided by the frames it took, not by one.
// - When the ski is first found, or found again after being lost, or after a respawn moves it
//   too far in a frame, nothing is known of its velocity: the first frame only sets the
//   position, the second sets the velocity outright, and only from the third is an
//   acceleration differenced, easing up from zero. The first two are sent marked as a
//   discontinuity, which the shared module fades across.
#include "motion.h"
#include "pace.h"
#include "telemetry.h"
#include <cmath>

namespace wr {
namespace {

// Game units to the metre as the game's own physics has it, which is not the headset's
// kUnitsPerMetre: that was set by eye for how big the world looks, while this is what makes
// the speed here agree with the speedometer. On the scripted Ocean City Harbor run the HUD
// read 91 and 98 km/h where 50 units to the metre gave 73 and 80, a ratio of 1.23-1.25
// both times, so the game's metre is about 40 units.
constexpr float kPhysicsUnitsPerMetre = 40.0f;

struct Track {
    int known = 0;         // 0 nothing, 1 the position, 2 the velocity too, 3 everything
    int still = 0;         // frames in a row the ski has not moved
    float pos[3];
    float vel[3];   // m/s, eased
    float acc[3];   // m/s^2, eased
    float heave;    // m/s^2, acc[1] eased again
    float tracked;  // seconds of accelerations since the track restarted
    float knock[3]; // m/s^2, the impact being let go
};
Track g_track;
constexpr int kPauseFrames = 4;
constexpr float kImpactAfter = 0.5f;   // seconds; ski_accel_s and heave_s are 0.15 each

}  // namespace

bool motion_enabled() { return telemetry::enabled(); }

void motion_frame(const EyePose& eye) {
    if (!telemetry::enabled()) return;
    telemetry::Motion m;
    Track& t = g_track;
    if (!eye.valid) {
        t.known = 0;
        telemetry::publish(m);   // inactive: the platform settles
        return;
    }
    const float dt = frame_dt();
    const float u = kPhysicsUnitsPerMetre;
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = eye.pos[i] - t.pos[i];
    // A respawn moves the ski a long way in one frame, as the eye's own easing allows for.
    const bool jump = d[0] * d[0] + d[2] * d[2] > 300.0f * 300.0f;
    const bool moved = d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f;
    if (t.known == 0 || jump) {
        for (int i = 0; i < 3; i++) {
            t.pos[i] = eye.pos[i];
            t.vel[i] = t.acc[i] = t.knock[i] = 0.0f;
        }
        t.heave = t.tracked = 0.0f;
        t.known = 1;
        t.still = 0;
        m.discontinuity = true;
    } else if (!moved) {
        // Paused, or a frame the game did not move the ski on: keep everything as it was.
        t.still++;
    } else {
        const float span = dt * (float)(t.still + 1);
        t.still = 0;
        if (t.known == 1) {
            for (int i = 0; i < 3; i++) t.vel[i] = d[i] / u / span;
            t.known = 2;
            m.discontinuity = true;
        } else {
            static const float vel_s = telemetry::get("ski_velocity_s", 0.05f);
            static const float acc_s = telemetry::get("ski_accel_s", 0.15f);
            static const float heave_s = telemetry::get("heave_s", 0.15f);
            static const float impact = telemetry::get("impact", 0.25f);
            static const float impact_threshold = telemetry::get("impact_threshold", 50.0f);
            static const float impact_s = telemetry::get("impact_s", 0.2f);
            auto rate = [span](float tau) { return tau > 0.0f ? 1.0f - expf(-span / tau) : 1.0f; };
            for (int i = 0; i < 3; i++) {
                const float v = d[i] / u / span;
                const float prev = t.vel[i];
                t.vel[i] += (v - t.vel[i]) * rate(vel_s);
                const float a = (t.vel[i] - prev) / span;   // the fast acceleration
                t.acc[i] += (a - t.acc[i]) * rate(acc_s);
                if (i == 1) t.heave += (t.acc[1] - t.heave) * rate(heave_s);
                // Only once the eased one has had time to follow: just after a restart it is
                // still easing up from zero, and the chop would read as a knock.
                const float excess = t.tracked >= kImpactAfter ? a - (i == 1 ? t.heave : t.acc[i]) : 0.0f;
                const float k = copysignf(fmaxf(0.0f, fabsf(excess) - impact_threshold), excess) * impact;
                if (fabsf(k) > fabsf(t.knock[i])) t.knock[i] = k;
                else t.knock[i] -= t.knock[i] * rate(impact_s);
            }
            t.known = 3;
            t.tracked += span;
        }
        for (int i = 0; i < 3; i++) t.pos[i] = eye.pos[i];
    }
    if (t.still >= kPauseFrames) {
        telemetry::publish(m);   // paused: the platform settles, and comes back on resuming
        return;
    }

    // The ski's heading in the world, whose up is Y: forward, and right = forward x up.
    const float sy = sinf(eye.yaw), cy = cosf(eye.yaw);
    const float fwd[3] = {sy, 0.0f, cy};
    const float right[3] = {-cy, 0.0f, sy};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    static const float raw_ang = telemetry::get("raw", 1.0f);
    auto mix = [](float eased, float r, float k) { return eased + (r - eased) * k; };
    float dyaw = eye.raw_yaw - eye.yaw;   // across the wrap the short way
    while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
    while (dyaw < -3.14159265f) dyaw += 6.2831853f;
    float acc[3];
    for (int i = 0; i < 3; i++) acc[i] = (i == 1 ? t.heave : t.acc[i]) + t.knock[i];
    constexpr float kDeg = 57.2957795f;
    m.active = true;
    m.speed_kmh = sqrtf(t.vel[0] * t.vel[0] + t.vel[2] * t.vel[2]) * 3.6f;
    m.pitch = mix(eye.pitch, eye.raw_pitch, raw_ang) * kDeg;
    m.roll = -mix(eye.roll, eye.raw_roll, raw_ang) * kDeg;   // the eye's roll is positive with the right side rising
    m.yaw = -(eye.yaw + dyaw * raw_ang) * kDeg;              // turning right lowers the eye's yaw; a heading rises
    m.surge = dot(acc, fwd);
    m.sway = dot(acc, right);
    m.heave = acc[1];
    m.sway_velocity = dot(t.vel, right);
    telemetry::publish(m);
}

}  // namespace wr
