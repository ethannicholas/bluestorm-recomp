#!/usr/bin/env python3
"""Compare the rider's path between two runs of the headless harness.

    compare_runs.py <log A> <fps A> <log B> <fps B> [--step S] [--from T] [--to T] [--reset N]

Each log is the stderr of `waverace_egl --eye` (or the desktop `waverace --eye`) run with
WR_FPLOG=1, whose first-person eye hook prints, per presented frame, where the rider's
eye is in the world: `[fp] f<frame> hull at ... eye at world (x, y, z) yaw <deg>`. The
two runs are lined up by time (frame / fps) and, every S seconds, the position in each,
the distance between them and the speed each is moving at are printed. A run at 60 fps
that keeps the game's per-frame constants moves at twice the speed of the 30 fps run it
replays; one that has them right drifts slowly.
"""
import math
import re
import sys

LINE = re.compile(r'\[fp\] f(\d+) hull at .*eye at world \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) yaw ([-\d.]+)')
# A headless run's per-frame watch (WR_WATCH, runtime/pace.cpp): the first three watched
# words are taken as a position, keyed by the presented frame.
WW = re.compile(r'\[ww\] f(\d+) p(\d+)((?: [0-9A-F]{8}=[0-9A-F]{8}/\S+)+)')


def load(path, reset=0):
    """The positions of a run by presented frame. With reset=N, frames are counted from
    the N-th time the game's frame counter (the first watched word) reads 0, so that two
    runs can be lined up on the scene they both reach."""
    out = {}
    resets = 0
    origin = 0
    prev_f = -1
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m:
            out[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5)))
            continue
        m = WW.search(line)
        if m:
            p = int(m.group(2))
            f = int(m.group(1))
            if f == 0 and prev_f != 0 and reset:  # the counter may sit at 0 for a frame or two
                resets += 1
                if resets == reset:
                    origin = p
                    out = {}
            prev_f = f
            vals = [float(w.split('/')[1]) for w in m.group(3).split()]
            if len(vals) >= 4:
                out[p - origin] = (vals[1], vals[2], vals[3], 0.0)
    return out


def at(run, fps, t):
    f = int(round(t * fps))
    for d in range(0, 4):
        for k in (f - d, f + d):
            if k in run:
                return k, run[k]
    return None, None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '1') for a in sys.argv[1:] if a.startswith('--'))
    a, fa, b, fb = args[0], float(args[1]), args[2], float(args[3])
    step = float(opts.get('step', 1.0))
    reset = int(opts.get('reset', 0))
    ra, rb = load(a, reset), load(b, reset)
    if not ra or not rb:
        print(f'no eye positions in {a if not ra else b}; was WR_FPLOG=1 set, and --eye?')
        return 1
    t0 = float(opts.get('from', max(min(ra) / fa, min(rb) / fb)))
    t1 = float(opts.get('to', min(max(ra) / fa, max(rb) / fb)))
    print(f'A: {len(ra)} frames at {fa:g} fps ({min(ra) / fa:.1f}-{max(ra) / fa:.1f} s)   '
          f'B: {len(rb)} frames at {fb:g} fps ({min(rb) / fb:.1f}-{max(rb) / fb:.1f} s)')
    print(f'{"t":>6} | {"A x":>8} {"A z":>8} {"A yaw":>6} {"A speed":>8} | {"B x":>8} {"B z":>8} {"B yaw":>6} {"B speed":>8} | {"apart":>8}')
    prev_a = prev_b = None
    t = t0
    while t <= t1 + 1e-6:
        ka, pa = at(ra, fa, t)
        kb, pb = at(rb, fb, t)
        if pa and pb:
            sa = sb = float('nan')
            if prev_a:
                sa = math.dist(pa[:3], prev_a[:3]) / step
            if prev_b:
                sb = math.dist(pb[:3], prev_b[:3]) / step
            apart = math.dist(pa[:3], pb[:3])
            print(f'{t:6.1f} | {pa[0]:8.1f} {pa[2]:8.1f} {pa[3]:6.1f} {sa:8.1f} | {pb[0]:8.1f} {pb[2]:8.1f} {pb[3]:6.1f} {sb:8.1f} | {apart:8.1f}')
            prev_a, prev_b = pa, pb
        t += step
    return 0


if __name__ == '__main__':
    sys.exit(main())
