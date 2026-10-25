// Split-screen co-op (experimental): a second Ratchet driven by a second controller.
//
// The engine is Insomniac's PS3 engine, which can render several views and keeps a player table,
// but this game reserved room for one player. The hero manager (static object at 0x101D5C30) holds
// one 0x58-byte player slot at +8 (count at +0x60): +0x10 view handle, +0x18 controller, +0x20 one
// moby handle per hero kind (Ratchet = kind 0). The hero itself is the update class (type 529) of a
// class-60 moby and is a heap object: player index at +0x20, kind at +0x24, its moby at +0x14.
//
// A second hero is spawned like any moby (sub_68EC88). Whenever it runs (its update, slot +0xC of the
// hero vtable), player slot 0 is swapped for player 2's copy, so code that asks for "player 0" sees
// player 2's controller and view. Lookups of player slot 1 (the frame loop asks for the player of
// view i) go to player 2's slot, which lives in guest memory.
//
// Split screen uses the engine's own 2-view layout (view 0 top half, view 1 bottom half). The second
// view gets a render context the level load never set up, so its draw list and buffers are set up
// here, and its command stream starts with the default render state (see on_dispatch). The native
// renderer keeps each view's copy of its scene (gfx::g.split_views, vkn_capture.cpp), because the
// game copies both views to the same place before either view's post effects run.
//
// Shared: the inventory (weapons, ammo, bolts, XP) is one global object both heroes point at. Health
// is per hero and is kept equal here; the players can't hold the same weapon (the wrench excepted);
// D-pad Down teleports a player to the other (window.cpp sends the presses).
#include "coop.h"

#include "cheats.h"
#include "gfx.h"
#include "overlay_notify.h"
#include "input.h"
#include "rsx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace coop {
void sway_forget();
void hud_forget();

