#!/usr/bin/env python3
"""Moby (character/object) classes in a ToD level's main.dat: geometry tables."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ighw import Ighw

class Level:
    def __init__(self, folder):
        self.main = Ighw(os.path.join(folder, 'main.dat'))
        self.vert = open(os.path.join(folder, 'vertices.dat'), 'rb').read()
        self.tex0 = self.main.sections[0x5200][0]
        self.sh0 = self.main.sections[0x5000][0]

    def w(self, o, n=1):
        v = struct.unpack('>%dI' % n, self.main.d[o:o + 4 * n])
        return v if n > 1 else v[0]

    def classes(self):
        off, n, sz, _, _ = self.main.sections[0xd100]
        for i in range(n):
            yield i, off + i * sz

    def cls(self, i):
        off, n, sz, _, _ = self.main.sections[0xd100]
        b = off + i * sz
        W = self.w(b, 48)
        d = dict(index=i, addr=b, ibase=0x80 + W[0x34 // 4], vbase=0x80 + (W[0x38 // 4] & 0x7fffffff),
                 scale=struct.unpack('>f', self.main.d[b + 0x3c:b + 0x40])[0], skel=W[8], groups=[])
        d['bones'] = struct.unpack('>H', self.main.d[W[8]:W[8] + 2])[0] if W[8] else 0
        lod = W[10]
        for g in range(4 if lod else 0):
            mp, mn = self.w(lod + g * 16, 2)
            meshes = []
            if not (0 < mp and mn < 256 and mp + mn * 64 <= len(self.main.d)): mn = 0
            for m in range(mn):
                M = self.w(mp + m * 64, 16)
                meshes.append(dict(addr=mp + m * 64, istart=M[0], vbyte=M[1], shader=M[2] >> 16, nv=M[2] & 0xffff,
                                   flags=M[3], nidx=M[4], raw=M))
            d['groups'].append(meshes)
        return d

if __name__ == '__main__':
    L = Level(sys.argv[1])
    for i, _ in L.classes():
        c = L.cls(i)
        nm = sum(len(g) for g in c['groups'])
        tot = sum(m['nidx'] for g in c['groups'] for m in g)
        print('%3d bones %3d meshes %2d ibase %08x..%08x vbase %08x' % (i, c['bones'], nm, c['ibase'], c['ibase'] + tot * 2, c['vbase']))
