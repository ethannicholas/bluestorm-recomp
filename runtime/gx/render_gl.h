#pragma once
#include "render.h"

namespace gx {
// Upper bound on the EFB's supersampling, so that a mistyped digit in vr.txt asks for a
// framebuffer that is merely large rather than one the device cannot allocate at all. 8
// is 5120x4224, past anything either view has a use for.
constexpr int kMaxInternalScale = 8;

void render_init(int internal_scale);

// Re-scale the EFB between frames. Call only when a whole frame is about to be drawn:
// it discards the EFB's contents and every texture an EFB copy has produced.
void render_set_internal_scale(int scale);

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

// Rotate the world back by the game's chase-camera pitch, so the sea comes out level with
// the room instead of sloping. Applies to world geometry only: the HUD frame is placed in
// the headset's own space and stays where it is put. 0 renders what the game draws.
void render_set_world_pitch(float pitch_rad);

// Draw one eye's view of a batch into `fbo`. Both eyes share the vertex buffer, the
// CPU-side transform and any render-to-texture results, so only uniforms and draw
// calls are repeated. Pass do_copies for the first eye only.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies);
extern bool g_cull_swap;
extern const char* g_dump_dir;
extern int g_dump_every;
}  // namespace gx
