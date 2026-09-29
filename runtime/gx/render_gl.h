#pragma once
#include "render.h"

namespace gx {
void render_init(int internal_scale);
void render_set_window_size(int w, int h);
bool render_execute(Batch& b);
extern bool g_cull_swap;
extern const char* g_dump_dir;
extern int g_dump_every;
}  // namespace gx
