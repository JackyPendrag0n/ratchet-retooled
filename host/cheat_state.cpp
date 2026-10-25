// Cheat state: what the menu shows and the actions it takes, all done through the game's own Lua
// bindings (hero_give_weapon, hero_set_armor, health_set_invulnerability, set_level, ...).
// Persistent cheats (god mode, infinite ammo, no Clank) are re-applied every second, because
// level loads, checkpoints and cutscenes reset them.
#include "cheats.h"
#include "overlay_notify.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include <map>
#include <sstream>
#include <vector>

namespace cheats {
namespace {

std::mutex g_lock;
Snapshot g_snap;
Toggles g_tog;
std::atomic<int> g_frame_limit{-1};   // -1: as started (settings.ini)
std::atomic<bool> g_refresh{true};
std::atomic<bool> g_stache{false};    // Ratchet's moustache (re-applied with the persistent cheats)
std::vector<int> g_remove;            // weapons to take away (done on the game's thread)
std::atomic<u64> g_frames{0};

constexpr u32 kClock = 0x10731D00;    // the game's frame clock: +4 frame time cap, +8 fixed step

// Helpers defined in the game's Lua state once per chunk (cheap; the state outlives levels but a
// fresh state after a reload would lose them).
const char* kLib = R"LUA(
if __tod == nil then
  __tod = {}
  function __tod.out(s) __tod_out("@TOD" .. s) end
  -- the bindings reject extra (even nil) arguments, so calls are wrapped in closures
  function __tod.try(f)
    local ok, e = pcall(f)
    if not ok then __tod.out("E " .. tostring(e)) end
    return ok, e
  end
  function __tod.hero()
    if Ratchet1 ~= nil then return Ratchet1 end
    return nil
  end
  -- named constants (WPN_*, LEVEL_*, ARMOR_*) and the XP table for weapon levels
  function __tod.consts()
    local parts = {}
    for k,v in pairs(_G) do
      if type(v) == "number" and (string.find(k, "^WPN_") or string.find(k, "^LEVEL_") or string.find(k, "^ARMOR_")) then
        table.insert(parts, k .. "=" .. v)
      end
    end
    __tod.out("K " .. table.concat(parts, ";"))
  end
end
)LUA";

void lua(const std::string& body) { run_lua(std::string(kLib) + body); }

std::string b(bool v) { return v ? "true" : "false"; }

void apply_persistent() {
    const Toggles t = toggles();
    std::string s = "local h = __tod.hero() if h ~= nil then\n";
    if (t.god) s += "  __tod.try(function() health_set_invulnerability(h, true) end)\n";
    if (t.infinite_ammo) s += "  __tod.try(function() hero_set_infinite_ammo(h, true) end)\n";
    if (t.no_clank) s += "  __tod.try(function() ratchet_remove_clankpack(h) end)\n";
    if (g_stache) s += "  __tod.try(function() ratchet_set_stache(h, true) end)\n";
    s += "end\n";
    lua(s);
}

void request_status() {
    // get_pos first: the host notes the hero's moby from it (inventory and position are read there)
    lua(R"LUA(
local h = __tod.hero()
if h == nil then __tod.out("S 0") return end
pcall(get_pos, h)
local s = "S 1"
local ok, v = pcall(hero_get_armor, h) if ok then s = s .. " armor=" .. tostring(v) end
ok, v = pcall(hero_get_bolts, h) if ok then s = s .. " bolts=" .. tostring(v) end
ok, v = pcall(hero_get_health, h) if ok then s = s .. " health=" .. tostring(v) end
if get_level ~= nil then ok, v = pcall(get_level) if ok then s = s .. " level=" .. tostring(v) end end
local w = {}
for k,id in pairs(_G) do
  if type(id) == "number" and string.find(k, "^WPN_") then
    local ok2, has = pcall(hero_has_weapon, h, id)
    if ok2 and has then table.insert(w, id) end
  end
end
__tod.out(s .. " weapons=" .. table.concat(w, ","))
if is_skillpoint_done ~= nil then
  local d = {}
  for i = 0, 59 do
    local ok3, v3 = pcall(is_skillpoint_done, i)
    if ok3 and v3 then table.insert(d, i) end
  end
  __tod.out("P " .. table.concat(d, ","))
end
)LUA");
}

// Weapon levels live in the hero's inventory (hero = moby +0x40, inventory at +0x1A68): 20 bytes
// per weapon type {type, float xp, float ammo, ..., u8 level at +0x11}. The XP needed for each
// level is in the weapon's config (sub_4632B0(type)): float[] at +0x234, level count at +0x460.
constexpr u32 kWeaponConfigOpd = 0x874878;
std::map<int, std::vector<float>> g_thresholds;

void read_thresholds() {
    if (!g_thresholds.empty()) return;
    for (int w = 0; w < 32; ++w) {
        const u32 cfg = u32(call(kWeaponConfigOpd, {u64(w)}));
        if (!cfg || !mem::is_mapped(cfg, 0x464)) continue;
        const u32 n = ld32(mem::base, cfg + 0x460);
        std::vector<float> t;
        for (u32 i = 0; i < n && i < 10; ++i) {
            const u32 bits = ld32(mem::base, cfg + 0x234 + 4 * i);
            float f;
            std::memcpy(&f, &bits, 4);
            t.push_back(f);
        }
        g_thresholds[w] = t;
    }
    std::string list;
    for (auto& [w, t] : g_thresholds) list += " " + std::to_string(w) + ":" + std::to_string(t.size());
    LOG_NOTE("cheats: weapon level tables%s", list.c_str());
}

u32 inventory() {
    const u32 moby = hero_moby();
    if (!moby || !mem::is_mapped(moby, 0x44)) return 0;
    const u32 hero = ld32(mem::base, moby + 0x40);
    if (!hero || !mem::is_mapped(hero + 0x1A68, 4)) return 0;
    const u32 inv = ld32(mem::base, hero + 0x1A68);
    if (!inv || !mem::is_mapped(inv, 32 * 20)) return 0;
    return inv;
}

// No binding takes a weapon away: clear the owned flag of its inventory entry (byte +0x10, which
// is what hero_has_weapon reads).
void take_weapons() {
    std::vector<int> ws;
    {
        std::lock_guard lk(g_lock);
        ws.swap(g_remove);
    }
    const u32 inv = inventory();
    if (!inv || ws.empty()) return;
    for (int w : ws)
        if (w > 0 && w < 32) mem::base[inv + u32(w) * 20 + 0x10] = 0;
    g_refresh = true;
}

void read_levels() {
    const u32 inv = inventory();
    if (!inv) return;
    std::lock_guard lk(g_lock);
    for (auto& [w, lv] : g_snap.weapon_level) {
        const u32 e = inv + u32(w) * 20;
        if (int(ld32(mem::base, e)) == w) lv = mem::base[e + 0x11] + 1;
    }
    g_snap.max_level.clear();
    for (auto& [w, t] : g_thresholds) g_snap.max_level[w] = int(t.size()) + 1;
}

// Raritanium upgrades ("mods"): bit n of the u32 at +0xC of a weapon's inventory entry. The vendor's
// Lua bindings show the way: get_num_mods -> sub_25878(type), get_mod_name -> sub_26548(type, n)
// (localized text), purchase_mod -> sub_2D2878, which checks and takes the raritanium (inventory
// +0x420) and calls sub_465D00(entry, n): set the bit, and if the upgrade changed the ammo capacity
// (sub_463DE0(entry), in f1) fill the ammo (+8) to it. Adding here calls sub_465D00 directly (free);
// removing clears the bit and trims the ammo to the smaller capacity.
// Functions at 0x2D0000 and above run with the TOC the trampolines (sub_12A20...) switch to.
constexpr u32 kBindingOpd = 0x85A8A0;   // purchase_mod's descriptor, for the main TOC
constexpr u32 kNumModsFn = 0x25878, kModNameFn = 0x26548, kAddModFn = 0x465D00, kMaxAmmoFn = 0x463DE0;
constexpr u32 kModBits = 0xC, kRaritanium = 0x420;
struct ModOp { int weapon, mod; bool add; };
std::vector<ModOp> g_mod_ops;
std::map<int, std::vector<std::string>> g_mod_names;   // per weapon type, filled once
std::atomic<int> g_raritanium_add{0};

u32 toc1() { return ld32(mem::base, kBindingOpd + 4); }
u32 toc2() { return toc1() + 0x10000 - 0x18; }

const std::vector<std::string>& mod_names(int w) {
    auto it = g_mod_names.find(w);
    if (it != g_mod_names.end()) return it->second;
    std::vector<std::string> names;
    const int n = int(s32(call_code(kNumModsFn, toc1(), {u64(w)})));
    for (int i = 0; i < n && i < 32; ++i) {
        const u32 p = u32(call_code(kModNameFn, toc1(), {u64(w), u64(i)}));
        std::string name;
        if (p && mem::is_mapped(p, 1))
            for (u32 a = p; mem::is_mapped(a, 1) && mem::base[a] && name.size() < 80; ++a) name += char(mem::base[a]);
        names.push_back(name);
    }
    return g_mod_names[w] = names;
}

// The game's own (localized) weapon names: the vendor's get_weapon_name binding calls
// sub_27528(type), which returns the text. Read once, when a level is loaded.
constexpr u32 kWeaponNameFn = 0x27528;
std::map<int, std::string> g_weapon_names;

void weapon_names() {
    if (!g_weapon_names.empty()) return;
    for (int w = 0; w < 32; ++w) {
        const u32 p = u32(call_code(kWeaponNameFn, toc1(), {u64(w)}));
        std::string name;
        if (p && mem::is_mapped(p, 1))
            for (u32 a = p; mem::is_mapped(a, 1) && mem::base[a] && name.size() < 60; ++a) name += char(mem::base[a]);
        // the menus' text is upper case: "PYRO BLASTER" -> "Pyro Blaster" (RYNO IV stays)
        bool start = true;
        for (size_t i = 0; i < name.size(); ++i) {
            const char ch = name[i];
            if (ch == ' ' || ch == '-' || ch == '.') { start = true; continue; }
            if (!start) name[i] = char(std::tolower(ch));
            start = false;
        }
        for (const char* keep : {"Ryno", " Iv"})
            if (auto at = name.find(keep); at != std::string::npos)
                for (size_t i = at; i < at + std::strlen(keep); ++i) name[i] = char(std::toupper(name[i]));
        if (!name.empty()) g_weapon_names[w] = name;
    }
    std::string list;
    for (auto& [w, n] : g_weapon_names) list += " " + std::to_string(w) + ":" + n;
    LOG_NOTE("cheats: weapon names%s", list.c_str());
    std::lock_guard lk(g_lock);
    for (auto& n : g_snap.weapons)
        if (auto gn = g_weapon_names.find(n.value); gn != g_weapon_names.end()) n.label = gn->second;
    ++g_snap.serial;
}

void apply_mods() {
    const u32 inv = inventory();
    if (!inv) return;
    std::vector<ModOp> ops;
    {
        std::lock_guard lk(g_lock);
        ops.swap(g_mod_ops);
    }
    for (const ModOp& o : ops) {
        if (o.weapon <= 0 || o.weapon >= 32 || o.mod < 0 || o.mod >= 32) continue;
        const u32 e = inv + u32(o.weapon) * 20;
        if (!mem::base[e + 0x10]) continue;   // not owned
        const u32 bits = ld32(mem::base, e + kModBits), bit = 1u << o.mod;
        if (o.add && !(bits & bit)) {
            call_code(kAddModFn, toc2(), {u64(e), u64(o.mod)});
        } else if (!o.add && (bits & bit)) {
            st32(mem::base, e + kModBits, bits & ~bit);
            const float cap = float(call_code_f(kMaxAmmoFn, toc2(), {u64(e)}));
            u32 ab = ld32(mem::base, e + 8);
            float ammo;
            std::memcpy(&ammo, &ab, 4);
            if (cap >= 0 && ammo > cap) { std::memcpy(&ab, &cap, 4); st32(mem::base, e + 8, ab); }
        }
    }
    if (const int add = g_raritanium_add.exchange(0))
        st32(mem::base, inv + kRaritanium, u32(std::max(0, int(ld32(mem::base, inv + kRaritanium)) + add)));
}

void read_mods() {
    const u32 inv = inventory();
    if (!inv) return;
    std::map<int, std::vector<Snapshot::Mod>> mods;
    std::vector<int> owned;
    {
        std::lock_guard lk(g_lock);
        for (auto& [w, lv] : g_snap.weapon_level) owned.push_back(w);
    }
    for (int w : owned) {
        if (w <= 0 || w >= 32) continue;
        const auto& names = mod_names(w);
        if (names.empty()) continue;
        const u32 bits = ld32(mem::base, inv + u32(w) * 20 + kModBits);
        auto& v = mods[w];
        for (size_t i = 0; i < names.size(); ++i) v.push_back({names[i], ((bits >> i) & 1) != 0});
    }
    std::lock_guard lk(g_lock);
    g_snap.mods.swap(mods);
    g_snap.raritanium = int(ld32(mem::base, inv + kRaritanium));
}

// Fun mod: the Gelanator is normally filled at gel pumps and runs dry. With the mod it is given once
// (hero_give_weapon) and kept at its full capacity every frame, so it fires anywhere.
int g_gel_type = -1;
void keep_gelanator() {
    if (!toggles().gelanator) return;
    if (g_gel_type < 0) {
        std::lock_guard lk(g_lock);
        for (auto& n : g_snap.weapons) if (n.name == "WPN_GELANATOR") g_gel_type = n.value;
        if (g_gel_type < 0) return;
    }
    const u32 inv = inventory();
    if (!inv || g_gel_type <= 0 || g_gel_type >= 32) return;
    const u32 e = inv + u32(g_gel_type) * 20;
    if (!mem::base[e + 0x10]) {
        static u64 last_try = 0;
        if (g_frames - last_try > 60) {
            last_try = g_frames;
            lua("local h = __tod.hero() if h then __tod.try(function() hero_give_weapon(h, " + std::to_string(g_gel_type) + ", 0) end) end");
            g_refresh = true;
        }
        return;
    }
    float cap = float(call_code_f(kMaxAmmoFn, toc2(), {u64(e)}));
    if (!(cap > 0)) cap = 100.f;
    static bool logged = false;
    if (!logged) { logged = true; LOG_NOTE("cheats: Gelanator (type %d) kept at %.0f gel", g_gel_type, cap); }
    u32 ab;
    std::memcpy(&ab, &cap, 4);
    if (ld32(mem::base, e + 8) != ab) st32(mem::base, e + 8, ab);
}

// ---- the game's own cheats (Extras menu) and skill points. The front end's Lua bindings
// (get_cheat_name, get_cheat_state, ...) are only registered in the menu's Lua state, so the
// functions behind them are called directly. They take the menu's index (0..count-1).
constexpr u32 kNumCheatsFn = 0x24ED8, kCheatUnlockedFn = 0x27108, kCheatNameFn = 0x25110, kCheatNumStatesFn = 0x24F18,
              kCheatStateFn = 0x27098, kCheatStateNameFn = 0x24F80, kCheatSetFn = 0x27008;
// skill points by id (0..59): name sub_11F60(id), how to earn it sub_10540(id); done comes from
// the Lua status chunk (g_skills_done)
constexpr u32 kSkillNameFn = 0x11F60, kSkillDescFn = 0x10540;
struct CheatOp { int id, state; };
std::vector<CheatOp> g_cheat_ops;
std::vector<int> g_skill_ops;
std::atomic<bool> g_extras_refresh{true};
std::atomic<bool> g_unlock_all{false};
// done skill points, reported by the status chunk's "P" line (is_skillpoint_done); the bitmask
// read from memory didn't match what the game reports
std::atomic<u64> g_skills_done{0};

std::string guest_text(u32 p, size_t max = 200) {
    std::string t;
    if (p && mem::is_mapped(p, 1))
        for (u32 a = p; mem::is_mapped(a, 1) && mem::base[a] && t.size() < max; ++a) t += char(mem::base[a]);
    return t;
}

// "BIG HEAD RATCHET" -> "Big Head Ratchet" (the menus' text is upper case)
std::string title_case(std::string n) {
    bool start = true;
    for (char& ch : n) {
        if (ch == ' ' || ch == '-' || ch == '.' || ch == '(' || ch == '/') { start = true; continue; }
        if (!start) ch = char(std::tolower(ch));
        start = false;
    }
    return n;
}

void read_extras() {
    request_status();  // its "P" line brings the done skill points
    std::vector<Snapshot::GameCheat> cheats;
    const int n = int(s32(call_code(kNumCheatsFn, toc1(), {})));
    for (int i = 0; i < n && i < 32; ++i) {
        Snapshot::GameCheat gc;
        gc.id = i;
        gc.name = title_case(guest_text(u32(call_code(kCheatNameFn, toc1(), {u64(i)}))));
        gc.unlocked = call_code(kCheatUnlockedFn, toc1(), {u64(i)}) & 1;
        gc.state = int(s32(call_code(kCheatStateFn, toc1(), {u64(i)})));
        const int ns = int(s32(call_code(kCheatNumStatesFn, toc1(), {u64(i)})));
        for (int k = 0; k < ns && k < 16; ++k) gc.states.push_back(title_case(guest_text(u32(call_code(kCheatStateNameFn, toc1(), {u64(i), u64(k)})))));
        cheats.push_back(gc);
    }
    std::vector<Snapshot::Skill> skills;
    // skill point ids run 0..59 (get_num_skillpoints counts the ones of a planet)
    for (int i = 0; i < 60; ++i) {
        Snapshot::Skill sk;
        sk.id = i;
        sk.name = title_case(guest_text(u32(call_code(kSkillNameFn, toc1(), {u64(i)}))));
        sk.desc = guest_text(u32(call_code(kSkillDescFn, toc1(), {u64(i)})), 400);
        sk.done = (g_skills_done.load() >> i) & 1;
        if (!sk.name.empty()) skills.push_back(sk);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::string list;
        for (auto& c : cheats) {
            list += " [" + std::to_string(c.id) + " " + c.name + (c.unlocked ? "" : " (locked)") + " = " + std::to_string(c.state);
            for (auto& st : c.states) list += " | " + st;
            list += "]";
        }
        LOG_NOTE("cheats: game cheats:%s", list.c_str());
        int done = 0;
        for (auto& k : skills) done += k.done;
        LOG_NOTE("cheats: %zu skill points, %d done", skills.size(), done);
    }
    std::lock_guard lk(g_lock);
    g_snap.game_cheats.swap(cheats);
    g_snap.skills.swap(skills);
    ++g_snap.serial;
}

void apply_extras() {
    std::vector<CheatOp> ops;
    std::vector<int> skills;
    {
        std::lock_guard lk(g_lock);
        ops.swap(g_cheat_ops);
        skills.swap(g_skill_ops);
    }
    for (const CheatOp& o : ops) {
        call_code(kCheatSetFn, toc1(), {u64(o.id), u64(o.state)});
        LOG_NOTE("cheats: game cheat %d set to %d (reads back %d)", o.id, o.state, int(s32(call_code(kCheatStateFn, toc1(), {u64(o.id)}))));
    }
    if (!skills.empty()) {
        // skill points by name constant: the menu index and the game's id can differ
        std::string body;
        for (int i : skills) body += "__tod.try(function() set_skillpoint_done(" + std::to_string(i) + ") end)\n";
        lua(body);
    }
    // enable_all_cheats unlocks everything in the Extras menu but also switches every cheat on:
    // the states are put back on the next frame
    static std::vector<std::pair<int, int>> restore;
    static int restore_in = 0;
    if (restore_in && --restore_in == 0) {
        for (auto [id, st] : restore) call_code(kCheatSetFn, toc1(), {u64(id), u64(st)});
        restore.clear();
        g_extras_refresh = true;
    }
    if (g_unlock_all.exchange(false)) {
        restore.clear();
        const int n = int(s32(call_code(kNumCheatsFn, toc1(), {})));
        for (int i = 0; i < n && i < 32; ++i) restore.push_back({i, int(s32(call_code(kCheatStateFn, toc1(), {u64(i)})))});
        lua("__tod.try(function() enable_all_cheats() end)");
        restore_in = 2;
    }
    // read back once the queued Lua has run (after this frame's hook)
    static int reread_in = 0;
    if (!ops.empty() || !skills.empty()) reread_in = 2;
    if (reread_in && --reread_in == 0) g_extras_refresh = true;
}

// ---- game speed: the game's global time scale (what the time_set_scale binding writes: float at
// +0xA0 of the object at TOC-0x6FB4, with the TOC of that binding's descriptor). 1 is left to the
// game (Clank's slow-time uses it); any other speed is written every frame.
constexpr u32 kTimeSetScaleOpd = 0x86B558;
std::atomic<float> g_speed{1.f};
std::atomic<float> g_slowmo{0.35f};
std::atomic<float> g_speed_written{1.f};

void apply_speed(bool force = false) {
    const float sp = g_speed;
    if (!force && sp == 1.f && g_speed_written == 1.f) return;
    const u32 toc = ld32(mem::base, kTimeSetScaleOpd + 4);
    const u32 obj = ld32(mem::base, toc - 0x6FB4);
    if (!obj || !mem::is_mapped(obj + 0xA0, 4)) return;
    if (sp != g_speed_written.load()) LOG_NOTE("cheats: game speed %.2f (time scale at 0x%08x was %08x)", sp, obj + 0xA0, ld32(mem::base, obj + 0xA0));
    u32 bits;
    std::memcpy(&bits, &sp, 4);
    st32(mem::base, obj + 0xA0, bits);
    g_speed_written = sp;
}

int to_int(const std::string& v) { return std::atoi(v.c_str()); }

void on_output(const std::string& line) {
    // "@TOD<kind> <payload>"
    const std::string body = line.substr(4);
    if (body.empty()) return;
    const char kind = body[0];
    const std::string rest = body.size() > 2 ? body.substr(2) : "";
    static const bool trace = std::getenv("TOD_CHEAT_TRACE") != nullptr;
    if (trace || (kind != 'K' && kind != 'S' && kind != 'E' && kind != 'P')) LOG_NOTE("lua: %s", body.c_str());
    std::lock_guard lk(g_lock);
    if (kind == 'E') { LOG_NOTE("cheats: lua error: %s", rest.c_str()); return; }
    if (kind == 'P') {
        u64 m = 0;
        std::stringstream ss(rest);
        std::string item;
        while (std::getline(ss, item, ',')) if (!item.empty()) { const int i = to_int(item); if (i >= 0 && i < 64) m |= 1ull << i; }
        g_skills_done = m;
        for (auto& sk : g_snap.skills) sk.done = (m >> sk.id) & 1;
        return;
    }
    if (kind == 'K') {
        g_snap.weapons.clear(); g_snap.levels.clear(); g_snap.armors.clear();
        std::stringstream ss(rest);
        std::string item;
        while (std::getline(ss, item, ';')) {
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            Named n{item.substr(0, eq), "", to_int(item.substr(eq + 1))};
            // "WPN_BUZZ_BLADES" -> "Buzz Blades"
            const size_t us = n.name.find('_');
            std::string label;
            bool up = true;
            for (char ch : n.name.substr(us + 1)) {
                if (ch == '_') { label += ' '; up = true; continue; }
                label += up ? ch : char(std::tolower(ch));
                up = false;
            }
            // retail names where the constant is a working title (replaced by the game's own text
            // once a level is loaded, see weapon_names())
            static const std::map<std::string, std::string> kRetail = {
                {"WPN_WRENCH", "Omniwrench 3000"}, {"WPN_FLAMETHROWER", "Pyro Blaster"}, {"WPN_GRENADE", "Fusion Grenade"},
                {"WPN_ALPHA_NOVA", "Alpha Disruptor"}, {"WPN_ROCKET_LAUNCHER", "Negotiator"}, {"WPN_PREDATOR", "Predator Launcher"},
                {"WPN_RAVAGER", "Shock Ravager"}, {"WPN_MAG_NET", "Mag-Net Launcher"}, {"WPN_RYNO", "RYNO IV"},
                {"WPN_TORNADO", "Tornado Launcher"}, {"WPN_ZURKON", "Mr. Zurkon"}, {"WPN_GOOP_MINES", "Plasma Beasts"},
                {"WPN_REAPER", "Shard Reaper"}, {"WPN_ROBO_HIVE", "Nano-Swarmers"}, {"WPN_ENERGY_CLAWS", "Razor Claws"},
                {"WPN_CONFUSION_GAS", "Confuzzler Gas"}, {"WPN_SLINKONATOR", "Death Springs"}, {"WPN_MINI_LEECH", "Leech Bomb"},
                {"WPN_MAXI_LEECH", "Mega Leech Bomb"}, {"WPN_MORPH_BOMB", "Transmorpher"}, {"WPN_VISICOPTER", "Visi-Copter"},
                {"WPN_INFLATOPOD", "Heli-Pods"}, {"WPN_CUTTING_LASER", "Geo-Laser"}, {"WPN_ROBOWINGS", "Robo-Wings"},
                {"WPN_MAGCYCLE", "Gyro-Cycle"}, {"WPN_PIRATEGUISE", "Holo-Pirate Disguise"}};
            if (auto r = kRetail.find(n.name); r != kRetail.end()) label = r->second;
            n.label = label;
            if (n.name.rfind("WPN_", 0) == 0) {
                if (auto gn = g_weapon_names.find(n.value); gn != g_weapon_names.end()) n.label = gn->second;
                g_snap.weapons.push_back(n);
            }
            else if (n.name.rfind("LEVEL_", 0) == 0) g_snap.levels.push_back(n);
            else if (n.name.rfind("ARMOR_", 0) == 0 && n.name != "ARMOR_TYPE_COUNT") g_snap.armors.push_back(n);
        }
        auto by_value = [](const Named& a, const Named& b) { return a.value < b.value; };
        std::sort(g_snap.weapons.begin(), g_snap.weapons.end(), by_value);
        std::sort(g_snap.levels.begin(), g_snap.levels.end(), by_value);
        std::sort(g_snap.armors.begin(), g_snap.armors.end(), by_value);
        ++g_snap.serial;
        return;
    }
    if (kind == 'S') {
        g_snap.in_level = rest.size() && rest[0] == '1';
        std::stringstream ss(rest);
        std::string tok;
        while (ss >> tok) {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
            if (k == "level") g_snap.level = to_int(v);
            else if (k == "armor") g_snap.armor = to_int(v);
            else if (k == "bolts") g_snap.bolts = to_int(v);
            else if (k == "health") g_snap.health = to_int(v);
            else if (k == "weapons") {
                std::map<int, int> levels;
                std::stringstream ws(v);
                std::string w;
                while (std::getline(ws, w, ',')) {
                    const int id = to_int(w);
                    auto old = g_snap.weapon_level.find(id);
                    levels[id] = old != g_snap.weapon_level.end() ? old->second : 1;
                }
                g_snap.weapon_level.swap(levels);
            }
        }
        ++g_snap.serial;
    }
}

void on_frame() {
    const u64 f = ++g_frames;
    static bool consts = false;
    if (!consts || g_refresh.exchange(false)) {
        if (!consts) lua("__tod.consts()");
        consts = true;
        request_status();
    }
    if (f % 60 == 0) apply_persistent();
    apply_speed();
    if (hero_moby()) {
        apply_extras();
        if (g_extras_refresh.exchange(false) || (overlay_menu_open() && f % 60 == 30)) read_extras();
        read_thresholds();
        take_weapons();
        read_levels();
        weapon_names();
        apply_mods();
        keep_gelanator();
        if (overlay_menu_open() && f % 20 == 5) read_mods();
    }
    if (overlay_menu_open() && f % 20 == 0) request_status();
}

struct Init {
    Init() { set_hooks({on_frame, on_output}); }
} g_init;

}  // namespace

