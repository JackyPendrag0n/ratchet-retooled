# PPU static recompiler (BCUS98127 01.00)

Turns the whole decrypted `EBOOT.elf` into C++: one function per guest entry point
(23,910 functions + 2,334 TOC trampolines + 118 import stubs, ~2.11M instructions).

## Build (Fedora / any Linux, x86-64)

Needs Python 3, CMake, Ninja, and GCC 13+ or Clang 16+ (C++20).

```bash
python3 ppu_codegen.py /path/to/EBOOT.elf gen            # ~25 s, writes gen/ (≈90 MB C++, 22 files)
python3 spu_codegen.py /path/to/EBOOT.elf gen_spu        # SPU programs (optional; without it SPUs are interpreted)
cmake -S . -B build -G Ninja && ninja -C build           # ≈5.5 min on 4 cores at -O2
./build/startup_check /path/to/EBOOT.elf one             # empty | one | many
```

Generated sources are not stored here because they are derived from the game binary. Regenerate them locally.
Add `--probe 0xADDR` to `ppu_codegen.py` to make execution stop at an address.

## Layout

- `ppu_codegen.py`: the generator. It uses `tools/ppu_funcs.py`, the whole-binary analysis (function
  boundaries, 371 switch tables, EH landing pads).
- `runtime/ppu.h`: guest context (GPR/FPR/VR, CR as 8 fields, XER, FPSCR), big-endian memory
  access over a flat 4 GiB host reservation, and arithmetic/FPU/VMX helpers.
- `runtime/ppu_runtime.cpp`: address → function lookup for indirect calls, `lwarx/stwcx.`
  reservations via host CAS, timebase (79.8 MHz), error paths.
- `harness/startup_check.cpp`: boots the entry point under checkpoint 004's synthetic launch and
  HLE models.

## How code is generated

- `bl` becomes a direct C++ call and `blr` becomes `return`. A branch to another entry is a tail call.
- Switch tables become `switch (ctr) { case target: goto … }`, keyed on the target address because the index
  register is often reused before the `bctr`.
- `bctrl` and other indirect branches go through `ppu_dispatch`.
- Import stubs reproduce the real stub's register effects (r12, r0, r2, CTR, TOC save at 40(r1)) and then call
  `ppu_import(index)`. `sc` calls `ppu_syscall`. `tw`/`td` and the one illegal word call `ppu_trap`.