namespace {
void remove_view();

constexpr u32 kHeroMgr = 0x101D5C30, kSlot0 = kHeroMgr + 8, kSlotSize = 0x58;
constexpr u32 kPadMgr = 0x10675610, kCtrlBase = kPadMgr + 0x288, kCtrlSize = 0x64, kCtrlClaim = 0x10724C18;
constexpr u32 kSpawnMoby = 0x68EC88;           // (class index u16, update type, pos*, rows*, f1 scale) -> moby
constexpr u32 kHeroUpdateType = 529;
constexpr u32 kHeroVtUpdate = 0x84A404;        // hero vtable slot holding the update descriptor
constexpr u32 kHeroUpdateCode = 0x1E5C28;

std::mutex g_lock;
std::deque<std::string> g_cmds;
PadState g_test_pad2;
bool g_test_pad2_held = false;

u32 g_scratch = 0;
u32 g_hero2 = 0, g_moby2 = 0;
u32 g_slot2_ea = 0;  // player 2's slot, in guest memory so per-view lookups of slot 1 can use it
u8* g_slot2 = nullptr;
bool g_have_slot2 = false;
u32 g_orig_update_opd = 0;
std::atomic<u64> g_updates1{0}, g_updates2{0};
bool g_split = true;
u32 g_ctx1 = 0;   // the second view's render context
u32 g_spawn_m1 = 0;   // player 1's moby when player 2 was spawned

// code address -> TOC, from the function descriptors in the image
std::unordered_map<u32, u32>& toc_map() {
    static std::unordered_map<u32, u32> m;
    if (m.empty()) {
        static const u32 tocs[] = {0x88FF38, 0x89FF20, 0x8AFE5C};
        for (u32 a = 0x840000; a < 0x8AD4E8; a += 4) {
            const u32 code = ld32(mem::base, a), toc = ld32(mem::base, a + 4);
            if (code >= 0x10200 && code < 0x83A50C && !(code & 3) && (toc == tocs[0] || toc == tocs[1] || toc == tocs[2])) m.emplace(code, toc);
        }
    }
    return m;
}
u32 toc_for(u32 code) { auto it = toc_map().find(code); return it == toc_map().end() ? 0x88FF38 : it->second; }

u64 gcall(u32 code, std::initializer_list<u64> args) { return cheats::call_code(code, toc_for(code), args); }

void swap_slot() {
    u8 tmp[kSlotSize];
    std::memcpy(tmp, mem::base + kSlot0, kSlotSize);
    std::memcpy(mem::base + kSlot0, g_slot2, kSlotSize);
    std::memcpy(g_slot2, tmp, kSlotSize);
}

// Player 2's slot names the second view (+0x10) only while they are activated (their camera is made
// for it). After that it names view 0, as in one-screen co-op: the game hides its HUD and quick
// select whenever a player's slot names a view other than the first. Player 2's hero finds its own
// view through on_hero_view.
constexpr u32 kSlotView = 0x10;
u64 g_view2 = 0;
void set_slot2_view(u64 h) { for (int i = 0; i < 8; ++i) g_slot2[kSlotView + i] = u8(h >> (56 - 8 * i)); }
void slot2_view_main() { if (g_view2) set_slot2_view(ld64(mem::base, kSlot0 + kSlotView)); }
bool g_in_p2 = false;   // player 2's slot is swapped into slot 0 (their update, activation)

void dump(const char* what, u32 a, u32 n) {
    for (u32 o = 0; o < n; o += 16)
        LOG_NOTE("coop: %s %08x: %08x %08x %08x %08x", what, a + o, ld32(mem::base, a + o), ld32(mem::base, a + o + 4), ld32(mem::base, a + o + 8),
                 ld32(mem::base, a + o + 12));
}

constexpr u32 kHeroPosFields[] = {0x1250, 0x1260, 0x1270, 0x1280, 0x1AA0};   // as the cheat menu's position restore
constexpr u32 kHeroVelocity = 0x1330;

float getf(u32 a) { const u32 v = ld32(mem::base, a); float f; std::memcpy(&f, &v, 4); return f; }
void setf(u32 a, float f) { u32 v; std::memcpy(&v, &f, 4); st32(mem::base, a, v); }

// Teleport to the partner: the position is held for a few updates, or the hero's own move undoes it.
std::atomic<bool> g_teleport_req[2];
int g_teleport_frames[2] = {0, 0};
float g_teleport_to[2][3];

void apply_teleport(u32 hero, int i) {
    if (g_teleport_frames[i] <= 0) return;
    --g_teleport_frames[i];
    for (u32 f : kHeroPosFields) for (int k = 0; k < 3; ++k) setf(hero + f + 4 * k, g_teleport_to[i][k]);
    for (int k = 0; k < 3; ++k) setf(hero + kHeroVelocity + 4 * k, 0.f);
}

// The player's interface (slot +0x50: the HUD and the quick select, one object in the HUD
// singleton) takes its input from the hero updates and reads player 1's controller. With both heroes
// holding it, player 2's update closed the quick select player 1 opened, so player 2's hero runs
// without it; player 2 uses player 1's quick select instead (route_pads, hand_over_weapon).
constexpr u32 kSlotIface = 0x50;
u32 g_iface = 0;
std::atomic<bool> g_tri[2];

void set_slot2_iface(u32 v) { for (int i = 0; i < 4; ++i) g_slot2[kSlotIface + i] = u8(v >> (24 - 8 * i)); }

// The hero's update, called through its vtable: player 2's hero runs with player 2's slot.
u32 g_old_hero2 = 0;   // player 2's hero before forget(keep_view)
std::atomic<int> g_pause2{0};   // player 2's pause menu: 1 opening, 2 open (pause_pads)
std::atomic<bool> g_leaving{false};   // a level change dropped the second view; player 2 waits for the new level
// The level the game is on (what sub_2D16A0 compares a requested level with), and the one player 2
// was spawned on: player 2 is forgotten, and their stale hero kept still, once they differ.
u32 cur_level() {
    static const u32 toc = toc_for(0x2D16A0);
    const u32 p = ld32(mem::base, toc - 0xD98);
    return p ? ld32(mem::base, p + 0x906E8) : ~0u;
}
std::atomic<u32> g_spawn_level{~0u};

void hero_update_thunk(PPUContext& c, u8* base) {
    const u32 self = u32(c.r[3]);
    const bool p2 = self && self == g_hero2 && g_have_slot2;
    if (p2 && (g_leaving || cur_level() != g_spawn_level)) return;
    // a player 2 hero forgotten in this level, still in the update list: left alone (run as an
    // ordinary hero it asked for a view through player 1's slot and crashed in sub_639548)
    if (self && self == g_old_hero2) return;   // with no view of their own, their camera work would go to player 1's
    if (++(p2 ? g_updates2 : g_updates1) == 1) LOG_NOTE("coop: hero 0x%08x updated from 0x%08x", self, u32(c.lr));
    if (p2) {
        set_slot2_iface(0);
        swap_slot();
        g_in_p2 = true;
    }
    try {
        ppu_lookup(kHeroUpdateCode)(c, base);
    } catch (...) {
        if (p2) { g_in_p2 = false; swap_slot(); set_slot2_iface(g_iface); }
        throw;
    }
    if (p2) {
        g_in_p2 = false;
        swap_slot();
        // outside player 2's update their slot keeps the interface: the frame loop looks it up
        // for the second view, and without one the HUD stays hidden
        set_slot2_iface(g_iface);
    }
    if (g_have_slot2) apply_teleport(self, p2 ? 1 : 0);
}

// The hero's other per-frame virtual (vtable slot before the update, sub_1E33C8: its ground probe
// against the moby under it). After a planet trip player 2's hero is still in the moby list for a
// frame or two, until manage() (script tick) sees the new level, and its probe hit a moby of the old
// level whose class memory the new level had reused: guest fault in sub_69C400. Skipped then, as the
// update is.
constexpr u32 kHeroVtProbe = 0x84A400;
u32 g_orig_probe_opd = 0;
void hero_probe_thunk(PPUContext& c, u8* base) {
    const u32 self = u32(c.r[3]);
    const bool stale = self && ((self == g_hero2 && (g_leaving || cur_level() != g_spawn_level)) || self == g_old_hero2);
    if (stale) {
        static u32 logged = 0;
        if (logged != self) { logged = self; LOG_NOTE("coop: player 2's hero 0x%08x left out of the moby update (level %d, spawned on %d)", self, int(cur_level()), int(g_spawn_level.load())); }
        return;
    }
    ppu_lookup(ld32(base, g_orig_probe_opd))(c, base);
}

void install_update_wrapper() {
    if (!g_orig_probe_opd) {
        g_orig_probe_opd = ld32(mem::base, kHeroVtProbe);
        const u32 opd = mem::alloc_main(16, 16);
        st32(mem::base, opd, ppu_register_thunk(hero_probe_thunk));
        st32(mem::base, opd + 4, ld32(mem::base, g_orig_probe_opd + 4));
        st32(mem::base, kHeroVtProbe, opd);
    }
    if (g_orig_update_opd) return;
    g_orig_update_opd = ld32(mem::base, kHeroVtUpdate);
    const u32 opd = mem::alloc_main(16, 16);
    st32(mem::base, opd, ppu_register_thunk(hero_update_thunk));
    st32(mem::base, opd + 4, ld32(mem::base, g_orig_update_opd + 4));
    st32(mem::base, kHeroVtUpdate, opd);
    LOG_NOTE("coop: hero update wrapped (descriptor 0x%08x -> 0x%08x)", g_orig_update_opd, opd);
}

// The hero's "take control" virtual (vtable +0x48) binds its input to the player's controller. A
// spawned moby also starts with flag 0x2 at +0x68, which keeps it out of the moby update list.
void activate() {
    st32(mem::base, g_moby2 + 0x68, ld32(mem::base, g_moby2 + 0x68) & ~2u);
    if (g_view2) set_slot2_view(g_view2);
    swap_slot();
    g_in_p2 = true;
    gcall(0x1E3A30, {g_hero2});
    g_in_p2 = false;
    swap_slot();
    slot2_view_main();
    LOG_NOTE("coop: hero 2 activated, input ctrl 0x%08x index %d", ld32(mem::base, g_hero2 + 0x1030), int(ld32(mem::base, g_hero2 + 0x1070)));
}

// CreateView (sub_680D0) takes a free view (4 at 0x101B0050, 0x21D0 bytes each) and adds it to the
// view list the frame renders. Its camera is set up when the hero is activated with this view.
// The view manager's InitAllViews (vtable +0x18, sub_69F28) runs at level load: per view it sets the
// camera factory table (sub_6393F0) and the render state (sub_68500 -> sub_63A410: post effects at
// +0x2138, render context at +0x213C, and the viewport from the split layout for view i of n,
// vtable +0x28: 2 views -> layout 1 (top half) and 2 (bottom half), applied by sub_694778 to
// context +0x1A84). The same is done here for the new view, and view 0 moves to the top half.
constexpr u32 kViews = 0x101B0050, kViewSize = 0x21D0;
u64 create_view() {
    int n = int(gcall(0x68040, {}));
    const u32 v0 = kViews;
    u32 v1;
    u64 h;
    if (n >= 2) {
        // still there from before a level load, which set it up again
        n = 2;
        v1 = u32(gcall(0x67FE0, {1}));
        h = ld64(mem::base, v1 + 0x2130);
        LOG_NOTE("coop: second view 0x%016llx at 0x%08x kept from before", (unsigned long long)h, v1);
    } else {
        gcall(0x680D0, {g_scratch + 0x100});
        h = ld64(mem::base, g_scratch + 0x100);
        n = int(gcall(0x68040, {}));
        v1 = u32(gcall(0x67FE0, {u64(n - 1)}));
        LOG_NOTE("coop: new view handle 0x%016llx at 0x%08x, %d views", (unsigned long long)h, v1, n);
        gcall(0x6393F0, {v1, 0x10060198, 0x20});
        gcall(0x68500, {v1, u64(n - 1), u64(n), 0});
    }
    st64(mem::base, g_scratch + 0x100, h);   // read back by on_hero_view
    const u32 ctx0 = ld32(mem::base, v0 + 0x213C), ctx1 = ld32(mem::base, v1 + 0x213C);
    st32(mem::base, ctx1 + 0x13BC, ld32(mem::base, ctx0 + 0x13BC));
    {
        // the level load only set up the contexts that were active then; do what 0x693118 does for this
        // one, every time: a respawn after a death resets the contexts in the list, and this one was out of it
        static u32 drawbuf = 0;
        if (!drawbuf) drawbuf = mem::alloc_main(0x2B88 * 16 + 0x80, 0x80);
        // empty, as the first time: the draw records the view left here when it was last removed
        // were dispatched again on its first frame back and crashed on objects freed since
        // (sub_5DEC50, a write pointer reading 1.0f)
        std::memset(mem::base + drawbuf, 0, 0x2B88 * 16 + 0x80);
        const u32 a = g_scratch + 0x200;
        st32(mem::base, a, drawbuf); st32(mem::base, a + 4, 0x2B88 * 16 + 0x80); st32(mem::base, a + 8, 0); st32(mem::base, a + 0xC, 0);
        mem::base[a + 0x10] = 0;
        gcall(0x4BEB10, {ctx1, u64(n - 1), 0x2B88, a});
        gcall(0x599B20, {ctx1 + 0x1580});
        st32(mem::base, ctx1 + 0x10, ctx1 + 0x1380);
        st32(mem::base, ctx1 + 0x14, ctx1 + 0x1700);
    }
    // The render context list (0x1072A780: count +0x2C, contexts from +0x1C; sub_694268 adds,
    // sub_694310 removes) keeps a context after its view is removed, and a view made again (after a
    // level change) gets a new one: the frame loop then walked three contexts, and the draw lists
    // (table at 0x100AF740 by list position, count at 0x10617B60) had nothing for the third.
    // Keep the list at the two views' contexts, and the draw list table in step with it.
    constexpr u32 kCtxList = 0x1072A780, kDrawCtxTable = 0x100AF740, kDrawCtxCount = 0x10617B60;
    for (int k = int(ld32(mem::base, kCtxList + 0x2C)) - 1; k >= 0; --k) {
        const u32 x = ld32(mem::base, kCtxList + 0x1C + 4 * k);
        if (x != ctx0 && x != ctx1) { LOG_NOTE("coop: render context 0x%08x dropped from the list", x); gcall(0x694310, {kCtxList, x}); }
    }
    gcall(0x694268, {kCtxList, ctx1});
    for (u32 k = 0, m = std::min(ld32(mem::base, kCtxList + 0x2C), 4u); k < m; ++k) {
        const u32 x = ld32(mem::base, kCtxList + 0x1C + 4 * k);
        if (k < ld32(mem::base, kDrawCtxCount)) st32(mem::base, kDrawCtxTable + 4 * k, x);
        else gcall(0x4F4060, {x});
    }
    // this frame's begin-frame pass (sub_6931C8 -> sub_4BEA08: draw list reset, bucket handlers) ran before the view existed
    gcall(0x4BEA08, {ctx1});
    st32(mem::base, ctx1 + 0x10, ctx1 + 0x1380);
    st32(mem::base, ctx1 + 0x14, ctx1 + 0x1700);
    // side by side: layouts 3 (left half) and 4 (right half) of sub_694778
    gcall(0x693E80, {ctx0, 3});
    gcall(0x693E80, {ctx1, 4});
    st32(mem::base, v1 + 0x21A0, kCtrlBase + kCtrlSize);
    st32(mem::base, v1 + 0x21A4, ld32(mem::base, v0 + 0x21A4));
    st32(mem::base, v1 + 0x21A8, ld32(mem::base, v0 + 0x21A8));
    LOG_NOTE("coop: view render contexts 0x%08x 0x%08x", ctx0, ctx1);
    {   // diagnostics for the respawn crash in the draw buckets
        const u32 nl = ld32(mem::base, kCtxList + 0x2C);
        LOG_NOTE("coop: context list (%u): %08x %08x %08x; draw table (%u): %08x %08x %08x", nl, ld32(mem::base, kCtxList + 0x1C), ld32(mem::base, kCtxList + 0x20),
                 ld32(mem::base, kCtxList + 0x24), ld32(mem::base, kDrawCtxCount), ld32(mem::base, kDrawCtxTable), ld32(mem::base, kDrawCtxTable + 4), ld32(mem::base, kDrawCtxTable + 8));
        for (u32 cx : {ctx0, ctx1}) dump("ctx", cx, 0x20);
    }
    gfx::g.split_views = true;
    g_ctx1 = ctx1;
    return h;
}

void spawn() {
    const u32 moby1 = cheats::hero_moby();
    if (!moby1) { LOG_ERR("coop: no hero yet"); return; }
    if (g_hero2) { LOG_NOTE("coop: player 2 already spawned"); return; }
    if (!g_scratch) {
        g_scratch = mem::alloc_main(0x1000, 0x100);
        g_slot2_ea = g_scratch + 0x400;
        g_slot2 = mem::base + g_slot2_ea;
    }
    install_update_wrapper();
    const u32 pos = g_scratch, rows = g_scratch + 0x10;
    // next to player 1, same orientation
    std::memcpy(mem::base + rows, mem::base + moby1, 0x30);
    std::memcpy(mem::base + pos, mem::base + moby1 + 0x30, 16);
    u32 x = ld32(mem::base, pos);
    float fx; std::memcpy(&fx, &x, 4); fx += 1.5f; std::memcpy(&x, &fx, 4); st32(mem::base, pos, x);
    const u32 cls = ld16(mem::base, moby1 + 0x50);
    // hide player 1 from the "is there a Ratchet already" check so the new hero initialises fully
    const u64 kind0 = ld64(mem::base, kSlot0 + 0x20);
    u8 saved[kSlotSize];
    std::memcpy(saved, mem::base + kSlot0, kSlotSize);
    st64(mem::base, kSlot0 + 0x20, 0);
    const u32 moby2 = u32(cheats::call_code_f1(kSpawnMoby, toc_for(kSpawnMoby), {cls, kHeroUpdateType, pos, rows}, 1.0));
    LOG_NOTE("coop: spawn class %u -> moby 0x%08x on level %d", cls, moby2, int(cur_level()));
    std::memcpy(g_slot2, mem::base + kSlot0, kSlotSize);
    std::memcpy(mem::base + kSlot0, saved, kSlotSize);
    st64(mem::base, kSlot0 + 0x20, kind0);
    if (!moby2) return;
    g_moby2 = moby2;
    g_hero2 = ld32(mem::base, moby2 + 0x40);
    g_old_hero2 = 0;
    LOG_NOTE("coop: hero 2 at 0x%08x (player %d kind %d), hero 1 at 0x%08x", g_hero2, ld32(mem::base, g_hero2 + 0x20), ld32(mem::base, g_hero2 + 0x24),
             ld32(mem::base, moby1 + 0x40));
    // player 2 reads controller slot 1
    const u32 ctrl1 = kCtrlBase + kCtrlSize;
    u32 be = ctrl1;
    // g_slot2 is host memory in guest byte order
    g_slot2[0x18] = u8(be >> 24); g_slot2[0x19] = u8(be >> 16); g_slot2[0x1A] = u8(be >> 8); g_slot2[0x1B] = u8(be);
    mem::base[kCtrlClaim + 1] = 1;
    if (g_split) {
        // kept since player 2 left in this level (forget), unless the game reset the views meanwhile
        // (a death does): then the handle names nothing and activating the hero with it crashed
        if (g_view2 && (int(gcall(0x68040, {})) < 2 || ld64(mem::base, u32(gcall(0x67FE0, {1})) + 0x2130) != g_view2)) {
            LOG_NOTE("coop: the kept view is gone");
            remove_view();
        }
        if (!g_view2) g_view2 = create_view();
        set_slot2_view(g_view2);
    }
    g_iface = ld32(mem::base, kSlot0 + kSlotIface);
    g_have_slot2 = true;
    g_spawn_m1 = moby1;
    g_spawn_level = cur_level();
    activate();   // with the interface (taking control needs it); the hero updates then pass it around
}

// ---- the co-op rules, run once per frame ---------------------------------------------------
constexpr u32 kHealthComponent = 0x6C;   // component of the hero (sub_68FFA0): +4 health, +8 maximum (floats)
constexpr u32 kWeaponMgr = 0x1E00;       // hero: weapon manager; sub_466EC0 = equipped weapon, vtable +0x1C equips one
constexpr u32 kHeroPos = 0x1AA0;

int g_last_weapon[2] = {-2, -2};

u32 hero1() { const u32 m = cheats::hero_moby(); return m ? ld32(mem::base, m + 0x40) : 0; }

// Each player has their own health; the maximum (which grows with the shared XP) is player 1's.
// When it grows, player 2 gains the same amount, as player 1 does on a level-up.
void share_max_health(u32 h1, u32 h2) {
    const u32 c1 = u32(gcall(0x68FFA0, {h1, kHealthComponent})), c2 = u32(gcall(0x68FFA0, {h2, kHealthComponent}));
    if (!c1 || !c2) return;
    const float top = std::max(getf(c1 + 8), 1.f), old = getf(c2 + 8);
    float v = getf(c2 + 4);
    if (top > old && old > 0) v += top - old;
    setf(c2 + 8, top);
    setf(c2 + 4, std::min(v, top));
}

int equipped(u32 hero) { return int(s32(u32(gcall(0x466EC0, {hero + kWeaponMgr})))); }
void equip(u32 hero, int weapon, bool p2) {
    const u32 mgr = hero + kWeaponMgr, opd = ld32(mem::base, ld32(mem::base, mgr) + 0x1C);
    if (p2) swap_slot();
    cheats::call_code(ld32(mem::base, opd), ld32(mem::base, opd + 4), {mgr, u64(weapon), 0});
    if (p2) swap_slot();
}

// Both players can't hold the same weapon (the Omniwrench excepted): whoever just switched to the
// partner's weapon goes back to the one they had.
// Player 2's quick select (route_pads): the weapon it equips lands on player 1's hero, so it goes
// to player 2 and player 1 gets back the one they had when it opened.
std::atomic<bool> g_qs2{false};
constexpr u32 kQuickSelectVt = 0x84CAD0;   // the quick select (sub_26E4A0 updates it)
std::atomic<u32> g_qs_widget{0};   // its widget, as on_widget_draw last saw it
void hand_over_weapon(u32 h1, u32 h2) {
    // w1: player 1's weapon as of the last frame before player 2 took the quick select (it pauses
    // the game, so no frame runs between the press and the choice)
    static int w1 = -1, linger = 0;
    const int now1 = equipped(h1);
    if (g_qs2) linger = 60;
    else if (linger > 0) --linger;
    else { w1 = now1; return; }
    if (w1 < 0 || now1 < 0 || now1 == w1) return;
    equip(h1, w1, false);
    equip(h2, now1, true);
    g_last_weapon[0] = w1;
}

void no_shared_weapon(u32 h1, u32 h2) {
    const int w[2] = {equipped(h1), equipped(h2)};
    if (w[0] > 0 && w[0] == w[1]) {
        const int who = w[1] != g_last_weapon[1] ? 1 : w[0] != g_last_weapon[0] ? 0 : -1;
        if (who < 0) return;
        int back = g_last_weapon[who];
        if (back < 0 || back == w[who]) back = 0;
        equip(who ? h2 : h1, back, who == 1);
        overlay_notify("Player %d: your partner has that weapon", who + 1);
        return;
    }
    g_last_weapon[0] = w[0];
    g_last_weapon[1] = w[1];
}

void teleports(u32 h1, u32 h2) {
    for (int i = 0; i < 2; ++i) {
        if (!g_teleport_req[i].exchange(false)) continue;
        const u32 to = i ? h1 : h2;
        for (int k = 0; k < 3; ++k) g_teleport_to[i][k] = getf(to + kHeroPos + 4 * k);
        g_teleport_to[i][0] += 1.0f;   // beside the partner, not inside
        g_teleport_frames[i] = 3;
    }
}

// Player 2 comes and goes with the co-op switch (settings.ini / F1 menu): spawned once a level has
// been running for a moment, and spawned again after every level load (the old one is gone then).
int g_settle = 0;

bool hero2_alive() {
    return g_moby2 && ld16(mem::base, g_moby2 + 0x50) == 60 && ld32(mem::base, g_moby2 + 0x40) == g_hero2 && ld32(mem::base, g_hero2 + 4) == kHeroUpdateType;
}

// Back to one full-screen view (RemoveView is sub_69E48 on the view manager). A level load sets up
// the render state for the views that exist then, so leaving view 1 in place through a load left the
// first frame of the new level waiting forever in the flip (sub_5057B0) for work view 1 never did.
constexpr u32 kViewMgr = 0x101B8798;
void remove_view() {
    if (!g_ctx1) return;
    const u32 v0 = kViews, v1 = kViews + kViewSize;
    if (int(gcall(0x68040, {})) >= 2) gcall(0x69E48, {kViewMgr, v1});
    if (const u32 ctx0 = ld32(mem::base, v0 + 0x213C)) {
        gcall(0x693E80, {ctx0, 0});
        const float full = getf(0x1064F430 + 0x128);
        setf(ctx0 + 0x138C, full);
        setf(ctx0 + 0x170C, full);
        st32(mem::base, v0 + 0x2110, 0);
    }
    gcall(0x694310, {0x1072A780, g_ctx1});   // out of the render context list (see create_view)
    gfx::g.split_views = false;
    g_view2 = 0;
    g_ctx1 = 0;
    LOG_NOTE("coop: back to one view (%d)", int(gcall(0x68040, {})));
}

// keep_view: player 2 is gone in the same level (a death, a checkpoint, the game rebuilding the
// heroes after a skin change) and spawns again soon: the second view stays as it is. Removing it
// and making it again mid-level crashed in the draw buckets on the next frame now and then (a
// command buffer whose write pointer read 1.0f).
void forget(bool keep_view = false) {
    LOG_NOTE("coop: player 2 left%s", keep_view ? ", keeping their view" : " with the level");
    sway_forget();
    hud_forget();
    g_pause2 = 0;
    if (!keep_view || !g_view2) remove_view();
    g_leaving = false;
    g_old_hero2 = keep_view ? g_hero2 : 0;
    g_hero2 = g_moby2 = 0;
    g_have_slot2 = false;
    g_last_weapon[0] = g_last_weapon[1] = -2;
    g_teleport_frames[0] = g_teleport_frames[1] = 0;
    if (!keep_view) g_ctx1 = 0;
}

// sub_2D16A0 past its "already on that level" check: a level change has been requested (ship
// travel, the level warp). The second view has to be gone before the load ends, or the main thread
// waits forever on the render ring at the end of it, so it goes now; player 2 is forgotten (and
// respawned) once the new level's hero exists.
void level_change() {
    if (!g_view2) return;
    LOG_NOTE("coop: level change, dropping the second view");
    remove_view();
    g_leaving = true;
}

void manage() {
    const u32 m1 = cheats::hero_moby();
    const bool new_level = cur_level() != g_spawn_level;
    if (g_hero2 && (new_level || m1 != g_spawn_m1 || !hero2_alive())) {
        LOG_NOTE("coop: player 2 check: level %d (spawned on %d), hero 1 moby %08x (was %08x), moby class %d, moby hero %08x, update type %08x", int(cur_level()), int(g_spawn_level.load()), m1, g_spawn_m1,
                 g_moby2 ? ld16(mem::base, g_moby2 + 0x50) : -1, g_moby2 ? ld32(mem::base, g_moby2 + 0x40) : 0, ld32(mem::base, g_hero2 + 4));
        // keep_view off: a death removes the second view in the game's own respawn anyway
        forget(false);
    }
    if (g_view2 && !g_hero2 && !gfx::g.coop.load()) remove_view();   // co-op switched off before player 2 came back
    if (!m1 || g_hero2 || !gfx::g.coop.load()) { g_settle = 0; return; }
    if (++g_settle < 90) return;   // let the level start
    g_settle = 0;
    spawn();
}

// Each view's projection uses the aspect in its render context's frame parameters (+0xC of the two
// blocks at +0x1380 and +0x1700), set from the screen's at level load. A half-width view needs half
// of it. The projection is rebuilt by sub_639948 only when the view's field of view (+0x2110)
// changes, so that is cleared to make the next camera update rebuild it.
void fix_aspect() {
    if (!g_ctx1) return;
    const float want = getf(0x1064F430 + 0x128) * 0.5f;
    for (u32 i = 0; i < 2; ++i) {
        const u32 ctx = ld32(mem::base, kViews + i * kViewSize + 0x213C);
        if (!ctx || std::fabs(getf(ctx + 0x138C) - want) < 1e-4f) continue;
        setf(ctx + 0x138C, want);
        setf(ctx + 0x170C, want);
        st32(mem::base, kViews + i * kViewSize + 0x2110, 0);
    }
}

std::mutex g_status_lock;
Status g_status;

void note_status(u32 h1, u32 h2) {
    Status st;
    st.active = true;
    const u32 hs[2] = {h1, h2};
    for (int i = 0; i < 2; ++i) {
        if (const u32 cmp = u32(gcall(0x68FFA0, {hs[i], kHealthComponent}))) { st.health[i] = getf(cmp + 4); st.max_health[i] = getf(cmp + 8); }
        st.weapon[i] = equipped(hs[i]);
    }
    std::lock_guard lk(g_status_lock);
    g_status = st;
}

void rules() {
    if (!g_have_slot2 || !g_hero2) { std::lock_guard lk(g_status_lock); g_status.active = false; return; }
    fix_aspect();
    const u32 h1 = hero1();
    if (!h1 || h1 == g_hero2) return;
    share_max_health(h1, g_hero2);
    hand_over_weapon(h1, g_hero2);
    no_shared_weapon(h1, g_hero2);
    teleports(h1, g_hero2);
    note_status(h1, g_hero2);
}

}  // namespace

