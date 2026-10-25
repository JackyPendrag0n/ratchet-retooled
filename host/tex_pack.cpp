// Texture packs. See tex_pack.h.
#include "tex_pack.h"
#include "skins.h"
#include <zlib.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace texpack {
namespace {

namespace fs = std::filesystem;

std::string g_dump_dir;
std::unordered_map<u64, std::string> g_files;  // hash -> replacement PNG
// decoded replacements with their mip chains, kept: the game uploads the same texture again
// whenever it streams it back in, and decoding a large PNG each time stalls the renderer
struct Decoded { std::vector<u8> rgba; u32 w, h, mips; };
std::unordered_map<std::string, Decoded> g_decoded;   // by PNG path: a worn skin pack can change the PNG for a hash
std::mutex g_lock;
std::unordered_set<u64> g_dumped;

struct DumpJob { u64 hash; DecodedTex dec; };
std::deque<DumpJob> g_jobs;
std::condition_variable g_cv;
bool g_worker = false;

// ---- texel decoding (host-order data as decode_texture_from leaves it) -------------------------
void rgb565(u16 c, u8* o) {
    o[0] = u8(((c >> 11) & 31) * 255 / 31);
    o[1] = u8(((c >> 5) & 63) * 255 / 63);
    o[2] = u8((c & 31) * 255 / 31);
    o[3] = 255;
}

// One BC1 colour block into a 4x4 RGBA tile (stride 16 bytes per row). bc1 = true allows the
// 3-colour + transparent mode.
void bc_color(const u8* b, u8 tile[64], bool bc1) {
    u16 c0, c1;
    std::memcpy(&c0, b, 2);
    std::memcpy(&c1, b + 2, 2);
    u8 pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !bc1) {
        for (int k = 0; k < 4; ++k) {
            pal[2][k] = u8((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = u8((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 4; ++k) { pal[2][k] = u8((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[3][3] = 0;
    }
    u32 idx;
    std::memcpy(&idx, b + 4, 4);
    for (int i = 0; i < 16; ++i) std::memcpy(tile + i * 4, pal[(idx >> (2 * i)) & 3], 4);
}

bool decode_top(const DecodedTex& dec, std::vector<u8>& out, u32& w, u32& h) {
    if (dec.levels.empty() || dec.view_type == VK_IMAGE_VIEW_TYPE_3D || dec.view_type == VK_IMAGE_VIEW_TYPE_CUBE) return false;
    const TexLevel& lv = dec.levels[0];
    w = lv.width; h = lv.height;
    if (!w || !h) return false;
    out.assign(size_t(w) * h * 4, 0);
    const u8* s = dec.data.data() + lv.offset;
    auto put = [&](u32 x, u32 y, const u8* px) { if (x < w && y < h) std::memcpy(&out[(size_t(y) * w + x) * 4], px, 4); };
    switch (dec.format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: {
        const u32 bsize = dec.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;
        const u32 bw = std::max<u32>(1, (lv.row_length + 3) / 4), bh = (h + 3) / 4;
        for (u32 by = 0; by < bh; ++by)
            for (u32 bx = 0; bx < bw; ++bx) {
                const u8* b = s + (size_t(by) * bw + bx) * bsize;
                if (b + bsize > dec.data.data() + dec.data.size()) return false;
                u8 tile[64];
                if (bsize == 8) bc_color(b, tile, true);
                else {
                    bc_color(b + 8, tile, false);
                    if (dec.format == VK_FORMAT_BC2_UNORM_BLOCK) {
                        for (int i = 0; i < 16; ++i) tile[i * 4 + 3] = u8(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                    } else {
                        u8 a[8] = {b[0], b[1]};
                        if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = u8(((7 - i) * a[0] + i * a[1]) / 7);
                        else { for (int i = 1; i < 5; ++i) a[i + 1] = u8(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
                        u64 bits = 0;
                        for (int i = 0; i < 6; ++i) bits |= u64(b[2 + i]) << (8 * i);
                        for (int i = 0; i < 16; ++i) tile[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                    }
                }
                for (int i = 0; i < 16; ++i) put(bx * 4 + (i & 3), by * 4 + (i >> 2), tile + i * 4);
            }
        break;
    }
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                const u8* p = s + (size_t(y) * lv.row_length + x) * 4;
                u8 px[4] = {p[0], p[1], p[2], p[3]};
                if (dec.format == VK_FORMAT_B8G8R8A8_UNORM) std::swap(px[0], px[2]);
                put(x, y, px);
            }
        break;
    case VK_FORMAT_R8_UNORM:
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) { const u8 v = s[size_t(y) * lv.row_length + x]; u8 px[4] = {v, 0, 0, 255}; put(x, y, px); }
        break;
    case VK_FORMAT_R8G8_UNORM:
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) { const u8* p = s + (size_t(y) * lv.row_length + x) * 2; u8 px[4] = {p[0], p[1], 0, 255}; put(x, y, px); }
        break;
    default: return false;
    }
    // the image view's mapping (channel order and the game's remap) gives what shaders read; channels
    // forced to 0 or 1 keep their stored data, so headers that share the data share one PNG (a
    // replacement gets the constants back, see vkn_render's do_texture)
    VkComponentSwizzle m[4] = {dec.mapping.r, dec.mapping.g, dec.mapping.b, dec.mapping.a};
    for (auto& c : m)
        if (c == VK_COMPONENT_SWIZZLE_ZERO || c == VK_COMPONENT_SWIZZLE_ONE) c = VK_COMPONENT_SWIZZLE_IDENTITY;
    bool identity = true;
    for (int k = 0; k < 4; ++k) identity &= m[k] == VK_COMPONENT_SWIZZLE_IDENTITY || m[k] == VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + k);
    if (!identity)
        for (size_t i = 0; i < out.size(); i += 4) {
            u8 src[4];
            std::memcpy(src, &out[i], 4);
            for (int k = 0; k < 4; ++k) {
                switch (m[k]) {
                case VK_COMPONENT_SWIZZLE_IDENTITY: out[i + k] = src[k]; break;
                case VK_COMPONENT_SWIZZLE_ZERO: out[i + k] = 0; break;
                case VK_COMPONENT_SWIZZLE_ONE: out[i + k] = 255; break;
                default: out[i + k] = src[m[k] - VK_COMPONENT_SWIZZLE_R]; break;
                }
            }
        }
    return true;
}

void worker() {
    for (;;) {
        DumpJob j;
        {
            std::unique_lock lk(g_lock);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        std::vector<u8> px;
        u32 w, h;
        if (!decode_top(j.dec, px, w, h)) continue;
        char name[64];
        std::snprintf(name, sizeof name, "/%016llx_%ux%u.png", (unsigned long long)j.hash, w, h);
        write_png_rgba(g_dump_dir + name, px.data(), w, h);
    }
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

}  // namespace

void init(const std::string& data_dir) {
    const fs::path root = fs::path(data_dir) / "textures";
    g_dump_dir = (root / "dump").string();
    std::error_code ec;
    fs::create_directories(root, ec);
    // the file name starts with the 16 hex digit hash; anything after it is free text.
    // (the worn skin packs' textures are looked up in skins.cpp first, see replacement())
    std::vector<fs::path> roots{root};
    for (const fs::path& top : roots)
        for (auto it = (ec.clear(), fs::recursive_directory_iterator(top, ec)); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            if (p.parent_path() == root / "dump") continue;
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            const std::string stem = p.stem().string();
            if (ext != ".png" || stem.size() < 16) continue;
            char* end = nullptr;
            const std::string hex = stem.substr(0, 16);
            const unsigned long long hval = std::strtoull(hex.c_str(), &end, 16);
            if (end != hex.c_str() + 16) continue;
            g_files[u64(hval)] = p.string();
        }
    LOG_NOTE("texture pack: %zu replacement(s) in %s", g_files.size(), root.string().c_str());
}

size_t replacement_count() { return g_files.size() + skins::texture_count(); }

void dump(u64 hash, const DecodedTex& dec) {
    std::lock_guard lk(g_lock);
    if (!g_dumped.insert(hash).second) return;
    if (!g_worker) {
        std::error_code ec;
        fs::create_directories(g_dump_dir, ec);
        g_worker = true;
        std::thread(worker).detach();
    }
    g_jobs.push_back({hash, dec});
    g_cv.notify_one();
}

bool replacement(u64 hash, std::vector<u8>& rgba, u32& w, u32& h, u32& mips) {
    std::string path;
    if (!skins::texture_for(hash, path)) {
        auto it = g_files.find(hash);
        if (it == g_files.end()) return false;
        path = it->second;
    }
    if (auto d = g_decoded.find(path); d != g_decoded.end()) {
        rgba = d->second.rgba; w = d->second.w; h = d->second.h; mips = d->second.mips;
        return true;
    }
    std::vector<u8> top;
    if (!read_png(path, top, w, h)) {
        LOG_NOTE("texture pack: can't read %s (8-bit PNG, not interlaced)", path.c_str());
        if (auto it = g_files.find(hash); it != g_files.end() && it->second == path) g_files.erase(it);
        return false;
    }
    // box-filtered mip chain
    rgba = top;
    mips = 1;
    std::vector<u8> prev = std::move(top);
    u32 pw = w, ph = h;
    while (pw > 1 || ph > 1) {
        const u32 nw = std::max<u32>(1, pw / 2), nh = std::max<u32>(1, ph / 2);
        std::vector<u8> next(size_t(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (int k = 0; k < 4; ++k) {
                    const u32 x0 = std::min(pw - 1, x * 2), x1 = std::min(pw - 1, x * 2 + 1);
                    const u32 y0 = std::min(ph - 1, y * 2), y1 = std::min(ph - 1, y * 2 + 1);
                    const u32 sum = prev[(size_t(y0) * pw + x0) * 4 + k] + prev[(size_t(y0) * pw + x1) * 4 + k] +
                                    prev[(size_t(y1) * pw + x0) * 4 + k] + prev[(size_t(y1) * pw + x1) * 4 + k];
                    next[(size_t(y) * nw + x) * 4 + k] = u8((sum + 2) / 4);
                }
        rgba.insert(rgba.end(), next.begin(), next.end());
        prev = std::move(next);
        pw = nw; ph = nh;
        ++mips;
    }
    g_decoded[path] = {rgba, w, h, mips};
    return true;
}

bool read_png(const std::string& path, std::vector<u8>& rgba, u32& w, u32& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<u8> d;
    u8 buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (d.size() < 8 || std::memcmp(d.data(), sig, 8) != 0) return false;
    u32 depth = 0, ctype = 0, interlace = 0;
    std::vector<u8> idat, plte, trns;
    w = h = 0;
    for (size_t p = 8; p + 12 <= d.size();) {
        const u32 len = be32(&d[p]);
        if (p + 12 + len > d.size()) return false;
        const u8* body = &d[p + 8];
        const std::string type(reinterpret_cast<const char*>(&d[p + 4]), 4);
        if (type == "IHDR" && len >= 13) { w = be32(body); h = be32(body + 4); depth = body[8]; ctype = body[9]; interlace = body[12]; }
        else if (type == "PLTE") plte.assign(body, body + len);
        else if (type == "tRNS") trns.assign(body, body + len);
        else if (type == "IDAT") idat.insert(idat.end(), body, body + len);
        else if (type == "IEND") break;
        p += 12 + len;
    }
    static const u32 kChannels[7] = {1, 0, 3, 1, 2, 0, 4};
    if (!w || !h || w > 16384 || h > 16384 || depth != 8 || interlace || ctype > 6 || !kChannels[ctype]) return false;
    const u32 bpp = kChannels[ctype];
    const size_t stride = size_t(w) * bpp;
    std::vector<u8> raw((stride + 1) * h);
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = idat.data(); zs.avail_in = uInt(idat.size());
    zs.next_out = raw.data(); zs.avail_out = uInt(raw.size());
    const int r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (r != Z_STREAM_END && zs.avail_out != 0) return false;
    // undo the row filters in place
    std::vector<u8> img(stride * h);
    for (u32 y = 0; y < h; ++y) {
        const u8 ft = raw[y * (stride + 1)];
        const u8* src = &raw[y * (stride + 1) + 1];
        u8* row = &img[y * stride];
        const u8* up = y ? &img[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= bpp ? row[x - bpp] : 0, b = up ? up[x] : 0, c = (up && x >= bpp) ? up[x - bpp] : 0;
            int v = src[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                break;
            }
            default: break;
            }
            row[x] = u8(v);
        }
    }
    rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const u8* s = &img[i * bpp];
        u8* o = &rgba[i * 4];
        switch (ctype) {
        case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
        case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
        case 3:
            if (size_t(s[0]) * 3 + 2 >= plte.size()) return false;
            o[0] = plte[s[0] * 3]; o[1] = plte[s[0] * 3 + 1]; o[2] = plte[s[0] * 3 + 2];
            o[3] = s[0] < trns.size() ? trns[s[0]] : 255;
            break;
        case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
        case 6: std::memcpy(o, s, 4); break;
        }
    }
    return true;
}

void write_png_rgba(const std::string& path, const u8* rgba, u32 w, u32 h) {
    std::vector<u8> raw;
    raw.reserve(size_t(w * 4 + 1) * h);
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<u8> z(zlen);
    compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6);
    z.resize(zlen);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto put32 = [](u8* p, u32 v) { p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); };
    auto chunk = [&](const char* type, const u8* data, u32 len) {
        u8 hdr[8]; put32(hdr, len); std::memcpy(hdr + 4, type, 4);
        std::fwrite(hdr, 8, 1, f);
        if (len) std::fwrite(data, len, 1, f);
        uLong crc = crc32(0, hdr + 4, 4);
        if (len) crc = crc32(crc, data, len);
        u8 c[4]; put32(c, u32(crc)); std::fwrite(c, 4, 1, f);
    };
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::fwrite(sig, 8, 1, f);
    u8 ihdr[13]; put32(ihdr, w); put32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), u32(z.size()));
    chunk("IEND", nullptr, 0);
    std::fclose(f);
}

}  // namespace texpack
