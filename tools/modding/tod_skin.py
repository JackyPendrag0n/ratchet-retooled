#!/usr/bin/env python3
"""Build a skin pack: Ratchet's replacement model for every level at once.

  tod_skin.py build <level folder> <model.gltf|glb> <out folder> [--game-skin N] [--textures DIR]

<level folder> is any level that has Ratchet (e.g. .../packed/levels/metropolis); it is only used to
encode the meshes in the game's vertex format. The model follows the same rules as `tod_model.py import`
(meshes named gG_mM, Ratchet's bone_NNN joints, each mesh within its original size). The output folder
<out>/skin.bin (+ textures/ when --textures is given) goes to <data>/skins/<name>/, and settings.ini
`skin = <name>` turns it on: the recomp lays it over Ratchet as each level loads (host/skins.cpp).
--game-skin N takes over the game's own skin N: the game draws that skin's mesh group instead of Ratchet's
armour, so put the model in that group. The slot is marked owned and is picked in the skins menu; --name and
--desc rename it there, --movie gives it its own clip there (Bink 1, e.g. from bink_encode.py), and
--select wears it at every level load instead. --preview gives it a picture in the F3 skin picker.
--relocate lets a mesh be bigger than its slot: in game it is moved into the room of the skin groups nobody
wears (about 40,000 vertices and 58,000 triangles in all), so a model can have more detail than Ratchet's.
Packs are listed in the skins menu as new skins after the game's nine (host/skins.cpp); --game-skin N is
the carrier whose group the pack is drawn in, so it must be 1..7.
--wrench model.gltf replaces the wrench while the pack is worn: its meshes gG_mM go over the wrench class's
(8 bones, 6 groups, one look per group; bone 1 is the handle, along +y), each within its original size.
--ability glide gives whoever wears the pack a long glide: hold X after a double jump (host/abilities.cpp).
--hide G,gG_mM,... empties those groups or meshes (e.g. the rest of the game skin's group; Convict, skin 7,
also draws Ratchet's head: groups 0, 5, 23 and 48 are his mouth, two heads and ears, which every other
outfit uses too).
"""
import argparse, os, shutil, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tod_model as tm

def find_ratchet(lv):
    for ci in range(lv.class_count()):
        c = lv.cls(ci)
        if c['bones'] == 281 and c['group_count'] == 49: return c
    raise SystemExit('no Ratchet (281 bones, 49 mesh groups) in %s' % lv.folder)

def find_wrench(lv):
    for ci in range(lv.class_count()):
        c = lv.cls(ci)
        if c['bones'] == 8 and c['group_count'] == 6: return c
    raise SystemExit('no wrench (8 bones, 6 mesh groups) in %s' % lv.folder)

