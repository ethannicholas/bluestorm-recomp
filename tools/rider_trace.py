#!/usr/bin/env python3
"""Tabulate rider 0's dynamics from per-frame snapshots of its record.

    rider_trace.py <dir> <rows per second> [--from T] [--to T] [--every DT] [--p0 P] [--lo HEX]

<dir> holds snap_<p>.bin files from the harness run with
WR_SNAP_RANGE=80620298-806214C4 WR_SNAP=<dir>:<p>-<q> (the saved positions at 0x80620298,
then the record at 0x80620328; --lo gives another range's start, 80340000 for a whole
data-region snapshot). Rows per second is 30 or 60 (the presented rate), --p0 the presented
frame of the race reset (3031 on the Ocean City Time Attack route), so that t is race time.

Columns: speed (+104, the centroid's displacement per physics pass), head_v (the velocity
direction +92..100, degrees), head_f (the hull's forward axis +108..116), slip = head_v -
head_f, roll (the right axis +132..140 against the horizon: the hull is twelve particles in
three rows of three, 0/3/6/9 the left bottom row, 2/5/7/10 the right, 1/4/8/11 the top),
pitch, yawrt (head_f's rate, deg/s), stk (+3280), jet (the vectored thrust's angle from
+3768/+3772), pow (+3344), wall (+3512), st (+4036), the hull particles' mean momentum on
the hull axes (the accumulator at +12 of each particle, which holds the next pass's
momentum at frame end), the heights of particles 0, 3, 2 and 5, and the centroid's
displacement (+80..88) with its lateral part. docs/dev/performance.md, "The steering,
seventh pass", is the reading of the record this follows.
"""
import glob, math, os, struct, sys

def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    d, rps = a[0], float(a[1])
    t0, t1, every, p0, lo = 0.0, 1e9, 1.0 / 30, 3031, 0x80620298
    for i in range(2, len(a), 2):
        if a[i] == '--from': t0 = float(a[i+1])
        elif a[i] == '--to': t1 = float(a[i+1])
        elif a[i] == '--every': every = float(a[i+1])
        elif a[i] == '--p0': p0 = int(a[i+1])
        elif a[i] == '--lo': lo = int(a[i+1], 16)
    R = 0x80620328 - lo
    def hd(v): return math.degrees(math.atan2(v[0], v[2]))
    def dot(x, y): return sum(p * q for p, q in zip(x, y))
    files = sorted(glob.glob(os.path.join(d, 'snap_*.bin')), key=lambda s: int(s.rsplit('_', 1)[1][:-4]))
    print('#    t     p   speed  head_v  head_f   slip  roll   pitch  yawrt   stk  jet   pow  wall st  lat_mom  fwd_mom  up_mom  h0  h3  h2  h5  cdisp lat_d')
    prev = None; last = None
    for path in files:
        p = int(path.rsplit('_', 1)[1][:-4])
        t = (p - p0) / rps
        if t < t0 or t > t1: continue
        b = open(path, 'rb').read()
        f = lambda o: struct.unpack('>f', b[R+o:R+o+4])[0]
        i32 = lambda o: struct.unpack('>i', b[R+o:R+o+4])[0]
        v = lambda o: (f(o), f(o+4), f(o+8))
        fwd, up, right, dirv = v(108), v(120), v(132), v(92)
        head_f, head_v = hd(fwd), hd(dirv)
        slip = (head_v - head_f + 180) % 360 - 180
        roll = math.degrees(math.asin(max(-1, min(1, right[1]))))
        pitch = math.degrees(math.asin(max(-1, min(1, fwd[1]))))
        mom = [0.0, 0.0, 0.0]
        for k in range(12):
            for j in range(3): mom[j] += f(144 + 52*k + 12 + 4*j)
        lat, fw, upm = dot(mom, right) / 12, dot(mom, fwd) / 12, dot(mom, up) / 12
        hs = [f(144 + 52*k + 4) for k in (0, 3, 2, 5)]
        cd = v(80)
        yawrt = ''
        if last is not None and t > last[0]:
            yawrt = '%6.1f' % (((head_f - last[1] + 180) % 360 - 180) / (t - last[0]))
        last = (t, head_f)
        if prev is not None and t - prev < every - 1e-6: continue
        prev = t
        print('%6.3f %5d %7.1f %7.2f %7.2f %6.2f %5.1f %6.1f %6s %5d %5.1f %5.1f %5.1f %2d %8.3f %8.3f %7.3f %4.0f %4.0f %4.0f %4.0f %5.2f %5.2f' % (
            t, p, f(104), head_v, head_f, slip, roll, pitch, yawrt, i32(3280), math.degrees(math.atan2(f(3772), f(3768))),
            f(3344), f(3512), i32(4036), lat, fw, upm, hs[0], hs[1], hs[2], hs[3], math.sqrt(dot(cd, cd)), dot(cd, right)))

if __name__ == '__main__':
    main()
