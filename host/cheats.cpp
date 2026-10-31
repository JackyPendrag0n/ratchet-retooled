// Cheats and debug tools. The game keeps its gameplay rules in Lua (5.0): level scripts call
// bindings such as hero_give_weapon, hero_set_armor, health_set_invulnerability or set_level.
// The host queues Lua chunks and runs them in the level scripts' Lua state on the game's main
// thread, at the start of a script object update (HOOKS 0x293C70) where no Lua code is running.
// A chunk reports back with __tod_out(line); the lines return as the message of a final error().
#include "cheats.h"
#include "weapon_xp.h"
#include "skins.h"
#include "abilities.h"
#include "coop.h"
#include "gfx.h"
#include "input.h"
#include "overlay_notify.h"
#include "rsx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

void debug_watch_write(u32 addr, u32 skip_fn, bool reads = false);  // core.cpp

namespace cheats {
void on_output(const std::string& s);
namespace {

// Lua API in the EBOOT (function descriptors): lua_load with the reader the game's own "execute"
// uses (sub_667290: data {const char* text; u8 done}), and lua_pcall.
constexpr u32 kLuaLoadOpd = 0x87DBB8, kLuaPcallOpd = 0x87DC20, kStringReaderOpd = 0x87DA08;
constexpr u32 kScratchSize = 0x20000;

std::mutex g_lock;
std::deque<std::string> g_queue;
std::atomic<bool> g_pending{false};
u32 g_scratch = 0;
std::atomic<u32> g_script_thread{0};
std::atomic<u64> g_runs{0};
std::atomic<u32> g_hero_moby{0};
bool g_capture = false;
thread_local PPUContext* t_ctx = nullptr;

// set from other files' static initialisers, so constructed on first use
Hooks& hooks() { static Hooks h; return h; }

// L: the level scripts' Lua state (the script object's owner at +4, its state at +4).
void execute_now(PPUContext& c, u8* base, u32 L, const std::string& chunk) {
    if (!g_scratch) g_scratch = mem::alloc_main(kScratchSize, 0x1000);
    // Lua 5.0 has no way to hand a string to C without more API calls, so the chunk's output
    // lines come back as the message of a final error(). Errors inside the chunk are caught first.
    const std::string text = "__tod_lines = {} function __tod_out(s) table.insert(__tod_lines, s) end\n"
                             "local __ok, __e = pcall(function()\n" + chunk +
                             "\nend) if not __ok then __tod_out('@TODE ' .. tostring(__e)) end\n"
                             "if table.getn(__tod_lines) > 0 then error(table.concat(__tod_lines, '\\n'), 0) end";
    const u32 data = g_scratch, name = g_scratch + 16, src = g_scratch + 32;
    const size_t n = std::min<size_t>(text.size(), kScratchSize - 33);
    st32(base, data, src);
    base[data + 4] = 0;
    std::memcpy(base + name, "=cheats", 8);
    std::memcpy(base + src, text.data(), n);
    base[src + n] = 0;
    // run on a copy of the registers below the current frame; the guest sees an ordinary call
    PPUContext saved = c;
    const u32 top = ld32(base, L + 8);   // L->top: dropped back afterwards (the message is left there)
    std::string out;
    c.r[1] = (c.r[1] - 0x800) & ~0xFull;
    try {
        guest_call(c, kLuaLoadOpd, {L, kStringReaderOpd, data, name});
        const bool loaded = u32(c.r[3]) == 0;
        if (loaded) guest_call(c, kLuaPcallOpd, {L, 0, 0, 0});
        // a message is a TValue {tt = 4 (string), TString*}; the text is 16 bytes into the TString
        const u32 t = ld32(base, L + 8);
        if (u32(c.r[3]) != 0 && t > top && ld32(base, t - 8) == 4) out = read_cstr(ld32(base, t - 4) + 16, kScratchSize);
        if (!loaded) { LOG_ERR("cheats: Lua chunk did not compile: %s", out.c_str()); out.clear(); }
    } catch (...) {
        c = saved;
        throw;
    }
    st32(base, L + 8, top);
    c = saved;
    ++g_runs;
    for (size_t p = 0; p < out.size();) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos) e = out.size();
        if (e > p) on_output(out.substr(p, e - p));
        p = e + 1;
    }
}

