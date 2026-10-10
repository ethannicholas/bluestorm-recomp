#!/usr/bin/env python3
"""List the places where the game steps a value in memory: the candidates for a frame-rate fix.

    rate_sites.py [--hist <log>] [--pcs <file>] [--min R] [--kind K] [--lit-only] [--state-only]

The game is a 30 Hz simulation that steps everything per frame (docs/dev/performance.md,
"60 frames per second"), so running it at 60 means finding every step. A step is a store of
a value computed from the old value at the same address:

    x = x + t        (fadds/fsubs/fmadds, or an integer add)   -- an integrator; t is the rate
    x = x * k        (fmuls)                                   -- a damping or decay
    x = x * k + b    (fmadds with x as a factor)               -- a damped step towards b/(1-k)

This walks the recompiled C (whose comments carry each instruction's address and encoding),
decodes the instructions it cares about, follows where each register's value came from
within a function (merging at branch targets, clobbering the volatile registers at calls),
and prints every store that fits one of those shapes, with what the rate or factor is: a
literal (read out of the DOL, so a 1/30 or a 0.98 shows as such), a word in memory, or an
expression of those. Nothing of the game's code is printed, only addresses and values.

Two things a static list cannot tell are answered by a `-DGCN_WATCH` build run with
`GCN_STORE_HIST=a-b` (gcn-recomp/docs/diagnostics.md), whose stderr `--hist` reads:

  - which sites run, and how often a frame (a site that ran once a frame is a per-frame
    step; one that ran 200 times is inside a loop over objects or vertices);
  - whether the stepped value is *state*, carried from one frame to the next, or a
    temporary that something else assigns afresh each frame before the step adds to it.
    With `GCN_STORE_HIST_PCS=<file>` (the list `--pcs` writes) the run also reports every
    writer of each address those sites wrote, and a site most of whose targets have another
    writer running as often as it is classed `temp`; the rest are `state`.

Output, one site per line, tab-separated:
    store pc  function  kind  runs/frame  class  target  rate-or-factor  op pc  width

(the op pc is the arithmetic instruction, which is what steps.txt names; width is f32,
f64, ps or int)

The analysis is a candidate list for a reader, not a proof.
"""
import argparse
import glob
import os
import re
import struct
import sys

INSN = re.compile(r'^\s*/\* (80[0-9A-Fa-f]{6}) ([0-9A-Fa-f]{8})( PATCHED| STEP)? \*/')
FUNC = re.compile(r'^void (\w+)\(CPU\* c\)')

R2 = 0x8069A400   # sdata2 base (.sdata2 is const data: the compiler's float literals)
R13 = 0x80698640  # sdata base (variables)
VOLATILE_GPR = [0] + list(range(3, 13))
VOLATILE_FPR = list(range(0, 14))


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Dol:
    """Reads words out of the DOL's sections, for literal values."""

    def __init__(self, path):
        d = open(path, 'rb').read()
        off = struct.unpack('>18I', d[0:72])
        addr = struct.unpack('>18I', d[72:144])
        size = struct.unpack('>18I', d[144:216])
        self.d = d
        self.secs = [(addr[i], addr[i] + size[i], off[i]) for i in range(18) if size[i]]

    def read(self, a, n):
        for lo, hi, off in self.secs:
            if lo <= a and a + n <= hi:
                return self.d[off + a - lo: off + a - lo + n]
        return None

    @staticmethod
    def is_const(a):
        # .rodata and .sdata2 are the literal pools; .data holds the game's tables, which
        # are constant too but are not the compiler's float literals.
        return 0x801535E0 <= a < 0x80173DA0 or 0x80692400 <= a < 0x806948C0


# ---- symbolic values -------------------------------------------------------------------
# GPRs: ('base', name, offset) -- an address or an integer; name is 'abs' (offset is the
#   absolute value), 'r1', 'r2', 'r13', 'mem:<ea>' (a word loaded from memory, usually a
#   pointer), or 'u<pc>' for a value this does not follow.
#   ('memv', ea, width) -- a word loaded from memory, usable as a base or a value.
# FPRs and computed values: ('lit', address, value) | ('mem', ea, width) |
#   ('op', name, args, (pc, word)) | ('imm', n) | ('phi', (alternatives...)) | ('unk', pc)