void filter_pad2(PadState& st) {
    std::lock_guard lk(g_lock);
    if (g_test_pad2_held) st = g_test_pad2;
    g_tri[1] = st.digital2 & 0x10;
}

Status status() {
    std::lock_guard lk(g_status_lock);
    return g_status;
}

// The quick select reads player 1's controller. When player 2 presses triangle (and player 1
// isn't using it), player 2's pad drives player 1's controller until player 2 has left the pad
// alone for a moment or player 1 presses triangle; player 1's own input waits meanwhile.
// Player 2's pause: start on player 2's pad (while the game runs) opens the pause menu for player 2,
// whose pad then drives player 1's controller until the game runs again. "Runs" = player 1's hero
// updated within the last few flips (nothing updates while the game is paused, the menu still
// flips). 1 = opening, 2 = open.
std::atomic<bool> g_game_running{true};
std::atomic<u64> g_flips{0};

void note_flip() {
    static u64 seen = 0;
    static int idle = 0;
    ++g_flips;
    if (const u64 u = g_updates1.load(); u != seen) { seen = u; idle = 0; }
    else ++idle;
    g_game_running = idle < 4;
    // every 10 s: flips where player 1's hero updated and player 2's didn't (a "freeze") go in the log,
    // with the longest run of them
    static u64 last1 = 0, last2 = 0, f1 = 0, f2 = 0;
    static int stuck = 0, run = 0, longest = 0;
    {
        const u64 u1 = g_updates1.load(), u2 = g_updates2.load();
        if (g_hero2 && u1 != f1 && u2 == f2) { ++stuck; longest = std::max(longest, ++run); }
        else run = 0;
        f1 = u1; f2 = u2;
    }
    // the quick select stops the game for both players while it's open (as the game's own option
    // has it by default): off while player 2 is in, the player's choice put back after
    {
        static int saved = -1;
        static const u32 toc = toc_for(0x39BAB0);
        const u32 opts = ld32(mem::base, toc + 0x2BB8), flag = opts + 0x114D2;
        if (opts && mem::is_mapped(flag, 1)) {
            if (g_hero2) {
                if (saved < 0) { saved = mem::base[flag]; LOG_NOTE("coop: quick select pause off for co-op (was %d)", saved); }
                if (mem::base[flag]) mem::base[flag] = 0;
            } else if (saved >= 0) {
                mem::base[flag] = u8(saved);
                saved = -1;
            }
        }
    }
    if (g_flips % 600 == 0) {
        const u64 u1 = g_updates1.load(), u2 = g_updates2.load();
        if (g_hero2 && stuck > 30)
            LOG_NOTE("coop: player 2 didn't update in %d of the last 600 flips (longest run %d; player 1 updated %llu times, player 2 %llu; leaving %d, level %d, spawned on %d)",
                     stuck, longest, (unsigned long long)(u1 - last1), (unsigned long long)(u2 - last2), int(g_leaving.load()), int(cur_level()),
                     int(g_spawn_level.load()));
        last1 = u1; last2 = u2; stuck = 0; longest = 0;
    }
}

