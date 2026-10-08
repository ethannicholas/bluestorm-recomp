# VR notes: the OpenXR app, theater and stereo

Everything learned building the headset frontend: how the app is put together, how the view
switches between the flat theater panel and per-eye stereo, how the game's frame is split for the
eyes, where the HUD goes, and the investigations behind each of those answers, including the ones
that were wrong first. Read the relevant section before changing anything in
`runtime/openxr_main.cpp`, `runtime/egl_main.cpp` or the eye paths in `runtime/gx/render_gl.cpp`.

The user-facing instructions (building, installing, controls, `vr.txt`) are in the top-level
README. The headless harness used throughout (`waverace_egl --eye`) is described in
[diagnostics.md](diagnostics.md).

## The OpenXR app

The Android app is an **immersive OpenXR** app, not a 2D panel: it takes over the display and
owns the controllers. A panel in the home environment cannot capture input at all (the A button
goes to the shell), which is why this is immersive even though it only shows a flat screen.

Theater mode is built on `XrCompositionLayerQuad`: the compositor is handed one flat texture and
places it in space, reprojecting at display rate. Head tracking therefore stays smooth however
slowly the game renders. Measured on a Quest 3: **72 Hz compositor with no stale frames while the
game fed it 30 fps**. Full 3D later replaces the quad with a projection layer and per-eye
`u_proj`/`u_view`; the session, swapchain and input code are unchanged by that.

The swapchain is touched **only when the game has produced a frame**, not once per display frame.
A quad layer keeps showing the last image released to it — that is the same property the
re-submission of the layer on non-rendered frames relies on — so re-blitting identical pixels into
a freshly acquired image at 72 Hz bought nothing, cost a full-screen blit every display frame, and
meant cycling the swapchain underneath the compositor while it was sampling. Removing it took the
game from ~187 to ~219 frames per five seconds with the compositor still at a full 72 Hz.

The OpenXR loader comes from Khronos' official Android AAR on Maven Central, fetched at configure
time, so no binary is committed. Because there is no Gradle and so no manifest merger, the
loader's own manifest requirements (two permissions and a `<queries>` block for the runtime
broker) are written out by hand in `android/AndroidManifest.xml`; without them the loader cannot
see the runtime on Android 11+ and `xrCreateInstance` fails.

## Theater and stereo

The app presents the game two ways and switches between them automatically:

- **Theater** — the frame on a flat screen in space, as an `XrCompositionLayerQuad`.
- **Stereo** — the world rendered per eye as an `XrCompositionLayerProjection`, with the 2D
  elements painted on a frame standing in front of the game's camera.

The switch is driven by the game's own state and by what it draws. `0x80602160` counts what the
course loaded and is the only signal that clears when a race is *quit*, so it gates. Within it,
stereo comes up when the **starting-light rig** is on screen (`gx::batch_shows_start_rig`), and
`0x806193BC`, the course's wave height -- non-zero from the lights to the finish -- carries it
through the race and is what notices a race *finishing*. Stereo is
`on_course > 0 && (wave_height || rig on screen)`.

Finding the rig. Every course has its own -- vines on Lost Temple Lagoon, bamboo on Southern
Island, a wooden frame on Aspen Lake, a scoreboard on Ocean City Harbor, another on Dolphin Park --
so the test must not depend on the model. What they share is how the game draws them: in
perspective through an identity position matrix, hung in view space in front of the camera
(`PixelState::view_space`). That is not the only thing drawn that way -- Championship's opening
screen and every course flyover draw the whole course through the identity too, transformed on
the CPU -- but nothing else so drawn stays near the camera. `WR_RIGLOG=1` in `waverace_egl` logs
those draws per frame, and the detector's verdict when it changes; on all five reachable courses:

| on screen | draws | vertices | extent |
|---|---|---|---|
| the rigs | 10-33 | 1,686-2,898 | no more than 100 units across or 210 from the camera |
| opening screen, flyovers | 150-600 | 29,000-85,000 | 25,000-85,000 units |

and in a race, the rider close-ups, the results and the menus there are none at all. So the rig
is a frame whose identity-matrix perspective draws all lie within 2,000 units of the camera (ten
times the farthest rig, a tenth of the nearest course) and add up to at least 100 vertices
(enough to ignore a stray quad). On every course it fired only while the rig was up: on the same
frame as the wave height in Time Attack, and 45 frames before it on Dolphin Park. Courses this
save has not unlocked are untested.

Two variables were tried for the start and failed:

- `0x80625A54`, the start sequence's phase, reads 12 during the intro and 5 from the lights to
  just after the start. But 5 is not only the countdown. Championship -> Exhibition -> Dolphin
  Park opens on a screen that cycles views of the course until A is pressed, and five seconds in
  the phase goes to 5 and stays there (traced with `WR_WATCH=0x80602160,0x80625A54,0x806193BC`,
  no input after picking the course) -- the headset switched to stereo on it every time. In Time
  Attack the phase never leaves 0.
- The wave height alone starts stereo up to a second and a half into the countdown, with the
  lights already lit.
