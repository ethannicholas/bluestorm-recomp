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
which found five more sites besides these two. With them the Championship starts at 60
(the race state reaches 3 at 7.0 s against 7.6 s at 30: the countdown's phase 1 comes at
frame 135 in both, phase 2 some 17 frames sooner at 60, not read further), and its AI
riders run at the right speed: rider 1 by real time is 5% quicker at 60 and launches 0.6 s
earlier, and stays 700-1000 units ahead of its 30 fps self over thirty seconds of the
course, where the unscaled AI would have been a lap ahead.

**What is still open in the Championship, 2026-10-10.** The scripted rider is beached, so
the race's end is not the field finishing: at 30 the mode goes 5 to 6 (`0x800FA118`, the
results) at 42.4 s on the race timer; at 60 the release build resets the race scene at
30.9 s with the timer frozen and the mode still 5, and the watch build at 60 does neither
by 41 s. The two release runs agree with each other frame for frame, so the difference
between the builds is a real-time one (the course music streams from the disc in real
time, and `[DVD] stream cancel` comes just before the results at 30), not a fault in the
tables. What ends a beached Championship race, and whether it ends at the same time at 60,
wants a played or recorded race rather than the scripted one; the other courses and the
headset are untried at 60 since the rider's half-step.

**Driving the race at 60 without driving the menus at 60.** `WR_FPS_AT=<presented frame>`
switches to 60 mid-run, so the scripted route's frame counts hold through the menus and only
the race runs at 60; `WR_FPS_AT=3035` on the route above lands four frames into the race
scene, and a 60 fps run then needs a third of the frames. `compare_runs.py --reset 4` lines
the two runs up on the race scene (the fourth restart of the frame counter) rather than on
boot.

**The headset's toggle, and the freeze it found (2026-10-10).** Clicking the left thumbstick
switches between 30 and 60 in play (`vr::GameHooks::left_click`, `wr::toggle_fps`; the
change is applied on the guest thread at the next frame step, like `WR_FPS_AT`). The first
build of it froze the game for tens of seconds at every click: the log showed one game frame
every 1.67 s for over a minute after a click to 60. `WR_FPS_AT` had switched mid-run without
trouble, so switching was believed safe; it was only safe unpaced. The shared virtual clock
was the count of back-edges times the ticks per edge, and the switch changes the ticks per
edge, so it rescaled every virtual second since boot: to 60 halved the clock and put every
pending timer minutes into the future, to 30 doubled it and left the guest minutes ahead of
the host, which the pacer sleeps off at 50 ms a poll. In the unpaced harness the idle jumps
cross such a gap for free. The clock now rebases at a scale change
(`gcn-recomp/docs/diagnostics.md`, `GCN_CPU_SCALE`).

**A recording with clock jumps is useless for this.** The first recording was made paced, on
the Quest's harness, and carried 4,931 catch-up jumps (`jump` lines in `inputs.txt`): the eye
path and the one-frame batch queue hold the guest up a few milliseconds a frame, each of which
the clock caught up. A replay makes exactly those jumps at exactly those back-edge counts,
which at the other frame rate are different instants, and the 60 fps replay's inputs landed
nowhere near the race. Record unpaced.

### The headset's first look (2026-10-10, fourth pass)

The user drove the toggle in the headset and listed what 60 looked like: the rider and the
ski drawn out of shape, the steering column huge, the water and the course's wildlife at
double speed, the wake streaked out ahead. The Quest harness reproduces the first two in a
frame dump at the countdown (`--eye --dump-every`, `GCN_DUMP_RANGE`), so they could be hunted
without wearing anything. Two more things it showed on the HUD: the race time ran double
while the race timer word (`0x806199F0`) stepped by 1/60 a frame exactly as it should
(`WR_WATCH`), and the speed read about half.

**The shape was the normalizations.** The hull's twelve particles sit in the same 3-by-4
layout at 60 as at 30 (`WR_WATCH` on all of them, compared about their centroid), so the
physics was not it. Bisecting the table with `GCN_STEP_SKIP` by address range, one dump per
range, took three rounds to six sites: `fn_80049F70` and `fn_8004C27C` multiply a bone axis
(three words at +12) by the reciprocal of its own length, 29 and 20 bones a frame. The emitter
read `x *= 1/sqrt(x*x + ...)` as a damping and raised the factor to the half power, which
left every axis at the square root of its length, and the ski and rider, skinned to those
bones, drew scaled. `tools/rate_sites.py` now refuses a `*=` whose factor is the reciprocal of
something computed from memory words (`is_normalization`), and the six are in
`recomp/steps_skip.txt` with the reason. Skipping either function's three alone restores the
whole picture.

