# Model and texture formats, and how to mod them

How Ratchet & Clank: Tools of Destruction stores characters (mobys) and textures, decoded from the
Metropolis (Kerwan) level and checked against the running game. Everything here is implemented in
`tools/modding/` (Python 3, standard library only):

| tool | what it does |
|------|--------------|
| `psarc.py` | list / extract PSARC archives |
| `ighw.py` | list the sections of an IGHW `.dat` |
| `tod_mobys.py` | list moby classes of a level |
| `tod_model.py` | `info`, `export` (glTF 2.0 + PNG), `textures`, `import` (back into a mod archive) |
| `tod_skin.py` | `build`: a skin pack (one model for Ratchet in every level, a skins menu slot) |
| `bink_encode.py` | PNG frames to a Bink 1 video (a skin pack's menu movie) |

In the AppImage these live in `usr/share/tod-recomp/` (`docs/` and `tools/`). Get them with
`./TOD-recomp-x86_64.AppImage --appimage-extract`, or use the source tree.

## Workflow: replace Ratchet's model

```sh
L=<game>/PS3_GAME/USRDIR/packed/levels/metropolis
python3 tod_model.py info $L                 # classes; Ratchet is class 60 in Metropolis
python3 tod_model.py export $L 60 ratchet/   # ratchet/class60.gltf + textures/*.png
# Blender: File > Import > glTF 2.0, edit, File > Export > glTF 2.0 (.glb or .gltf)
python3 tod_model.py import $L 60 edited.glb <data dir>
```

`<data dir>` is the recomp's data folder (where `settings.ini` lives). The import writes
`<data>/mods/USRDIR/packed/levels/metropolis/level_cached.psarc`; the game uses it the next time
the level loads. Delete it to undo. Further imports stack on that file unless `--fresh` is given.

Rules for the edited model:
- Keep the mesh object names `gG_mM` (group G, mesh M). Blender's `.001` suffixes are fine, and
  several primitives of one object are merged. Meshes missing from the file are left as they were.
- Keep the armature (bones are matched by their `bone_NNN` names). Weights are reduced to the 4
  strongest per vertex. A skinned mesh can use at most as many distinct bones as its palette had.
- **Size limit:** each mesh is written back into its own original space, so it can have at most
  its original vertex and triangle counts (the import prints the room; meshes with lower-detail
  lists get the largest span of their lists). Blender splits vertices on UV seams and sharp
  edges, so the exported count can be higher than what Blender shows. Data appended past the
  original end of `vertices.dat`/`textures.dat` is not loaded by the game (tried growing the files
  and patching `level_cached.toc`: the tail never reaches guest memory, so the load size comes
  from somewhere not yet found). Shrinking a mesh always works.
- Positions are s16 times the class's scale, so the model can't grow far beyond the class's
  bounds (the importer says so). Ratchet is stored in every level; repeat the import per level, or build a skin pack (below).

Verified in game: unchanged round trip, a stretched Ratchet (all vertices scaled) animating
correctly, recoloured textures.

## Workflow: skin packs (new skins in the skins menu)

`tod_model.py import` rebuilds one level's archive. A skin pack instead applies one model to Ratchet
in every level and is listed in the game's skins menu as a new skin:

```sh
python3 tod_skin.py build $L edited.glb <data>/skins/myskin --game-skin 5 --name "My skin" --desc "..." --hide 45 --textures mytextures/ [--relocate]
# then pick "My skin" in Extras > Skins (or settings.ini: skin = myskin)
```

- **New entries:** every folder in `<data>/skins` becomes a skin after the game's nine (ids 9, 10, ...),
  so packs never take a game skin's place and there is no limit on how many. `host/skins.cpp` raises
  the Lua constant `SKIN_TYPE_COUNT` (registered at 0x2A58C) and answers the menu's skin functions for
  ids 9 and up at their entry: is_skin_owned / is_skin_available (sub_24B08 / 24B38, always yes),
  get_skin_cost (sub_25B90, free), purchase_skin (sub_27AA0), select_skin (sub_26FC0), get_skin_name /
  get_skin_desc (sub_25B68 / 25B40, from `--name` / `--desc`) and get_skin_movie (sub_25B18, gives
  `SKIN_<NAME>`, so the menu plays `built/movies/skin_<name>/...`, which is the pack's `movie.bik` or else its
  carrier's clip).
- **Carrier:** the game draws a worn skin's mesh group instead of Ratchet's armour. A pack names one of
  those skins with `--game-skin N` (1..7; Cronk 5 = group 45, Zephyr 6 = group 46). Put the model in that
  group's meshes (gGG_mM) and `--hide` the group's other meshes. When the pack is picked, the game is told
  to wear the carrier and the pack's vertices, indices, palettes and mesh entries are written over the
  carrier's in Ratchet's class in memory, at once. The original bytes are kept, so picking the carrier
  itself (or anything else) puts it back. Nothing on disk changes.
- **Bigger than the slot (`--relocate`):** a mesh may have more vertices or triangles than the one it
  replaces. In game it is moved into the room of the skin groups no player wears (groups 41..47: about
  40,000 vertices and 58,000 triangles in all, less what other worn packs use), and its mesh entry is
  pointed there. Tested with a 14,156 vertex / 19,200 triangle body mesh, about double the
  biggest game skin group. A mesh with no room left is drawn empty (log line `... without room`).
- **Co-op:** player 2 wears their own skin (`skin2 = <pack or game skin number>`, `!skin2 <id>`; empty =
  as player 1). sub_1E4CF0 picks each hero's skin meshes through the selected-skin getter sub_1E21A0 (r3 = the hero), which answers player 2's. Both players
  share Ratchet's class, so two worn packs must have different carriers (else player 2 shows player 1's).
- **Preview (`--preview picture.png`):** copied into the pack as `preview.png`, the pack's picture in the F3
  skin picker (square looks best; any size, it is scaled down to 256 pixels). A pack without one shows its
  initials. It can also be dropped into an existing pack folder by hand.
- **Abilities (`--ability glide`):** a pack can bring gameplay along, for whichever player wears it (player 1,
  or co-op player 2 with their own skin). The abilities are bits in `skin.bin`'s flags word (`host/skins.h`
  `kAbility*`), read by `host/abilities.cpp`. `glide`: after a double jump, holding Cross gives a long glide
  (the fall held to 0.9 m/s, the forward speed building up to 12 m/s, for as long as Cross is held), where
  the game's own glide (Cross held while falling) caps the fall at 2 m/s and ends after about 3 s. It runs as
  the hero copies its position into its moby (HOOKS 0x23E608): hero +0x1210 is the velocity, +0x1334 a timer
  counting up on the ground and down from 0 in the air, +0x1260 / +0x1280 / +0x1AA0 this frame's position
  (+0x1250 / +0x1270 the last frame's). The frame's movement is done by then, so the glide adds the
  difference to its own motion to the position. Note +0x1210 is only a copy of the game's velocity: writing
  it changes nothing.
- **Wrench (`--wrench model.gltf`):** the Omniwrench is its own class (8 bones, 6 mesh groups, one look
  per group g1..g5; Kerwan class 188; bone 1 is the handle, along +y, the hand at the origin). A pack can
  carry meshes for it (`skin.bin` group | 0x8000), written over the wrench's in place while the pack is worn
  and put back after. The class is found next to Ratchet's (classes sit 0xC0 apart). The wrench is shared,
  so in co-op both players hold player 1's pack's wrench. A replacement meant for every look goes into all
  five, with all five looks' textures replaced, so whichever one the game draws is the new wrench.
- Each mesh is checked against the original counts stored in `skin.bin`; a level whose Ratchet
  differs is left alone.
- Convict (7, `g47_m0`) also draws Ratchet's head, ears and mouth (groups 5, 23, 48, 0), which every
  other outfit uses too, so hiding them breaks the other skins.
- `bink_encode.py out.bik <fps> frame*.png` writes a menu clip from PNG frames (Bink 1, raw or
  skipped 8x8 blocks, no sound; a still background keeps it small); `--movie clip.bik` adds it.
- `--textures DIR` copies texture-pack PNGs into `<pack>/textures/`. They apply only while the pack is
  worn, and the renderer re-uploads the textures when that changes. Replace the carrier's textures
  under every hash they have: while a skin is worn the game streams a higher resolution copy with its own
  hash (find it with `texture_dump`: the dumped files are the same image at 512x512). Cronk:
  611 `c0b27a3dcfa4c3c3` / `895dc40dcd45a42a`, 612 `7dccad6ddec362a4` / `253779c293d7e776`,
  613 `998a2e78d304b496` / `00f3c0be0618e5ca`, 614 (and 617) `ccd42dea368c3c10` / `53fde4916f0e395e`,
  615 `db29f31bfca96cbe` / `e39b505e555d2f70`, 616 `b8cb48c8781fa8d9` / `da0e96df53f17c68` (256 / 512).

Game skin ids (front-end struct at `0x888624`: owned flags at +0x45C + 4*id, selected id at +0x480;
select/owned/purchase/name helpers at sub_26FC0/24B08/27AA0/25B68): 0 Ratchet, 1 Dan Johnson
(group 41), 2 Snowman (42), 3 Cragmite (43), 4 Rusty Pete (44), 5 Cronk (45), 6 Zephyr (46),
7 Convict Ratchet (47), 8 Mustachio Furioso. Cheat console: `!skin <id>` (`!skin -1` lists names, packs included), `!skin2 <id>` for co-op player 2.

`skin.bin` (little endian): `TODSKIN2`, u32 mesh count, i32 game skin (the carrier), u32 flags
(unused now), char name[32], char description[160] (empty = keep the game's); per mesh u16 group, u16
mesh, u32 original vertex count / index count / flags, u32 new vertex count / index count /
palette size, u16 palette[], u32 length + vertex bytes, u32 length + index bytes (both already in
the game's big-endian format). `TODSKIN1` files (no flags or name; the game skin is always worn)
still load.

In memory (Ratchet's class, from the hero's moby +0x54): +0x19 u8 mesh group count, +0x20 skeleton
(u16 bone count first), +0x28 group table ({mesh array, count} pairs, 64-byte mesh entries as below),
+0x34 index base (RSX local offset: EA 0xC0000000 + base + istart * 2), +0x38 vertex base | 0x80000000
(EA (base & 0x7fffffff) + 0x50000000 + vbyte). Mesh entry +0x20 = palette address.

## Workflow: textures

Textures are replaced at runtime by texture packs, not inside the archives:
- `tod_model.py textures $L out/ [class]` (or `export`) writes PNGs named `<hash16>_WxH.png`.
- Or turn on Graphics > Texture pack > dump (setting `texture_dump=1`) to dump what the game
  draws into `<data>/textures/dump/`.
- Edit a PNG and put it in `<data>/textures/` (subfolders OK, any size, the name must start with
  the hash). Hash: xxh64 (seed 0) of the texture's guest data, all mips (`texture_guest_size`).
  Channels the texture's remap forces to 0/1 are re-applied to the replacement.

## Mods folder

`<data>/mods/<path under PS3_GAME>` replaces that game file, e.g.
`mods/USRDIR/packed/levels/metropolis/level_cached.psarc`. `fs: mod file` log lines report each one.

## Archives (PSARC)

Big-endian header `>4sI4sIIIII`: `PSAR`, version, `zlib`, TOC length, entry size (30), entry count,
block size (64 KiB), flags. Entries: 16-byte md5, u32 first block index, u40 length, u40 offset.
Then a u16 table of compressed block sizes (0 = full block). Blocks are zlib streams, or raw when
they didn't compress. Entry 0 is the newline-separated file name list.

`packed/levels/<level>/level_cached.psarc` holds a level's own data (no shared character file):

| file | contents |
|------|----------|
| `built/levels/<level>/main.dat` | IGHW: classes, skeletons, meshes, animations, materials, texture headers |
| `built/levels/<level>/vertices.dat` | vertex data for classes whose vertex base has bit 31 set |
| `built/levels/<level>/textures.dat` | raw RSX local memory image: textures, index buffers, other vertex data |
| `level_cached.toc` | 16-byte records: u32 name offset, u32 file size at +4 |
| `level_textures.psarc: texstream.dat` | higher resolution mips streamed in later |

At runtime textures.dat is loaded raw into RSX local memory (Metropolis base 0xC22AC280) and
vertices.dat into main memory (block base 0x53BBE880, data from file offset +0x80).

## IGHW container (v1.1)

Header: `IGHW`, u16 1, u16 1, u32 section count, u32 header length, u32 file size, u32 count,
`deaddead`. Section table at 0x20, 16 bytes each: u32 id, u32 offset, u32 count (bit 28 = array of
`count` elements), u32 element size (or blob size). Pointers are file offsets.

main.dat sections (Metropolis counts):

| id | layout | meaning |
|----|--------|---------|
| 0xD100 | 201 x 192 | moby classes |
| 0xD300 | blob | skeletons |
| 0xD700 | blob | mesh group tables, bone palettes |
| 0xDD00 | 1618 x 64 | moby meshes |
| 0xF000 / 0xF200 / 0xF400 | 642 x 64 / blob / 642 x 48 | animations (`ratchet_forward_run`, ...) |
| 0x5000 | 631 x 128 | materials |
| 0x5200 | 1297 x 32 | texture headers |
| 0x3300 / 0x3400 / 0x3500 | | ties (static level geometry) |

### Moby class (0xD100, 192 bytes)

| offset | field |
|--------|-------|
| 0x00 | bounding sphere: float x, y, z, radius |
| 0x18 | u16 mesh group count |
| 0x20 | skeleton pointer |
| 0x28 | group table pointer: {u32 mesh array pointer, u32 mesh count} per group |
| 0x34 | index base: offset in textures.dat |
| 0x38 | vertex base: bit 31 set = vertices.dat at `(v & 0x7fffffff) + 0x80`, else textures.dat offset |
| 0x3C | float position scale |

Ratchet (class 60, Metropolis): 281 bones, 49 groups, 120 meshes.

### Mesh (0xDD00, 64 bytes)

| offset | field |
|--------|-------|
| 0x00 | first index, in u16 units from the class index base |
| 0x04 | vertex byte offset from the class vertex base |
| 0x08 | material << 16 \| vertex count |
| 0x0C | flags: top byte = palette size; byte 2 = 1 when skinned; low byte 04 = has LOD lists |
| 0x10 | index count |
| 0x14..0x1C | lower-detail index lists as (start << 16 \| count), when the low flag byte is 04 |
| 0x20 | bone palette pointer (u16 bone ids) |

Indices: u16 triangle lists. Skinning runs on the SPU into scratch memory (0x50000000).

### Vertex

Skinned, 28 bytes (big-endian):

| offset | field |
|--------|-------|
| 0 | s16 x, y, z (times the class scale) |
| 6 | s16 w: bitangent sign, stored opposite to `sign(dot(cross(N, T), B))` |
| 8 | u8 bone[4]: palette slots |
| 12 | u8 weight[4], sum 255 |
| 16 | f16 u, v |
| 20 | u32 normal, RSX CMP packing (11/11/10 signed bits, x in the low bits) |
| 24 | u32 tangent, same packing |

Rigid, 20 bytes: the same without bones and weights.

### Skeleton (0xD300)

u16 bone count, then pointers to: hierarchy (8 bytes per bone, s16 parent at +2), world bind
matrices (64 bytes per bone, row-major, translation in the last row), inverse bind matrices.

### Materials and texture headers

Material (128 bytes): texture header pointers at +0 (diffuse), +4 (normal map: DXT5, remap
0x6ae0, so x and y sit in A and G; a flat normal map is (128,128,255,128) in the channel order the
dump shows) and +8 (a specular-like mask, R carries the strength). Unused slots are 0. A model
with new UVs needs all three replaced, or the old normal and spec maps show through. A texture header (32 bytes) is the raw
RSX register set: offset (in textures.dat), format, address, control0, remap, filter,
image_rect (w << 16 \| h), control3. Formats seen: DXT1/3/5, A8R8G8B8, B8, G8B8; swizzled (z-order)
unless the linear bit is set. Decoding mirrors `host/rsx_tex.cpp`.

## Debug helpers in the recomp

- `TOD_FS_TRACE=1`: log every file read (path, offset, size, buffer).
- `TOD_DRAW_TRACE=1`: create `<data>/drawtrace` and the next frame's indexed draws are logged
  (index address, count, ranges, vertex attributes); the file is then removed.
- Cheat console `!find <hex bytes>`: search guest memory (up to 64 hits).
