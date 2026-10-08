// What this game shows that the VR frontend keys off: the starting-light rig. See
// docs/dev/vr.md ("stereo that waits for the starting lights").
#pragma once
#include "gx/render.h"

namespace gx {
// Whether a frame draws the starting-light rig -- the gantry that descends in front of the
// camera at the start line, whose lights count the race in. It is what starts stereo: the
// one thing on screen that only a race's start shows.
bool batch_shows_start_rig(const Batch& b);
}
