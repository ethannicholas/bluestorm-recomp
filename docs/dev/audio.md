# Audio notes

How the mix is structured, why AAudio rather than Oboe, and how to test the audio path on a
headset without wearing it.

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
