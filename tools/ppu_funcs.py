#!/usr/bin/env python3
"""Whole-binary PPU function analysis for BCUS98127 EBOOT.elf (decrypted).

Finds function entry points (OPD descriptors, bl targets, import stubs),
computes boundaries by recursive descent, resolves GCC switch jump tables,
classifies indirect branches, and reports anything left unexplained.

Usage: ppu_funcs.py EBOOT.elf OUT_DIR
"""
import bisect
import collections
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ehframe import landing_pads  # noqa: E402

EXPECTED = '0ee9a8414c8fc182bc19fa2a523e6d050d0be76138b3733ae515798d77d2468c'

# Section layout of the pinned ELF (see readelf -S).
TEXT_LO, TEXT_HI = 0x10200, 0x83a50c      # sections 1..4 (crt, .text, glue, import stubs)
STUB_LO, STUB_HI = 0x83964c, 0x83a50c     # import stubs, 32 bytes each
# memset tail: mtctr r8 at 0x5ac924 computes an entry into 7x 'stdu r4,8(r6)' + blr
# (Duff's device). Any of the eight words may be the target.
COMPUTED_JUMPS = {0x5ac964: [0x5ac968 + 4 * k for k in range(8)]}
OPD_LO, OPD_HI = 0x859408, 0x859408 + 0x2eb30


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Image:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        if hashlib.sha256(self.d).hexdigest() != EXPECTED:
            raise SystemExit('EBOOT.elf SHA-256 differs from the pinned BCUS98127 02.00 build')
        phoff, = struct.unpack_from('>Q', self.d, 0x20)
        phn, = struct.unpack_from('>H', self.d, 0x38)
        self.segs = []
        for i in range(phn):
            t, fl, off, va, pa, fs, ms, al = struct.unpack_from('>IIQQQQQQ', self.d, phoff + 56 * i)
            if t == 1 and fs:
                self.segs.append((va, off, fs))

    def off(self, a):
        for va, off, fs in self.segs:
            if va <= a < va + fs:
                return off + a - va
        return None

    def u32(self, a):
        o = self.off(a)
        return None if o is None else struct.unpack_from('>I', self.d, o)[0]


def in_text(a):
    return TEXT_LO <= a < TEXT_HI and a % 4 == 0


