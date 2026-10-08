// First person on the ski: this game's half of the renderer's eye hook. See
// first_person.cpp, and docs/dev/vr.md for how the eye follows the hull.
#pragma once

namespace wr {
// Where the eye sits on the hull -- x right, y up, z forward, in game units from the
// hull's origin -- and how it follows the hull: time constants, in seconds, for its
// height, heading and pitch/roll (0 follows exactly), and how much of the hull's pitch and
// roll it takes (1 all of it, 0 a level horizon). Switching first person on and off is
// gx::render_set_first_person.
void first_person_configure(const float anchor[3], float height_s, float yaw_s, float tilt, float tilt_s);
}
