// The game's frame rate: 30, or the 60 of the experiment in pace.cpp.
#pragma once

namespace wr {
void set_fps(int fps);  // 30 or 60; anything else is ignored
void request_fps(int fps);  // the same, from another thread: applied at the next frame step
void toggle_fps();          // 30 to 60 or back, likewise
int fps();
float frame_dt();       // seconds per game frame
}
