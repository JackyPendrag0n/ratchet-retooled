#!/usr/bin/env python3
"""List or extract files from a PS3 PSARC (zlib) archive.
  psarc.py list <archive>
  psarc.py extract <archive> <outdir> [substring...]"""
import struct, zlib, sys, os

class Psarc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(32)
        magic, ver, comp, toclen, esz, n, bs, flags = struct.unpack('>4sI4sIIIII', h)
        assert magic == b'PSAR', magic
        toc = self.f.read(toclen - 32)
        self.bs = bs
        self.ents = []
        for i in range(n):
            e = toc[i * esz:(i + 1) * esz]
            z = struct.unpack('>I', e[16:20])[0]
            ln = int.from_bytes(e[20:25], 'big'); off = int.from_bytes(e[25:30], 'big')
            self.ents.append((z, ln, off))
        nb = {65536: 2, 16777216: 3}.get(bs, 4)
        zs = toc[n * esz:]
        self.zsz = [int.from_bytes(zs[i:i + nb], 'big') for i in range(0, len(zs) - nb + 1, nb)]
        self.names = self.read(0).decode().split('\n')
        self.files = dict(zip(self.names, range(1, len(self.ents))))

    def read(self, i):
        z, ln, off = self.ents[i]
        out = bytearray(); self.f.seek(off); bi = z
        while len(out) < ln:
            s = self.zsz[bi] or self.bs
            d = self.f.read(s)
            if s != min(self.bs, ln - len(out)) or d[:2] in (b'\x78\xda', b'\x78\x9c', b'\x78\x01'):
                try: d = zlib.decompress(d)
                except zlib.error: pass
            out += d; bi += 1
        return bytes(out[:ln])

if __name__ == '__main__':
    p = Psarc(sys.argv[2])
    if sys.argv[1] == 'list':
        for name, i in p.files.items(): print(p.ents[i][1], name)
    else:
        out = sys.argv[3]; subs = sys.argv[4:]
        for name, i in p.files.items():
            if subs and not any(s in name for s in subs): continue
            dst = os.path.join(out, name.lstrip('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(p.read(i))
            print(dst)