**Three steps outside the emitter's window.** The histogram over the Time Attack window,
rated over the whole DOL rather than `0x80020000-0x80100000`, and the candidates watched at
both rates, gave three per-frame steps the window had excluded, now in
`recomp/steps_manual.txt`: the keyframe animation player at `fn_8000CCF0` (an object's time
at +56 stepped by (1/30)/its duration, nine objects in the race scene: the course's animated
things at double speed), a phase in small data (`0x80691DD0`, stepped by 0.005 a frame at
`fn_80103C08`, read once a frame by one low-range function) and the replay recorder's frame
count at `fn_80106100` (`0x80691E04`, one a frame in a race, which samples the rider's position
every eighth frame up to 7200). The HUD's race time is that count less the count at the green
light, over 30, which is why it ran double while the timer word was right. The window's
bounds were a guess about where the game's code ends, and both ends were wrong by a little;
the SDK's own counters (the retrace count at `0x80114E50`, two a frame) are the reason not to
simply widen it.

**The HUD's speed is a second speed word, still half.** The rider's own speed at +104 is
converted back to the game's units (the third pass), and the physics agree at both rates:
lined up on the race start, the 60 fps rider is at 619 units/s against 608 one second in,
1117 against 1105 at three, 1137 against 1114 at five, and within 75 units of the other's
position until the wall at ten seconds. But the HUD read 49 km/h against 90. The word it
shows is in small data at `0x806913C8`: world units a frame (36 at 30, 17 at 60 while +104
read the same at both, `WR_WATCH`), written once a frame at `0x8005420C` in the race helper
`fn_800540A4` (`GCN_WATCH_ADDR`) as a difference against the rider's field at +3432, after
the helper has copied the centroid (+68..76) into small data and taken a Newton square root
of something. Converting the root's last multiply with `R -1` changed nothing, so the root is
not on the path to this word; the entry was taken out again. What +3432 is, and where the 36
comes from, is the next thing to read. A frame count at `0x80691084` (`fn_80008FC4`), the
one per-frame integer a snapshot diff of the guest's data still found unhalved at 60, was
halved here and taken back out in the fifth pass: its function is the water simulation,
not the sound engine, and the water is treated whole there.

**The HUD's race time is still double, and is not a stored number.** With every per-frame
integer and float accumulator in the guest's writable data halved at 60 (snapshot diffs at
both rates, as words, halfwords and doubles: the only words advancing per frame at the 30 fps
rate were the two retrace counts), the HUD still showed 11.3 s where the timer word read
8.75 since the scene start and the countdown took 3.1, i.e. twice the seconds since the green
light. The timer word's only reader in the race is its own stepper, and the clock's only
other reader converts it to a frame count. So the display is computed at draw time from
something not yet identified; the race logic's own time is right. Left for the next pass.

**Also recorded.** The integrator's last loop (`0x80093A90`-`0x80093AB0`) adds each
particle's accumulator to its position once more after the relaxation; it was in the table
and is now in the skip list on principle, but the accumulator is zero there: a 60 fps run
with those scaled and one without were identical frame for frame. The readers of the race
timer word are only its own stepper, which is how the HUD's time was known to come from
somewhere else. `GCN_STORE_HIST` output on the device can have the harness's progress line
interleaved into a writers' line by the other thread; the parser skips such tokens now.


### The second headset report (2026-10-10, fifth pass)

With the bones fixed the user raced properly at 60 and reported: the rider's feet turning
through 180 degrees now and then with the ankles shrunk to a point; dolphins and buoys still
at double speed; the wake's white streaks still shooting out ahead; steering about half as
sensitive; and, lower down, the title and menus at double speed (the menu repeat makes a
double move easy) and the flyover camera the same.

**The white wash was the spray's spawn rate.** The Dolphin Park start at 60, with the
scripted rider waiting on the grid while eight riders launch, washed the water near the
camera white from frame 2700 to 3000 and was clear at 30. In order, what did not fix it:
scaling the spray particles' moves (`fn_8002FABC`, classed temporaries because a byte-copy
loop at `0x80003430` also writes them), gating the water simulation, scaling the wake
emitters' velocity terms (`fn_80030E2C`), and skip-bisecting every effect range in the
table. The draw log said why: `GCN_DRAW_SKIP` over the first half of the draws left a grey
hole where the water had been, and `GCN_DRAWLOG_VERBOSE` at both rates found every draw's
TEV state, textures and first vertex colours identical, so no draw was drawn differently,
only more of something was there. `GCN_WATCH_ADDR` on a word of one of the seven groups of
36 words that moved twice as far at 60 in a snapshot diff named the writers: the moves,
the copy that compacts the particle list after each frame (so the words at fixed addresses
are different particles over time, and that diff said less than it seemed to), and the
spawner `fn_8002F478`, called once a frame per rider from the effects function at
`0x800BBC08` and `0x800BBD9C` to place new particles at the hull. Twice the spawns a
second with the same life in frames is twice the spray; `patches.txt` runs the spawner on
every other frame at 60, and the start is clear. The moves and the wake emitters' terms
stay in the manual table: they are right in themselves, and the dampings beside them were
already scaled.