Snapshot snapshot() {
    std::lock_guard lk(g_lock);
    return g_snap;
}

Toggles toggles() {
    std::lock_guard lk(g_lock);
    return g_tog;
}

void set_god(bool on) {
    { std::lock_guard lk(g_lock); g_tog.god = on; }
    lua("local h = __tod.hero() if h then __tod.try(function() health_set_invulnerability(h, " + b(on) + ") end) end");
    overlay_notify(on ? "God mode on" : "God mode off");
}

void set_infinite_ammo(bool on) {
    { std::lock_guard lk(g_lock); g_tog.infinite_ammo = on; }
    lua("local h = __tod.hero() if h then __tod.try(function() hero_set_infinite_ammo(h, " + b(on) + ") end) end");
    overlay_notify(on ? "Infinite ammo on" : "Infinite ammo off");
}

void set_no_clank(bool on) {
    { std::lock_guard lk(g_lock); g_tog.no_clank = on; }
    lua(std::string("local h = __tod.hero() if h then __tod.try(function() ") + (on ? "ratchet_remove_clankpack" : "ratchet_add_clankpack") + "(h) end) end");
    overlay_notify(on ? "Clank removed" : "Clank is back");
}

void set_noclip(bool on) {
    // cheat_fly.cpp holds the hero where the pad flies it; the game's collision never gets a say
    { std::lock_guard lk(g_lock); g_tog.noclip = on; }
    overlay_notify(on ? "No clip on: left stick moves, R1/L1 up and down, R2 faster" : "No clip off");
}

