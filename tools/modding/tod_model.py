#!/usr/bin/env python3
"""Export and import Ratchet & Clank: Tools of Destruction models and textures.

  tod_model.py info     <level>                      moby classes, mesh groups, meshes
  tod_model.py export   <level> <class> <out dir>    one class as glTF 2.0 (+ textures as PNG)
  tod_model.py textures <level> <out dir> [class]    textures as PNG, named like texture pack files
  tod_model.py import   <level> <class> <model.gltf|.glb> <data dir>
                                                     writes the edited class into a rebuilt
                                                     level archive in <data dir>/mods

<level> is a level folder of the game, e.g. PS3_GAME/USRDIR/packed/levels/metropolis (the
folder holding level_cached.psarc). Ratchet is class 60 in Metropolis; `info` lists the rest.
Formats are described in docs/model_formats.md. Only the Python standard library is needed.
"""
import argparse, base64, hashlib, json, math, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psarc import Psarc  # noqa: E402

# ---------------------------------------------------------------------------------------------
# small helpers

def u16(b, o): return struct.unpack_from('>H', b, o)[0]
def u32(b, o): return struct.unpack_from('>I', b, o)[0]
def f32(b, o): return struct.unpack_from('>f', b, o)[0]
def half(h): return struct.unpack('<e', struct.pack('<H', h))[0]
def to_half(f): return struct.unpack('<H', struct.pack('<e', max(-65504.0, min(65504.0, f))))[0]

def s11(v): return (v & 0x7ff) - (0x800 if v & 0x400 else 0)
def s10(v): return (v & 0x3ff) - (0x400 if v & 0x200 else 0)

def unpack_cmp(v):
    """RSX CMP vector: x 11 bits, y 11 bits, z 10 bits, signed, as floats in -1..1."""
    return (s11(v) / 1023.0, s11(v >> 11) / 1023.0, s10(v >> 22) / 511.0)

def pack_cmp(x, y, z):
    def q(f, bits):
        m = (1 << (bits - 1)) - 1
        return int(round(max(-1.0, min(1.0, f)) * m)) & ((1 << bits) - 1)
    return q(x, 11) | (q(y, 11) << 11) | (q(z, 10) << 22)

def norm(v):
    l = math.sqrt(sum(c * c for c in v))
    return tuple(c / l for c in v) if l > 1e-12 else (0.0, 1.0, 0.0)

def mat_mul(a, b):  # 4x4 row-major lists of 16
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]