int pause_player() { return g_pause2 ? 2 : 1; }

bool pause_pads(PadState& p1, PadState& p2) {
    static u64 since = 0;
    static bool start1_was = false, start2_was = false;
    const bool running = g_game_running;
    const bool start1 = p1.digital1 & 8, start2 = p2.digital1 & 8;
    const bool press1 = start1 && !start1_was, press2 = start2 && !start2_was;
    start1_was = start1; start2_was = start2;
    if (!g_pause2 && running && !g_qs2) {
        if (press2 && !start1) { g_pause2 = 1; since = g_flips; LOG_NOTE("coop: player 2 pauses"); overlay_notify("Player 2's pause"); }
        else if (press1) overlay_notify("Player 1's pause");
    }
    if (g_pause2 == 1 && !running) { g_pause2 = 2; LOG_NOTE("coop: player 2's pause is open"); }
    else if (g_pause2 == 1 && g_flips - since > 120) { g_pause2 = 0; LOG_NOTE("coop: player 2's start didn't pause the game"); }
    // player 1's start takes the pad back (and closes the menu if it is still up)
    else if (g_pause2 && press1) { g_pause2 = 0; LOG_NOTE("coop: player 1 took the pause back"); return false; }
    else if (g_pause2 == 2 && running) { g_pause2 = 0; LOG_NOTE("coop: player 2's pause closed"); }
    if (!g_pause2) return false;
    p1 = p2;
    p2 = PadState{};
    return true;
}