**The feet are a half-applied solver.** `fn_8005DC1C` runs the rider's body as a chain of
nodes: each is pulled along a direction by a spring term and snapped to its anchor in the
same add (`0x8005E1C8/D4/E0`), then pulled back by any overshoot past its reach
(`0x8005E270/7C/88`). The emitter scaled all six as rates. The anchor snap and the overshoot
are position corrections; at half they leave a node lagging its anchor every frame, which is
the stretched-ankle picture. The six are on the skip list; the spring term rides along
unscaled, which converges twice as fast in time and is the lesser wrong. Not verified in a
frame dump, since the scripted rider never showed it; the user's next session is the test.

**Steering, measured.** With the stick held left from four seconds into the race (`A+SL` in
the input script) the first second and a half of the turn agree at both rates, then differ:
the 30 fps rider's turn rate keeps building to about 48 degrees a second while the 60 fps
rider's stays flat near 33, with the speeds within 5%. So the steering's first response is
right and something that accumulates under a held stick is not. Where the game keeps the
stick: two guest snapshots at the same race frame, one with the stick held and one without,
differ in 24,000 bytes, and the only word in the rider's record that reads the stick's value
is +3280 (`0x80620FF8`, -87 held, 0 not). Its readers (`GCN_READERS`) are the rider's input
function `fn_800948E0`, which slews it toward the pad's signed stick at +3248 by at most 15
a frame (`0x8009496C`, an integer step by a variable amount, which the emitter's rule leaves
out), and `fn_80070D34`. Unscaled, the slew reaches full lock in half the time at 60, which
is the opposite of what the user feels, so the loss is further down: how +3280 becomes a
torque in `fn_80091F5C` and how the lean that builds under it is damped. The two raw stick
bytes a byte-for-byte diff of the data region turned up were coincidences: the pad buffers
live outside the snapshot's range, in the heap.

**The wake emitters, unverified.** `fn_80030E2C` keeps nine 40-byte slots, one per rider: a position,
a velocity and an age. The position step was in the table and the age counts down; the
velocity's dozen per-frame terms (drag against itself, lift, a gravity that grows with the
age) were classed temporaries, again because the respawn rewrites the slot, so at 60 the
velocities built twice as fast and the strips shot out ahead. Sixteen of them are in
`recomp/steps_manual.txt`; the two that add half the rider's own per-frame displacement are
left alone, since that displacement already halves. The Dolphin Park start at 60, where
eight riders launch at once, is the test: at frame 2700 the water near the camera was
washed white in every build before this one, and is clear at 30.

**Water.** The water mesh is written by `fn_800097D0`, which samples a 64-by-64 field of
16-bit heights for each of 1,860 vertices with bilinear interpolation; the field is the
32 KB region at `0x80346488` that changes every frame, two buffers of it. `GCN_WATCH_ADDR`
on a word of it names the writers: `fn_80008FC4` and the `fn_8000A2E0` it calls, the
integer loops at hundreds of stores a frame that the first pass took for the sound mixer
and left alone as "audio and data decoding". They are the wave simulation, called once a
frame from the rider's water forces (`0x800BDF8C`), and `fn_80013830` is the same for the
title screen's water. A wave equation cannot be scaled store by store, so `patches.txt`
gates it with `gcn_step_int`. Gating the simulation call alone was not enough: the caller
`fn_800BDDC4` is the water-and-effects update, which first pushes every rider's hull into
the field (`fn_8000E94C`, once per rider), then runs the simulation, then the keyframe
animation player and `fn_8000E9B0`; with the pushes still at twice the rate the field held
twice the energy and its foam washed the Dolphin Park start white at 60 in every build
(clear at 30). So the whole update runs on every other frame at 60, from its two call
sites, and the animation player's own step needs no entry. The buoys ride the field, so
they follow it. The field's frame count (`0x80691084`) halves by itself. **The white start
is still open:** with the whole update gated, the spray and the wake emitters scaled and
every effect range skip-tested, the water near the camera at the Dolphin Park start is
still washed white at 60 (frames 2700-3000 with the scripted rider waiting on the grid
while the field launches) and clear at 30; the rider's own run at speed looks right. It is
something the table does not reach, so the next tool is a frame dump with the draw log
(`GCN_DRAWLOG`, `docs/dev/vr.md`) to name the draw that paints it.

