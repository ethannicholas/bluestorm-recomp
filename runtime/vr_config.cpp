// See vr_config.h.
#include "vr_config.h"
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

VrConfig vr_config_load(const std::string& dir) {
    VrConfig c;
    const std::string path = dir + "/vr.txt";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        __android_log_print(ANDROID_LOG_INFO, "waverace",
                            "no %s, using VR defaults (units_per_metre %.1f)",
                            path.c_str(), c.units_per_metre);
        return c;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        double val;
        if (line[0] == '#' || sscanf(line, "%63s %lf", key, &val) != 2) continue;
        if (!strcmp(key, "units_per_metre")) c.units_per_metre = (float)val;
        else if (!strcmp(key, "offset_x")) c.offset_x = (float)val;
        else if (!strcmp(key, "offset_y")) c.offset_y = (float)val;
        else if (!strcmp(key, "offset_z")) c.offset_z = (float)val;
        else if (!strcmp(key, "near_m")) c.near_m = (float)val;
        else if (!strcmp(key, "far_m")) c.far_m = (float)val;
        else if (!strcmp(key, "hud_scale")) c.hud_scale = (float)val;
        else if (!strcmp(key, "hud_distance_m")) c.hud_distance_m = (float)val;
        else if (!strcmp(key, "hud_height_m")) c.hud_height_m = (float)val;
        else if (!strcmp(key, "hud_pitch_deg")) c.hud_pitch_deg = (float)val;
        else if (!strcmp(key, "world_pitch_deg")) c.world_pitch_deg = (float)val;
        else if (!strcmp(key, "theater_scale")) c.theater_scale = (int)val;
        else if (!strcmp(key, "stereo_scale")) c.stereo_scale = (int)val;
        else if (!strcmp(key, "eye_scale")) c.eye_scale = (float)val;
        else if (!strcmp(key, "msaa")) c.msaa = (int)val;
        else if (!strcmp(key, "log_frames")) c.log_frames = val != 0;
        else if (!strcmp(key, "dump_every")) c.dump_every = (int)val;
        else if (!strcmp(key, "start_in_stereo")) c.start_in_stereo = val != 0;
        else if (!strcmp(key, "first_person")) c.first_person = val != 0;
        else if (!strcmp(key, "fp_x")) c.fp_x = (float)val;
        else if (!strcmp(key, "fp_y")) c.fp_y = (float)val;
        else if (!strcmp(key, "fp_z")) c.fp_z = (float)val;
        else if (!strcmp(key, "transition_s")) c.transition_s = (float)val;
        else __android_log_print(ANDROID_LOG_INFO, "waverace", "vr.txt: ignoring '%s'", key);
    }
    fclose(f);
    __android_log_print(ANDROID_LOG_INFO, "waverace",
                        "vr.txt: units_per_metre=%.1f offset=(%.1f,%.1f,%.1f) "
                        "hud=%.3f@%.1fm height=%.2fm pitch=%.1fdeg world_pitch=%.1fdeg "
                        "scale=%d/%d eyes=%.2fx msaa=%d stereo_at_start=%d transition=%.2fs "
                        "first_person=%d at (%.1f,%.1f,%.1f)",
                        c.units_per_metre, c.offset_x, c.offset_y, c.offset_z, c.hud_scale,
                        c.hud_distance_m, c.hud_height_m, c.hud_pitch_deg, c.world_pitch_deg,
                        c.theater_scale, c.stereo_scale, c.eye_scale, c.msaa, (int)c.start_in_stereo,
                        c.transition_s, (int)c.first_person, c.fp_x, c.fp_y, c.fp_z);
    return c;
}
