// The game's frame rate: 30, or the 60 of pace.cpp, which is applied in a race only.
#pragma once

namespace wr {
void set_fps(int fps);      // the rate wanted, 30 or 60 (anything else is ignored): in force in a race
void request_fps(int fps);  // the same, from another thread: applied at the next frame's end
void toggle_fps();          // 30 to 60 or back, likewise
int fps();                  // the rate in force now: the wanted one in a race, 30 elsewhere
int wanted_fps();           // the rate asked for
float frame_dt();           // seconds per game frame, at the rate in force
}