def encode(lv, c, meshes, out, group_bit, label, relocate):
    n = 0
    for name in sorted(meshes):
        try:
            gi, mi = (int(x) for x in name[1:].split('_m'))
            me = c['groups'][gi][mi]
        except (ValueError, IndexError):
            print('skipping %s%s: no such mesh' % (label, name)); continue
        P, N, UV, J, W, T, TG = meshes[name]
        msgs = []
        vb, palette = tm.build_vertices(lv, c, me, P, N, UV, J, W, T, TG, msgs.append)
        ib = b''.join(struct.pack('>3H', *t) for t in T)
        vcap = me['nv'] * me['stride']
        icap = me['nidx'] * 2 if not me['lods'][0] else max(me['nidx'], max((x >> 16) + (x & 0xffff) for x in me['lods'] if x)) * 2
        if (len(vb) > vcap or len(ib) > icap) and not relocate:
            raise SystemExit('%s%s: %d vertices and %d triangles, but this mesh has room for at most %d vertices and %d triangles'
                             % (label, name, len(P), len(T), vcap // me['stride'], icap // 6)
                             + ('' if group_bit else ' (--relocate moves it elsewhere)'))
        out += struct.pack('<HHIIIIII', gi | group_bit, mi, me['nv'], me['nidx'], me['flags'], len(P), len(T) * 3, len(palette))
        out += struct.pack('<%dH' % len(palette), *palette)
        out += struct.pack('<I', len(vb)) + vb + struct.pack('<I', len(ib)) + ib
        n += 1
        print('%s%s: %d vertices, %d triangles (room for %d and %d)%s%s' % (label, name, len(P), len(T), vcap // me['stride'], icap // 6,
              '; bigger than its slot, relocated in game' if len(vb) > vcap or len(ib) > icap else '', ''.join('; ' + m for m in msgs)))
    return n

ABILITIES = {'glide': 0x2}   # host/skins.h kAbility*: gameplay that comes with the pack while it is worn

def ability_bits(names):
    bits = 0
    for a in (names or '').split(','):
        a = a.strip().lower()
        if not a: continue
        if a not in ABILITIES: raise SystemExit('unknown ability %r (known: %s)' % (a, ', '.join(ABILITIES)))
        bits |= ABILITIES[a]
    return bits

def build(args):
    lv = tm.Level(args.level)
    c = find_ratchet(lv)
    meshes = tm.gltf_meshes(args.model)
    if not meshes: raise SystemExit('no meshes named gG_mM in %s' % args.model)
    title = (args.name or '').encode('utf-8')[:31]
    desc = (args.desc or '').encode('utf-8')[:159]
    out = (bytearray(b'TODSKIN2') + struct.pack('<IiI', 0, args.game_skin, (1 if args.select else 0) | ability_bits(args.ability))
           + title.ljust(32, b'\0') + desc.ljust(160, b'\0'))
    n = encode(lv, c, meshes, out, 0, '', args.relocate)
    if args.wrench:
        n += encode(lv, find_wrench(lv), tm.gltf_meshes(args.wrench), out, 0x8000, 'wrench ', False)
    hide = []
    for x in (args.hide or '').split(','):
        x = x.strip()
        if not x: continue
        if x.startswith('g'):
            gi, mi = (int(v) for v in x[1:].split('_m')); hide.append((gi, mi))
        else:
            hide += [(int(x), mi) for mi in range(len(c['groups'][int(x)]))]
    for gi, mi in sorted(set(hide)):
        if 'g%d_m%d' % (gi, mi) in meshes: continue   # the model's own mesh wins
        me = c['groups'][gi][mi]
        # one degenerate triangle on a single zero vertex: nothing is drawn, the draw itself stays valid
        out += struct.pack('<HHIIIIII', gi, mi, me['nv'], me['nidx'], me['flags'], 1, 3, 1 if me['palette_n'] else 0)
        if me['palette_n']: out += struct.pack('<H', 0)
        out += struct.pack('<I', me['stride']) + bytes(me['stride']) + struct.pack('<I', 6) + bytes(6)
        n += 1
        print('g%d_m%d: hidden' % (gi, mi))
    struct.pack_into('<I', out, 8, n)
    os.makedirs(args.out, exist_ok=True)
    open(os.path.join(args.out, 'skin.bin'), 'wb').write(out)
    if args.movie:
        shutil.copy(args.movie, os.path.join(args.out, 'movie.bik'))
    if args.preview:
        shutil.copy(args.preview, os.path.join(args.out, 'preview.png'))
    if args.textures:
        td = os.path.join(args.out, 'textures'); os.makedirs(td, exist_ok=True)
        for f in os.listdir(args.textures):
            if f.lower().endswith('.png'): shutil.copy(os.path.join(args.textures, f), td)
    print('wrote %s (%d meshes). Copy the folder to <data>/skins/ and set skin = %s in settings.ini.'
          % (os.path.join(args.out, 'skin.bin'), n, os.path.basename(os.path.normpath(args.out))))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('build'); p.add_argument('level'); p.add_argument('model'); p.add_argument('out')
    p.add_argument('--game-skin', type=int, default=-1); p.add_argument('--textures')
    p.add_argument('--hide', help='comma-separated mesh groups (G) or meshes (gG_mM) to empty')
    p.add_argument('--name', help='the game skin slot\'s name in the skins menu')
    p.add_argument('--desc', help='its description in the skins menu')
    p.add_argument('--movie', help='Bink video for the slot in the skins menu (see bink_encode.py)')
    p.add_argument('--preview', help='PNG picture for the F3 skin picker (square looks best; copied as preview.png)')
    p.add_argument('--select', action='store_true', help='wear the game skin at every level load')
    p.add_argument('--wrench', help='a model for the wrench (its gG_mM meshes, the wrench\'s bone_NNN joints), worn with the pack')
    p.add_argument('--ability', help='comma-separated gameplay abilities worn with the pack: glide (hold X after a double jump)')
    p.add_argument('--relocate', action='store_true', help='allow meshes bigger than their slot (moved into unworn skins\' room in game)')
    args = ap.parse_args()
    build(args)

if __name__ == '__main__':
    main()
