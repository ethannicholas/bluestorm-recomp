# Motion telemetry

What drives a motion platform from this game, and how its numbers were checked. The shared half
(the packet, `--telemetry`, `telemetry.txt`) is in `gcn-recomp/runtime/telemetry.*` and
`gcn-recomp/docs/diagnostics.md`, "Motion telemetry"; the user-facing part is the README's
"Motion platforms".

## What moves the platform

The platform follows the **first-person eye** (`runtime/first_person.cpp`), in every view and
not only in first person. That was the request, and it is the right source: the eye's pitch and
roll are the hull's, eased over `fp_tilt_s` and scaled by `fp_tilt` (see vr.md, "First
person"), which takes the 30 Hz slap of each wave out and leaves the swell, and that is what
riding it in the headset already felt like.

So the eye has to be followed whenever telemetry is on. It used to be stepped inside the eye
hook, which runs only in the stereo path with first person on. Now the renderer has a frame hook
(`gx::render_set_frame_hook`), called on every batch however it is drawn. `first_person.cpp` does
all its work there: finding the hull, stepping the eye, and, in first person, the view transform
and the rider's draws to hide. The eye hook just returns what the frame hook found in the same
batch. When neither first person nor telemetry wants the eye, the frame hook does nothing and
the eye lapses, as it used to on a gap in the frames.

`runtime/motion.cpp` turns the eye into motion:

- **pitch, roll** are the eye's eased angles, already scaled by `fp_tilt`, blended by `raw`
  toward the hull's own (all the way, by default since the second rig session; see "Livelier"
  below). Roll is negated: the eye's is positive with the right side up, the telemetry's with
  the right side down.
- **yaw** is the eye's eased heading, blended the same way, negated so that it rises turning
  right.
