# SPU instruction set: decoding table shared by the disassembler and the recompiler generator.
# Each entry: name -> (format, opcode width in bits, opcode value)
# Formats: RR (rt ra rb), RI7 (rt ra i7), RI8 (rt ra i8), RI10 (rt ra i10), RI16 (rt i16),
# RI18 (rt i18), RRR (rt ra rb rc), plus special forms handled by name.
OPS = {
    # RRR, 4-bit
    'selb': ('RRR', 4, 0x8), 'shufb': ('RRR', 4, 0xB), 'mpya': ('RRR', 4, 0xC),
    'fnms': ('RRR', 4, 0xD), 'fma': ('RRR', 4, 0xE), 'fms': ('RRR', 4, 0xF),
    # RI18, 7-bit
    'ila': ('RI18', 7, 0x21), 'hbra': ('HBR18', 7, 0x08), 'hbrr': ('HBR18', 7, 0x09),
    # RI10, 8-bit
    'mpyi': ('RI10', 8, 0x74), 'mpyui': ('RI10', 8, 0x75),
    'cgti': ('RI10', 8, 0x4C), 'cgthi': ('RI10', 8, 0x4D), 'cgtbi': ('RI10', 8, 0x4E),
    'clgti': ('RI10', 8, 0x5C), 'clgthi': ('RI10', 8, 0x5D), 'clgtbi': ('RI10', 8, 0x5E),
    'ceqi': ('RI10', 8, 0x7C), 'ceqhi': ('RI10', 8, 0x7D), 'ceqbi': ('RI10', 8, 0x7E),
    'heqi': ('RI10', 8, 0x7F), 'hgti': ('RI10', 8, 0x4F), 'hlgti': ('RI10', 8, 0x5F),
    'andi': ('RI10', 8, 0x14), 'andhi': ('RI10', 8, 0x15), 'andbi': ('RI10', 8, 0x16),
    'ori': ('RI10', 8, 0x04), 'orhi': ('RI10', 8, 0x05), 'orbi': ('RI10', 8, 0x06),
    'xori': ('RI10', 8, 0x44), 'xorhi': ('RI10', 8, 0x45), 'xorbi': ('RI10', 8, 0x46),
    'ai': ('RI10', 8, 0x1C), 'ahi': ('RI10', 8, 0x1D), 'sfi': ('RI10', 8, 0x0C), 'sfhi': ('RI10', 8, 0x0D),
    'lqd': ('RI10', 8, 0x34), 'stqd': ('RI10', 8, 0x24),
    # RI16, 9-bit
    'br': ('RI16', 9, 0x064), 'bra': ('RI16', 9, 0x060), 'brsl': ('RI16', 9, 0x066), 'brasl': ('RI16', 9, 0x062),
    'brnz': ('RI16', 9, 0x042), 'brz': ('RI16', 9, 0x040), 'brhnz': ('RI16', 9, 0x046), 'brhz': ('RI16', 9, 0x044),
    'lqa': ('RI16', 9, 0x061), 'lqr': ('RI16', 9, 0x067), 'stqa': ('RI16', 9, 0x041), 'stqr': ('RI16', 9, 0x047),
    'ilh': ('RI16', 9, 0x083), 'ilhu': ('RI16', 9, 0x082), 'il': ('RI16', 9, 0x081), 'iohl': ('RI16', 9, 0x0C1),
    'fsmbi': ('RI16', 9, 0x065),
    # RI8, 10-bit
    'cflts': ('RI8', 10, 0x1D8), 'cfltu': ('RI8', 10, 0x1D9), 'csflt': ('RI8', 10, 0x1DA), 'cuflt': ('RI8', 10, 0x1DB),
    # RR / RI7, 11-bit
    'stop': ('STOP', 11, 0x000), 'lnop': ('NONE', 11, 0x001), 'sync': ('NONE', 11, 0x002), 'dsync': ('NONE', 11, 0x003),
    'mfspr': ('RR', 11, 0x00C), 'rdch': ('RR', 11, 0x00D), 'rchcnt': ('RR', 11, 0x00F),
    'sf': ('RR', 11, 0x040), 'or': ('RR', 11, 0x041), 'bg': ('RR', 11, 0x042), 'sfh': ('RR', 11, 0x048),
    'nor': ('RR', 11, 0x049), 'absdb': ('RR', 11, 0x053), 'rot': ('RR', 11, 0x058), 'rotm': ('RR', 11, 0x059),
    'rotma': ('RR', 11, 0x05A), 'shl': ('RR', 11, 0x05B), 'roth': ('RR', 11, 0x05C), 'rothm': ('RR', 11, 0x05D),
    'rotmah': ('RR', 11, 0x05E), 'shlh': ('RR', 11, 0x05F),
    'roti': ('RI7', 11, 0x078), 'rotmi': ('RI7', 11, 0x079), 'rotmai': ('RI7', 11, 0x07A), 'shli': ('RI7', 11, 0x07B),
    'rothi': ('RI7', 11, 0x07C), 'rothmi': ('RI7', 11, 0x07D), 'rotmahi': ('RI7', 11, 0x07E), 'shlhi': ('RI7', 11, 0x07F),
    'a': ('RR', 11, 0x0C0), 'and': ('RR', 11, 0x0C1), 'cg': ('RR', 11, 0x0C2), 'ah': ('RR', 11, 0x0C8),
    'nand': ('RR', 11, 0x0C9), 'avgb': ('RR', 11, 0x0D3), 'mtspr': ('RR', 11, 0x10C), 'wrch': ('RR', 11, 0x10D),
    'biz': ('RR', 11, 0x128), 'binz': ('RR', 11, 0x129), 'bihz': ('RR', 11, 0x12A), 'bihnz': ('RR', 11, 0x12B),
    'stopd': ('RR', 11, 0x140), 'stqx': ('RR', 11, 0x144), 'bi': ('RR', 11, 0x1A8), 'bisl': ('RR', 11, 0x1A9),
    'iret': ('RR', 11, 0x1AA), 'bisled': ('RR', 11, 0x1AB), 'hbr': ('RR', 11, 0x1AC),
    'gb': ('RR', 11, 0x1B0), 'gbh': ('RR', 11, 0x1B1), 'gbb': ('RR', 11, 0x1B2),
    'fsm': ('RR', 11, 0x1B4), 'fsmh': ('RR', 11, 0x1B5), 'fsmb': ('RR', 11, 0x1B6),
    'frest': ('RR', 11, 0x1B8), 'frsqest': ('RR', 11, 0x1B9), 'lqx': ('RR', 11, 0x1C4),
    'rotqbybi': ('RR', 11, 0x1CC), 'rotqmbybi': ('RR', 11, 0x1CD), 'shlqbybi': ('RR', 11, 0x1CF),
    'cbx': ('RR', 11, 0x1D4), 'chx': ('RR', 11, 0x1D5), 'cwx': ('RR', 11, 0x1D6), 'cdx': ('RR', 11, 0x1D7),
    'rotqbi': ('RR', 11, 0x1D8), 'rotqmbi': ('RR', 11, 0x1D9), 'shlqbi': ('RR', 11, 0x1DB),
    'rotqby': ('RR', 11, 0x1DC), 'rotqmby': ('RR', 11, 0x1DD), 'shlqby': ('RR', 11, 0x1DF),
    'orx': ('RR', 11, 0x1F0),
    'cbd': ('RI7', 11, 0x1F4), 'chd': ('RI7', 11, 0x1F5), 'cwd': ('RI7', 11, 0x1F6), 'cdd': ('RI7', 11, 0x1F7),
    'rotqbii': ('RI7', 11, 0x1F8), 'rotqmbii': ('RI7', 11, 0x1F9), 'shlqbii': ('RI7', 11, 0x1FB),
    'rotqbyi': ('RI7', 11, 0x1FC), 'rotqmbyi': ('RI7', 11, 0x1FD), 'shlqbyi': ('RI7', 11, 0x1FF),
    'nop': ('NONE', 11, 0x201),
    'cgt': ('RR', 11, 0x240), 'xor': ('RR', 11, 0x241), 'cgth': ('RR', 11, 0x248), 'eqv': ('RR', 11, 0x249),
    'cgtb': ('RR', 11, 0x250), 'sumb': ('RR', 11, 0x253), 'hgt': ('RR', 11, 0x258),
    'clz': ('RR', 11, 0x2A5), 'xswd': ('RR', 11, 0x2A6), 'xshw': ('RR', 11, 0x2AE), 'cntb': ('RR', 11, 0x2B4),
    'xsbh': ('RR', 11, 0x2B6), 'clgt': ('RR', 11, 0x2C0), 'andc': ('RR', 11, 0x2C1), 'fcgt': ('RR', 11, 0x2C2),
    'dfcgt': ('RR', 11, 0x2C3), 'fa': ('RR', 11, 0x2C4), 'fs': ('RR', 11, 0x2C5), 'fm': ('RR', 11, 0x2C6),
    'clgth': ('RR', 11, 0x2C8), 'orc': ('RR', 11, 0x2C9), 'fcmgt': ('RR', 11, 0x2CA), 'dfcmgt': ('RR', 11, 0x2CB),
    'dfa': ('RR', 11, 0x2CC), 'dfs': ('RR', 11, 0x2CD), 'dfm': ('RR', 11, 0x2CE), 'clgtb': ('RR', 11, 0x2D0),
    'hlgt': ('RR', 11, 0x2D8), 'dfma': ('RR', 11, 0x35C), 'dfms': ('RR', 11, 0x35D), 'dfnms': ('RR', 11, 0x35E),
    'dfnma': ('RR', 11, 0x35F), 'ceq': ('RR', 11, 0x3C0), 'mpyhhu': ('RR', 11, 0x3CE), 'addx': ('RR', 11, 0x340),
    'sfx': ('RR', 11, 0x341), 'cgx': ('RR', 11, 0x342), 'bgx': ('RR', 11, 0x343), 'mpyhha': ('RR', 11, 0x346),
    'mpyhhau': ('RR', 11, 0x34E), 'fscrrd': ('RR', 11, 0x398), 'fesd': ('RR', 11, 0x3B8), 'frds': ('RR', 11, 0x3B9),
    'fscrwr': ('RR', 11, 0x3BA), 'dftsv': ('RI7', 11, 0x3BF), 'fceq': ('RR', 11, 0x3C2), 'dfceq': ('RR', 11, 0x3C3),
    'mpy': ('RR', 11, 0x3C4), 'mpyh': ('RR', 11, 0x3C5), 'mpyhh': ('RR', 11, 0x3C6), 'mpys': ('RR', 11, 0x3C7),
    'ceqh': ('RR', 11, 0x3C8), 'fcmeq': ('RR', 11, 0x3CA), 'dfcmeq': ('RR', 11, 0x3CB), 'mpyu': ('RR', 11, 0x3CC),
    'ceqb': ('RR', 11, 0x3D0), 'fi': ('RR', 11, 0x3D4), 'heq': ('RR', 11, 0x3D8),
}

