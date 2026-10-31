// tod: runs the statically recompiled Ratchet & Clank Future: Tools of Destruction (BCUS98127).
#include "cheats.h"
#include "coop.h"
#include "weapon_xp.h"
#include "fs.h"
#include "gfx.h"
#include "skins.h"
#include "guest.h"
#include "platform.h"
#include "rsx.h"
#ifdef TOD_RENDERER
#include "vk_backend.h"
bool run_windowed(const std::function<int()>& game, bool vsync);
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>

void register_import_names();
void register_sysPrxForUser();
void register_misc_modules();
void register_spurs();
void register_gcm();
void register_io_modules();
void register_savedata();
void start_pad_script();

void register_all_modules() {
    register_import_names();
    register_sysPrxForUser();
    fs_register_imports();
    register_misc_modules();
    register_spurs();
    register_gcm();
    register_io_modules();
    register_savedata();
}

// SHA-256, to tell the EBOOT the code was generated from from another file of the same size.
static std::string sha256_hex(const std::vector<u8>& d) {
    static const u32 K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<u8> m(d);
    const u64 bits = u64(d.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(u8(bits >> (i * 8)));
    auto rotr = [](u32 x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        u32 w[64];
        for (int i = 0; i < 16; ++i) w[i] = u32(m[o + 4 * i]) << 24 | u32(m[o + 4 * i + 1]) << 16 | u32(m[o + 4 * i + 2]) << 8 | m[o + 4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return out;
}

static void load_elf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<u8> elf((std::istreambuf_iterator<char>(f)), {});
    if (elf.size() != 10923400)
        throw std::runtime_error("EBOOT.elf is " + std::to_string(elf.size()) + " bytes, not the decrypted BCUS98127 02.00 build (10923400 bytes)."
                                 " The v1.00 disc and game updates are not supported.");
    // the generated code only works with the exact EBOOT it was made from
    static const char* kExpected = "0ee9a8414c8fc182bc19fa2a523e6d050d0be76138b3733ae515798d77d2468c";
    static const char* kDiscV100 = "b04e0fb3f50311461316b1609bcf08ae889551a94d4262922fe1b57f3386a23b";
    const std::string sum = sha256_hex(elf);
    if (sum == kDiscV100)
        throw std::runtime_error("EBOOT.elf (" + path + ") is from the v1.00 disc. Only the v2.00 disc (PARAM.SFO VERSION 02.00) "
                                 "is supported: the recompiled code is tied to that EBOOT.");
    if (sum != kExpected)
        throw std::runtime_error("EBOOT.elf (" + path + ") is not the expected BCUS98127 02.00 build: SHA-256 " + sum +
                                 ", expected " + kExpected + ". Decrypt the disc's PS3_GAME/USRDIR/EBOOT.BIN again with RPCS3.");
    auto be = [&](size_t o, int n) { u64 v = 0; for (int i = 0; i < n; ++i) v = v << 8 | elf[o + i]; return v; };
    for (int i = 0; i < 8; ++i) {
        const size_t o = 64 + 56 * i;
        const u64 off = be(o + 8, 8), va = be(o + 16, 8), fs = be(o + 32, 8), ms = be(o + 40, 8);
        if (be(o, 4) == 1 && ms) {
            mem::map(u32(va), u32(ms));
            std::memcpy(mem::base + va, elf.data() + off, fs);
        }
    }
}

// settings.ini in the data folder: created with the defaults on first run, read on every start.
// Settings added in later versions are appended (with their comment) to an older file.
struct SettingDefault { const char* key; const char* text; };
const SettingDefault kSettings[] = {
    {"skip_intro", "# Skip the studio logo movies and the legal screens (true/false).\n"
                   "# Story movies always play; press START to skip one.\n"
                   "skip_intro = true\n"},
    {"framerate", "# 30 (as on PS3) or 60 (the game's own 60 fps mode; needs a fast PC).\n"
                  "# Above 60 (up to 240, e.g. 120 or 144) the game times each frame itself and caps at that\n"
                  "# rate; 0 = no cap. Set vsync = false to go beyond your monitor's refresh rate.\n"
                  "framerate = 30\n"},
    {"vsync", "# Wait for the monitor's refresh when showing a frame (true/false).\n"
              "vsync = true\n"},
    {"aspect", "# Picture shape: 16:9 (as on PS3), 21:9 or 32:9 for ultrawide monitors.\n"
               "aspect = 16:9\n"},
    {"renderer", "# Graphics renderer: native (faster, draws on its own thread) or legacy.\n"
                 "renderer = native\n"},
    {"resolution_scale", "# Native renderer only: draw at 1, 2, 3 or 4 times the PS3's 1280x720 (the picture is then\n"
                         "# scaled down to the window, which also smooths edges). The F1 menu's Graphics tab changes it live.\n"
                         "resolution_scale = 1\n"},
    {"antialiasing", "# Native renderer only: fxaa (smooths jagged edges, almost free) or off.\n"
                     "antialiasing = fxaa\n"},
    {"anisotropic_filtering", "# Native renderer only: sharper textures on floors and walls seen at an angle:\n"
                              "# 0 (as the game asks), 2, 4, 8 or 16.\n"
                              "anisotropic_filtering = 8\n"},
    {"soft_shadows", "# Native renderer only: smoother shadow edges (true) or the PS3's own filtering (false).\n"
                     "soft_shadows = true\n"},
    {"async_shaders", "# Native renderer only: build new shaders in the background (true) instead of pausing\n"
                      "# the game while they build (false). Objects can appear a moment late the first time.\n"
                      "async_shaders = true\n"},
    {"rumble", "# Controller rumble strength in percent: 0 (off) to 100. The game's own Vibration option also applies.\n"
               "rumble = 100\n"},
    {"texture_dump", "# Texture packs: PNGs named by a texture's hash in <data>/textures replace the game's textures.\n"
                     "# true saves every texture the game draws to <data>/textures/dump, to find and edit them.\n"
                     "texture_dump = false\n"},
    {"cheat_menu", "# Cheat and debug menu: F1 on the keyboard or L3+R3 on a controller opens it (true/false).\n"
                   "cheat_menu = true\n"},
    {"god_mode", "# Cheats switched on at start (the menu can change them while playing).\n"
                 "god_mode = false\n"},
    {"infinite_ammo", "infinite_ammo = false\n"},
    {"no_clank", "# Ratchet without Clank on his back, like the Reepor section of Cragmite Ruins.\n"
                 "no_clank = false\n"},
    {"gelanator_mod", "# Fun mod: the Gelanator is always in your inventory and always full of gel.\n"
                      "gelanator_mod = false\n"},
    {"weapon_xp", "# The pause menu's weapons page shows the selected weapon's XP and the XP its next level needs, as\n"
                  "# the first line of its description (XP: 700 / 1,000), in the game's own text.\n"
                  "weapon_xp = true\n"},
    {"coop", "# Two-player co-op (experimental): a second Ratchet on a second controller, split screen top and\n"
             "# bottom. Weapons, ammo, bolts, XP and health are shared; the players can't hold the same weapon\n"
             "# (the Omniwrench excepted); D-pad Down teleports you to your partner. With one controller, the\n"
             "# keyboard plays player 1. The F1 menu's Fun tab switches it too.\n"
             "coop = false\n"},
    {"skin", "# Skin packs: every folder in <data>/skins (e.g. my_skin) is listed in Extras > Skins after the\n"
             "# game's own skins. skin = the pack you wear (the menu sets it); empty = a game skin.\n"
             "skin =\n"},
    {"skin2", "# Co-op player 2's skin: a pack name or a game skin number (0 = Ratchet, 1-8); empty = as player 1.\n"
              "skin2 =\n"},
    {"save_folder", "# Folder for your saves (one BCUS98127_SAVE_<n> folder per save; copy them to back up).\n"
                    "# Empty = the \"TOD Saves\" folder next to the AppImage.\n"
                    "save_folder =\n"},
};

// Save folder: created if needed. Saves from the old place inside the data folder are copied over once
// (the old copies stay as a backup, with a note saying where the saves went).
void prepare_save_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = g_config.save_dir, old = g_config.data_dir + "/dev_hdd0/home/00000001/savedata";
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "Warning: can't create the save folder " << dir << " (" << ec.message() << "), using the data folder\n";
        g_config.save_dir = old.string();
        fs::create_directories(old, ec);
    }
    std::cerr << "Saves: " << g_config.save_dir << "\n";
    if (fs::equivalent(dir, old, ec) || !fs::is_directory(old, ec) || fs::exists(old / "SAVES_MOVED.txt", ec)) return;
    int copied = 0;
    for (fs::directory_iterator it(old, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!it->is_directory() || name.empty() || name[0] == '.' || fs::exists(dir / name)) continue;
        std::error_code cec;
        fs::copy(it->path(), dir / name, fs::copy_options::recursive, cec);
        if (cec) std::cerr << "Warning: couldn't copy save " << name << ": " << cec.message() << "\n";
        else ++copied;
    }
    if (std::ofstream note{old / "SAVES_MOVED.txt"})
        note << "TOD Recomp now keeps saves in " << g_config.save_dir << "\nThe folders here are old copies.\n";
    if (copied) std::cerr << "Copied " << copied << " save(s) from " << old << "\n";
}