// Player 2's triangle gives player 2 player 1's pad for the quick select, until it closes: it was
// until player 2 left the pad alone for 1.5 s, so player 2 walking off after choosing kept driving
// player 1's hero (and a tap that opened nothing did too). Now it ends once the quick select is
// closed: its widget's state (+0x34, 2 while open) back to 0, a few flips after the press at the
// earliest. With its pause off (co-op turns it off) it is open while triangle is held.
void route_pads(PadState& p1, PadState& p2) {
    static u64 since = 0;
    if (g_have_slot2 && pause_pads(p1, p2)) return;
    const bool tri1 = p1.digital2 & 0x10, tri2 = p2.digital2 & 0x10;
    if (!g_have_slot2) { g_qs2 = false; return; }
    const u64 now = g_flips;
    if (!g_qs2 && tri2 && !tri1) { g_qs2 = true; since = now; LOG_NOTE("coop: player 2 has the quick select"); }
    if (!g_qs2) return;
    const u32 w = g_qs_widget;
    const bool open = w && ld32(mem::base, w) == kQuickSelectVt && ld32(mem::base, w + 0x34) != 0;
    if (tri1 || (!tri2 && !open && now - since > 6)) {
        g_qs2 = false;
        LOG_NOTE("coop: player 2's quick select closed%s", tri1 ? " (player 1's triangle)" : "");
        return;
    }
    p1 = p2;
    p2 = PadState{};
}

void note_pad1(const PadState& st) { g_tri[0] = st.digital2 & 0x10; }

void request_teleport(int player) { if (player == 0 || player == 1) g_teleport_req[player] = true; }

// sub_1E1298 (hero of a player slot) and sub_1E1120 (hero of a slot by kind) at entry: the frame
// loop asks for slot i of view i, and slot 1 would be past the end of the one-slot table.
// The hero asks for its view through its player index (sub_658E48), and player 2's hero is player
// 0 too: outside its own update that is player 1's slot, so player 2's camera work went to the first
// view. create_view keeps the second view's handle at g_scratch + 0x100, i.e. at +0x10 of
// g_scratch + 0xF0.
void on_level_change() { level_change(); }