# Decode table: for each width, opcode value -> name
_BY_WIDTH = {}
for _n, (_f, _w, _v) in OPS.items():
    _BY_WIDTH.setdefault(_w, {})[_v] = _n
_WIDTHS = sorted(_BY_WIDTH, reverse=True)  # try the longest opcode first


def decode(w):
    """Return (name, fields dict) or (None, {}) for an invalid word."""
    for width in _WIDTHS:
        name = _BY_WIDTH[width].get(w >> (32 - width))
        if name is None:
            continue
        fmt = OPS[name][0]
        f = {'rt': w & 0x7f, 'ra': (w >> 7) & 0x7f, 'rb': (w >> 14) & 0x7f}
        if fmt == 'RRR':
            f['rc'] = w & 0x7f
            f['rt'] = (w >> 21) & 0x7f
        elif fmt == 'RI7':
            i7 = (w >> 14) & 0x7f
            f['i7'] = i7 - 0x80 if i7 & 0x40 else i7
        elif fmt == 'RI8':
            f['i8'] = (w >> 14) & 0xff
        elif fmt == 'RI10':
            i10 = (w >> 14) & 0x3ff
            f['i10'] = i10 - 0x400 if i10 & 0x200 else i10
        elif fmt == 'RI16':
            i16 = (w >> 7) & 0xffff
            f['i16'] = i16 - 0x10000 if i16 & 0x8000 else i16
            f['u16'] = i16
        elif fmt == 'RI18':
            f['i18'] = (w >> 7) & 0x3ffff
        elif fmt == 'HBR18':
            f['brinst'] = (((w >> 23) & 3) << 7) | (w & 0x7f)  # roh:rol, 9 bits
            i16 = (w >> 7) & 0xffff
            f['i16'] = i16 - 0x10000 if i16 & 0x8000 else i16
        elif fmt == 'STOP':
            f['code'] = w & 0x3fff
        return name, f
    return None, {}