**The menus.** A histogram over presented frames 1000-3030 of the route (title, menus,
flyover) lists 289 per-frame state steps not in the tables, most of them the sound mixer's
and decoders' (`fn_80008FC4`, `fn_8000A2E0`, `fn_80013830`) which must be left alone; the
menu repeat timer is among the rest and not yet picked out. The camera's springs
(`fn_80059084`: a velocity damped and driven, a position stepped by it) have only the
position step in the table, the velocity's updates having been classed temporaries, which
is why the camera moves differently; the Verlet treatment the rider got applies.

**Still open after this pass.** The dolphins still run double: the creature mover
`fn_8006DFA8` and its damping are scaled, the animation player runs on alternate frames
inside the water update, and the only Dolphin-Park-only integer step left (`fn_800BAA58`, a
fade by four) is scaled, so what moves them is something not yet named. A snapshot pair across the same 3.3 s at both
rates turned up only the spray pools and copies of the AI riders' positions, which means
the dolphins' state is in the heap, above the `WR_SNAP` range, and the draw log shows them
drawn with the camera's matrix over world-space vertices, so `GCN_POSMTX_STACK` cannot
single one out either. The way in is the course-object list: extend `WR_SNAP` to the heap,
or watch a dolphin draw's first vertex position (`GCN_DRAWLOG_VERBOSE` prints it) back to
the word it was transformed from. The
water's lighting differs at 60 (the reflections define the surface less), which the gated
simulation may itself cause. The steering, the HUD's time and km/h as before.

### The steering, sixth pass (2026-10-10 night)

**What was measured.** The Ocean City Time Attack route with the stick held left from 4.2 s
into the race (`3155:A+SL:3000` at 30; at 60 the hold starts at presented frame 3283 after
`WR_FPS_AT=3031`, since the frame counter at `0x80690FF8` is halved at 60, so race time is
f/30 at both rates). Heading from the rider's direction at +92/+100, the hull's lean from
its up axis at +124, both through `WR_WATCH` (addresses are `0x80620328` + offset; two
tables in this pass were read off wrong addresses before that was checked, so compute them,
and prefer the rider-record diff below). Over 4.27-6.27 s the 30 fps rider turns through
77.6 degrees and leans to up.y 0.70; the 60 fps rider turned 49.4 (64%) and stayed level
(0.97). The rider reaches the harbour wall at 6.3 s, which is why the window is short; the
Dolphin Park start is open water but the field runs the scripted rider down, so it gives no
clean turn either.

**How the stick steers.** `fn_800948E0` slews +3280 toward the pad byte at +3248 by 15 a
frame, but nothing in the physics reads +3280: its readers are the rider's body animation
(`fn_8006DFA8` at `0x80070D34`, the lean pose), `fn_800646FC` and `fn_800A43CC`. The
physics read the pad byte itself, in `fn_80095848`, the rider's control function, which
makes two things of it. One is a yaw couple: +3436 = stick x a per-character table, applied
in the rider update at `0x80092A64` as opposite forces along the right axis on particles
0-2 and 3-5. The other is the jet: +3772/+3768 = sin/cos of (stick x the gain at +3684 x
70/176 of a turn), and the update at `0x800923BC` builds the thrust direction as cos x
forward + sin x right, so the stick vectors the thrust up to about 50 degrees. The couple
is gated at `0x80096508`: when the speed word at +3332 is 11.11 or more it needs +3512,
the wall-contact depth from `fn_80088E14`, to be non-zero (a slow rider in states 1, 24
and 30 gets it freely). So at speed the ski steers by the vectored jet alone, and the
couple is a low-speed and wall aid; the gain +3684 is +3376 below the speed +3676 and
shrinks above it by +3680.

**What was per-frame and is now converted.** `fn_80097824` measures the rider's motion every
frame as differences: velocity +3308..3316 = position (+68..76) - last frame's (+3424..3432),
acceleration +3320..3328 = velocity - last frame's, speed +3332 = |velocity|; and the same
for a reference point on the hull placed by a per-character table (+3464..3472, its velocity
+3476..3484 and acceleration +3488..3496). At a half step the velocities read half and the
accelerations a quarter. The readers compare them against the game's constants or feed them
to forces: the couple's gate and the gain above; `0x80095918`/`0x80096080` (state changes
at 2.7 x +3332 >= 30); `0x80092810` (a planing lift in state 31 above 5.56); the lateral grip
at `0x8009297C`, which takes the reference point's velocity against the right axis as a
force on the accumulators (so at 60 the grip was computed from half the velocity and then
scaled by the square of the step: an eighth); the camera (`0x8005F8FC`), the AI (`0x8007xxxx`)
and the sound, whose rumble index is the acceleration's magnitude (`0x800BDBB8`). The
twelve subtractions carry `R -1` in `recomp/steps_manual.txt`, the acceleration's being a
difference of already-converted velocities. The HUD's km/h was the same quantity measured
again by the HUD (fourth pass), which is why it, too, needed `R -1`.