gfx::Options gfx::g;

// Rewrites the `key = value` line in settings.ini (or appends one), keeping everything else.
void gfx::save_setting(const std::string& key, const std::string& value) {
    static std::mutex lock;
    std::lock_guard lk(lock);
    const std::string path = g_config.data_dir + "/settings.ini";
    std::ifstream in(path);
    std::string line, out;
    bool done = false;
    while (std::getline(in, line)) {
        const size_t a = line.find_first_not_of(" \t");
        if (!done && a != std::string::npos && line.compare(a, key.size(), key) == 0) {
            const size_t e = line.find_first_not_of(" \t", a + key.size());
            if (e != std::string::npos && line[e] == '=') { line = key + " = " + value; done = true; }
        }
        out += line + "\n";
    }
    in.close();
    if (!done) out += key + " = " + value + "\n";
    const std::string tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, path, ec);
    }
}

std::string gfx::screenshot_path() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_config.data_dir + "/screenshots";
    if (const std::string home = plat::home_dir(); !home.empty() && fs::is_directory(fs::path(home) / "Pictures", ec))
        dir = fs::path(home) / "Pictures" / "TOD Recomp";
    fs::create_directories(dir, ec);
    const std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
    fs::path p = dir / (std::string("tod_") + stamp + ".png");
    for (int n = 2; fs::exists(p, ec); ++n) p = dir / (std::string("tod_") + stamp + "_" + std::to_string(n) + ".png");
    return p.string();
}