void set_freecam(bool on) {
    { std::lock_guard lk(g_lock); g_tog.freecam = on; }
    overlay_notify(on ? "Free camera on: sticks fly and look, R1/L1 up and down, R2 faster" : "Free camera off");
}

// XP that reaches `level` (1-based): 0 for level 1, else the table entry for that level.
float xp_for(int weapon, int level) {
    auto it = g_thresholds.find(weapon);
    if (it == g_thresholds.end() || level <= 1 || it->second.empty()) return 0;
    return it->second[std::min<size_t>(size_t(level - 2), it->second.size() - 1)];
}

void give_weapon(int weapon, int level) {
    // hero_give_weapon(hero, weapon, xp) also sets the XP (and so the level) of an owned weapon
    lua("local h = __tod.hero() if h then __tod.try(function() hero_give_weapon(h, " + std::to_string(weapon) + ", " +
        std::to_string(int(xp_for(weapon, level))) + ") end) end");
    g_refresh = true;
}

void remove_weapon(int weapon) {
    std::lock_guard lk(g_lock);
    g_remove.push_back(weapon);
}

void give_all_weapons(int level) {
    std::string body = "local h = __tod.hero() if h then\n";
    for (int w = 1; w < 32; ++w)
        body += "  __tod.try(function() hero_give_weapon(h, " + std::to_string(w) + ", " + std::to_string(int(xp_for(w, level))) + ") end)\n";
    lua(body + "end");
    g_refresh = true;
    overlay_notify("Gave every weapon, gadget and device");
}