def gpr_init():
    return {n: ('base', 'r%d' % n if n in (1, 2, 13) else 'u0_%d' % n, 0) for n in range(32)}


def ea_of(base, disp):
    kind, name, off = base
    if name == 'abs':
        return 'abs:%08X' % ((off + disp) & 0xFFFFFFFF)
    if name == 'r2':
        return 'abs:%08X' % ((R2 + off + disp) & 0xFFFFFFFF)
    if name == 'r13':
        return 'abs:%08X' % ((R13 + off + disp) & 0xFFFFFFFF)
    o = (off + disp) & 0xFFFFFFFF
    if o >= 0x80000000:
        return '%s+%08X' % (name, o)
    return '%s%+d' % (name, sext(o, 32))


def ea_addr(ea):
    return int(ea[4:], 16) if ea.startswith('abs:') else None


def fmt_lit(x):
    if isinstance(x, float):
        return '%.9g' % x
    return str(x)


def desc(v, depth=0):
    """A short description of a symbolic value for the report."""
    if depth > 4:
        return '...'
    k = v[0]
    if k == 'lit':
        return 'lit %08X=%s' % (v[1], fmt_lit(v[2]))
    if k in ('mem', 'memv'):
        return 'mem %s' % v[1]
    if k == 'base':
        return 'int ' + ea_of(v, 0)
    if k == 'imm':
        return '%d' % v[1]
    if k == 'op':
        return '%s(%s)' % (v[1], ', '.join(desc(a, depth + 1) for a in v[2]))
    if k == 'phi':
        return 'phi(%s)' % ' | '.join(desc(a, depth + 1) for a in v[1])
    return 'unk'


def lits_in(v, acc=None):
    if acc is None:
        acc = []
    if v[0] == 'lit':
        acc.append(v)
    elif v[0] == 'op':
        for a in v[2]:
            lits_in(a, acc)
    elif v[0] == 'phi':
        for a in v[1]:
            lits_in(a, acc)
    return acc


def merge(a, b):
    if a == b:
        return a
    alts = []
    for v in (a, b):
        for x in (v[1] if v[0] == 'phi' else (v,)):
            if x not in alts:
                alts.append(x)
    if len(alts) > 4:
        return ('unk', 0)
    return ('phi', tuple(alts))


