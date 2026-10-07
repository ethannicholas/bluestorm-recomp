# Performance: open issues and measurements

Working notes on where the frame rate stands, what has been fixed, and what is left. The
measurement tools these refer to (`WR_FRAMETIME`, `WR_TIMESCALE`, the scripted route to Ocean
City Harbor) are described in [diagnostics.md](diagnostics.md).

## Open issues

In rough priority order. Everything here reproduces; where there is a lead it is written down so
the next attempt does not start from nothing.

1. **Performance.** Ocean City Harbor did not hold 30 fps. Dropping the per-display-frame repaint
   bought about 15%. It is **not** the pixel pipeline: raising the EFB from 640×528 to 2560×2112,
   sixteen times the pixels, costs about 2% and nothing beyond that scales with area — see
   [How many pixels the theater panel gets](vr.md#how-many-pixels-the-theater-panel-gets). It is CPU
   work, on both threads, and `WR_FRAMETIME=1` says which.

   Done so far, profiled on a Mac with the scripted route in [diagnostics.md](diagnostics.md): the brief drop to
   single digits at a fixed spot was the render thread compiling a burst of new TEV shaders on
   first use, now built at startup from a cache (see [Shader cache](graphics.md#shader-cache)); the GX
   front end on the guest thread went from 5.5 to 3.1 ms a frame (a pixel-state snapshot per
   draw command that was identical 12 times in 13, vertices repeated to make triangle lists
   rather than indexed, a batch reallocated from nothing every frame, a serial texture hash);
   and the render thread from 4.9 to 3.2 ms (a full state re-application per draw where 94% of
   them change one texture). The guest ceiling in the race is now ~115 fps on an M-series Mac,
   8.7 ms a frame, two thirds of it the recompiled game code — which `-O3` does not help.

   What is left, in order of likely value on the headset: the stereo path issues every draw
   three times (one flat pass into the EFB, then one per eye); the flat pass's main-scene draws
   feed only copies the eyes substitute, except the partial copy the submerged tint samples,
   which is why they cannot simply be skipped. A thousand draws per pass is a thousand driver
   calls, and the remaining per-vertex work (a matrix multiply, the texgens, a 116-byte vertex)
   only moves off the guest thread by doing the transform in the vertex shader.

## A note on measuring performance here

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

The draw-call shape is still worth knowing: the game issues some 13,000 GX draw commands per race
frame, averaging five vertices each, which the front end merges into ~1,000 draws with ~1,000
distinct pixel states between them. Measured over a million consecutive pairs of those states,
94% differ in a texture and almost nothing else -- projection, viewport, scissor, fog and the
indirect matrices are the same more than 99% of the time, the program 81%. So `apply_state`
shadows what it last put into GL and issues only the difference, which makes the typical state
change a texture bind and its size rather than forty calls. (Deduplicating `PixelState` by
content, suggested here earlier, would not have helped: consecutive states really are different,
just barely.) But it is an optimisation, not the explanation for a slideshow that was really a
memory leak.
