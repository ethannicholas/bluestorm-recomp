// The game's frame rate: 30, or the 60 of the experiment in pace.cpp.
#pragma once

namespace wr {
void set_fps(int fps);  // 30 or 60; anything else is ignored
int fps();
float frame_dt();       // seconds per game frame
}