- `setjmp`/`longjmp` (Lua's error handling): the translated longjmp restores the saved registers and throws
  `GuestLongjmp`; the function that called setjmp catches it and resumes after its setjmp call.
- Every reached instruction form is handled. Unhandled forms would compile to `ppu_unimplemented`, and the
  count is 0 (`codegen_report.json`).

## Verification so far

With a probe at the old stop point (0x78142c), all three launch fixtures produce the **same
state checksum as checkpoint 004**: all GPRs, LR, CTR, CR, XER and every mapped byte of guest memory
(`0x5fd1347c0743ca30`, `0xfa418efe10a54b70`, `0x9eade7f45859643f`). All 11 import events match
(arguments, results, LR, SP).

Without the probe, execution continues through the static constructors until
`sc` with r11 = 147 (`sys_time_get_timebase_frequency`) at 0x7d4318. Stubbing that, the next stop is syscall 145
(`sys_time_get_current_time`). From here, what blocks progress is the lv2 syscall + HLE layer, not the CPU.

That path covers about 17k distinct executed instructions, mostly integer, load/store and branch.
FPU and VMX lowering compiles but has not been executed against a reference yet. Fixed choices that
hardware leaves undefined: divide-by-zero gives 0, the upper word of `fctiw` gives 0xFFF80000, and
FP-record (Rc=1) sets CR1 to 0. The C++ exception unwinder (runtime library only) is not modelled.

## Host executable (`tod`)

`host/` is the native runtime the recompiled code runs on: an lv2 kernel layer, HLE system libraries and
a command processor for the RSX, all specific to this game.

```bash
cmake -S . -B build -G Ninja && ninja -C build tod
./build/tod /path/to/EBOOT.elf --game /path/to/disc --data ./tod-data [-v | -q]
```

`--game` is the folder holding `PS3_GAME/`. `--data` backs `/dev_hdd0` and `/dev_hdd1` (saves, caches).
`-v` traces every syscall and import, with the guest thread id on each line. `TOD_RSX_TRACE=file` writes
every RSX method and argument.

- `core.cpp`: guest memory map, kernel object ids, one host thread per guest thread, import dispatch,
  host thunks (host functions callable through guest function pointers), `guest_call`.
- `lv2.cpp`, `sync.h`: the 80 syscalls the EBOOT issues: threads, mutex/cond/rwlock, event queues,
  timers, memory and mmapper, TTY. File system calls go to `fs.cpp` (VFS onto host folders).
- `hle_prx.cpp`: sysPrxForUser (TLS, lwmutex, threads, mmapper). `hle_misc.cpp`: sysmodule, sysutil,
  video/audio out, syscache, disc game. `hle_io.cpp`: cellPad, cellAudio (48 kHz mixer thread), game data.
- `hle_gcm.cpp`, `rsx.cpp`: cellGcmSys on top of a command processor that walks the FIFO (jumps, calls,
  PUT/GET), keeps the method register file (reset values in `rsx_defaults.inc`), and performs the
  effects the CPU can see: labels, semaphores, reports, NV0039 copies and flips, with a 59.94 Hz
  vblank/interrupt thread. Drawing is counted but not rendered yet; the Vulkan backend hooks in at
  `rsx::on_flip` and the draw methods.
- SPUs. `spu_interp.cpp`, `spu_ops.h`: SPU interpreter (full ISA). `spu_mfc.cpp`: MFC DMA (lists with
  stall-and-notify, lock-line atomics GETLLAR/PUTLLC/PUTLLUC), tag groups, events, decrementer, mailboxes and
  signals. `hle_spurs.cpp`: a SPURS kernel on host threads (workloads, ready counts, policy-module
  start-up and the kernel's exit/select traps); the game's own job manager (WWS) runs on it as SPU code.
  The Edge zlib inflate task (psarc decompression) still runs natively as host zlib.
  `spu_raw.cpp`: raw SPUs (the collision code), with their problem-state registers reached through 32-bit
  MMIO hooks in `runtime/ppu.h`.
- SPU static recompiler. `spu_codegen.py /path/to/EBOOT.elf gen_spu` translates the SPU programs embedded
  in the EBOOT into position-independent C++ blocks (about 19k blocks, 242k instructions). When `gen_spu/`
  exists CMake builds it in. At run time `spu_recomp.cpp` matches the bytes in local store against the blocks,
  so it does not matter where a program was DMA'd; code it has no block for runs in the interpreter, and a
  page generation counter catches code overwritten by later DMA. `TOD_SPU_INTERP=1` forces the interpreter.
- `hle_savedata.cpp`: the four list save/load operations with their callbacks. Saves go to the save folder
  (`--saves`, or `save_folder` in settings.ini; the AppImage uses `TOD Saves` next to itself, else
  `<data>/saves`), one `BCUS98127_SAVE_<n>` folder per save in the layout RPCS3 uses. Saves from the old
  place (`<data>/dev_hdd0/home/00000001/savedata/`) are copied over once. There is no save list dialog:
  a list save overwrites only the slot being played (the game's save manager keeps it at
  `*userdata + 0xEC`, -1 after New Game) and otherwise takes the new save the game offers, so New Game
  never overwrites a save; a list load takes the newest save.

- `settings.ini` in the data folder (created on the first run): `skip_intro` (hides the logo movies and
  passes the game's `-no_legal`), `framerate = 30|60|61..240|0` (60 is the game's `-60fps`; above 60 the
  game's `-capfps`, which caps the frame time and runs each frame with the measured time step; 0 = no
  cap), `vsync`, `cheat_menu`, `god_mode`, `infinite_ammo`, `no_clank`, and
  `aspect = 16:9|21:9|32:9` (patches the game's 16:9 projection constant and presents at that shape).
  `renderer = native|legacy`, `resolution_scale = 1..4`, `antialiasing = fxaa|off`, `anisotropic_filtering = 0|2|4|8|16`,
  `rumble = 0..100` (controller vibration strength, from the game's cellPadSetActDirect motors; 0 = off), `texture_dump`,
  `soft_shadows` and `async_shaders` (native renderer; all but the last can also be changed live in the F1 menu's
  Graphics tab, which writes them back to settings.ini). Settings
  added in newer versions are appended to an existing settings.ini. With `skip_intro`, the title screen's
  background movie (`l00_s00_attr_fmv`) still plays.
  Arguments after `--` go to the game as its own switches.
- Profile-guided SPU blocks: `TOD_SPU_PROFILE=1 TOD_SPU_ENTRIES=spu_entries.txt` records code at entries
  that had no block; `spu_codegen.py` reads `spu_entries.txt` next to it and adds them as block starts. The profiler writes each entry as its first 32 bytes of code in hex; the `spu_entries.txt` shipped here stores a hash of those bytes instead (`h:` + 16 hex digits of SHA-256), so the repository holds no game code, and the generator hashes every word offset of the EBOOT's SPU programs to find them. The
  `spu_entries.txt` here (1,644 entries, recorded from the title screen into Kerwan) takes the share of executed SPU
  instructions that run recompiled from about 89% to 99% there; the profile log prints that share every 10 s.
  MFC channel writes (16-23) and channel counts run inline in blocks.
- SPU floats approximate the SPU's extended range (no Inf/NaN): exponent-255 inputs read as +-FLT_MAX and
  results clamp to it.

- PPU `sync`/`lwsync`/`eieio`/`isync` become host fences (`sync` a full fence, the others compiler
  barriers), so the C++ compiler can't move guest stores past the flag writes other threads poll. Every
  loop head (target of a backward branch) gets a compiler barrier too, so spin waits re-read memory
  (one waiting for RSX get == put had become an endless loop).
- `HOOKS` in `ppu_codegen.py` insert host checks at guest addresses: draw-bucket records with no callback
  are skipped (sub_4BE238), and story movies (`built/movies/*_fmv`) end when START or Cross is pressed
  (BinkOpen results are noted at 0x5D2FD4/0x5D30A4; the "still playing" test at 0x5D28AC reports the end).
- Draw guards: a side render pass (sub_536960 on a stack pass struct, scene 0x10369300) can pick up the
  main scene's depth-sorted bucket 6, which has no handler there; those records are skipped. sub_524018
  leaves a gap in each skinning command chunk for a later step to fill; every gap is pre-filled with RSX
  NOPs (0x524B14), so a dropped list entry can't leave stale bytes for the RSX to run, and the entry is
  skipped when the list has no array yet.
- Atomics: PPU `stwcx`/`stdcx`, SPU `getllar`/`putllc`/`putlluc` and the HLE SPURS counters share
  per-128-byte line locks (`ppu_line_lock`). Before posting a "special" raw-SPU command (0x4A3788) the PPU
  waits until the helper SPU has released the job lock and the master has cleared flag 4 (after its
  done-header PUT), which on hardware always happens first.
- Diagnostics (env): `TOD_SLOTLOG` traces the raw-SPU command slot, `TOD_WATCH_EA=<hex>` traces DMA and
  draw-record writes covering an address, `TOD_SORTCHECK` checks the SPU draw-record sort. The RSX logs
  its recent jumps and commands when the FIFO leaves the IO map.
- SPU `shufb` uses SSSE3 `pshufb` (the byte loop was a fifth of all CPU time).
- The log prints `perf: N fps, slowest frame, draws per frame` every 10 s.
- `TOD_OPEN_CALLERS=1` logs the guest call chain of every file open.
- `TOD_FS_TRACE=1` logs every file read (path, offset, size, guest buffer).
- `TOD_DRAW_TRACE=1`: creating `<data>/drawtrace` logs every indexed draw of the next frame (index address, count,
  ranges, vertex attributes), then removes the file. The test command `!find <hex bytes>` lists the guest addresses that hold some bytes (used to place level files in memory).

### Cheat and debug menu

F1 on the keyboard or L3+R3 on a controller opens a Dear ImGui overlay (`third_party/imgui`, drawn by
`host/overlay.cpp` after the frame is blitted to the swapchain; Esc or F1 closes it). While it is open
the game gets a neutral pad.

- Player: god mode, infinite ammo, remove Clank, no clip, free camera, +100,000 bolts, armor, and the Gelanator mod
  (`gelanator_mod` in settings.ini: the Gelanator is given once and kept full of gel every frame).
- Raritanium upgrades (Weapons tab): every owned weapon's upgrades by name, each one added or removed on its own
  (free), plus +1,000 raritanium. A weapon's upgrades are bits in the u32 at +0xC of its inventory entry; names and
  counts come from the vendor bindings' helpers (sub_25878, sub_26548), adding calls sub_465D00 (sets the bit and
  refills ammo when the capacity grows), removing clears the bit and trims ammo to sub_463DE0's capacity.
  Raritanium is the s32 at inventory +0x420. Test commands: `!mod <weapon> <n> 0|1`, `!rari n`, `!gel 0|1`,
  `!res n`, `!aa 0|1`.
- Weapons: give any weapon, gadget or device, take it away again, and set each weapon's level (V1..V10).
- Level warp: any planet from its start (the game's `set_level`).
- Fun: the game's own Extras cheats (Big Head, Mirrored levels, weapon and hero cheats...) with every state, a
  button that unlocks everything in the Extras menu, Ratchet's moustache, a game speed slider (0.1x-3x, the global
  time scale float at frame clock +0xA0; F2 toggles a slow-motion preset), photo mode (F4: the world freezes at
  speed 0.001 and the free camera flies; F12 saves a screenshot), practice tools (F5 saves
  Ratchet's position, F9 puts him back, same level only), a speedometer, and the 60 skill points with how to earn
  each one, marked done from `is_skillpoint_done` and settable by hand. The Extras cheats are read through the
  front end's helpers by menu index (count sub_24ED8, name sub_25110, states sub_24F18/sub_24F80, get sub_27098,
  set sub_27008); skill point names and hints are sub_11F60/sub_10540 by id. Test commands: `!speed <percent>`,
  `!cheat <id> <state>`, `!unlockcheats`, `!skill <id>`, `!stache 0|1`, `!savepos`, `!loadpos`, `!shot`.
- Texture packs (`host/tex_pack.cpp`): a PNG in `<data>/textures` whose name starts with a texture's 16-digit hash
  replaces that texture (any size; mips are rebuilt; subfolders work). `texture_dump = true` (or the Graphics tab)
  saves every texture the game uploads to `<data>/textures/dump/<hash>_WxH.png`, already in the channel order the
  shaders see, so a dumped file can be edited and moved up one folder. The hash is xxh64 of the texture's guest data
  (all mips), so it doesn't depend on where the game loaded it. Replacements apply when a texture is uploaded, so
  from the next level load.
- Mods folder (`host/fs.cpp`): `<data>/mods/<path under PS3_GAME>` replaces that game file.
- Model swaps: `tools/modding/tod_model.py` exports a character (Ratchet is class 60 in Metropolis) to glTF 2.0 with
  skeleton, weights and textures, and imports an edited glTF back into a rebuilt `level_cached.psarc` in the mods
  folder. Each mesh must fit its original vertex/triangle space. Formats and the Blender workflow:
  `docs/model_formats.md`. The AppImage carries both in `usr/share/tod-recomp/` (`--model ...`, `--docs`).
- Skin packs (`host/skins.cpp`): `tools/modding/tod_skin.py build` turns one edited Ratchet model into
  `<data>/skins/<name>/skin.bin` (+ `textures/`). Every pack is listed in the skins menu as a new skin after the
  game's nine (the menu's skin functions are hooked for ids 9 and up, and `SKIN_TYPE_COUNT` is raised), so there
  is no limit on how many. Picking one writes it into Ratchet's model in memory right away: the pack names a
  game skin as its carrier (1..7, e.g. Cronk), the game draws that skin's mesh group, and the pack's meshes,
  palettes and textures replace that group's. The original bytes are kept and put back when the pack comes off.
  A mesh bigger than its slot (`--relocate`) is moved into the room of the skin groups nobody wears (about
  40,000 vertices and 58,000 triangles), so a model can have more detail than Ratchet's. In co-op, player 2 can
  wear another skin (`skin2`, `!skin2 <id>`): sub_1E4CF0 picks each hero's skin meshes through the
  selected-skin getter sub_1E21A0 (r3 = the hero), which answers player 2's. Two worn packs need different
  carriers. A pack can also replace the wrench (`--wrench`, its own class, written in place while worn), and
  bring gameplay along for whoever wears it (`--ability glide`, `host/abilities.cpp`: after a double jump,
  holding Cross is a long glide; flags in `skin.bin`, see `docs/model_formats.md`). `!glide trace n` logs the
  hero's movement, and `TOD_HERO_REC=<flip>,<updates>,<file>` records player 1's hero object every update.
  `skin = <name>` is what the menu last picked; `!skin <id>` picks any entry by id (-1 lists them).
  The skins menu has the picked skin's own data loaded after it closes, over a pack laid out on that skin, so
  the bytes of the layout are kept and compared every other hero update: when the game has written over them,
  what it wrote becomes the new original and the pack is written again (`!skinclobber` simulates it).
  The F1 menu's Skins tab (F3 opens it straight away, `!menu 2`) is a picker with a card for every skin, custom
  ones first: `<pack>/preview.png` as its picture (`tod_skin.py --preview`), else its initials; badges for who
  wears it and its abilities, a search box, and a player 1 / player 2 switch in co-op.
  Headless tests: `!pad lx ly rx ry d1 d2` and `!shot` in the command file also work in the pause menu.
- F12 saves a screenshot (PNG, at render resolution) to `~/Pictures/TOD Recomp`, or `<data>/screenshots` if there is
  no Pictures folder. F2/F3/F4/F5/F9 work while `cheat_menu` is on (F3: the skin picker).
- Graphics: resolution scale (1x-4x), anti-aliasing (FXAA), anisotropic filtering, soft shadows, and the frame
  rate limit (30, 60, 120, 144, 165, 240 or uncapped). Changes apply at once and are saved to settings.ini
  (`gfx::save_setting`). The game's own options menu (Anark/Lua UI) is not extended; this tab replaces that.

How it works (`host/cheats.cpp`, `cheat_state.cpp`, `cheat_fly.cpp`, `cheat_menu.cpp`):

- The game's rules are in Lua 5.0, and its bindings (`hero_give_weapon`, `hero_set_armor`,
  `health_set_invulnerability`, `hero_set_infinite_ammo`, `ratchet_remove_clankpack`, `set_level`...)
  do the work. HOOKS 0x293C70 (a level script update, on the main thread, outside any Lua call) runs
  queued chunks in the level scripts' Lua state with `lua_load`/`lua_pcall`; a chunk's output lines come
  back as the message of a final `error()`. Bindings reject extra arguments, even nil ones.
- HOOKS 0x280DD8 (`get_pos` on a moby) notes the hero's moby. The hero is `moby+0x40`; its inventory
  (`hero+0x1A68`) has 20 bytes per weapon type: XP at +4, ammo at +8, owned flag at +0x10, level at +0x11.
  XP per level comes from the weapon's config (`sub_4632B0`, floats at +0x234, count at +0x460), and
  `hero_give_weapon(h, type, xp)` sets the XP (so the level) of a weapon already owned.
- Free camera: HOOKS 0x48C910 gets the camera's world matrix (`r25`, rows x, y, z, position; the
  world is y-up) before the frame's view, frustum and culling are built from it.
