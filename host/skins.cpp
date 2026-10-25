// Skin packs: replacement models for Ratchet, as new entries in the game's skins menu.
//
// Every <data>/skins/<name>/skin.bin (meshes already in the game's vertex format, written by
// tools/modding/tod_skin.py) is a skin of its own. The skins menu (pause > Extras > Skins, Anark/Lua) lists
// the skins 0..SKIN_TYPE_COUNT-1 through eight functions (is_skin_available, is_skin_owned, get_skin_cost,
// get_skin_name, get_skin_desc, get_skin_movie, select_skin, purchase_skin). SKIN_TYPE_COUNT is raised
// by the number of packs and those functions answer for the ids past the game's nine, so the menu shows
// the packs after Mustachio, owned and free, with their own name, description and preview movie.
//
// Each game skin is one of Ratchet's 49 mesh groups (Dan 41, Snowman 42, Cragmite 43, Rusty Pete 44,
// Cronk 45, Zephyr 46, Convict 47), drawn instead of his body and armour. A pack is built against one of
// them, its carrier: wearing the pack selects the carrier skin and writes the pack's meshes over the
// carrier group in guest memory, live, where the class sits once the level is loaded (the hero moby's
// class pointer at +0x54). Everything written is saved first and put back when the pack comes off, so the
// carrier skin itself still looks as it should. A mesh bigger than the carrier's own is moved into the
// room of mesh groups nobody wears at that moment (the other skins' groups), so a pack can have far more
// vertices and triangles than Ratchet. The pack's textures (textures/, named by the carrier textures'
// hashes) only apply while it is worn.
//
// Co-op: player 2 can wear another skin (skin2 in settings.ini, or the overlay). The game reads the
// selected skin through sub_1E21A0; during player 2's update that returns player 2's choice.
#include "skins.h"
#include "cheats.h"
#include "coop.h"
#include "gfx.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace skins {
namespace {

constexpr int kGameSkins = kGameSkinCount;      // SKIN_NONE .. SKIN_MUSTACIO
constexpr u32 kFrontEnd = 0x888624;            // *: +0x45C + 4n owned, +0x480 selected skin
const int kCarrierGroup[kGameSkins] = {-1, 41, 42, 43, 44, 45, 46, 47, -1};
const char* const kGameNames[kGameSkins] = {"Ratchet", "Dan Johnson", "Snowman", "Cragmite", "Rusty Pete", "Cronk", "Zephyr", "Convict", "Mustachio"};
// the skins menu plays built/movies/<skin>/<skin>[.<language>].bik for the highlighted skin
const char* const kMovies[kGameSkins] = {"", "skin_dan", "skin_snowman", "skin_cragmite", "skin_rusty_pete",
                                         "skin_cronk", "skin_zephyr", "skin_jailbird", "skin_mustacio"};

constexpr u16 kWrench = 0x8000;                // Mesh::group bit: a mesh of the wrench's class, not Ratchet's

struct Mesh {
    u16 group, mesh;              // group | kWrench: the wrench (each group is one wrench look)
    u32 orig_nv, orig_nidx, orig_flags;
    u32 nv, nidx;
    std::vector<u16> palette;
    std::vector<u8> verts, idx;   // vertex bytes (game format), u16 BE indices
};
struct Pack {
    std::string name, dir, title, desc;
    s32 carrier = -1;             // the game skin it is built against
    u32 abilities = 0;            // skins::kAbility* bits, gameplay that comes with the pack (abilities.cpp)
    std::vector<Mesh> meshes;
    std::map<u64, std::string> textures;   // carrier texture hash -> PNG
    u32 vertices = 0, triangles = 0;
};
std::vector<Pack> g_packs;        // entry 9 + i
std::string g_data, g_cfg[2];     // settings.ini skin / skin2
bool g_scanned = false;

std::recursive_mutex g_lock;
int g_worn[2] = {-1, -1};         // entry worn by player 1 (a pack, or -1) / player 2 (any entry, -1 = as player 1)
bool g_dirty = true;              // the layout must be written again
u32 g_cls = 0;                    // Ratchet's class in guest memory the layout is written into
s32 g_last_sel = -1;              // player 1's game skin when the layout was written
struct Saved { u32 ea; std::vector<u8> bytes; };
std::vector<Saved> g_saved;       // original bytes under everything written, newest last
// what was written, to notice the game putting its own data back over it (the skins menu has the picked
// skin's meshes loaded after it closes, over a pack laid out on that skin): one per g_saved entry, with the
// bytes each region held once the whole layout was in
struct Written { u32 ea; std::vector<u8> bytes; };
std::vector<Written> g_written;
int g_check = 0;                  // hero updates until the next check
std::map<u64, std::string> g_tex; // textures of the worn packs
std::atomic<u32> g_tex_gen{1};
std::atomic<int> g_request[2] = {-2, -2};   // wear() from another thread, taken at the next hero update
std::map<std::string, u32> g_strings;       // guest copies of menu strings

std::string upper(std::string s) { for (char& ch : s) ch = char(std::toupper(u8(ch))); return s; }
std::string lower(std::string s) { for (char& ch : s) ch = char(std::tolower(u8(ch))); return s; }

bool load_pack(const std::string& dir, Pack& pk) {
    std::ifstream f(dir + "/skin.bin", std::ios::binary);
    if (!f) return false;
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    size_t p = 0;
    auto need = [&](size_t n) { return p + n <= d.size(); };
    auto r32 = [&] { u32 v; std::memcpy(&v, &d[p], 4); p += 4; return v; };   // little endian
    auto r16 = [&] { u16 v; std::memcpy(&v, &d[p], 2); p += 2; return v; };
    const bool v2 = need(8) && std::memcmp(d.data(), "TODSKIN2", 8) == 0;
    if (!need(16) || (!v2 && std::memcmp(d.data(), "TODSKIN1", 8) != 0)) { LOG_ERR("skins: %s/skin.bin is not a skin file", dir.c_str()); return false; }
    p = 8;
    const u32 n = r32();
    pk.carrier = s32(r32());
    if (v2) {
        if (!need(4 + 32 + 160)) return false;
        // flags: bit 0 was version 2's "select" (the pack is now picked in the menu or with skin = <name>),
        // the bits above it are the pack's abilities
        pk.abilities = r32() & ~1u;
        pk.title.assign(reinterpret_cast<const char*>(&d[p]), strnlen(reinterpret_cast<const char*>(&d[p]), 32));
        p += 32;
        pk.desc.assign(reinterpret_cast<const char*>(&d[p]), strnlen(reinterpret_cast<const char*>(&d[p]), 160));
        p += 160;
    }
    for (u32 i = 0; i < n; ++i) {
        Mesh m;
        if (!need(32)) return false;
        m.group = r16(); m.mesh = r16();
        m.orig_nv = r32(); m.orig_nidx = r32(); m.orig_flags = r32();
        m.nv = r32(); m.nidx = r32();
        const u32 np = r32();
        if (!need(np * 2 + 4)) return false;
        for (u32 k = 0; k < np; ++k) m.palette.push_back(r16());
        u32 len = r32();
        if (!need(len + 4)) return false;
        m.verts.assign(d.begin() + p, d.begin() + p + len); p += len;
        len = r32();
        if (!need(len)) return false;
        m.idx.assign(d.begin() + p, d.begin() + p + len); p += len;
        if (m.nv > 1) { pk.vertices += m.nv; pk.triangles += m.nidx / 3; }
        pk.meshes.push_back(std::move(m));
    }
    if (pk.carrier < 1 || pk.carrier >= kGameSkins || kCarrierGroup[pk.carrier] < 0) {
        LOG_ERR("skins: %s is built on game skin %d, which has no mesh group of its own; rebuild it with --game-skin 1..7", dir.c_str(), pk.carrier);
        return false;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir + "/textures", ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::string stem = it->path().stem().string();
        if (lower(it->path().extension().string()) != ".png" || stem.size() < 16) continue;
        char* end = nullptr;
        const std::string hex = stem.substr(0, 16);
        const u64 h = std::strtoull(hex.c_str(), &end, 16);
        if (end == hex.c_str() + 16) pk.textures[h] = it->path().string();
    }
    if (pk.title.empty()) { pk.title = pk.name; if (!pk.title.empty()) pk.title[0] = char(std::toupper(u8(pk.title[0]))); }
    return true;
}

int find_pack(const std::string& name) {
    for (size_t i = 0; i < g_packs.size(); ++i) if (g_packs[i].name == name) return kGameSkins + int(i);
    return -1;
}

void scan() {
    if (g_scanned || g_data.empty()) return;
    g_scanned = true;
    std::vector<std::string> dirs;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(g_data + "/skins", ec))
        if (e.is_directory(ec) && std::filesystem::exists(e.path() / "skin.bin", ec)) dirs.push_back(e.path().filename().string());
    std::sort(dirs.begin(), dirs.end());
    for (const std::string& n : dirs) {
        Pack pk;
        pk.name = n;
        pk.dir = g_data + "/skins/" + n;
        if (!load_pack(pk.dir, pk)) continue;
        LOG_NOTE("skins: entry %zu '%s' (%s): %zu meshes, %u vertices, %u triangles, on game skin %d, %zu textures", kGameSkins + g_packs.size(),
                 pk.title.c_str(), n.c_str(), pk.meshes.size(), pk.vertices, pk.triangles, pk.carrier, pk.textures.size());
        g_packs.push_back(std::move(pk));
    }
    for (int pl = 0; pl < 2; ++pl) {
        if (g_cfg[pl].empty()) continue;
        int e = find_pack(g_cfg[pl]);
        if (e < 0 && !g_cfg[pl].empty() && std::isdigit(u8(g_cfg[pl][0]))) e = std::atoi(g_cfg[pl].c_str());   // skin2 = <game skin number>
        if (e < 0) LOG_ERR("skins: no skin pack '%s' in %s/skins", g_cfg[pl].c_str(), g_data.c_str());
        else if (pl == 1 || e >= kGameSkins) g_worn[pl] = e;
    }
}

