# Diagnostics and measurement

How to measure the port, drive it without a controller, and find out which guest code or which
draw is responsible for something. Most of this is environment variables and headless harnesses;
none of it is needed to play the game.

## Scripted input

`WR_INPUT="frame:BUTTON:duration,..."` presses buttons at given submitted-frame counts, which is
how anything past the title screen is reached without a controller. On a headset the same script
is read from `wr_input.txt` beside `vr.txt`. The presses are counted in submitted game frames,
not wall-clock, so a script reaches the same place whatever the frame rate; `WR_INPUT_LOG=1`
prints each one as it fires.

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

## Measuring the guest ceiling

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

## Measuring headroom

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

## Benchmarking on an Android device (e.g. a Quest headset)

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

## Validating the ES renderer on a device

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

The desktop build has the same `--eye`: it shows the eye in the window instead of the flat frame,
and `--dump-dir`/`--dump-every` write the eye's frames as `eye_NNNNN.png`. `--first-person`
(optionally `=x,y,z`, the anchor in game units) puts the eye on the ski the way the thumbstick
click does in the headset, and `WR_FPLOG=1` prints, per frame, where the hull was found and how
many of the rider's draws were left out -- or that no racer was found. It is the quickest way to
look at the first-person view, since the headset cannot be driven from this Mac:

```sh
WR_FPLOG=1 WR_INPUT='<the Ocean City Harbor route above>' \
  ./build/waverace --hidden --eye --first-person --dump-dir=/tmp/fp --dump-every=100
```

## Which guest function did that?

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

## When the game stops on purpose

The game has its own assert: `OSPanic(file, line, format, ...)` at `0x8010A700`, which prints a
message and a backtrace and then falls into `PPCHalt` at `0x80108110`, four instructions ending
in a branch to themselves, with interrupts disabled. Recompiled faithfully, that is a disaster to
diagnose. The process sits
there spinning on `IRQ_CHECK`, producing no frames and no audio, with every profile sample in
`irq_poll`, and **says nothing**: the panic text goes through the SDK's own vprintf, not through
`OSReport`, which is the only printing path the runtime listens to. It looks precisely like the
port having deadlocked, and on a headset it cost a session of profiling to tell the two apart.

So `OSPanic` is HLE'd (`recomp/names.txt` names it, `recomp/hle.txt` lists it, `hle_OSPanic` in
`runtime/hle_os.cpp` implements it). It formats the game's own message, walks the backchain for a
guest backtrace with names, and then stops instead of spinning — the game is over either way, and
a process that dies explaining itself beats one that freezes:

```
guest panic: wrMainMenu.c:13626: sInitCourseMenu: error, cannot allocate memory for the frame buffer copy
guest backtrace (innermost first):
    800AFDC0  fn_800AFCDC+0xE4
    ...
```

It lands in three places, because each loses in a different way. stderr, which on a headset is a
log ring the app can lap within a minute. `fatal()`'s crash record, which survives that but holds
only the first line. And `<files>/panic.txt` (`saves/panic.txt` on desktop), which keeps the whole
report including the backtrace until the next panic overwrites it — that is the one to read after
the fact, and the one that does not need anyone to have been watching.