def mat_inv(m):
    a = [list(m[r * 4:r * 4 + 4]) + [1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        if abs(a[c][c]) < 1e-12: return [1.0 if r == c else 0.0 for r in range(4) for c in range(4)]
        d = a[c][c]
        a[c] = [x / d for x in a[c]]
        for r in range(4):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [a[r][4 + c] for r in range(4) for c in range(4)]

def write_png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

# xxh64, as the recompiler hashes textures (rsx_tex.cpp), so exported PNGs carry texture pack names
_P1, _P2, _P3, _P4, _P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
_M = (1 << 64) - 1
def _rotl(x, r): return ((x << r) | (x >> (64 - r))) & _M
def _round(acc, v): return (_rotl((acc + v * _P2) & _M, 31) * _P1) & _M
def xxh64(data, seed=0):
    n = len(data); p = 0
    if n >= 32:
        v = [(seed + _P1 + _P2) & _M, (seed + _P2) & _M, seed, (seed - _P1) & _M]
        lanes = struct.unpack_from('<%dQ' % ((n // 32) * 4), data)
        for i in range(0, len(lanes), 4):
            v[0] = _round(v[0], lanes[i]); v[1] = _round(v[1], lanes[i + 1])
            v[2] = _round(v[2], lanes[i + 2]); v[3] = _round(v[3], lanes[i + 3])
        p = (n // 32) * 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & _M
        for x in v: h = ((h ^ _round(0, x)) * _P1 + _P4) & _M
    else:
        h = (seed + _P5) & _M
    h = (h + n) & _M
    while p + 8 <= n:
        h ^= _round(0, struct.unpack_from('<Q', data, p)[0]); h = (_rotl(h, 27) * _P1 + _P4) & _M; p += 8
    if p + 4 <= n:
        h ^= (struct.unpack_from('<I', data, p)[0] * _P1) & _M; h = (_rotl(h, 23) * _P2 + _P3) & _M; p += 4
    while p < n:
        h ^= (data[p] * _P5) & _M; h = (_rotl(h, 11) * _P1) & _M; p += 1
    h ^= h >> 33; h = (h * _P2) & _M; h ^= h >> 29; h = (h * _P3) & _M; h ^= h >> 32
    return h

# ---------------------------------------------------------------------------------------------
# level files

LEVEL_FILES = ('main.dat', 'vertices.dat', 'textures.dat')

class Level:
    """A level's main.dat, vertices.dat and textures.dat, read from its level_cached.psarc (or
    from a folder with those files extracted)."""

    def __init__(self, folder, archive=None):
        self.folder = folder
        self.name = os.path.basename(os.path.normpath(folder))
        self.archive = archive or os.path.join(folder, 'level_cached.psarc')
        self.files = {}
        if os.path.exists(self.archive):
            self.pa = Psarc(self.archive)
            for fn in LEVEL_FILES:
                key = next(k for k in self.pa.files if k.endswith('/' + fn))
                self.files[fn] = bytearray(self.pa.read(self.pa.files[key]))
        else:
            self.pa = None
            for fn in LEVEL_FILES:
                self.files[fn] = bytearray(open(os.path.join(folder, fn), 'rb').read())
        self.sizes = {fn: len(d) for fn, d in self.files.items()}  # as loaded, before any edit
        self.main = self.files['main.dat']
        self.vert = self.files['vertices.dat']
        self.tex = self.files['textures.dat']
        m = self.main
        assert m[:4] == b'IGHW', 'main.dat is not an IGHW file'
        self.sections = {}
        for i in range(u32(m, 8)):
            sid, off, cnt, sz = struct.unpack_from('>IIII', m, 0x20 + i * 16)
            n = cnt & 0x0fffffff
            self.sections[sid] = (off, n, sz, bool(cnt & 0x10000000))

    def sec(self, sid): return self.sections[sid]

    # ---- moby classes (0xD100)
    def class_count(self): return self.sec(0xD100)[1]

    def cls(self, ci):
        off, n, sz, _ = self.sec(0xD100)
        if not 0 <= ci < n: raise SystemExit('class %d does not exist (0..%d)' % (ci, n - 1))
        b = off + ci * sz
        m = self.main
        c = dict(index=ci, addr=b, sphere=struct.unpack_from('>4f', m, b), skel=u32(m, b + 0x20),
                 group_table=u32(m, b + 0x28), group_count=u16(m, b + 0x18),
                 ioff=u32(m, b + 0x34), voff_raw=u32(m, b + 0x38), scale=f32(m, b + 0x3c))
        # bit 31: vertices in vertices.dat (main memory, skinned on the SPUs); else in textures.dat
        c['vin_main'] = bool(c['voff_raw'] & 0x80000000)
        c['vbase'] = (c['voff_raw'] & 0x7fffffff) + (0x80 if c['vin_main'] else 0)
        c['ibase'] = c['ioff']  # index lists are in textures.dat (RSX local memory)
        c['groups'] = []
        for g in range(c['group_count'] if c['group_table'] else 0):
            mp, mn = struct.unpack_from('>II', m, c['group_table'] + g * 8)
            c['groups'].append([self.mesh(mp + k * 64) for k in range(mn)] if mp else [])
        c['bones'] = u16(m, c['skel']) if c['skel'] else 0
        return c

    def mesh(self, a):
        m = self.main
        M = struct.unpack_from('>16I', m, a)
        me = dict(addr=a, istart=M[0], vbyte=M[1], material=M[2] >> 16, nv=M[2] & 0xffff, flags=M[3], nidx=M[4],
                  lods=M[5:8], palette_ptr=M[8], raw=M)
        me['skinned'] = bool((M[3] >> 16) & 0xff)
        me['stride'] = 28 if me['skinned'] else 20
        me['palette_n'] = M[3] >> 24
        p = me['palette_ptr']
        me['palette'] = [u16(m, p + 2 * i) for i in range(max(1, me['palette_n']))] if p else [0]
        return me

    def vsrc(self, c): return self.vert if c['vin_main'] else self.tex

    def skeleton(self, c):
        m = self.main; s = c['skel']; n = u16(m, s)
        hier, world, inv = u32(m, s + 4), u32(m, s + 8), u32(m, s + 12)
        parents = [struct.unpack_from('>h', m, hier + i * 8 + 2)[0] for i in range(n)]
        W = [list(struct.unpack_from('>16f', m, world + i * 64)) for i in range(n)]
        I = [list(struct.unpack_from('>16f', m, inv + i * 64)) for i in range(n)]
        return parents, W, I

    def read_mesh(self, c, me):
        """positions, normals, uvs, joints (bone ids), weights, tangent sign, triangles"""
        src = self.vsrc(c); base = c['vbase'] + me['vbyte']; st = me['stride']; sc = c['scale']
        P, N, UV, J, Wt, S, TG = [], [], [], [], [], [], []
        for i in range(me['nv']):
            o = base + i * st
            x, y, z, w = struct.unpack_from('>4h', src, o)
            P.append((x * sc, y * sc, z * sc)); S.append(-1 if w < 0 else 1)
            if me['skinned']:
                bones = src[o + 8:o + 12]; wts = src[o + 12:o + 16]; q = o + 16
                J.append(tuple(me['palette'][b] if b < len(me['palette']) else 0 for b in bones))
                Wt.append(tuple(v / 255.0 for v in wts))
            else:
                q = o + 8
                J.append((me['palette'][0], 0, 0, 0)); Wt.append((1.0, 0.0, 0.0, 0.0))
            uu, vv = struct.unpack_from('>2H', src, q)
            UV.append((half(uu), half(vv)))
            N.append(norm(unpack_cmp(u32(src, q + 4))))
            TG.append(norm(unpack_cmp(u32(src, q + 8))))
        ib = c['ibase'] + me['istart'] * 2
        idx = struct.unpack_from('>%dH' % me['nidx'], self.tex, ib)
        tris = [idx[t:t + 3] for t in range(0, len(idx) - 2, 3)]
        # glTF tangent: xyz and the bitangent sign (the opposite of the stored w)
        self.last_tangents = [t + (float(-s),) for t, s in zip(TG, S)]
        return P, N, UV, J, Wt, S, tris

    # ---- textures (0x5200 headers: the RSX texture registers) and materials (0x5000)
    def texture_header(self, ti):
        off, n, sz, _ = self.sec(0x5200)
        h = struct.unpack_from('>8I', self.main, off + ti * sz)
        return dict(index=ti, offset=h[0], format=h[1], remap=h[4] & 0xffff, width=h[6] >> 16, height=h[6] & 0xffff,
                    pitch=h[7] & 0xfffff, regs=h)

    def material_textures(self, mi):
        moff, mn, msz, _ = self.sec(0x5000)
        toff, tn, tsz, _ = self.sec(0x5200)
        # +0 diffuse, +4 normal map (DXT5, remap 0x6ae0: x in A, y in G), +8 specular-like mask; 0 = unused
        out = []
        for k in (0, 4, 8):
            p = u32(self.main, moff + mi * msz + k)
            if toff <= p < toff + tn * tsz and (p - toff) % tsz == 0: out.append((p - toff) // tsz)
        return out

# ---------------------------------------------------------------------------------------------
# texture decoding (a port of host/rsx_tex.cpp, enough for the formats levels use)

TEX_B8, TEX_A1R5G5B5, TEX_A4R4G4B4, TEX_R5G6B5, TEX_A8R8G8B8 = 0x81, 0x82, 0x83, 0x84, 0x85
TEX_DXT1, TEX_DXT23, TEX_DXT45, TEX_G8B8, TEX_D8R8G8B8 = 0x86, 0x87, 0x88, 0x8B, 0x9E
BLOCK = {TEX_B8: (1, 1), TEX_A1R5G5B5: (2, 1), TEX_A4R4G4B4: (2, 1), TEX_R5G6B5: (2, 1), TEX_A8R8G8B8: (4, 1),
         TEX_DXT1: (8, 4), TEX_DXT23: (16, 4), TEX_DXT45: (16, 4), TEX_G8B8: (2, 1), TEX_D8R8G8B8: (4, 1)}
BASE_ARGB = {TEX_A4R4G4B4: 'RGBA', TEX_G8B8: 'GRGR', TEX_B8: '1RRR', TEX_D8R8G8B8: '1RGB'}

def tex_layout(h):
    """(base format, linear, levels [(offset, w, h, wb, hb, pitch, full_h, border)], total bytes)"""
    reg = h['format']; fmt = (reg >> 8) & 0xff & ~0x60; linear = bool((reg >> 8) & 0x20)
    if fmt not in BLOCK: return None
    bpb, bw = BLOCK[fmt]; dxt = fmt in (TEX_DXT1, TEX_DXT23, TEX_DXT45)
    w, hh = max(h['width'], 1), max(h['height'], 1)
    border = not ((reg >> 3) & 1) and not dxt
    bsz = (1 if linear else 4) if border else 0
    pitch = h['pitch']
    if linear:
        packed = ((w + bw - 1) // bw + 2 * bsz) * bpb
        if pitch == 0: pitch = packed
        elif pitch < packed: w = max(pitch // bpb * bw, 1)
    mips = max(reg >> 16, 1)
    fl = lambda v: v.bit_length() - 1 if v else 0
    max_mips = fl(max(min(w // 4, hh // 4), 1)) + 1 if dxt else fl(max(w, hh)) + 1
    mips = min(mips, max_mips)
    np2 = lambda v: 1 << (v - 1).bit_length() if v > 1 else 1
    lv, off, mw, mh = [], 0, w, hh
    for _ in range(mips):
        wb, hb = (mw + bw - 1) // bw, (mh + bw - 1) // bw
        if linear: p, fh = pitch, hb + 2 * bsz
        elif not border: p, fh = wb * bpb, hb
        else: p, fh = np2(wb + 2 * bsz) * bpb, np2(hb + 2 * bsz)
        lv.append((off, mw, mh, wb, hb, p, fh, bsz))
        off += p * fh
        mw, mh = max(mw // 2, 1), max(mh // 2, 1)
    return fmt, linear, lv, off

def ztables(w, h):
    lw, lh = (w - 1).bit_length() if w > 1 else 0, (h - 1).bit_length() if h > 1 else 0
    px, py, s = [], [], 0
    while lw or lh:
        if lw: px.append(s); s += 1; lw -= 1
        if lh: py.append(s); s += 1; lh -= 1
    tx = [sum(((i >> b) & 1) << p for b, p in enumerate(px)) for i in range(w)]
    ty = [sum(((i >> b) & 1) << p for b, p in enumerate(py)) for i in range(h)]
    return tx, ty

def rgb565(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

def dxt_colors(b, o, bc1):
    c0, c1 = struct.unpack_from('<HH', b, o)
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1 or not bc1:
        pal = [p0 + (255,), p1 + (255,), tuple((2 * a + c) // 3 for a, c in zip(p0, p1)) + (255,),
               tuple((a + 2 * c) // 3 for a, c in zip(p0, p1)) + (255,)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((a + c) // 2 for a, c in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    bits = struct.unpack_from('<I', b, o + 4)[0]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]

def decode_texture(data, h):
    """Top mip level as RGBA bytes in the channel order shaders see (the same as the recompiler's
    texture dumps), or None for formats not handled here."""
    L = tex_layout(h)
    if not L: return None
    fmt, linear, levels, total = L
    off, w, hh, wb, hb, pitch, fh, bsz = levels[0]
    src = data[h['offset']:h['offset'] + total]
    out = bytearray(w * hh * 4)
    if fmt in (TEX_DXT1, TEX_DXT23, TEX_DXT45):
        bs = 8 if fmt == TEX_DXT1 else 16
        for by in range(hb):
            for bx in range(wb):
                o = by * pitch + bx * bs
                if o + bs > len(src): break
                if fmt == TEX_DXT1: px = dxt_colors(src, o, True)
                else:
                    px = [list(c) for c in dxt_colors(src, o + 8, False)]
                    if fmt == TEX_DXT23:
                        a64 = struct.unpack_from('<Q', src, o)[0]
                        for i in range(16): px[i][3] = ((a64 >> (4 * i)) & 15) * 17
                    else:
                        a0, a1 = src[o], src[o + 1]
                        al = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else
                                         [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
                        bits = int.from_bytes(src[o + 2:o + 8], 'little')
                        for i in range(16): px[i][3] = al[(bits >> (3 * i)) & 7]
                for i in range(16):
                    x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                    if x < w and y < hh: out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(px[i])
    else:
        bpb = BLOCK[fmt][0]
        if linear:
            get = lambda x, y: (y + bsz) * pitch + (x + bsz) * bpb
        else:
            pw = pitch // bpb
            tx, ty = ztables(pw, fh)
            get = lambda x, y: (ty[y + bsz] | tx[x + bsz]) * bpb
        for y in range(hh):
            for x in range(w):
                o = get(x, y)
                if o + bpb > len(src): continue
                if bpb == 4: a, r, g, b = src[o:o + 4]
                elif bpb == 1: a = r = g = b = src[o]
                else:
                    v = struct.unpack_from('>H', src, o)[0]
                    if fmt == TEX_R5G6B5: r, g, b = rgb565(v); a = 255
                    elif fmt == TEX_A1R5G5B5: a = 255 if v >> 15 else 0; r, g, b = ((v >> 10) & 31) * 8, ((v >> 5) & 31) * 8, (v & 31) * 8
                    elif fmt == TEX_A4R4G4B4: a, r, g, b = ((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17
                    else: r, g, b, a = v >> 8, v & 255, 0, 255
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, a))
    # channel mapping: base ARGB mapping of the format, then the game's remap (CONTROL1)
    base = BASE_ARGB.get(fmt, 'ARGB')
    remap = h['remap']
    sel = []
    for ch in range(4):  # A, R, G, B
        s = (remap >> (2 * ch)) & 3; ctl = (remap >> (8 + 2 * ch)) & 3
        sel.append(None if ctl < 2 else base[s])
    # channels the game forces to 0 or 1 keep their stored data here (the renderer applies the
    # constant to a replacement too, see tex_pack), so one PNG serves every header sharing the data
    mapping = tuple(m if m not in (None, '0', '1') else d for m, d in zip((sel[1], sel[2], sel[3], sel[0]), 'RGBA'))
    if mapping != ('R', 'G', 'B', 'A'):
        pick = {'R': 0, 'G': 1, 'B': 2, 'A': 3}
        for i in range(0, len(out), 4):
            p = out[i:i + 4]
            out[i:i + 4] = bytes(0 if m == '0' else 255 if m == '1' else p[pick[m]] for m in mapping)
    return w, hh, out

def texture_hash(data, h):
    L = tex_layout(h)
    return xxh64(bytes(data[h['offset']:h['offset'] + L[3]])) if L else None

def export_texture(lv, ti, outdir, cache):
    if ti in cache: return cache[ti]
    h = lv.texture_header(ti)
    dec = decode_texture(lv.tex, h)
    name = None
    if dec:
        name = '%016x_%dx%d.png' % (texture_hash(lv.tex, h), dec[0], dec[1])
        write_png(os.path.join(outdir, name), dec[0], dec[1], dec[2])
    cache[ti] = name
    return name

# ---------------------------------------------------------------------------------------------
# commands

def cmd_info(lv, args):
    print('%s: %d moby classes' % (lv.name, lv.class_count()))
    for ci in range(lv.class_count()):
        c = lv.cls(ci)
        meshes = sum(len(g) for g in c['groups'])
        if not meshes: continue
        verts = sum(m['nv'] for g in c['groups'] for m in g)
        print('class %3d: %3d bones, %2d groups, %3d meshes, %6d vertices, radius %.2f%s' % (
            ci, c['bones'], len(c['groups']), meshes, verts, c['sphere'][3], '  (skinned)' if c['vin_main'] else ''))
        if args.verbose:
            for gi, g in enumerate(c['groups']):
                for mi, me in enumerate(g):
                    print('    g%d_m%d: %5d verts %6d indices material %d textures %s bones %d' % (
                        gi, mi, me['nv'], me['nidx'], me['material'], lv.material_textures(me['material']), me['palette_n']))

def cmd_textures(lv, args):
    os.makedirs(args.out, exist_ok=True)
    cache = {}
    if args.cls is not None:
        c = lv.cls(args.cls)
        tis = sorted({t for g in c['groups'] for m in g for t in lv.material_textures(m['material'])})
    else:
        tis = range(lv.sec(0x5200)[1])
    for ti in tis:
        n = export_texture(lv, ti, args.out, cache)
        print('texture %4d -> %s' % (ti, n or '(format not handled)'))

def cmd_export(lv, args):
    c = lv.cls(args.cls)
    if not any(c['groups']): raise SystemExit('class %d has no meshes' % args.cls)
    os.makedirs(args.out, exist_ok=True)
    stem = '%s_class%d' % (lv.name, args.cls)
    want = set(int(g) for g in args.groups.split(',')) if args.groups else None
    buf = bytearray()
    gl = dict(asset=dict(version='2.0', generator='tod_model.py (TOD recomp)'), scene=0, scenes=[dict(nodes=[])],
              nodes=[], meshes=[], accessors=[], bufferViews=[], buffers=[], materials=[], textures=[], images=[],
              samplers=[dict(magFilter=9729, minFilter=9987)], skins=[])

    def view(data, target=None):
        while len(buf) % 4: buf.append(0)
        bv = dict(buffer=0, byteOffset=len(buf), byteLength=len(data))
        if target: bv['target'] = target
        buf.extend(data)
        gl['bufferViews'].append(bv)
        return len(gl['bufferViews']) - 1

    def accessor(fmt_char, comps, values, type_, ctype, target=None, minmax=False, normalized=False):
        flat = [x for v in values for x in (v if isinstance(v, (tuple, list)) else (v,))]
        data = struct.pack('<%d%s' % (len(flat), fmt_char), *flat)
        a = dict(bufferView=view(data, target), componentType=ctype, count=len(values), type=type_)
        if normalized: a['normalized'] = True
        if minmax:
            a['min'] = [min(v[k] for v in values) for k in range(comps)]
            a['max'] = [max(v[k] for v in values) for k in range(comps)]
        gl['accessors'].append(a)
        return len(gl['accessors']) - 1

    # skeleton: one node per bone, named bone_NNN (the importer maps joints back by these names)
    root = None
    joints = []
    if c['bones']:
        parents, W, I = lv.skeleton(c)
        def clean(m):  # affine: last column exactly 0 0 0 1
            m = [round(x, 7) for x in m]
            m[3] = m[7] = m[11] = 0.0; m[15] = 1.0
            return m
        identity = [1.0 if r == c else 0.0 for r in range(4) for c in range(4)]
        first = len(gl['nodes'])
        for b in range(c['bones']):
            p = parents[b]
            local = clean(W[b] if p < 0 or p >= b else mat_mul(W[b], mat_inv(W[p])))
            node = dict(name='bone_%03d' % b)
            if any(abs(x - y) > 1e-6 for x, y in zip(local, identity)): node['matrix'] = local
            gl['nodes'].append(node)
        for b in range(c['bones']):
            p = parents[b]
            if 0 <= p < b: gl['nodes'][first + p].setdefault('children', []).append(first + b)
        roots = [first + b for b in range(c['bones']) if not (0 <= parents[b] < b)]
        root = len(gl['nodes'])
        gl['nodes'].append(dict(name='skeleton', children=roots))
        joints = list(range(first, first + c['bones']))
        # inverse bind matrices from the bind pose (the file's own inverse table matches it)
        ibm = accessor('f', 16, [tuple(clean(mat_inv(clean(m)))) for m in W], 'MAT4', 5126)
        gl['skins'].append(dict(name='skeleton', joints=joints, inverseBindMatrices=ibm, skeleton=root))
        gl['scenes'][0]['nodes'].append(root)

    tex_cache, mat_index, img_index = {}, {}, {}
    def material(mi):
        if mi in mat_index: return mat_index[mi]
        mat = dict(name='material_%d' % mi, extras=dict(tod_material=mi))
        tis = lv.material_textures(mi)
        names = [export_texture(lv, ti, args.out, tex_cache) for ti in tis]
        mat['extras']['tod_textures'] = tis
        mat['extras']['texture_files'] = names
        if names and names[0]:
            if names[0] not in img_index:
                gl['images'].append(dict(uri=names[0]))
                gl['textures'].append(dict(source=len(gl['images']) - 1, sampler=0))
                img_index[names[0]] = len(gl['textures']) - 1
            mat['pbrMetallicRoughness'] = dict(baseColorTexture=dict(index=img_index[names[0]]), metallicFactor=0.0)
        gl['materials'].append(mat)
        mat_index[mi] = len(gl['materials']) - 1
        return mat_index[mi]

    for gi, g in enumerate(c['groups']):
        if want is not None and gi not in want: continue
        # mesh nodes sit at the scene root (glTF ignores parent transforms of skinned meshes)
        for mi, me in enumerate(g):
            P, N, UV, J, Wt, S, tris = lv.read_mesh(c, me)
            if not tris: continue
            name = 'g%d_m%d' % (gi, mi)
            attrs = dict(POSITION=accessor('f', 3, P, 'VEC3', 5126, 34962, minmax=True),
                         NORMAL=accessor('f', 3, N, 'VEC3', 5126, 34962),
                         TEXCOORD_0=accessor('f', 2, UV, 'VEC2', 5126, 34962),
                         TANGENT=accessor('f', 4, lv.last_tangents, 'VEC4', 5126, 34962))
            if joints:
                attrs['JOINTS_0'] = accessor('H', 4, [tuple(j if w > 0 else 0 for j, w in zip(jj, ww)) for jj, ww in zip(J, Wt)],
                                             'VEC4', 5123, 34962)
                # weights as floats that sum to 1 exactly (glTF validators check)
                Wn = []
                for w in Wt:
                    s = sum(w) or 1.0
                    Wn.append(tuple(x / s for x in w))
                attrs['WEIGHTS_0'] = accessor('f', 4, Wn, 'VEC4', 5126, 34962)
            ind = accessor('H', 1, [i for t in tris for i in t], 'SCALAR', 5123, 34963)
            gl['meshes'].append(dict(name=name, primitives=[dict(attributes=attrs, indices=ind, material=material(me['material']))],
                                     extras=dict(tod_mesh_addr=me['addr'], vertices=me['nv'], skinned=me['skinned'])))
            node = dict(name=name, mesh=len(gl['meshes']) - 1)
            if joints: node['skin'] = 0
            gl['nodes'].append(node)
            gl['scenes'][0]['nodes'].append(len(gl['nodes']) - 1)
    gl['extras'] = dict(level=lv.name, tod_class=args.cls, position_scale=c['scale'],
                        note='Mesh nodes are named gG_mM (mesh group G, mesh M). Keep those names and the bone_NNN joints '
                             'when editing; tod_model.py import writes the edited meshes back.')
    for k in ('materials', 'textures', 'images', 'skins'):
        if not gl[k]: del gl[k]
    if 'textures' not in gl: del gl['samplers']
    with open(os.path.join(args.out, stem + '.bin'), 'wb') as f: f.write(buf)
    gl['buffers'] = [dict(uri=stem + '.bin', byteLength=len(buf))]
    with open(os.path.join(args.out, stem + '.gltf'), 'w') as f: json.dump(gl, f, indent=1)
    print('wrote %s (%d meshes, %d bones, %d textures)' % (os.path.join(args.out, stem + '.gltf'), len(gl['meshes']),
                                                            c['bones'], len(gl.get('images', []))))

# ---- glTF reading (for import)

def load_gltf(path):
    data = open(path, 'rb').read()
    bins = []
    if data[:4] == b'glTF':
        n = struct.unpack_from('<I', data, 12)[0]
        gl = json.loads(data[20:20 + n])
        p = 20 + n
        if p < len(data): bins.append(data[p + 8:p + 8 + struct.unpack_from('<I', data, p)[0]])
    else:
        gl = json.loads(data)
        for b in gl.get('buffers', []):
            uri = b.get('uri', '')
            if uri.startswith('data:'): bins.append(base64.b64decode(uri.split(',', 1)[1]))
            else: bins.append(open(os.path.join(os.path.dirname(path), uri), 'rb').read())
    return gl, bins

COMP = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}

def read_accessor(gl, bins, ai):
    a = gl['accessors'][ai]; bv = gl['bufferViews'][a['bufferView']]
    ch, sz = COMP[a['componentType']]; n = NCOMP[a['type']]
    stride = bv.get('byteStride') or sz * n
    base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    b = bins[bv['buffer']]
    out = []
    for i in range(a['count']):
        v = struct.unpack_from('<%d%s' % (n, ch), b, base + i * stride)
        if a.get('normalized'):
            m = {'b': 127.0, 'B': 255.0, 'h': 32767.0, 'H': 65535.0}.get(ch)
            if m: v = tuple(x / m for x in v)
        out.append(v)
    return out

def node_world(gl, ni, parents):
    def local(n):
        if 'matrix' in n:
            m = n['matrix']  # column-major = row-major with row vectors
            return list(m)
        t = n.get('translation', [0, 0, 0]); r = n.get('rotation', [0, 0, 0, 1]); s = n.get('scale', [1, 1, 1])
        x, y, z, w = r
        R = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
             2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
             2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]
        return [R[0] * s[0], R[1] * s[0], R[2] * s[0], 0, R[3] * s[1], R[4] * s[1], R[5] * s[1], 0,
                R[6] * s[2], R[7] * s[2], R[8] * s[2], 0, t[0], t[1], t[2], 1]
    m = local(gl['nodes'][ni])
    p = parents.get(ni)
    while p is not None:
        m = mat_mul(m, local(gl['nodes'][p])); p = parents.get(p)
    return m

def xform(m, v, w=1.0):
    return tuple(v[0] * m[0 + k] + v[1] * m[4 + k] + v[2] * m[8 + k] + w * m[12 + k] for k in range(3))

def gltf_meshes(path):
    """{name gG_mM: (positions, normals, uvs, [(bone ids)], [(weights)], triangles)} in the model's space"""
    gl, bins = load_gltf(path)
    parents = {}
    for i, n in enumerate(gl['nodes']):
        for ch in n.get('children', []): parents[ch] = i
    out = {}
    for ni, n in enumerate(gl['nodes']):
        if 'mesh' not in n: continue
        mesh = gl['meshes'][n['mesh']]
        name = next((s for s in (n.get('name', ''), mesh.get('name', '')) if s.startswith('g') and '_m' in s), None)
        if not name: continue
        name = name.split('.')[0]  # Blender may add .001
        joints = None
        if 'skin' in n:
            joints = [gl['nodes'][j].get('name', '') for j in gl['skins'][n['skin']]['joints']]
            # skinned meshes ignore the node's own transform in glTF
            M = [1.0 if r == c else 0.0 for r in range(4) for c in range(4)]
        else:
            M = node_world(gl, ni, parents)
        P, N, UV, J, W, T, TG = [], [], [], [], [], [], []
        for prim in mesh['primitives']:
            if prim.get('mode', 4) != 4: raise SystemExit('%s: only triangle meshes can be imported' % name)
            at = prim['attributes']; base = len(P)
            pos = read_accessor(gl, bins, at['POSITION'])
            P += [xform(M, p) for p in pos]
            N += [norm(xform(M, v, 0.0)) for v in read_accessor(gl, bins, at['NORMAL'])] if 'NORMAL' in at else [(0, 1, 0)] * len(pos)
            UV += read_accessor(gl, bins, at['TEXCOORD_0']) if 'TEXCOORD_0' in at else [(0.0, 0.0)] * len(pos)
            TG += [norm(xform(M, t[:3], 0.0)) + (t[3],) for t in read_accessor(gl, bins, at['TANGENT'])] if 'TANGENT' in at else [None] * len(pos)
            if 'JOINTS_0' in at and joints:
                for jj in read_accessor(gl, bins, at['JOINTS_0']):
                    J.append(tuple(int(joints[j][5:]) if joints[j].startswith('bone_') else 0 for j in jj))
                W += read_accessor(gl, bins, at['WEIGHTS_0'])
            else:
                J += [(0, 0, 0, 0)] * len(pos); W += [(1.0, 0, 0, 0)] * len(pos)
            idx = [i[0] for i in read_accessor(gl, bins, prim['indices'])] if 'indices' in prim else list(range(len(pos)))
            T += [(base + idx[k], base + idx[k + 1], base + idx[k + 2]) for k in range(0, len(idx) - 2, 3)]
        out[name] = (P, N, UV, J, W, T, TG if all(TG) else None)
    return out

# ---- import

def tangents(P, N, UV, T):
    acc = [[0.0, 0.0, 0.0] for _ in P]; bacc = [[0.0, 0.0, 0.0] for _ in P]
    for a, b, c in T:
        e1 = [P[b][k] - P[a][k] for k in range(3)]; e2 = [P[c][k] - P[a][k] for k in range(3)]
        du1, dv1 = UV[b][0] - UV[a][0], UV[b][1] - UV[a][1]
        du2, dv2 = UV[c][0] - UV[a][0], UV[c][1] - UV[a][1]
        r = du1 * dv2 - du2 * dv1
        if abs(r) < 1e-12: continue
        r = 1.0 / r
        t = [(e1[k] * dv2 - e2[k] * dv1) * r for k in range(3)]
        bt = [(e2[k] * du1 - e1[k] * du2) * r for k in range(3)]
        for v in (a, b, c):
            for k in range(3): acc[v][k] += t[k]; bacc[v][k] += bt[k]
    out = []
    for i, n in enumerate(N):
        t = acc[i]; d = sum(t[k] * n[k] for k in range(3))
        t = norm([t[k] - n[k] * d for k in range(3)]) if any(t) else norm((n[1], -n[0], 0.0) if abs(n[2]) < 0.9 else (0.0, n[2], -n[1]))
        cr = (n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0])
        sign = -1 if sum(cr[k] * bacc[i][k] for k in range(3)) < 0 else 1
        out.append((t, sign))
    return out

def palette_capacity(lv, c, me):
    """u16 slots the mesh's bone palette can grow to without overwriting another palette"""
    ptrs = sorted({m['palette_ptr'] for g in c['groups'] for m in g if m['palette_ptr']})
    later = [p for p in ptrs if p > me['palette_ptr']]
    cap = ((later[0] - me['palette_ptr']) // 2) if later else ((me['palette_n'] + 7) // 8) * 8
    return min(cap, 255)

def build_vertices(lv, c, me, P, N, UV, J, W, T, TG, warn):
    sc = c['scale']
    # palette: bone ids this mesh uses, heaviest first so trimming drops the least used
    use = {}
    for jj, ww in zip(J, W):
        for j, w in zip(jj, ww):
            if w > 0: use[j] = use.get(j, 0) + w
    if not me['skinned']:
        palette = [me['palette'][0]]
    else:
        cap = palette_capacity(lv, c, me)
        if set(use) <= set(me['palette']) and len(me['palette']) <= cap:
            palette = list(me['palette'])  # same bones as before: keep the game's order
        else:
            palette = sorted(use, key=lambda j: -use[j])
            if len(palette) > cap:
                warn('%d bones used but the palette holds %d: the lightest bones are left out' % (len(palette), cap))
                palette = palette[:cap]
            palette.sort()
    slot = {b: i for i, b in enumerate(palette)}
    # tangents from the model when it has them (Blender exports them with "Tangents" ticked)
    tan = [((t[0], t[1], t[2]), -1 if t[3] < 0 else 1) for t in TG] if TG else tangents(P, N, UV, T)
    out = bytearray()
    for i in range(len(P)):
        q = [int(round(P[i][k] / sc)) for k in range(3)]
        if any(v < -32768 or v > 32767 for v in q):
            raise SystemExit('a vertex at %s is outside what the class can store (|coordinate| <= %.2f); scale the model down'
                             % (tuple(round(x, 2) for x in P[i]), 32767 * sc))
        t, sign = tan[i]
        out += struct.pack('>4h', q[0], q[1], q[2], -sign)  # the game stores the opposite sign
        if me['skinned']:
            pairs = sorted(((w, j) for j, w in zip(J[i], W[i]) if w > 0 and j in slot), reverse=True)[:4]
            if not pairs: pairs = [(1.0, palette[0])]
            tot = sum(w for w, _ in pairs)
            ws = [int(round(w / tot * 255)) for w, _ in pairs]
            ws[0] += 255 - sum(ws)
            bones = [slot[j] for _, j in pairs] + [0] * (4 - len(pairs))
            ws += [0] * (4 - len(ws))
            out += bytes(bones) + bytes(ws)
        out += struct.pack('>2H', to_half(UV[i][0]), to_half(UV[i][1]))
        out += struct.pack('>II', pack_cmp(*N[i]), pack_cmp(*t))
    return out, palette

def write_psarc(src_path, dst_path, replace):
    """Copy a PSARC, replacing entries by name ({name: bytes}). Blocks are zlib-compressed."""
    pa = Psarc(src_path)
    f = open(src_path, 'rb'); hdr = f.read(32)
    magic, ver, comp, toclen, esz, n, bs, flags = struct.unpack('>4sI4sIIIII', hdr)
    old_toc = f.read(toclen - 32)
    inv = {i: nm for nm, i in pa.files.items()}
    entries = []
    zsizes = []
    body = bytearray()
    for i in range(n):
        data = replace[inv[i]] if inv.get(i) in replace else pa.read(i)
        first = len(zsizes)
        off = len(body)
        for p in range(0, len(data), bs):
            blk = data[p:p + bs]
            z = zlib.compress(blk, 6)
            if len(z) >= len(blk): z = blk  # stored raw
            zsizes.append(0 if len(z) == bs else len(z))
            body += z
        entries.append((old_toc[i * esz:i * esz + 16], first, len(data), off))
    nb = {65536: 2, 16777216: 3}.get(bs, 4)
    toc_len = 32 + n * esz + len(zsizes) * nb
    out = bytearray(struct.pack('>4sI4sIIIII', magic, ver, comp, toc_len, esz, n, bs, flags))
    for md5, first, ln, off in entries:
        out += md5 + struct.pack('>I', first) + ln.to_bytes(5, 'big') + (off + toc_len).to_bytes(5, 'big')
    for z in zsizes: out += z.to_bytes(nb, 'big')
    out += body
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    tmp = dst_path + '.tmp'
    with open(tmp, 'wb') as g: g.write(out)
    os.replace(tmp, dst_path)

def cmd_import(lv0, args):
    rel = os.path.join('USRDIR', 'packed', 'levels', lv0.name, 'level_cached.psarc')
    dst = os.path.join(args.data, 'mods', rel)
    # stack edits: start from an earlier mod of this level if there is one
    lv = Level(lv0.folder, dst) if os.path.exists(dst) and not args.fresh else lv0
    c = lv.cls(args.cls)
    meshes = gltf_meshes(args.model)
    if not meshes: raise SystemExit('no meshes named gG_mM in %s (export a class first and keep the names)' % args.model)
    vert, tex, main = lv.vert, lv.tex, lv.main
    for name in sorted(meshes):
        try:
            gi, mi = (int(x) for x in name[1:].split('_m'))
            me = c['groups'][gi][mi]
        except (ValueError, IndexError):
            print('skipping %s: no such mesh in class %d' % (name, args.cls)); continue
        P, N, UV, J, W, T, TG = meshes[name]
        if len(P) > 65535: raise SystemExit('%s: %d vertices, at most 65535 per mesh' % (name, len(P)))
        msgs = []
        vb, palette = build_vertices(lv, c, me, P, N, UV, J, W, T, TG, msgs.append)
        ib = b''.join(struct.pack('>3H', *t) for t in T)
        # written in place: data appended past the original end of vertices.dat/textures.dat is not
        # loaded by the game (it reads a fixed amount per level), so a mesh must fit its old space.
        # The index space includes the mesh's lower-detail lists when it has them.
        vcap = me['nv'] * me['stride']
        icap = me['nidx'] * 2 if not me['lods'][0] else max(me['nidx'], max((x >> 16) + (x & 0xffff) for x in me['lods'] if x)) * 2
        if len(vb) > vcap or len(ib) > icap:
            raise SystemExit('%s: %d vertices and %d triangles, but this mesh has room for at most %d vertices and %d triangles.\n'
                             'Decimate it in Blender (Decimate modifier, applied). Each mesh has its own fixed space; see docs/model_formats.md.'
                             % (name, len(P), len(T), vcap // me['stride'], icap // 6))
        vpos = c['vbase'] + me['vbyte']
        target = vert if c['vin_main'] else tex
        target[vpos:vpos + len(vb)] = vb
        ipos = c['ibase'] + me['istart'] * 2
        tex[ipos:ipos + len(ib)] = ib
        a = me['addr']
        struct.pack_into('>I', main, a + 0x00, (ipos - c['ibase']) // 2)
        struct.pack_into('>I', main, a + 0x04, vpos - c['vbase'])
        struct.pack_into('>I', main, a + 0x08, (me['material'] << 16) | len(P))
        struct.pack_into('>I', main, a + 0x0c, (me['flags'] & 0x00ffffff) | (len(palette) << 24 if me['skinned'] else me['flags'] & 0xff000000))
        struct.pack_into('>I', main, a + 0x10, len(T) * 3)
        if me['lods'][0]:  # lower detail index lists: all use the new list
            for k in range(3): struct.pack_into('>I', main, a + 0x14 + 4 * k, len(T) * 3)
        if me['skinned'] and me['palette_ptr']:
            for k, b in enumerate(palette): struct.pack_into('>H', main, me['palette_ptr'] + 2 * k, b)
        print('%s: %d vertices, %d triangles (room for %d and %d)%s' % (name, len(P), len(T), vcap // me['stride'], icap // 6,
                                                                          ''.join('; ' + m for m in msgs)))
    replace = {}
    for fn, data in (('main.dat', main), ('vertices.dat', vert), ('textures.dat', tex)):
        key = next(k for k in lv0.pa.files if k.endswith('/' + fn))
        replace[key] = bytes(data)
    write_psarc(lv0.archive, dst, replace)
    print('wrote %s\nThe game uses it from the next load of %s; delete it to undo.' % (dst, lv0.name))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info'); p.add_argument('level'); p.add_argument('-v', '--verbose', action='store_true')
    p = sub.add_parser('export'); p.add_argument('level'); p.add_argument('cls', type=int); p.add_argument('out')
    p.add_argument('--groups', help='only these mesh groups, e.g. 0,1,2')
    p = sub.add_parser('textures'); p.add_argument('level'); p.add_argument('out'); p.add_argument('cls', type=int, nargs='?')
    p = sub.add_parser('import'); p.add_argument('level'); p.add_argument('cls', type=int); p.add_argument('model'); p.add_argument('data')
    p.add_argument('--fresh', action='store_true', help='start from the game\'s archive, not an earlier mod')
    args = ap.parse_args()
    lv = Level(args.level)
    dict(info=cmd_info, export=cmd_export, textures=cmd_textures, **{'import': cmd_import})[args.cmd](lv, args)

if __name__ == '__main__':
    main()
