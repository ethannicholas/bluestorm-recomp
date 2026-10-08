# Diagnostics and measurement

What is specific to driving, measuring and debugging *this* game. The shared tooling -- the
`GCN_*` environment variables, scripted input, input logging and replay, frame dumps, the eye
preview, the benchmark, the guest call stack, panics and heap tracing -- is documented in
[`gcn-recomp/docs/diagnostics.md`](../../gcn-recomp/docs/diagnostics.md); this file does not
repeat it.

## Environment variables on the headset

An APK launched from the headset's own launcher has no environment, so `gcn_env.txt` beside
`vr.txt` (`/sdcard/Android/data/com.example.waverace/files/`) stands in for it: one
`KEY=VALUE` per line, applied before the runtime starts. It works for anything read lazily,
which is most of the `GCN_*` switches; a static initializer that read its variable at library
load (`GCN_FRAMETIME` is one) has already run. `gcn_input.txt` in the same place is the
`GCN_INPUT` script.

## Recording a run on the headset, and playing it back

The VR app records every run the way the desktop frontend does: the controller state the
guest polled, keyed by presented frame, into `<files>/inputs/<timestamp>/inputs.txt`, with a
snapshot of the memory card beside it. That is how a crash that only happens on the headset
gets a route a desktop can follow:

```sh
adb pull /sdcard/Android/data/com.example.waverace/files/inputs/20261007-225215 /tmp/run
./build/waverace --replay=/tmp/run
```

The replay runs on a scratch copy of the logged card, so it starts from the same saves and
leaves `saves/memcard_a.raw` alone. It is not deterministic -- the guest runs on wall-clock
time and the headset and a Mac do not fade a menu in the same number of frames -- so a long
route can drift; watch it, and if it leaves the rails, trim the log or re-record from the last
point it reached. `GCN_REPLAY=inputs/<timestamp>` in `gcn_env.txt` plays a log back on the
headset itself, and `GCN_NO_INPUT_LOG=1` there turns recording off. Each log holds a copy of
the card (512 KB), so the directory is worth clearing now and then.

## A route to Ocean City Harbor

A deterministic route to Ocean City Harbor, the course that has been the performance
benchmark, is `GCN_INPUT='600:START:10,800:START:10,1000:START:10,1200:A:10,1400:DOWN:10,
1500:A:10,1700:A:10,1900:A:10,2250:RIGHT:5,2350:RIGHT:5,2450:RIGHT:5,2600:A:10,3000:A:4000'`
(title, memory card, Time Attack / Normal, the default rider, three courses to the right, then
hold the throttle). The first three presses are the three logo screens; the frame counts are
loose because the menus fade in on wall-clock time, so a press that lands during a transition
is lost. Course select cycles Lost Temple Lagoon, Southern Island, Aspen Lake, Ocean City
Harbor; the three RIGHTs assume it opens on the first of those, as it did here, so check a
frame dump if a different save opens it elsewhere. Without the DOWN press the same route
starts a Championship instead.

A scripted rider cannot finish a race: the throttle alone drives him into the first wall and
he stays there, and a race does not end for a rider who does not finish -- eight minutes beached
on Dolphin Park with the whole field home, and it is still lap 1. Anything past a race's end has
to be played, or replayed from a recorded run. The pause menu (Start) offers Continue, Restart,
Change Character, Change Course and Main Menu, so `START, DOWN, DOWN, DOWN, A` from a race returns
to course select, which is how the heap was cycled below.

## Measuring the guest ceiling

`waverace_bench --seconds=25` is the shared benchmark (see the shared notes for the method).
`GCN_TIMESCALE=N` makes the guest try to run at N times real time; raise it until the frame
rate stops climbing and that plateau is the machine's ceiling:

| | 1.0 | 1.5 | 2.0 | 3.0 |
|---|---|---|---|---|
| Quest 3 (6× Cortex-A78C, 2.05/2.36 GHz) | 29.9 | 38.3 | 43.1 | **48.5** |
| VMware guest, 2 vCPUs of Apple Silicon | 20.5 | – | 29.9 | **30.7** |

The Quest tops out near 48 fps -- about 1.6× real time, so the 30 fps simulation leaves roughly
60% headroom there. The second row shows why a single measurement at 1.0 can mislead: 20.5 fps
is *below* that machine's own 30.7 fps ceiling, which throughput alone cannot explain. With only
two cores, the guest thread contends with the 200 µs ticker and the batch drain and overshoots
its retrace waits, so it is latency-bound rather than CPU-bound. Prefer a machine with cores to
spare when measuring, and read the ceiling rather than the 1.0 figure.

Guest execution is serialised: many guest threads exist but exactly one runs at a time (see
`gcn-recomp/runtime/threads.cpp`), so extra cores do not raise this ceiling. The vertex
throughput column matters for the same reason -- CPU-side transform and lighting run on that
one thread.