- No clip: HOOKS 0x23E608 runs as the hero copies its position into its moby. The hero keeps its
  position at +0x1250..+0x1280 and +0x1AA0 (all have to change for a move to stick) and its velocity at
  +0x1330.
- Frame rate: the frame clock at 0x10731D00 has the frame time cap at +4 and the fixed step at +8 (equal
  means a locked step, as `-lockfps`/`-60fps` set them).
- Tests without a window: `<data>/lua_commands.txt` is read every half second and renamed to `.done`.
  It holds Lua, or one command such as `!give 5 5`, `!armor 2`, `!god 1`, `!freecam 1`, `!pad 0 -127`
  (hold the sticks), `!status`, `!dump <hex> <hex>` or `!watch <hex address>` (logs the next guest code
  that writes there). See `poll_command_file`.

- A guard at 0x4BE5B0 skips a draw bucket call that hands sub_4D3D00 a record count instead of an object
  (crashed after warping to Ardolis).

### Weapon XP in the pause menu

`weapon_xp = true` in settings.ini (the default), or F1 > Fun > "Weapon XP on the weapons page" (`host/weapon_xp.cpp`).
The pause menu's Weapons page starts the selected weapon's description with its XP and the XP its next level needs,
e.g. `XP: 700 / 1,000` for a Combuster at V1, or `XP: 6,440 (max)` at the highest level it can reach. It is the
page's own text object, so it uses the menu's font and colour, and a description that gets too long scrolls the way
long descriptions already do.