// The loader opened packed/levels/<name>/level_cached.psarc (fs.cpp): logged only. The game also
// reads other planets' archives while staying put (Kerwan reads Cobalia's), and taking that for a
// level change froze player 2 and spawned another one next to them. Level changes are seen through
// cur_level() (manage, hero_update_thunk) instead.
void on_level_file(const std::string& name) {
    static std::mutex m;
    static std::string last;
    std::lock_guard lk(m);
    if (name == last) return;
    const bool first = last.empty();
    last = name;
    if (first) return;
    LOG_NOTE("coop: level archive %s opened (level %d, player 2 spawned on %d)", name.c_str(), int(cur_level()), int(g_spawn_level.load()));
}

void on_hero_view(PPUContext& c) {
    if (g_view2 && u32(c.r[29]) == g_hero2) c.r[3] = g_scratch + 0xF0;
}

// Foliage wind: sub_4E8D18 uploads a material's 16-pair sway table (c42..c73) from the buffer at
// material+0x14, which the wind update fills once per frame in per-frame memory. The second view
// is built after that memory has been handed out again, so its trees bent by whatever was there
// (long vertical streaks). While the screen is split, keep this frame's table (rotation rows
// (c, s, -s, c), offset rows (x, y, 0, 0)) and have the upload read that copy when the buffer no
// longer holds one. Only the register the upload reads from changes (HOOK at 0x4E8E94, after its
// load of material+0x14); the material itself is left alone.
namespace {
struct Sway { u32 mat = 0, buf = 0, frame = 0; u8 rows[0x200]; bool have = false; };
Sway g_sway[8];
u32 g_sway_src = 0, g_frame = 0;
bool sway_valid(u32 src) {
    if (src < 0x10000 || !mem::is_mapped(src, 0x200)) return false;
    for (u32 r = 0; r < 32; ++r) {
        float v[4];
        for (int k = 0; k < 4; ++k) v[k] = getf(src + r * 16 + k * 4);
        if (r % 2 == 0) {
            if (std::fabs(v[0] - v[3]) > 1e-3f || std::fabs(v[1] + v[2]) > 1e-3f || std::fabs(v[0] * v[0] + v[1] * v[1] - 1) > 0.01f) return false;
        } else if (v[2] != 0 || v[3] != 0 || !(std::fabs(v[0]) < 1) || !(std::fabs(v[1]) < 1)) return false;
    }
    return true;
}
}  // namespace

void on_sway(PPUContext& c, bool load) {
    if (load) {
        if (g_sway_src) c.r[3] = g_sway_src;
        g_sway_src = 0;
        return;
    }
    g_sway_src = 0;
    if (!g_view2) return;
    const u32 mat = u32(c.r[3]), src = ld32(mem::base, mat + 0x14);
    Sway* e = nullptr;
    for (auto& x : g_sway) if (x.mat == mat || !x.mat) { e = &x; break; }
    if (!e) return;
    e->mat = mat;
    if (sway_valid(src)) { std::memcpy(e->rows, mem::base + src, 0x200); e->have = true; e->frame = g_frame; return; }
    if (!e->have || g_frame - e->frame > 1) return;   // only this frame's table
    if (!e->buf) e->buf = mem::alloc_main(0x200, 0x80);
    std::memcpy(mem::base + e->buf, e->rows, 0x200);
    g_sway_src = e->buf;
    static bool noted = false;
    if (!noted) { noted = true; LOG_NOTE("coop: foliage wind table for %08x restored (its buffer at %08x was reused)", mat, src); }
}

void sway_forget() { for (auto& x : g_sway) { x.mat = 0; x.have = false; } }

void on_slot_lookup(PPUContext& c) {
    if (u32(c.r[3]) != kSlot0 + kSlotSize) return;
    if (g_have_slot2) c.r[3] = g_slot2_ea;
    else if (g_view2) c.r[3] = kSlot0;   // the second view kept while player 2 is away (forget) follows player 1
}

// sub_639948 at entry: r3 = a view, f1 = its horizontal field of view in degrees. With the view
// half as wide (fix_aspect) the same horizontal angle would double the vertical one; this keeps the
// vertical angle a little wider than in single player, as the game's own split screen does.
void on_set_fov(PPUContext& c) {
    if (!g_have_slot2 || !g_split || !g_ctx1) return;
    const u32 v = u32(c.r[3]);
    if (v != kViews && v != kViews + kViewSize) return;
    constexpr double kDeg = 3.14159265358979 / 180, kScale = 0.5 * 1.15;
    c.f[1] = float(2 * std::atan(std::tan(c.f[1] * kDeg * 0.5) * kScale) / kDeg);
}