// For testing without a window: <data>/lua_commands.txt is run once, then renamed to .done.
void poll_command_file() {
    static std::mutex busy;   // called from the script update and the camera update
    std::unique_lock guard(busy, std::try_to_lock);
    if (!guard.owns_lock()) return;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(500)) return;
    last = now;
    const std::string path = g_config.data_dir + "/lua_commands.txt";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    std::filesystem::rename(path, path + ".done", ec);
    const std::string text = ss.str();
    if (text.rfind("!co ", 0) == 0) { coop::command(text.substr(4)); return; }
    if (text.rfind("!glide", 0) == 0) { abilities::command(text.substr(6)); return; }
    if (text.rfind("!dump ", 0) == 0) {
        // "!dump <hex address> <hex length>": memory to the log (debugging)
        char* end = nullptr;
        u32 a = u32(std::strtoul(text.c_str() + 6, &end, 16)), n = u32(std::strtoul(end, nullptr, 16));
        for (u32 o = 0; o < n; o += 16)
            LOG_NOTE("dump %08x: %08x %08x %08x %08x", a + o, ld32(mem::base, a + o), ld32(mem::base, a + o + 4), ld32(mem::base, a + o + 8), ld32(mem::base, a + o + 12));
        return;
    }
    if (text.rfind("!mobys", 0) == 0) {
        // "!mobys [radius]": mobys whose position (+0x30) is near the hero's, with their class (+0x50, +0x54)
        const u32 moby = g_hero_moby;
        if (!moby) return;
        auto f = [](u32 a) { float v; u32 b = ld32(mem::base, a); std::memcpy(&v, &b, 4); return v; };
        const float hx = f(moby + 0x30), hy = f(moby + 0x34), hz = f(moby + 0x38);
        const u32 cls0 = ld32(mem::base, moby + 0x54);
        float r = 3;
        std::stringstream(text.substr(6)) >> r;
        for (u32 page = 0x10000; page < 0x60000000; page += 0x10000) {
            if (!mem::is_mapped(page, 0x10000)) continue;
            for (u32 a = page; a + 0x60 < page + 0x10000; a += 0x10) {
                const float dx = f(a + 0x30) - hx, dy = f(a + 0x34) - hy, dz = f(a + 0x38) - hz;
                if (!(dx * dx + dy * dy + dz * dz <= r * r)) continue;
                const u32 cp = ld32(mem::base, a + 0x54);
                const u16 ci = ld16(mem::base, a + 0x50);
                if (ci >= 1000 || cp < cls0 - 0x100000 || cp > cls0 + 0x100000) continue;
                LOG_NOTE("cheats: moby 0x%08x class %u (0x%08x) at %.2f %.2f %.2f", a, ci, cp, dx, dy, dz);
            }
        }
        return;
    }
    if (text.rfind("!scan", 0) == 0 || text.rfind("!nudge", 0) == 0) {
        // "!scan [radius]": float triples near the hero, to <data>/scanN.txt; "!nudge dx dy dz [hex hero
        // field offsets]": move the hero's moby (and those fields)
        const u32 moby = g_hero_moby;
        if (!moby) return;
        auto f = [](u32 a) { float v; u32 b = ld32(mem::base, a); std::memcpy(&v, &b, 4); return v; };
        auto setf = [](u32 a, float v) { u32 b; std::memcpy(&b, &v, 4); st32(mem::base, a, b); };
        const float hx = f(moby + 0x30), hy = f(moby + 0x34), hz = f(moby + 0x38);
        std::stringstream cmd(text.substr(6));
        if (text[1] == 'n') {
            float dx = 0, dy = 0, dz = 0;
            cmd >> dx >> dy >> dz;
            setf(moby + 0x30, hx + dx); setf(moby + 0x34, hy + dy); setf(moby + 0x38, hz + dz);
            const u32 hero = ld32(mem::base, moby + 0x40);
            u32 offs[16]; int n = 0; u32 o;
            while (n < 16 && (cmd >> std::hex >> o)) offs[n++] = o;
            for (int i = 0; hero && i < n; ++i) { setf(hero + offs[i], hx + dx); setf(hero + offs[i] + 4, hy + dy); setf(hero + offs[i] + 8, hz + dz); }
            LOG_NOTE("cheats: hero %.2f %.2f %.2f -> %.2f %.2f %.2f", hx, hy, hz, hx + dx, hy + dy, hz + dz);
            return;
        }
        float r = 20;
        cmd >> r;
        // every hit goes to <data>/scan<N>.txt (compare two scans to see what moved)
        static int n = 0;
        const std::string out = g_config.data_dir + "/scan" + std::to_string(n++) + ".txt";
        std::ofstream o(out);
        o << "hero " << hx << " " << hy << " " << hz << "\n";
        static const struct { u32 from, to; } kRanges[] = {{0x00010000, 0x10000000}, {0x20000000, 0x30000000}, {0x30000000, 0x40000000}, {0x50000000, 0x60000000}, {0xD0000000, 0xE0000000}};
        int hits = 0;
        for (auto& rg : kRanges)
            for (u32 page = rg.from; page < rg.to; page += 0x10000) {
                if (!mem::is_mapped(page, 0x10000)) continue;
                for (u32 a = page; a < page + 0x10000 - 12; a += 4) {
                    const float x = f(a), y = f(a + 4), z = f(a + 8);
                    const float dx = x - hx, dy = y - hy, dz = z - hz, d2 = dx * dx + dy * dy + dz * dz;
                    if (!(d2 <= r * r) || std::fabs(x) < 1) continue;
                    char line[96];
                    std::snprintf(line, sizeof line, "%08x %.3f %.3f %.3f\n", a, x, y, z);
                    o << line;
                    ++hits;
                }
            }
        LOG_NOTE("cheats: scan near %.2f %.2f %.2f r=%.1f: %d hits in %s", hx, hy, hz, r, hits, out.c_str());
        return;
    }
    if (text[0] == '!') {
        // menu actions by name, for scripted tests: "!give <weapon> <level>", "!giveall <level>",
        // "!take <weapon>", "!armor n", "!god 0|1", "!ammo 0|1", "!clank 0|1" (0 = removed),
        // "!noclip 0|1", "!freecam 0|1", "!warp <level>", "!fps n", "!bolts n", "!menu 0|1", "!status"
        std::stringstream cmd(text.substr(1));
        std::string op;
        int a = 0, b = 0;
        cmd >> op >> a >> b;
        LOG_NOTE("cheats: command %s %d %d", op.c_str(), a, b);
        if (op == "give") give_weapon(a, b);
        else if (op == "giveall") give_all_weapons(a);
        else if (op == "take") remove_weapon(a);
        else if (op == "armor") set_armor(a);
        else if (op == "god") set_god(a);
        else if (op == "ammo") set_infinite_ammo(a);
        else if (op == "clank") set_no_clank(!a);
        else if (op == "noclip") set_noclip(a);
        else if (op == "freecam") set_freecam(a);
        else if (op == "warp") warp_to_level(a);
        else if (op == "fps") set_frame_limit(a);
        else if (op == "bolts") add_bolts(a);
        else if (op == "menu") overlay_set_menu_open(a);
        else if (op == "mod") { int on = 1; cmd >> on; set_mod(a, b, on); }   // "!mod <weapon> <n> 0|1"
        else if (op == "gel") set_gelanator(a);
        else if (op == "res") gfx::g.resolution_scale = std::clamp(a, 1, 4);   // graphics options, as the menu sets them
        else if (op == "aa") gfx::g.aa = a;
        else if (op == "rari") add_raritanium(a);
        else if (op == "speed") set_game_speed(float(a) / 100.f);   // "!speed 50" = half speed
        else if (op == "cheat") set_game_cheat(a, b);               // the game's own cheats, by menu index
        else if (op == "unlockcheats") unlock_all_game_cheats();
        else if (op == "skill") set_skill_done(a);
        else if (op == "stache") set_stache(a);
        else if (op == "skin") skins::command(a);
        else if (op == "skin2") skins::command2(a);
        else if (op == "skinclobber") skins::clobber();
        else if (op == "savepos") save_position();
        else if (op == "loadpos") restore_position();
        else if (op == "shot") gfx::g.screenshot = true;
        else if (op == "nodraw") gfx::g.no_draw = a != 0;
        else if (op == "shotsurf") { u32 ea = 0; std::stringstream(text.substr(10)) >> std::hex >> ea; gfx::g.shot_surf = ea; }
        else if (op == "skipdraws") { gfx::g.skip_from = unsigned(a); gfx::g.skip_to = unsigned(b); }
        else if (op == "dumpidx") { unsigned h = 0; std::stringstream(text.substr(9)) >> std::hex >> h; gfx::g.dump_idx = h; }
        else if (op == "skipvp" || op == "dumpvp") {
            unsigned h = 0;
            std::stringstream(text.substr(op.size() + 2)) >> std::hex >> h;
            (op == "skipvp" ? gfx::g.skip_vps : gfx::g.dump_vps) = h;
        }
        else if (op == "passlog") gfx::g.passlog = std::max(a, 1);
        else if (op == "find") {
            // "!find <hex bytes>": every guest address holding them (main memory and RSX local memory)
            std::string hex;
            std::stringstream(text.substr(6)) >> hex;
            std::vector<u8> pat;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) pat.push_back(u8(std::stoul(hex.substr(i, 2), nullptr, 16)));
            int hits = 0;
            for (u64 page = 0; page < 0x100000000ull && hits < 64 && !pat.empty(); page += 0x10000) {
                if (!mem::is_mapped(u32(page), 0x10000)) continue;
                const u8* b = mem::base + page;
                for (u32 k = 0; k + pat.size() <= 0x10000; ++k)
                    if (b[k] == pat[0] && std::memcmp(b + k, pat.data(), pat.size()) == 0) {
                        LOG_NOTE("cheats: found at %08x", u32(page + k));
                        if (++hits >= 64) break;
                    }
            }
            LOG_NOTE("cheats: find done, %d hit(s)", hits);
        }
        else if (op == "watchr") {
            // "!watchr <hex address>": report each guest function that reads [addr, addr+16) (0 stops)
            u32 addr = 0;
            std::stringstream(text.substr(8)) >> std::hex >> addr;
            debug_watch_write(addr, 0, true);
        }
        else if (op == "watch") {
            // "!watch <hex address> [hex function to ignore]"
            u32 addr = 0, skip = 0;
            std::stringstream(text.substr(7)) >> std::hex >> addr >> skip;
            debug_watch_write(addr, skip);
        }
        else if (op == "pad") {
            // "!pad lx ly [rx ry buttons1 buttons2]": hold the sticks (-127..127) for headless tests
            int rx = 0, ry = 0, d1 = 0, d2 = 0;
            cmd >> rx >> ry >> d1 >> d2;
            PadState st;
            st.lx = u8(128 + a); st.ly = u8(128 + b); st.rx = u8(128 + rx); st.ry = u8(128 + ry);
            st.digital1 = u16(d1); st.digital2 = u16(d2);
            hold_test_pad(st);
            filter_pad(st);
            pad_set_state(0, st);
        }
        else if (op == "status") {
            const Snapshot sn = snapshot();
            std::string w;
            for (auto& [id, lv] : sn.weapon_level) w += " " + std::to_string(id) + ":V" + std::to_string(lv);
            LOG_NOTE("cheats: status in_level=%d level=%d armor=%d bolts=%d health=%d weapons%s", sn.in_level, sn.level, sn.armor,
                     sn.bolts, sn.health, w.c_str());
            for (auto& [id, ms] : sn.mods) {
                std::string m;
                for (auto& x : ms) m += " [" + std::string(x.owned ? "x" : " ") + "] " + x.name;
                LOG_NOTE("cheats: upgrades of %d:%s", id, m.c_str());
            }
            LOG_NOTE("cheats: raritanium %d", sn.raritanium);
            for (auto& k : sn.skills) LOG_NOTE("cheats: skill %d %s [%s] %s", k.id, k.done ? "done" : "-", k.name.c_str(), k.desc.c_str());
        }
        return;
    }
    LOG_NOTE("cheats: running %s", path.c_str());
    run_lua(text);
}