**The result, and what is still short.** With the twelve the 60 fps turn is 53.6 degrees
over the same two seconds (69% of 30's) and the lean comes back in part (up.y 0.85). The
rest is not a gate: the thrust power, the jet angle, the dampings and the states are
identical at both rates in snapshots at the same race time (`sn/t` against `sn/c`,
`riderdiff.py`), the couple is off in both, and leaving each group of the rider's steps
unscaled (`GCN_STEP_SKIP` on the integrator's dampings, `fn_800BDFC0`, the 136 force
sites, the water's forces) made the turn worse or wild, never better. `GCN_STEP_POW`
(new in the shared runtime, `gcn-recomp/docs/diagnostics.md`) tried the water's forces at
power 1 (49.7, slightly worse) and the thrust at 1.5 (a faster turn, but the top speed goes
with it). What is left differs in the dynamics, not the constants: the hull's yaw under a
sideways push at the stern goes through the Verlet momentum, the lateral damping k (0.57
across the heading, per frame) and eleven constraint relaxations a frame, and a half step
with k^s and forces x s^2 is not the same system; a first-order model of it predicts a
slightly *faster* turn at 60, so the model is missing something, most likely in how the
relaxation and the k-damping share a frame. The acceleration conversions change nothing in
the turn (bisected), and are kept for the sound.

**Beliefs this pass overturned.** That the loss was in how +3280 becomes a torque: +3280 is
only the animation's copy. That +124 is the hull's roll: the particle heights (rows at
-80/-90/-104 at 30, -73/-92/-96 at 60) show both hulls rolled alike, and +124 is an axis
the body code derives. That the body chain at +1880 (40-byte nodes) is physics: its one
caller is in the camera/draw range (`0x8005F6E4`). That 60 from boot is safe in the
harness: `WR_FPS=60` set at boot hangs the harness at the second frame-counter reset (the
course load, presented frame 2696 on this route, 2076 on Dolphin Park) with `--fast`,
every time, while `WR_FPS_AT` at the race is fine; the headset app applies `fps 60` from
`vr.txt` at boot too, so this wants a look before `fps 60` is recommended there.

**Tooling.** The desktop binary can no longer be rebuilt here (its clang is gone), so the
loop was the Quest harness: `--frames=N` to stop at a presented frame, `WR_WATCH`, and
`WR_SNAP` with `WR_SNAP_RANGE` for the heap. Two desktop runs at once derail each other's
route (the memory card). Never `sed -i "s/\r//"` a script from PowerShell: `\r` reaches
the device as `r` and the first `r` of every line is deleted; push LF files as they are.

**The dolphins, after this pass.** Heap snapshots at both rates (`WR_SNAP_RANGE=806A48C0-817F9E20`,
Dolphin Park with the field launching, 3 s and 6.3 s of race, `snap4.py` in the scratch
notes: words whose change at 60 is twice their change at 30) turned up bone matrices at
`0x8102xxxx` and `0x81197xxx`, which the writer watch names as `fn_8004A460` and
`fn_8004BAA4`, the skinning of a *rider* record (it reads +68..76 and the heading at
+3632..3644): the AI riders bobbing on the grid, not dolphins. And `fn_8006DFA8`, which the
fifth pass called the course's creatures because Dolphin Park showed seven of them and
Ocean City none, reads rider+3288 (the character index) at `0x8006F0B0`: it is the riders'
body mover, and the seven were the seven AI riders. So nothing in the tables touches the
dolphins yet, and no word of theirs has been found; the way in is the draw side
(`GCN_DRAWLOG_VERBOSE` for a dolphin draw's first vertex, `GCN_WATCH_ADDR` on that word,
then on what wrote it). The `0x80E5xxxx-0x80EAxxxx` region of the diff is the water's
two vertex buffers (`fn_800097D0` and `fn_8000F7CC` write them), also not them.

### The dolphins, found (2026-10-11)

The user drove up to a dolphin somersaulting in a loop and toggled 30/60 while watching it,
and recorded the session. **The recording replays in the harness** (`GCN_REPLAY=<dir>` with
the log and its card copy beside the harness): it gets through the menus and into the Dolphin
Park race, which the earlier "sits on the logo" note did not expect. What it cannot do is
drive the race: the headset app paces itself, so the log holds a clock jump a frame, and the
stretches the user spent at 60 ran the guest at double CPU scale; a replay at 30 makes the
same jumps at the same back-edge counts but gets half the frames out of them, so the 357 s of
polls were spent by presented frame 4120 and the rider never left the grid. The poll density
between jumps in the log does show where the toggles were. It was not needed: the dolphins
are in Dolphin Park whether or not the rider moves.

**The way in was the draw side.** `GCN_VTXLOG=<frame>` (shared front end) prints each draw's
vertex array address, its first vertices in model space and the matrix each selects, and the
course geometry's matrix is the view matrix, so a bone's view-space translation inverts to a
world position. At presented frame 4483 of the scripted Dolphin Park route a six-part mesh
with one texture (`0x80E04600`..`0x80E0655C`, 54 units long) sat at world (-2498, -24, 2554)
and moved 13 units in a second, mostly up; fish (three parts, 20-30 units, `0x80E16xxx`,
`0x80E1Cxxx`, `0x80E23xxx`) swam under the grid, gulls (`0x80E11xxx`) flew. Searching the
*data region's* snapshot for a 3x4 matrix with that translation found the course object
list: records of 296 bytes from `0x805A4F90` (position at +4, +8, +12; a scaled matrix at
+0xF8; the animation index at +156 and its frame at +158; update and draw functions at +0xB4
and +0xB8; the model's heap pointer at +0xBC). Five records share the dolphin's functions
(`fn_8005697C`, `fn_800569F4`), the birds have theirs (`fn_8005C080`, `fn_80057D4C`). The
first dolphin sits at (-2500, y, 2556) with only its height moving: the somersault in place.

**The step.** The writer watch on the record's frame halfword named `fn_800540A4` at
`0x80054544`: the course objects' per-frame pass advances every object's animation frame by
one and wraps it at the animation's length (an add, a divide and a multiply, so the
histogram's "counts by one" test never saw it). At 60 every animated course object played at
double speed. `80054544 A` in the table steps it on alternate frames; the dolphin's frame
now reads the same at both rates through the race (16 at 0.5 s, 31 at 1 s, then the
somersault's 1-4-7-10 cycle). The object mover `fn_8005A9C8` moves the height toward its
target through four branches at `0x8005AB08-0x8005AB44`, of which the histogram had listed
one; the other three are in the manual table now, for the two-second rise at the start.

**Beliefs this overturned.** That the dolphins' state was in the heap: the records are static
data at `0x805A4F90`, inside the old snapshot range, and only their models are in the heap.
That `fn_800540A4` was the HUD's speed: it is the course objects' pass, and the km/h word it
writes is one of the things that pass does. That the `0x80E5xxxx-0x80EAxxxx` words were
creatures or sound: they are the water's vertex buffers. The vertex log and a data-region
snapshot together get from a thing on screen to its record in two runs; the heap diff never
would have, since the records do not move.

### The steering, seventh pass: the bank, and the four passes (2026-10-11)

**What was measured.** `WR_SNAP` now takes frame ranges, so one run dumps the rider's
record (`WR_SNAP_RANGE=80620298-806214C4`) every presented frame of the turn, and
`tools/rider_trace.py` tabulates it: the velocity's heading, the hull's heading, the slip
between them, the roll from the hull's right axis (+132..140), the momentum on the hull's
axes, the particle heights. Over the Ocean City turn the 30 fps hull banks to 37 degrees by
5.5 s and 46 by 5.8, and the turn rate rises with the bank; the 60 fps hull (sixth-pass
tables) banked to 24 and 31. **The loss was the bank.** The sixth pass had concluded both
hulls rolled alike from the heights of particles 0, 3, 2 and 5: that compared the wrong
pairs. The hull is twelve particles in three rows of three, 0/3/6/9 the left bottom row,
2/5/7/10 the right, 1/4/8/11 the top (a keel-shaped prism, 72 long, 24 wide, 21 high);
0 and 3 are both on the left, and left against right the 30 fps hull sat 16 units lower on
the right, the 60 fps one 4.

**What the roll is.** The site the sixth pass called the lateral grip, `0x8009297C`, is the
roll torque: gravity plus (right axis . the stern's velocity +3476) times the gain +3692
goes on the right bottom row, gravity minus it on the left. The reference point +3464 is
the stern, 27 units behind the centroid (the character table at `0x805A4880`), so the ski
banks when its stern slides out: the jet pushes the stern out, the stern's lateral velocity
banks the hull, the bank turns the ski, and the turn adds yaw rate times the lever arm to
the stern's velocity. A loop with gain near one, which is why a modest error in the
dynamics settled at a very different bank. The torque's input was the same at both rates
(right . stern velocity about -5 units a frame from 5 s on), so the response to it differed.

**The hull is not rigid.** The integrator's constraints (65 of them, 12-byte records of two
particle pointers and a tenth of the rest length at `0x80620628`) are relaxed once per
call: each adds (rest / length - 1) / 10 of the separation to one end's accumulator and
subtracts it from the other's, and the position step at `0x80093A90` applies that with
the momentum. The "eleven relaxations" of the third pass were a misreading of the 11 at
rider+40, the last hull particle's index. The six correction sites are tabled as forces
(`A 2`, `recomp/steps_manual.txt`); with them alone the 60 fps turn moved from 55 to 57
degrees of hull heading at 6 s against 74, and powers 1.5 to 3 ran 54 to 60: the hull's
stiffness was a part of it, not the thing.

**The four passes.** A writer watch on one bottom particle's vertical accumulator
(`GCN_WATCH_ADDR=80620430` on the watch build; `watchseq.py` in the scratch notes parses
it) showed the whole sequence of stores -- the rider update's thrust and roll torque, the
water's buoyancy and lift, the damping, the integrator's zeroing, the constraints, the
momentum store -- **four times between two frame marks at 30, and four times per presented
frame at 60**. The riders' frame `fn_80091C04` runs each player's whole physics step in a
loop of four passes a frame (`0x80091D4C` and `0x80091DC4` load the count), the AI riders'
once, and then measures (`fn_80097824`, once a frame). The sixth-pass treatment scaled the
sites inside the pass -- forces at s^2, dampings at k^s -- and so ran eight half-size
passes a game frame: the dampings, the constraint corrections and the lateral k acted
twice as often, and the hull was stiffer and more damped than the game's. The game's own
dynamics come back with **two passes of the original size per paced frame**: the patches at
`0x80091D4C`/`0x80091DC4` set the count to four times the step scale and the shared
runtime's `step_override(1)` (gcn-recomp `docs/diagnostics.md`) leaves every site in the
loop unscaled; the patch at `0x80091E00`, the first instruction after both loops, ends it
(`WR_PACE_KEEP=16` keeps the four passes). What runs once a paced frame outside the loop
keeps the table's scaling: the control law `fn_80095848`, the frame's measurements
(`fn_80097824`, `R -1`), the HUD's km/h. The AI riders' single pass is still a half-step
with the sixth-pass tables, which the Championship showed to be close enough.

**The result.** Hull heading and bank over the turn, race time against the 30 fps run:

| race time | 30 fps heading / bank | 60 before (sixth pass) | 60 now |
|---|---|---|---|
| 5.0 s | 27 / -25 | 21 / -20 | 22 / -23 |
| 5.6 s | 54 / -40 | 40 / -25 | 46 / -33 |
| 6.0 s | 74 / -46 | 52 / -31 | 61 / -42 |
| 6.4 s | 91 (wall) / -48 | 66 / -32 | 78 / -38 |

and with the first-pass fix below as well, 60 reads 43 / -32 at 5.6 s, 68 / -50 at 6.0 and
87 / -49 at 6.4. The 60 fps route's stick starts two game frames later (its presses sit at
doubled frame offsets), which accounts for the lag that remains; the turn develops at the
30 fps rate and the bank reaches the 30 fps bank. The 30 fps run is bit for bit the old one.

**The first pass of a frame.** With the passes right the 60 fps rider still accelerated 9%
slower off the line, with the same power word and the launch at the same race time. A
writer watch on the speed word showed the four passes of a 30 fps frame gaining 0.021,
0.030, 0.030 and 0.030: the first pass of every frame gains less, and at 60 every paced
frame has that first pass, so a game frame got two small passes instead of one. A watch
on a stern particle's forward accumulator put the difference at the thrust store itself,
three quarters of the other passes' -- the constant the rider update applies at
`0x80092134` when neither stern particle's flag word has bit 0, "in the water". A watch
on the flag word named the writer: the control law `fn_80095848`, once a paced frame,
zeroes every particle's flag word at `0x80095894`, and the water sets the bit again in
each pass, after the first pass's rider update has already read it clear. So the game's
own first pass thrusts at three quarters, and the patch at `0x80095894` takes that store
on alternate frames at 60, once a game frame. With that the 60 fps rider's speed off the
line tracks the 30 fps rider's to within a per cent, one game frame ahead: the launch
lands a presented frame earlier at 60 (the route holds A from before the countdown, and the
countdown's last frame is halved), which is a 33 ms lead in the script, not a dynamics
difference.

The `+4238` halfword, which the third pass called the player's four damping passes, is an
inner variant (four more integrator calls and forces times four, in `fn_800935xx`) set per
rider from a global in `fn_800984A4` and `fn_800BB69C`; it is 0 for every rider in Time
Attack and was not touched. The rider's stick copy at +3280 (`fn_800948E0`, a clamped slew
of up to 15 a frame that only the lean pose and two readers in the camera and AI consume)
takes alternate frames too, so the rider leans into a turn at the 30 fps rate.

**Beliefs overturned.** That the rider is integrated once a frame (four passes for a
player). That the constraints are relaxed eleven times (once per pass, at a tenth). That
the two hulls rolled alike (wrong particle pairs). That `+4238` marks the player (it marks
nothing in these runs). The sixth pass's bisection of the rider's step groups could not
have found this: every group was scaled right for a single pass, and the error was in how
many passes there were.

### The HUD's race clock (2026-10-11)

The user asked why this had not been read off the harness's own frames, and it should have
been: `--eye --dump-every=30` with `GCN_DUMP_RANGE` over the race at both rates gives the
clock as drawn, and at the same race time (3.97 s and 5.97 s after the reset) the 30 fps
frames read 0'00"924 and 0'02"904 and the 60 fps frames 0'01"947 and 0'05"907. Exactly
double from the launch, with the current build. The float race timer the fourth pass
watched (`0x806199F0`, +1/30 a frame at `0x80083FD4`, in the table) was never what the HUD
draws: it is rider 0's +280 of the 828-byte rider-state record at `0x806198B8`, and the
clock the HUD draws is a millisecond count beside it, +312 of the record's +32 block
(`0x80619A10`), stepped by 33 a frame in the lap timing `fn_80085624` (`0x800857D4`; the
lap's own count at +400, `0x800857F4`) and capped at 9'59"999, with a 4'59"999 pair in
`fn_80086D24` for the other mode. A step of 33 is one the emitter leaves alone (it keeps
integer steps to those that count by one), and the integer search in the sixth pass
compared counters that step by one, so it did not see these. The four sites take alternate
frames at 60 (`recomp/steps_manual.txt`). Found by diffing the rider-state record between
two 30 fps snapshots fifteen frames apart (a word that gains 495) and against a 60 fps
snapshot at the same race time (the same word at twice the value).
Verified on the same frame dumps with the fix: at 3.98 s and 5.98 s the 60 fps clock reads
0'00"957 and 0'02"937 against 30's 0'00"924 and 0'02"904 at 3.97 s and 5.97 s, one frame's
33 ms apart, which is the route's one-frame-earlier launch at 60. The starting countdown
on the gantry board was already right at both rates (3 at 1.5 s, 3-2 at 2.0 s, 3-2-1 at
2.5 s, go at 3.5 s).

### The title sequence (2026-10-11)

The logo and title screens ran at double speed at 60, every part of them, and the reason
was not a missing table entry. Harness frame dumps of the sequence (`--eye --dump-every=10`
over presented frames 200-700 from boot, with `WR_FPS=60` and without) are byte-identical
frame for frame at the two rates: nothing on the title was halved or gated, although the
title's water simulation (`fn_80013830`) has been gated to alternate frames since the fifth
pass. The step clock the gating keys on, `step_frame()`, was called from the mode runner's
advance of the game's frame counter (`0x80006F14`), and the logo and title screens never
reach the mode runner: on them `gcn_step_int` saw the same frame number forever and
answered 1 every time, so every integer step and every gated call ran every frame. The
call now sits in `wr_fields_per_frame`, the hook in `fn_80007A94` at the retrace wait,
which ends every frame the game presents (`runtime/pace.cpp`); a rate switch asked for
from the headset is applied there too, so the thumbstick toggle now works on the title.

The sequence's own steps, from a store histogram over presented frames 250-600 (the
emitter's earlier windows all began in the menus): the title-side frame `fn_800129E8`
steps the mode's frame counter (`0x806910AC`, which every logo and title handler reads for
its timing) and six texture-scroll phases (`0x80691094`-`0x806910A8`, each wrapped by a
mask); the handlers `fn_8001400C` and `fn_80014760` step the fade level `0x806910AE` by 8
a frame. The emitter leaves integer steps that do not count by one, so the scrolls and the
fades are in `recomp/steps_manual.txt` by hand; all take alternate frames at 60. The SDK's
counters in the same window (the retrace count at `0x80114E4C`, the scheduler's at
`0x8010E55C`) are left alone, as before.
