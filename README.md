# bluestorm-recomp

A static recompilation of *Wave Race: Blue Storm* (Nintendo GameCube, 2001) to native code,
for macOS, Windows, and the Meta Quest 3.

The game's PowerPC executable is translated ahead of time into C, then compiled and linked
against a runtime that provides virtual GameCube hardware (graphics, audio, DVD, controllers,
memory card). The result is a native program that runs without an emulator.

This recompilation contains several enhancements over the original:

* VR support, both PCVR and standalone
* 60fps support (but see Known Issues)
* First person camera (toggleable with a right stick click)
* Telemetry support for motion simulators

> **This repository contains no game code or data.** You must supply your own disc image,
> dumped from a copy of the game that you own. The build process reads the executable out of
> the disc image and recompiles it locally. No game code or assets are distributed here,

## License

The code in this repository is released under the [MIT License](LICENSE).

## Legal

- *Wave Race: Blue Storm*, GameCube and all related names and marks are property of Nintendo.
  This project is not affiliated with or endorsed by Nintendo.
- This repository contains only original source code (the recompiler, the hardware/runtime
  layer and the renderer) plus metadata describing code layout (function addresses and
  names in `analysis/`). It contains no copyrighted game code, assets, BIOS/IPL files or
  DSP microcode.
- You need a legally obtained copy of the game. Dump it from your own disc (e.g. with
  [CleanRip](https://wiibrew.org/wiki/CleanRip)). Do not download game images, and do not
  share the image, the extracted executable, the generated C code (`build/gen/`), built
  binaries, or memory card files, since all of these contain or are derived from the game.

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

With [winget](https://learn.microsoft.com/windows/package-manager/):

```powershell
winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.13 LLVM.LLVM Microsoft.VisualStudio.2022.BuildTools
```

**Quest 3**: see [Running on a Quest 3](#running-on-a-quest-3) below.

### 1. Provide your game image

Dump your disc, then place the image in `rom/` as an `.iso` or `.ciso`, e.g.:

```
rom/game.iso
```

If your dump is in another format (`.rvz`, `.gcz`), convert it to ISO first, for example with
Dolphin (right-click the game → *Convert File…* → *Uncompressed Disc Image*), or with
`dolphin-tool convert`, or [`nodtool`](https://github.com/encounter/nod):

```sh
cargo install --locked nodtool
nodtool convert "rom/Wave Race - Blue Storm (USA).rvz" rom/game.iso
```

`nodtool` also ships prebuilt binaries (including Windows ARM64) on its
[releases page](https://github.com/encounter/nod/releases), which avoids needing Rust. Note that
Dolphin only gained RVZ support in 5.0-12188, so the 5.0 release that package managers offer
cannot convert one.

The build uses the first `.iso` it finds in `rom/`. To use an image elsewhere, pass
`-DGAME_ISO=/path/to/game.iso` when building.

### 2. Build

```sh
./build.sh          # macOS
```

```powershell
.\build.ps1         # Windows
```

### 3. Run

```sh
./build/waverace            # uses the configured image / rom/*.iso
./build/waverace path/to/game.iso
```

Run it from the repository root: the memory card is created in `saves/` relative to the
current directory.

There may be some shader compilation stutters during your first playthrough, but the shader cache
should take care of this on subsequent runs.

## Playing in VR on a PC

On Windows, `--vr` plays the game on a PCVR headset through OpenXR, using whichever runtime you
have set as active.

```powershell
.\build\waverace.exe --vr
```

The same [`vr.txt`](#tuning-the-vr-view) settings apply, read from a `vr.txt` in the directory you
run the game from. To use a runtime other than the active one, point `XR_RUNTIME_JSON` at its
manifest, for example:

```powershell
$env:XR_RUNTIME_JSON = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json'
```

## Motion platforms

`--telemetry` sends the jet ski's motion to a motion simulator, in the format of
[SimRacingStudio](https://www.simracingstudio.com/)'s custom game API: UDP packets to
`127.0.0.1:33001`, which is where SimRacingStudio listens by default. `--telemetry=host:port` sends
them somewhere else.

Every channel can be scaled, inverted or limited with a `telemetry.txt` in the directory you run
the game from (all optional):

```
pitch_gain 2        # multiply each channel; a negative value inverts it
roll_gain 1
yaw_gain 1
surge_gain 1        # forward acceleration
sway_gain 1         # sideways acceleration
heave_gain 1        # vertical acceleration
fade_s 1            # seconds to ease in and out of rest, and to fade across a respawn
max_angle 0         # the most pitch, roll or yaw sent, in degrees; 0 is no limit
angle_rate 0        #   and how fast pitch and roll may change, in degrees a second
yaw_rate 0          #   and yaw
max_accel 0         # the most acceleration sent, in m/s^2
accel_rate 0        #   and how fast it may change, in m/s^2 a second
slip_deadzone 0     # the platform turns by how far the ski's nose is off the way it is
slip_gain 1         #   going: past this many degrees, by this much of the rest,
slip_max 0          #   up to this many degrees (0 is no limit),
slip_s 0            #   eased over this many seconds
raw 1               # 0 to 1: how much of the hull's own tilt to use, rather than the
                    #   first-person view's gentler, smoothed one
impact 0.25         # how hard collisions and landings knock; 0 turns them off
impact_threshold 50 #   how big a hit has to be to knock, in m/s^2
impact_s 0.2        #   seconds a knock takes to fade
heave_s 0.15        # extra seconds of smoothing on the vertical; higher is gentler on chop
chop 0.03           # how much of the chop's buzz comes through; 0 turns it off
engine_idle 1500    # the engine's revs at rest
engine_redline 7000 #   at engine_top_kmh
engine_top_kmh 120
engine_max 9000     #   and during a turbo boost
ski_accel_s 0.15    # seconds over which accelerations are smoothed; higher is gentler
ski_velocity_s 0.05 # the same for the velocity they are taken from
log 0               # 1 prints what is sent, twice a second
```

## Running on a Quest 3

### Requirements

On your PC:

- [Android NDK](https://developer.android.com/ndk/downloads), unpacked to
  `%LOCALAPPDATA%\Android\Sdk\ndk\<version>\`
- [platform-tools](https://developer.android.com/tools/releases/platform-tools) (for `adb`),
  unpacked to `%LOCALAPPDATA%\Android\Sdk\platform-tools\`
- The Android SDK's `build-tools;34.0.0` and `platforms;android-34`, installed with
  `sdkmanager` from the
  [command-line tools](https://developer.android.com/studio#command-line-tools-only)
  (run `sdkmanager --licenses` first), so that they land under `%LOCALAPPDATA%\Android\Sdk\`
- A JDK, for signing the APK: `winget install Microsoft.OpenJDK.21`
- CMake, Ninja and Python, as for the desktop build

On your Quest 3:

- [Developer mode](https://developer.oculus.com/documentation/native/android/mobile-device-setup/)
  enabled (this needs a Meta developer account, set up through the Meta Horizon phone app).
- Connect it over USB, put the headset on, and accept the *Allow USB debugging* prompt. Check
  that `adb devices` lists it as `device`.

### Build and install

With your disc image in `rom/` as described above:

```powershell
.\build-android.ps1          # cross-compiles libwaverace.so for arm64
.\package-apk.ps1 -Install   # packages the APK, installs it, and pushes the disc image
```

The disc image is not part of the APK. It is copied to the app's data directory on the headset,
`/sdcard/Android/data/com.ethannicholas.waverace/files/game.iso`, which may take a few minutes the
first time. Your save file (the emulated memory card) and the shader cache live in the same
directory, and survive reinstalling the app.

### Playing

The app appears on the headset under **Library → Unknown Sources → Wave Race: Blue Storm Recomp**.

### Tuning the VR view

Scale, comfort and quality settings are read at startup from
`/sdcard/Android/data/com.ethannicholas.waverace/files/vr.txt`, so they can be changed with
`adb push` between runs without rebuilding. The file is optional; every key has a default. One
`key value` per line, `#` starts a comment:

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
eye_scale 1.4            # eye render size, as a multiple of what the runtime recommends
msaa 4                   # samples per eye pixel; 0 for none
start_in_stereo 0        # start in stereo rather than theater
transition_s 1           # seconds the morph between the two views takes; 0 snaps
transition_out_s -1      # seconds for the morph back to theater; negative uses transition_s
theater_stereo 0         # show the theater screen as a stereo pair (the water's reflections suffer)
theater_depth 1          # how much of your eye separation the pair is drawn with; less is flatter
panel_band 0             # depth-buffer band of draws pinned to the screen itself; 0 pins none
perf_boost 1             # ask the headset for its highest CPU and GPU clocks
fps 60                   # the race's frame rate, 60 or 30; the menus always run at the game's 30
first_person 0           # start races in first person rather than behind the chase camera
fp_x 0                   # where the first-person eye sits relative to the ski, in game units:
fp_y 57.5                #   x right, y up, z forward
fp_z -46.5
fp_up_m 0                # nudge the first-person eye up (metres; negative is down)
fp_forward_m 0           #   and forward (negative is back)
fp_tilt 0.6              # how much of the ski's pitch and roll the first-person eye takes
fp_tilt_s 0.15           # seconds it takes to follow the ski's pitch and roll,
fp_yaw_s 0.1             #   its heading,
fp_height_s 0            #   and its height; 0 follows exactly
```

## Controls

On a Mac / Windows computer:

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

On a Quest 3:

| GameCube | Touch controller |
|---|---|
| Control stick | Left thumbstick |
| C-stick | Right thumbstick |
| A / B | A / B (right) |
| X / Y | X / Y (left) |
| Z | Right grip |
| L / R | Left / right trigger |
| Start | Menu (left) |
| D-pad | Left thumbstick while holding the left grip |
| *(toggle first person)* | Right thumbstick click |

## Command-line options

| Option | Description |
|---|---|
| `--scale=N` | Internal render resolution multiplier (default 2) |
| `--hidden` | Don't show the window (testing) |
| `--headless` | Run without graphics or audio output |
| `--dump-dir=DIR --dump-every=N` | Save every Nth frame as a PNG |
| `--eye` | Show the VR eye view instead of the flat frame (testing) |
| `--first-person[=x,y,z]` | With `--eye`, view from the rider's seat, optionally at that offset from the ski |
| `--vr` | Play on a PC VR headset (Windows; see [Playing in VR on a PC](#playing-in-vr-on-a-pc)) |
| `--telemetry[=host:port]` | Send motion to a motion platform (see [Motion platforms](#motion-platforms)) |
| `--no-music` | Play without the soundtrack (sound effects stay), for recording video |
| `--record=FILE` | Record a movie of the game with its sound -- in VR, what your left eye sees; needs [ffmpeg](https://ffmpeg.org/) on the PATH. Written when you quit. |
| `--record-size=WxH` | The movie's size (default 2560x1440) |
| `--sample` | Print periodic profiling samples of what the game code is doing |
| `--log-all` | Verbose hardware logging |

The build also produces `waverace_bench`, which runs the game with no graphics, audio or input
and reports how fast the recompiled code advances, for comparing machines:

```sh
./build/waverace_bench --seconds=25
```

## Graphics

The renderer needs an **OpenGL 3.3 core profile** driver on the desktop, or **OpenGL ES 3.2** on
Android. It uses nothing newer than GL 3.3 / ES 3.0, so compatibility layers that stop there
(such as Mesa on Direct3D 12) work.

## Files the game writes

| File | What |
|---|---|
| `saves/memcard_a.raw` | The emulated memory card. Back this up to keep your save. |
| `saves/shaders.bin` | The shader cache. Safe to delete; the next run rebuilds it. |
| `saves/panic.txt` | Written if the game stops with its own `guest panic:` assertion. |
| `saves/inputs/` | One directory per run: the controller input it read, with a copy of the memory card as it was. Safe to delete. |
| `saves/view.txt` | With `--vr`, whether you last chose first person or the chase camera. |

On the Quest all of these are in `/sdcard/Android/data/com.ethannicholas.waverace/files/`, beside the
disc image.

## How it works

The recompiler, the GameCube runtime, the renderer and the desktop frontend are
[gcn-recomp](https://github.com/ethannicholas/gcn-recomp), a git submodule.
This repository holds what is specific to Wave Race: the function layout (`analysis/`), the
recompiler's steering tables (`recomp/*.txt`) and the game's own answers to the headset
frontend's questions (`runtime/`).

```
rom/game.iso ──extract_dol.py──> build/main.dol ──recomp.py──> build/gen/*.c ──┐
                                                                              ├─> waverace
                               gcn-recomp/runtime/ (hardware + renderer) ─────┘
```

- **Recompiler** (`gcn-recomp/recomp/`): a Gekko (PowerPC 750CL with paired singles) to C
  translator. Function boundaries and jump tables come from
  [decomp-toolkit](https://github.com/encounter/decomp-toolkit) analysis (`analysis/symbols.txt`);
  every function becomes a C function operating on a CPU state struct. Indirect branches go
  through a lookup table; `recomp/patches.txt` can replace individual instructions.
- **CPU/OS** (`gcn-recomp/runtime/cpu.cpp`, `threads.cpp`): memory is a flat host allocation
  indexed by guest address. The game's own OS runs unmodified. Only context switching
  (`OSSaveContext`/`OSLoadContext`) is replaced: each guest thread runs on a host thread and
  resumes via `setjmp`/`longjmp`. Interrupts are delivered at loop back-edges.
- **Hardware** (`gcn-recomp/runtime/hw/`): register-level emulation of the processor interface, video
  interface, DVD interface, serial interface (controllers), EXI (IPL/RTC/SRAM, memory card),
  audio interface and DSP interface, with high-level emulation of the DSP's boot ROM, the AX
  audio ucode and the memory card unlock ucode.
- **Graphics** (`gcn-recomp/runtime/gx/`): the GX FIFO is parsed as the game writes it. Vertex transform,
  lighting and texture coordinate generation run on the CPU, and the per-pixel TEV stages are
  compiled into GLSL shaders. Draw batches are handed to an OpenGL renderer on the main thread.
- **Audio** (`gcn-recomp/runtime/audio.cpp`): the AX mix and the DVD-streamed music are resampled into one
  stereo stream, played through SDL on the desktop and AAudio on Android.
- **VR** (`gcn-recomp/android/openxr_main.cpp`): an OpenXR frontend that shows the flat frame on
  a quad layer, or re-projects the game's draws per eye for stereo. When to do which, and where
  the first-person eye sits on the ski, are this game's answers in `runtime/vr_waverace.cpp`,
  `runtime/start_rig.cpp` and `runtime/first_person.cpp`.

Design notes, measurements and the record of how the harder problems were solved are in
[`docs/dev/`](docs/dev/).

## Tools

- `gcn-recomp/tools/fetch_dtk.sh`: downloads decomp-toolkit, used to regenerate `analysis/`
  (`dtk dol split analysis/config.yml analysis/out` after a build has produced `build/main.dol`).
- `tools/dis`: prints the disassembly of a function from dtk's output.
- `gcn-recomp/tools/contact.py`: tiles dumped frames into a contact sheet.
- `gcn-recomp/tools/glprobe.c` (Windows): reports the OpenGL versions the host can actually
  create, to diagnose the "too old" failure in [Graphics](#graphics).
  Build with `clang gcn-recomp/tools/glprobe.c -o build/glprobe.exe -lopengl32 -lgdi32 -luser32`.
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