void load_settings(bool& vsync) {
    const std::string path = g_config.data_dir + "/settings.ini";
    if (!std::filesystem::exists(path)) {
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fputs("# TOD Recomp settings. Edit, save, and start the game again.\n", f);
            for (const auto& d : kSettings) { std::fputs("\n", f); std::fputs(d.text, f); }
            std::fclose(f);
        }
    }
    {
        std::ifstream old(path);
        std::string text((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        std::string add;
        for (const auto& d : kSettings) {
            bool found = false;
            for (size_t at = text.find(d.key); at != std::string::npos && !found; at = text.find(d.key, at + 1)) {
                const size_t bol = text.rfind('\n', at);
                const size_t ls = bol == std::string::npos ? 0 : bol + 1;
                found = text.find_first_not_of(" \t", ls) == at && text.find('#', ls) > at;
            }
            if (!found) { add += "\n"; add += d.text; }
        }
        if (!add.empty())
            if (FILE* f = std::fopen(path.c_str(), "a")) { std::fputs(add.c_str(), f); std::fclose(f); }
    }
    std::ifstream in(path);
    std::string line;
    std::map<std::string, std::string> cheat_kv;
    // the skin packs are listed even when settings.ini has no "skin =" line (a file with only skin2 set)
    skins::configure("", g_config.data_dir);
    auto trim = [](std::string v) {
        const size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    auto boolean = [](const std::string& v) { return v == "true" || v == "1" || v == "yes" || v == "on"; };
    while (std::getline(in, line)) {
        if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "skip_intro") g_config.skip_intro = boolean(v);
        else if (k == "framerate") {
            const int f = std::atoi(v.c_str());
            g_config.framerate = (v == "unlimited" || f == 0) ? 0 : f > 60 ? std::min(f, 240) : f >= 60 ? 60 : 30;
        }
        else if (k == "vsync") vsync = vsync && boolean(v);
        else if (k == "aspect") {
            const size_t c = v.find(':');
            const float w = std::strtof(v.c_str(), nullptr), h = c == std::string::npos ? 1.f : std::strtof(v.c_str() + c + 1, nullptr);
            if (w > 0 && h > 0 && w / h >= 1.2f && w / h <= 4.f) g_config.aspect = w / h;
            else std::cerr << "settings.ini: aspect '" << v << "' not understood, using 16:9\n";
        }
        else if (k == "renderer") g_config.native_renderer = v != "legacy";
        else if (k == "resolution_scale") { g_config.resolution_scale = std::clamp(std::atoi(v.c_str()), 1, 4); gfx::g.resolution_scale = g_config.resolution_scale; }
        else if (k == "antialiasing") gfx::g.aa = (v == "off" || v == "none" || v == "false") ? gfx::AA_OFF : gfx::AA_FXAA;
        else if (k == "anisotropic_filtering") {
            const int a = std::atoi(v.c_str());
            gfx::g.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0;
        }
        else if (k == "soft_shadows") gfx::g.soft_shadows = boolean(v);
        else if (k == "async_shaders") g_config.async_shaders = boolean(v);
        else if (k == "rumble") gfx::g.rumble = (v == "false" || v == "off") ? 0 : v == "true" || v == "on" ? 100 : std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "texture_dump") gfx::g.texture_dump = boolean(v);
        else if (k == "cheat_menu") g_config.cheat_menu = boolean(v);
        else if (k == "skin") skins::configure(v, g_config.data_dir);
        else if (k == "skin2") skins::configure2(v);
        else if (k == "coop") gfx::g.coop = boolean(v);
        else if (k == "weapon_xp") weapon_xp::configure(boolean(v));
        else if (k == "god_mode" || k == "infinite_ammo" || k == "no_clank" || k == "gelanator_mod") cheat_kv[k] = v;
        else if (k == "save_folder") {
            if (v.size() >= 2 && v[0] == '"' && v.back() == '"') g_config.save_dir = v.substr(1, v.size() - 2);
            else if (!v.empty()) g_config.save_dir = v;
            if (g_config.save_dir.rfind("~/", 0) == 0 && !plat::home_dir().empty()) g_config.save_dir = plat::home_dir() + g_config.save_dir.substr(1);
        }
        else if (!k.empty()) std::cerr << "settings.ini: unknown setting '" << k << "'\n";
    }
    std::cerr << "Settings (" << path << "): skip_intro=" << (g_config.skip_intro ? "true" : "false")
              << " framerate=" << g_config.framerate << " vsync=" << (vsync ? "true" : "false")
              << " aspect=" << g_config.aspect << " renderer=" << (g_config.native_renderer ? "native" : "legacy")
              << " resolution_scale=" << g_config.resolution_scale
              << " antialiasing=" << (gfx::g.aa == gfx::AA_FXAA ? "fxaa" : "off") << " anisotropic_filtering=" << gfx::g.anisotropy
              << " soft_shadows=" << (gfx::g.soft_shadows ? "true" : "false")
              << " async_shaders=" << (g_config.async_shaders ? "true" : "false")
              << " cheat_menu=" << (g_config.cheat_menu ? "true" : "false") << "\n";
    cheats::apply_settings(cheat_kv);
}

