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

// Frames presented so far, for tracing what a display frame is actually showing.
uint32_t present_count();

// ---- VR ----
// Set the eye projection and the eye's transform relative to the game's camera, plus
// how much of the field of view the flat 2D elements are shrunk into.
void render_set_vr_eye(const float proj[16], const float view[16], float hud_scale);

// Draw one eye's view of a batch into `fbo`. Both eyes share the vertex buffer, the
// CPU-side transform and any render-to-texture results, so only uniforms and draw
// calls are repeated. Pass do_copies for the first eye only.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies);
extern bool g_cull_swap;
extern const char* g_dump_dir;
extern int g_dump_every;
}  // namespace gx
