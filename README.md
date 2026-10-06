# bluestorm-recomp

A static recompilation of *Wave Race: Blue Storm* (Nintendo GameCube, 2001) to native code.

The game's PowerPC executable is translated ahead of time into C, then compiled and linked
against a runtime that stands in for the GameCube hardware (graphics, audio, DVD, controllers,
memory card). The result is a native program that runs the original game logic without an
emulator's CPU core.

> **This repository contains no game code or data.** You must supply your own disc image,
> dumped from a copy of the game that you own. The build process reads the executable out of
> *your* image and recompiles it locally. Nothing derived from the game is distributed here.

## Legal

- *Wave Race: Blue Storm*, GameCube and all related names and marks are property of Nintendo.
  This project is not affiliated with or endorsed by Nintendo.
- This repository contains only original source code (the recompiler, the hardware/runtime
  layer and the renderer) plus metadata describing code layout (function addresses and
  names in `analysis/`). It contains no copyrighted game code, assets, BIOS/IPL files or
  DSP microcode.
- You need a legally obtained copy of the game. Dump it from your own disc using your own
  GameCube/Wii (e.g. with [CleanRip](https://wiibrew.org/wiki/CleanRip)). Do not download
  game images, and do not share the image, the extracted executable, the generated C code
  (`build/gen/`), built binaries, or memory card files, since all of these contain or are
  derived from the game.

## Status

Working:

- Boots, attract-mode movie (THP), title, menus, memory card save/load, character select,
  course flyovers and full races.
- Graphics: GX command processing, CPU-side transform and lighting, a generated TEV pixel
  pipeline (including indirect texturing), all texture formats, EFB copies and render-to-texture.
  OpenGL 4.1 renderer with a configurable internal resolution.
- Audio: AX DSP ucode (sound effects) and DVD-streamed music, on the desktop through SDL
  and on Android/Quest through AAudio.
- Input: keyboard and any SDL-compatible gamepad.
- Memory card: emulated slot A card stored in `saves/memcard_a.raw`. You don't need to
  provide one: a blank, formatted card is created automatically the first time you run the
  game, and the game creates its save file on it as usual.

Known limitations:

- Builds on **macOS (Apple Silicon)** and **Windows (x64 and ARM64)**. Other platforms should
  need little beyond a CMake branch: the POSIX/Win32 split is confined to `runtime/platform.cpp`.
- Windows needs a real OpenGL 3.3 driver, which some virtual machines do not provide; see
  [Graphics](#graphics).
- Runs at the game's native 30 fps (the simulation advances a fixed 1/30 s per frame).
- Accuracy is still being validated. Expect visual and audio differences from real hardware.

### Open issues

In rough priority order. Everything here reproduces; where there is a lead it is written down so
the next attempt does not start from nothing.

1. **Performance.** Ocean City Harbor did not hold 30 fps. Dropping the per-display-frame repaint
   bought about 15%; the next candidates are deduplicating `PixelState` by content and the triple
   scene render the stereo path does (one flat into the EFB, then one per eye). It is **not** the
   pixel pipeline: raising the EFB from 640×528 to 2560×2112, sixteen times the pixels, costs
   about 2% and nothing beyond that scales with area — see
   [How many pixels the theater panel gets](#how-many-pixels-the-theater-panel-gets). Look in the
   recompiled guest code and in the per-draw CPU work instead.

## Supported version

Only the North American release is supported:

| Title | Disc ID | `main.dol` SHA-1 |
|---|---|---|
| Wave Race: Blue Storm (USA) | `GWRE01` | `d500d8f5bab39ae5fed67d0a0864095a9ea190d9` |

The build verifies this hash and stops with an error for any other version.

## Building

### Requirements

**macOS (Apple Silicon)**

- Xcode (or the Command Line Tools), CMake 3.20+, Ninja, Python 3
- SDL2

With Homebrew:

```sh
brew install cmake ninja sdl2 python
```

**Windows (x64 or ARM64)**

- Clang, CMake 3.20+, Ninja, Python 3
- The Windows SDK and MSVC headers/libraries (the "Desktop development with C++" workload of
  Visual Studio or the standalone Build Tools) — clang targets the MSVC ABI and uses them

Clang is used rather than MSVC: the runtime and the generated code rely on GCC-style flags and
builtins, and clang copes better with the very large generated translation units. SDL2 is built
from source automatically (`FetchContent`), since there are no prebuilt ARM64 Windows binaries.

With [winget](https://learn.microsoft.com/windows/package-manager/):

```powershell
winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.13 LLVM.LLVM Microsoft.VisualStudio.2022.BuildTools
```

### 1. Provide your game image

Dump your disc, then place the image in `rom/` as an **uncompressed `.iso`**:

```
rom/game.iso
```

If your dump is in a compressed format (`.rvz`, `.gcz`, `.ciso`), convert it to ISO first,
for example with Dolphin (right-click the game → *Convert File…* → *Uncompressed Disc Image*),
or with `dolphin-tool convert`, or [`nodtool`](https://github.com/encounter/nod):

```sh
cargo install --locked nodtool
nodtool convert "rom/Wave Race - Blue Storm (USA).rvz" rom/game.iso
```

`nodtool` also ships prebuilt binaries (including Windows ARM64) on its
[releases page](https://github.com/encounter/nod/releases), which avoids needing Rust. Note that
Dolphin only gained RVZ support in 5.0-12188, so the 5.0 release that package managers offer
cannot convert one.

The build uses the first `.iso` it finds in `rom/`. To use an image elsewhere, pass
`-DGAME_ISO=/path/to/game.iso` when configuring.

### 2. Build

```sh
./build.sh          # macOS
```

```powershell
.\build.ps1         # Windows
```

The first run configures CMake, extracts `main.dol` from your image into `build/`, verifies it,
recompiles all 2,572 game functions to C (`build/gen/`) and builds the `build/waverace`
executable. Recompilation plus compilation takes well under a minute on a recent Mac.

Without a game image, only `ninja -C build runtime_check` is available: it compiles the runtime
(but cannot link, which needs the recompiled game code). That is enough to check a change to the
runtime or a port to a new platform.

### 3. Run

```sh
./build/waverace            # uses the configured image / rom/*.iso
./build/waverace path/to/game.iso
```

Run it from the repository root: the memory card is created in `saves/` relative to the
current directory. Like any save file, it contains data written by the game, so don't
share or commit it (`saves/` is git-ignored).

## Benchmarking

`waverace_bench` runs the game with no graphics, audio or input and reports how fast the guest
actually advances. It exists to compare CPUs on equal terms — in particular to find out whether
a device can sustain the game's 30 fps before porting the renderer to it.

```sh
./build/waverace_bench --seconds=25
```

```
whole run:    411 frames in 25.0 s = 16.44 fps
steady state: 360 frames in 17.0 s = 21.18 fps = 71% of the game's 30 fps
              1533k vertices/s
```

The first seconds are boot and asset loading, which run at a different rate than gameplay;
`--warmup=N` (default 8) excludes them from the steady-state figure, which is the number to
compare between machines. The target drops the renderer and the SDL frontend entirely, so it
builds anywhere the recompiled code builds — including a cross-build for a device.

### Measuring headroom

A host that is only just keeping up and one with plenty to spare both report the same capped
30 fps, because the simulation advances a fixed 1/30 s per frame. Per-thread CPU time does not
settle it either: the game paces itself by waiting on video retrace, and that wait reads as a
busy thread, so the guest thread shows ~99% of a core even when it has room to spare.

`WR_TIMESCALE=N` makes the emulated timebase advance N times faster, so the game tries to run
at N times real time. Raise it until the frame rate stops climbing and that plateau is the
machine's actual ceiling:

| | 1.0 | 1.5 | 2.0 | 3.0 |
|---|---|---|---|---|
| Quest 3 (6× Cortex-A78C, 2.05/2.36 GHz) | 29.9 | 38.3 | 43.1 | **48.5** |
| VMware guest, 2 vCPUs of Apple Silicon | 20.5 | – | 29.9 | **30.7** |

The Quest tops out near 48 fps — about 1.6× real time, so the 30 fps simulation leaves roughly
60% headroom there. `WR_TIMESCALE` skews all other emulated timing, so it is a diagnostic only.

The second row shows why a single measurement at 1.0 can mislead: 20.5 fps is *below* that
machine's own 30.7 fps ceiling, which throughput alone cannot explain. With only two cores, the
guest thread contends with the 200 µs ticker and the batch drain and overshoots its retrace
waits, so it is latency-bound rather than CPU-bound. Prefer a machine with cores to spare when
measuring, and read the ceiling rather than the 1.0 figure.

Note that guest execution is serialised: many guest threads exist but exactly one runs at a
time (see `runtime/threads.cpp`), so extra cores do not raise this ceiling. The vertex
throughput column matters for the same reason — CPU-side transform and lighting run on that
one thread.

### On an Android device (e.g. a Quest headset)

Needs only the NDK and `adb`, not the full SDK, Gradle or a JDK:

- [Android NDK](https://developer.android.com/ndk/downloads) unpacked to
  `%LOCALAPPDATA%\Android\Sdk\ndk\<version>\`
- [platform-tools](https://developer.android.com/tools/releases/platform-tools) unpacked to
  `%LOCALAPPDATA%\Android\Sdk\platform-tools\`

Put the headset in developer mode, allow USB debugging, confirm `adb devices` lists it, then:

```powershell
.\build-android.ps1 -Run
```

That cross-compiles for `arm64-v8a`, pushes the binary and the disc image to
`/data/local/tmp/waverace/`, and runs the benchmark. The image is only pushed when it isn't
already on the device at the right size. `-DWR_BENCH_ONLY=ON` is what makes a build with no
SDL2, no GL and no renderer possible.

## Graphics

The renderer needs an **OpenGL 3.3 core profile**, or **OpenGL ES 3.2** when built with
`-DWR_GL_ES=ON` (the default for Android). It uses nothing newer than GL 3.3 / ES 3.0:
samplers, VAOs, FBOs and explicit attribute locations are the whole requirement, so a 3.3 floor
keeps the mapping layers that stop there usable. One source serves both profiles; where they
differ, the difference is confined to a small block of helpers at the top of
`runtime/gx/render_gl.cpp`.

On macOS the system framework is linked directly; elsewhere the entry points are resolved at
runtime by a vendored [glad](https://gen.glad.sh/) loader (`runtime/gx/glad/` and
`glad_es/`, regenerated with `glad --api gl:core=3.3 --extensions ""` and
`--api gles2:core=3.2`).

Two things GL ES cannot do, both accepted rather than emulated:

- **Logic-op blending.** ES has no `GL_COLOR_LOGIC_OP`. GX logic ops are skipped, so such draws
  write through with blending off. Reproducing them would mean reading the framebuffer in the
  generated TEV shader via `GL_EXT_shader_framebuffer_fetch`.
- **Sampler LOD bias.** ES has no `GL_TEXTURE_LOD_BIAS`, so mip selection can differ slightly.

### Shader cache

Each TEV configuration becomes a generated fragment shader, compiled the first time a draw
uses it -- on the render thread, mid-frame. A race through a course meets around eighty of
them, and they do not arrive one at a time: the race start brings in nine at once (the spray,
the wake, the speed effects), and on this machine's compiler that frame took 60 ms against a
4 ms norm. A mobile driver takes tens of milliseconds per program, so the same burst is a
visible hitch at a fixed spot in the course, on every fresh launch.

So every key compiled is appended to a file (`saves/shaders.bin` on desktop, `shaders.bin` in
the app's files directory on a headset), and the next run builds all of them in `render_init`,
before the game boots. Where the driver hands back program binaries (GL ES 3.0 does; macOS
reports no binary formats) those are stored too, and the run after that loads rather than
compiles. A binary is only trusted with the same driver and the same generated source -- the
file carries the GL strings and each record a hash of its GLSL -- and anything stale falls back
to compiling and rewrites the file. Deleting the file is always safe; the first run just pays
the compiles at first use again. The startup line reports what happened:

```
[shaders] 80 programs from cache (0 from binaries, 80 compiled) in 138 ms
```

### Validating the ES renderer on a device

`waverace_egl` (Android) runs the real renderer on a headless EGL pbuffer and writes frames as
PNGs. No APK, no window, no OpenXR — it exists to check the ES back end on real hardware:

```powershell
.\build-android.ps1 -Render -Seconds 200 -DumpEvery 150 -InputScript "1050:START:12,1250:A:12,1450:A:12"
```

`-InputScript` sets `WR_INPUT`, which is how anything past the title screen is reached without a
controller. Frames are pulled back to `build-android/frames/`. The presses are counted in
submitted game frames, not wall-clock, so a script reaches the same place whatever the frame rate;
`WR_INPUT_LOG=1` prints each one as it fires. Without input the game loops its attract movie,
which is a single textured quad per frame and exercises nothing.

`--eye` renders through the stereo path instead, into an offscreen eye target, and dumps that.
Changes to the VR renderer are visible here without putting the headset on, which is the only
practical way to tell a wrong frame split from a wrong projection:

```powershell
adb shell "cd /data/local/tmp && WR_INPUT='600:START:10,800:START:10,1000:START:10,1200:A:10,1400:A:10,1600:A:10' \
  ./waverace_egl --eye --seconds=150 --dump-dir=/data/local/tmp/eye --dump-every=500 <iso>"
```

## Audio

The mix is portable and lives in `runtime/audio.cpp`: it resamples the AI DMA stream (32 kHz,
from the AX HLE) up to 48 kHz, adds DVD-streamed DTK music decoded in place, and hands back one
interleaved stereo buffer. The device that pulls on it is the only part that belongs to a
platform, so that is all a backend is — `audio_sdl.cpp` opens an SDL device, `audio_aaudio.cpp`
opens an AAudio stream, each supplies `audio_open()` and each calls `audio_render()` from
whatever callback its API hands it. `audio_stub.cpp` still stands in for the benchmark, which
has no device at all.

AAudio rather than Oboe: Oboe exists to paper over the broken audio paths on Android 4.4–7.x by
falling back to OpenSL ES, and this targets API 29 and up, where Oboe is a thin wrapper over
exactly the calls the backend makes. The stream asks for 48 kHz stereo 16-bit, low latency, and
a device buffer of two bursts; on a Quest 3 that is a 192-frame burst and a 384-frame buffer,
which runs at 265–268 callbacks a second with no xruns. A device that will not give 48 kHz
stereo 16-bit is reported and left alone rather than played at the wrong rate.

Nothing in the data callback calls back into AAudio. The API rules out stop, pause, close and
`waitForStateChange` from in there; `getState` is no better in practice, so the callback only
bumps a counter and a separate watcher thread reads it. That watcher stays silent while the
stream is healthy and logs one line when it starts playing, which is what answers "is there
sound?" from a logcat alone — the app has no environment to set a debug variable in.

**The headset must be awake.** Asleep on a desk, the audio sink does not consume: the stream
opens, reports `low latency`, takes exactly enough callbacks to fill its buffer and then stops,
staying in `AAUDIO_STREAM_STATE_STARTING` forever with no error callback. It looks precisely
like a broken mix and is not one. `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close`
followed by `adb shell input keyevent KEYCODE_WAKEUP` wakes it for testing over adb, and
`mWakefulness=Asleep` in `dumpsys power` is the tell.

`WR_AUDIO=1` opens the device in `waverace_egl` too, which is the only way to exercise the audio
path without the VR frontend. With `WR_WAV=<path>` it records exactly what the device was handed,
so the result is checkable afterwards rather than by listening: a 140-second run through boot, the
menus and a race at Dolphin Park gave a steady ring level at the 60 ms cushion, a resampling trim
under 0.06%, and one underrun — the initial fill. `WR_AUDIO_TEST=1` fills the buffer with a sine
instead of the mix, which separates "the device is not pulling" from "the mix is not returning";
`WR_AUDIO_PERF=none` and `WR_AUDIO_BURSTS=N` change the stream's performance mode and device
buffer, since what a given device will actually start playing is not something the documentation
settles.

### Installing on a headset

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

| GameCube | Touch controller |
|---|---|
| Control stick | Left thumbstick |
| C-stick | Right thumbstick |
| A / B | A / B (right) |
| X / Y | X / Y (left) |
| Z | Right grip |
| L / R | Left / right trigger |
| Start | Menu (left) |
| *(toggle view)* | Right thumbstick click |

### Theater and stereo

The app presents the game two ways and switches between them automatically:

- **Theater** — the frame on a flat screen in space, as an `XrCompositionLayerQuad`.
- **Stereo** — the world rendered per eye as an `XrCompositionLayerProjection`, with the 2D
  elements painted on a frame standing in front of the game's camera.

The switch is driven by the game's own state. No single variable spans a race at both ends, so
three are read, one job each: `0x80602160` counts what the course loaded and is the only one that
clears when a race is *quit*, so it gates the rest; `0x80625A54` is the start sequence's state and
brings stereo up with the starting lights; `0x806193BC` is the course's wave height, which is what
notices a race *finishing*. Stereo is `on_course > 0 && (wave_height || start_state == countdown)`.
Clicking the
right thumbstick pins the view manually, which is also the way to compare the two. The sections
below are the working: how they were found, and why the earlier answers were wrong.

They replaced counting a frame's perspective draws, which was wrong in both directions: the course
overview is a full 3D flyover and cleared the threshold, so stereo began before the race, and a
sparse view during a race dipped below it, so the view flapped. **There is no fallback to that
heuristic.** But an unexpected value is no longer fatal: it presents the flat view and says so
once in the log. An earlier build aborted instead, on the belief that `0x80602160` was a flag that
could only read 0 or 1, and that belief was wrong — see
[When a count was mistaken for a flag](#when-a-count-was-mistaken-for-a-flag).

#### Finding it, and why the first two answers were wrong

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

#### Why the wave height was the wrong variable, twice

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

#### When a count was mistaken for a flag

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

#### How many pixels the theater panel gets

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
the first open issue: whatever is keeping this below 30 fps, it is not the pixel pipeline.

#### Splitting a frame for the eyes

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

Verified on the device through the `--eye` harness below: at 103 km/h the rectangles over the HUD,
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

#### Where the HUD goes

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

#### Levelling the sea

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

#### Attributing part of the image to the draws that made it

Not a VR problem but found through this harness, and the tools stay because the question recurs:
*which draw put that there?* A race frame is ~390 draws in the flat path.

- `WR_COMPLOG=1` logs every draw that samples a render-to-texture result, with its vertex count,
  texgens and texture ids.
- `WR_DRAWLOG=<frame>` lists every draw in one frame with its index, so geometry drawn twice shows
  up as two draws with identical vertex counts and textures.
- `WR_DRAW_SKIP=a-b` drops a range of draw indices, `WR_NO_COMP` / `WR_ONLY_COMP` drop or isolate
  the draws sampling a whole-frame copy, and `WR_NO_EFBTEX` drops those sampling a partial one.
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

#### Tracing what the compositor is shown

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

#### One launch per process

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

### Tuning VR (`vr.txt`)

Some values cannot be known from the source. `a_pos` arrives in the game's own units and nothing
says how many make a metre — get it wrong and the world is giant or doll-sized, which is only
judgeable by wearing the headset. So they are read at startup from
`/sdcard/Android/data/com.example.waverace/files/vr.txt` and can be changed with `adb push`
between runs, no rebuild:

```
units_per_metre 50       # game units per real metre: sets the apparent size of the world
offset_x 0               # viewpoint relative to the game's camera, in game units
offset_y 0               #   x right, y up, z back
offset_z 0
near_m 0.1               # near/far planes in metres; too wide a ratio causes z-fighting
far_m 2000
hud_scale 0.5            # how much of the field of view the HUD's frame occupies
hud_distance_m 4         # how far in front of the game's camera that frame stands, in metres
hud_height_m -0.36       # how far above the forward axis the frame's centre sits
hud_pitch_deg 0          # frame tilt; 0 is square to the room, which a levelled sea wants
world_pitch_deg 23.2     # degrees of chase-camera pitch taken back out of the world; 0 keeps it
theater_scale 3          # EFB samples per hardware pixel, per axis, in theater
stereo_scale 1           #   and in stereo, where the EFB is only scratch space
start_in_stereo 0          # start in stereo rather than theater
```

The defaults are starting guesses, `units_per_metre` especially. The logged values appear in
`adb logcat -s waverace` at startup.



`android/AndroidManifest.xml` plus `package-apk.ps1` build an installable APK around a
`NativeActivity`. There is no Java source and no Gradle — the framework's
`android.app.NativeActivity` loads `libwaverace.so` and calls `android_main()` — so packaging is
just `aapt2` → zip → `zipalign` → `apksigner`. It appears on Horizon OS under *Unknown Sources*.

Needs, in addition to the NDK: a JDK, and the SDK's `build-tools` and a `platform`:

```powershell
winget install Microsoft.OpenJDK.21
# then sdkmanager --licenses, and: platforms;android-34  build-tools;34.0.0
.\build-android.ps1          # builds libwaverace.so
.\package-apk.ps1 -Install   # packages, installs, pushes the disc image
```

The disc image is **not** in the APK — it is yours, and far too large. It is pushed to the app's
external files directory, which needs no runtime permission:
`/sdcard/Android/data/com.example.waverace/files/game.iso`.

The headset must actually be worn: the OpenXR session stays `IDLE` otherwise and nothing
renders. `adb logcat -s waverace` reports compositor and game frame counts every five seconds.
`wr_input.txt` in that same directory takes a `WR_INPUT` script, which drives the game without
touching the controllers.

### A note on measuring performance here

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

The draw-call shape is still worth knowing — ~830 draws per frame during a race, averaging ~106
vertices each — and deduplicating `PixelState` by content would likely still help. But it is an
optimisation, not the explanation for a slideshow that was really a memory leak. Note that
`render_execute` already skips `apply_state` when consecutive draws share a state index; what it
cannot skip is distinct indices holding identical contents.

If the game exits with `OpenGL 1.1 is too old (got "1.1.0" / "GDI Generic")`, the host has no
OpenGL driver at all and Windows is falling back to its software 1.1 implementation. This is
common in virtual machines: VMware's and VirtualBox's Windows guest drivers expose Direct3D but
no OpenGL ICD, and on Windows ARM64 there is no vendor GL driver to fall back on. Options:

- Enable 3D acceleration in the VM's settings, and install the guest additions / VMware Tools
  that match it.
- Install Microsoft's **OpenCL, OpenGL, and Vulkan Compatibility Pack** (`winget install
  9NQPSL29BFFF --source msstore`), a Mesa build that maps OpenGL 3.3 onto Direct3D 12. It needs
  a D3D12-capable adapter; `dxdiag` reports the feature levels your adapter supports.
- Drop a Mesa `opengl32.dll` (llvmpipe) next to `waverace.exe` to render in software.
  `opengl32.dll` is not a KnownDLL, so a local copy takes precedence.
- Run on the host rather than in the VM.

`--headless` skips graphics and audio entirely, which is useful for checking that everything
below the renderer works.

## Controls

| GameCube | Keyboard | Gamepad |
|---|---|---|
| Control stick | Arrow keys | Left stick |
| C-stick | – | Right stick |
| A | X | A (south) |
| B | Z | X (west) |
| X | C | B (east) |
| Y | S | Y (north) |
| Z | D | Right shoulder |
| L / R | Q / W | Left / right trigger |
| Start | Enter | Start |
| D-pad | I / J / K / L | D-pad |

Escape quits.

## Command-line options

| Option | Description |
|---|---|
| `--scale=N` | Internal render resolution multiplier (default 2) |
| `--hidden` | Don't show the window (testing) |
| `--headless` | Run without graphics or audio output |
| `--dump-dir=DIR --dump-every=N` | Save every Nth frame as a PNG |
| `--sample` | Print periodic profiling samples of what the game code is doing |
| `--log-all` | Verbose hardware logging |

Debugging environment variables used during development include `WR_INPUT` (scripted
controller input), `WR_GXSTATS`, `WR_TRACE_FRAME`, `WR_WAV` (record audio output) and
`WR_PEEK`. See the source for details.

`WR_FRAMETIME=1` prints a line per frame from each thread: the guest's interval between
presents and how much of it the GX front end took (vertex decode, transform and lighting,
texture hashing and decoding), the batch's shape -- GX draw commands before and after merging,
vertices, pixel states, textures decoded -- and the submission queue's depth, which is non-zero
only when the guest is waiting on the renderer; then the renderer's time to issue the batch,
split out into texture and vertex-buffer uploads, with the number of full state applications
and of shader programs so far. A long frame identifies itself: a jump in programs is a shader
compile, a texture burst is a load, and a long interval with a short front end is the game
itself.

A deterministic route to Ocean City Harbor, the course that has been the performance
benchmark, is `WR_INPUT='600:START:10,800:START:10,1000:START:10,1200:A:10,1400:DOWN:10,
1500:A:10,1700:A:10,1900:A:10,2250:RIGHT:5,2350:RIGHT:5,2450:RIGHT:5,2600:A:10,3000:A:4000'`
(title, memory card, Time Attack / Normal, the default rider, three courses to the right, then
hold the throttle). The first three presses are the three logo screens; the frame counts are
loose because the menus fade in on wall-clock time, so a press that lands during a transition
is lost. Course select cycles Lost Temple Lagoon, Southern Island, Aspen Lake, Ocean City
Harbor; the three RIGHTs assume it opens on the first of those, as it did here, so check a
frame dump if a different save opens it elsewhere.

### Which guest function did that?

Every recompiled function opens with `ENTER(its own address)` and leaves through `RET()`, which
pops — together they keep a guest call stack in `CPU`, and `func_name()` turns an address into a
name from `analysis/symbols.txt` (the GX SDK entry points are named there, so the stack reads
sensibly). `debug_dump_threads()` prints it from the fault and interrupt handlers, and anything
asking which guest code produced a particular draw, copy or matrix can read it directly.

It is **off by default**: `cmake -DWR_TRACE_CALLS=ON`. Both macros compile to nothing otherwise
and the fields leave `CPU` with them, which also takes 1 KB off the snapshot every context switch
copies.

Not because it is slow, which was the expectation and is wrong. Benchmarked on a Quest 3 at
`WR_TIMESCALE=3`, running OFF/ON/OFF, the steady state was 40.60, 41.34 and 40.84 fps — the
instrumented build measured *faster*, consistently. A push per call and a pop per return are below
the noise floor here, and the couple of per cent between builds is code layout. It is out of a
release build because it is diagnostic machinery the running game has no use for; there are no
frames to win by removing it.

The stack replaces a ring of the last 256 functions *entered*, which was compiled into every build
and could not answer the question it looked like it answered. Between a callee and its caller sit
all the callee's siblings and any interrupt handler that ran, so the entry just before
`GXLoadPosMtxImm` is another GX call, not the code that wanted the matrix; and two draws a few
microseconds apart share almost the whole ring. Diffing 200 entries either side of a draw found
nothing unique to it. A stack answers it exactly.

It survives the thread switching because the fields live in `CPU`, which `OSSaveContext` snapshots
and `OSLoadContext` restores beside the `jmp_buf` — so longjmping back into a parked guest thread
brings the stack back with the C stack it belongs to. Depth is counted past the end of the array,
so recursion deeper than 256 frames still unwinds to the right place.

**Ask at the right end of the FIFO.** A GX command cannot be attributed where the renderer sees it.
The game writes commands into a FIFO and the GP is kept a frame behind, so the stack at a draw, an
EFB copy or a matrix load is the *drain* site — six frames straight to `main`, identical for every
command in the frame. Ask where the command is written into the write-gather pipe instead and the
guest is still inside the code that wanted it. `WR_GP_STACK=<hex word>` prints the stack the first
few times that word is pushed into the pipe, which is how the countdown rig's placement was traced
to `fn_800D4260`; `WR_POSMTX_STACK=<z>` does the same for a position matrix load, and shows the
drain site rather than the owner, which is the point.

**The guest keeps its own stack too.** The PowerPC ABI puts the caller's frame pointer at `[sp]` and
its return address at `[sp+4]`, so a backtrace can be walked straight out of guest memory with no
instrumentation at all — which is what `--sample` reports, and what makes attribution available in a
release build. `ENTER`/`RET` give an exact stack with names for the price of the instrumentation;
the backchain gives one for free, and misses frames that have none.

## How it works

```
rom/game.iso ──extract_dol.py──> build/main.dol ──recomp.py──> build/gen/*.c ──┐
                                                                              ├─> waverace
                                          runtime/ (hardware + renderer) ─────┘
```

- **Recompiler** (`recomp/`): a Gekko (PowerPC 750CL with paired singles) to C translator.
  Function boundaries and jump tables come from
  [decomp-toolkit](https://github.com/encounter/decomp-toolkit) analysis (`analysis/symbols.txt`);
  every function becomes a C function operating on a CPU state struct. Indirect branches go
  through a lookup table; `recomp/patches.txt` can replace individual instructions.
- **CPU/OS** (`runtime/cpu.cpp`, `runtime/threads.cpp`): memory is a flat host allocation
  indexed by guest address. The game's own OS runs unmodified. Only context switching
  (`OSSaveContext`/`OSLoadContext`) is replaced: each guest thread runs on a host thread and
  resumes via `setjmp`/`longjmp`. Interrupts are delivered at loop back-edges.
- **Hardware** (`runtime/hw/`): register-level emulation of the processor interface, video
  interface, DVD interface, serial interface (controllers), EXI (IPL/RTC/SRAM, memory card),
  audio interface and DSP interface, with high-level emulation of the DSP's boot ROM, the AX
  audio ucode and the memory card unlock ucode.
- **Graphics** (`runtime/gx/`): the GX FIFO is parsed as the game writes it. Vertex transform,
  lighting and texture coordinate generation run on the CPU, and the per-pixel TEV stages are
  compiled into GLSL shaders. Draw batches are handed to an OpenGL renderer on the main thread.

## Tools

- `tools/fetch_dtk.sh`: downloads decomp-toolkit, used to regenerate `analysis/`
  (`dtk dol split analysis/config.yml analysis/out` after a build has produced `build/main.dol`).
- `tools/dis`: prints the disassembly of a function from dtk's output.
- `tools/contact.py`: tiles dumped frames into a contact sheet.
- `tools/glprobe.c` (Windows): reports the OpenGL versions the host can actually create, to
  diagnose the "too old" failure in [Graphics](#graphics).
  Build with `clang tools/glprobe.c -o build/glprobe.exe -lopengl32 -lgdi32 -luser32`.
- `tools/d3d12probe.cpp` (Windows): reports which adapters can create a D3D12 device, i.e.
  whether the OpenGL compatibility pack has anything to map onto.
  Build with `clang++ tools/d3d12probe.cpp -o build/d3d12probe.exe -ld3d12 -ldxgi -lole32`.

## Acknowledgements

- [Dolphin](https://dolphin-emu.org/): the hardware behaviour here, particularly the DSP/AX HLE,
  GX/TEV semantics and I/O registers, follows the extensive documentation embodied in
  Dolphin's source.
- [decomp-toolkit](https://github.com/encounter/decomp-toolkit) and
  [nod](https://github.com/encounter/nod) by Luke Street.
- YAGCD (*Yet Another GameCube Documentation*) and the GameCube homebrew/decompilation communities.