class Analyzer:
    def __init__(self, img):
        self.img = img
        o = img.off(TEXT_LO)
        n = (TEXT_HI - TEXT_LO) // 4
        self.w = struct.unpack_from('>%dI' % n, img.d, o)

    def word(self, a):
        return self.w[(a - TEXT_LO) >> 2]

    def apply_patches(self, patches):
        """patches: address -> (expected original word, new word)."""
        w = list(self.w)
        for a, (old, new) in patches.items():
            if w[(a - TEXT_LO) >> 2] != old:
                raise SystemExit('patch at 0x%X: expected 0x%08X, found 0x%08X' % (a, old, w[(a - TEXT_LO) >> 2]))
            w[(a - TEXT_LO) >> 2] = new
        self.w = tuple(w)

    # ---- instruction helpers -------------------------------------------------
    @staticmethod
    def branch(a, w):
        """Return (kind, target) for a branch word, else None.
        kind: 'b' uncond, 'bl' call, 'bc' conditional, 'bcl' cond call,
              'blr', 'bclr' (cond return), 'bctr', 'bctrl', 'bcctr' (cond)."""
        op = w >> 26
        if op == 18:
            li = sx(w & 0x3fffffc, 26)
            tgt = (li if w & 2 else a + li) & 0xffffffff
            return ('bl' if w & 1 else 'b', tgt)
        if op == 16:
            bd = sx(w & 0xfffc, 16)
            tgt = (bd if w & 2 else a + bd) & 0xffffffff
            bo = (w >> 21) & 31
            always = (bo & 0x14) == 0x14
            if w & 1:
                return ('bl' if always else 'bcl', tgt)
            return ('b' if always else 'bc', tgt)
        if op == 19:
            xo = (w >> 1) & 1023
            bo = (w >> 21) & 31
            always = (bo & 0x14) == 0x14
            if xo == 16:
                if w & 1:
                    return ('bclrl', None)
                return ('blr' if always else 'bclr', None)
            if xo == 528:
                if w & 1:
                    return ('bctrl', None)
                return ('bctr' if always else 'bcctr', None)
        return None

    # ---- entry discovery -----------------------------------------------------
    def entries(self):
        img = self.img
        opd = {}
        for a in range(OPD_LO, OPD_HI, 8):
            code, toc = img.u32(a), img.u32(a + 4)
            opd[a] = (code, toc)
        calls = collections.defaultdict(set)   # target -> callers
        for i, w in enumerate(self.w):
            a = TEXT_LO + 4 * i
            b = self.branch(a, w)
            if b and b[0] in ('bl', 'bcl') and in_text(b[1]):
                calls[b[1]].add(a)
        stubs = list(range(STUB_LO, STUB_HI, 32))
        return opd, calls, stubs

    # ---- recursive descent within one function --------------------------------
    def descend(self, start, limit, entryset, jt_targets, extra_roots=()):
        """Walk from start; never cross into another entry (that is a tail call).
        Returns (visited set, terminators, tailcalls, bctr sites, fallthrough_into_next)."""
        seen, work = set(), [start] + list(extra_roots)
        tails, bctrs, falls = set(), [], False
        while work:
            a = work.pop()
            while True:
                if a in seen:
                    break
                if a != start and a in entryset:
                    falls = True  # fell (or branched) into another entry
                    break
                if not (TEXT_LO <= a < TEXT_HI) or a >= limit:
                    falls = True
                    break
                seen.add(a)
                w = self.word(a)
                b = self.branch(a, w)
                if b is None or b[0] in ('bl', 'bcl', 'bctrl', 'bclrl'):
                    a += 4
                    continue
                k, t = b
                if k == 'b':
                    if t in entryset and t != start:
                        tails.add(t)
                    elif in_text(t):
                        work.append(t)
                    break
                if k == 'bc':
                    if t in entryset and t != start:
                        tails.add(t)
                    elif in_text(t):
                        work.append(t)
                    a += 4
                    continue
                if k in ('blr',):
                    break
                if k in ('bclr', 'bcctr'):
                    a += 4
                    continue
                if k == 'bctr':
                    bctrs.append(a)
                    for t2 in jt_targets.get(a, ()):
                        work.append(t2)
                    break
        return seen, tails, bctrs, falls

    # ---- jump tables -----------------------------------------------------------
    def find_jump_table(self, site, toc, preds):
        """Match the SN/GCC pattern ending at `site` (bctr):
             lwz rT, off(r2) ; (sldi|rldic|rlwinm) rI, rX, 2 ; lwzx rO, rI, rT ;
             extsw rS, rO ; add rD, rS, rT ; mtctr rD ; bctr
        Returns dict or None."""
        w = [self.word(site - 4 * k) for k in range(7)][::-1]  # w[6] == bctr
        def fld(x):
            return (x >> 21) & 31, (x >> 16) & 31, (x >> 11) & 31
        mtctr = w[5]
        if not (mtctr >> 26 == 31 and ((mtctr >> 1) & 1023) == 467 and ((mtctr >> 11) & 0x3ff) == 0x120):
            return None
        rd = (mtctr >> 21) & 31
        add = w[4]
        if not (add >> 26 == 31 and ((add >> 1) & 511) == 266):
            return None
        a_rt, a_ra, a_rb = fld(add)
        if a_rt != rd:
            return None
        ext = w[3]
        if not (ext >> 26 == 31 and ((ext >> 1) & 1023) == 986):
            return None
        e_rs, e_ra, _ = fld(ext)
        lwzx = w[2]
        if not (lwzx >> 26 == 31 and ((lwzx >> 1) & 1023) == 23):
            return None
        l_rt, l_ra, l_rb = fld(lwzx)
        if l_rt != e_rs:
            return None
        rbase = a_rb if a_ra == e_ra else a_ra
        if rbase not in (l_ra, l_rb):
            return None
        ridx = l_rb if rbase == l_ra else l_ra
        # Table base candidates: lwz rbase, d(rX) from the TOC (r2 or a copy of it),
        # or an inline table directly after the bctr.
        cands = []
        for k in range(3, 16):
            x = self.word(site - 4 * k)
            if x >> 26 == 32 and (x >> 21) & 31 == rbase:
                v = self.img.u32((toc + sx(x & 0xffff, 16)) & 0xffffffff)
                if v is not None:
                    cands.append(('toc' if (x >> 16) & 31 == 2 else 'toc_copy', v))
                break
        cands.append(('inline', site + 4))
        # Unscaled index: source of the 'x4' shift (rldic/rldicr/rlwinm rA, rS, 2, ...) writing ridx.
        scaled_from = None
        for k in range(2, 16):
            x = self.word(site - 4 * k)
            op = x >> 26
            if op in (21, 30) and (x >> 16) & 31 == ridx:
                scaled_from = (x >> 21) & 31
                break
        bound, dflt = self.find_bound(site, preds)
        return dict(site=site, cands=cands, idx_reg=scaled_from, scaled_reg=ridx, bound=bound, default=dflt)

    def compare_before(self, a, cr):
        """Find 'cmplwi crN, rX, imm' feeding a branch at a; return imm or None."""
        c = a - 4
        for _ in range(2048):
            y = self.word(c)
            if y >> 26 == 10 and (y >> 23) & 7 == cr and not (y >> 21) & 1:
                return y & 0xffff
            if (y >> 26 == 11 or y >> 26 == 31 and ((y >> 1) & 1023) in (0, 32)) and (y >> 23) & 7 == cr:
                return None  # a different compare on that field
            b = self.branch(c, y)
            if b and b[0] in ('b', 'blr', 'bctr'):
                return None
            c -= 4
        return None

    def find_bound(self, site, preds, depth=0, via_taken=False):
        """Walk back from site to the guard of the dispatch block:
           linear 'bgt crN, default' or 'bgtlr crN', or arriving from a
           'ble crN, block' predecessor; then its 'cmplwi crN, rX, imm'.
        Returns (case_count, default_target)."""
        a = site - 4
        for step in range(48):
            if a < TEXT_LO:
                return None, None
            x = self.word(a)
            b = self.branch(a, x)
            bo, bi = (x >> 21) & 31, (x >> 16) & 31
            cr, bit = bi >> 2, bi & 3
            if b and b[0] in ('bc', 'bclr') and bit == 1:
                if (bo & 0x1c) == 0x0c and not (via_taken and step == 0):   # bgt / bgtlr: fallthrough is in range
                    imm = self.compare_before(a, cr)
                    if imm is not None:
                        return imm + 1, (b[1] if b[0] == 'bc' else 'return')
                if (bo & 0x1c) == 0x04 and via_taken and step == 0 and b[0] == 'bc':  # ble taken into dispatch
                    imm = self.compare_before(a, cr)
                    if imm is not None:
                        return imm + 1, a + 4
            if b and b[0] in ('b', 'blr', 'bctr') and not (via_taken and step == 0):
                start = a + 4
                break
            if via_taken and step == 0:
                via_taken = False
                a -= 4
                continue
            a -= 4
        else:
            return None, None
        if depth < 3:
            for blk in range(site - 4, start - 4, -4):   # nearest block entry first
                for p in sorted(preds.get(blk, ())):
                    r = self.find_bound(p + 4, preds, depth + 1, via_taken=True)
                    if r[0]:
                        return r
        return None, None