class Walker:
    def __init__(self, dol):
        self.dol = dol
        self.sites = []

    def load_value(self, ea, width, kind):
        a = ea_addr(ea)
        if a is not None and self.dol.is_const(a):
            raw = self.dol.read(a, width)
            if raw is not None:
                if kind == 'f32':
                    return ('lit', a, struct.unpack('>f', raw)[0])
                if kind == 'f64':
                    return ('lit', a, struct.unpack('>d', raw)[0])
                return ('lit', a, int.from_bytes(raw, 'big', signed=True))
        return ('mem', ea, width)

    @staticmethod
    def base_of(v):
        """A GPR value as an address base."""
        if v[0] == 'base':
            return v
        if v[0] == 'memv':
            return ('base', 'mem:' + v[1], 0)
        if v[0] == 'op' and v[1] == 'iadd' and v[2][1][0] == 'imm':
            b = Walker.base_of(v[2][0])
            return (b[0], b[1], b[2] + v[2][1][1])
        return ('base', 'u?', 0)

    @staticmethod
    def arith(xo5, A, B, C, tag):
        if xo5 == 21:
            return ('op', 'add', (A, B), tag)
        if xo5 == 20:
            return ('op', 'sub', (A, B), tag)
        if xo5 == 25:
            return ('op', 'mul', (A, C), tag)
        if xo5 == 18:
            return ('op', 'div', (A, B), tag)
        if xo5 == 29:
            return ('op', 'madd', (A, C, B), tag)   # A*C + B
        if xo5 == 28:
            return ('op', 'msub', (A, C, B), tag)   # A*C - B
        if xo5 == 31:
            return ('op', 'nmadd', (A, C, B), tag)  # -(A*C + B)
        if xo5 == 30:
            return ('op', 'nmsub', (A, C, B), tag)  # -(A*C - B)
        return ('unk', 0)

    def walk(self, fname, insns):
        g = gpr_init()
        f = {n: ('unk', 0) for n in range(32)}
        pending = {}   # branch target -> list of (g, f) states arriving there
        dead = False   # after an unconditional branch, until a label brings a live state
        last_live = (dict(g), dict(f))
        # Every branch target in the function, so that a block only reached from below (a
        # loop body after a jump over it) is walked from a fresh state rather than skipped.
        targets = set()
        for pc, w in insns:
            op = w >> 26
            if op == 18 and not (w & 1):
                targets.add((pc + sext(w & 0x3FFFFFC, 26)) & 0xFFFFFFFF if not (w & 2) else (w & 0x3FFFFFC))
            elif op == 16 and not (w & 1):
                targets.add((pc + sext(w & 0xFFFC, 16)) & 0xFFFFFFFF if not (w & 2) else (w & 0xFFFC))
        for pc, w in insns:
            if pc in pending:
                states = pending.pop(pc)
                if dead:
                    g, f = dict(states[0][0]), dict(states[0][1])
                    states = states[1:]
                    dead = False
                for g2, f2 in states:
                    for r in g:
                        g[r] = merge(g[r], g2[r])
                    for r in f:
                        f[r] = merge(f[r], f2[r])
            elif dead and pc in targets:
                # Reached only from below: start afresh, keeping the non-volatile
                # registers as they were, since that is where the compiler keeps a loop's
                # invariants (a rate loaded once before the loop).
                g = gpr_init()
                f = {n: ('unk', pc) for n in range(32)}
                for r in range(14, 32):
                    g[r], f[r] = last_live[0][r], last_live[1][r]
                dead = False
            if dead:
                continue
            op = w >> 26
            rd = (w >> 21) & 31
            ra = (w >> 16) & 31
            rb = (w >> 11) & 31
            rc = (w >> 6) & 31
            simm = sext(w & 0xFFFF, 16)
            uimm = w & 0xFFFF
            xo10 = (w >> 1) & 0x3FF
            xo5 = (w >> 1) & 31
            tag = (pc, w)

            def unk(r):
                return ('base', 'u%X_%d' % (pc, r), 0)

            def bump(r, d):
                g[r] = (g[r][0], g[r][1], g[r][2] + d) if g[r][0] == 'base' else unk(r)

            def base(r):
                return self.base_of(g[r]) if r else ('base', 'abs', 0)

            def call():
                for r in VOLATILE_GPR:
                    g[r] = unk(r)
                for r in VOLATILE_FPR:
                    f[r] = ('unk', pc)

            # ---- branches ----
            if op == 18:  # b / bl
                target = (pc + sext(w & 0x3FFFFFC, 26)) & 0xFFFFFFFF if not (w & 2) else (w & 0x3FFFFFC)
                if w & 1:
                    call()
                else:
                    if target > pc:
                        pending.setdefault(target, []).append((dict(g), dict(f)))
                    last_live, dead = (dict(g), dict(f)), True
                continue
            if op == 16:  # bc
                target = (pc + sext(w & 0xFFFC, 16)) & 0xFFFFFFFF if not (w & 2) else (w & 0xFFFC)
                if w & 1:
                    call()
                elif target > pc:
                    pending.setdefault(target, []).append((dict(g), dict(f)))
                continue
            if op == 19 and xo10 in (16, 528):  # bclr / bcctr
                if w & 1:
                    call()
                elif ((w >> 21) & 0x14) == 0x14:  # unconditional: a return or a jump
                    last_live, dead = (dict(g), dict(f)), True
                continue

            # ---- integer: addresses and counters ----
            if op == 14:  # addi / li
                if ra == 0:
                    g[rd] = ('base', 'abs', simm & 0xFFFFFFFF)
                else:
                    b = g[ra]
                    g[rd] = (b[0], b[1], b[2] + simm) if b[0] == 'base' else ('op', 'iadd', (b, ('imm', simm)), tag)
            elif op == 15:  # addis / lis
                if ra == 0:
                    g[rd] = ('base', 'abs', (uimm << 16) & 0xFFFFFFFF)
                else:
                    b = g[ra]
                    g[rd] = (b[0], b[1], b[2] + (simm << 16)) if b[0] == 'base' else unk(rd)
            elif op == 24:  # ori rA,rS,uimm
                bs = g[rd]
                g[ra] = ('base', 'abs', bs[2] | uimm) if bs[0] == 'base' and bs[1] == 'abs' else (bs if uimm == 0 else unk(ra))
            elif op == 31 and xo10 == 444 and rd == rb:  # mr
                g[ra] = g[rd]
            elif op == 31 and xo10 == 266:  # add rD,rA,rB
                a, b = g[ra], g[rb]
                if a[0] == 'base' and b[0] == 'base' and b[1] == 'abs':
                    g[rd] = (a[0], a[1], a[2] + b[2])
                elif b[0] == 'base' and a[0] == 'base' and a[1] == 'abs':
                    g[rd] = (b[0], b[1], b[2] + a[2])
                else:
                    g[rd] = ('op', 'iadd', (a, b), tag)
            elif op == 31 and xo10 == 40:  # subf rD,rA,rB = rB - rA
                g[rd] = ('op', 'isub', (g[rb], g[ra]), tag)
            elif op in (32, 33, 34, 40, 42):  # lwz lwzu lbz lhz lha
                width = {32: 4, 33: 4, 34: 1, 40: 2, 42: 2}[op]
                ea = ea_of(base(ra), simm)
                g[rd] = ('memv', ea, 4) if op in (32, 33) else self.load_value(ea, width, 'int')
                if op == 33:
                    bump(ra, simm)
            elif op in (36, 37, 38, 44):  # stw stwu stb sth
                ea = ea_of(base(ra), simm)
                self.int_store(fname, pc, ea, g[rd])
                if op == 37:
                    bump(ra, simm)
            elif op in (10, 11):  # cmpli cmpi
                pass
            elif op in (7, 8, 12, 13):  # mulli subfic addic addic.
                g[rd] = unk(rd)
            elif op in (20, 21, 23, 25, 26, 27, 28, 29):  # rlwimi rlwinm rlwnm oris xori xoris andi andis
                g[ra] = unk(ra)
            elif op == 31:
                if xo10 in (0, 32, 4, 20, 150, 467, 83, 146, 598, 86, 470, 54, 982, 246, 1014, 854, 566):
                    pass  # compares, traps, reservations, SPR moves, cache and sync
                elif xo10 == 339:  # mfspr
                    g[rd] = unk(rd)
                elif xo10 in (444, 28, 60, 124, 284, 316, 412, 476, 536, 792, 824, 922, 954, 26, 24, 27, 568, 537):
                    g[ra] = unk(ra)  # the logical and shift forms write rA
                elif xo10 in (151, 215, 407, 663, 695, 727, 759, 983, 183, 247, 439):
                    pass  # indexed stores
                elif xo10 in (535, 567, 599, 631):  # lfsx lfsux lfdx lfdux
                    f[rd] = ('unk', pc)
                else:
                    g[rd] = unk(rd)

            # ---- floating point ----
            elif op in (48, 49, 50, 51):  # lfs lfsu lfd lfdu
                dbl = op >= 50
                ea = ea_of(base(ra), simm)
                f[rd] = self.load_value(ea, 8 if dbl else 4, 'f64' if dbl else 'f32')
                if op in (49, 51):
                    bump(ra, simm)
            elif op in (56, 57):  # psq_l psq_lu: ps0 from base+d
                d = sext(w & 0xFFF, 12)
                ea = ea_of(base(ra), d)
                f[rd] = self.load_value(ea, 4, 'f32') if ((w >> 12) & 7) == 0 else ('mem', ea, 4)
                if op == 57:
                    bump(ra, d)
            elif op in (52, 53, 54, 55):  # stfs stfsu stfd stfdu
                ea = ea_of(base(ra), simm)
                self.classify(fname, pc, ea, f[rd], 'f64' if op >= 54 else 'f32')
                if op in (53, 55):
                    bump(ra, simm)
            elif op in (60, 61):  # psq_st psq_stu
                d = sext(w & 0xFFF, 12)
                ea = ea_of(base(ra), d)
                if ((w >> 12) & 7) == 0:
                    self.classify(fname, pc, ea, f[rd], 'ps')
                if op == 61:
                    bump(ra, d)
            elif op in (59, 63, 4):
                A, B, C = f[ra], f[rb], f[rc]
                # The arithmetic forms use the 5-bit extended opcode, the moves and
                # compares the 10-bit one; the two sets do not collide in their low bits.
                if op == 4 and xo5 in (12, 13, 14, 15, 10, 11):  # ps_muls0/1 ps_madds0/1 ps_sum0/1
                    if xo5 in (12, 13):
                        f[rd] = ('op', 'mul', (A, C), tag)
                    elif xo5 in (14, 15):
                        f[rd] = ('op', 'madd', (A, C, B), tag)
                    else:
                        f[rd] = ('op', 'add', (A, B), tag)
                elif xo5 in (18, 20, 21, 25, 28, 29, 30, 31):
                    f[rd] = self.arith(xo5, A, B, C, tag)
                elif xo5 == 24:
                    f[rd] = ('op', 'res', (B,), tag)
                elif xo5 == 26:
                    f[rd] = ('op', 'rsqrte', (B,), tag)
                elif xo5 == 22:
                    f[rd] = ('op', 'sqrt', (B,), tag)
                elif xo5 == 23:  # fsel
                    f[rd] = ('unk', pc)
                elif xo10 in (72, 12):  # fmr frsp (ps_mr)
                    f[rd] = B
                elif xo10 in (40, 264, 136):  # fneg fabs fnabs
                    f[rd] = ('op', {40: 'neg', 264: 'abs', 136: 'nabs'}[xo10], (B,), tag)
                elif xo10 in (14, 15):  # fctiw fctiwz: an integer bit pattern
                    f[rd] = ('op', 'fctiw', (B,), tag)
                elif xo10 in (0, 32, 64, 38, 70, 134, 583, 711):
                    pass  # compares and FPSCR moves
                else:
                    f[rd] = ('unk', pc)

    def int_store(self, fname, pc, ea, v):
        if v[0] == 'op' and v[1] in ('iadd', 'isub'):
            self.classify(fname, pc, ea, v, 'int')
        elif v[0] == 'phi':
            for alt in v[1]:
                if alt[0] == 'op' and alt[1] in ('iadd', 'isub'):
                    self.classify(fname, pc, ea, alt, 'int')
                    break

    def classify(self, fname, pc, ea, v, width):
        """Report v if it is 'old value at ea' combined with something."""
        def same(x):
            if x[0] == 'phi':
                return any(same(a) for a in x[1])
            return x[0] in ('mem', 'memv') and x[1] == ea

        if v[0] == 'phi':
            for alt in v[1]:
                if alt[0] == 'op':
                    v = alt
                    break
        if v[0] != 'op':
            return
        name, args = v[1], v[2]
        # Which operand field of the instruction holds the carried value: the forms are
        # add/sub/div (A, B), mul (A, C), the multiply-adds (A, C, B), an immediate add (A),
        # a register add (A, B) and subf (B, A).
        fields = {'add': 'AB', 'sub': 'AB', 'div': 'AB', 'mul': 'AC', 'madd': 'ACB', 'msub': 'ACB',
                  'nmsub': 'ACB', 'nmadd': 'ACB', 'iadd': 'AB', 'isub': 'BA'}.get(name, '')
        if name in ('add', 'sub', 'iadd', 'isub') and len(args) == 2:
            a, b = args
            if same(a):
                self.emit(fname, pc, ea, '+=' if name in ('add', 'iadd') else '-=', b, v, width, fields[0])
            elif same(b) and name in ('add', 'iadd'):
                self.emit(fname, pc, ea, '+=', a, v, width, fields[1])
            elif same(b) and name in ('sub', 'isub'):
                self.emit(fname, pc, ea, '=-x+', a, v, width, fields[1])
        elif name in ('madd', 'msub', 'nmsub', 'nmadd'):
            a, c, b = args
            if same(b):
                kind = {'madd': '+=', 'nmsub': '-=', 'msub': '=-x+', 'nmadd': '=-x-'}[name]
                self.emit(fname, pc, ea, kind, ('op', 'mul', (a, c), v[3]), v, width, 'B')
            elif same(a) or same(c):
                k = c if same(a) else a
                self.emit(fname, pc, ea, '*=k+', ('op', 'pair', (k, b), v[3]), v, width, 'A' if same(a) else 'C')
        elif name == 'mul':
            a, b = args
            if same(a):
                self.emit(fname, pc, ea, '*=', b, v, width, 'A')
            elif same(b):
                self.emit(fname, pc, ea, '*=', a, v, width, 'C')
        elif name == 'div':
            a, b = args
            if same(a):
                self.emit(fname, pc, ea, '/=', b, v, width, 'A')

    def emit(self, fname, pc, ea, kind, rate, op, width, xpos):
        self.sites.append({'pc': pc, 'fn': fname, 'kind': kind, 'ea': ea, 'rate': rate,
                           'op': op, 'width': width, 'xpos': xpos, 'op_pc': op[3][0]})