The guest call stack (`-DGCN_TRACE_CALLS=ON`) was expected to cost frames and does not: on a
Quest 3 at `GCN_TIMESCALE=3`, running OFF/ON/OFF, the steady state was 40.60, 41.34 and 40.84
fps -- the instrumented build measured *faster*, consistently. A push per call and a pop per
return are below the noise floor here, and the couple of per cent between builds is code
layout. It stays out of a release build because it is diagnostic machinery, not to buy back
frames.

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
already on the device at the right size. `-DGCN_BENCH_ONLY=ON` is what makes a build with no
SDL2, no GL and no renderer possible.

## Validating the ES renderer on a device

`waverace_egl` (Android) runs the real renderer on a headless EGL pbuffer and writes frames as
PNGs. No APK, no window, no OpenXR -- it exists to check the ES back end on real hardware:

```powershell
.\build-android.ps1 -Render -Seconds 200 -DumpEvery 150 -InputScript "1050:START:12,1250:A:12,1450:A:12"
```

`-InputScript` sets `GCN_INPUT`. Frames are pulled back to `build-android/frames/`. Without
input the game loops its attract movie, which is a single textured quad per frame and exercises
nothing.

`--eye` renders through the stereo path instead, into an offscreen eye target, and dumps that.
Changes to the VR renderer are visible here without putting the headset on, which is the only
practical way to tell a wrong frame split from a wrong projection:

```powershell
adb shell "cd /data/local/tmp && GCN_INPUT='600:START:10,800:START:10,1000:START:10,1200:A:10,1400:A:10,1600:A:10' \
  ./waverace_egl --eye --seconds=150 --dump-dir=/data/local/tmp/eye --dump-every=500 <iso>"
```

The desktop build has the same `--eye` and `--first-person` (shared notes); for this game the
anchor defaults to the rider's seat, and `GCN_FPLOG=1` prints, per frame, where the hull was
found and how many of the rider's draws were left out -- or that no racer was found:

```sh
GCN_FPLOG=1 GCN_INPUT='<the Ocean City Harbor route above>' \
  ./build/waverace --hidden --eye --first-person --dump-dir=/tmp/fp --dump-every=100
```

`GCN_GP_STACK=<hex word>` prints the guest call stack the first few times that word is pushed
into the write-gather pipe, which is how the countdown rig's placement was traced to
`fn_800D4260`; `GCN_POSMTX_STACK=<z>` does the same for a position matrix load. Ask at the
write end of the FIFO: at the drain the stack is six frames straight to `main`, identical for
every command in the frame.

## When the game stops on purpose

The game has its own assert, `OSPanic` at `0x8010A700`, which prints through the SDK's own
vprintf -- not `OSReport`, the only printing path the runtime listens to -- and then spins in
`PPCHalt` with interrupts disabled. Recompiled faithfully that looked exactly like the port
deadlocking, and on a headset it cost a session of profiling to tell the two apart. So it is
HLE'd (`recomp/names.txt` names it, `recomp/hle.txt` lists it), and the report lands in
stderr, the crash record and `panic.txt` -- on the headset in the external files directory,
`/sdcard/Android/data/com.example.waverace/files/`, next to `vr.txt`; the app's private files
directory is empty.

### When the heap runs out

The panic this was all built for:

```
guest panic: wrMainMenu.c:13626: sInitCourseMenu: error, cannot allocate memory for the frame buffer copy
guest backtrace (innermost first):
    800AFDC0  fn_800AFCDC+0xE4
    80006D60  fn_80006D20+0x40
    80006B20  main+0x400
    800031EC  __start+0xEC
```

It is `OSAllocFromHeap` returning NULL for the course menu's 219,520-byte frame-buffer copy,
on a Quest 3, in Championship, right after a race was finished. The report now ends with the
heaps, and `GCN_HEAP=1` names who holds them (shared notes; the four hooks are in
`recomp/patches.txt`, with `HeapArray` at `0x80691E60`). The game creates two: heap 0 of
16.4 MB, where the frame-buffer copy goes, and heap 1 of 512,000 bytes for the small
allocations behind the `fn_80036AA0` wrapper, which runs at 97% full during a race.

What the tracer found on the desktop, driving Time Attack with the scripted route and then
cycling pause → Change Course → course select four times: no leak. Heap 0 holds 5.0 MB at the
course menu, 12.5–13.6 MB during a race, and returns to exactly 5.8 MB after every Change
Course, the extra 1.2 MB over the first visit being one cell from `fn_801044A4` that appears
after the first race and stays constant from then on. The same with the headset's own memory
card. A Championship race with the full field holds 12.5 MB with 3.9 MB free in one block, so
the 219 KB copy fits comfortably in every state a script can reach.

The crash was after *finishing* a Championship race, which a script cannot do (above). So the
finished-race path -- results, standings, then the next course's menu -- is the one state not
yet measured, and the working hypothesis is that it keeps something a Change Course frees:
3.9 MB of headroom is one race's worth of leak, and a session of two or three races fits the
seven minutes the process lived. The next data point is a `panic.txt` from a build with
`GCN_HEAP=1` in `gcn_env.txt`, or the input log of a crashing session replayed on the desktop
with `GCN_HEAP=300`.