How it works:

- The pause menu is an Anark presentation (`built/anark/pause/built.dat`) with Lua 5.0 scripts. The Weapons page's
  update (line 156 of its script) sets ammo, `weapName`, `weapDesc.textString = get_weapon_desc(w)`, `weapPwr`, then
  `verValue.textString = "V" .. get_weapon_level(w)` and the XP bar's scale from `get_weapon_progress(w)`, and resets
  the description's scroller. The XP is never shown as a number.
- HOOKS 0x2F938 and 0x33B50 note the weapon passed to the get_weapon_desc (sub_2F880) and get_weapon_level
  (sub_33A98) bindings. HOOKS 0x74F0F4 sits in the Text.textString setter (sub_74F050, tolua), just before
  CLuaObjectText SetText (sub_742B4C, r28 = text object, r3 = string). The text set right after get_weapon_desc is
  noted; when the text set right after get_weapon_level of the same weapon is `V<level>`, the noted description is
  set again with the XP line first. The quick select page writes the bare level number, so it is left alone.
- The numbers are the ones the game's progress bar uses (sub_465FF0): the inventory entry (main TOC[-0x7914] +
  20 * type: XP float at +4, level index at +0x11), the config's XP per level (sub_4632B0, floats at +0x230) and the
  highest level reachable now (sub_465F08: V5 outside challenge mode).