def load_hist(path):
    """The [sthist] counts and the [stwr] writers of a GCN_STORE_HIST run."""
    hist, writers, frames = {}, {}, None
    streaks = {}
    for line in open(path, errors='replace'):
        if line.startswith('[sthist] '):
            parts = line.split()
            if parts[1] == 'frames':
                a, b = parts[2].split('-')
                frames = int(b) - int(a)
            elif parts[1] != 'end':
                hist[int(parts[1], 16)] = int(parts[2])
                if len(parts) > 3:
                    streaks[int(parts[1], 16)] = int(parts[3])
        elif line.startswith('[stwr] '):
            parts = line.split()
            addr = int(parts[1], 16)
            ws = {}
            for p in parts[2:]:
                k, n = p.split(':')
                if k != 'more':
                    ws[int(k, 16)] = int(n)
            writers[addr] = ws
    return hist, writers, frames, streaks


def classify_state(site_pcs, writers, frames):
    """For each candidate pc: the share of its writes that went to an address which some
    non-candidate store assigns about every frame (at least every other frame of the
    window) -- a temporary, such as a force accumulator zeroed after use -- and the pcs
    of those other writers. The test is against the window's length, not the site's own
    count: an accumulator takes many adds a frame and one reset."""
    per_pc = {}
    every_frame = max(1.0, (frames or 1) * 0.5)
    for addr, ws in writers.items():
        for pc, n in ws.items():
            if pc not in site_pcs:
                continue
            others = {q: m for q, m in ws.items() if q not in site_pcs and m >= every_frame}
            e = per_pc.setdefault(pc, [0, 0, {}])
            e[1] += n
            if others:
                e[0] += n
                for q, m in others.items():
                    e[2][q] = e[2].get(q, 0) + m
    out = {}
    for pc, (temp, total, others) in per_pc.items():
        share = temp / total if total else 0.0
        top = sorted(others.items(), key=lambda kv: -kv[1])[:3]
        out[pc] = ('temp' if share > 0.5 else 'state', share, top)
    return out


