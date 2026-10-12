// The game's frame rate: the 60 of pace.cpp in a race (the headset's default, `fps 30` in
// vr.txt for the game's own), 30 everywhere else.
#pragma once

namespace wr {
void set_fps(int fps);      // the rate wanted, 30 or 60 (anything else is ignored): in force in a race
int fps();                  // the rate in force now: the wanted one in a race, 30 elsewhere
int wanted_fps();           // the rate asked for
float frame_dt();           // seconds per game frame, at the rate in force
void set_water_freeze(bool on);  // `water_freeze 1` in vr.txt: the sea stands still (a test switch)
}