- Tried first: after the level text (hidden under the XP bar) and on the name line (long names such as FUSION GRENADE
  run into the level).

### Two-player co-op (experimental)

`coop = true` in settings.ini, or the Fun tab's "Two players, split screen" (`host/coop.cpp`). A second Ratchet
joins a moment after a level starts and the screen splits side by side: player 1 (left) is the first controller,
player 2 (right) the second controller or the keyboard (with no controller at all, the keyboard is player 1). Each
player has their own camera and the game's own HUD in their half (health, bolts, weapon and ammo; the quick select
shows in the half of whoever has it open). Max health and XP are shared; each hero keeps their own current health.
Weapons, ammo and bolts are shared because both heroes point at the one inventory, and the players can't hold the
same weapon (the Omniwrench excepted: whoever just switched to the partner's weapon goes back to their last one).
Player 2 opens the quick select with their own triangle: while it is open their controller drives it, and the
weapon picked goes to player 2. D-pad Down teleports you next to your partner. Player 2's view is dropped as soon
as a level change starts and player 2 comes back once the new level has started; switching co-op off takes effect
at the next level load or restart.

How it works:

- Player 2's hero is spawned like any moby (`sub_68EC88`, class 60, update type 529) with player 1 hidden from the
  "a Ratchet already exists" check, so it initialises fully; flag 0x2 at moby +0x68 (kept out of the update list)
  is cleared, and its "take control" virtual (hero vtable +0x48, sub_1E3A30) binds its input to controller 1.
- The hero manager (0x101D5C30) has room for one 0x58-byte player slot. The hero update (vtable +0xC) is
  wrapped by a host thunk that swaps player 2's slot (view handle, controller) into slot 0 while player 2's hero
  runs. HOOKS at sub_1E1298 / sub_1E1120 send lookups of slot 1 (the frame loop asks for view i's player) to
  player 2's slot in guest memory.
- Split screen is the engine's own 2-view layout (sub_693E80 / sub_694778: layout 3 left half, 4 right half).
  The second view comes from CreateView (sub_680D0), its camera table (sub_6393F0) and render init (sub_68500).
  Its render context was never set up by the level load, so its draw buffer (sub_4BEB10), camera block
  (sub_599B20, both redone every time the view is made: a respawn after a death resets only the contexts in the
  list, and a stale one crashed in the draw buckets on player 2's second return), per-context draw list table entry (sub_4F4060, table 0x100AF740) and frame reset (sub_4BEA08)
  are done by hand. HOOKS at sub_4BE238 (draw bucket dispatch) starts the second context's commands with the
  default render state (sub_4D7B98): otherwise it inherits depth testing off from the first view's post effects.
- The game copies each view's scene into the display buffer (from row 8) and runs both views' post effects only
  after both copies, so the second copy lands on the first. With `gfx::g.split_views` the native renderer keeps
  each copy as a surface of its own, puts each in its own half, and routes the post effects' reads to the right
  copy (`vkn_capture.cpp`, `vc_*`). The legacy renderer has no such fix.
- Player 2's hero is player index 0 like player 1's, so their camera lookups (sub_658E48) go through a HOOK that
  hands back a slot naming the second view; the game hides its HUD whenever player 2's slot names another view, so
  outside that lookup the slot names view 0. The quick select reads player 1's controller: the host routes player
  2's pad there while player 2 has it open (`route_pads`) and hands the chosen weapon over (`hand_over_weapon`).
- Foliage wind: sub_4E8D18 uploads each foliage material's sway table from per-frame memory that has been handed
  out again by the time the second view is built (trees in the right view streaked). While the screen is split, a HOOK keeps this frame's
  valid table and points the upload's source register at that copy (`on_sway`).
- Level changes: a HOOK in sub_2D16A0 removes the second view (RemoveView, sub_69E48) when a level change goes
  ahead, since a load that ends with the second view still in place leaves the main thread waiting in the flip.
  Not every trip goes through it, so player 2 is also stopped and forgotten once the game's current level
  (`cur_level`, the one sub_2D16A0 compares with) differs from the one they were spawned on: their stale hero hung
  the collision jobs on the new planet otherwise. (Opening another level's level_cached.psarc is no sign of a trip:
  Kerwan reads Cobalia's.)
- HUD: the game draws its HUD widget tree (sub_65E898, from sub_661680) once over the whole picture. While split,
  a HOOK draws it a second time first: each top-level element (sub_65CD00) is placed in the half through the UI's
  pixel scale and offset at 0x10724BF4, at the same reduced size (`hudmid`, 0.6) for all:
  edge elements keep their scaled distance to the half's nearest edge, mid-screen ones are centred, and one that
  spans the screen is squeezed. The weapon element (vtable 0x84CA40, one 0xCFC block with
  its children) is swapped for player 2's own copy for the right half and updated (sub_65CEE8) with the interface's
  player slot (+0x5B4FC) pointing at player 2; the host shows that copy when player 2 switches weapons.
- Pause: start on player 2's pad (while the game runs) opens the pause menu for player 2 (`pause_pads`): player
- Split frames where only one view copied its scene (`vkn: split screen: ...` in the log) left the right half
  as it was, and the second view's post effects piled up on it (the glow that keeps frames): the right half now
  starts each frame from the second view's last scene copy (`vkn_capture.cpp`).
- Known problem: player 2 coming back after a death or the game rebuilding the heroes (a skin change) can crash
  in the draw buckets (sub_5DEC50 via sub_4BE5C4, a command buffer whose write pointer reads 1.0f). The game's
  respawn removes the second view itself; keeping our view across it did not help (the view is gone).
  2's pad drives player 1's controller until the game runs again, i.e. until player 1's hero updates within a few
  flips (`note_flip`). `coop::pause_player()` is 2 meanwhile; the skin menu then dresses player 2 (skins.cpp).
  A notice says whose pause it is. Headless: `!co pad2 ...` also works while paused (polled at each flip).
- Known limits: cutscenes and vehicles only know player 1. It costs a second scene render per frame.
- Test commands: `!co spawn`, `!co unview`, `!co rules` (health, weapons, positions), `!co tp 0|1`, `!co eq <player> <weapon>`,
  `!co hurt <player> <amount>`, `!co hud 0|1|3`, `!co hudtree`, `!co pad2 lx ly rx ry b1 b2` (hold player 2's controller), `!passlog` (the native
  renderer logs one frame's targets), `TOD_VKN_SHOTSURF=<hex>` (F12 saves that surface instead).

Status: New Game reaches Kerwan and plays, with NextSynth audio and Bink movies (frames and sound).

## Renderer (Vulkan)

- `rsx.cpp` / `rsx_state.h`: the command processor also captures graphics state for the renderer
  (transform program and constants, immediate-mode vertices, draw ranges, inline transfers).
- `rsx_iface.h`: the fixed contract between the shader translators and the backend.
- `rsx_vp.cpp`, `rsx_fp.cpp`: RSX vertex/fragment microcode to GLSL 450, compiled at runtime with shaderc.
- `rsx_tex.cpp`: CPU texture decode (all RSX formats, swizzle, DXT, cube, 3D) and channel remap.
- `vk_backend.cpp`: Vulkan 1.3 (dynamic rendering, push descriptors); surfaces keyed by guest address,
  render-to-texture by sampling surfaces directly, presentation to an SDL window or headless.
- `window.cpp`: SDL2 window, keyboard/gamepad to cellPad, cellAudio to SDL audio.
- `pad_script.cpp`: `TOD_PAD_SCRIPT=start@1500+20,lsup@1600+40,...` scripted presses (and full stick pushes: `lsup`, `lsleft`, `rsdown`, ...) for headless tests.
- Debug env vars: `TOD_DUMP_FRAMES=dir[,every_n]`, `TOD_DUMP_SHADERS=dir`, `TOD_DUMP_GLSL=1`,
  `TOD_VK_VALIDATE=1`, `TOD_GPU=<name substring>`.
- Draw ranges that continue each other are joined before drawing (a draw method carries at most 256
  vertices, so long strips and lists arrive split; drawing the pieces separately broke skinned characters).
- Surfaces are keyed by address and format, so passes that alternate formats at one address keep both.
- Main-memory offsets ignore bit 31 (Bink sets it on its YUV plane textures).
- Texture reads apply the RSX format conversions the game relies on (`FpKey::tex_gamma` / `tex_expand`): sRGB
  decode for ADDRESS gamma bits (the env/reflection cube maps) and signed expansion for UNSIGNED_REMAP = BIASED
  (normal maps; the game never uses bx2). Without them the image was blown out. The sRGB framebuffer switch
  (SHADER_PACKER 0x1fec) is also honoured, though this game leaves it off.
- Debug: `TOD_VP_ATTRS=1` (vertex layouts, skinning checks), `TOD_TEX_TRACE=<frame>` (texture units).
- Antialiased render targets are rendered supersampled (2x1 or 2x2), because the game reads them back as
  textures at sample resolution. NV3089 scaled copies from a rendered surface are GPU blits.
- Build with `-DTOD_RENDERER=ON` (needs Vulkan, shaderc, SDL2). `packaging/AppRun` is the AppImage launcher.

### Native renderer (default)

`renderer = native` in settings.ini (or `TOD_RENDERER=native|legacy`). `vkn_capture.cpp` runs on the RSX
command thread: for each draw, clear, blit and flip it writes a packet (registers changed since the last
packet, programs and textures the render thread has not seen, the draw itself) and copies what the packet
needs from guest memory into a 256 MB host-visible Vulkan buffer (the data ring): raw big-endian vertex
arrays, byte-swapped indices, constants, changed textures. `vkn_render.cpp` replays the packets on its own
thread. Vertex shaders pull attributes straight from the ring (`kVertexPullGlsl`), so no CPU vertex
conversion; textures are decoded on the render thread.

- Ring data may be referenced again (cached constants) only if it was written after the last flip or
  ring submit: the render thread releases ring space up to those points, so older data can be overwritten
  before a later packet that names it runs (this was the wrong-colours bug).
- At most 2 frames in flight; `P_SUBMIT` asks the render thread to flush when the ring fills.
- Shaders and pipelines build on background threads (`async_shaders = true`): a draw whose pipeline isn't
  ready is skipped. Frame dumps turn this off unless `TOD_ASYNC_SHADERS=1`.
- Disk caches in `<data>/cache/`: SPIR-V per GLSL text (`spirv/`) and the Vulkan pipeline cache
  (`pipelines.bin`, saved at most every 20 s).
- `resolution_scale = 1..4` (or `TOD_RES_SCALE`) renders surfaces at that multiple; the fragment shader
  divides gl_FragCoord by it where the game's shaders read pixel positions.
- Debug env: `TOD_VKN_SYNC=1` (no render thread), `TOD_VKN_CHECK=1` (verify ring copies),
  `TOD_VKN_REHASH=1`, `TOD_VKN_NOCACHE=1`, `TOD_VKN_NODRAW=1` (CPU-side benchmark), `TOD_VKN_SURFSIG=1`.
- Shadow maps: a depth texture format with a depth function (ADDRESS bits 28-31) strictly between NEVER and ALWAYS
  samples as `sampler2DShadow` with a compare sampler (RSX compares texel OP reference, so the ordered functions are
  swapped for Vulkan; outside a clamped-to-border map reads as unshadowed). The game renders its per-object shadow
  maps (DEPTH16, 64-128 px) with a dummy colour target at the same address, so a depth texture format looks up the
  depth surface first. `soft_shadows` takes 9 taps instead of the hardware 2x2.
- Depth read as colour: a D24S8 surface sampled as A8R8G8B8/D8R8G8B8 (the soft-particle depth fade reads the scene
  depth this way) is rebuilt in the shader from the depth value: A = bits 23-16, R = 15-8, G = 7-0, B = stencil (0),
  then the CONTROL1 remap (`FpKey::depth_rgba`).
- Presenting: the display surface is drawn into the window by a full-screen pass (`post_process`): at resolution
  scale 2-4 it averages the source pixels under each window pixel (supersampling), otherwise it applies FXAA when
  `antialiasing = fxaa`. Frame dumps (`TOD_DUMP_FRAMES`) go through the same pass at 1280x720. `TOD_AA=0|1` and
  `TOD_RES_SCALE` override the settings. A resolution change from the menu drops all surfaces at the next flip.
- Feedback loops: a draw that samples the colour surface it renders into (the Zoni's glow, Clank's time slow-down)
  is undefined in Vulkan and came out black on some drivers; such draws now sample a copy of the surface taken
  just before the draw (`feedback_view`). `TOD_NO_FEEDBACK_COPY=1` turns this off.
- Surfaces nobody has drawn to or sampled for 3600 frames are freed (checked every 600 frames), so per-object
  shadow maps at changing addresses no longer pile up (over 1,200 surfaces after a long session before).
- With more than 512 surfaces both sides drop the ones unused for about 2 s (100 frames capture side, 120 render
  side, checked every 30): some effects render to a new small surface at a new address every frame, and 13,700 of
  them ran a session out of GPU memory.
- The capture side forgets surfaces unused for 3000 frames (before the render thread frees them, so a texture
  there reads guest memory again), and remembers which colour surface holds an address (`containing`) until a
  surface comes, goes or grows: split screen asks for every texture it binds, and searching all surfaces each
  time made frames slower as shadow-map surfaces piled up.
- Textures the renderer can't decode are logged once each (`texture not drawn`, up to 24) with address, size,
  pitch, format and reason.
- `TOD_SAMPLE_TRACE=1` logs every distinct render-to-texture binding (unit, surface kind, format, address, filter), depth textures and shadow map sizes.
- A/B switches for the shadow and smoke fixes: `TOD_ZCMP_NOSWAP=1` (depth compare not reversed), `TOD_SHADOW_NOBORDER=1` (shadow lookups may read past a smaller map in a reused surface), `TOD_WPOS_RAW=1` (WPOS in supersampled samples, the old behaviour that broke soft particles).