- **velocity** is the eye's position differenced per game frame (`frame_dt`) and eased over
  `ski_velocity_s` (0.05 s); **acceleration** is that differenced again and eased over
  `ski_accel_s` (0.15 s, the eye's own `fp_tilt_s`). The position is the hull's exactly, so
  this second easing is the only smoothing the accelerations get. At 0.1 s the heave was
  still jumpy at 30 Hz.
- **surge** and **sway** are that acceleration along and across the eye's heading, **heave** is
  its world-vertical part (no gravity, eased again over `heave_s`), and the **lateral
  velocity** is the slide. On top of each goes a knock for an impact ("Livelier" below).

## No jerks

Some commercial games step their motion data when a race starts or ends, and the rig throws its
rider into position. Nothing here is allowed to do that. The first version only faded linearly
as the ski appeared and disappeared, and going through it turned up worse cases:

- **Pausing at speed** freezes the ski. Differenced, 90 km/h to nothing in one frame is about
  700 m/s^2, and unpausing is the same forwards. Now a ski that has not moved at all for four
  frames is paused: the platform eases to rest, the velocity is kept, and resuming eases back
  in. On the run below, a pause at 79 km/h eases to zero over the second after it.
- **The ski lost and found again** (or found the first time) restarted the track with a
  velocity of zero, so the next frame read as an acceleration from rest to racing speed. Now
  the first frame only takes the position, the second sets the velocity outright, and
  accelerations ease up from zero from the third. The first two are marked discontinuities.
- **A respawn** jumps the eye more than 300 units in a frame. It restarts the track the same
  way, and the eye's own angles snap to the new hull too, so it is marked a discontinuity.
- **Heading** wraps from +180 to -180. It is now unwrapped and washed out (`yaw_washout_s`):
  the turn in progress, returning to centre.
- **A crash** into a wall read -70 m/s^2 for a frame.
- **The fade** was linear, so the platform left rest at full speed.

`gcn-recomp/runtime/telemetry.cpp` now builds the output in four stages, each of which stops a
class of jerk on its own: a smoothstep ease to and from rest; a smoothstep crossfade across a
discontinuity; the yaw washout; and, last, after the gains, a clamp on every channel's range
and its rate of change (25 degrees at 60 degrees/s; 15 m/s^2 at 60 m/s^2 per second). The time
between packets is capped at 0.1 s, so a stall in the game cannot let a step through either.

Checked by running the scripted route: start from rest, a hard right turn, pause at 79 km/h,
continue, crash, then quit out to course select. On every channel the largest step between
packets was 1.8 degrees, and 5.8 m/s^2 on the accelerations. That is the rate limit applied
across the longest gap between packets the hidden desktop window produced. The pause, the
resume and the quit each eased over a second. At 60 fps (`WR_FPS_AT`) 3,000 race packets had
no false pause, and the largest steps were smaller still.

## The game's metre is 40 units, not 50

`units_per_metre 50` is the headset's scale, chosen by eye for how big the world looks (vr.md).
Using it for telemetry put the speed at 0.8 of the speedometer's. On the scripted Ocean City
Harbor run (diagnostics.md) the HUD read **91 km/h at presented frame 3200 and 98 at 3300**,
against 73 and 80 from 50 units to the metre: a ratio of 1.23-1.25 both times. So the physics'
metre is about 40 units (`kPhysicsUnitsPerMetre` in `motion.cpp`). With it, a later run
reported 84.5 km/h where the HUD read 87, mid-turn. The race clock confirms the time base:
3.3 s of race time per 100 presented frames, so 1/30 s per frame.

## Signs, checked on a turn

The scripted route with `3180:SR:45,3300:SL:45` added steers hard right, then left. Through the
right turn (frames 3200-3240): the heading rises from 0 to +70 degrees, roll goes to **+22
degrees** (right side down: the ski banks into the turn, which the dumped frame shows), sway is
**+19 to +23 m/s^2** (the centripetal acceleration, toward the inside) and the slide is -4.6 m/s
(drifting to the outside). Throttle from rest gives a surge of +10 to +15 m/s^2. Hitting the
wall gives a surge spike of about -70 m/s^2 for a frame or two. SimRacingStudio's own crash
handling is what limits that.

How SimRacingStudio scales each channel, and which way it expects each sign, is still to be
checked against its telemetry display. The profile it keeps for API games stores maxima of 10
for pitch, roll, surge and sway and 2 for heave (`metadata.db`, read as raw bytes), which reads
like degrees and m/s^2. Accelerations are therefore sent in m/s^2, not g. If that is wrong, or a
sign is backwards, `telemetry.txt`'s gains fix it without a rebuild.

## On the rig (2026-10-11)

First, flat with the rig off, to SimRacingStudio's port: SRS picked the game up from the v102
packet. That run's start showed four discontinuities in its first seconds, and between them a
heading of -25 degrees (the limit) at 3 km/h and a surge of +14 m/s^2 at 16 km/h, before
the speed fell back to zero. That would be a twist and a shove on the start line. Roll, yaw
and sway also reached their limits often in that run.

Then a whole championship in the headset (`--vr --telemetry`) on the rig, with the default
`telemetry.txt` values: "no weird jerks at all". Its log, 1,407 samples over six races, has
no heading beyond 10 degrees below 10 km/h, and yaw and roll never at their limits; sway
reached 15 m/s^2 in 23 samples. So the start in the flat run was not repeated. What caused
it is open. A guess not yet checked: that run passed through the attract demo before the
race, so the racer was found in the demo and lost and found again around it. If it comes
back, record a start (`saves/inputs`) and replay it to a listener on another port.

## Livelier: the `raw` blend

After the championship the motion felt "a little tame", and the request was to blend in some
of the motion from before the smoothing. The first answer, `raw` (0 to 1, with `raw_angles`
and `raw_accel` to set the halves apart), mixed the eye's eased, fp_tilt-scaled pitch and roll
and its eased heading toward the hull's own, and the eased acceleration toward one
differenced frame to frame with none. It was blended before telemetry.cpp's safeguards, so
the ease to rest, the crossfades and the limits applied to it as to everything else.

The scripted Ocean City Harbor route with three hard turns added, run at 0, 0.5 and 1 to a
listener (each run from its own directory, with its own `telemetry.txt` and a copy of
`saves/`). Mean step between packets during the race, first at 30 fps, then again at 60 once
60 became the race's default (a packet a game frame, so the steps there are about half):

| fps | raw | pitch | roll | surge | sway | heave | pitch rms |
|-----|-----|-------|------|-------|------|-------|-----------|
| 30  | 0   | 0.16  | 0.55 | 1.07  | 1.25 | 1.79  | 2.0       |
| 30  | 0.5 | 0.31  | 0.56 | 1.72  | 1.70 | 1.90  | 2.2       |
| 30  | 1   | 0.55  | 0.81 | 1.86  | 1.81 | 1.99  | 3.4       |
| 60  | 0   | 0.05  | 0.19 | 0.50  | 0.80 | 0.90  | 1.8       |
| 60  | 0.5 | 0.12  | 0.33 | 0.86  | 0.97 | 0.97  | 2.3       |
| 60  | 1   | 0.23  | 0.43 | 0.93  | 0.98 | 0.98  | 3.0       |

On those numbers it did what was asked: at 1, three to four times the pitch activity and
about twice the roll and surge, with pitch's swing 1.5-1.7 times as big (fp_tilt's 0.6 gone). The largest
step on any channel stayed under 2.2 in every run (1.8-2.1 at 60), which is the rate limit
(60 per second) across the longest gap between packets. With the raw accelerations the limit
is doing much of the work: their mean step is close to it, and at 60 fps sway and heave sit
on it from 0.5 up. That last point was the real problem, and it took a ride to see it.

