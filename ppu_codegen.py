#!/usr/bin/env python3
"""PPU -> C++ static recompiler for BCUS98127 02.00 EBOOT.elf.

Uses the whole-binary analysis in ppu_funcs.py (function boundaries, jump tables,
EH landing pads) and emits one C++ function per guest entry point.

Usage: ppu_codegen.py EBOOT.elf OUT_DIR [--probe 0xADDR ...] [--chunk N]
"""
import argparse
import collections
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'tools'))
import ppu_funcs  # noqa: E402
from ppu_funcs import STUB_LO, STUB_HI, TEXT_LO, TEXT_HI, sx  # noqa: E402


def mask64(mb, me):
    """PowerPC MASK(mb, me) with bit 0 = MSB."""
    if mb <= me:
        m = ((1 << (64 - mb)) - 1) & ~((1 << (63 - me)) - 1)
    else:
        m = ((1 << (64 - mb)) - 1) | ~((1 << (63 - me)) - 1)
    return m & 0xffffffffffffffff


def h(x):
    return '0x%Xull' % (x & 0xffffffffffffffff)


def imm_s(v):
    """Signed immediate as a u64 C++ literal."""
    return h(v & 0xffffffffffffffff)


CRBIT = ('lt', 'gt', 'eq', 'so')


def crb(bi):
    return 'c.cr[%d].%s' % (bi >> 2, CRBIT[bi & 3])


