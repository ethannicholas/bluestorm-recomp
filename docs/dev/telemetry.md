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

- **pitch, roll** are the eye's eased angles, already scaled by `fp_tilt`. Roll is negated:
  the eye's is positive with the right side up, the telemetry's with the right side down.
- **yaw** is the eye's eased heading, negated so that it rises turning right.
- **velocity** is the eye's position differenced per game frame (`frame_dt`) and eased over
  `ski_velocity_s` (0.05 s); **acceleration** is that differenced again and eased over
  `ski_accel_s` (0.15 s, the eye's own `fp_tilt_s`). The position is the hull's exactly, so
  this second easing is the only smoothing the accelerations get. At 0.1 s the heave was
  still jumpy at 30 Hz.
- **surge** and **sway** are that acceleration along and across the eye's heading, **heave** is
  its world-vertical part (no gravity), and the **lateral velocity** is the slide.

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

## Testing without moving the rig

SimRacingStudio holds UDP 33001 while it runs, and a packet it accepts may move the platform.
So the checks above sent to another port (`--telemetry=127.0.0.1:33011`) and decoded each
packet with SRS's own format string. Anything sent to 33001 has to wait for someone at the rig.