STEP_KINDS = ('+=', '-=', '*=', '/=', '*=k+')


def emit_steps(args, sites, hist, frames, classes):
    """Write a steps.txt: the op of every site that ran in the window, is classed state,
    lies in the game's code, runs at least --min-rate times a frame (a rarer one is an
    event, not a rate) and, for an integer step, counts by one and runs at most --int-max
    times a frame. The hand patches' addresses are skipped, and an op that two
    sites disagree on is left out."""
    patched = set()
    for path in (args.patches, args.skip):
        if os.path.exists(path):
            for line in open(path):
                line = line.split('#', 1)[0].strip()
                if line:
                    patched.add(int(line.split()[0], 16))
    chosen = {}
    skipped = {'kind': 0, 'range': 0, 'notrun': 0, 'temp': 0, 'unclassified': 0, 'intrate': 0, 'intstep': 0, 'event': 0, 'patched': 0, 'conflict': 0}
    for s in sites:
        if s['kind'] not in STEP_KINDS:
            skipped['kind'] += 1
            continue
        if not (args.lo <= s['op_pc'] < args.hi):
            skipped['range'] += 1
            continue
        c = (hist or {}).get(s['pc'], 0)
        if c == 0:
            skipped['notrun'] += 1
            continue
        if s['pc'] not in classes:
            skipped['unclassified'] += 1
            continue
        if classes[s['pc']][0] != 'state':
            skipped['temp'] += 1
            continue
        if s['width'] == 'int' and frames and c / frames > args.int_max:
            skipped['intrate'] += 1
            continue
        if frames and c / frames < args.min_rate:
            # A step that runs on an event rather than every frame (a lap counted, a
            # state advanced, a decay applied on a hit) is the event's size, not a rate,
            # and halving it loses the event. A step that is active for part of the window
            # only (the start countdown, 110 frames of 1269) still ran on consecutive
            # frames, which the histogram's streak tells: keep those.
            frames_ran = min(c, frames)
            streak = getattr(args, 'streaks', {}).get(s['pc'], 0)
            if not (frames_ran >= 8 and streak >= 0.8 * frames_ran):
                skipped['event'] += 1
                continue
        if s['width'] == 'int' and not (s['rate'][0] == 'imm' and abs(s['rate'][1]) == 1):
            # A counter steps by one. A word stepped by four, or by another word, is a
            # cursor or a sum: scaled, the first faulted the GX flush and the second
            # stalled the mode runner.
            skipped['intstep'] += 1
            continue
        if s['op_pc'] in patched:
            skipped['patched'] += 1
            continue
        prev = chosen.get(s['op_pc'])
        if prev and prev != s['xpos']:
            chosen[s['op_pc']] = None
            skipped['conflict'] += 1
            continue
        chosen[s['op_pc']] = s['xpos']
    manual = []
    if os.path.exists(args.manual):
        for line in open(args.manual):
            body = line.split('#', 1)[0].strip()
            if body:
                pc = int(body.split()[0], 16)
                chosen.pop(pc, None)
                manual.append(line.rstrip('\n'))
    with open(args.emit_steps, 'w') as out:
        out.write('# Per-frame steps and the operand that carries the state (A, B or C), with the\n'
                  '# power of the step scale where it is not 1: the recompiler scales each by the\n'
                  '# runtime step scale. Written by tools/rate_sites.py --emit-steps from a\n'
                  '# GCN_STORE_HIST run; see docs/dev/performance.md, "60 frames per second".\n')
        for pc in sorted(chosen):
            if chosen[pc]:
                out.write('%08X %s\n' % (pc, chosen[pc]))
        if manual:
            out.write('# Sites read out of the code by hand (recomp/steps_manual.txt):\n')
            for line in manual:
                out.write(line + '\n')
    print('steps: %d written, %d by hand; skipped %s' % (sum(1 for v in chosen.values() if v), len(manual), skipped), file=sys.stderr)