class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, g, start, seen, eh_roots, tables):
        self.g = g
        self.start = start
        self.seen = sorted(seen)
        self.seen_set = seen
        self.tables = tables
        self.labels = set(eh_roots)
        self.loop_heads = set()
        self.unsupported = collections.Counter()
        self.setjmp_sites = []

    # -- naming
    def fn(self, a):
        return self.g.fname(a)

    def ra0(self, ra):
        return '0' if ra == 0 else 'c.r[%d]' % ra

    def ea_d(self, ra, d):
        if ra == 0:
            return imm_s(d)
        return 'c.r[%d] + %s' % (ra, imm_s(d))

    def ea_x(self, ra, rb):
        return ('c.r[%d]' % rb) if ra == 0 else 'c.r[%d] + c.r[%d]' % (ra, rb)

    # -- branch condition
    def cond(self, bo, bi):
        parts = []
        pre = ''
        if not bo & 4:
            pre = '--c.ctr; '
            parts.append('c.ctr %s 0' % ('==' if bo & 2 else '!='))
        if not bo & 16:
            parts.append('%s%s' % ('' if bo & 8 else '!', crb(bi)))
        return pre, ' && '.join(parts) if parts else 'true'

    def goto_or_tail(self, t):
        if t in self.seen_set and (t == self.start or t not in self.g.entryset):
            return 'goto loc_%X;' % t
        if t in self.g.entryset:
            return '{ %s(c, base); return; }' % self.fn(t)
        return 'ppu_bad_jump(c, 0x%X, 0x%X);' % (self.cur, t)

    def collect_labels(self):
        an = self.g.an
        for a in self.seen:
            w = an.word(a)
            b = an.branch(a, w)
            if b and b[0] in ('b', 'bc') and b[1] in self.seen_set and not (b[1] in self.g.entryset and b[1] != self.start):
                self.labels.add(b[1])
                if b[1] <= a:
                    self.loop_heads.add(b[1])
            if b and b[0] == 'bctr':
                for t in self.tables.get(a, {}).get('labels', []):
                    self.labels.add(t)
        for a, (labels, _) in HOOKS.items():
            if a in self.seen_set:
                self.labels.update(labels)
        # fallthrough over a gap needs a label at the next emitted address
        prev = None
        for a in self.seen:
            if prev is not None and a != prev + 4:
                self.labels.add(a)
            prev = a

    def emit(self):
        self.collect_labels()
        out = ['void %s(PPUContext& c, u8* base) {' % self.fn(self.start)]
        if self.start in self.labels:
            pass
        an = self.g.an
        n = len(self.seen)
        for i, a in enumerate(self.seen):
            self.cur = a
            if a in self.labels or a == self.start:
                out.append('loc_%X:' % a)
            if a in self.loop_heads:
                # a loop may poll memory another thread writes (spin waits on RSX get/put,
                # locks): keep the compiler from hoisting its loads out of the loop
                out.append('\tstd::atomic_signal_fence(std::memory_order_seq_cst);')
            if a in self.g.probes:
                out.append('\tppu_probe(c, base, 0x%X);' % a)
            if a in HOOKS:
                out.append('\t' + HOOKS[a][1])
            w = an.word(a)
            try:
                code, falls = self.insn(a, w)
            except Unsupported:
                self.unsupported[self.g.opkey(w)] += 1
                code, falls = 'ppu_unimplemented(c, base, 0x%X, 0x%08X);' % (a, w), True
            out.append('\t' + code)
            nxt = self.seen[i + 1] if i + 1 < n else None
            if falls and nxt != a + 4:
                if a + 4 in self.g.entryset:
                    out.append('\t{ %s(c, base); return; }' % self.fn(a + 4))
                elif a + 4 in self.seen_set:
                    out.append('\tgoto loc_%X;' % (a + 4))
                else:
                    out.append('\tppu_bad_jump(c, 0x%X, 0x%X);' % (a, a + 4))
        out.append('}')
        if self.setjmp_sites:
            # body inside try; a longjmp to this frame's buffer re-enters at the setjmp's return
            cases = ' '.join('case 0x%X: goto sj_%X;' % (x, x) for x in self.setjmp_sites)
            out[1:1] = ['\tu32 sj_buf = 0, sj_site = 0, sj_resume = 0;', 'sj_retry:', '\ttry {',
                        '\tswitch (sj_resume) { %s default: break; }' % cases]
            out[-1:] = ['\t} catch (const GuestLongjmp& j) {',
                        '\t\tif (j.buf != sj_buf) throw;',
                        '\t\tsj_resume = sj_site;',
                        '\t\tgoto sj_retry;', '\t}', '}']
        text = '\n'.join(out)
        if self.start == LONGJMP_FUNC:
            name = self.fn(self.start)
            text = text.replace('void %s(' % name, 'static void %s_body(' % name, 1)
            text += ('\n\nvoid %s(PPUContext& c, u8* base) {\n'
                     '\tconst u32 buf = (u32)((c.r[3] + 15) & ~15ull);\n'
                     '\t%s_body(c, base);  // restores the registers saved by setjmp, r3 = value\n'
                     '\tthrow GuestLongjmp{buf};\n}' % (name, name))
        # labels referenced but never placed would fail to compile; all labels are in seen
        return text

    # -- one instruction -> (C++ code, can_fall_through)
    def insn(self, a, w):
        op = w >> 26
        rt, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        rc = w & 1
        simm = sx(w & 0xffff, 16)
        uimm = w & 0xffff
        R = lambda r: 'c.r[%d]' % r  # noqa: E731
        rec = (lambda r: ' cr0(c, %s);' % r) if rc else (lambda r: '')

        if w == 0:  # deliberate illegal word (debug-break stub at 0x6c95f4)
            return 'ppu_trap(c, base, 0x%X);' % a, False
        # ---------------- branches
        if op == 18:
            li = sx(w & 0x3fffffc, 26)
            t = (li if w & 2 else a + li) & 0xffffffff
            if w & 1:
                if t in SETJMP_CALLEES:
                    self.setjmp_sites.append(a + 4)
                    return ('c.lr = 0x%X; sj_buf = (u32)((c.r[3] + 15) & ~15ull); sj_site = 0x%X; %s(c, base);\nsj_%X:'
                            % (a + 4, a + 4, self.callee(t), a + 4)), True
                return 'c.lr = 0x%X; %s(c, base);' % (a + 4, self.callee(t)), True
            return self.goto_or_tail(t), False
        if op == 16:
            bo, bi = rt, ra
            bd = sx(w & 0xfffc, 16)
            t = (bd if w & 2 else a + bd) & 0xffffffff
            pre, cnd = self.cond(bo, bi)
            if w & 1:
                return '%sif (%s) { c.lr = 0x%X; %s(c, base); }' % (pre, cnd, a + 4, self.callee(t)), True
            if cnd == 'true':
                return pre + self.goto_or_tail(t), False
            return '%sif (%s) %s' % (pre, cnd, self.goto_or_tail(t)), True
        if op == 19:
            xo = (w >> 1) & 1023
            bo, bi = rt, ra
            if xo == 16:   # bclr
                pre, cnd = self.cond(bo, bi)
                if w & 1:
                    return ('%sif (%s) { const u32 t = (u32)c.lr; c.lr = 0x%X; ppu_dispatch(c, base, t); }'
                            % (pre, cnd, a + 4)), True
                if cnd == 'true':
                    return pre + 'return;', False
                return '%sif (%s) return;' % (pre, cnd), True
            if xo == 528:  # bcctr
                pre, cnd = self.cond(bo, bi)
                if pre:
                    raise Unsupported
                if w & 1:
                    body = 'c.lr = 0x%X; ppu_dispatch(c, base, (u32)c.ctr);' % (a + 4)
                    return (body if cnd == 'true' else 'if (%s) { %s }' % (cnd, body)), True
                t = self.tables.get(a)
                if t and t.get('labels'):
                    cases = ' '.join('case 0x%X: goto loc_%X;' % (x, x) for x in sorted(set(t['labels'])))
                    sw = 'switch ((u32)c.ctr) { %s default: ppu_bad_jump(c, 0x%X, (u32)c.ctr); }' % (cases, a)
                    if cnd != 'true':
                        raise Unsupported
                    return sw, False
                body = 'ppu_dispatch(c, base, (u32)c.ctr); return;'
                if cnd == 'true':
                    return '{ %s }' % body, False
                return 'if (%s) { %s }' % (cnd, body), True
            crops = {257: '&', 449: '|', 193: '^'}
            if xo in crops:
                return '%s = %s %s %s;' % (crb(rt), crb(ra), crops[xo], crb(rb)), True
            if xo == 225:
                return '%s = !(%s & %s);' % (crb(rt), crb(ra), crb(rb)), True
            if xo == 33:
                return '%s = !(%s | %s);' % (crb(rt), crb(ra), crb(rb)), True
            if xo == 289:
                return '%s = !(%s ^ %s);' % (crb(rt), crb(ra), crb(rb)), True
            if xo == 129:
                return '%s = %s & !%s;' % (crb(rt), crb(ra), crb(rb)), True
            if xo == 417:
                return '%s = %s | !%s;' % (crb(rt), crb(ra), crb(rb)), True
            if xo == 0:
                return 'c.cr[%d] = c.cr[%d];' % (rt >> 2, ra >> 2), True
            if xo == 150:
                return 'std::atomic_signal_fence(std::memory_order_seq_cst);  /* isync */', True
            raise Unsupported
        if op == 17:
            return 'ppu_syscall(c, base, 0x%X);' % a, True

        # ---------------- integer immediate
        if op == 14:
            return '%s = %s;' % (R(rt), ('%s + %s' % (R(ra), imm_s(simm))) if ra else imm_s(simm)), True
        if op == 15:
            v = simm << 16
            return '%s = %s;' % (R(rt), ('%s + %s' % (R(ra), imm_s(v))) if ra else imm_s(v)), True
        if op in (12, 13):
            s = '%s = add_ca(%s, %s, 0, c.xer_ca);' % (R(rt), R(ra), imm_s(simm))
            return s + (' cr0(c, %s);' % R(rt) if op == 13 else ''), True
        if op == 8:
            return '{ const u64 x = %s; %s = %s - x; c.xer_ca = %s >= x; }' % (R(ra), R(rt), imm_s(simm), imm_s(simm)), True
        if op == 7:
            return '%s = (u64)((s64)%s * %d);' % (R(rt), R(ra), simm), True
        if op == 24:
            return '%s = %s | %s;' % (R(ra), R(rt), h(uimm)), True
        if op == 25:
            return '%s = %s | %s;' % (R(ra), R(rt), h(uimm << 16)), True
        if op == 26:
            return '%s = %s ^ %s;' % (R(ra), R(rt), h(uimm)), True
        if op == 27:
            return '%s = %s ^ %s;' % (R(ra), R(rt), h(uimm << 16)), True
        if op == 28:
            return '%s = %s & %s; cr0(c, %s);' % (R(ra), R(rt), h(uimm), R(ra)), True
        if op == 29:
            return '%s = %s & %s; cr0(c, %s);' % (R(ra), R(rt), h(uimm << 16), R(ra)), True
        if op == 11:  # cmpi
            crf, L = rt >> 2, rt & 1
            if L:
                return 'cr_set_s(c.cr[%d], (s64)%s, %d, c.xer_so);' % (crf, R(ra), simm), True
            return 'cr_set_s(c.cr[%d], (s32)%s, %d, c.xer_so);' % (crf, R(ra), simm), True
        if op == 10:  # cmpli
            crf, L = rt >> 2, rt & 1
            if L:
                return 'cr_set_u(c.cr[%d], %s, %s, c.xer_so);' % (crf, R(ra), h(uimm)), True
            return 'cr_set_u(c.cr[%d], (u32)%s, %s, c.xer_so);' % (crf, R(ra), h(uimm)), True
        if op in (2, 3):  # tdi / twi
            return 'if (trap_cond(%d, %s, %s, %s)) ppu_trap(c, base, 0x%X);' % (
                rt, R(ra), imm_s(simm), 'true' if op == 2 else 'false', a), True

        # ---------------- rotates
        if op in (20, 21, 23):
            mb, me = (w >> 6) & 31, (w >> 1) & 31
            m = mask64(mb + 32, me + 32)
            sh = str(rb) if op != 23 else '(unsigned)(%s & 31)' % R(rb)
            rot = 'rotl32(%s, %s)' % (R(rt), sh)
            if op == 20:
                return '%s = (%s & %s) | (%s & %s);%s' % (R(ra), rot, h(m), R(ra), h(~m), rec(R(ra))), True
            return '%s = %s & %s;%s' % (R(ra), rot, h(m), rec(R(ra))), True
        if op == 30:
            sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
            mbe = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
            sub = (w >> 2) & 7
            if sub == 0:
                return '%s = rotl64(%s, %d) & %s;%s' % (R(ra), R(rt), sh, h(mask64(mbe, 63)), rec(R(ra))), True
            if sub == 1:
                return '%s = rotl64(%s, %d) & %s;%s' % (R(ra), R(rt), sh, h(mask64(0, mbe)), rec(R(ra))), True
            if sub == 2:
                return '%s = rotl64(%s, %d) & %s;%s' % (R(ra), R(rt), sh, h(mask64(mbe, 63 - sh)), rec(R(ra))), True
            if sub == 3:
                m = mask64(mbe, 63 - sh)
                return '%s = (rotl64(%s, %d) & %s) | (%s & %s);%s' % (R(ra), R(rt), sh, h(m), R(ra), h(~m), rec(R(ra))), True
            sub4 = (w >> 1) & 15
            if sub4 == 8:
                return '%s = rotl64(%s, (unsigned)(%s & 63)) & %s;%s' % (R(ra), R(rt), R(rb), h(mask64(mbe, 63)), rec(R(ra))), True
            if sub4 == 9:
                return '%s = rotl64(%s, (unsigned)(%s & 63)) & %s;%s' % (R(ra), R(rt), R(rb), h(mask64(0, mbe)), rec(R(ra))), True
            raise Unsupported

        # ---------------- loads / stores (D, DS)
        dl = {32: ('ld32', ''), 34: ('ld8', ''), 40: ('ld16', ''), 42: ('ld16', 's16')}
        if op in dl or op in (33, 35, 41, 43):
            base_op = op & ~1
            fn, cast = dl[base_op]
            ea = self.ea_d(ra, simm)
            val = '(u64)(s64)(%s)%s(base, ea)' % (cast, fn) if cast else '%s(base, ea)' % fn
            if op & 1:
                return '{ const u64 ea = %s; %s = %s; %s = ea; }' % (ea, R(rt), val, R(ra)), True
            return '{ const u64 ea = %s; %s = %s; }' % (ea, R(rt), val), True
        ds = {36: 'st32', 38: 'st8', 44: 'st16'}
        if op in ds or op in (37, 39, 45):
            fn = ds[op & ~1]
            ea = self.ea_d(ra, simm)
            if op & 1:
                return '{ const u64 ea = %s; %s(base, ea, %s); %s = ea; }' % (ea, fn, R(rt), R(ra)), True
            return '%s(base, %s, %s);' % (fn, ea, R(rt)), True
        if op == 58:
            ds_ = sx(w & 0xfffc, 16)
            kind = w & 3
            ea = self.ea_d(ra, ds_)
            if kind == 0:
                return '%s = ld64(base, %s);' % (R(rt), ea), True
            if kind == 1:
                return '{ const u64 ea = %s; %s = ld64(base, ea); %s = ea; }' % (ea, R(rt), R(ra)), True
            if kind == 2:
                return '%s = (u64)(s64)(s32)ld32(base, %s);' % (R(rt), ea), True
            raise Unsupported
        if op == 62:
            ds_ = sx(w & 0xfffc, 16)
            kind = w & 3
            ea = self.ea_d(ra, ds_)
            if kind == 0:
                return 'st64(base, %s, %s);' % (ea, R(rt)), True
            if kind == 1:
                return '{ const u64 ea = %s; st64(base, ea, %s); %s = ea; }' % (ea, R(rt), R(ra)), True
            raise Unsupported
        if op == 46:  # lmw
            return '{ const u64 ea = %s; for (int k = %d; k < 32; ++k) c.r[k] = ld32(base, ea + 4 * (k - %d)); }' % (
                self.ea_d(ra, simm), rt, rt), True
        if op == 47:  # stmw
            return '{ const u64 ea = %s; for (int k = %d; k < 32; ++k) st32(base, ea + 4 * (k - %d), c.r[k]); }' % (
                self.ea_d(ra, simm), rt, rt), True
        if op in (48, 49, 50, 51):
            fn = 'ldf32' if op < 50 else 'ldf64'
            ea = self.ea_d(ra, simm)
            upd = ' %s = ea;' % R(ra) if op & 1 else ''
            return '{ const u64 ea = %s; c.f[%d] = %s(base, ea);%s }' % (ea, rt, fn, upd), True
        if op in (52, 53, 54, 55):
            fn = 'stf32' if op < 54 else 'stf64'
            ea = self.ea_d(ra, simm)
            upd = ' %s = ea;' % R(ra) if op & 1 else ''
            return '{ const u64 ea = %s; %s(base, ea, c.f[%d]);%s }' % (ea, fn, rt, upd), True

        if op == 31:
            return self.op31(a, w, rt, ra, rb, rc)
        if op == 59:
            return self.op59(w, rt, ra, rb, rc)
        if op == 63:
            return self.op63(w, rt, ra, rb, rc)
        if op == 4:
            return self.op4(w, rt, ra, rb)
        raise Unsupported

    def callee(self, t):
        if t in self.g.entryset:
            return self.fn(t)
        raise Unsupported

    def op31(self, a, w, rt, ra, rb, rc):
        R = lambda r: 'c.r[%d]' % r  # noqa: E731
        xo = (w >> 1) & 1023
        xo9 = xo & 511
        oe = (w >> 10) & 1
        rec = (' cr0(c, %s);' % R(rt)) if rc else ''
        reca = (' cr0(c, %s);' % R(ra)) if rc else ''
        # XO-form arithmetic (rt = dest)
        if xo9 in (266, 40, 104, 235, 233, 491, 459, 489, 457, 138, 202, 136, 200, 234, 232, 10, 8, 75, 11, 73, 9):
            if oe:
                raise Unsupported
            A, B = R(ra), R(rb)
            ex = {
                266: '%s = %s + %s;' % (R(rt), A, B),
                40: '%s = %s - %s;' % (R(rt), B, A),
                104: '%s = 0 - %s;' % (R(rt), A),
                235: '%s = (u64)((s64)(s32)%s * (s64)(s32)%s);' % (R(rt), A, B),
                233: '%s = %s * %s;' % (R(rt), A, B),
                491: '%s = divw_(%s, %s);' % (R(rt), A, B),
                459: '%s = divwu_(%s, %s);' % (R(rt), A, B),
                489: '%s = divd_(%s, %s);' % (R(rt), A, B),
                457: '%s = divdu_(%s, %s);' % (R(rt), A, B),
                75: '%s = mulhw_(%s, %s);' % (R(rt), A, B),
                11: '%s = mulhwu_(%s, %s);' % (R(rt), A, B),
                73: '%s = mulhd_(%s, %s);' % (R(rt), A, B),
                9: '%s = mulhdu_(%s, %s);' % (R(rt), A, B),
                10: '%s = add_ca(%s, %s, 0, c.xer_ca);' % (R(rt), A, B),
                8: '%s = add_ca(~%s, %s, 1, c.xer_ca);' % (R(rt), A, B),
                138: '%s = add_ca(%s, %s, c.xer_ca, c.xer_ca);' % (R(rt), A, B),
                136: '%s = add_ca(~%s, %s, c.xer_ca, c.xer_ca);' % (R(rt), A, B),
                202: '%s = add_ca(%s, 0, c.xer_ca, c.xer_ca);' % (R(rt), A),
                200: '%s = add_ca(~%s, 0, c.xer_ca, c.xer_ca);' % (R(rt), A),
                234: '%s = add_ca(%s, ~0ull, c.xer_ca, c.xer_ca);' % (R(rt), A),
                232: '%s = add_ca(~%s, ~0ull, c.xer_ca, c.xer_ca);' % (R(rt), A),
            }[xo9]
            return ex + rec, True
        if (xo >> 1) == 413:  # sradi (xo 826/827)
            sh = rb | ((w >> 1) & 1) << 5
            return '%s = srad_(c, %s, %d);%s' % (R(ra), R(rt), sh, reca), True
        logic = {28: '%s & %s', 444: '%s | %s', 316: '%s ^ %s', 60: '%s & ~%s', 412: '%s | ~%s',
                 476: '~(%s & %s)', 124: '~(%s | %s)', 284: '~(%s ^ %s)'}
        if xo in logic:
            return '%s = %s;%s' % (R(ra), logic[xo] % (R(rt), R(rb)), reca), True
        shifts = {24: 'slw_', 536: 'srw_', 27: 'sld_', 539: 'srd_'}
        if xo in shifts:
            return '%s = %s(%s, %s);%s' % (R(ra), shifts[xo], R(rt), R(rb), reca), True
        if xo == 792:
            return '%s = sraw_(c, %s, %s);%s' % (R(ra), R(rt), R(rb), reca), True
        if xo == 794:
            return '%s = srad_(c, %s, %s);%s' % (R(ra), R(rt), R(rb), reca), True
        if xo == 824:
            return '%s = sraw_(c, %s, %d);%s' % (R(ra), R(rt), rb, reca), True
        if xo == 26:
            return '%s = cntlzw_(%s);%s' % (R(ra), R(rt), reca), True
        if xo == 58:
            return '%s = cntlzd_(%s);%s' % (R(ra), R(rt), reca), True
        if xo == 954:
            return '%s = (u64)(s64)(s8)%s;%s' % (R(ra), R(rt), reca), True
        if xo == 922:
            return '%s = (u64)(s64)(s16)%s;%s' % (R(ra), R(rt), reca), True
        if xo == 986:
            return '%s = (u64)(s64)(s32)%s;%s' % (R(ra), R(rt), reca), True
        if xo in (0, 32):  # cmp / cmpl
            crf, L = rt >> 2, rt & 1
            if xo == 0:
                cast = '(s64)' if L else '(s32)'
                return 'cr_set_s(c.cr[%d], %s%s, %s%s, c.xer_so);' % (crf, cast, R(ra), cast, R(rb)), True
            cast = '' if L else '(u32)'
            return 'cr_set_u(c.cr[%d], %s%s, %s%s, c.xer_so);' % (crf, cast, R(ra), cast, R(rb)), True
        if xo in (4, 68):
            return 'if (trap_cond(%d, %s, %s, %s)) ppu_trap(c, base, 0x%X);' % (
                rt, R(ra), R(rb), 'true' if xo == 68 else 'false', a), True
        # SPR / CR moves
        if xo == 339 or xo == 467:
            spr = ra | (rb << 5)
            names = {1: None, 8: 'c.lr', 9: 'c.ctr', 256: 'c.vrsave'}
            if spr not in names:
                raise Unsupported
            if xo == 339:
                if spr == 1:
                    return '%s = xer_get(c);' % R(rt), True
                return '%s = %s;' % (R(rt), names[spr]), True
            if spr == 1:
                return 'xer_put(c, %s);' % R(rt), True
            if spr == 256:
                return 'c.vrsave = (u32)%s;' % R(rt), True
            return '%s = %s;' % (names[spr], R(rt)), True
        if xo == 371:
            return '%s = ppu_timebase();' % R(rt), True
        if xo == 19:
            return '%s = cr_get(c);' % R(rt), True
        if xo == 144:
            fxm = (w >> 12) & 0xff
            parts = ['cr_put_field(c, %d, (u32)(%s >> %d) & 15);' % (i, R(rt), 28 - 4 * i)
                     for i in range(8) if fxm & (0x80 >> i)]
            return ' '.join(parts) or '/* mtcrf 0 */', True
        # indexed loads / stores
        X = self.ea_x(ra, rb)
        lx = {23: ('ld32(base, ea)', 0), 55: ('ld32(base, ea)', 1), 87: ('ld8(base, ea)', 0), 119: ('ld8(base, ea)', 1),
              279: ('ld16(base, ea)', 0), 311: ('ld16(base, ea)', 1), 343: ('(u64)(s64)(s16)ld16(base, ea)', 0),
              375: ('(u64)(s64)(s16)ld16(base, ea)', 1), 341: ('(u64)(s64)(s32)ld32(base, ea)', 0),
              373: ('(u64)(s64)(s32)ld32(base, ea)', 1), 21: ('ld64(base, ea)', 0), 53: ('ld64(base, ea)', 1),
              534: ('__builtin_bswap32(ld32(base, ea))', 0), 790: ('__builtin_bswap16(ld16(base, ea))', 0),
              532: ('__builtin_bswap64(ld64(base, ea))', 0)}
        if xo in lx:
            val, upd = lx[xo]
            return '{ const u64 ea = %s; %s = %s;%s }' % (X, R(rt), val, (' %s = ea;' % R(ra)) if upd else ''), True
        sxs = {151: ('st32', 0), 183: ('st32', 1), 215: ('st8', 0), 247: ('st8', 1), 407: ('st16', 0), 439: ('st16', 1),
               149: ('st64', 0), 181: ('st64', 1)}
        if xo in sxs:
            fn, upd = sxs[xo]
            return '{ const u64 ea = %s; %s(base, ea, %s);%s }' % (X, fn, R(rt), (' %s = ea;' % R(ra)) if upd else ''), True
        if xo == 662:
            return 'st32(base, %s, __builtin_bswap32((u32)%s));' % (X, R(rt)), True
        if xo == 918:
            return 'st16(base, %s, __builtin_bswap16((u16)%s));' % (X, R(rt)), True
        if xo == 660:
            return 'st64(base, %s, __builtin_bswap64(%s));' % (X, R(rt)), True
        fx = {535: ('ldf32', 0), 567: ('ldf32', 1), 599: ('ldf64', 0), 631: ('ldf64', 1)}
        if xo in fx:
            fn, upd = fx[xo]
            return '{ const u64 ea = %s; c.f[%d] = %s(base, ea);%s }' % (X, rt, fn, (' %s = ea;' % R(ra)) if upd else ''), True
        fsx = {663: ('stf32', 0), 695: ('stf32', 1), 727: ('stf64', 0), 759: ('stf64', 1)}
        if xo in fsx:
            fn, upd = fsx[xo]
            return '{ const u64 ea = %s; %s(base, ea, c.f[%d]);%s }' % (X, fn, rt, (' %s = ea;' % R(ra)) if upd else ''), True
        if xo == 983:
            return 'st32(base, %s, (u32)fbits(c.f[%d]));' % (X, rt), True
        if xo == 20:
            return ('{ const u64 ea = %s; %s = ld32(base, ea); c.reserve = true; c.reserve_addr = (u32)ea; '
                    'c.reserve_value = %s; }' % (X, R(rt), R(rt))), True
        if xo == 84:
            return ('{ const u64 ea = %s; %s = ld64(base, ea); c.reserve = true; c.reserve_addr = (u32)ea; '
                    'c.reserve_value = %s; }' % (X, R(rt), R(rt))), True
        if xo == 150:
            return ('{ const bool ok = ppu_store_conditional32(c, base, (u32)(%s), (u32)%s); '
                    'c.cr[0].lt = 0; c.cr[0].gt = 0; c.cr[0].eq = ok; c.cr[0].so = c.xer_so; }' % (X, R(rt))), True
        if xo == 214:
            return ('{ const bool ok = ppu_store_conditional64(c, base, (u32)(%s), %s); '
                    'c.cr[0].lt = 0; c.cr[0].gt = 0; c.cr[0].eq = ok; c.cr[0].so = c.xer_so; }' % (X, R(rt))), True
        if xo == 597:
            n = rb or 32
            return 'lswi_(c, base, %s, %d, %d);' % (self.ra0(ra), rt, n), True
        if xo == 725:
            n = rb or 32
            return 'stswi_(c, base, %s, %d, %d);' % (self.ra0(ra), rt, n), True
        # Barriers: the host compiler must not move guest loads and stores across them (other
        # threads and SPUs poll flags the guest orders with these). sync also orders a store
        # before a later load, which x86 needs a full fence for; lwsync and eieio do not.
        if xo == 598:
            if rt & 3:
                return 'std::atomic_signal_fence(std::memory_order_seq_cst);  /* lwsync */', True
            return 'std::atomic_thread_fence(std::memory_order_seq_cst);  /* sync */', True
        if xo == 854:
            return 'std::atomic_signal_fence(std::memory_order_seq_cst);  /* eieio */', True
        if xo in (278, 246, 86, 54, 982):
            return '/* cache hint */', True
        if xo == 1014:  # dcbz: Cell cache line is 128 bytes
            return 'std::memset(base + (PPU_EA(%s) & ~127u), 0, 128);' % X, True
        # VMX loads/stores
        if xo in (103, 359):
            return 'ldv(base, %s, c.v[%d]);' % (X, rt), True
        if xo in (231, 487):
            return 'stv(base, %s, c.v[%d]);' % (X, rt), True
        if xo == 6:
            return '{ const unsigned sh = PPU_EA(%s) & 15; for (int k = 0; k < 16; ++k) c.v[%d].b(k) = (u8)(sh + k); }' % (X, rt), True
        if xo == 38:
            return '{ const unsigned sh = PPU_EA(%s) & 15; for (int k = 0; k < 16; ++k) c.v[%d].b(k) = (u8)(16 - sh + k); }' % (X, rt), True
        if xo == 71:  # lvewx
            return '{ const u32 ea = PPU_EA(%s) & ~3u; c.v[%d].w[(ea >> 2) & 3] = ld32(base, ea); }' % (X, rt), True
        if xo == 199:  # stvewx
            return '{ const u32 ea = PPU_EA(%s) & ~3u; st32(base, ea, c.v[%d].w[(ea >> 2) & 3]); }' % (X, rt), True
        raise Unsupported

    def op59(self, w, rt, ra, rb, rc):
        if rc:
            raise Unsupported
        xo = (w >> 1) & 31
        rcf = (w >> 6) & 31
        A, B, C = 'c.f[%d]' % ra, 'c.f[%d]' % rb, 'c.f[%d]' % rcf
        D = 'c.f[%d]' % rt
        ex = {21: 'rnd_s(%s + %s)' % (A, B), 20: 'rnd_s(%s - %s)' % (A, B), 25: 'rnd_s(%s * %s)' % (A, C),
              18: 'rnd_s(%s / %s)' % (A, B), 29: 'rnd_s(std::fma(%s, %s, %s))' % (A, C, B),
              28: 'rnd_s(std::fma(%s, %s, -%s))' % (A, C, B), 31: 'rnd_s(-std::fma(%s, %s, %s))' % (A, C, B),
              30: 'rnd_s(-std::fma(%s, %s, -%s))' % (A, C, B), 22: 'rnd_s(std::sqrt(%s))' % B,
              24: '(double)(1.0f / (float)%s)' % B}
        if xo in ex:
            return '%s = %s;' % (D, ex[xo]), True
        raise Unsupported

    def op63(self, w, rt, ra, rb, rc):
        xo10 = (w >> 1) & 1023
        xo5 = (w >> 1) & 31
        rcf = (w >> 6) & 31
        A, B, C = 'c.f[%d]' % ra, 'c.f[%d]' % rb, 'c.f[%d]' % rcf
        D = 'c.f[%d]' % rt
        cr1 = ' c.cr[1] = CRField{0, 0, 0, 0};' if rc else ''
        if xo5 >= 16 or xo5 in (18, 20, 21):
            ex = {21: '%s + %s' % (A, B), 20: '%s - %s' % (A, B), 25: '%s * %s' % (A, C), 18: '%s / %s' % (A, B),
                  29: 'std::fma(%s, %s, %s)' % (A, C, B), 28: 'std::fma(%s, %s, -%s)' % (A, C, B),
                  31: '-std::fma(%s, %s, %s)' % (A, C, B), 30: '-std::fma(%s, %s, -%s)' % (A, C, B),
                  22: 'std::sqrt(%s)' % B, 23: 'fsel_(%s, %s, %s)' % (A, B, C),
                  26: '1.0 / std::sqrt(%s)' % B, 24: '1.0 / %s' % B}
            if xo5 in ex:
                return '%s = %s;%s' % (D, ex[xo5], cr1), True
        ex10 = {72: B, 40: '-%s' % B, 264: 'std::fabs(%s)' % B, 136: '-std::fabs(%s)' % B,
                12: 'rnd_s(%s)' % B, 14: 'fctiw_(%s, false)' % B, 15: 'fctiw_(%s, true)' % B,
                814: 'fctid_(%s, false)' % B, 815: 'fctid_(%s, true)' % B,
                846: '(double)(s64)fbits(%s)' % B}
        if xo10 in ex10:
            if xo10 in (72, 40, 264, 136):  # sign ops are bit-exact (NaN payloads kept)
                bit = {72: 'fbits(%s)' % B, 40: 'fbits(%s) ^ 0x8000000000000000ull' % B,
                       264: 'fbits(%s) & 0x7fffffffffffffffull' % B, 136: 'fbits(%s) | 0x8000000000000000ull' % B}[xo10]
                return '%s = fdbl(%s);%s' % (D, bit, cr1), True
            return '%s = %s;%s' % (D, ex10[xo10], cr1), True
        if xo10 in (0, 32):
            return 'fcmp_(c, %d, %s, %s);' % (rt >> 2, A, B), True
        if xo10 == 583:
            return '%s = fdbl((u64)c.fpscr);%s' % (D, cr1), True
        if xo10 == 711:
            flm = (w >> 17) & 0xff
            m = 0
            for i in range(8):
                if flm & (0x80 >> i):
                    m |= 0xf << (28 - 4 * i)
            return 'c.fpscr = (c.fpscr & ~0x%Xu) | ((u32)fbits(%s) & 0x%Xu);%s' % (m, B, m, cr1), True
        if xo10 == 70:
            return 'c.fpscr &= ~(1u << %d);' % (31 - rt), True
        if xo10 == 38:
            return 'c.fpscr |= (1u << %d);' % (31 - rt), True
        if xo10 == 134:
            crf = rt >> 2
            imm = (w >> 12) & 15
            return 'c.fpscr = (c.fpscr & ~(0xFu << %d)) | (%du << %d);' % (28 - 4 * crf, imm, 28 - 4 * crf), True
        raise Unsupported

    def op4(self, w, vd, va, vb):
        V = lambda r: 'c.v[%d]' % r  # noqa: E731
        x6 = w & 0x3f
        vc = (w >> 6) & 31
        if 32 <= x6 < 48:
            if x6 == 46:
                return 'for (int k = 0; k < 4; ++k) c.v[%d].f[k] = std::fma(%s.f[k], %s.f[k], %s.f[k]);' % (vd, V(va), V(vc), V(vb)), True
            if x6 == 47:
                return 'for (int k = 0; k < 4; ++k) c.v[%d].f[k] = -std::fma(%s.f[k], %s.f[k], -%s.f[k]);' % (vd, V(va), V(vc), V(vb)), True
            if x6 == 43:
                return '%s = vperm_(%s, %s, %s);' % (V(vd), V(va), V(vb), V(vc)), True
            if x6 == 42:
                return 'for (int k = 0; k < 4; ++k) c.v[%d].w[k] = (%s.w[k] & ~%s.w[k]) | (%s.w[k] & %s.w[k]);' % (
                    vd, V(va), V(vc), V(vb), V(vc)), True
            if x6 == 44:
                return '%s = vsldoi_(%s, %s, %d);' % (V(vd), V(va), V(vb), (w >> 6) & 15), True
            raise Unsupported
        xo = w & 0x7ff
        rc = (w >> 10) & 1
        xo_c = w & 0x3ff
        cmps = {198: '%s.f[k] == %s.f[k]', 454: '%s.f[k] >= %s.f[k]', 710: '%s.f[k] > %s.f[k]',
                134: '%s.w[k] == %s.w[k]', 646: '%s.w[k] > %s.w[k]', 902: '%s.sw[k] > %s.sw[k]'}
        if xo_c in cmps:
            e = cmps[xo_c] % (V(va), V(vb))
            s = '{ v128 t; for (int k = 0; k < 4; ++k) t.w[k] = (%s) ? ~0u : 0u; %s = t;%s }' % (
                e, V(vd), (' vcr6(c, t);' if rc else ''))
            return s, True
        A, B = V(va), V(vb)
        simple = {
            10: 'f[k] = %s.f[k] + %s.f[k]' % (A, B), 74: 'f[k] = %s.f[k] - %s.f[k]' % (A, B),
            1034: 'f[k] = %s.f[k] > %s.f[k] ? %s.f[k] : %s.f[k]' % (A, B, A, B),
            1098: 'f[k] = %s.f[k] < %s.f[k] ? %s.f[k] : %s.f[k]' % (A, B, A, B),
            1220: 'w[k] = %s.w[k] ^ %s.w[k]' % (A, B), 1156: 'w[k] = %s.w[k] | %s.w[k]' % (A, B),
            1028: 'w[k] = %s.w[k] & %s.w[k]' % (A, B), 1092: 'w[k] = %s.w[k] & ~%s.w[k]' % (A, B),
            1284: 'w[k] = ~(%s.w[k] | %s.w[k])' % (A, B),
            128: 'w[k] = %s.w[k] + %s.w[k]' % (A, B), 1152: 'w[k] = %s.w[k] - %s.w[k]' % (A, B),
            388: 'w[k] = %s.w[k] << (%s.w[k] & 31)' % (A, B), 644: 'w[k] = %s.w[k] >> (%s.w[k] & 31)' % (A, B),
            900: 'sw[k] = %s.sw[k] >> (%s.w[k] & 31)' % (A, B),
            640: 'w[k] = (u32)std::min<u64>((u64)%s.w[k] + %s.w[k], 0xffffffffull)' % (A, B),
            1664: 'w[k] = %s.w[k] > %s.w[k] ? %s.w[k] - %s.w[k] : 0' % (A, B, A, B),
            266: 'f[k] = 1.0f / %s.f[k]' % B, 330: 'f[k] = 1.0f / std::sqrt(%s.f[k])' % B,
            778: 'f[k] = (float)%s.w[k] / (float)(1u << %d)' % (B, va),
            842: 'f[k] = (float)%s.sw[k] / (float)(1u << %d)' % (B, va),
            970: 'w[k] = sat_s32((double)%s.f[k] * (double)(1u << %d))' % (B, va),
            906: 'w[k] = sat_u32((double)%s.f[k] * (double)(1u << %d))' % (B, va),
        }
        if xo in simple:
            # compute into a temporary so vd may alias inputs
            return '{ v128 t_; for (int k = 0; k < 4; ++k) t_.%s; c.v[%d] = t_; }' % (simple[xo], vd), True
        if xo == 652:
            return '%s = vsplat_w(%s.w[%d]);' % (V(vd), B, va & 3), True
        if xo == 588:
            return '{ const u16 x = %s.h(%d); for (int k = 0; k < 8; ++k) c.v[%d].h(k) = x; }' % (B, va & 7, vd), True
        if xo == 524:
            return '{ const u8 x = %s.b(%d); for (int k = 0; k < 16; ++k) c.v[%d].b(k) = x; }' % (B, va & 15, vd), True
        if xo == 908:
            return '%s = vsplat_w((u32)(s32)%d);' % (V(vd), sx(va, 5)), True
        if xo == 844:
            return 'for (int k = 0; k < 8; ++k) c.v[%d].h(k) = (u16)(s16)%d;' % (vd, sx(va, 5)), True
        if xo == 780:
            return 'for (int k = 0; k < 16; ++k) c.v[%d].b(k) = (u8)(s8)%d;' % (vd, sx(va, 5)), True
        merges = {140: ('w', 4, 0), 396: ('w', 4, 2), 12: ('b', 16, 0), 268: ('b', 16, 8), 76: ('h', 8, 0), 332: ('h', 8, 4)}
        if xo in merges:
            kind, n, off = merges[xo]
            acc = {'w': '.w[%s]', 'b': '.b(%s)', 'h': '.h(%s)'}[kind]
            return ('{ v128 t_; for (int k = 0; k < %d; ++k) { t_%s = %s%s; t_%s = %s%s; } c.v[%d] = t_; }' % (
                n // 2, acc % '2 * k', A, acc % ('k + %d' % off), acc % '2 * k + 1', B, acc % ('k + %d' % off), vd)), True
        if xo == 590:  # vupkhsh
            return '{ v128 t_; for (int k = 0; k < 4; ++k) t_.sw[k] = (s16)%s.h(k); c.v[%d] = t_; }' % (B, vd), True
        if xo == 718:  # vupklsh
            return '{ v128 t_; for (int k = 0; k < 4; ++k) t_.sw[k] = (s16)%s.h(k + 4); c.v[%d] = t_; }' % (B, vd), True
        if xo == 206:  # vpkuwus
            return ('{ v128 t_; for (int k = 0; k < 4; ++k) { t_.h(k) = (u16)std::min<u32>(%s.w[k], 0xffff); '
                    't_.h(k + 4) = (u16)std::min<u32>(%s.w[k], 0xffff); } c.v[%d] = t_; }' % (A, B, vd)), True
        if xo == 72:  # vmulouh
            return '{ v128 t_; for (int k = 0; k < 4; ++k) t_.w[k] = (u32)%s.h(2 * k + 1) * %s.h(2 * k + 1); c.v[%d] = t_; }' % (A, B, vd), True
        if xo == 584:  # vmuleuh
            return '{ v128 t_; for (int k = 0; k < 4; ++k) t_.w[k] = (u32)%s.h(2 * k) * %s.h(2 * k); c.v[%d] = t_; }' % (A, B, vd), True
        if xo == 1540:  # mfvscr
            return '%s = v128{}; c.v[%d].w[3] = c.vscr;' % (V(vd), vd), True
        if xo == 1604:  # mtvscr
            return 'c.vscr = %s.w[3];' % B, True
        raise Unsupported


# Instruction overrides (address -> new word), applied after analysis. Only for patches that
# keep the control-flow graph the analysis found (e.g. making a conditional branch always taken).
PATCHES = {
    # Cinematic tick (sub_7E278): the START-to-skip check only runs while the global "boot /
    # unskippable" byte at 0x10063BFC is 0, so the new-game intro can't be skipped.
    # beq cr1,0x7e6c8 -> b 0x7e6c8: always check for START.
    0x7E5D8: (0x418600F0, 0x480000F0),
}


# C++ inserted before the instruction at an address (address -> (extra labels, code)), for
# guards that need host logic. The labels listed are emitted even when no branch targets them.
HOOKS = {
    # Cheat menu (host/cheats.cpp): a level script object is about to be updated on the game's
    # main thread, outside any Lua call, so queued Lua chunks can run here.
    0x293C70: ((), 'ppu_host_hook(c, base, 0x293C70);'),
    # Lua get_pos(moby_handle) (sub_280DA8): r9 is the moby; the cheat menu notes the hero's moby here.
    0x280DD8: ((), 'ppu_host_hook(c, base, 0x280DD8);'),
    # Free camera: r25 = the game camera's world matrix, just before the view is built from it.
    0x48C910: ((), 'ppu_host_hook(c, base, 0x48C910);'),
    # No clip: r3 = the hero, about to copy its position into its moby.
    0x23E608: ((), 'ppu_host_hook(c, base, 0x23E608);'),
    # Co-op (host/coop.cpp): r3 = a player slot; slot 1 is redirected to player 2's slot.
    0x1E1298: ((), 'ppu_host_hook(c, base, 0x1E1298);'),
    0x1E1120: ((), 'ppu_host_hook(c, base, 0x1E1120);'),
    # Co-op: sub_639948(view, f1 = horizontal field of view in degrees) rebuilds the view's projection.
    0x639948: ((), 'ppu_host_hook(c, base, 0x639948);'),
    # Co-op: sub_658E48(hero) returns the hero's view; r29 = the hero, r3 = its player slot (view handle at +0x10).
    0x658E90: ((), 'ppu_host_hook(c, base, 0x658E90);'),
    # Skin packs (host/skins.cpp): the packs are listed in the skins menu after the game's skins. At the
    # entry of each skin function behind the menu (r3 = skin id) the host answers for a pack's id and the
    # function is skipped; 0x2A58C registers the Lua constant SKIN_TYPE_COUNT (f1); sub_1E21A0 returns the
    # selected skin, which player 2 can have their own of.
    0x24B08: ((), 'if (ppu_host_hook(c, base, 0x24B08)) return;'),   # is_skin_owned
    0x24B38: ((), 'if (ppu_host_hook(c, base, 0x24B38)) return;'),   # is_skin_available
    0x25B90: ((), 'if (ppu_host_hook(c, base, 0x25B90)) return;'),   # get_skin_cost
    0x27AA0: ((), 'if (ppu_host_hook(c, base, 0x27AA0)) return;'),   # purchase_skin
    0x26FC0: ((), 'if (ppu_host_hook(c, base, 0x26FC0)) return;'),   # select_skin
    0x25B68: ((), 'if (ppu_host_hook(c, base, 0x25B68)) return;'),   # get_skin_name
    0x25B40: ((), 'if (ppu_host_hook(c, base, 0x25B40)) return;'),   # get_skin_desc
    0x25B18: ((), 'if (ppu_host_hook(c, base, 0x25B18)) return;'),   # get_skin_movie
    0x1E21A0: ((), 'if (ppu_host_hook(c, base, 0x1E21A0)) return;'),   # selected skin
    0x2A58C: ((), 'ppu_host_hook(c, base, 0x2A58C);'),
    # Weapon XP in the pause menu (host/weapon_xp.cpp): the get_weapon_desc (sub_2F880) and get_weapon_level
    # (sub_33A98) bindings are about to look up weapon r3, and the Anark Text.textString setter (sub_74F050)
    # is about to hand the new string (r3) to text object r28; the weapons page's description gets an XP line.
    0x2F938: ((), 'ppu_host_hook(c, base, 0x2F938);'),
    0x33B50: ((), 'ppu_host_hook(c, base, 0x33B50);'),
    0x74F0F4: ((), 'ppu_host_hook(c, base, 0x74F0F4);'),
    # Co-op: sub_4E8D18 uploads a foliage material's wind table; when its per-frame buffer was reused the
    # host points the upload's source register at a good copy (coop::on_sway).
    0x4E8D18: ((), 'ppu_host_hook(c, base, 0x4E8D18);'),
    # Co-op: sub_65E898 draws the HUD's widget tree (only caller: sub_661680, back at 0x661A14); while the
    # screen is split it is drawn once per half (coop::on_hud_draw), each element of it placed by
    # coop::on_widget_draw at the entry of sub_65CD00 (a widget and its children).
    0x65E898: ((), 'ppu_host_hook(c, base, 0x65E898);'),
    0x65CD00: ((), 'ppu_host_hook(c, base, 0x65CD00);'),
    0x661A14: ((), 'ppu_host_hook(c, base, 0x661A14);'),
    0x4E8E94: ((), 'ppu_host_hook(c, base, 0x4E8E94);'),
    # sub_524018 leaves a hole of r27 bytes in its command chunk and appends a 16-byte entry to
    # the list at TOC[0x76F0] (count at +0x5BC, array at +0x5C0) for a later step to fill it.
    # Entries added before that list is set up for the pass (sub_520DA8) are dropped, leaving
    # stale bytes the RSX runs as commands. Fill every hole with RSX NOPs first; when the list
    # has no array at all (the main scene's depth-sorted bucket in a side pass, see 0x4BE684)
    # also skip the entry instead of writing at address 0.
    0x524B14: ((0x524B50,), '{ const u32 hole = (u32)(c.r[27] + 0xF) & 0xFFFFFFF0u; for (u32 o = 0; o < hole; o += 4) st32(base, c.r[8] + o, 0); '
                            'const u32 lst = ld32(base, c.r[2] + 0x76F0); if (!ld32(base, lst + 0x5C0)) { ppu_note(0x524B14, "draw list entry with no list skipped"); '
                            'c.r[19] = c.r[27] + 0xF; c.r[21] = hole; c.r[6] = c.r[8] + c.r[21]; goto loc_524B50; } }'),
    # TOD_SORTCHECK: the command context sub_524018 writes to has no buffer
    0x524018: ((), 'if (ppu_sortcheck_on()) { const u32 pp = ld32(base, c.r[2] + 0x7740), cx = ld32(base, pp); if (!ld32(base, cx + 8)) ppu_watch_ctx(base, pp, cx, (u32)c.lr); }'),
    # TOD_SORTCHECK: compare the draw-record pool before and after the SPU sort
    0x4BEC08: ((), 'if (ppu_sortcheck_on()) ppu_sort_before(base, (u32)c.r[3]);'),
    0x4BE238: ((), 'if (ppu_sortcheck_on()) ppu_sort_after(base, (u32)c.r[3]); ppu_host_hook(c, base, 0x4BE238);'),
    # TOD_WATCH_EA tracing of the draw-record overwrite
    0x536960: ((), 'ppu_watch(0x536960, 0, 0, (u32)c.lr, 0);'),
    0x5EA158: ((), 'ppu_watch(0x5EA158, 0, 0, (u32)c.r[3], (u32)c.r[4]);'),
    0x4BDF88: ((), 'if (ppu_watch_ea()) ppu_watch(0x4BDF88, ld32(base, c.r[3] + 4), 16, (u32)c.r[7], (u32)c.lr); if ((u32)c.r[7] == 6 && (u32)c.r[6] && !ppu_valid_opd(base, (u32)c.r[6])) ppu_note_rec((u32)c.lr, (u32)c.r[8], (u32)c.r[5], (u32)c.r[6]);'),
    # sub_4A34E0 posts a "special" raw-SPU command in a fixed slot. Raw SPU 1 marks it done before
    # its helper (raw SPU 0) has finished, and the helper re-reads the slot header part way
    # through. Hold the PPU until the helper has released the job lock before the slot is
    # rewritten (see ppu_wait_slot_helper).
    0x4A3788: ((), 'ppu_wait_slot_helper(base);'),
    # TOD_SLOTLOG tracing of the raw-SPU command slot handshake (sub_4A34E0)
    0x4A37F4: ((), 'ppu_trace(0x4A37F4, (u32)c.r[12], (u32)c.r[4]);'),
    0x4A39B4: ((), 'ppu_trace(0x4A39B4, (u32)c.r[8], (u32)c.r[9]);'),
    # Draw-bucket dispatch (sub_4BE238) groups each bucket's 16-byte records by the callback
    # descriptor at +12 and calls it. Right after a level loads, a bucket can hold records that
    # were never filled (zero, or stale bytes from an earlier frame), and the call through such a
    # descriptor faults. Skip groups whose descriptor isn't a function descriptor: one check for
    # a run in the middle of a bucket, one for the last.
    0x4BE684: ((), 'if (!ppu_valid_opd(base, (u32)c.r[11])) { ppu_note_mem(0x4BE684, "draw record with no callback skipped", base, (u32)c.r[3]); ppu_note_vals(0x4BE684, (u32)((c.r[26] - c.r[21] - 0x110) >> 4), (u32)c.r[31], (u32)c.r[20]); '
                   'c.r[4] = c.r[29]; goto loc_4BE630; }'),
    # The second check: in a side pass (sub_536960 at 0x536CA0) a bucket can be dispatched to
    # sub_4D3D00, which takes an object pointer in r4, with the record count there instead (seen
    # as a fault reading 0xAD after a warp to Ardolis). Skip that call.
    0x4BE5B0: ((0x4BE5C8,), 'if (!ppu_valid_opd(base, (u32)c.r[9])) { ppu_note_mem(0x4BE5B0, "draw bucket with no callback skipped", base, (u32)c.r[6]); '
                            'goto loc_4BE5C8; } '
                            'if (ld32(base, (u32)c.r[9]) == 0x4D3D00u && (u32)c.r[4] < 0x10000u) { ppu_note(0x4BE5B0, "draw bucket call with a count for an object skipped"); goto loc_4BE5C8; }'),
    # Story movies (sub_5D2EB8 opens them with BinkOpen at 0x7F2C60) can't be skipped on PS3
    # the first time. The host notes each new movie handle, and the per-frame "still playing"
    # test in sub_5D2818 (FrameNum < Frames) reports the end once START or Cross is pressed.
    0x5D2FD4: ((), 'ppu_movie_opened((u32)c.r[3]);'),
    0x5D30A4: ((), 'ppu_movie_opened((u32)c.r[3]);'),
    0x5D28AC: ((), 'if (ppu_movie_skip((u32)c.r[9])) c.r[5] = c.r[31];'),
}


# setjmp/longjmp. The guest longjmp restores the saved registers and "returns" from setjmp a
# second time, which a recompiled function cannot do. Instead the longjmp's translation throws
# GuestLongjmp after restoring the registers, and each function that calls setjmp catches it and
# resumes right after its own setjmp call. (Lua's luaD_rawrunprotected is the only caller.)
SETJMP_CALLEES = {0x826A10, 0x59E630}   # libc setjmp and its TOC stub
LONGJMP_FUNC = 0x826B34                 # libc longjmp (its stub tail-calls it)


class Gen:
    def __init__(self, elf, probes):
        self.res = ppu_funcs.analyze(elf)
        self.an = self.res['an']
        self.an.apply_patches(PATCHES)
        self.img = self.res['img']
        self.entries = self.res['entries']
        self.entryset = set(self.entries)
        self.probes = set(probes)
        self.tables = self.res['tables']

    def fname(self, a):
        return 'sub_%X' % a

    @staticmethod
    def opkey(w):
        op = w >> 26
        if op in (19, 31):
            return '%d/%d' % (op, (w >> 1) & 1023)
        if op in (59, 63):
            return '%d/%d' % (op, (w >> 1) & (31 if (w >> 1) & 31 >= 16 or op == 59 else 1023))
        if op == 4:
            return '4/%d' % (w & 0x3f if 32 <= (w & 0x3f) < 48 else w & 0x7ff)
        return str(op)

    def imports(self):
        """Import stub address -> (index, library, nid), from the module import table."""
        img = self.img
        out = {}
        idx = 0
        for addr in range(0x83c46c, 0x83c5f8, 44):
            count = img.u32(addr + 4) & 0xffff
            lib_ptr = img.u32(addr + 16)
            o = img.off(lib_ptr)
            lib = img.d[o:img.d.index(b'\0', o)].decode()
            nids, slots = img.u32(addr + 20), img.u32(addr + 24)
            for i in range(count):
                out[slots + 4 * i] = (lib, img.u32(nids + 4 * i))
        # stub -> slot: stubs load the slot address with lis/lwz; match by scanning the stub
        stubs = {}
        for s in range(STUB_LO, STUB_HI, 32):
            val, slot = 0, None
            for k in range(8):
                w = self.an.word(s + 4 * k)
                op, imm = w >> 26, w & 0xffff
                if op == 14:
                    val = sx(imm, 16) & 0xffffffff
                elif op == 15:
                    val = (sx(imm, 16) << 16) & 0xffffffff
                elif op == 25:
                    val |= imm << 16
                elif op == 32 and slot is None:
                    slot = (val + sx(imm, 16)) & 0xffffffff
            lib, nid = out[slot]
            stubs[s] = (idx, lib, nid, slot)
            idx += 1
        return stubs

    def generate(self, out, chunk):
        out.mkdir(parents=True, exist_ok=True)
        stubs = self.imports()
        funcs = self.res['funcs']
        eh = self.res['eh_roots']
        decls = []
        bodies = []
        unsupported = collections.Counter()
        stats = collections.Counter()
        for s, limit, seen, tails, bctrs, falls in funcs:
            decls.append('void %s(PPUContext& c, u8* base);' % self.fname(s))
            if s in stubs:
                idx, lib, nid, slot = stubs[s]
                bodies.append((s, ('void %s(PPUContext& c, u8* base) {\n'
                    '\t// %s 0x%%08X: same register effects as the real stub, then HLE\n'
                    '\tc.r[12] = ld32(base, 0x%X); st64(base, c.r[1] + 0x28, c.r[2]);\n'
                    '\tif (c.r[12]) { c.r[0] = ld32(base, c.r[12]); c.r[2] = ld32(base, c.r[12] + 4); c.ctr = c.r[0]; }\n'
                    '\tppu_import(c, base, %d);\n}' % (self.fname(s), lib, slot, idx)) % nid))
                stats['import_stubs'] += 1
                continue
            fg = FuncGen(self, s, seen, eh.get(s, ()), self.tables)
            bodies.append((s, fg.emit()))
            unsupported.update(fg.unsupported)
            stats['functions'] += 1
            stats['instructions'] += len(seen)
        def write_if_changed(p, text):
            # unchanged files keep their timestamps, so a rebuild only compiles what changed
            if not p.exists() or p.read_text() != text:
                p.write_text(text)
        write_if_changed(out / 'ppu_decls.h', '#pragma once\n#include "ppu.h"\n' + '\n'.join(decls) + '\n')
        files = []
        cur, size, n = [], 0, 0
        for s, body in bodies:
            cur.append(body)
            size += len(body)
            if size > chunk:
                files.append(cur)
                cur, size = [], 0
        if cur:
            files.append(cur)
        names = []
        for i, f in enumerate(files):
            name = 'ppu_code_%03d.cpp' % i
            names.append(name)
            text = '#include "ppu_decls.h"\n#include <algorithm>\n\n' + '\n\n'.join(f) + '\n'
            write_if_changed(out / name, text)
        # function table + import table
        tab = ['#include "ppu_decls.h"', '#include "ppu_tables.h"', '',
               'const PPUFunctionEntry ppu_function_table[] = {']
        tab += ['    {0x%X, %s},' % (s, self.fname(s)) for s, *_ in funcs]
        tab += ['};', 'const unsigned ppu_function_count = %d;' % len(funcs), '',
                'const PPUImportEntry ppu_import_table[] = {']
        for s in sorted(stubs):
            idx, lib, nid, slot = stubs[s]
            tab.append('    {%d, "%s", 0x%08X, 0x%X, 0x%X},' % (idx, lib, nid, s, slot))
        tab += ['};', 'const unsigned ppu_import_count = %d;' % len(stubs), '']
        write_if_changed(out / 'ppu_tables.cpp', '\n'.join(tab))
        report = dict(stats, files=len(names), unsupported=dict(unsupported.most_common()),
                      unsupported_total=sum(unsupported.values()))
        (out / 'codegen_report.json').write_text(json.dumps(report, indent=2) + '\n')
        write_if_changed(out / 'sources.txt', '\n'.join(names + ['ppu_tables.cpp']) + '\n')
        return report


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('elf')
    p.add_argument('out', type=Path)
    p.add_argument('--probe', action='append', default=[], type=lambda s: int(s, 0))
    p.add_argument('--chunk', type=int, default=4 << 20, help='approximate bytes of C++ per source file')
    a = p.parse_args()
    rep = Gen(a.elf, a.probe).generate(a.out, a.chunk)
    print(json.dumps(rep, indent=2))


if __name__ == '__main__':
    main()