// Started with no paths (e.g. double-clicked): look next to the executable for the game folder (the one
// holding PS3_GAME, or one folder down) and a decrypted EBOOT.elf in PS3_GAME/USRDIR or beside the executable.
// The data and save folders then also go next to the executable.
bool find_game_next_to_exe(std::string& saves_arg) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string exe = plat::exe_dir();
    if (exe.empty()) return false;
    auto is_game = [&](const fs::path& p) { return fs::is_directory(p / "PS3_GAME" / "USRDIR", ec); };
    if (g_config.game_dir.empty()) {
        if (is_game(exe)) g_config.game_dir = exe;
        else
            for (const auto& e : fs::directory_iterator(exe, ec))
                if (e.is_directory(ec) && is_game(e.path())) { g_config.game_dir = e.path().string(); break; }
    }
    if (g_config.elf_path.empty())
        for (const fs::path& p : {fs::path(g_config.game_dir) / "PS3_GAME" / "USRDIR" / "EBOOT.elf", fs::path(exe) / "EBOOT.elf"})
            if (!g_config.game_dir.empty() && fs::is_regular_file(p, ec)) { g_config.elf_path = p.string(); break; }
    if (g_config.elf_path.empty() || g_config.game_dir.empty()) return false;
    if (g_config.data_dir.empty()) g_config.data_dir = (fs::path(exe) / "tod-data").string();
    if (saves_arg.empty()) saves_arg = (fs::path(exe) / "TOD Saves").string();
    std::cerr << "game: " << g_config.game_dir << "\nelf:  " << g_config.elf_path << "\ndata: " << g_config.data_dir << "\n";
    return true;
}

