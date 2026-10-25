// Weapon XP in the pause menu.
//
// The pause menu is an Anark presentation (built/anark/pause/built.dat) scripted in Lua 5.0. Its weapons
// page fills the selected weapon's panel through the get_weapon_* bindings (the function at line 156 of
// its script): ammo, name, `weapDesc.textString = get_weapon_desc(w)`, power, then
// `verValue.textString = "V" .. get_weapon_level(w)` ("X" from level 10), the XP bar's scale from
// get_weapon_progress(w), and textScroller:resetText(). The XP itself is never shown. The level text sits
// right against the bar and long names fill the name line, so the XP goes on a first line of the
// description ("XP: 700 / 1,000"), drawn by the game's own text object in the menu's font.
//
// - HOOKS 0x2F938: the get_weapon_desc binding (sub_2F880) is about to call sub_274E8(type); r3 = the type.
// - HOOKS 0x33B50: the get_weapon_level binding (sub_33A98) is about to call sub_27608(type); r3 = the type.
// - HOOKS 0x74F0F4: the Text.textString setter (sub_74F050, tolua) is about to call sub_742B4C(text, string)
//   (CLuaObjectText's SetText); r28 = the text object, r3 = the new string.
// The text set right after a get_weapon_desc call is noted (object and string). When the text set right
// after a get_weapon_level call of the same weapon is "V<level>" (the weapons page; the quick select page
// writes the bare number and has no room), the noted description gets its text again with the XP line
// first, before the page resets its scroller. SetText copies the string, so one buffer does.
//
// The numbers follow the game's own progress (sub_465FF0, what get_weapon_progress returns): the inventory
// entry (TOC[-0x7914] + 20 * type: XP float +4, level index +0x11), the weapon's config (sub_4632B0: XP to
// reach each level, floats at +0x230) and the highest level it can reach now (sub_465F08: 4 = V5 outside
// challenge mode). At that level the line shows the XP and "(max)".
#include "weapon_xp.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace weapon_xp {
namespace {

constexpr u32 kTocOpd = 0x85A8A0;                    // a binding's descriptor: its TOC word is the main TOC
constexpr u32 kConfigFn = 0x4632B0, kMaxLevelFn = 0x465F08;
constexpr u32 kSetTextFn = 0x742B4C;          // CLuaObjectText SetText(text, string)
constexpr u32 kBufSize = 1024;

std::atomic<bool> g_enabled{true};
enum Pending { NONE, DESC, LEVEL };
thread_local Pending t_pending = NONE;              // the binding called last, before a text was set
thread_local int t_type = -1;                       // ... and its weapon
thread_local u32 t_desc_obj = 0;                    // the text that got get_weapon_desc(t_desc_type)
thread_local int t_desc_type = -1;
thread_local std::string t_desc;
u32 g_buf = 0;

u32 toc1() { return ld32(mem::base, kTocOpd + 4); }
u32 toc2() { return toc1() + 0x10000 - 0x18; }
float ldf(u32 a) { float v; const u32 b = ld32(mem::base, a); std::memcpy(&v, &b, 4); return v; }

// a guest function (entry address + TOC) called from inside a hook, below the hooked frame
u64 call(PPUContext& c, u32 code, u32 toc, std::initializer_list<u64> args) {
    PPUFunc f = ppu_lookup(code);
    if (!f) return 0;
    PPUContext saved = c;
    c.r[1] = (c.r[1] - 0x800) & ~0xFull;
    int i = 3;
    for (u64 a : args) c.r[i++] = a;
    c.r[2] = toc;
    c.lr = 0;
    f(c, mem::base);
    const u64 r = c.r[3];
    c = saved;
    return r;
}

std::string grouped(float v) {
    std::string s = std::to_string(long(std::floor(std::max(v, 0.f))));
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

// "XP: <xp> / <next level's xp>" for the weapon whose level text is `level`, or "" when it isn't one.
std::string xp_text(PPUContext& c, int type, const std::string& level) {
    if (type < 0 || type >= 32) return "";
    const u32 inv = ld32(mem::base, toc1() - 0x7914);
    if (!inv || !mem::is_mapped(inv + u32(type) * 20, 20)) return "";
    const u32 e = inv + u32(type) * 20;
    const int lvl = mem::base[e + 0x11];
    if (level != "V" + (lvl + 1 >= 10 ? std::string("X") : std::to_string(lvl + 1))) return "";
    const u32 cfg = u32(call(c, kConfigFn, toc2(), {u64(type)}));
    if (!cfg || !mem::is_mapped(cfg + 0x230, 40)) return "";
    const int top = int(u8(call(c, kMaxLevelFn, toc2(), {u64(e)})));
    std::string out = "XP: " + grouped(ldf(e + 4));
    if (lvl < top && lvl < 9) out += " / " + grouped(ldf(cfg + 0x230 + 4 * u32(lvl + 1)));
    else out += " (max)";
    return out;
}

}  // namespace

void configure(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }
void set_enabled(bool on) { g_enabled = on; }

bool on_hook(PPUContext& c, u32 site) {
    if (site == 0x2F938 || site == 0x33B50) {
        t_pending = site == 0x2F938 ? DESC : LEVEL;
        t_type = int(s32(u32(c.r[3])));
        return false;
    }
    // 0x74F0F4: text object r28 is about to get the string at r3
    const Pending pending = t_pending;
    t_pending = NONE;
    if (pending == NONE || !g_enabled) return false;
    const u32 str = u32(c.r[3]);
    if (!str || !mem::is_mapped(str, 4)) return false;
    if (pending == DESC) {
        t_desc_obj = u32(c.r[28]);
        t_desc_type = t_type;
        t_desc = read_cstr(str, kBufSize);
        return false;
    }
    const u32 obj = t_desc_obj;
    t_desc_obj = 0;
    if (!obj || t_desc_type != t_type) return false;
    const std::string xp = xp_text(c, t_type, read_cstr(str, 8));
    if (xp.empty()) return false;
    if (!g_buf) g_buf = mem::alloc_main(kBufSize, 16);
    write_cstr(g_buf, (xp + "\n" + t_desc).substr(0, kBufSize - 1));
    call(c, kSetTextFn, u32(c.r[2]), {u64(obj), u64(g_buf)});
    return false;
}

}  // namespace weapon_xp