const Pack* pack_of(int entry) { return entry >= kGameSkins && entry - kGameSkins < int(g_packs.size()) ? &g_packs[entry - kGameSkins] : nullptr; }
// the game skin an entry draws (a pack's carrier)
int game_skin_of(int entry) { const Pack* p = pack_of(entry); return p ? p->carrier : entry; }

u32 front_end() {
    const u32 fe = ld32(mem::base, kFrontEnd);
    return fe && mem::is_mapped(fe + 0x45C, 0x28) ? fe : 0;
}

u32 guest_string(const std::string& text) {
    auto it = g_strings.find(text);
    if (it != g_strings.end()) return it->second;
    const u32 ea = mem::alloc_main(u32(text.size() + 16) & ~15u, 16);
    std::memset(mem::base + ea, 0, text.size() + 1);
    std::memcpy(mem::base + ea, text.data(), text.size());
    g_strings[text] = ea;
    return ea;
}

// ---- writing packs into Ratchet's class in guest memory
void write(u32 ea, const void* p, size_t n) {
    if (!n || !mem::is_mapped(ea, u32(n))) return;
    g_saved.push_back({ea, std::vector<u8>(mem::base + ea, mem::base + ea + n)});
    std::memcpy(mem::base + ea, p, n);
    g_written.push_back({ea, {}});
}
// after the whole layout is written (later writes may overlap earlier ones): what each region holds now
void seal() { for (Written& w : g_written) w.bytes.assign(mem::base + w.ea, mem::base + w.ea + g_saved[&w - g_written.data()].bytes.size()); }
// true when something the layout wrote no longer holds it
bool overwritten() {
    for (const Written& w : g_written)
        if (!mem::is_mapped(w.ea, u32(w.bytes.size())) || std::memcmp(mem::base + w.ea, w.bytes.data(), w.bytes.size()) != 0) return true;
    return false;
}
// the game wrote its own data over the layout: whatever it put there is now what lies under the pack (and comes
// back when the pack comes off), byte by byte, so a region the game only partly wrote keeps its other originals
void adopt() {
    for (size_t i = 0; i < g_written.size(); ++i) {
        const Written& w = g_written[i];
        if (!mem::is_mapped(w.ea, u32(w.bytes.size()))) continue;
        const u8* now = mem::base + w.ea;
        std::vector<u8>& orig = g_saved[i].bytes;
        for (size_t k = 0; k < w.bytes.size(); ++k) if (now[k] != w.bytes[k]) orig[k] = now[k];
    }
}
void write32(u32 ea, u32 v) { const u32 b = __builtin_bswap32(v); write(ea, &b, 4); }
void restore() {
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it)
        if (mem::is_mapped(it->ea, u32(it->bytes.size()))) std::memcpy(mem::base + it->ea, it->bytes.data(), it->bytes.size());
    g_saved.clear();
    g_written.clear();
}

