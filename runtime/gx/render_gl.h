#pragma once
#include "render.h"

namespace gx {
void render_init(int internal_scale);
void render_set_window_size(int w, int h);

// Redirect the finished frame somewhere other than the window framebuffer, e.g. a VR
// swapchain image. 0 restores the default.
void render_set_output_fbo(unsigned fbo);

bool render_execute(Batch& b);

// Frames presented so far, for tracing what a display frame is actually showing.
uint32_t present_count();


// ---- VR ----
// Set the eye projection, the eye's transform relative to the game's camera, and the
// frame the flat 2D elements are painted on (render_hud_frame).
void render_set_vr_eye(const float proj[16], const float view[16], const float hud[16]);

// Builds that frame: a quad `dist` game units in front of the game's camera, `scale` of
// the vertical field of view tall, its centre `height` game units above the forward axis,
// tilted back by `pitch_rad` (0 standing vertical). `tan_half_fovy` is tan of half that
// field. Both eyes must be given the same frame, or there is nothing for them to fuse
// into.
void render_hud_frame(float dist, float tan_half_fovy, float scale, float height,
                      float pitch_rad, float out[16]);

// Draw one eye's view of a batch into `fbo`. Both eyes share the vertex buffer, the
// CPU-side transform and any render-to-texture results, so only uniforms and draw
// calls are repeated. Pass do_copies for the first eye only.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies);
extern bool g_cull_swap;
extern const char* g_dump_dir;
extern int g_dump_every;
}  // namespace gx
