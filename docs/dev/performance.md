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

**Where it stood at the end of the second pass (2026-10-10).** 131 sites were scaled
(`recomp/steps.txt`). At 60, switched on at the race, the game clock (`0x806919F0`) and the
race timer (`0x806199F0`, stepped by 1/30 at `0x80083FD4`) both advance one second per real
second, where before the pass they ran double. The race state (`0x806916F8`, 1 to 3 at the
start) is reached at 60 too, and the countdown ends on time. One thing was still at double
speed, and it was outside what the histogram can reach (the third pass below is its
reading); the second item was the same symptom with a different cause:

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
  reading of the rider's functions rather than a filter: the third pass.
- **The countdown ran at 60 until the event filter learned about streaks.** The start came
  at 1.5 s instead of 3.3 s. The counter is `0x806919E4`, found by taking `WR_SNAP`
  snapshots during the countdown and listing the words that step by exactly one per frame
  between them: it is loaded with 30 per stage (`0x800C30F4`, an event) and decremented at
  `0x800CBB04`, which ran 110 times in the 1,269-frame window and so fell under the 0.9 a
  frame rule. The histogram's streak column (how many of a site's frames followed another
  it ran in: 109 of 110 here) now keeps a step that runs every frame while it is active;
  with it the start comes at 3.1 s at both rates, and the table has 131 sites.

The things the first pass thought were the obstacle -- the hundred reads of the frame counter
and forty of the clock -- were not: the counter steps every other frame like any other
integer and nothing minds, and the clock was one site.

### The rider's half-step (2026-10-10, third pass)

The rider is a Verlet particle system, and the reading of it is in `recomp/steps_manual.txt`
(a table the emitter copies into `steps.txt`; the mechanism's additions -- a power column,
the `R` and `P` letters, `GCN_READERS` -- are in `gcn-recomp/docs/diagnostics.md`). The
rider's record is 4508 bytes at `0x80620328 + 4508 * i` (`fn_80091F5C` takes the index);
its sixteen particles are inline at +144, 52 bytes each: position, then the accumulator at
+12, a lateral damping `k` (0.57) at +24 and flags at +28. The frame, in order:

- `fn_80091F5C`, the rider update, adds the thrust (+3344 is the engine's power, chosen by
  a curve read against the speed at `0x80091FD0`), the steering torque, gravity (the record's
  first float, applied to eight particles, twelve in state 31) and the pitch and roll
  torques to the accumulators -- 136 add sites, which the histogram cannot see because the
  accumulators are zeroed every frame, listed by a script that pairs each add with the
  load and store of the same particle field -- then a lateral drag in a loop over the
  particles when the rider is neither turning nor airborne (`0x800933FC`).
- `fn_800BE4E0` samples the water under each particle through the function pointer at
  `r13-30172` and adds buoyancy, the wave slope's push and a lift that grows with the
  speed; it also runs the wake emitter, whose counter at `0x80632BF0 + 4 * i` accumulates
  the speed (`0x800BE5FC`). Then `fn_800BDFC0` damps the accumulators: the vertical
  component by +16 (0.835), the lateral one by +20 (0.898, as `k*a + (1-k)(a.d)d` on the
  heading `d`), once a frame for the AI and four times for a rider whose +4238 is set.
- The collisions (`fn_80088E14` the course walls, `fn_8008978C` and `fn_8008908C` the
  colliders, `fn_80089C74` and `fn_8008AB68` the other riders) cancel the normal part of
  the accumulator and push every particle out by the penetration depth. Both are derived
  from the state, not rates: the impulse from the accumulator, the push-out from the
  positions. When the scripted rider sits against a wall the push-out runs every frame and
  the emitter took it for a rate (`0x80089034`), which is why those sites are in
  `recomp/steps_skip.txt`.
- `fn_80093858` integrates: it saves the positions to `0x80620298`, steps each by
  `0.992 * acc` (the 0.992 at +12 is a damping, not a time step) and zeroes the
  accumulator, relaxes the distance constraints eleven times, applies the corrections,
  then stores the next frame's momentum into the accumulator: the displacement since the
  save with its component across the direction of travel scaled by the particle's `k`. It
  also derives the speed: the centroid's displacement at +80..88, its magnitude at +104
  and its direction at +92..100.

