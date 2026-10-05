#pragma once
#include "render.h"

namespace gx {
void render_init(int internal_scale);
void render_set_window_size(int w, int h);

// Redirect the finished frame somewhere other than the window framebuffer, e.g. a VR
// swapchain image. 0 restores the default.
void render_set_output_fbo(unsigned fbo);

// Re-blit the most recent frame to the current output. A VR compositor needs an image
// every display frame, far more often than this game produces one. False if nothing
// has been presented yet.
bool render_repaint();
bool render_execute(Batch& b);
extern bool g_cull_swap;
extern const char* g_dump_dir;
extern int g_dump_every;
}  // namespace gx
