# Working notes for this repository

This is a static recompilation of *Wave Race: Blue Storm* (GameCube), with a VR port for the
Quest 3. The recompiler, runtime, renderer and desktop frontend are the `gcn-recomp` submodule
(canonical checkout at `~/Source/gcn-recomp`), shared with `~/Source/prime-recomp`, where the
code was first written for this game. Fix shared things there: commit in the submodule, then
commit the new pointer here (and in prime-recomp). Only what is specific to this game lives in
this repository: `analysis/`, the `recomp/*.txt` tables, the game's answers to the headset
frontends in `runtime/` (`vr_waverace.cpp`, the `vr::GameHooks`; `start_rig.cpp`, what starts
stereo; `first_person.cpp`, the renderer's eye hook), the two Android build scripts, and
`docs/dev/`. The headset frontends themselves (`gcn-recomp/android/`) are shared since
2026-10-10; a decision that is this game's goes in a hook, not in them.

**The repository must contain no game code or data.** Never commit, quote at length, or paste
disassembly of the game into tracked files; a function name and address is fine, its
instructions are not.

`README.md` is the public landing page. It holds only what someone needs to build and play the
game on macOS, Windows or a Quest. Keep it that way: no investigation records, measurements,
debugging environment variables or design rationale go there.

Those live in `docs/dev/`, and that is where to record anything non-obvious you learn, in the
file for its area:

- `docs/dev/performance.md` — open issues, what has been profiled and fixed, what is left, and
  the measurements that were wrong before.
- `docs/dev/diagnostics.md` — the route to Ocean City Harbor, recording a run on the headset
  and replaying it, the Quest benchmark and the headless `waverace_egl` harness, `gcn_env.txt`,
  and what the guest heap holds. The shared tooling (`GCN_*` variables, input logging and
  replay, the benchmark, panics, heap tracing) is documented in
  `gcn-recomp/docs/diagnostics.md`.
- `docs/dev/graphics.md` — GL profile requirements, what GL ES cannot do, the shader cache.
- `docs/dev/audio.md` — the mix, AAudio on the headset, and how to test audio over adb.
- `docs/dev/vr.md` — the OpenXR app, the theater/stereo switch and the guest addresses behind
  it, splitting a frame for the eyes, the HUD frame, levelling the sea, tracing the compositor,
  and the one-launch-per-process rule. Read the relevant section before touching
  `runtime/vr_waverace.cpp`, `gcn-recomp/android/openxr_main.cpp`, `gcn-recomp/android/egl_main.cpp`
  or the eye paths in `gcn-recomp/runtime/gx/render_gl.cpp`.

Several of those sections record an answer that turned out to be wrong and why. Keep that habit:
when a belief is overturned, say what the belief was and what disproved it, so the next attempt
does not start from nothing.