def collect(gen, dol):
    walker = Walker(dol)
    for path in sorted(glob.glob(os.path.join(gen, 'recomp_*.c'))):
        fname = None
        insns = []
        for line in open(path, errors='replace'):
            m = FUNC.match(line)
            if m:
                if fname and insns:
                    walker.walk(fname, insns)
                fname, insns = m.group(1), []
                continue
            m = INSN.match(line)
            if m and fname:
                insns.append((int(m.group(1), 16), int(m.group(2), 16)))
        if fname and insns:
            walker.walk(fname, insns)
    return sorted(walker.sites, key=lambda s: s['pc'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default='build/gen')
    ap.add_argument('--dol', default='build/main.dol')
    ap.add_argument('--hist', help='stderr of a GCN_STORE_HIST run')
    ap.add_argument('--frames', type=float, help="the histogram window's length in frames")
    ap.add_argument('--min', type=float, default=0.0, help='least runs per frame to list (with --hist)')
    ap.add_argument('--kind', help='only these kinds, comma separated (+=,-=,*=,*=k+,/=)')
    ap.add_argument('--lit-only', action='store_true', help='only sites whose rate involves a literal')
    ap.add_argument('--state-only', action='store_true', help='only sites classed state (with --hist writers)')
    ap.add_argument('--pcs', help='write every candidate pc to this file, for GCN_STORE_HIST_PCS')
    ap.add_argument('--emit-steps', help='write the selected sites as a steps.txt (needs --hist)')
    ap.add_argument('--lo', type=lambda v: int(v, 16), default=0x80020000, help='lowest pc to emit (hex)')
    ap.add_argument('--hi', type=lambda v: int(v, 16), default=0x80100000, help='pc bound to emit (hex)')
    ap.add_argument('--int-max', type=float, default=4.0, help='most runs/frame for an integer step to emit')
    ap.add_argument('--min-rate', type=float, default=0.9, help='fewest runs/frame for a step to emit (below is an event)')
    ap.add_argument('--patches', default='recomp/patches.txt', help='hand patches, whose addresses are left alone')
    ap.add_argument('--skip', default='recomp/steps_skip.txt', help='addresses never to emit (one per line, # comments)')
    ap.add_argument('--manual', default='recomp/steps_manual.txt', help='sites added by hand (pc operand power), copied into the output')
    args = ap.parse_args()

    sites = collect(args.gen, Dol(args.dol))
    if args.pcs:
        with open(args.pcs, 'w') as out:
            for pc in sorted({s['pc'] for s in sites}):
                out.write('%08X\n' % pc)

    hist = writers = frames = None
    classes = {}
    if args.hist:
        hist, writers, hf, streaks = load_hist(args.hist)
        frames = args.frames or hf
        args.streaks = streaks
        classes = classify_state({s['pc'] for s in sites}, writers, frames)
    kinds = set(args.kind.split(',')) if args.kind else None
    if args.emit_steps:
        emit_steps(args, sites, hist, frames, classes)
    n = 0
    for s in sites:
        if kinds and s['kind'] not in kinds:
            continue
        if args.lit_only and not lits_in(s['rate']):
            continue
        per = cls = ''
        if hist is not None:
            c = hist.get(s['pc'], 0)
            if c == 0 or (frames and c / frames < args.min):
                continue
            per = '%.3g' % (c / frames) if frames else str(c)
            if s['pc'] in classes:
                k, share, top = classes[s['pc']]
                cls = k if not top else '%s(%d%%:%s)' % (k, round(share * 100), ','.join('%08X' % q for q, _ in top))
            if args.state_only and cls.startswith('temp'):
                continue
        print('%08X\t%s\t%s\t%s\t%s\t%s\t%s\t%08X\t%s' % (s['pc'], s['fn'], s['kind'], per, cls, s['ea'], desc(s['rate']), s['op_pc'], s['width']))
        n += 1
    print('%d sites' % n, file=sys.stderr)


if __name__ == '__main__':
    main()