void set_armor(int armor) {
    lua("local h = __tod.hero() if h then __tod.try(function() hero_set_armor(h, " + std::to_string(armor) + ") end) end");
    g_refresh = true;
}

void set_mod(int weapon, int mod, bool owned) {
    std::lock_guard lk(g_lock);
    g_mod_ops.push_back({weapon, mod, owned});
    if (auto it = g_snap.mods.find(weapon); it != g_snap.mods.end() && mod >= 0 && size_t(mod) < it->second.size())
        it->second[size_t(mod)].owned = owned;   // shown at once; the next read confirms it
}

void set_all_mods(int weapon, bool owned) {
    std::lock_guard lk(g_lock);
    auto it = g_snap.mods.find(weapon);
    if (it == g_snap.mods.end()) return;
    for (size_t i = 0; i < it->second.size(); ++i) { g_mod_ops.push_back({weapon, int(i), owned}); it->second[i].owned = owned; }
}

void add_raritanium(int n) { g_raritanium_add += n; }

void set_gelanator(bool on) {
    { std::lock_guard lk(g_lock); g_tog.gelanator = on; }
    overlay_notify(on ? "Gelanator mod on: always yours, always full of gel" : "Gelanator mod off");
}

void set_game_cheat(int id, int state) {
    std::lock_guard lk(g_lock);
    g_cheat_ops.push_back({id, state});
    for (auto& c : g_snap.game_cheats) if (c.id == id) c.state = state;
}
void unlock_all_game_cheats() {
    g_unlock_all = true;
    overlay_notify("Unlocked everything in the game's Extras menu");
}
void set_skill_done(int id) {
    std::lock_guard lk(g_lock);
    g_skill_ops.push_back(id);
}
void refresh_extras() { g_extras_refresh = true; }