void on_script_tick(PPUContext& c, u8* base) {
    const u32 owner = ld32(base, u32(c.r[3]) + 4), L = owner ? ld32(base, owner + 4) : 0;
    if (!L) return;
    const PPUThread* t = current_thread();
    if (t && g_script_thread != t->id) {
        LOG_NOTE("cheats: script updates run on thread %s", t->name.c_str());
        g_script_thread = t->id;
    }
    t_ctx = &c;
    // once per frame: let the cheat state queue its periodic work
    static u64 last_flip = ~0ull;
    const u64 flip = rsx::flip_count();
    if (flip != last_flip) {
        last_flip = flip;
        if (hooks().frame) hooks().frame();
        poll_command_file();
        coop::tick();
    }
    if (g_pending) {
        std::deque<std::string> q;
        {
            std::lock_guard lk(g_lock);
            q.swap(g_queue);
            g_pending = false;
        }
        for (auto& chunk : q) {
            g_capture = true;
            execute_now(c, base, L, chunk);
            g_capture = false;
        }
    }
    t_ctx = nullptr;
}

}  // namespace

void set_hooks(const Hooks& h) { hooks() = h; }

void run_lua(std::string chunk) {
    std::lock_guard lk(g_lock);
    g_queue.push_back(std::move(chunk));
    g_pending = true;
}

