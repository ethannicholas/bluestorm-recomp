# Working notes for this repository

`README.md` is the public landing page. It holds only what someone needs to build and play the
game on macOS, Windows or a Quest. Keep it that way: no investigation records, measurements,
debugging environment variables or design rationale go there.

Those live in `docs/dev/`, and that is where to record anything non-obvious you learn, in the
file for its area:

- `docs/dev/performance.md` — open issues, what has been profiled and fixed, what is left, and
  the measurements that were wrong before.
- `docs/dev/diagnostics.md` — `WR_*` environment variables, scripted input and the route to
  Ocean City Harbor, the benchmark and the headless `waverace_egl` harness, the guest call
  stack, and how `OSPanic` is reported.
- `docs/dev/graphics.md` — GL profile requirements, what GL ES cannot do, the shader cache.
- `docs/dev/audio.md` — the mix, AAudio on the headset, and how to test audio over adb.
- `docs/dev/vr.md` — the OpenXR app, the theater/stereo switch and the guest addresses behind
  it, splitting a frame for the eyes, the HUD frame, levelling the sea, tracing the compositor,
  and the one-launch-per-process rule. Read the relevant section before touching
  `runtime/openxr_main.cpp`, `runtime/egl_main.cpp` or the eye paths in `runtime/gx/render_gl.cpp`.

Several of those sections record an answer that turned out to be wrong and why. Keep that habit:
when a belief is overturned, say what the belief was and what disproved it, so the next attempt
does not start from nothing.