void set_stache(bool on) {
    g_stache = on;
    lua("local h = __tod.hero() if h then __tod.try(function() ratchet_set_stache(h, " + b(on) + ") end) end");
    overlay_notify(on ? "Moustache on" : "Moustache off");
}
bool stache() { return g_stache; }

float game_speed() { return g_speed; }
void set_game_speed(float sp) {
    // very small values freeze the world (photo mode); exactly 0 would also stop the script updates
    // the cheat code runs from
    sp = std::clamp(sp, 0.001f, 4.f);
    if (std::fabs(sp - 1.f) < 0.01f) sp = 1.f;
    g_speed = sp;
    apply_speed(true);   // written now too: the per-frame write only runs while scripts update
}
void toggle_slowmo() {
    const bool on = g_speed == 1.f;
    set_game_speed(on ? g_slowmo.load() : 1.f);
    overlay_notify(on ? "Slow motion (F2 for normal speed)" : "Normal speed");
}
float slowmo_preset() { return g_slowmo; }
void set_slowmo_preset(float sp) { g_slowmo = std::clamp(sp, 0.05f, 0.95f); }

void warp_to_level(int level) {
    lua("__tod.try(function() set_level(" + std::to_string(level) + ") end)");
}

void add_bolts(int n) {
    lua("local h = __tod.hero() if h then __tod.try(function() hero_add_bolts(h, " + std::to_string(n) + ") end) end");
    g_refresh = true;
}

