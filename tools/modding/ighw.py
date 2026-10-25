#!/usr/bin/env python3
"""Insomniac IGHW container (ToD .dat files): header, section table, pointer fixups."""
import struct, sys

class Ighw:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        assert d[:4] == b'IGHW'
        self.ver = struct.unpack('>HH', d[4:8])
        self.nsec, self.hdrlen, self.size, self.nfix = struct.unpack('>IIII', d[8:24])
        self.sections = {}
        self.order = []
        for i in range(self.nsec):
            sid, off, cnt, sz = struct.unpack('>IIII', d[0x20 + i * 16:0x30 + i * 16])
            arr = bool(cnt & 0x10000000)
            n = cnt & 0x0FFFFFFF
            total = n * sz if arr else sz
            self.sections[sid] = (off, n, sz, arr, total)
            self.order.append(sid)
        fo = 0x20 + self.nsec * 16
        self.fixups = struct.unpack('>%dI' % self.nfix, d[fo:fo + 4 * self.nfix])

    def sec(self, sid):
        off, n, sz, arr, total = self.sections[sid]
        return self.d[off:off + total]

    def u32(self, o): return struct.unpack('>I', self.d[o:o + 4])[0]
    def cstr(self, o):
        e = self.d.index(b'\0', o)
        return self.d[o:e].decode(errors='replace')

if __name__ == '__main__':
    f = Ighw(sys.argv[1])
    print('version', f.ver, 'sections', f.nsec, 'hdr', hex(f.hdrlen), 'size', f.size, 'fixups', f.nfix)
    for sid in f.order:
        off, n, sz, arr, total = f.sections[sid]
        print('%06x  off %08x  %s  total %d' % (sid, off, ('%d x %d' % (n, sz)) if arr else ('blob %d' % sz), total))
