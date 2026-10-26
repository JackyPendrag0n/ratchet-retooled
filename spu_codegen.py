#!/usr/bin/env python3
"""SPU static recompiler for BCUS98127: turns the SPU code embedded in EBOOT.elf into C++.

The game's SPU programs (job manager, jobs, collision, audio) are copied into local store at
addresses only known at run time, so every generated block is position independent: it gets its
LS address as an argument and returns the next pc. Blocks are keyed by their bytes; at run time
the dispatcher (host/spu_recomp.cpp) matches the code actually in local store against them and
falls back to the interpreter for anything else.

usage: spu_codegen.py EBOOT.elf outdir
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools'))
from spu_isa import OPS, decode  # noqa: E402

EBOOT_SHA256 = '0ee9a841'
# SPU code in the data segment (file offset = va - 0x10060000 + 0x900000 for the second range,
# va - 0x10000000 + 0x8a0000 for the first).
REGIONS = [(0x10048000, 0x10056b80), (0x100d4000, 0x101aef00)]
MAX_BLOCK = 200
FILES = 16

# Instructions whose semantics live in spu_ops.h and need only the instruction word.
SIMPLE = {
    'selb', 'shufb', 'mpya', 'fnms', 'fma', 'fms', 'ila', 'il', 'ilh', 'ilhu', 'iohl', 'fsmbi', 'ai', 'ahi', 'sfi',
    'sfhi', 'andi', 'andhi', 'andbi', 'ori', 'orhi', 'orbi', 'xori', 'xorhi', 'xorbi', 'mpyi', 'mpyui', 'cgti',
    'cgthi', 'cgtbi', 'ceqi', 'ceqhi', 'ceqbi', 'clgti', 'clgthi', 'clgtbi', 'a', 'ah', 'sf', 'sfh', 'and', 'andc',
    'or', 'orc', 'xor', 'nand', 'nor', 'eqv', 'orx', 'cg', 'bg', 'addx', 'sfx', 'cgx', 'bgx', 'absdb', 'avgb', 'sumb',
    'clz', 'cntb', 'xsbh', 'xshw', 'xswd', 'cgt', 'cgth', 'cgtb', 'clgt', 'clgth', 'clgtb', 'ceq', 'ceqh', 'ceqb',
    'mpy', 'mpyu', 'mpyh', 'mpys', 'mpyhh', 'mpyhhu', 'mpyhha', 'mpyhhau', 'rot', 'rotm', 'rotma', 'shl', 'roth',
    'rothm', 'rotmah', 'shlh', 'roti', 'rotmi', 'rotmai', 'shli', 'rothi', 'rothmi', 'rotmahi', 'shlhi', 'rotqby',
    'rotqbybi', 'rotqbyi', 'rotqmby', 'rotqmbybi', 'rotqmbyi', 'shlqby', 'shlqbybi', 'shlqbyi', 'rotqbi', 'rotqbii',
    'rotqmbi', 'rotqmbii', 'shlqbi', 'shlqbii', 'cbx', 'chx', 'cwx', 'cdx', 'cbd', 'chd', 'cwd', 'cdd', 'gb', 'gbh',
    'gbb', 'fsm', 'fsmh', 'fsmb', 'lqd', 'stqd', 'lqx', 'stqx', 'lqa', 'stqa', 'fa', 'fs', 'fm', 'fcgt', 'fceq',
    'fcmgt', 'fcmeq', 'frest', 'frsqest', 'fi', 'cflts', 'cfltu', 'csflt', 'cuflt', 'dfa', 'dfs', 'dfm', 'dfma',
    'dfms', 'dfnms', 'dfnma', 'dfcgt', 'dfceq', 'dfcmgt', 'dfcmeq', 'fesd', 'frds', 'dftsv', 'fscrrd',
}
CPP_NAME = {'and': 'and_', 'or': 'or_', 'xor': 'xor_'}
NOPS = {'hbra', 'hbrr', 'hbr', 'lnop', 'nop', 'sync', 'dsync', 'fscrwr', 'mtspr'}
REL_BRANCH = {'br', 'brsl', 'brz', 'brnz', 'brhz', 'brhnz'}
ABS_BRANCH = {'bra', 'brasl'}
IND_BRANCH = {'bi', 'bisl', 'biz', 'binz', 'bihz', 'bihnz'}


# Channel writes that never block or halt (MFC command parameters, the command itself, tag mask
# and tag update) run inline; with interrupts enabled the block hands the write back to the
# interpreter, which checks for an interrupt after it.
INLINE_WRCH = set(range(16, 24))


def is_terminator(name, w):
    return name in REL_BRANCH or name in ABS_BRANCH or name in IND_BRANCH


def is_complex(name, w):
    """Instructions the generated code leaves to the interpreter (the block ends before them)."""
    if name is None:
        return True
    if name in IND_BRANCH and (w >> 14) & 0x30:  # interrupt enable/disable flags
        return True
    if name == 'wrch' and (w >> 7) & 0x7f in INLINE_WRCH:
        return False
    return not (name in SIMPLE or name in NOPS or name in ('lqr', 'stqr', 'mfspr', 'rchcnt') or is_terminator(name, w))


def emit_insn(name, w, k):
    """C++ for one non-terminating instruction at byte offset k from the block start."""
    if name in NOPS:
        return None
    if name == 'mfspr':
        return 'G(%d).q = 0;' % (w & 0x7f)
    if name == 'rchcnt':
        return 'G(%d) = spu::pref_only(spu_channel_count(s, %du));' % (w & 0x7f, (w >> 7) & 0x7f)
    if name == 'wrch':
        # bit 0 of the returned pc: run the instruction there in the interpreter
        return ('if (s.int_enabled) return (pc + %du) | 1u; s.pc = pc + %du; spu_write_channel(s, %du, PREF(%d));'
                % (k, k, (w >> 7) & 0x7f, w & 0x7f))
    if name in ('lqr', 'stqr'):
        return 'spu::%s(s, 0x%08xu, pc + %du);' % (name, w, k)
    return 'spu::%s(s, 0x%08xu);' % (CPP_NAME.get(name, name), w)


def sext16(x):
    return x - 0x10000 if x & 0x8000 else x


def emit_terminator(name, w, k):
    i16 = sext16((w >> 7) & 0xffff)
    rt, ra = w & 0x7f, (w >> 7) & 0x7f
    nxt = 'pc + %du' % (k + 4)
    rel = '((pc + %du) & 0x3fffcu)' % ((k + (i16 << 2)) & 0xffffffff)
    ab = '0x%xu' % ((i16 << 2) & 0x3fffc)
    if name == 'br':
        return ['return %s;' % rel]
    if name == 'bra':
        return ['return %s;' % ab]
    if name == 'brsl':
        return ['G(%d) = spu::pref_only(pc + %du);' % (rt, k + 4), 'return %s;' % rel]
    if name == 'brasl':
        return ['G(%d) = spu::pref_only(pc + %du);' % (rt, k + 4), 'return %s;' % ab]
    if name == 'brz':
        return ['return PREF(%d) == 0 ? %s : %s;' % (rt, rel, nxt)]
    if name == 'brnz':
        return ['return PREF(%d) != 0 ? %s : %s;' % (rt, rel, nxt)]
    if name == 'brhz':
        return ['return SPU_H(G(%d), 1) == 0 ? %s : %s;' % (rt, rel, nxt)]
    if name == 'brhnz':
        return ['return SPU_H(G(%d), 1) != 0 ? %s : %s;' % (rt, rel, nxt)]
    tgt = '(PREF(%d) & 0x3fffcu)' % ra
    if name == 'bi':
        return ['return %s;' % tgt]
    if name == 'bisl':
        return ['const u32 t = %s;' % tgt, 'G(%d) = spu::pref_only(pc + %du);' % (rt, k + 4), 'return t;']
    if name == 'biz':
        return ['return PREF(%d) == 0 ? %s : %s;' % (rt, tgt, nxt)]
    if name == 'binz':
        return ['return PREF(%d) != 0 ? %s : %s;' % (rt, tgt, nxt)]
    if name == 'bihz':
        return ['return SPU_H(G(%d), 1) == 0 ? %s : %s;' % (rt, tgt, nxt)]
    if name == 'bihnz':
        return ['return SPU_H(G(%d), 1) != 0 ? %s : %s;' % (rt, tgt, nxt)]
    raise ValueError(name)


def load_regions(elf):
    if hashlib.sha256(elf).hexdigest()[:8] != EBOOT_SHA256:
        raise SystemExit('EBOOT.elf does not match BCUS98127 02.00')
    phoff = struct.unpack('>Q', elf[0x20:0x28])[0]
    phnum = struct.unpack('>H', elf[0x38:0x3a])[0]
    segs = []
    for i in range(phnum):
        p = struct.unpack('>IIQQQQQQ', elf[phoff + i * 56:phoff + i * 56 + 56])
        if p[0] == 1 and p[5]:
            segs.append((p[2], p[3], p[5]))
    out = []
    for lo, hi in REGIONS:
        for off, va, sz in segs:
            if va <= lo and hi <= va + sz:
                b = elf[off + lo - va:off + hi - va]
                out.append((lo, struct.unpack('>%dI' % (len(b) // 4), b)))
                break
        else:
            raise SystemExit('region 0x%x not in a segment' % lo)
    return out


def block_starts(words, extra=()):
    starts = {0} | set(extra)
    prev_pad = False
    for i, w in enumerate(words):
        # loop entries reached through "ila rX, label; bi rX" are usually aligned with nop/lnop padding
        name0, _ = decode(w)
        pad = name0 in ('nop', 'lnop')
        if prev_pad and not pad:
            starts.add(i)
        prev_pad = pad
        name, f = decode(w)
        if name in REL_BRANCH:
            t = i + sext16((w >> 7) & 0xffff)
            if 0 <= t < len(words):
                starts.add(t)
        if name is None or is_terminator(name, w) or is_complex(name, w) or name == 'brsl':
            starts.add(i + 1)
    return sorted(s for s in starts if s < len(words))


def build_block(words, i):
    """Returns (words of the block, lines) or None when the first instruction is complex."""
    lines = []
    n = 0
    while n < MAX_BLOCK and i + n < len(words):
        w = words[i + n]
        name, _ = decode(w)
        if is_complex(name, w):
            break
        k = n * 4
        if is_terminator(name, w):
            lines += emit_terminator(name, w, k)
            n += 1
            return words[i:i + n], lines
        c = emit_insn(name, w, k)
        if c:
            lines.append(c)
        n += 1
    if n == 0:
        return None
    lines.append('return pc + %du;' % (n * 4))
    return words[i:i + n], lines


def main():
    elf_path, out = sys.argv[1], sys.argv[2]
    elf = open(elf_path, 'rb').read()
    os.makedirs(out, exist_ok=True)
    # Entry points seen at run time that no block covered (TOD_SPU_PROFILE=1 TOD_SPU_ENTRIES=file):
    # 32 bytes of code each, found again here in the EBOOT. A line is either those 32 bytes in hex (what
    # the profiler writes) or "h:" and the first 16 hex digits of their SHA-256, the form the shipped
    # spu_entries.txt uses so that it holds no game code.
    entries, hashed = [], set()
    ent_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'spu_entries.txt')
    if os.path.exists(ent_path):
        for l in open(ent_path):
            l = l.strip()
            if len(l) == 64:
                entries.append(bytes.fromhex(l))
            elif l.startswith('h:') and len(l) == 18:
                hashed.add(l[2:])
    blocks = {}  # bytes -> (index, lines)
    found = set()
    for base, words in load_regions(elf):
        raw = struct.pack('>%dI' % len(words), *words)
        extra = set()
        for e in entries:
            j = raw.find(e)
            while j >= 0:
                if j % 4 == 0:
                    extra.add(j // 4)
                    found.add(e)
                j = raw.find(e, j + 1)
        if hashed:
            for j in range(0, len(raw) - 31, 4):
                h = hashlib.sha256(raw[j:j + 32]).hexdigest()[:16]
                if h in hashed:
                    extra.add(j // 4)
                    found.add(h)
        for i in block_starts(words, extra):
            r = build_block(words, i)
            if not r:
                continue
            bw, lines = r
            key = struct.pack('>%dI' % len(bw), *bw)
            if key not in blocks:
                blocks[key] = (len(blocks), lines)
    if entries or hashed:
        print('runtime entries found in the EBOOT: %d of %d' % (len(found), len(entries) + len(hashed)))
    order = sorted(blocks.items(), key=lambda kv: kv[1][0])
    per_file = (len(order) + FILES - 1) // FILES
    sources = []
    for f in range(FILES):
        chunk = order[f * per_file:(f + 1) * per_file]
        name = 'spu_code_%02d.cpp' % f
        sources.append(name)
        with open(os.path.join(out, name), 'w') as fp:
            fp.write('// Generated by spu_codegen.py from EBOOT.elf. Do not edit.\n')
            fp.write('#include "spu_ops.h"\n#include "spu_blocks.h"\n\n')
            for key, (idx, lines) in chunk:
                fp.write('u32 spu_b%d(SPUThread& s, u32 pc) {\n' % idx)
                for ln in lines:
                    fp.write('    %s\n' % ln)
                fp.write('}\n')
    with open(os.path.join(out, 'spu_blocks.h'), 'w') as fp:
        fp.write('// Generated by spu_codegen.py. Do not edit.\n#pragma once\n#include "spu_core.h"\n\n')
        fp.write('using SpuBlockFn = u32 (*)(SPUThread&, u32);\n')
        fp.write('struct SpuBlockInfo { SpuBlockFn fn; u32 offset, len; };  // offset/len into g_spu_block_bytes\n')
        fp.write('extern const SpuBlockInfo g_spu_blocks[];\nextern const u32 g_spu_block_count;\n')
        fp.write('extern const u8 g_spu_block_bytes[];\n')
    with open(os.path.join(out, 'spu_table.cpp'), 'w') as fp:
        fp.write('// Generated by spu_codegen.py. Do not edit.\n#include "spu_blocks.h"\n\n')
        for key, (idx, lines) in order:
            fp.write('u32 spu_b%d(SPUThread&, u32);\n' % idx)
        fp.write('\nconst u8 g_spu_block_bytes[] = {\n')
        offsets = []
        pos = 0
        for key, (idx, lines) in order:
            offsets.append(pos)
            fp.write(','.join(str(b) for b in key) + ',\n')
            pos += len(key)
        fp.write('};\n\nconst SpuBlockInfo g_spu_blocks[] = {\n')
        for (key, (idx, lines)), off in zip(order, offsets):
            fp.write('    {spu_b%d, %d, %d},\n' % (idx, off, len(key)))
        fp.write('};\nconst u32 g_spu_block_count = %d;\n' % len(order))
    sources.append('spu_table.cpp')
    with open(os.path.join(out, 'sources.txt'), 'w') as fp:
        fp.write('\n'.join(sources) + '\n')
    ninsn = sum(len(k) // 4 for k in blocks)
    print('%d blocks, %d instructions (with overlap), %d files' % (len(blocks), ninsn, len(sources)))


if __name__ == '__main__':
    main()