// The game's HUD is one widget tree (the interface, slot +0x50, and its siblings) that sub_661680 draws
// over the whole screen once both views are composited: sub_65E898 walks it from the root, sub_65CD00
// draws a widget and its children. Widgets are laid out in UI units (the screen is 1251.6 x 704 at
// 0x10724BE4) and turned into pixels with the scale and offset at 0x10724BF4 (x offset, x scale,
// y offset, y scale). While the screen is split the tree is drawn twice, once per half: each
// top-level element (a grandchild of the root: the bolt counter, the health bar, the weapon and its
// ammo, the quick select, ...) keeps its distance to the screen edge it sits nearest to inside its
// half, elements in the middle of the screen are centred in the half at a smaller size, and ones as
// wide as the screen are squeezed into the half. Laying
// the tree out again per half doesn't work: elements that moved since the last frame aren't drawn.
namespace {
constexpr u32 kUiRoot = 0x10724BDC;   // the holder sub_661680 passes; +0 = the root
constexpr u32 kUiScreen = 0x10724BE4, kUiXform = 0x10724BF4;
int g_hud_split = 3;   // 1: a HUD per half, 2: player 2's weapon in theirs
int g_hud_pass = -1;   // the half being drawn
float g_ui_xform[4], g_hud_mid_scale = 0.6f;

void hud_tree(u32 w, int depth) {
    for (; w && depth < 16; w = ld32(mem::base, w + 0x44)) {
        LOG_NOTE("coop: hud %*s%08x vt %08x rect %.1f %.1f %.1f %.1f local %.2f %.2f %.2f %.2f mode %d flags %08x", depth * 2, "", w,
                 ld32(mem::base, w), getf(w + 0x24), getf(w + 0x28), getf(w + 0x2C), getf(w + 0x30), getf(w + 4), getf(w + 8), getf(w + 0xC),
                 getf(w + 0x10), mem::base[w + 0x68], ld32(mem::base, w + 0x64));
        hud_tree(ld32(mem::base, w + 0x48), depth + 1);
    }
}

void hud_restore() { for (int i = 0; i < 4; ++i) setf(kUiXform + 4 * i, g_ui_xform[i]); }

// The weapon element (the weapon's icon, level and ammo; sub_26B0E8 updates it) shows the hero of the
// interface's player slot (interface +0x5B4FC, its hero cached at +0x5B4F4). Its children live inside
// it, so the whole element is one block of memory: for the right half it is swapped for player 2's
// copy, updated for player 2's slot (sub_65CEE8 updates a widget and its children), drawn, and swapped
// back, so each player's copy keeps its own state from frame to frame.
constexpr u32 kWeaponVt = 0x84CA40, kWeaponSize = 0xCFC, kIfaceSlot = 0x5B4FC, kIfaceHero = 0x5B4F4;
u32 g_weapon = 0;
bool g_weapon_have2 = false, g_weapon_swapped = false;
u8 g_weapon_mine[kWeaponSize], g_weapon_p2[kWeaponSize];

u32 find_weapon(u32 root) {
    const u32 iface = ld32(mem::base, root + 0x48);
    if (g_weapon && ld32(mem::base, g_weapon) == kWeaponVt && ld32(mem::base, g_weapon + 0x54) == iface) return g_weapon;
    g_weapon = 0;
    g_weapon_have2 = false;
    for (u32 w = iface ? ld32(mem::base, iface + 0x48) : 0, n = 0; w && n < 64; w = ld32(mem::base, w + 0x44), ++n)
        if (ld32(mem::base, w) == kWeaponVt) return g_weapon = w;
    return 0;
}

void weapon_for_p2(PPUContext& c, u32 root) {
    const u32 w = find_weapon(root), iface = ld32(mem::base, root + 0x48);
    if (!w || !g_hero2 || !g_slot2_ea) return;
    std::memcpy(g_weapon_mine, mem::base + w, kWeaponSize);
    if (g_weapon_have2) std::memcpy(mem::base + w, g_weapon_p2, kWeaponSize);
    g_weapon_swapped = true;
    // the game shows the element (sub_65C840) when player 1 switches weapons; player 2's switches
    // don't reach the interface, so show their copy here
    // (and hides it again, sub_65C858, a few seconds later)
    using clock = std::chrono::steady_clock;
    static int shown = -1;
    static clock::time_point since;
    if (const int now = g_last_weapon[1]; now != shown) {
        shown = now;
        since = clock::now();
        st32(mem::base, w + 0x3C, 2);
        st32(mem::base, w + 0x5C, 1);
    } else if (ld32(mem::base, w + 0x3C) == 2 && clock::now() - since > std::chrono::seconds(3)) {
        st32(mem::base, w + 0x3C, 0);
        st32(mem::base, w + 0x5C, 0);
    }
    const u32 mode = ld32(mem::base, kUiRoot + 4), slot = ld32(mem::base, iface + kIfaceSlot), hero = ld32(mem::base, iface + kIfaceHero);
    st32(mem::base, kUiRoot + 4, 0);   // as during the game's own update
    st32(mem::base, iface + kIfaceSlot, kSlot0 + kSlotSize);   // on_slot_lookup makes it player 2's slot
    st32(mem::base, iface + kIfaceHero, g_hero2);
    static const PPUFunc update = ppu_lookup(0x65CEE8);
    PPUContext saved = c;
    c.r[1] = (c.r[1] - 0x400) & ~0xFull;
    c.lr = 0;
    c.r[3] = w;
    update(c, mem::base);
    c = saved;
    st32(mem::base, kUiRoot + 4, mode);
    st32(mem::base, iface + kIfaceSlot, slot);
    st32(mem::base, iface + kIfaceHero, hero);
}

void weapon_back() {
    if (!g_weapon_swapped) return;
    g_weapon_swapped = false;
    std::memcpy(g_weapon_p2, mem::base + g_weapon, kWeaponSize);
    g_weapon_have2 = true;
    std::memcpy(mem::base + g_weapon, g_weapon_mine, kWeaponSize);
}
}  // namespace

void hud_forget() { g_weapon = 0; g_weapon_have2 = g_weapon_swapped = false; }

// sub_65CD00(widget) at entry, while a half's HUD is drawn: place a top-level element in that half.
void on_widget_draw(PPUContext& c) {
    if (g_hud_pass < 0) return;
    const u32 w = u32(c.r[3]), parent = ld32(mem::base, w + 0x54);
    const u32 root = ld32(mem::base, kUiRoot);
    if (!parent || ld32(mem::base, parent + 0x54) != root) return;
    if (ld32(mem::base, w) == kQuickSelectVt) g_qs_widget = w;
    if (ld32(mem::base, w) == kQuickSelectVt && g_hud_pass != (g_qs2 ? 1 : 0)) {
        // one quick select, in the half of the player using it
        setf(kUiXform, -1e5f); setf(kUiXform + 4, 0.f); setf(kUiXform + 8, -1e5f); setf(kUiXform + 12, 0.f);
        return;
    }
    {   // its layout (sub_65C898) as its draw is about to make it: a widget just shown hasn't one yet
        static const PPUFunc layout = ppu_lookup(0x65C898);
        PPUContext saved = c;
        c.r[1] = (c.r[1] - 0x400) & ~0xFull;
        c.lr = 0;
        layout(c, mem::base);
        c = saved;
    }
    const float x0 = getf(kUiScreen), x1 = getf(kUiScreen + 8);
    const float third = (x1 - x0) / 3.f, ax = (getf(w + 0x24) + getf(w + 0x2C)) * 0.5f, ay = (getf(w + 0x28) + getf(w + 0x30)) * 0.5f;
    const float sx = g_ui_xform[1], sy = g_ui_xform[3], ox = g_ui_xform[0], oy = g_ui_xform[2];
    const float half = (x1 - x0) * 0.5f * sx;   // a half's width in pixels
    const float px = ax * sx + ox;               // where the element sits on the whole screen
    float kx = 1.f, ky = 1.f, tx;
    if (getf(w + 0x2C) - getf(w + 0x24) > third) { kx = 0.5f; tx = px * 0.5f + g_hud_pass * half; }   // spans the screen: squeeze it
    else if (ax < x0 + third) {   // keeps to the half's left edge, as small as the middle ones
        const float edge = x0 * sx + ox;
        kx = ky = g_hud_mid_scale; tx = edge + g_hud_pass * half + (px - edge) * kx;
    } else if (ax > x1 - third) {
        const float edge = x1 * sx + ox;
        kx = ky = g_hud_mid_scale; tx = edge - (1 - g_hud_pass) * half - (edge - px) * kx;
    } else { kx = ky = g_hud_mid_scale; tx = px * 0.5f + g_hud_pass * half; }
    setf(kUiXform + 4, sx * kx);
    setf(kUiXform, tx - ax * sx * kx);
    setf(kUiXform + 12, sy * ky);
    setf(kUiXform + 8, oy + ay * sy * (1.f - ky));
}

// sub_65E898(holder, arg) at entry: draw the left half's HUD here and leave the right half's to the
// game's own call; 0x661A14 (after it returns): back to the whole screen.
void on_hud_draw(PPUContext& c, bool after) {
    if (after) {
        if (g_hud_pass < 0) return;
        g_hud_pass = -1;
        hud_restore();
        weapon_back();
        return;
    }
    if (g_hud_pass >= 0 || !g_view2 || !g_split || !g_hud_split || !ld32(mem::base, kUiRoot)) return;
    for (int i = 0; i < 4; ++i) g_ui_xform[i] = getf(kUiXform + 4 * i);
    static const PPUFunc draw = ppu_lookup(0x65E898);
    PPUContext saved = c;   // the draw runs on the render thread: call it on this context
    c.r[1] = (c.r[1] - 0x400) & ~0xFull;
    c.lr = 0;
    g_hud_pass = 0;
    draw(c, mem::base);
    c = saved;
    hud_restore();
    if (g_hud_split & 2) weapon_for_p2(c, ld32(mem::base, kUiRoot));
    g_hud_pass = 1;
}

// sub_4BE238 at entry: r3 = the render context whose draw buckets are about to be sent to the RSX.
// Each context's command stream assumes the default render state (sub_4D7B98: depth test on,
// blending off, ...) at its start, but the second view's follows the first view's post effects,
// which leave depth testing off. Reset the state before every context but the first.
void on_dispatch(PPUContext& c) {
    if (gfx::g.passlog.load(std::memory_order_relaxed) > 0) LOG_NOTE("pass: dispatch ctx %08x from %08x", u32(c.r[3]), u32(c.lr));
    if (!g_have_slot2 || !g_split) return;
    if (u32(c.r[3]) != g_ctx1) return;
    static const PPUFunc reset = ppu_lookup(0x4D7B98);
    if (!reset) return;
    PPUContext saved = c;
    c.r[1] = (c.r[1] - 0x400) & ~0xFull;
    c.lr = 0;
    reset(c, mem::base);
    c = saved;
}