BRANCH_REL = {'br', 'brsl', 'brnz', 'brz', 'brhnz', 'brhz'}
BRANCH_ABS = {'bra', 'brasl'}
BRANCH_IND = {'bi', 'bisl', 'biz', 'binz', 'bihz', 'bihnz', 'iret', 'bisled'}


def disasm(w, pc):
    name, f = decode(w)
    if name is None:
        return '.word 0x%08x' % w
    fmt = OPS[name][0]
    if name in BRANCH_REL:
        tgt = (pc + (f['i16'] << 2)) & 0x3fffc
        return '%s $%d, 0x%05x' % (name, f['rt'], tgt) if name not in ('br',) else 'br 0x%05x' % tgt
    if name in BRANCH_ABS:
        return '%s $%d, 0x%05x' % (name, f['rt'], (f['i16'] << 2) & 0x3fffc)
    if name in ('lqr', 'stqr'):
        return '%s $%d, 0x%05x' % (name, f['rt'], (pc + (f['i16'] << 2)) & 0x3fff0)
    if name in ('lqa', 'stqa'):
        return '%s $%d, 0x%05x' % (name, f['rt'], (f['i16'] << 2) & 0x3fff0)
    if fmt == 'RRR':
        return '%s $%d, $%d, $%d, $%d' % (name, f['rt'], f['ra'], f['rb'], f['rc'])
    if fmt == 'RR':
        return '%s $%d, $%d, $%d' % (name, f['rt'], f['ra'], f['rb'])
    if fmt == 'RI7':
        return '%s $%d, $%d, %d' % (name, f['rt'], f['ra'], f['i7'])
    if fmt == 'RI8':
        return '%s $%d, $%d, %d' % (name, f['rt'], f['ra'], f['i8'])
    if fmt == 'RI10':
        if name in ('lqd', 'stqd'):
            return '%s $%d, %d($%d)' % (name, f['rt'], f['i10'] << 4, f['ra'])
        return '%s $%d, $%d, %d' % (name, f['rt'], f['ra'], f['i10'])
    if fmt == 'RI16':
        return '%s $%d, 0x%x' % (name, f['rt'], f['u16'])
    if fmt == 'RI18':
        return '%s $%d, 0x%x' % (name, f['rt'], f['i18'])
    if fmt == 'HBR18':
        return '%s 0x%05x, %d' % (name, (pc + (f['brinst'] << 2)) & 0x3fffc if True else 0, f['i16'])
    if fmt == 'STOP':
        return 'stop 0x%x' % f['code']
    return name
