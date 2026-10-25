#!/usr/bin/env python3
"""Write a Bink 1 (BIKi) video from PNG frames, for skin pack menu movies.

  bink_encode.py <out.bik> <fps> <frame.png>...

A minimal encoder: every 8x8 block is either stored raw or skipped when it is the same as in the
previous frame, so a clip on a still background stays small. No audio track. Needs Pillow; the
frames are resized to the first frame's size (rounded down to a multiple of 16).
"""
import struct, sys
from PIL import Image

class Bits:
    """little-endian bit writer (Bink reads bits from the low end of each 32-bit word)"""
    def __init__(self): self.out = bytearray(); self.acc = 0; self.n = 0
    def put(self, v, n):
        self.acc |= (v & ((1 << n) - 1)) << self.n; self.n += n
        while self.n >= 8:
            self.out.append(self.acc & 0xFF); self.acc >>= 8; self.n -= 8
    def align32(self):
        if self.n: self.put(0, 8 - self.n)
        while len(self.out) % 4: self.out.append(0)
    def bytes(self):
        self.align32(); return bytes(self.out)

def bits_for(n):  # Bink's bundle length field: av_log2(n + 511) + 1
    return (n + 511).bit_length()

SKIP, RAW = 0, 9
def encode_plane(bw, bh, pix, stride, prev, out):
    """pix/prev: bytes of one plane (stride wide), out: Bits"""
    width = bw * 8
    blen = bits_for(width >> 3); slen = bits_for(width >> 4); clen = bits_for(bw * 64)
    plen = bits_for(bw << 3); rlen = bits_for(bw * 48)
    rows = []   # per block row: block types, raw colours
    for by in range(bh):
        types, colors = [], bytearray()
        for bx in range(bw):
            blk = [pix[(by * 8 + j) * stride + bx * 8:(by * 8 + j) * stride + bx * 8 + 8] for j in range(8)]
            # every row keeps at least one raw block: its colours are sent row by row, and a row
            # asking for none would end the colour bundle for the rest of the plane
            if bx and prev is not None and all(blk[j] == prev[(by * 8 + j) * stride + bx * 8:(by * 8 + j) * stride + bx * 8 + 8] for j in range(8)):
                types.append(SKIP)
            else:
                types.append(RAW); colors += b''.join(blk)
        rows.append((types, colors))
    # trees: every bundle uses tree 0 (plain 4-bit codes); colours also have 16 high-nibble trees
    for _ in range(2): out.put(0, 4)          # block types, sub block types
    for _ in range(17): out.put(0, 4)         # colours: 16 high-nibble trees + its own
    for _ in range(4): out.put(0, 4)          # pattern, x offset, y offset, run (DC bundles have none)
    # The decoder refills a bundle at the start of a row once everything it holds has been used.
    # RAD's decoder (unlike ffmpeg's) has room for about one row of values, so each row sends its own.
    for by, (types, colors) in enumerate(rows):
        out.put(len(types), blen)
        if len(set(types)) == 1:
            out.put(1, 1); out.put(types[0], 4)
        else:
            out.put(0, 1)
            for t in types: out.put(t, 4)
        if by == 0: out.put(0, slen)                  # no sub block types in this plane
        out.put(len(colors), clen)
        out.put(0, 1)
        for v in colors: out.put(v >> 4, 4); out.put(v & 15, 4)
        if by == 0:
            out.put(0, plen); out.put(0, blen); out.put(0, blen)    # pattern, x, y offsets
            out.put(0, blen); out.put(0, blen)                      # intra / inter DC
            out.put(0, rlen)                                        # runs
    out.align32()

def rgb_to_yuv(im):
    """BT.601, limited range (Bink 1 before 'k')"""
    y, cb, cr = im.convert('YCbCr').split()
    lim = lambda ch, lo, hi: ch.point(lambda v: lo + (v * (hi - lo) + 127) // 255)
    return lim(y, 16, 235), lim(cb, 16, 240), lim(cr, 16, 240)

def main():
    out_path, fps = sys.argv[1], float(sys.argv[2])
    files = sys.argv[3:]
    first = Image.open(files[0]).convert('RGB')
    w, h = first.width // 16 * 16, first.height // 16 * 16
    frames, prevs = [], [None, None, None]
    largest = 0
    for f in files:
        im = Image.open(f).convert('RGB')
        if im.size != (w, h): im = im.resize((w, h), Image.LANCZOS)
        y, cb, cr = rgb_to_yuv(im)
        cb = cb.resize((w // 2, h // 2), Image.BOX); cr = cr.resize((w // 2, h // 2), Image.BOX)
        bits = Bits()
        bits.put(0, 32)                            # version 'i': where the chroma planes start, filled in below
        planes = [(y.tobytes(), w, w // 8, h // 8), (cr.tobytes(), w // 2, w // 16, h // 16), (cb.tobytes(), w // 2, w // 16, h // 16)]
        for k, (data, stride, bw, bh) in enumerate(planes):
            encode_plane(bw, bh, data, stride, prevs[k], bits)
            prevs[k] = data
            if k == 0: chroma_at = len(bits.out)    # planes end on a 32-bit boundary
        pkt = bytearray(bits.bytes())
        # RAD's decoder takes this as the byte offset of the second plane (to decode the planes in
        # parallel); ffmpeg skips it. Seen in the game's own movies: always inside the packet, 4-aligned.
        pkt[0:4] = struct.pack('<I', chroma_at); pkt = bytes(pkt)
        frames.append(pkt); largest = max(largest, len(pkt))
    n = len(frames)
    num, den = int(round(fps * 100)), 100
    header_len = 44 + 4 * (n + 1)
    index, pos = [], header_len
    for i, f in enumerate(frames):
        index.append(pos | (1 if i == 0 else 0)); pos += len(f)
    index.append(pos)
    head = b'BIKi' + struct.pack('<IIIIIIIIII', pos - 8, n, largest, n, w, h, num, den, 0, 0)
    with open(out_path, 'wb') as o:
        o.write(head + struct.pack('<%dI' % (n + 1), *index) + b''.join(frames))
    print('%s: %dx%d, %d frames at %g fps, %d bytes' % (out_path, w, h, n, fps, pos))

if __name__ == '__main__':
    main()