// +0x19: the mesh group count (the byte before it is set at run time)
bool is_ratchet(u32 cls) {
    if (!cls || !mem::is_mapped(cls, 0x40) || mem::base[cls + 0x19] != 49) return false;
    const u32 skel = ld32(mem::base, cls + 0x20);
    return skel && mem::is_mapped(skel, 2) && ld16(mem::base, skel) == 281;
}

// the wrench's class (8 bones, 6 groups) among the level's classes, which sit 0xC0 bytes apart in main.dat
u32 find_wrench(u32 ratchet) {
    for (int k = 1; k < 600; ++k)
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const u32 c = ratchet + u32(sgn * k * 0xC0);
            if (!mem::is_mapped(c, 0xC0) || mem::base[c + 0x19] != 6) continue;
            const u32 skel = ld32(mem::base, c + 0x20);
            if (skel && mem::is_mapped(skel, 2) && ld16(mem::base, skel) == 8 && mem::is_mapped(ld32(mem::base, c + 0x28), 6 * 8)) return c;
        }
    return 0;
}

struct Span { u32 a, b; };
void add_span(std::vector<Span>& v, u32 a, u32 b) { if (b > a) v.push_back({a, b}); }
void merge(std::vector<Span>& v) {
    std::sort(v.begin(), v.end(), [](const Span& x, const Span& y) { return x.a < y.a; });
    std::vector<Span> o;
    for (const Span& s : v) {
        if (!o.empty() && s.a <= o.back().b + 16) o.back().b = std::max(o.back().b, s.b);
        else o.push_back(s);
    }
    v.swap(o);
}
// first fit, aligned; the span is cut out of the free list
bool alloc(std::vector<Span>& v, u32 size, u32 align, u32& at) {
    for (size_t i = 0; i < v.size(); ++i) {
        const u32 a = (v[i].a + align - 1) / align * align;
        if (a + size > v[i].b) continue;
        const Span rest{a + size, v[i].b};
        const bool head = a > v[i].a;
        if (head) v[i].b = a; else v.erase(v.begin() + long(i));
        if (rest.b > rest.a) v.insert(v.begin() + long(i) + (head ? 1 : 0), rest);
        at = a;
        return true;
    }
    return false;
}