// A disc copy missing files the game reads at boot fails later in odd ways (a memory fault while
// loading, "Auto-save failed"); name the missing files up front instead.
void check_game_files() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path usr = fs::path(g_config.game_dir) / "PS3_GAME" / "USRDIR";
    if (!fs::is_directory(usr, ec)) usr = fs::path(g_config.game_dir) / "USRDIR";  // --game pointing at PS3_GAME itself
    std::string missing;
    for (const char* f : {"EBOOT.BIN", "packed/game/game.psarc", "packed/game/global_cached.psarc", "packed/game/global_uncached.psarc"})
        if (!fs::is_regular_file(usr / f, ec)) missing += std::string("\n  ") + (usr / f).string();
    if (!missing.empty())
        std::cerr << "Warning: these game files are missing, so the game may crash or fail to save:" << missing
                  << "\nCopy the whole PS3_GAME folder from your disc dump.\n";
}

int run(int argc, char** argv);

int main(int argc, char** argv) {
    const int rc = run(argc, argv);
    if (rc != 0 && !plat::show_stopped(rc) && plat::own_console()) {  // keep a double-clicked window open long enough to read why
        std::cerr << "\nPress Enter to close this window.\n";
        std::cin.get();
    }
    return rc;
}

int run(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool headless = false, vsync = true;
    std::string saves_arg;
    // Arguments after "--" are passed to the game itself (its own switches such as -60fps,
    // -no_movies, -no_legal, -level <name>).
    std::vector<std::string> guest_argv{"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"};
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") { guest_argv.insert(guest_argv.end(), args.begin() + i + 1, args.end()); break; }
        if (a == "--game" && i + 1 < args.size()) g_config.game_dir = args[++i];
        else if (a == "--data" && i + 1 < args.size()) g_config.data_dir = args[++i];
        else if (a == "--saves" && i + 1 < args.size()) saves_arg = args[++i];
        else if (a == "-v") g_log_level = 2;
        else if (a == "-q") g_log_level = 0;
        else if (a == "--headless") headless = true;
        else if (a == "--no-vsync") vsync = false;
        else if (g_config.elf_path.empty()) g_config.elf_path = a;
    }
    if ((g_config.elf_path.empty() || g_config.game_dir.empty()) && !find_game_next_to_exe(saves_arg)) {
        std::cerr << "usage: tod EBOOT.elf --game <folder with PS3_GAME> [--data <cache folder>] [--saves <save folder>] [--headless] [--no-vsync] [-v|-q] [-- game arguments]\n"
                     "\nWith no arguments, tod looks next to itself for a folder containing PS3_GAME (your own copy of\n"
                     "Ratchet & Clank Future: Tools of Destruction, BCUS98127) and a decrypted EBOOT.elf in PS3_GAME/USRDIR.\n"
                     "See README.md for how to dump the disc and decrypt EBOOT.BIN.\n";
        return 2;
    }
    if (g_config.data_dir.empty()) g_config.data_dir = "tod-data";
    std::filesystem::create_directories(g_config.data_dir + "/dev_hdd0");
    plat::log_to_file(g_config.data_dir + "/tod.log");
    std::filesystem::create_directories(g_config.data_dir + "/dev_hdd1");
    load_settings(vsync);
    check_game_files();
    // save_folder in settings.ini, else --saves (the AppImage passes "TOD Saves" next to itself), else <data>/saves
    if (g_config.save_dir.empty()) g_config.save_dir = saves_arg.empty() ? g_config.data_dir + "/saves" : saves_arg;
    prepare_save_dir();
    // the game's own switches for what the settings ask for, ahead of any given after "--"
    std::vector<std::string> sw;
    if (g_config.skip_intro) sw.push_back("-no_legal");
    if (g_config.framerate == 60) sw.push_back("-60fps");
    // above 60 (or no cap): the game's own -capfps, which keeps its fixed 30 fps step setting but
    // caps the frame time instead, and then runs each frame with the measured time step
    if (g_config.framerate > 60 || g_config.framerate == 0) {
        sw.push_back("-capfps");
        sw.push_back(std::to_string(g_config.framerate ? g_config.framerate : 1000));
    }
    guest_argv.insert(guest_argv.begin() + 1, sw.begin(), sw.end());
    try {
        mem::reserve();
        load_elf(g_config.elf_path);
        // widescreen: the 16:9 constant the game picks for a 16:9 TV (TOC entry read at 0x501f14
        // and 0x23023c) becomes the chosen aspect; the picture is then shown at that shape
        if (std::abs(g_config.aspect - 16.f / 9.f) > 0.01f) {
            u32 bits;
            std::memcpy(&bits, &g_config.aspect, 4);
            st32(mem::base, 0x8a73fc, bits);
        }
        register_all_modules();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    if (guest_argv.size() > 1) {
        std::string list;
        for (size_t i = 1; i < guest_argv.size(); ++i) list += " " + guest_argv[i];
        std::cerr << "Game arguments:" << list << "\n";
    }
    auto game = [guest_argv] { return run_main_thread(0x859418, guest_argv, {}); };
    // at each flip, windowed or not: the command file's pad input, and co-op telling a paused game from a running one
    rsx::on_flip = [](u32) { cheats::poll_input_commands(); coop::note_flip(); };
    start_pad_script();
#ifdef TOD_RENDERER
    if (!headless && !std::getenv("TOD_NO_GPU")) {
        // windowed: the guest runs on its own thread while this one pumps window events
        std::fflush(stdout);
        if (run_windowed(game, vsync)) { std::fflush(stdout); std::fflush(stderr); std::_Exit(0); }
        std::cerr << "Warning: no window available, running headless\n";
    }
    if (!std::getenv("TOD_NO_GPU")) {
        if (rsx::Renderer* r = rsx::create_renderer(nullptr)) rsx::set_renderer(r);
        else std::cerr << "Warning: running without a renderer\n";
    }
#endif
    const int rc = run_main_thread(0x859418, guest_argv, {});
    std::fflush(stdout);
    std::fflush(stderr);
    if (rc != 0) plat::show_stopped(rc);
    std::_Exit(rc);  // other guest threads are still running; skip static teardown
}