The half-step follows from that. The momentum is carried whole and the forces take the
square of the step (power 2), since Verlet adds a displacement and a force to the same
position. Every per-frame damping -- the 0.992, the particle's `k` and its `1 - k`, the
two in `fn_800BDFC0` -- takes the root (power 1, as `k^s` and `1 - k^s`). The speed, which
the game measures over one step and compares against thresholds, feeds the thrust curve,
the lift and the HUD (`0x8000A538`), is converted back to the game's units where it is
stored (`R -1` on `0x80093B90` and the two after it); the only reader that integrates it
rather than comparing it, the wake counter, is then a rate and is scaled. The position
step and the collisions are left alone. Finding the readers of the speed is what
`GCN_READERS` was written for: 76 sites in the game load a float at offset 104 of something, and
nine sites read the rider's.

The result, on the route above with `WR_FPS_AT=3035` and `compare_runs.py --reset=4`: the
rider accelerates on the same curve at both rates (196 against 233 units/s one second after
the start, 672 against 713, 998 against 1007, 1109 against 1119) and tops out at the same
speed (1138 against 1144), where unscaled he reached 2227; at 30 the build is still bit for
bit the old one. What remains different is after the wall at ten seconds: at 30 the rider
stops against it (under 10 units/s), at 60 he slides along it at 30-70 for ten seconds, and
his heading drifts a degree or so the other way over the run-up. The wall contact is a
position correction applied per step with an impulse derived from the accumulator, and
neither is obviously wrong at a half step; it has not been read further.

The table was checked against the writers' histogram with the writers' cap removed: over
the Time Attack window every store to rider 0's accumulators is the integrator's own, the
wall impulse or a site in the table.

What was wrong before: the +12 field of the rider's record was taken for a time step. It
is 0.992, a drag applied to the whole accumulator, and the thing the first bisection
halved was that product, which is why the rider stopped. And the first histogram run was
killed by a fifteen-minute alarm before its window closed: the watch build takes twenty
minutes to reach presented frame 4300 on a loaded Mac, and the table is printed only then.

### Wider windows (2026-10-10)

The Championship (the route without its `DOWN`) reaches its race scene at the same presented
frame, 3031, so the same window serves; at 30 the scene ends at 4307 with the race timer at
42.4 s (the field is home and the scripted rider is beached). Its histogram adds the eight
riders' steps, and the emitter now takes one `--hist` per window and judges each window on
its own: a step is kept if some window keeps it, since a rate active in one scene and not
another (an AI rider's) or for a stretch of one (a countdown) would fall under the rate
rule of the windows summed. The temporary test works the same way, for a reason found the
hard way: eighteen steps in `fn_80032FDC`, positions of pooled spray particles, are state
in Time Attack, where the beached rider spawns none, and a "temporary" in the Championship,
where the spawner assigns the field often enough to look like a per-frame reset. A real
temporary (the accumulators) is one in every scene. And the limit on an integer step's
rate rose from four to sixteen a frame, because a per-rider counter runs once per rider.
Both windows together: 227 sites from the histograms and 225 by hand.

**The Championship would not start at 60.** The race state (`0x806916F8`) stayed at 1
with every table site skipped by halves, which pointed away from the table; what the halves
could not say was that `GCN_STEP_ONLY` leaves the frame counter's own patched site
(`0x80006F14`) unscaled, and every "only" subset started the race. So the start depended on
the frame counter's halving, through `fn_80025EB8`: before the countdown, the AI drives the
riders to the grid along waypoints, and the start phase (`0x80691870`, a halfword) goes to
1 when the drive is still on (the byte at `0x8069118A`) and the frame counter has reached
135. The drive ends when the last waypoint's time is up, a per-rider counter at +7436 of the
AI record (`0x803DFF6C` for rider 0) stepped by 1.0 a frame at `0x8002C8A8` and
`0x8002B368`; at 60 it ended before the counter reached 135, and the phase never left 0.
Both steppers are `lfsx`/`stfsx`, the indexed forms, which `rate_sites.py` did not pair as
a load and a store of the same address: it now keys an indexed access by both registers,
which found five more sites besides these two.

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