struct MeshRef {
    u32 entry, istart, vbyte, w8, flags, nidx, nv, stride, icap;
    bool lods;
};
MeshRef mesh_ref(u32 gtab, int g, int m) {
    MeshRef r{};
    const u32 mp = ld32(mem::base, gtab + g * 8), mn = ld32(mem::base, gtab + g * 8 + 4);
    if (!mp || u32(m) >= mn || !mem::is_mapped(mp + m * 64, 64)) return r;
    r.entry = mp + m * 64;
    const u32 e = r.entry;
    r.istart = ld32(mem::base, e); r.vbyte = ld32(mem::base, e + 4); r.w8 = ld32(mem::base, e + 8);
    r.flags = ld32(mem::base, e + 12); r.nidx = ld32(mem::base, e + 16); r.nv = r.w8 & 0xffff;
    r.stride = ((r.flags >> 16) & 0xff) ? 28 : 20;
    r.icap = r.nidx;
    r.lods = (r.flags & 0xff) == 4;
    if (r.lods) for (int k = 0; k < 3; ++k) { const u32 x = ld32(mem::base, e + 0x14 + 4 * k); if (x) r.icap = std::max(r.icap, (x >> 16) + (x & 0xffff)); }
    return r;
}

void layout(u32 cls, s32 sel) {
    restore();
    const u32 gtab = ld32(mem::base, cls + 0x28), vraw = ld32(mem::base, cls + 0x38);
    // vertices in main memory (bit 31: skinned on the SPUs); index lists in RSX local memory
    const u32 vbase = (vraw & 0x80000000u) ? (vraw & 0x7fffffffu) + 0x50000000u : 0xC0000000u + vraw;
    const u32 ibase = 0xC0000000u + ld32(mem::base, cls + 0x34);
    // who draws which game skin's group
    int skin_of[2] = {g_worn[0] >= 0 ? game_skin_of(g_worn[0]) : sel, g_worn[1] >= 0 ? game_skin_of(g_worn[1]) : -1};
    if (skin_of[1] < 0) skin_of[1] = skin_of[0];
    std::vector<const Pack*> packs;
    for (int pl = 0; pl < 2; ++pl) {
        const Pack* p = pack_of(pl == 0 ? g_worn[0] : g_worn[1]);
        if (p && std::find(packs.begin(), packs.end(), p) == packs.end()) {
            bool clash = false;
            for (const Pack* q : packs) clash |= q->carrier == p->carrier;
            if (clash) { LOG_NOTE("skins: '%s' and another worn pack share game skin %d; player 2 shows player 1's", p->name.c_str(), p->carrier); continue; }
            packs.push_back(p);
        }
    }
    // room: the skin groups nobody draws now
    std::vector<Span> vfree, ifree;
    for (int s = 1; s < kGameSkins; ++s) {
        const int g = kCarrierGroup[s];
        if (g < 0 || s == skin_of[0] || s == skin_of[1]) continue;
        const u32 mn = ld32(mem::base, gtab + g * 8 + 4);
        for (u32 m = 0; m < mn; ++m) {
            const MeshRef r = mesh_ref(gtab, g, int(m));
            if (!r.entry) continue;
            add_span(vfree, r.vbyte, r.vbyte + r.nv * r.stride);
            add_span(ifree, r.istart, r.istart + r.icap);
        }
    }
    struct Place { const Mesh* m; MeshRef r; u32 vbyte, istart; bool moved; };
    std::vector<Place> places;
    std::vector<const Mesh*> wrench;
    for (const Pack* p : packs) {
        for (const Mesh& m : p->meshes) {
            if (m.group & kWrench) { wrench.push_back(&m); continue; }
            if (m.group != kCarrierGroup[p->carrier]) continue;
            const MeshRef r = mesh_ref(gtab, m.group, m.mesh);
            if (!r.entry) continue;
            if (r.nv != m.orig_nv || r.nidx != m.orig_nidx || r.flags != m.orig_flags) {
                LOG_NOTE("skins: g%u_m%u differs in this level (%u vertices, %u indices), left as it is", m.group, m.mesh, r.nv, r.nidx);
                continue;
            }
            if (m.verts.size() != size_t(m.nv) * r.stride || m.idx.size() != size_t(m.nidx) * 2 || m.nv > 0xffff) continue;
            Place pl{&m, r, r.vbyte, r.istart, false};
            const u32 vlen = m.nv * r.stride, vcap = r.nv * r.stride;
            if (vlen > vcap) { pl.moved = true; add_span(vfree, r.vbyte, r.vbyte + vcap); }
            else add_span(vfree, r.vbyte + vlen, r.vbyte + vcap);
            if (m.nidx > r.icap) { pl.moved = true; add_span(ifree, r.istart, r.istart + r.icap); }
            else add_span(ifree, r.istart + m.nidx, r.istart + r.icap);
            places.push_back(pl);
        }
    }
    merge(vfree);
    merge(ifree);
    int moved = 0, dropped = 0;
    for (Place& pl : places) {
        if (!pl.moved) continue;
        // both parts move together, so each part keeps its own room when it fits there
        const u32 vlen = pl.m->nv * pl.r.stride;
        bool ok = true;
        if (vlen > pl.r.nv * pl.r.stride) ok = alloc(vfree, vlen, 16, pl.vbyte);
        if (ok && pl.m->nidx > pl.r.icap) ok = alloc(ifree, pl.m->nidx, 8, pl.istart);
        if (!ok) { pl.m = nullptr; ++dropped; continue; }
        ++moved;
    }
    for (const Place& pl : places) {
        const MeshRef& r = pl.r;
        if (!pl.m) {
            // no room: draw nothing for that mesh (one degenerate triangle on its first vertex)
            const u8 zero[28] = {};
            write(vbase + r.vbyte, zero, r.stride);
            const u8 z6[6] = {};
            write(ibase + r.istart * 2, z6, 6);
            write32(r.entry + 8, (r.w8 & 0xffff0000u) | 1u);
            write32(r.entry + 16, 3);
            continue;
        }
        const Mesh& m = *pl.m;
        write(vbase + pl.vbyte, m.verts.data(), m.verts.size());
        write(ibase + pl.istart * 2, m.idx.data(), m.idx.size());
        write32(r.entry + 0, pl.istart);
        write32(r.entry + 4, pl.vbyte);
        write32(r.entry + 8, (r.w8 & 0xffff0000u) | m.nv);
        const bool skinned = (r.flags >> 16) & 0xff;
        if (skinned) write32(r.entry + 12, (r.flags & 0x00ffffffu) | u32(m.palette.size()) << 24);
        write32(r.entry + 16, m.nidx);
        if (r.lods) for (int k = 0; k < 3; ++k) if (ld32(mem::base, r.entry + 0x14 + 4 * k)) write32(r.entry + 0x14 + 4 * k, std::min<u32>(m.nidx, 0xffff));
        const u32 pal = ld32(mem::base, r.entry + 0x20);
        if (skinned && pal && !m.palette.empty()) {
            std::vector<u8> pb(m.palette.size() * 2);
            for (size_t k = 0; k < m.palette.size(); ++k) { pb[2 * k] = u8(m.palette[k] >> 8); pb[2 * k + 1] = u8(m.palette[k]); }
            write(pal, pb.data(), pb.size());
        }
    }
    // wrench meshes go over the wrench's own, in place
    int wrote_wrench = 0;
    if (const u32 wc = wrench.empty() ? 0 : find_wrench(cls)) {
        const u32 wtab = ld32(mem::base, wc + 0x28), wraw = ld32(mem::base, wc + 0x38);
        const u32 wv = (wraw & 0x80000000u) ? (wraw & 0x7fffffffu) + 0x50000000u : 0xC0000000u + wraw;
        const u32 wi = 0xC0000000u + ld32(mem::base, wc + 0x34);
        std::set<u32> done;
        for (const Mesh* m : wrench) {
            const MeshRef r = mesh_ref(wtab, m->group & ~kWrench, m->mesh);
            if (!r.entry || !done.insert(r.entry).second) continue;
            if (r.nv != m->orig_nv || r.nidx != m->orig_nidx || r.flags != m->orig_flags || m->nv > r.nv || m->nidx > r.icap ||
                m->verts.size() != size_t(m->nv) * r.stride || m->idx.size() != size_t(m->nidx) * 2) {
                LOG_NOTE("skins: wrench g%u_m%u differs in this level or doesn't fit, left as it is", m->group & ~kWrench, m->mesh);
                continue;
            }
            write(wv + r.vbyte, m->verts.data(), m->verts.size());
            write(wi + r.istart * 2, m->idx.data(), m->idx.size());
            write32(r.entry + 8, (r.w8 & 0xffff0000u) | m->nv);
            write32(r.entry + 16, m->nidx);
            if (r.lods) for (int k = 0; k < 3; ++k) if (ld32(mem::base, r.entry + 0x14 + 4 * k)) write32(r.entry + 0x14 + 4 * k, std::min<u32>(m->nidx, 0xffff));
            const bool skinned = (r.flags >> 16) & 0xff;
            const u32 pal = ld32(mem::base, r.entry + 0x20);
            if (skinned) write32(r.entry + 12, (r.flags & 0x00ffffffu) | u32(m->palette.size()) << 24);
            if (skinned && pal && !m->palette.empty()) {
                std::vector<u8> pb(m->palette.size() * 2);
                for (size_t k = 0; k < m->palette.size(); ++k) { pb[2 * k] = u8(m->palette[k] >> 8); pb[2 * k + 1] = u8(m->palette[k]); }
                write(pal, pb.data(), pb.size());
            }
            ++wrote_wrench;
        }
        if (wrote_wrench) LOG_NOTE("skins: %d wrench meshes written at class 0x%08x", wrote_wrench, wc);
    } else if (!wrench.empty()) LOG_NOTE("skins: no wrench class found near 0x%08x", cls);
    // the worn packs' textures
    std::map<u64, std::string> tex;
    for (const Pack* p : packs) tex.insert(p->textures.begin(), p->textures.end());
    if (tex != g_tex) { g_tex.swap(tex); ++g_tex_gen; }
    seal();
    std::string w;
    for (const Pack* p : packs) w += " " + p->name;
    LOG_NOTE("skins: layout at class 0x%08x: player skins %d/%d, packs:%s, %zu meshes (%d moved into free room, %d without room), %zu writes",
             cls, skin_of[0], skin_of[1], w.empty() ? " none" : w.c_str(), places.size(), moved, dropped, g_saved.size());
}

