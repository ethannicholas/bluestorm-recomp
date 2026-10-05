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
- Audio: AX DSP ucode (sound effects) and DVD-streamed music.
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

### Installing on a headset

The Android app is an **immersive OpenXR** app, not a 2D panel: it takes over the display and
owns the controllers. A panel in the home environment cannot capture input at all (the A button
goes to the shell), which is why this is immersive even though it only shows a flat screen.

Theater mode is built on `XrCompositionLayerQuad`: the compositor is handed one flat texture and
places it in space, reprojecting at display rate. Head tracking therefore stays smooth however
slowly the game renders. Measured on a Quest 3: **72 Hz compositor with no stale frames while the
game fed it 30 fps**. Full 3D later replaces the quad with a projection layer and per-eye
`u_proj`/`u_view`; the session, swapchain and input code are unchanged by that.

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
  elements kept flat as an overlay.

The switch is driven by the game's own state: `0x80631FA4` is 1 from the moment the countdown
starts until the race ends, and 0 through boot, the menus, course select, loading, the course
overview flyover and the results screen. Being exact, it needs no hysteresis. Clicking the right
thumbstick pins the view manually, which is also the way to compare the two.

It replaces counting a frame's perspective draws, which was wrong in both directions: the course
overview is a full 3D flyover and cleared the threshold, so stereo began before the race, and a
sparse view during a race dipped below it, so the view flapped. The draw count survives only as a
fallback for a disc the address does not suit — if it ever reads anything but 0 or 1 it is not the
flag, and the old heuristic takes over.

The address came from diffing guest RAM against labelled screenshots: `WR_RAMSNAP=<dir>` makes
`waverace_egl` write the low 8 MB of guest RAM beside a PNG every `WR_RAMSNAP_EVERY` frames, and
the frames say which snapshot is a menu, the overview, a race or the results. Asking for the words
that hold one value across every racing snapshot and a different single value across every
non-racing one left exactly two candidates out of two million.

Stereo is cheap here because of where `xf.cpp` stops. Vertices reach the renderer in the game's
*view* space with the projection applied in the shader, so an eye is just another matrix in front
of it: both eyes share one vertex buffer, one CPU-side transform and one set of render-to-texture
results, and only the uniforms and draw calls repeat.

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
composited over the scene. They are left in the world, where the billboard's edge shows as a seam.
The two alternatives are both worse. Dropping them leaves the seabed showing through bare sand
instead of blue-green water (`WR_EYE_SKIPCOMP=1`, for comparison). Drawing them flat across the
eye via the overlay path pins them to the viewer's face, ocean and all, along with the racer baked
into the copy — the overlay path is in NDC and therefore head-locked, which is what the HUD wants
and the sea emphatically does not.

What remains visible at speed: the seam at the billboard's edge, and the spray grabs, which are
replayed as billboards too and show as faint squares with pieces of scene inside them.

`--eye-yaw=N` turns the head N degrees. With the view left at identity nothing in the image can be
seen to be head-locked, and a change that pinned the ocean and a copy of the racer to the viewer's
face went through this harness looking perfectly correct. Dump a frame at two yaws: whatever does
not move with the world is locked to the head, which only the HUD should be.

`WR_EYELOG=1` prints each frame's split — every copy with its size, source rect, clear flag, the
draws ahead of it and whether they were replayed — and is the quickest way to tell "the eye
rendered the wrong part" from "the eye rendered nothing". `WR_DUMP_COPIES=20` dumps whenever a
frame holds at least that many copies, which is how a frame thick with spray gets caught: the
faults that only appear at speed are in exactly those frames, and a fixed dump interval almost
never lands on one.

#### Attributing part of the image to the draws that made it

Not a VR problem but found through this harness, and the tools stay because the question recurs:
*which draw put that there?* A race frame is ~390 draws in the flat path.

- `WR_COMPLOG=1` logs every draw that samples a render-to-texture result, with its vertex count,
  texgens and texture ids.
- `WR_DRAWLOG=<frame>` lists every draw in one frame with its index, so geometry drawn twice shows
  up as two draws with identical vertex counts and textures.
- `WR_DRAW_SKIP=a-b` drops a range of draw indices, `WR_NO_COMP` / `WR_ONLY_COMP` drop or isolate
  the draws sampling a whole-frame copy, and `WR_NO_EFBTEX` drops those sampling a partial one.

One caution that has cost time twice: **the emulated timebase is wall-clock driven, so frame N is
not the same moment in two runs.** Any A/B that compares frame N across runs is comparing different
scenes. Either make the comparison inside one run, or pick a selector that is stable from frame to
frame — "draws sampling the water copy" rather than "draws 256 to 273".

### Tuning VR (`vr.txt`)

Some values cannot be known from the source. `a_pos` arrives in the game's own units and nothing
says how many make a metre — get it wrong and the world is giant or doll-sized, which is only
judgeable by wearing the headset. So they are read at startup from
`/sdcard/Android/data/com.example.waverace/files/vr.txt` and can be changed with `adb push`
between runs, no rebuild:

```
units_per_metre 100      # game units per real metre: sets the apparent size of the world
offset_x 0               # viewpoint relative to the game's camera, in game units
offset_y 0               #   x right, y up, z back
offset_z 0
near_m 0.1               # near/far planes in metres; too wide a ratio causes z-fighting
far_m 2000
hud_scale 0.55           # how much of the field of view the 2D overlay occupies
stereo_draw_threshold 300  # perspective draws above which a frame counts as in-world
stereo_switch_frames 30    # frames it must hold before switching
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