int frame_limit() {
    const int f = g_frame_limit;
    return f >= 0 ? f : g_config.framerate;
}

void set_frame_limit(int fps) {
    g_frame_limit = fps;
    // 30 and 60: the game's fixed step (cap == step, as -lockfps does). Otherwise only the cap
    // changes (as -capfps does) and the game measures each frame, with its own 0.08 s default
    // in the other field.
    const float cap = fps > 0 ? 1.f / float(fps) : 1.f / 1000.f;
    const float step = fps == 30 || fps == 60 ? cap : 0.08f;
    u32 bits;
    std::memcpy(&bits, &step, 4);
    st32(mem::base, kClock + 8, bits);
    std::memcpy(&bits, &cap, 4);
    st32(mem::base, kClock + 4, bits);
    if (fps) overlay_notify("Frame rate cap: %d fps", fps);
    else overlay_notify("Frame rate: uncapped");
}

void apply_settings(const std::map<std::string, std::string>& kv) {
    auto on = [&](const char* k) {
        auto it = kv.find(k);
        return it != kv.end() && (it->second == "true" || it->second == "1" || it->second == "yes" || it->second == "on");
    };
    std::lock_guard lk(g_lock);
    g_tog.god = on("god_mode");
    g_tog.infinite_ammo = on("infinite_ammo");
    g_tog.no_clank = on("no_clank");
    g_tog.gelanator = on("gelanator_mod");
}

}  // namespace cheats

// Without the window build there is no overlay: notices go to the log.
__attribute__((weak)) void overlay_notify(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG_NOTE("cheats: %s", buf);
}
__attribute__((weak)) bool overlay_menu_open() { return false; }
__attribute__((weak)) void overlay_set_menu_open(int) {}