void save_choice(int pl) {
    const Pack* p = pack_of(g_worn[pl]);
    std::string v = p ? p->name : (pl == 1 && g_worn[1] >= 0 ? std::to_string(g_worn[1]) : std::string());
    g_cfg[pl] = v;
    gfx::save_setting(pl == 0 ? "skin" : "skin2", v);
}

// player 1 picks entry e (also from the menu's select_skin)
void wear_now(int pl, int e) {
    if (e < -1 || e >= kGameSkins + int(g_packs.size())) return;
    if (pl == 0) {
        const u32 fe = front_end();
        const int gs = game_skin_of(e < 0 ? 0 : e);
        if (fe) {
            if (pack_of(e)) st32(mem::base, fe + 0x45C + 4 * u32(gs), 1);   // the carrier must count as owned to be drawn
            st32(mem::base, fe + 0x480, u32(gs));
        }
        g_worn[0] = pack_of(e) ? e : -1;
    } else {
        g_worn[1] = e;
    }
    save_choice(pl);
    g_dirty = true;
    LOG_NOTE("skins: player %d wears %s", pl + 1, e < 0 ? "player 1's skin" : entry_name(e).c_str());
}

}  // namespace

void configure(const std::string& name, const std::string& data_dir) {
    std::lock_guard lk(g_lock);
    g_cfg[0] = name;
    g_data = data_dir;
}
void configure2(const std::string& name) { std::lock_guard lk(g_lock); g_cfg[1] = name; }

