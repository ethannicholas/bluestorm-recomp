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
controller. Frames are pulled back to `build-android/frames/`.

### Known performance problem on mobile GPUs

The renderer applies one `PixelState` per draw call, and the game issues **~830 draws per frame**
during a race. Each one does a `glUseProgram`, a full uniform re-upload and eight texture and
sampler binds, for an average of only ~106 vertices. Desktop drivers absorb that; a tiled mobile
GPU does not. Measured on a Quest 3 (Adreno 740) at `--scale=1`: menus run at ~30 fps, a race at
**0.5–3 fps**. Texture uploads (1 new texture per frame) and EFB copies (1 per frame) are *not*
the cause — it is state-change overhead. Deduplicating consecutive identical states and merging
adjacent draws that share one would be the fix.

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