There is no manual override any more: the right thumbstick click used to pin the view, and now
switches between the chase camera and [first person](#first-person) within stereo.

A switch either way is a morph rather than a cut, `transition_s` long (1 s by default). Theater
is reachable from the stereo renderer: run every draw through the chain the HUD already uses —
the game's own projection onto a flat rectangle, seen from each eye — with the rectangle where
theater hangs its panel, and each eye sees the game's frame on that panel. So the eyes are drawn
with every vertex blended between that point and its real one (`render_set_vr_morph`), and the
picture opens out of the panel into the world, or folds back onto it. Details that matter:

- The blend is of homogeneous points, one matrix per draw, so the GPU's clipping still works on
  the panel side: a vertex behind the game's camera stays behind it. Weighted as it is, it is
  interpolation in 1/depth — each vertex's disparity grows evenly, rather than distant scenery
  waiting on the panel and leaving all at once.
- A crop, as clip distances, cuts the scene to the game's frustum at the start and opens out from
  it; outside it is theater's black, coming up to the scene's clear colour. On ES this needs
  `GL_EXT_clip_cull_distance`; without it the morph runs uncropped and says so once.
- Exactly flat is degenerate — every vertex on one plane, with nothing for the depth test to sort
  by — so the morph never goes below 0.002, which already matches the flat frame.
- Only the eyes draw during a morph, and only when the game delivers a frame, so it advances at
  the game's rate. The panel comes back once it has a frame of its own; until then the eyes'
  last, nearly flat image stays up.

`WR_EYE_MORPH=0,0.5,1` makes `waverace_egl --eye` dump every dumped frame at each value, plus
the flat frame beside it, which is how the morph is checked without a headset: at 0 the middle of
the eye image is the flat frame, and nothing else.

The sections
below are the working: how they were found, and why the earlier answers were wrong.

They replaced counting a frame's perspective draws, which was wrong in both directions: the course
overview is a full 3D flyover and cleared the threshold, so stereo began before the race, and a
sparse view during a race dipped below it, so the view flapped. **There is no fallback to that
heuristic.** But an unexpected value is no longer fatal: it presents the flat view and says so
once in the log. An earlier build aborted instead, on the belief that `0x80602160` was a flag that
could only read 0 or 1, and that belief was wrong — see
[When a count was mistaken for a flag](#when-a-count-was-mistaken-for-a-flag).

### Finding it, and why the first two answers were wrong

`WR_RAMSNAP=<dir>` makes `waverace_egl` write the low 8 MB of guest RAM beside a PNG every
`WR_RAMSNAP_EVERY` frames; the PNGs say which snapshot is a menu, the overview, a race or the
results. `WR_RAMSNAP_RANGE=a-b` bounds it to a window of frames: a transition that takes a second
needs a snapshot every few frames to bracket, and a whole run at that interval is gigabytes, while
the states either side of one boundary are all a diff for that boundary needs. Asking for words holding one value across every racing snapshot and a different single
value across every non-racing one is then a few lines of numpy.

Sampled every 150 frames that gave `0x80631FA4`, apparently a perfect 0/1 race flag. It is not:
traced every frame with `WR_WATCH=<addr,...>` it drops to 0 for ten frames in every seventy-four,
which would have flapped the view exactly as the draw count did. **Snapshots can only disprove a
flag, never confirm one** — a blink shorter than the sampling interval is invisible, and seven
samples landing on the steady phase of an 86%-duty signal is a coin toss, not evidence.

Re-sampling every 25 frames left three candidates, and tracing those every frame killed two more:
one dropped out mid-race, one never cleared. A fourth candidate from the first pass, `0x80619434`,
survived the on/off test but turned out to be a float that is merely never exactly zero during a
race. Only `0x806193BC` has exactly two transitions, and its boundaries line up with the countdown
appearing and the race ending in dumped frames either side — which is exactly as far as a session
that always runs a race to the finish can take you. It is the wrong variable, and the next section
is how that came out.

### Why the wave height was the wrong variable, twice

`0x806193BC` got the view wrong at both ends of a race.

It comes up only as the countdown reaches the line, about a second *after* the starting lights are
already on screen — dumped frames put the light rig in view at 2278 and the flag at 2312 — so a race
began flat in the headset and popped into 3D once it was already under way. And it is never cleared
on the quit path: traced every frame through a retire to the main menu it went to 3.0 at 2312 and
was *still* 3.0 on the title screen 2,300 frames later, which left the headset in stereo over the
menus until another race started. It is the course's wave height, and nothing resets it when the
course is abandoned.

Two candidates for the earlier moment were rejected along the way, and both are worth recording.
Counting the frame's **view-space perspective draws** looked ideal, the countdown rig being the
game's one piece of view-space 3D — but traced over a session those draws run 2301–2381, *after*
the lights appear, so they are something else. The **object block at `0x80631F30`** springs into
existence at the right moment but is the slot caught blinking mid-race above: a recycled allocation,
not a state. `0x80625A54`, the start sequence's own state, covers the lights but goes out at 2401,
mid-race.

`0x80602160` is the flag that is actually wanted, and it replaces both. Traced every frame it has
**exactly two transitions** in a 6,663-frame session that races to the finish (1 at 1996, 0 at 3797)
and exactly two in a 5,837-frame session that retires to the main menu (1 at 1996, 0 at 3539,
against a quit confirmed at 3520). It is zero through boot, the menus, course select, loading, the
results screen and the title screen. That made it look like a flag; it is a count, and the
difference mattered — see [When a count was mistaken for a flag](#when-a-count-was-mistaken-for-a-flag).

No single variable spans a race at both ends, so three are used, one job each.

`0x80602160` gates everything, being the only one that clears when a race is *quit*. On its own it
comes up as the course intro begins, which put the headset into stereo the moment the track
finished loading — the intro is a full 3D flyover and the flag is up for it — and it stays up for
another 170 frames past the chequered flag, through the results. So within it, the start sequence's
phase at `0x80625A54` brings stereo up with the starting lights (value 5, from 2287), and the wave
height at `0x806193BC` carries it from there to the finish: `on_course && (race || countdown)`.
That switches at 2288 and back at 3629, against 1997 and 3798 for the flag alone.

Using `0x80602160` by itself was wrong in a way worth recording, because the test that missed it is
an easy one to repeat: every trace behind it ended by *quitting*, so the 170-frame tail past a
finish never showed up, and a finished race sat in stereo over the results screen.

The rig is still visible for about half a second before the switch: the lights come into view at
2278 and `0x80625A54` leaves the intro phase at 2287, with a frame of debounce on top. Closing that
last gap means a signal that fires with the lights rather than just after them — `0x806263C4` goes
0 to 1 at exactly 2278, but it latches at 2 afterwards and never resets, so it cannot be used as it
stands. Worth picking up if the half second starts to grate.

Being a plain flag, anything but 0 or 1 is fatal rather than guessed at — the same stance as before,
for the same reason: being wrong does not degrade gracefully.

The flag is written by the guest thread and read by the frame loop, so a sample can land on a
transient. Reading the old race flag from that side caught it non-zero for a single frame twice
before a race and three times on the results screen, each of which would have flashed stereo over a
menu, so the switch wants one game frame of agreement before it acts.

Stereo is cheap here because of where `xf.cpp` stops. Vertices reach the renderer in the game's
*view* space with the projection applied in the shader, so an eye is just another matrix in front
of it: both eyes share one vertex buffer, one CPU-side transform and one set of render-to-texture
results, and only the uniforms and draw calls repeat.

### When a count was mistaken for a flag

On 2026-10-06 the app aborted about six minutes into a session in the headset. It was not the
system reclaiming memory: peak RSS was 521 MB, `Killed-By-AM: No`, and the tombstone's backtrace
goes through our own `fatal()`. `llvm-addr2line` on `android_main+4888` against the crashing
build's BuildId named the line exactly — the guard that refused to run when `0x80602160` read
anything but 0 or 1.

**The value that tripped it was gone.** `fatal()` wrote only to stderr, which on Android is piped
into logcat, and logcat is a 256 KiB ring; `log_frames 1` writes a line per display frame and laps
it in well under a minute. The one diagnostic the guard existed to produce had been overwritten by
our own tracing. `fatal()` now also calls `android_set_abort_message`, which puts the text in the
tombstone and in the `crash` buffer, neither of which the app's own logging can flush.

So the value had to come from the code instead, and it is worth recording how, because the same
route answers "what can this address actually hold" for any of them.

`analysis/symbols.txt` gives the first surprise:

```
lbl_80602140 = .bss:0x80602140; // type:object size:0xF00
```

It is not a variable. It is field `+0x20` of a 3840-byte object, whose first `0x24` bytes are
`memset` as a unit — the generated code calls `fn_80003320(0x80602140, 0, 0x24)` — with a pointer
at `+0x18` that is freed when non-null. Identically shaped descriptors sit at `0x80632ED8` and
`0x80632F68`.

A `WR_WATCH` build then named the two instructions that touch it across a whole race. That build is
not one of the presets — `WR_WATCH` has to reach the generated C as well as the runtime, since the
hook sits on every store the recompiler emits:

```
cmake -S . -B build-android-watch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DWR_BENCH_ONLY=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS=-DWR_WATCH -DCMAKE_CXX_FLAGS=-DWR_WATCH
ninja -C build-android-watch waverace_egl
```

Run it with `WR_WATCH_ADDR=<hex address>` and it reports every change to that word, with the guest
PC of the instruction responsible:

```
[watch] 80602160: 00000000 -> 00000001 at store 8004561C
[watch] 80602160: 00000001 -> 00000000 at store 800033E8   (inside the memset)
```

And `0x8004561C` is `907E0020`, `stw r3,0x20(r30)`, immediately after `bl fn_80047320`. So the
field is that function's return value, which is either `r24` — zeroed at `0x80047348`, incremented
once per entry at `0x800473CC`, and used at `0x80047404` to size an allocation of sixteen bytes
apiece — or `-1` from the error path at `0x80047578`.

**It is a count.** Every session measured while the switch was being worked out counted exactly one
entry, so it read 0 and 1, and two sessions of that was enough to convince me it was a flag and to
write an abort around the belief. A session that counted two, or that took the error path, was
always going to end the way this one did. The crash came on the first session with a *persisting*
memory card — the save was written a minute before — which is exactly the kind of thing that sends
a game down a path no cold-boot trace covered.

The reading is now `on_course > 0`, signed, so a failed load is off-course rather than on. The
guard is gone. Being wrong about which view to present does not degrade gracefully, which is why it
was there, but aborting in a headset degrades worse, and a value this build has not seen before is
not evidence of the wrong disc — the build already verifies the disc by hash.

### How many pixels the theater panel gets

Leaving a race used to be the worst-looking moment in the app: stereo draws each eye at the
headset's own 1680×1760, and theater dropped straight back to a 640×480 frame stretched across a
panel 65 degrees wide. Two separate limits were doing that, and both had to move.

The **panel's own texel count** was a fixed 1024×768. What it should be follows from its angular
size: it hangs 2.5 m out and is 3.2 m across, so it covers `2*atan(1.6/2.5)` = 65 degrees of the
viewer's horizontal field, and an eye swapchain spans that whole field — call it a hundred degrees
in front of one eye. Two thirds of an eye's width is then one texel per display pixel, and the
panel is given a little over that, because the compositor samples it at whatever angle the head is
holding and texels that align with pixels nowhere are better filtered down than invented. Derived
from `recommendedImageRectWidth` rather than fixed, so a denser headset gets a denser panel: a
Quest 3 gives 1260×945.

The **EFB the panel is blitted from** was the GameCube's own 640×528, so every one of those texels
was interpolated from something smaller. `theater_scale` in `vr.txt` is how many samples it keeps
per hardware pixel per axis. It is separate from `stereo_scale` because the EFB means different
things in the two views: in theater it *is* the picture, while in stereo the eyes are drawn at the
headset's resolution and the EFB is only scratch space holding what they sample out of it — the
water reflection, the sheet the spray is cut from — filled by a third full scene render that
already costs more than it returns. So the scale follows the view, which means re-allocating the
EFB when the view changes. Nothing in it has to survive that: it is cleared and redrawn every
frame. The textures EFB copies land in are another matter — they are allocated at `dst * scale`
but matched for reuse by their logical size alone, so a survivor would be silently reused at the
old resolution. They are dropped with it.

It costs almost nothing, because this path is bound by the recompiled guest code and not by fill
rate. Measured on a Quest 3 with `waverace_egl`, 150 s from boot into a race, comparing the same
550-frame stretch of mid-race in every run:

| `theater_scale` | EFB | mid-race fps | frames in 150 s |
|---|---|---|---|
| 1 | 640×528 | 26.93 | 3638 |
| 2 | 1280×1056 | 26.78 | 3570 |
| 3 | 1920×1584 | 27.08 | 3560 |
| 4 | 2560×2112 | 26.75 | 3567 |

Mid-race the four are within 0.7% of each other, which is run-to-run noise; across the whole run 2,
3 and 4 are indistinguishable, so the 2% between 1 and the rest is a fixed step and not something
that scales with area. The default is 3, which puts 1920 samples behind a 1260-texel panel —
enough to antialias it rather than merely fill it — and leaves a step of headroom for a headset
that has been warm for an hour, which a 150 s run does not measure.

That the frame rate does not move at sixteen times the pixels is also the clearest evidence yet for
the first open issue in [performance.md](performance.md): whatever is keeping this below 30 fps, it is not the pixel pipeline.

### How many pixels the eyes get

The eyes used to render at exactly what the runtime recommends, 1680×1760 on a Quest 3, with no
antialiasing. That is less than the panel's own 2064×2208, and the lens magnifies the middle of
the image further, so stereo looked soft and stair-stepped. They now render at `eye_scale` times
the recommendation (1.4 by default: 2352×2464) with `msaa` samples (4 by default).

The antialiasing is `GL_EXT_multisampled_render_to_texture` (`runtime/gx/gl_msrtt.h`). On a
tiled GPU the samples live only in tile memory and are averaged as each tile is written out, so
the swapchain image stays single-sampled and the cost is mostly extra shading at edges. The water
and spray read the eye's own image back part way through a frame, and that still works: the
read-back sees the resolved image, and the frames show the same water, reflection and depth order
with and without it.

`waverace_egl --eye` measures this without a headset: `--eyes=2` renders both eyes as the headset
does, `--eye-size=WxH` and `--msaa=N` set the target, and `WR_EYE_GPU=1` reports the eye passes'
GPU time from timer queries every 2 s. On a Quest 3, mid-race on the scripted route:

| eyes | GPU per game frame | render-thread time for the eyes | lowest fps |
|---|---|---|---|
| 1680×1760, no AA | 5.8 ms | 9.8 ms | 29.5 |
| 2016×2112, 4× MSAA | 10.8 ms | 8.8 ms | 29.5 |
| 2352×2464, 4× MSAA | 11.7 ms | 9.1 ms | 29.5 |

A game frame is 33 ms, so this leaves the GPU most of it; what the table cannot show is the
compositor's own share, or a headset that has been warm for an hour. 1.2 is the step down if
either turns out to matter.

The render-thread column depends on a fix made along the way. The renderer refilled one vertex
buffer every frame, and the driver made that wait until the GPU had finished the previous
frame's eyes, so GPU time leaked into CPU time. At 1.4× with 4× MSAA the flat pass sat at 27 ms
a race frame, nearly all of it in `glBufferData`, and the frame rate fell to 25. The vertex and
index buffers are now a ring of three (`g_vaos` in `render_gl.cpp`), which took that to 4.9 ms.

Measuring it also turned up a fault in the harness: it never called `glFlush`, which the headset
app does after every eye, so the driver queued whole frames up and the next frame's upload waited
for them to drain. With no flush, even the old eye size showed a 17–21 ms flat pass; with one, 6.
It now flushes after every frame in both modes. The `theater_scale` table above was measured
before that, so its absolute frame rates are likely low; the comparison between its rows is
unaffected.

### Splitting a frame for the eyes

A frame is not one pass. A race frame holds about a thousand draws and five EFB copies: three
off-screen passes (a 480×480 reflection, a 128×128, a 320×240), then the main scene, then a
640×480 copy of the finished image, then the display copy. Only the main scene may be re-projected
per eye — the off-screen passes produce *textures* the scene samples, and re-aiming those at an eye
corrupts them.

Two things make this work:

- **The first eye runs the whole frame flat into the EFB first**, scanout aside. An eye pass never
  draws into the EFB, so without this every render-to-texture copy reads an empty one. That is
  what left the ski untextured and put a black square on the water.
- **A pass is skipped when its copy *clears* the EFB**, wherever that pass sits. The clear is what
  marks a real off-screen pass: the next pass needs the buffer empty. A copy that does not clear
  is a *grab* — the game lifting a piece of the live scene to texture with — and the draws in
  front of it belong to whatever pass is still in progress.

  Both halves of that matter, and each was learned the hard way. The off-screen passes are not all
  up front: at speed the spray grabs some fifty 32×32 and 64×64 rects from scattered screen
  positions *after* the main scene, so "the scene is whatever follows the last copy" drops the
  entire scene the moment you get going. And treating every copy as ending a pass then throws away
  whatever draws sit in front of the first grab — the water surface among them, so the ocean
  vanished at speed instead.

  The copy of the whole displayed frame is never an off-screen pass. "Whole frame" is measured
  against the display copy in the same batch, since the frame is 640×480 while the EFB is 640×528;
  measuring against the EFB matches nothing.

Draws that sample a whole-frame copy are screen-space — in this game the water surface is one,
refracting by looking the finished frame up at the screen position the game computed for each of
its vertices. Those positions belong to the flat view, and so does the copy, so an eye re-projecting
them sampled off the edge of the copy wherever it could see water beyond the game's own 60-degree
frustum. Clamping smeared the scene down the sea in streaks — the start banner, the shoreline and
the racer among them, painted across the water.

No amount of moving the lookup around fixes that, because the pixels the eye needs are not in the
copy. The eye grabs what it has drawn itself instead, which covers exactly what it can see, and the
fragment samples that at its own position rather than at the flat view's. The grab is taken at the
first draw that wants the finished frame, by which point the scene behind the water is in the target
and the water is not; the indirect stage that ripples the lookup still applies on top, so the water
keeps its wobble. It costs one full-target copy per eye on frames that have such a draw.

The coordinate swap is a uniform rather than a shader variant, so the flat path is untouched: it
keeps the coordinate the game computed, which is right there. Dropping these draws instead
(`WR_EYE_SKIPCOMP=1`) leaves the seabed showing through bare sand, and sending them down the overlay
path puts the ocean on the HUD frame, racer and all.

What the eye is *cleared* to is part of that same lookup. The water refracts whatever stands
behind it, and out past the point where a course gives the sea a bottom that is nothing at all --
just the colour the buffer was cleared to, which in a race is a blue-grey the game sets with the
copy ahead of the main scene (`WR_EYELOG=1` prints it). An eye cleared to black turned the open
sea black in stereo while the flat view, cleared to the game's own colour, showed water. On
Southern Island that took most of the distance with it, and it was neither the geometry nor the
projection: it survived the game's own frustum (`WR_EYE_GAMEPROJ=1`) and it survived fog being
switched off. So the eye takes its clear from the batch -- the last copy to clear the colour
buffer ahead of the first draw the eye replays.

Fog is the other thing an eye cannot read off the hardware's depth. GX fogs on the 24-bit screen
z it writes, and the generated shader recovered that from `gl_FragCoord.z` -- which in an eye is
the headset's frustum, 0.1 m to 2000 m, nothing like the game's. Handed to a curve calibrated for
the game's near and far it saturates within a few metres, so everything past that came back the
fog colour: a black band across the sea bed on Dolphin Park, and part of the darkness on Southern
Island. The game's own screen z is rebuilt in the vertex shader instead, from the z row of its
projection (`u_zproj`) and the GX viewport's z scale, and handed to the fragment as a numerator
and a w to divide by -- both affine in the vertex position, so perspective-correct interpolation
delivers the exact value where a single interpolated z/w would not. The depth fed in is the eye's
own and not the game camera's: fog is a distance cue, and the distance that matters to a viewer
who has turned their head is the one along their own line of sight, where the game's z would fog
something off to the side as though it were only as far away as its forward depth.
`WR_EYE_FOGZ=0` takes the game's depth instead, which fogs exactly as the flat view does whatever
the head is doing.

The spray grabs are the same mechanism one size down — partial EFB copies replayed as billboards at
coordinates belonging to the flat view — and show as faint squares with pieces of scene inside them.
They get the same substitution. A copy that neither scans out nor clears and is no bigger than
64×64 is one of the spray's: the clear flag is what separates a grab from an off-screen pass, and
the size keeps the other partial copies (the water reflection, the rect the submerged tint samples)
out. Those ids cannot be collected in a set the way the whole-frame ones are, because the spray
copies to addresses that rotate and a set would grow by fifty entries a frame, so the flag rides on
the texture instead.

Measured on the device, a frame at speed holds 30–65 copies, of which 33 or so are grabs; the
other partial copies in such a frame (480×480, 128×128, 320×240) all clear, so the clear flag
alone separates them and the size test is only belt and braces. A droplet draw is one TEV stage
with one indirect stage, a static indirect matrix and no wrap — `WR_EYELOG=1` prints that
configuration for the first substituted draw of a frame, which is what settled the next paragraph.

Two things differ from the water. The spray composites itself over the *finished* scene, water
included, while the water's own grab is deliberately taken before the water is drawn — so there are
two grabs, not one shared, or substituting the spray would quietly re-point the water's lookup, and
the game's final composite with it, at a later moment. And what survives of the game's own lookup is
just the indirect offset that distorts it, which is an absolute displacement in the copy's texels.
Those texels are EFB pixels; an eye's are not, since it sees a wider field across more pixels, so
the offset is converted by the ratio of the two grids' pixels per unit of frustum tangent, and
negated in y (an EFB copy's row 0 is the top of its source rect, an eye grab's is the bottom).
Without that conversion a droplet keeps a displacement of a few pixels on a target several times the
EFB's width, a small fraction of the distortion asked for: it degenerates into a near-exact copy of
its own background, so the spray disappears instead of reading wrongly. `WR_EYE_RIPPLE=0` leaves the
offset unconverted, which is what the water shipped with, and separates "the lookup is in the wrong
space" from "the distortion is the wrong size".

One trap, worth the paragraph because the first attempt fell straight into it and the symptom
looked nothing like the cause. The substitution originally replaced the texgen's coordinate
outright, at the point where `uv0` is computed. But an indirect stage samples its bump map through
the *same* texgen, and that map is an ordinary texture that wants the game's coordinate — addressed
by the fragment's position instead, a ripple meant to span one 32×32 droplet is stretched across
the whole eye. The squares were duly gone and a striped ladder stood over the ski in their place.
So the substituted coordinate is declared beside the game's as `suv<n>` and only the stage that
reads the copy picks it up; the indirect lookup keeps `uv<n>`. Shaders with no EFB-copy texmap
generate exactly as before either way.

Verified on the device through the `--eye` harness in [diagnostics.md](diagnostics.md): at 103 km/h the rectangles over the HUD,
the turbo bar and the water are gone, and the ski and its spray read as themselves. `WR_EYE_SPRAY=0`
brings them back in the same run for comparison.

`--eye-yaw=N` turns the head N degrees. With the view left at identity nothing in the image can be
seen to be head-locked, and a change that pinned the ocean and a copy of the racer to the viewer's
face went through this harness looking perfectly correct. Dump a frame at two yaws: whatever does
not move is locked to the head, and nothing in this game should be — not even the HUD, which is
anchored to the game's camera rather than to the viewer.

`WR_DUMP_RANGE=a-b` narrows dumping to a window of frames in both the flat and the eye path, so a
short stretch can be caught every few frames without writing a gigabyte — anything that is only on
screen for three seconds, the countdown among them, is missed by any interval coarse enough to run
a whole race with.

`WR_EYELOG=1` prints each frame's split — every copy with its size, source rect, clear flag, the
draws ahead of it and whether they were replayed — and is the quickest way to tell "the eye
rendered the wrong part" from "the eye rendered nothing". `WR_DUMP_COPIES=20` dumps whenever a
frame holds at least that many copies, which is how a frame thick with spray gets caught: the
faults that only appear at speed are in exactly those frames, and a fixed dump interval almost
never lands on one.

### Where the HUD goes

The game's 2D elements — the race HUD, the mode panel over the course flyover, the results screen
— reach the renderer as orthographic batches: an identity position matrix, a 640×480 ortho
projection, vertices that are screen coordinates. In stereo they are painted on a **frame**: a quad
standing a fixed distance in front of the game's camera with the game's own 2D frame mapped onto it
corner to corner. Four `vr.txt` keys place it: `hud_distance_m` and `hud_scale` for how far out it
stands and how much of the field of view it fills, `hud_height_m` and `hud_pitch_deg` for how high
its centre sits off the forward axis and how far its top leans.

The frame is square to the room, with `hud_pitch_deg` at 0, and that is only right because the sea
is levelled first — see [Levelling the sea](#levelling-the-sea). Before it was, a frame square to
the room read as leaning back against a world that was tilted, and it had to be leaned 23.2 degrees
to look upright. `hud_height_m -0.36` hangs it 5.2 degrees under the horizon, which is where it was
tuned to sit while wearing it.

The forward axis is eye level only if the runtime fixed its `LOCAL` space while the headset was being
worn. Fix it with the headset on a desk — easy to do on a device that is usually driven over adb —
and the frame hangs wherever the desk was, with nothing in its own geometry wrong. `log_frames 1`
prints the head's height and pitch alongside the frame's placement once a second, which is the way
to tell those apart from inside the headset, where they look the same.

Not everything the game places in view space belongs on that frame, though. A draw with a
*perspective* frustum placed in view space is a 3D object held in front of the camera rather than a
2D overlay — the countdown light rig is the only one in this game. The frame's Z column is zero, so
every vertex it carries lands on one plane, which throws away such an object's own depth: the rig
lost the occlusion that hides each lamp behind its lens and the lamps showed through as white
squares on the glass. Those draws keep the eye's projection and stay 3D, and they take `g_vr_view`
rather than `g_vr_view_world`, because the world pitch is taken out of the *world* to level the sea
and applying it to something attached to the camera is what stood the rig over at 23 degrees.
`WR_EYE_VS3D=0` puts them back on the frame. Across 4,871 frames the only view-space perspective
geometry is the rig, so this reaches nothing else.

Writing those elements straight into each eye's NDC — what this replaced — cannot work, and the
reason is worth keeping. The headset's per-eye frustums are **asymmetric**, so one NDC position is
a different direction in each eye. There is no depth at which the two images agree, so the HUD
never fuses into one; it reads as two overlapping HUDs at whatever distance the eyes give up and
settle on, which is far too close to focus on comfortably.

Three details the frame depends on:

- **The chain starts at the game's own clip space, not after GX's viewport transform.** That
  transform places the frame inside the 640×528 EFB, and an eye's render target is not the EFB —
  the same reason the scissor rect is dropped in an eye. Carrying it in maps the whole EFB instead
  of the 480 lines the game displays, which leaves the HUD a few per cent small and off centre.
- **Both eyes are given the same frame.** It is built once per frame, outside the per-eye loop,
  from the wider half-field of the two eyes. Sizing it per eye would hand the viewer two different
  HUDs to fuse, which is the original fault in a subtler form.
- **The depth test goes off for these draws.** Every element of the HUD lands on one plane, so the
  game's own depth state no longer orders them, and the scene is mostly nearer than the frame. The
  HUD is submitted last, so submission order is the layering.

The frame is anchored to the game's camera, not to the head. It frames the race while the viewer
looks forward and stays where it is when they turn to look at something else — which is what
`--eye-yaw` above checks: under a yaw the HUD swings with the scene instead of sitting still.

One thing on the frame is not 2D. The countdown light rig is a 3D model, and the game places it in
*view space*: its position matrix is the identity and a translation of (0, 30, -160), so it hangs a
fixed distance in front of the camera and never moves with the course. Re-projected into an eye as
world geometry it became a solid object standing in the water between the viewer and the racer, so
it goes on the frame with the rest of the HUD — the frame's matrix carries clip `w` through, so a
perspective batch lands on the plane the same way an orthographic one does.

An identity position matrix is what tells those draws from the scene, and it is a sharp test rather
than a threshold: GX position matrices carry the modelview, so world geometry can never have one.
Two frames dumped with `WR_MTXLOG`, one during the countdown and one mid-race, say so exactly —
the countdown frame has 327 perspective draws with an identity matrix, all of them the rig, in one
contiguous block just before the 2D overlay, and the mid-race frame has **none at all**. Only the
first vertex's matrix is tested, which no draw in this game disagrees with.

A mono dump cannot show the thing that matters here — whether the HUD fuses — so the frame was
checked by arithmetic instead, against the rig, whose view-space position is fixed and therefore
identical in every frame of every run. Moving it onto the frame predicts a uniform 0.9527× shrink
towards the centre of the image (the game's 60° vertical field against the harness's 90°, times
`hud_scale`), and the lamp centres moved by 0.956×. Yawing the head 20° predicts them at
(296.8, 258.1) and (348.7, 263.2) in a 960×720 dump, and they landed within a quarter of a pixel
of both. Against a flat capture of the same moment, the lamps land within two pixels of the flat
frame's own layout scaled by `hud_scale`. The three numbers between them say the frame carries the
game's layout faithfully, sits where the geometry says it should, and is anchored in the world
rather than to the head. Whether four metres is a comfortable place to read it from is still a
question only the headset can answer.

## First person

In stereo, clicking the right thumbstick moves the eye from the game's chase camera to the rider's
seat: a point on the ski (`fp_x`/`fp_y`/`fp_z` in `vr.txt`, game units in the ski's own frame,
x right, y up, z forward), with the rider not drawn. The chase camera's pitch correction
(`world_pitch_deg`) does not apply here. `render_set_first_person` turns it on,
`first_person_prepare` and `first_person_eye` in `render_gl.cpp` do the work.

Two attempts bracket what the eye should do:

- **Fixed rigidly to the hull** -- turning, pitching and rolling with it -- every slap of a wave
  went straight into the viewer's head at 30 Hz. Rough even for strong VR legs.
- **Levelled outright, height eased over 0.3 s** -- placed in the world, only the hull's heading
  kept -- it was terrible the other way: "just flying a camera around the track, completely
  disconnected from the water". The waves did nothing to the eye, and since the eased height
  lagged the hull's, the ski rose through the viewer on every crest.

What it does now: the eye's **position** is the rider's head on the hull exactly as it lies, with
no lag -- the only thing that keeps the ski under the viewer, and the waves lift and drop the eye
as they lift and drop him. Its **orientation** is eased, in the world (the hull's pose is taken out
of the chase camera's frame, which moves every frame too, through the world matrix; the world's up
is its Y): the heading follows the hull's over `fp_yaw_s` (0.1 s), and pitch and roll follow over
`fp_tilt_s` (0.15 s), scaled by `fp_tilt` (0.7; 1 rides with the hull, 0 is the level horizon
that did not work). `fp_height_s` can ease the height too, but defaults to 0 for the reason above.
A jump of more than 300 units in a frame (a respawn) is followed at once.

The HUD frame hangs in front of the eye, so once the eye pitched and rolled with the ski the HUD
tipped against the horizon, which read as wrong even though it was steady in the room. It is now
hung on the eye's *level* frame -- same position and heading, the world's up -- and turned back
into the eye's own by the inverse of the eye's pitch and roll (`g_hud_xform`), so it stays
upright to the world as the view tilts. Outside first person that transform is the identity.

The head position the headset reports is re-zeroed each time first person is entered: see
"Where the eye is" below.

### Finding the ski in a batch

The vertices reach the renderer already in the chase camera's view space, and the game's own idea
of where the player is was not gone looking for. What *is* in the batch is the position matrix each
draw went through, which `Cmd::mtx` now carries (`Batch::mtxs`, deduplicated against the previous
draw's, and a draw no longer merges with its predecessor across a change of matrix). Reading a
`WR_MTXLOG` dump of Ocean City Harbor 300 frames into a race, which now prints the copies and
textures beside the matrices, the structure of a frame is this:

- The main scene is 3,790 draws, and all but 270 go through one matrix: the world's view matrix,
  loaded at `GX_PNMTX0`. The course, the water, the spray, and the **other racers too**: the game
  transforms them into the world on the CPU, so there is nothing in the matrices to confuse with
  the player.
- The 270 are pieces of the player's racer, through four rigid matrices at `GX_PNMTX1..4`: 87
  draws of the rider's **head**, 18 of the **handlebar** (a bar 25 across and 4 deep), 55 of the
  **steering pole** (42 long; it hinges, so it moves against the hull -- folded flat at the start
  line, raised to the rider's chest once he stands), and 108 draws (677 vertices) of the **hull**.
  The hull is unmistakable by size: its vertices span 81 units along the ski. This list first
  read "head, arms, torso, hull", and the code hid all three that were not the hull, which took
  the handlebars off the ski and left the body standing on it.
- **The rider's body is not among them.** It is skinned by the game on the CPU and drawn through
  the *world* matrix, like the course: some 220 draws, textures of its own, right beside the hull.
  Nothing in its matrix says it is his.
- The hull's model frame is **X to the ski's left, Y up, Z forward**: its matrix's columns come out
  as view-space -X, the same up the world matrix has, and the direction of travel. That is a proper
  rotation, not a mirror -- the world matrix negates X too.
- The racer's reflection, the 128x128 off-screen pass, draws him with *ten* matrices of his own,
  mirrored. So the reflection is skinned per bone and the main scene is not; whatever the reason,
  it means the main scene's four cannot be found by matching the reflection's.

The rule is therefore: among the draws an eye replays, the matrix most vertices go through is
the world's; every other rigid matrix within 600 units of the camera is a piece of the racer,
provided its draws sample a texture the off-screen passes drew near the camera through a rigid
non-world matrix (the reflection pass is the one place in the batch that says what the racer looks
like) and it is under 300 units across; the hull is the longest piece that is at least twice as
long, along its own forward axis, as it is wide. Then the rider, who is left out of every pass,
the flat one included, is two things:

- the body: draws through the world matrix that sample a texture from that same set and whose
  vertices centre within 90 units of the hull's origin. Spray and water are drawn through the
  world too, but in textures the reflection pass never uses;
- the head: a rigid piece within 90 units of the hull that shares a texture with the body (the
  skin) and not with the hull. The handlebar and pole share textures only with the hull.

The body test then grows: any nearby world draw sharing a texture with the body found so far
joins it, until nothing more does. Without that, a few strips of the rider stayed in the air:
about 15 of his draws are textured only with four shading maps the game loads after his model,
which the reflection pass never uses -- but each also wears one the rest of the body does.

A frame with no racer -- the
menus, the course flyover, or a course that does without the reflection -- falls back to the chase
camera, and `WR_FPLOG=1` says so.

Each of those conditions was added after the rule before it picked something else, so they are
worth keeping even where they look redundant:

- *A texture from the reflection pass.* On the title screen a menu quad, 640x480, drawn with a
  perspective projection through a matrix turned a quarter turn, was rigid and near the camera and
  nothing else said it was not a ski. With no reflection pass in the frame there is no racer.
- *Under 300 units.* The sky is a rotation at the origin, 8,000 units wide, and wears the same
  environment map the ski's paint reflects, so it passed the texture test however that set was
  built -- from the reflection pass alone, or from every off-screen pass.
- *Long and narrow.* The course intro flies the camera past a buoy: rigid, within reach, about 90
  units every way, and textured like the buoys the reflection pass draws at that moment. It was
  the hull for the two seconds before the countdown, and the eye sat inside it. A hull is 28
  across and 82 long; nothing else near a ski has that shape.

One thing in the dump worth recording because it looked wrong for a while: the hull's origin is
*above* the camera's forward axis (view-space y of +8 at a depth of 151) though the racer is drawn
at the bottom quarter of the frame. The game's projection is off-centre -- `p[3]` is 0.48, which
shifts the whole image down by a quarter of its height -- so the chase camera's axis points at the
ski and the frustum looks mostly above it. The eye paths use their own projections and never see
that shift; it only matters when reading view-space numbers against a screenshot.

### Where the eye is

The anchor's default is 57.5 up and 46.5 back: 25 cm above and 75 cm behind the rider's eyes,
set by eye in the headset with `fp_up_m`/`fp_forward_m` (offsets in metres from the anchor, for
exactly that). His eyes themselves, 45 up and 9 back, felt too low and too far forward. They are
his eyes in his idle pose: standing at rest
after the start, his head's centre is 43.5 units above the hull's origin and 11 behind it, its
face 7 behind (`WR_MTXLOG=200` with no throttle, 200 frames after the race flag). Racing,
crouched, it is 40 up; kneeling at the start line, before he stands, 16. The grips are at 28 up,
so his head is only some 15 units -- 30 cm at 50 units to the metre -- above his hands.

That is what made the first version look wrong in the headset, with the eye "between his hands".
The anchor (then 42 up and 12 back) was right. What was not was the headset's own position: the
reference space is LOCAL, whose origin is wherever the head was at launch, so the viewer's posture
since then is added to the eye, multiplied by `units_per_metre`. Behind the chase camera a 30 cm
lean is lost in the distance; on a rider whose head is 15 units above his hands it is the whole
gap. So the app takes the head position as zero at the moment first person is entered (and again
whenever stereo begins with it on), and moves the eye only by how the head moves from there.
Leaving first person puts the space's own origin back.

### Hiding a draw keeps its textures

GL textures are evicted after 240 frames unbound, and the guest-side cache, which is what would
send a texture again, is kept alive by the game *referencing* it. Those two only disagree for a
draw left out on purpose, and the first version left the rider out: after eight seconds in first
person the head's textures were deleted on the GL side while the game, still referencing them,
never sent them again, so leaving first person brought the rider back with flat, untextured hair.
A hidden draw now marks its textures used (`touch_textures`).

### What is left alone

- The rider's reflection is still drawn: the 128x128 pass is untouched, and the patch of water
  under the ski shows a rider who is not there. Arguably right -- the viewer has a body, it just
  is not drawn -- and cheap to change if it reads wrongly.
- The morph between theater and stereo blends from the panel to wherever the eye is, so with first
  person on it is a blend from the chase camera's frame to the rider's seat, and the crop planes
  are the chase camera's. It lasts a second.
- `offset_x/y/z` still apply, now relative to the anchor.
- Switching is a cut, not a morph.
- The choice is remembered across sessions, in `<files>/view.txt` (the app writes it; `vr.txt`'s
  `first_person` is only the choice before one has been made), and is in effect only in stereo.
  Leaving stereo drops to the chase camera *before* the morph to theater: the morph folds the
  world onto the panel the game's camera drew, and starting it from the rider's seat swung the
  scene across to that camera on the way.
- `waverace_egl` takes `--first-person[=x,y,z]`, `--fp-window=a-b` (on only for those presented
  frames, which is how switching in and back out is reproduced) and `--eye-pitch=deg` (look down,
  which is the only way to see the handlebars from the rider's eyes).

### Levelling the sea

The game's chase camera looks down **23.2 degrees**, and vertices reach the renderer already in its
view space, which is handed to the headset as though it were gravity-aligned. So the sea was
rendered as a 23-degree slope with the horizon riding high, and everything square to the room
leaned against it. `world_pitch_deg` rotates the world back by that angle before the eye transform,
which puts the sea level with the room. It applies to world geometry only; the HUD frame is placed
in the headset's own space and stays where it is put.

The cost is that the ski ends up about 40 degrees below the forward axis, because the camera was
aimed down at it and that aim is what has been taken out — the viewer's own neck supplies it now,
which is what one does on a jet ski. It is a knob rather than a constant because that trade is a
matter for the headset: 0 renders the tilt the game draws, and intermediate values split it.

That 23.2 is measured, not fitted. GX position matrices are modelview, so for any scenery whose
model is unrotated the matrix *is* the view matrix, and the dot of its second row with world up is
`cos(pitch)` whatever the camera's yaw. Across a `WR_MTXLOG` frame the dominant rotation — 4800 of
13274 draws in one frame, 4218 of 10942 in another a thousand frames later — gives **23.20°** and
**23.27°**. The second cluster sits at exactly 180° minus that, which is the reflection pass:
mirroring about the water plane negates the up component and takes the angle to its supplement, so
the axis being measured against really is the water's normal.

Two traps on the way, both paid for. The horizon is not a shortcut: on a course ringed by land the
visible sea/sky line is the far shoreline, well below the true horizon, and reading it that way
suggests about 7 degrees. And the same frame number in two runs is two different moments, since the
timebase is wall-clock, so a feature cannot be compared across runs that way — the countdown rig
can, being pinned in view space, and it put the flat and eye paths within a quarter degree of each
other.

Worth knowing that it was argued down from the other direction first. A 23-degree *pitch* of a
seascape does not announce itself the way a roll would; it reads as a camera angled at the water,
which is what the game looks like on a television. What gave it away was the HUD needing exactly
23.2 degrees of lean to look upright — a panel aligned to the world rather than to the room.

### Attributing part of the image to the draws that made it

Not a VR problem but found through this harness, and the tools stay because the question recurs:
*which draw put that there?* A race frame is ~390 draws in the flat path.

- `WR_COMPLOG=1` logs every draw that samples a render-to-texture result, with its vertex count,
  texgens and texture ids.
- `WR_DRAWLOG=<frame>` lists every draw in one frame with its index, so geometry drawn twice shows
  up as two draws with identical vertex counts and textures.
- `WR_DRAW_SKIP=a-b` drops a range of draw indices, `WR_NO_COMP` / `WR_ONLY_COMP` drop or isolate
  the draws sampling a whole-frame copy, and `WR_NO_EFBTEX` drops those sampling a partial one.
- `WR_MTXLOG=<n>` dumps the nth frame after the race flag comes up: every draw with its position
  matrix, vertex count, state index and textures, and every EFB copy where it falls among them, so
  the draws can be read against the pass boundaries. It is what the first-person section above is
  read from.
- `WR_PNMLOG=a-b` lists the position matrices a frame uses over a window of frames, one line each
  time the matrix changes. It is what showed that the countdown rig does not tilt as it arrives — it
  turns about the vertical axis while descending — and that of 6,464 matrices over sixteen frames
  only three leave the view-space Y axis alone.
- `WR_EYE_SPRAY=0` and `WR_EYE_RIPPLE=0` turn off the spray substitution and the conversion of its
  distortion, in that order of bluntness. Both select on content rather than draw index, so they
  are safe to A/B across runs. With `WR_EYELOG=1` the eye summary carries `spray=N`, the number of
  substituted droplet draws, and the first of them prints its indirect configuration.

One caution that has cost time twice: **the emulated timebase is wall-clock driven, so frame N is
not the same moment in two runs.** Any A/B that compares frame N across runs is comparing different
scenes. Either make the comparison inside one run, or pick a selector that is stable from frame to
frame — "draws sampling the water copy" rather than "draws 256 to 273".

### Tracing what the compositor is shown

`log_frames 1` in `vr.txt` logs the theater path one display frame at a time: which swapchain image
was written and which game frame went into it — only frames the game produced touch the swapchain
at all. `dump_every N` makes the app write its own EFB to `<files>/frames`, which is the only way to
see what the app rendered rather than what a different frontend renders from the same code.

The app runs on a headset nobody is wearing, so both can be captured without help: push a
`wr_input.txt` script beside `vr.txt`, `am start` the activity, and read `adb logcat -s waverace`.

**Capturing can destroy what you are capturing.** The character-select tear was timing-sensitive: it
reproduced reliably on a clean build and vanished under per-frame image dumping, which shifts the
phase between a ~38 fps game and a 72 Hz compositor without moving the average frame rate enough to
notice. Check whether a fault survives the instrument before trusting a clean capture, and prefer
CPU-side logging to anything that reads back the GPU.

Two further cautions, both paid for. A detector answers the question it was given: a hunt for
"a hard vertical edge" spent a long investigation on the game's own mode panel for the course
flyover, which has a deliberately sharp edge, before anyone asked whether the edge belonged there —
checking the other platform settled it in one run. And a claim that the fault was Quest-only was an
assumption about the Mac that nobody had tested; when tested, the Mac rendered the same thing.

One thing that investigation turned up and left alone was the real fault behind a different
symptom. The scissor offset register reads 340, where the renderer assumed the 342 that
`GXSetScissorBoxOffset(0, 0)` implies. The game sets the offset to (-2, -2) and moves every
viewport by the same amount, so on hardware the two cancel; assuming 342 drew every pass two
pixels up and left of where the game believes it is. The water surface looks its refraction up in
a copy of the frame at screen coordinates the game computes itself, so it sampled that copy two
pixels out of register and stood a water-tinted second copy of the racer beside the real one —
long taken for a misplaced reflection. `scissor_offset()` now reads the register.

The racer's actual reflection is a separate draw, and it was missing altogether: a 128×128 pass
renders him mirrored, and a patch of water under the ski samples it through an indirect stage
that adds the previous stage's coordinate. The shader generator carried over only the previous
stage's *offset*, so the lookup landed in the texture's empty corner. Two things worth knowing
before searching for a mirror again: the game mirrors the camera about the water plane *and*
negates view-space X, so the reflection passes have a positive determinant, and it un-flips X
when it samples them.

### One launch per process

`android_main` is entered again if the activity is destroyed and re-created inside a live process,
which happens whenever the app is backgrounded and relaunched rather than force-stopped. Nothing in
this app survives that. The EGL context, the OpenXR instance and session, the recompiled game and
the threads it booted are all global and are built exactly once, so the second call deadlocks on
the first call's leftovers — a launch that hangs with a black screen until the app is killed.

The process therefore exits whenever `android_main` would return, and the next launch gets a clean
one. Unwinding instead is not on offer: the game is a recompiled executable with no shutdown path.
The memory card does not mind, since it is rewritten at the end of every command that dirties it.

Worth knowing because it was slow to spot from inside the headset, where it looks like a random
launch failure: **the signature is in `/proc`**. The app is `android:debuggable`, so its own uid can
read what the shell cannot:

```sh
PID=$(adb shell pidof com.example.waverace)
adb shell "run-as com.example.waverace sh -c 'for t in /proc/$PID/task/*; do cat \$t/syscall; done'"
```

Two threads parked in syscall 63 with a count of `0x1ff` are two copies of `log_pump` blocked in
its `read(fd, buf, sizeof(buf) - 1)`, and two copies of `log_pump` mean two calls to
`android_main`. `ls -l /proc/$PID/fd` through the same `run-as` shows the matching pair of pipes.
Neither `kill -3` nor `debuggerd` works here — the first is refused across uids and the second
wants root — and the traces the system does write land in `/data/anr`, which the shell cannot read
either.