void level_archive(const std::string& host_path) {
    const std::string tail = "/level_cached.psarc";
    if (host_path.size() < tail.size() || host_path.compare(host_path.size() - tail.size(), tail.size(), tail) != 0 ||
        host_path.find("/packed/levels/") == std::string::npos)
        return;
    std::lock_guard lk(g_lock);
    scan();
    // a new level replaces the class: what was written there is gone with it
    if (!g_saved.empty()) LOG_NOTE("skins: level archive opened again with %zu writes in place (%s)", g_saved.size(), host_path.c_str());
    g_saved.clear();
    g_written.clear();
    g_cls = 0;
    g_dirty = true;
}

void on_hero_update(u32 hero) {
    std::unique_lock lk(g_lock, std::try_to_lock);
    if (!lk.owns_lock()) return;
    scan();
    if (g_packs.empty() && g_worn[1] < 0) return;
    for (int pl = 0; pl < 2; ++pl) {
        const int r = g_request[pl].exchange(-2);
        if (r != -2) wear_now(pl, r);
    }
    const u32 moby = hero && mem::is_mapped(hero + 0x14, 4) ? ld32(mem::base, hero + 0x14) : 0;
    const u32 cls = moby && mem::is_mapped(moby + 0x54, 4) ? ld32(mem::base, moby + 0x54) : 0;
    if (!is_ratchet(cls)) return;
    const u32 fe = front_end();
    if (!fe) return;
    s32 sel = s32(ld32(mem::base, fe + 0x480));
    if (cls != g_cls) {
        if (!g_saved.empty()) LOG_NOTE("skins: hero class 0x%08x -> 0x%08x with %zu writes in place", g_cls, cls, g_saved.size());
        g_saved.clear();   // a class we haven't written to (new level)
        g_written.clear();
        g_cls = cls;
        g_dirty = true;
        // a pack from settings.ini goes on at the first level, unless the save wears another game skin
        if (const Pack* p = pack_of(g_worn[0]); p && sel != p->carrier) {
            if (sel == 0) { st32(mem::base, fe + 0x45C + 4 * u32(p->carrier), 1); st32(mem::base, fe + 0x480, u32(p->carrier)); sel = p->carrier; }
            else { LOG_NOTE("skins: the save wears game skin %d, so '%s' stays off", sel, p->name.c_str()); g_worn[0] = -1; save_choice(0); }
        }
    }
    if (sel != g_last_sel) {
        // picked in a way the menu hooks didn't see (a save loaded, a cheat): a pack stays on only with its carrier
        if (const Pack* p = pack_of(g_worn[0]); p && sel != p->carrier) { g_worn[0] = -1; save_choice(0); }
        g_last_sel = sel;
        g_dirty = true;
    }
    // the game can load a picked skin's meshes after the layout went in (the skins menu does), which puts its
    // own vertices back under the pack's mesh entries: write the layout again when that happens
    if (!g_dirty && !g_written.empty() && --g_check <= 0) {
        g_check = 2;
        if (overwritten()) {
            static int logged = 0;
            if (logged++ < 20) LOG_NOTE("skins: the game wrote over the worn skins' meshes, writing them again");
            adopt();
            g_dirty = true;
        }
    }
    if (!g_dirty) return;
    g_dirty = false;
    layout(cls, sel);
}

