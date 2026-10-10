# Performance: open issues and measurements

Working notes on where the frame rate stands, what has been fixed, and what is left. The
measurement tools these refer to (`GCN_FRAMETIME`, `GCN_TIMESCALE`, the scripted route to Ocean
City Harbor) are described in [diagnostics.md](diagnostics.md).

## Open issues

In rough priority order. Everything here reproduces; where there is a lead it is written down so
the next attempt does not start from nothing.

1. **Performance.** Ocean City Harbor did not hold 30 fps. Dropping the per-display-frame repaint
   bought about 15%. It is **not** the pixel pipeline: raising the EFB from 640×528 to 2560×2112,
   sixteen times the pixels, costs about 2% and nothing beyond that scales with area — see
   [How many pixels the theater panel gets](vr.md#how-many-pixels-the-theater-panel-gets). It is CPU
   work, on both threads, and `GCN_FRAMETIME=1` says which.

   Done so far, profiled on a Mac with the scripted route in [diagnostics.md](diagnostics.md): the brief drop to
   single digits at a fixed spot was the render thread compiling a burst of new TEV shaders on
   first use, now built at startup from a cache (see [Shader cache](graphics.md#shader-cache)); the GX
   front end on the guest thread went from 5.5 to 3.1 ms a frame (a pixel-state snapshot per
   draw command that was identical 12 times in 13, vertices repeated to make triangle lists
   rather than indexed, a batch reallocated from nothing every frame, a serial texture hash);
   and the render thread from 4.9 to 3.2 ms (a full state re-application per draw where 94% of
   them change one texture). The guest ceiling in the race is now ~115 fps on an M-series Mac,
   8.7 ms a frame, two thirds of it the recompiled game code — which `-O3` does not help.

   What is left, in order of likely value on the headset: the stereo path issues every draw
   three times (one flat pass into the EFB, then one per eye); the flat pass's main-scene draws
   feed only copies the eyes substitute, except the partial copy the submerged tint samples,
   which is why they cannot simply be skipped. A thousand draws per pass is a thousand driver
   calls, and the remaining per-vertex work (a matrix multiply, the texgens, a 116-byte vertex)
   only moves off the guest thread by doing the transform in the vertex shader.

## A note on measuring performance here

An earlier revision of this file claimed a race ran at 0.5-3 fps on a Quest 3 and blamed
draw-call overhead. **That was wrong**, and the way it was wrong is worth recording.

The measurement was taken with `waverace_egl` before the batch queue was bounded. The queue was
growing by tens of megabytes a second, the process reached 3.8 GB resident and was swapping hard,
and the frame rate being measured was the frame rate of a thrashing process. Bounding the queue
fixed the frame rate as a side effect, and the race is playable in the headset.

Two lessons for anyone measuring this again: check resident memory before trusting a frame rate,
and prefer measuring in the app you actually ship over a headless harness.

A second leak had the same shape and the same symptom — a grey compositor and an unresponsive
headset part-way into a race. Every EFB copy was given a fresh texture id, so the renderer kept
one GL texture per copy per frame forever. A copy to an address it already holds now keeps its id
and re-renders into the texture it has. That is not enough on its own: at speed the spray copies
out around fifty sprites a frame to addresses that rotate, so both caches evict EFB entries too,
the renderer waiting four times as long as the guest so an address is always forgotten guest-side
first and a draw can never reach an id whose texture has gone. Resident memory over two and a
half minutes of racing at speed now oscillates between 145 and 274 MB with no upward trend.

The draw-call shape is still worth knowing: the game issues some 13,000 GX draw commands per race
frame, averaging five vertices each, which the front end merges into ~1,000 draws with ~1,000
distinct pixel states between them. Measured over a million consecutive pairs of those states,
94% differ in a texture and almost nothing else -- projection, viewport, scissor, fog and the
indirect matrices are the same more than 99% of the time, the program 81%. So `apply_state`
shadows what it last put into GL and issues only the difference, which makes the typical state
change a texture bind and its size rather than forty calls. (Deduplicating `PixelState` by
content, suggested here earlier, would not have helped: consecutive states really are different,
just barely.) But it is an optimisation, not the explanation for a slideshow that was really a
memory leak.

## 60 frames per second (2026-10-10)

The question was whether the game, which is 30 fps by construction, could be run at 60 on the
Quest, where the guest has the headroom: `waverace_bench` unpaced through a race on the Quest 3
runs at 75 fps (`[clock]` work 675k back-edges a frame, 2.5x the game's rate). The answer is
that the pacing is easy and the simulation is not: the game steps everything per frame, so at 60
it runs at double speed.

**How the game paces itself.** `fn_80007A94` ends a frame: it spins until at least N retraces
have passed since the mark it took at the end of the last frame (`VIGetRetraceCount` is
`fn_80116398`; N is 1 in play, 2 in mode 0), takes a new mark, hands the VI the next frame
buffer (`fn_8011631C`, `fn_801162B0`) and waits for one more retrace (`VIWaitForRetrace` is
`fn_80115634`). So a frame that fits in a field still takes two, and one that does not takes
three. The mode runner `fn_80006D20` then advances the game's clock in seconds at `0x806919F0`
by the 1/30 at `0x80692400` and a frame counter at `0x80690FF8` by one. The counter is the
frame's identity, not a timer: the course flyover renders at 60 while advancing it every other
frame (`GCN_CLOCKLOG` shows +60 frames a virtual second there), so the game already has a
render-twice-per-logic-frame path, for that one scene.

**The switch.** `WR_FPS=60` (or `fps 60` in `vr.txt`; `runtime/pace.cpp`, the patches at the
end of `recomp/patches.txt`) makes N zero, halves the clock's step, and asks the runtime for a
CPU twice as fast (`clock_set_cpu_scale`, `GCN_CPU_SCALE` in the shared notes): on the virtual
clock a frame's work is two fields by construction, so without that the game saw every frame
as dropped and stayed at 30. With it the desktop headless run and the Quest harness both
present 60 game frames a virtual second through a race (`[clock] ... frames (+60) ... 100% of a
field`).

**What broke, in order.** The frame counter was first advanced every other frame too, so
that anything counting it would keep the game's seconds; with that the starting lights never
finish (`0x80625A54` stays at 5 forever, watched with `GCN_WATCH`), whatever else is kept
(`WR_PACE_KEEP` isolates the three patches). Advanced every frame, the countdown ends at frame
2400 exactly as at 30, i.e. in 1.9 s instead of 3.8: the start sequence counts frames.

**The comparison.** `tools/compare_runs.py` lines up two `--eye --first-person` runs of the
harness with `WR_FPLOG=1`, whose eye hook prints the rider's world position every frame, by
time or by frame. A recording made unpaced on the Quest (`GCN_INPUT_LOG`, `--fast`, so the log
carries no clock jumps) replays at 30 bit for bit (apart 0.0 over a whole race). Replayed at 60
and lined up by frame, the rider reaches the same top speed per frame (1,200 units per 90
frames in both) and accelerates over the same number of frames: the physics are per-frame
constants. Lined up by time, the 60 fps rider therefore moves at twice the speed. The inputs do
not line up exactly either: the SI polls are the 120 Hz timer's plus one direct poll per frame
(`hw/si.cpp`), so a log keyed by poll lands a frame or two off at the other rate, which is why
a frame-aligned comparison drifts after the first throttle press.

**What it would take.** Not a couple of constants: top speed, acceleration, the waves, the AI,
every timer and the countdown are stepped per frame with constants of their own, scattered
through the game's logic (`0x8009D000-0x800B6000` in `recomp_005.c` alone reads the clock in
forty places and the counter in a hundred). The first conclusion was that a real 60 would be a
conversion of the simulation to a time step, a rewrite of the physics rather than a patch.
The second pass below found the steps instead.

### Finding the steps (2026-10-10, second pass)

The constants cannot be patched one by one, but the *steps* can be found mechanically: a step
is a store of a value computed from the old value at the same address (`x += t`, `x *= k`,
`x = x*k + b`, an integer `x += 1`), which is a shape the recompiled C exposes. The
pipeline, all of it under `tools/` and `gcn-recomp/docs/diagnostics.md` ("Finding a game's
per-frame steps"):

1. `tools/rate_sites.py` walks the generated C, decoding each instruction from the encoding
   in its comment, and follows where every register's value came from within a function. It
   lists every store of that shape with its rate or factor, literals read out of the DOL so a
   1/30 or a 0.98 shows as such: 3,062 candidates in the whole game.
2. A `-DGCN_WATCH` build (the build has to be configured with `-DCMAKE_C_FLAGS=-DGCN_WATCH
   -DCMAKE_CXX_FLAGS=-DGCN_WATCH`) run with `GCN_STORE_HIST=3031-4300` over the Time Attack
   race scene of the Ocean City Harbor route (`diagnostics.md`), headless and `--fast`, counts
   how often each store ran: 748 candidates run in a race. `GCN_STORE_HIST_PCS` with the
   candidate list adds every writer of every address they wrote, which tells state carried
   across frames (its only writers are steps) from a temporary assigned afresh each frame:
   352 state, 304 temporary, 89 unclassified (addresses with more writers than the table
   keeps, stack temporaries mostly).
3. `--emit-steps` writes `recomp/steps.txt`: the state sites in the game's own code
   (`0x80020000-0x80100000`; below is audio and data decoding, above the SDK), running at
   least 0.9 times a frame, and for an integer step counting by one. The recompiler emits
   each through the shared runtime's step scale, so the table holds addresses and an operand
   letter and no game code. `WR_PACE_KEEP=8` leaves them unscaled, and `GCN_STEP_SKIP` /
   `GCN_STEP_ONLY` take a list of addresses for bisecting.

At 30 the scaled build is bit for bit the unscaled one through a race (the `[ww]` watch of
the frame counter and the camera target, `WR_WATCH`, agrees line for line), which is the
regression check for the whole mechanism.

**What the filters are for, each learned from a broken run.** An integer stepped by four
(`0x80023078`, a cursor in a 128-entry ring) faulted the GX flush when halved; one stepped by
another word (`0x800F9364`, a sum) stalled the mode runner: hence "counts by one". A site
that runs on an event rather than every frame -- a lap counted, a state advanced, a decay
applied on a hit, forty-two of them in the race window at rates from once in the window to
0.8 a frame -- is the event's size, not a rate, and halving it loses the event: hence the
0.9. The windows so far cover Time Attack with one rider; the AI, the other courses and the
menus have their own sites, which a Championship window (the route without its `DOWN`) and
a longer one will add.

**Where it stands (2026-10-10, end of the second pass).** 109 sites are scaled
(`recomp/steps.txt`). At 60, switched on at the race, the game clock (`0x806919F0`) and the
race timer (`0x806199F0`, stepped by 1/30 at `0x80083FD4`) both advance one second per real
second, where before the pass they ran double. The race state (`0x806916F8`, 1 to 3 at the
start) is reached at 60 too. Two things are still at double speed, and both are outside what
the mechanism can reach:

- **The rider's physics is a particle system, not a rate.** `fn_80093858` integrates sixteen
  particles (52 bytes each: position, then an accumulator that is zeroed after use) with the
  constraint pass at `0x8009394C-0x80093A50` and the forces added in `fn_800BDFC0` and
  after. With its position steps halved the rider never moves off the line: the accumulator
  carries the momentum as well as the forces, so halving the step halves the speed every
  frame. Found by bisecting the 157 sites of the first list with `GCN_STEP_SKIP`: every group
  stalled except the one holding those three, and the z step alone (`0x800938D0`) decided it.
  Unscaled (`recomp/steps_skip.txt`), the rider accelerates to about 2,200 units/s against
  1,100 at 30: the per-frame physics at twice the frames. The right treatment is the
  Verlet one -- momentum carried whole, forces scaled by the square of the step, the derived
  speed (position minus the previous position, the array at `0x80620298`) converted back to
  the game's units wherever it feeds the thrust and drag curves and the HUD -- and that is a
  reading of the rider's functions rather than a filter, the one piece of the first pass's
  "rewrite" that stands.
- **The countdown still runs at 60.** The start comes at 1.5 s instead of 3.3 s. The counter
  at `0x806912AC` (100, decremented twice, zeroed) is not it, and nothing in the scaled set
  is; it is one of the sites the filters leave out (an event-rate site, a temporary, or one
  of the 79 unclassified), or a read of the retrace count. `GCN_STORE_HIST` over the ten
  frames around the start, intersected with the candidate list, is the next step.

The things the first pass thought were the obstacle -- the hundred reads of the frame counter
and forty of the clock -- were not: the counter steps every other frame like any other
integer and nothing minds, and the clock was one site.

**Driving the race at 60 without driving the menus at 60.** `WR_FPS_AT=<presented frame>`
switches to 60 mid-run, so the scripted route's frame counts hold through the menus and only
the race runs at 60; `WR_FPS_AT=3035` on the route above lands four frames into the race
scene, and a 60 fps run then needs a third of the frames. `compare_runs.py --reset 4` lines
the two runs up on the race scene (the fourth restart of the frame counter) rather than on
boot.

**A recording with clock jumps is useless for this.** The first recording was made paced, on
the Quest's harness, and carried 4,931 catch-up jumps (`jump` lines in `inputs.txt`): the eye
path and the one-frame batch queue hold the guest up a few milliseconds a frame, each of which
the clock caught up. A replay makes exactly those jumps at exactly those back-edge counts,
which at the other frame rate are different instants, and the 60 fps replay's inputs landed
nowhere near the race. Record unpaced.