Running the three at once did not work: each stopped sending after 30 s. Nor did `timeout`
stop the game under Git Bash (the native process outlives it); `taskkill` does.

### At 0.5 on the rig: better, but no pitch, no knocks, no waves

Ridden at `raw 0.5`: "better", but "almost no pitch action" going off a ramp and diving, a
collision "feels like nothing", and no vertical movement from the waves. The ride's input log
(`saves/inputs/20261011-222353`) was replayed with `--fast` to a listener, at `raw 1` with
every range and rate limit opened up, which gives the unsmoothed signal a game frame at a time
(the limits work in wall time, so `--fast` is only good with them off). It showed:

- **The pitch is there.** The hull pitches 30 degrees on a dive (12 up to -33 in 0.18 s, some
  300 degrees a second); p90 12 degrees, p99 25 in a race. At 0.5 with `angle_rate 60`, the
  dive reached the rig as a lean to -22 over a third of a second.
- **The unsmoothed accelerations are the chop**, not motion a platform can make: at 96 km/h
  the heave goes -76, -66, -19, ... 187, 158, ... -202 m/s^2 frame to frame, its
  autocorrelation gone in three frames (10-20 Hz). Median 25 m/s^2 for heave, 16 for sway.
- **So the rate limit was the smoothing.** Modelled on that ride, the accelerations sent at 0.5
  sat at `accel_rate` 80-90% of the frames (heave 90%), a triangle wave slewing at 60 m/s^2 a
  second, and even at `raw 0` 62% (surge) and 88% (heave). The six biggest collisions reached
  the rig as -4.9, -9.3, -8.5, -10.2, +2.7 and +0.1 m/s^2: two the wrong way. Whatever a
  platform makes of a signal held at its slew limit, it is not a knock or a wave.
- **The impacts stand clear of it.** With only the velocity's 0.05 s easing, collisions read
  -200 to -340 m/s^2 and landings +160 to +220, where the chop is 20-40.

What replaced the blend (`motion.cpp`): the accelerations are never blended toward the
unsmoothed ones; heave is eased again over `heave_s`; and an impact is the fast acceleration's
excess over the eased one beyond `impact_threshold` (50), times `impact` (0.25), held at its
peak and released over `impact_s` (0.2 s), and only from 0.5 s after the track restarts, while
the eased value is still rising from zero. `raw` blends the angles only, and is 1 by default.
In the shared module the rates went up to `angle_rate 120` and `accel_rate 300`, with yaw
split off as `yaw_rate` and left at 60: the unexplained start in the flat run above was a
heading swing. The same model gave, per channel, the share of race frames at the rate limit and
the six collisions' lows:

| | surge at limit | heave at limit | collisions (m/s^2) |
|---|---|---|---|
| `raw 0.5`, rate 60 | 80% | 90% | -4.9 -9.3 -8.5 -10.2 +2.7 +0.1 |
| impacts, rate 300 | 1% | 2% | -15 (all six, the range limit) |

Pitch at `raw 1` and 120 degrees a second is at its limit 1% of the frames. Then on the rig,
at `raw 1` with these defaults, about 20 minutes of races: "amazing, much better". Its input
log is `saves/inputs/20261011-225452`.

## Testing without moving the rig

SimRacingStudio holds UDP 33001 while it runs, and a packet it accepts may move the platform.
So the checks above sent to another port (`--telemetry=127.0.0.1:33011`) and decoded each
packet with SRS's own format string. Anything sent to 33001 has to wait for someone at the rig.