bool on_menu_hook(PPUContext& c, u32 site) {
    std::lock_guard lk(g_lock);
    scan();
    const s32 id = s32(c.r[3]);
    const int n = kGameSkins + int(g_packs.size());
    switch (site) {
    case 0x2A58C:   // the Lua constant SKIN_TYPE_COUNT is being registered (value in f1)
        if (!g_packs.empty()) { c.f[1] = double(n); LOG_NOTE("skins: SKIN_TYPE_COUNT = %d", n); }
        return false;
    case 0x1E21A0:  // the selected game skin, r3 = a hero (front end data at +0x1A68). The skin's meshes are
                    // picked per hero each frame by sub_1E4CF0, which asks here, so player 2 can have their own.
        if (g_worn[1] >= 0 && (u32(c.r[3]) == coop::hero2() || coop::in_p2_update()) && coop::hero2()) {
            c.r[3] = u64(game_skin_of(g_worn[1]));
            return true;
        }
        return false;
    default:
        break;
    }
    // a pause menu player 2 opened picks player 2's skin (skin2): the game's own selection stays player 1's
    const bool p2_menu = coop::pause_player() == 2;
    if (site == 0x26FC0 && p2_menu && id >= 0 && id < n) {
        wear_now(1, id);
        c.r[3] = 1;
        return true;
    }
    if (id < kGameSkins) {
        if (site == 0x26FC0 && g_worn[0] >= 0) { g_worn[0] = -1; save_choice(0); g_dirty = true; }   // a game skin again
        return false;
    }
    const Pack* p = pack_of(id);
    switch (site) {
    case 0x24B08:   // is_skin_owned
    case 0x24B38:   // is_skin_available
        c.r[3] = id < n ? 1 : 0;
        return true;
    case 0x25B90:   // get_skin_cost (gold bolts)
        c.r[3] = 0;
        return true;
    case 0x27AA0:   // purchase_skin
        c.r[3] = p ? 1 : 0;
        return true;
    case 0x26FC0:   // select_skin
        if (p) wear_now(0, id);
        c.r[3] = p ? 1 : 0;
        return true;
    case 0x25B68:   // get_skin_name
        c.r[3] = guest_string(p ? p->title : "?");
        return true;
    case 0x25B40:   // get_skin_desc
        c.r[3] = guest_string(p ? (p->desc.empty() ? p->title : p->desc) : "");
        return true;
    case 0x25B18:   // get_skin_movie: the menu plays built/movies/<lower case>/...
        c.r[3] = guest_string(p ? "SKIN_" + upper(p->name) : "SKIN_NONE");
        return true;
    default:
        return false;
    }
}