def analyze(path):
    """Run the whole analysis; returns a dict of results."""
    img = Image(path)
    an = Analyzer(img)
    opd, calls, stubs = an.entries()

    # Intra-text branch predecessors (for jump-table bound search through block entries).
    preds = collections.defaultdict(set)
    for i, w in enumerate(an.w):
        a = TEXT_LO + 4 * i
        b = an.branch(a, w)
        if b and b[0] in ('b', 'bc') and in_text(b[1]):
            preds[b[1]].add(a)

    entry_src = collections.defaultdict(set)
    toc_of = {}
    for d, (code, toc) in opd.items():
        if in_text(code):
            entry_src[code].add('opd')
            toc_of.setdefault(code, toc)
    for t, srcs in calls.items():
        entry_src[t].add('call')
    for s in stubs:
        entry_src[s].add('import_stub')
    entry_src[0x10200].add('crt')

    entries = sorted(entry_src)
    entryset = set(entries)

    # TOC propagation for call-only entries: inherit from the caller's function.
    def owner(a):
        i = bisect.bisect_right(entries, a) - 1
        return entries[i] if i >= 0 else None
    for _ in range(4):
        for t in entries:
            if t in toc_of:
                continue
            tocs = {toc_of.get(owner(c)) for c in calls.get(t, ())} - {None}
            if len(tocs) == 1:
                toc_of[t] = tocs.pop()

    # C++ exception landing pads (runtime library only; game code has no EH tables).
    fdes, pads = landing_pads(img.d)
    eh_roots = collections.defaultdict(list)
    for fpc, lp in pads:
        eh_roots[owner(lp)].append(lp)

    # Pass 1: descent without jump tables, collect bctr sites; pass 2 with tables.
    jt_targets, tables = {}, {}
    for rnd in range(12):
        funcs = []
        new_tables = False
        for i, s in enumerate(entries):
            limit = entries[i + 1] if i + 1 < len(entries) else TEXT_HI
            seen, tails, bctrs, falls = an.descend(s, TEXT_HI, entryset, jt_targets, eh_roots.get(s, ()))
            funcs.append((s, limit, seen, tails, bctrs, falls))
            for site in bctrs:
                if site in tables:
                    continue
                if STUB_LO <= site < STUB_HI:
                    tables[site] = dict(site=site, status='import_stub')
                    continue
                if site in COMPUTED_JUMPS:
                    tables[site] = dict(site=site, status='computed_jump', function=s,
                                        labels=COMPUTED_JUMPS[site])
                    jt_targets[site] = COMPUTED_JUMPS[site]
                    new_tables = True
                    continue
                toc = toc_of.get(s)
                if toc is None:
                    tables[site] = dict(site=site, status='no_toc')
                    continue
                jt = an.find_jump_table(site, toc, preds)
                if jt is None:
                    tables[site] = dict(site=site, status='not_switch')
                    continue
                # Read table from each base candidate; keep the first fully valid one.
                best = None
                for how, base in jt.pop('cands'):
                    n = jt['bound'] or 4096
                    labels = []
                    for k in range(n):
                        v = img.u32(base + 4 * k)
                        if v is None:
                            break
                        tgt = (base + sx(v, 32)) & 0xffffffff
                        if not (s <= tgt < TEXT_HI) or not in_text(tgt) or (tgt in entryset and tgt != s):
                            break
                        labels.append(tgt)
                    if jt['bound'] and len(labels) == jt['bound']:
                        best = (how, base, labels)
                        break
                    if best is None or len(labels) > len(best[2]):
                        best = (how, base, labels)
                jt['base_source'], jt['base'], labels = best
                jt['labels'] = labels
                jt['status'] = 'ok' if jt['bound'] and len(labels) == jt['bound'] else (
                    'bound_mismatch' if jt['bound'] else 'unbounded_guess')
                jt['function'] = s
                tables[site] = jt
                jt_targets[site] = labels
                new_tables = True
        if not new_tables:
            break

    # ---- boundaries & holes --------------------------------------------------
    func_rows = []
    covered = set()
    overlap = 0
    for s, limit, seen, tails, bctrs, falls in funcs:
        end = max(seen) + 4 if seen else s
        if end > limit:
            overlap += 1
        covered |= seen
        kinds = sorted(entry_src[s])
        w0 = an.word(s)
        if STUB_LO <= s < STUB_HI:
            kind = 'import_stub'
        elif w0 == 0xf8410028 and len(seen) == 4 and tails:   # std r2,40(r1); addis/addi r2; b
            kind = 'toc_trampoline'
        else:
            kind = 'function'
        func_rows.append(dict(address=s, kind=kind, end=end, next_entry=limit, size=end - s,
                              toc=toc_of.get(s), sources=kinds,
                              instructions=len(seen), tail_calls=sorted(tails),
                              falls_into_next=falls,
                              jump_tables=[b for b in bctrs if tables.get(b, {}).get('status') in
                                           ('ok', 'bound_mismatch', 'unbounded_guess')]))

    # Jump-table data words count as data, not unexplained code.
    table_words = set()
    for t in tables.values():
        if t['status'] == 'ok' and t.get('labels') and t.get('base') is not None and TEXT_LO <= t['base'] < TEXT_HI:
            for k in range(len(t['labels'])):
                table_words.add(t['base'] + 4 * k)
    covered_code = len(covered)
    covered |= table_words

    # Unreached words, grouped into runs, classified.
    holes = []
    run = None
    for i, w in enumerate(an.w):
        a = TEXT_LO + 4 * i
        if a in covered:
            if run:
                holes.append(run)
                run = None
            continue
        if run is None:
            run = [a, a + 4, 0, 0]
        else:
            run[1] = a + 4
        if w in (0, 0x60000000):
            run[2] += 1
        else:
            run[3] += 1
    if run:
        holes.append(run)
    pad_holes = [h for h in holes if h[3] == 0]
    code_holes = [h for h in holes if h[3] > 0]

    # Save/restore-register helper runs (many bl targets in a straight std/ld run).
    # Indirect call/branch census
    bctrl = sum(1 for i, w in enumerate(an.w) if an.branch(TEXT_LO + 4 * i, w) == ('bctrl', None))
    status = collections.Counter(t['status'] for t in tables.values())

    # Functions whose descent runs into the next entry end in a call that never
    # returns (abort/exit/_Unwind_Resume); record those callees.
    noreturn, odd_fallthrough = set(), []
    for r in func_rows:
        if not r['falls_into_next']:
            continue
        a = r['next_entry'] - 4
        while a > r['address'] and an.word(a) in (0, 0x60000000, 0x7fe00008):
            a -= 4
        b = an.branch(a, an.word(a))
        if b and b[0] == 'bl':
            noreturn.add(b[1])
        elif a >= 4 and an.word(a) == 0xe8410028 and an.branch(a - 4, an.word(a - 4))[0] == 'bl':
            noreturn.add(an.branch(a - 4, an.word(a - 4))[1])   # bl; ld r2,40(r1) (cross-TOC call)
        else:
            odd_fallthrough.append(r['address'])

    total = len(an.w)
    report = dict(
        elf_sha256=EXPECTED,
        text_range=[hex(TEXT_LO), hex(TEXT_HI)],
        text_words=total,
        entries=len(entries),
        entries_by_kind=dict(collections.Counter(r['kind'] for r in func_rows)),
        entries_by_source={k: sum(1 for e in entries if k in entry_src[e]) for k in
                           ('opd', 'call', 'import_stub', 'crt')},
        call_only_entries=sum(1 for e in entries if entry_src[e] == {'call'}),
        entries_without_toc=[hex(e) for e in entries if e not in toc_of and not STUB_LO <= e < STUB_HI],
        code_words=covered_code,
        inline_table_words=len(table_words),
        covered_words=len(covered),
        covered_pct=round(100.0 * len(covered) / total, 3),
        padding_words=sum(h[2] for h in holes),
        unexplained_code_words=sum(h[3] for h in code_holes),
        unexplained_code_runs=len(code_holes),
        functions_extending_past_next_entry=overlap,
        functions_ending_in_noreturn_call=sum(1 for r in func_rows if r['falls_into_next']) - len(odd_fallthrough),
        functions_genuinely_falling_through=[hex(x) for x in odd_fallthrough],
        indirect_calls_bctrl=bctrl,
        bctr_sites=len(tables),
        bctr_status=dict(status),
        eh_fdes=len(fdes),
        eh_landing_pads=len(pads),
        noreturn_callees=sorted(hex(x) for x in noreturn),
        jump_table_labels=sum(len(t.get('labels', [])) for t in tables.values()),
    )

    return dict(img=img, an=an, funcs=funcs, func_rows=func_rows, tables=tables, entries=entries,
                entry_src=entry_src, toc_of=toc_of, report=report, noreturn=noreturn,
                eh_roots=eh_roots, covered=covered, code_holes=code_holes)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    res = analyze(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    report, func_rows, tables, code_holes = res['report'], res['func_rows'], res['tables'], res['code_holes']

    def hx(r):
        r = dict(r)
        for k in ('address', 'end', 'next_entry', 'toc', 'site', 'base', 'default', 'function'):
            if isinstance(r.get(k), int):
                r[k] = hex(r[k])
        for k in ('tail_calls', 'labels', 'jump_tables'):
            if k in r:
                r[k] = [hex(x) for x in r[k]]
        return r

    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (out / 'functions.json').write_text(json.dumps([hx(r) for r in func_rows]) + '\n')
    (out / 'jump_tables.json').write_text(json.dumps([hx(t) for t in sorted(tables.values(), key=lambda t: t['site'])], indent=1) + '\n')
    (out / 'unexplained_code.json').write_text(json.dumps(
        [dict(start=hex(h[0]), end=hex(h[1]), code_words=h[3], pad_words=h[2]) for h in code_holes], indent=1) + '\n')

    # XenonRecomp-style switch table file, as a starting point for a PPU recompiler config.
    lines = ['# Switch tables for BCUS98127 02.00 EBOOT.elf, XenonRecomp-style layout.',
             '# base = bctr address; labels = case targets in index order; default = bound-check target.',
             '# r = unscaled index register at the x4 shift. The compiler often reuses it before the',
             '# bctr, so a recompiler should dispatch on the CTR value (target address), not on r.',
             '']
    for t in sorted(tables.values(), key=lambda t: t['site']):
        if t['status'] != 'ok':
            continue
        lines.append('[[switch]]')
        lines.append('base = 0x%X' % t['site'])
        lines.append('r = %d' % t['idx_reg'])
        lines.append('default = 0x%X' % (t['default'] if isinstance(t['default'], int) else 0))
        lines.append('labels = [%s]' % ', '.join('0x%X' % x for x in t['labels']))
        lines.append('')
    (out / 'switch_tables.toml').write_text('\n'.join(lines))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