namespace {

void run(const std::string& line) {
    std::stringstream ss(line);
    std::string op;
    ss >> op;
    if (op == "spawn") spawn();
    else if (op == "nosplit") g_split = false;
    else if (op == "unview") level_change();
    else if (op == "qsdump") { LOG_NOTE("coop: quick select widget %08x", g_qs_widget.load()); if (g_qs_widget) dump("qs", g_qs_widget, 0x200); }
    else if (op == "forget") { if (g_hero2) forget(); }
    else if (op == "hud") { int v = 1; ss >> v; g_hud_split = v; }
    else if (op == "hudmid") { float v = 0.6f; ss >> v; g_hud_mid_scale = v; }
    else if (op == "hudtree") hud_tree(ld32(mem::base, kUiRoot), 0);
    else if (op == "dumpview") { u32 v = 0; ss >> std::hex >> v; dump("view", 0x101B0050 + v * 0x21D0 + 0x2080, 0x150); }
    else if (op == "activate") { if (g_hero2) activate(); }
    else if (op == "tp") { int i = 0; ss >> i; g_teleport_req[i & 1] = true; }
    else if (op == "eq") { int i = 0, w = 0; ss >> i >> w; const u32 h = i ? g_hero2 : hero1(); if (h) equip(h, w, i == 1); }
    else if (op == "hurt") {
        int i = 0; float d = 1; ss >> i >> d;
        const u32 h = i ? g_hero2 : hero1();
        if (const u32 cmp = h ? u32(gcall(0x68FFA0, {h, kHealthComponent})) : 0) setf(cmp + 4, getf(cmp + 4) - d);
    }
    else if (op == "rules") {
        const u32 h1 = hero1();
        const u32 c1 = h1 ? u32(gcall(0x68FFA0, {h1, kHealthComponent})) : 0, c2 = g_hero2 ? u32(gcall(0x68FFA0, {g_hero2, kHealthComponent})) : 0;
        LOG_NOTE("coop: health %.1f/%.1f, %.1f/%.1f, weapons %d / %d, positions %.2f %.2f %.2f / %.2f %.2f %.2f", c1 ? getf(c1 + 4) : -1.f, c1 ? getf(c1 + 8) : -1.f,
                 c2 ? getf(c2 + 4) : -1.f, c2 ? getf(c2 + 8) : -1.f, h1 ? equipped(h1) : -9, g_hero2 ? equipped(g_hero2) : -9, getf(h1 + kHeroPos),
                 getf(h1 + kHeroPos + 4), getf(h1 + kHeroPos + 8), getf(g_hero2 + kHeroPos), getf(g_hero2 + kHeroPos + 4), getf(g_hero2 + kHeroPos + 8));
    }
 else if (op == "flags") {
        u32 v = 0; ss >> std::hex >> v;
        if (g_moby2) st32(mem::base, g_moby2 + 0x68, v);
    }
    else if (op == "call") {
        // "call <code> [args...]" (hex)
        u32 code = 0; ss >> std::hex >> code;
        std::vector<u64> a; u64 v;
        while (ss >> std::hex >> v) a.push_back(v);
        u64 r = 0;
        switch (a.size()) {
        case 0: r = gcall(code, {}); break;
        case 1: r = gcall(code, {a[0]}); break;
        case 2: r = gcall(code, {a[0], a[1]}); break;
        case 3: r = gcall(code, {a[0], a[1], a[2]}); break;
        default: r = gcall(code, {a[0], a[1], a[2], a[3]}); break;
        }
        LOG_NOTE("coop: call 0x%x -> 0x%llx", code, (unsigned long long)r);
    } else if (op == "poke") {
        u32 a = 0, v = 0; ss >> std::hex >> a >> v;
        st32(mem::base, a, v);
    } else if (op == "snap") {
        // "snap <addr> <len> <file>" (hex): raw guest memory to a file, for diffing
        u32 a = 0, n = 0; std::string f; ss >> std::hex >> a >> n >> f;
        if (FILE* fp = std::fopen(f.c_str(), "wb")) {
            static const std::vector<u8> zero(0x10000);
            for (u32 o = 0; o < n; o += 0x10000) std::fwrite(mem::is_mapped(a + o, 0x10000) ? mem::base + a + o : zero.data(), 1, 0x10000, fp);
            std::fclose(fp);
        }
    } else if (op == "slots") {
        dump("mgr", kHeroMgr, 0x70);
        if (g_have_slot2) for (u32 o = 0; o < kSlotSize; o += 16) LOG_NOTE("coop: slot2 +%02x: %02x%02x%02x%02x %02x%02x%02x%02x", o, g_slot2[o], g_slot2[o+1], g_slot2[o+2], g_slot2[o+3], g_slot2[o+4], g_slot2[o+5], g_slot2[o+6], g_slot2[o+7]);
        if (g_moby2) dump("moby2", g_moby2, 0x60);
    } else if (op == "stat") {
        auto f = [](u32 a) { float v; u32 b = ld32(mem::base, a); std::memcpy(&v, &b, 4); return v; };
        const u32 m1 = cheats::hero_moby();
        LOG_NOTE("coop: updates p1 %llu p2 %llu; p1 at %.2f %.2f %.2f; p2 at %.2f %.2f %.2f", (unsigned long long)g_updates1.load(),
                 (unsigned long long)g_updates2.load(), f(m1 + 0x30), f(m1 + 0x34), f(m1 + 0x38), f(g_moby2 + 0x30), f(g_moby2 + 0x34), f(g_moby2 + 0x38));
        dump("ctrl0", kCtrlBase, kCtrlSize);
        dump("ctrl1", kCtrlBase + kCtrlSize, kCtrlSize);
        dump("padmap", kPadMgr + 0x410, 0x10);
    } else if (op == "pad2") {
        // "pad2 lx ly rx ry buttons1 buttons2": hold player 2's controller (headless tests)
        int lx = 0, ly = 0, rx = 0, ry = 0, d1 = 0, d2 = 0; ss >> std::dec >> lx >> ly >> rx >> ry >> d1 >> d2;
        PadState st;
        st.lx = u8(128 + lx); st.ly = u8(128 + ly); st.rx = u8(128 + rx); st.ry = u8(128 + ry);
        st.digital1 = u16(d1); st.digital2 = u16(d2);
        std::lock_guard lk(g_lock);
        g_test_pad2 = st;
        g_test_pad2_held = lx || ly || rx || ry || d1 || d2;
        g_tri[1] = d2 & 0x10;
        // without a window (headless) nothing polls the pads: route them once here
        PadState p1;
        route_pads(p1, st);
        if (g_qs2 || g_pause2) pad_set_state(0, p1);
        pad_set_state(1, st);
    } else LOG_ERR("coop: unknown command %s", op.c_str());
}

}  // namespace

void command(const std::string& line) {
    std::lock_guard lk(g_lock);
    g_cmds.push_back(line);
}

// "pad2 ..." from the flip (cheats::poll_input_commands): run now, since the queue waits for the
// game's frame and none runs while the pause menu is up
void pad2_command(const std::string& line) {
    if (line.rfind("pad2", 0) == 0) run(line);
}

void tick() {
    ++g_frame;
    std::deque<std::string> q;
    {
        std::lock_guard lk(g_lock);
        q.swap(g_cmds);
    }
    for (auto& l : q) run(l);
    manage();
    rules();
}

bool in_p2_update() { return g_in_p2; }
u32 hero2() { return g_hero2; }

}  // namespace coop