std::string movie_override(const std::string& guest, const std::string& host) {
    if (guest.size() < 4 || guest.compare(guest.size() - 4, 4, ".bik") != 0 || guest.find("/built/movies/skin_") == std::string::npos) return host;
    std::lock_guard lk(g_lock);
    scan();
    for (const Pack& p : g_packs) {
        const std::string key = "skin_" + lower(p.name);
        if (guest.find("/built/movies/" + key + "/") == std::string::npos) continue;
        const std::string movie = p.dir + "/movie.bik";
        if (std::filesystem::exists(movie)) { LOG_NOTE("skins: %s -> %s", guest.c_str(), movie.c_str()); return movie; }
        // no movie of its own: its carrier's
        std::string h = host;
        for (size_t at; (at = h.find(key)) != std::string::npos;) h.replace(at, key.size(), kMovies[p.carrier]);
        return h;
    }
    return host;
}

bool texture_for(u64 hash, std::string& png) {
    std::lock_guard lk(g_lock);
    auto it = g_tex.find(hash);
    if (it == g_tex.end()) return false;
    png = it->second;
    return true;
}
u32 texture_generation() { return g_tex_gen.load(std::memory_order_relaxed); }
size_t texture_count() {
    std::lock_guard lk(g_lock);
    scan();
    size_t n = 0;
    for (const Pack& p : g_packs) n += p.textures.size();
    return n;
}

int entry_count() { std::lock_guard lk(g_lock); scan(); return kGameSkins + int(g_packs.size()); }
std::string entry_name(int e) {
    std::lock_guard lk(g_lock);
    if (e >= 0 && e < kGameSkins) return kGameNames[e];
    const Pack* p = pack_of(e);
    return p ? p->title : std::string();
}
int worn(int pl) {
    std::lock_guard lk(g_lock);
    if (pl == 1) return g_worn[1];
    if (g_worn[0] >= 0) return g_worn[0];
    const u32 fe = front_end();
    return fe ? int(ld32(mem::base, fe + 0x480)) : 0;
}
void wear(int pl, int e) { g_request[pl & 1] = e; }

EntryInfo entry_info(int e) {
    std::lock_guard lk(g_lock);
    scan();
    EntryInfo in;
    if (e >= 0 && e < kGameSkins) {
        in.title = kGameNames[e];
        const u32 fe = front_end();
        in.owned = e == 0 || !fe || ld32(mem::base, fe + 0x45C + 4 * u32(e)) != 0;
        return in;
    }
    const Pack* p = pack_of(e);
    if (!p) return in;
    in.title = p->title; in.desc = p->desc; in.folder = p->name;
    in.pack = true; in.carrier = p->carrier; in.abilities = p->abilities;
    in.vertices = p->vertices; in.triangles = p->triangles;
    std::error_code ec;
    if (std::filesystem::exists(p->dir + "/preview.png", ec)) in.preview = p->dir + "/preview.png";
    return in;
}

u32 abilities(int pl) {
    std::unique_lock lk(g_lock, std::try_to_lock);
    if (!lk.owns_lock()) return 0;
    // player 2 wearing nothing of their own looks like player 1
    const int e = pl == 1 && g_worn[1] >= 0 ? g_worn[1] : g_worn[0];
    const Pack* p = pack_of(e);
    return p ? p->abilities : 0;
}

void command(int e) {
    std::lock_guard lk(g_lock);
    scan();
    if (e < 0) {
        std::string list;
        for (int i = 0; i < kGameSkins + int(g_packs.size()); ++i) list += " [" + std::to_string(i) + " " + entry_name(i) + "]";
        LOG_NOTE("skins: skins:%s", list.c_str());
        return;
    }
    wear_now(0, e);
}
// testing ("!skin clobber"): put the game's own bytes back under the layout, as the skins menu's loading does
void clobber() {
    std::lock_guard lk(g_lock);
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it)
        if (mem::is_mapped(it->ea, u32(it->bytes.size()))) std::memcpy(mem::base + it->ea, it->bytes.data(), it->bytes.size());
    LOG_NOTE("skins: clobbered %zu writes", g_saved.size());
}
void command2(int e) { std::lock_guard lk(g_lock); scan(); wear_now(1, e); }

}  // namespace skins
