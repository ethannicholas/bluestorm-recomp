#!/usr/bin/env python3
"""Compare the rider's path between two runs of the headless harness.

    compare_runs.py <log A> <fps A> <log B> <fps B> [--step S] [--from T] [--to T]

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


def load(path):
    out = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m:
            out[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5)))
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
    ra, rb = load(a), load(b)
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