// "!pad", "!co pad2" and "!shot" from the command file at each flip, so they also reach the pause menu (where the
// script update that polls the file doesn't run)
void poll_input_commands() {
    const std::string path = g_config.data_dir + "/lua_commands.txt";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    std::ifstream f(path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    const bool pad2 = text.rfind("!co pad2", 0) == 0;   // player 2's pad, for player 2's pause menu
    if (text.rfind("!pad ", 0) != 0 && text.rfind("!shot", 0) != 0 && !pad2) return;
    std::filesystem::rename(path, path + ".done", ec);
    if (ec) return;
    if (pad2) { coop::pad2_command(text.substr(4)); return; }
    if (text.rfind("!shot", 0) == 0) { gfx::g.screenshot = true; return; }
    std::stringstream cmd(text.substr(5));
    int lx = 0, ly = 0, rx = 0, ry = 0, d1 = 0, d2 = 0;
    cmd >> lx >> ly >> rx >> ry >> d1 >> d2;
    PadState st;
    st.lx = u8(128 + lx); st.ly = u8(128 + ly); st.rx = u8(128 + rx); st.ry = u8(128 + ry);
    st.digital1 = u16(d1); st.digital2 = u16(d2);
    hold_test_pad(st);
    filter_pad(st);
    pad_set_state(0, st);
}

bool script_thread_seen() { return g_script_thread != 0; }

u32 hero_moby() { return g_hero_moby; }

u64 call_code(u32 code, u32 toc, std::initializer_list<u64> args) {
    PPUFunc f = ppu_lookup(code);
    if (!t_ctx || !f) return 0;
    PPUContext& c = *t_ctx;
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

u64 call_code_f1(u32 code, u32 toc, std::initializer_list<u64> args, double f1) {
    PPUFunc f = ppu_lookup(code);
    if (!t_ctx || !f) return 0;
    PPUContext& c = *t_ctx;
    PPUContext saved = c;
    c.r[1] = (c.r[1] - 0x800) & ~0xFull;
    int i = 3;
    for (u64 a : args) c.r[i++] = a;
    c.f[1] = f1;
    c.r[2] = toc;
    c.lr = 0;
    f(c, mem::base);
    const u64 r = c.r[3];
    c = saved;
    return r;
}

double call_code_f(u32 code, u32 toc, std::initializer_list<u64> args) {
    PPUFunc f = ppu_lookup(code);
    if (!t_ctx || !f) return 0;
    PPUContext& c = *t_ctx;
    PPUContext saved = c;
    c.r[1] = (c.r[1] - 0x800) & ~0xFull;
    int i = 3;
    for (u64 a : args) c.r[i++] = a;
    c.r[2] = toc;
    c.lr = 0;
    f(c, mem::base);
    const double r = c.f[1];
    c = saved;
    return r;
}

u64 call(u32 opd, std::initializer_list<u64> args) {
    if (!t_ctx) return 0;
    PPUContext& c = *t_ctx;
    PPUContext saved = c;
    c.r[1] = (c.r[1] - 0x800) & ~0xFull;
    guest_call(c, opd, args);
    const u64 r = c.r[3];
    c = saved;
    return r;
}

void on_output(const std::string& s) {
    if (hooks().output) hooks().output(s);
    else LOG_NOTE("lua: %s", s.c_str());
}

}  // namespace cheats

bool ppu_host_hook(PPUContext& c, u8* base, u32 site) {
    switch (site) {
    case 0x293C70:
        cheats::on_script_tick(c, base);
        return false;
    case 0x280DD8:
        // get_pos(Ratchet1) at the start of a queued chunk: the first moby looked up is the hero
        if (cheats::g_capture) {
            if (cheats::g_hero_moby.exchange(u32(c.r[9])) != u32(c.r[9])) LOG_NOTE("cheats: hero moby at 0x%08X", u32(c.r[9]));
            cheats::g_capture = false;
        }
        return false;
    case 0x48C910:
        cheats::on_camera(base, u32(c.r[25]));
        // the camera updates even while the world is frozen (photo mode) and script updates stop
        cheats::poll_command_file();
        return false;
    case 0x23E608:
        cheats::on_hero_update(base, u32(c.r[3]));
        skins::on_hero_update(u32(c.r[3]));
        abilities::on_hero_update(u32(c.r[3]));
        return false;
    case 0x1E1298:
    case 0x1E1120:
        coop::on_slot_lookup(c);
        return false;
    case 0x639948:
        coop::on_set_fov(c);
        return false;
    case 0x658E90:
        coop::on_hero_view(c);
        return false;
    case 0x4E8D18:
        coop::on_sway(c, false);
        return false;
    case 0x4E8E94:
        coop::on_sway(c, true);
        return false;
    case 0x65CD00:
        coop::on_widget_draw(c);
        return false;
    case 0x65E898:
    case 0x661A14:
        coop::on_hud_draw(c, site == 0x661A14);
        return false;
    case 0x2D16E0:
        coop::on_level_change();
        return false;
    case 0x2A58C:
    case 0x1E21A0:
    case 0x24B08:
    case 0x24B38:
    case 0x25B90:
    case 0x27AA0:
    case 0x26FC0:
    case 0x25B68:
    case 0x25B40:
    case 0x25B18:
        return skins::on_menu_hook(c, site);
    case 0x4BE238:
        coop::on_dispatch(c);
        return false;
    case 0x2F938:
    case 0x33B50:
    case 0x74F0F4:
        return weapon_xp::on_hook(c, site);
    default:
        return false;
    }
}
