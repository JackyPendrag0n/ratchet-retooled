// RSX texture decoding. See rsx_tex.h.
#include "rsx_tex.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace rsx {
namespace {

constexpr VkComponentSwizzle A = VK_COMPONENT_SWIZZLE_A, R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G,
                             B = VK_COMPONENT_SWIZZLE_B, ONE = VK_COMPONENT_SWIZZLE_ONE, ZERO = VK_COMPONENT_SWIZZLE_ZERO;

struct BlockInfo {
    u32 bytes;   // bytes per block in guest memory
    u32 bw, bh;  // texels per block (width, height)
};

bool block_info(u32 fmt, BlockInfo& bi) {
    switch (fmt) {
    case TEX_B8: bi = {1, 1, 1}; return true;
    case TEX_A1R5G5B5: case TEX_A4R4G4B4: case TEX_R5G6B5: case TEX_G8B8: case TEX_R6G5B5:
    case TEX_DEPTH16: case TEX_DEPTH16_FLOAT: case TEX_X16: case TEX_R5G5B5A1:
    case TEX_HILO8: case TEX_HILO_S8: case TEX_D1R5G5B5:
        bi = {2, 1, 1}; return true;
    case TEX_A8R8G8B8: case TEX_DEPTH24_D8: case TEX_DEPTH24_D8_FLOAT: case TEX_Y16_X16:
    case TEX_X32_FLOAT: case TEX_D8R8G8B8: case TEX_Y16_X16_FLOAT:
        bi = {4, 1, 1}; return true;
    case TEX_B8R8_G8R8: case TEX_R8B8_R8G8: bi = {4, 2, 1}; return true;
    case TEX_W16_Z16_Y16_X16_FLOAT: bi = {8, 1, 1}; return true;
    case TEX_W32_Z32_Y32_X32_FLOAT: bi = {16, 1, 1}; return true;
    case TEX_DXT1: bi = {8, 4, 4}; return true;
    case TEX_DXT23: case TEX_DXT45: bi = {16, 4, 4}; return true;
    default: return false;
    }
}

inline u32 div_up(u32 a, u32 b) { return (a + b - 1) / b; }
inline u32 next_pow2(u32 v) { return v <= 1 ? 1 : std::bit_ceil(v); }
inline u32 ceil_log2(u32 v) { return v <= 1 ? 0 : 32 - std::countl_zero(v - 1); }
inline u32 floor_log2(u32 v) { return v ? 31 - std::countl_zero(v) : 0; }
inline u16 rd16be(const u8* p) { u16 v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline u32 rd32be(const u8* p) { u32 v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline void wr16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
inline void wr32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

// ---- layout -----------------------------------------------------------------------------
struct SrcLevel {
    u32 offset;           // relative to desc.ea
    u32 w, h, d;          // texels
    u32 wb, hb;           // blocks
    u32 pitch;            // source row pitch in bytes (one block row)
    u32 full_h;           // block rows per slice in memory (incl. border/padding)
    u32 border;           // border width in blocks
    u32 size;             // bytes
};

struct Layout {
    u32 fmt = 0;
    BlockInfo bi{};
    bool linear = false;
    bool cube = false;
    u32 dim = 2;
    u32 layers = 1;
    u32 mips = 1;
    std::vector<SrcLevel> levels;  // layer-major
    u32 total = 0;
};

bool compute_layout(const TexDesc& t, Layout& L) {
    L.fmt = tex_base_format(t.format);
    if (!block_info(L.fmt, L.bi)) return false;
    const BlockInfo& bi = L.bi;
    const bool dxt = is_compressed_dxt(L.fmt);
    L.linear = tex_is_linear(t.format);
    L.dim = tex_dimension(t.format);
    if (L.dim < 1 || L.dim > 3) L.dim = 2;
    L.cube = L.dim == 2 && tex_is_cubemap(t.format);
    L.layers = L.cube ? 6 : 1;

    u32 w = std::max(t.width, 1u);
    u32 h = L.dim == 1 ? 1 : std::max(t.height, 1u);
    u32 d = L.dim == 3 ? std::max(t.depth, 1u) : 1;
    const bool border = tex_has_border(t.format) && !dxt;
    const u32 bsz = border ? (L.linear ? 1 : 4) : 0;

    u32 pitch = t.pitch;
    if (L.linear) {
        const u32 packed = (div_up(w, bi.bw) + 2 * bsz) * bi.bytes;
        if (pitch == 0) {
            if (w > 1 || h > 1) LOG_NOTE("rsx_tex: linear texture with pitch 0 (%ux%u fmt 0x%x)", w, h, L.fmt);
            pitch = packed;
        } else if (pitch < packed) {
            // RSX scans `pitch` bytes per row; the visible width is what fits (RPCS3 behaviour).
            w = std::max<u32>(pitch / bi.bytes * bi.bw, 1);
        }
    }

    u32 mips = t.mipmaps ? t.mipmaps : (t.format >> 16);
    mips = std::max(mips, 1u);
    u32 max_mips = dxt ? floor_log2(std::max(std::min(w / 4, h / 4), 1u)) + 1 : floor_log2(std::max(w, h)) + 1;
    L.mips = std::min(mips, max_mips);

    u32 off = 0;
    L.levels.clear();
    for (u32 layer = 0; layer < L.layers; ++layer) {
        if (layer && !L.linear) off = (off + 127) & ~127u;  // swizzled/packed faces are 128-byte aligned
        u32 mw = w, mh = h, md = d;
        for (u32 m = 0; m < L.mips; ++m) {
            SrcLevel s{};
            s.offset = off;
            s.w = mw; s.h = mh; s.d = md;
            s.wb = div_up(mw, bi.bw);
            s.hb = div_up(mh, bi.bh);
            s.border = bsz;
            if (L.linear) {
                s.pitch = pitch;
                s.full_h = s.hb + 2 * bsz;
            } else if (!border) {
                s.pitch = s.wb * bi.bytes;
                s.full_h = s.hb;
            } else {
                s.pitch = next_pow2(s.wb + 2 * bsz) * bi.bytes;
                s.full_h = next_pow2(s.hb + 2 * bsz);
            }
            s.size = s.pitch * s.full_h * md;
            off += s.size;
            L.levels.push_back(s);
            mw = std::max(mw / 2, 1u);
            mh = std::max(mh / 2, 1u);
            md = std::max(md / 2, 1u);
        }
    }
    L.total = off;
    return true;
}

// ---- swizzle ----------------------------------------------------------------------------
// Per-axis Z-order deposit tables: bits are assigned round-robin x, y, z while each axis still has bits.
struct ZTables {
    std::vector<u32> x, y, z;
    ZTables(u32 w, u32 h, u32 d) {
        u32 lw = ceil_log2(w), lh = ceil_log2(h), ld = ceil_log2(d);
        u32 px[32], py[32], pz[32], nx = 0, ny = 0, nz = 0, shift = 0;
        while (lw || lh || ld) {
            if (lw) { px[nx++] = shift++; --lw; }
            if (lh) { py[ny++] = shift++; --lh; }
            if (ld) { pz[nz++] = shift++; --ld; }
        }
        auto build = [](std::vector<u32>& t, u32 n, const u32* pos, u32 cnt) {
            t.resize(n);
            for (u32 i = 0; i < n; ++i) {
                u32 v = 0;
                for (u32 b = 0; b < cnt; ++b) v |= ((i >> b) & 1u) << pos[b];
                t[i] = v;
            }
        };
        build(x, w, px, nx);
        build(y, h, py, ny);
        build(z, d, pz, nz);
    }
};

template <u32 U>
void deswizzle_t(const u8* src, u8* dst, u32 w, u32 h, u32 d, size_t src_size) {
    ZTables zt(w, h, d);
    const size_t max_idx = src_size / U;
    for (u32 zz = 0; zz < d; ++zz)
        for (u32 yy = 0; yy < h; ++yy) {
            const u32 base = zt.z[zz] | zt.y[yy];
            for (u32 xx = 0; xx < w; ++xx, dst += U) {
                const u32 idx = base | zt.x[xx];
                if (idx < max_idx) std::memcpy(dst, src + size_t(idx) * U, U);
                else std::memset(dst, 0, U);
            }
        }
}

template <u32 U>
void swizzle_t(const u8* src, u8* dst, u32 w, u32 h, u32 d) {
    ZTables zt(w, h, d);
    for (u32 zz = 0; zz < d; ++zz)
        for (u32 yy = 0; yy < h; ++yy) {
            const u32 base = zt.z[zz] | zt.y[yy];
            for (u32 xx = 0; xx < w; ++xx, src += U) std::memcpy(dst + size_t(base | zt.x[xx]) * U, src, U);
        }
}

// ---- texel conversion: guest (BE) blocks -> host data -------------------------------------
u32 host_block_bytes(u32 fmt, const BlockInfo& bi) {
    switch (fmt) {
    case TEX_DEPTH16_FLOAT: return 4;          // -> R32_SFLOAT
    case TEX_B8R8_G8R8: case TEX_R8B8_R8G8: return 8;  // 2 BGRA8 texels
    default: return bi.bytes;
    }
}

inline float decode_e4m12(u32 v) {
    const u32 bits = (v << 11) & 0x07FFF800u;
    return std::bit_cast<float>(bits) * std::bit_cast<float>(0x77000000u);
}

void convert_blocks(u32 fmt, const u8* s, u8* o, size_t count) {
    switch (fmt) {
    case TEX_B8:
    case TEX_DXT1: case TEX_DXT23: case TEX_DXT45:
        // DXT blocks are stored in the same (little-endian) layout as BCn; RPCS3 uploads them as-is.
        std::memcpy(o, s, count * (fmt == TEX_B8 ? 1 : fmt == TEX_DXT1 ? 8 : 16));
        return;
    case TEX_A1R5G5B5: case TEX_A4R4G4B4: case TEX_R5G6B5: case TEX_G8B8: case TEX_X16: case TEX_R5G5B5A1:
    case TEX_HILO8: case TEX_HILO_S8: case TEX_D1R5G5B5: case TEX_DEPTH16:
        for (size_t i = 0; i < count; ++i) wr16(o + 2 * i, rd16be(s + 2 * i));
        return;
    case TEX_Y16_X16: case TEX_Y16_X16_FLOAT:
        for (size_t i = 0; i < count * 2; ++i) wr16(o + 2 * i, rd16be(s + 2 * i));
        return;
    case TEX_W16_Z16_Y16_X16_FLOAT:
        for (size_t i = 0; i < count * 4; ++i) wr16(o + 2 * i, rd16be(s + 2 * i));
        return;
    case TEX_A8R8G8B8: case TEX_D8R8G8B8: case TEX_X32_FLOAT:
        for (size_t i = 0; i < count; ++i) wr32(o + 4 * i, rd32be(s + 4 * i));
        return;
    case TEX_W32_Z32_Y32_X32_FLOAT:
        for (size_t i = 0; i < count * 4; ++i) wr32(o + 4 * i, rd32be(s + 4 * i));
        return;
    case TEX_R6G5B5:
        for (size_t i = 0; i < count; ++i) {
            const u16 v = rd16be(s + 2 * i);
            wr16(o + 2 * i, u16((v & 0xF81F) | ((v & 0x3E0) << 1)));  // drop R's LSB, G5 -> G6
        }
        return;
    case TEX_DEPTH24_D8:
        for (size_t i = 0; i < count; ++i) {
            const float f = float(rd32be(s + 4 * i) >> 8) / 16777215.0f;
            std::memcpy(o + 4 * i, &f, 4);
        }
        return;
    case TEX_DEPTH24_D8_FLOAT:
        for (size_t i = 0; i < count; ++i) {
            const float f = std::bit_cast<float>((rd32be(s + 4 * i) >> 8) << 7);
            std::memcpy(o + 4 * i, &f, 4);
        }
        return;
    case TEX_DEPTH16_FLOAT:
        for (size_t i = 0; i < count; ++i) {
            const float f = decode_e4m12(rd16be(s + 2 * i));
            std::memcpy(o + 4 * i, &f, 4);
        }
        return;
    case TEX_B8R8_G8R8: case TEX_R8B8_R8G8:
        for (size_t i = 0; i < count; ++i) {
            const u8* p = s + 4 * i;
            u32 r0, r1, bl, gr;
            if (fmt == TEX_B8R8_G8R8) { bl = p[0]; r0 = p[1]; gr = p[2]; r1 = p[3]; }
            else { r0 = p[0]; bl = p[1]; r1 = p[2]; gr = p[3]; }
            wr32(o + 8 * i, bl | (gr << 8) | (r0 << 16) | 0xFF000000u);
            wr32(o + 8 * i + 4, bl | (gr << 8) | (r1 << 16) | 0xFF000000u);
        }
        return;
    default:
        return;
    }
}

bool deswizzle_applies(u32 fmt) {
    // DXT and the B8R8/R8B8 packed formats are never Z-ordered (packed rows only).
    return !is_compressed_dxt(fmt) && fmt != TEX_B8R8_G8R8 && fmt != TEX_R8B8_R8G8;
}

// Undo NVIDIA VTC tiling of compressed 3D textures (groups of 4 slices interleaved per block).
void undo_vtc(const u8* src, u8* dst, u32 blocks_per_slice, u32 depth, u32 bsize) {
    u32 slice = 0;
    size_t group_off = 0;
    while (slice < depth) {
        const u32 n = std::min(4u, depth - slice);
        for (u32 s = 0; s < n; ++s)
            for (u32 i = 0; i < blocks_per_slice; ++i)
                std::memcpy(dst + (size_t(slice + s) * blocks_per_slice + i) * bsize,
                            src + (group_off + size_t(i) * n + s) * bsize, bsize);
        group_off += size_t(blocks_per_slice) * n;
        slice += n;
    }
}

}  // namespace

// ---- public -----------------------------------------------------------------------------
bool is_supported_format(u32 f) { BlockInfo bi; return block_info(f, bi); }
bool is_depth_format(u32 f) { return f >= TEX_DEPTH24_D8 && f <= TEX_DEPTH16_FLOAT; }
bool is_compressed_dxt(u32 f) { return f == TEX_DXT1 || f == TEX_DXT23 || f == TEX_DXT45; }

void deswizzle(const u8* src, u8* dst, u32 w, u32 h, u32 d, u32 unit, size_t src_size) {
    switch (unit) {
    case 1: deswizzle_t<1>(src, dst, w, h, d, src_size); break;
    case 2: deswizzle_t<2>(src, dst, w, h, d, src_size); break;
    default: deswizzle_t<4>(src, dst, w, h, d, src_size); break;
    }
}

void swizzle(const u8* src, u8* dst, u32 w, u32 h, u32 d, u32 unit) {
    switch (unit) {
    case 1: swizzle_t<1>(src, dst, w, h, d); break;
    case 2: swizzle_t<2>(src, dst, w, h, d); break;
    default: swizzle_t<4>(src, dst, w, h, d); break;
    }
}

VkFormat host_format(u32 f) {
    switch (f) {
    case TEX_B8: return VK_FORMAT_R8_UNORM;
    case TEX_A1R5G5B5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case TEX_A4R4G4B4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case TEX_R5G6B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case TEX_A8R8G8B8: return VK_FORMAT_B8G8R8A8_UNORM;
    case TEX_DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TEX_DXT23: return VK_FORMAT_BC2_UNORM_BLOCK;
    case TEX_DXT45: return VK_FORMAT_BC3_UNORM_BLOCK;
    case TEX_G8B8: return VK_FORMAT_R8G8_UNORM;
    case TEX_B8R8_G8R8: case TEX_R8B8_R8G8: return VK_FORMAT_B8G8R8A8_UNORM;
    case TEX_R6G5B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case TEX_DEPTH24_D8: case TEX_DEPTH24_D8_FLOAT: case TEX_DEPTH16_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case TEX_DEPTH16: return VK_FORMAT_R16_UNORM;
    case TEX_X16: return VK_FORMAT_R16_UNORM;
    case TEX_Y16_X16: return VK_FORMAT_R16G16_UNORM;
    case TEX_R5G5B5A1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case TEX_HILO8: return VK_FORMAT_R8G8_UNORM;
    case TEX_HILO_S8: return VK_FORMAT_R8G8_SNORM;
    case TEX_W16_Z16_Y16_X16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TEX_W32_Z32_Y32_X32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TEX_X32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case TEX_D1R5G5B5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case TEX_D8R8G8B8: return VK_FORMAT_B8G8R8A8_UNORM;
    case TEX_Y16_X16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

std::array<VkComponentSwizzle, 4> base_mapping_argb(u32 f) {
    switch (f) {
    case TEX_A1R5G5B5: case TEX_R5G5B5A1: case TEX_R6G5B5: case TEX_R5G6B5:
    case TEX_DXT1: case TEX_DXT23: case TEX_DXT45:
    case TEX_W16_Z16_Y16_X16_FLOAT: case TEX_W32_Z32_Y32_X32_FLOAT:
    case TEX_B8R8_G8R8: case TEX_R8B8_R8G8: case TEX_A8R8G8B8:
        return {A, R, G, B};
    case TEX_DEPTH24_D8: case TEX_DEPTH24_D8_FLOAT: case TEX_DEPTH16: case TEX_DEPTH16_FLOAT:
    case TEX_X32_FLOAT:
        return {R, R, R, R};
    case TEX_A4R4G4B4: return {R, G, B, A};
    case TEX_G8B8: return {G, R, G, R};
    case TEX_B8: return {ONE, R, R, R};
    case TEX_X16: return {R, ONE, R, ONE};
    case TEX_Y16_X16: return {G, R, G, R};
    case TEX_Y16_X16_FLOAT: return {R, G, R, G};
    case TEX_D8R8G8B8: case TEX_D1R5G5B5: return {ONE, R, G, B};
    case TEX_HILO8: case TEX_HILO_S8: return {R, G, R, G};
    default: return {A, R, G, B};
    }
}

u32 effective_remap(u32 f, u32 remap) {
    const u32 override_hi = (remap >> 16) & 0xFFFF;
    switch (f) {
    case TEX_X32_FLOAT: case TEX_W32_Z32_Y32_X32_FLOAT: case TEX_W16_Z16_Y16_X16_FLOAT:
        // Float textures cannot be remapped on real hardware
        return (remap & ~0xFFu) | 0xE4;
    case TEX_Y16_X16_FLOAT:
        return (remap & ~0xFFu) | (override_hi ? 0x56 : 0x66);
    case TEX_X16: case TEX_Y16_X16: case TEX_HILO8: case TEX_HILO_S8: {
        // 16-bit channel formats: remap selects are in 16-bit units (0x4 = first, 0xE = second channel)
        u32 lo = remap & 0xFF;
        switch (lo) {
        case 0xE4: lo = override_hi ? 0x56 : 0x66; break;
        case 0x4E: lo = override_hi ? 0xA9 : 0x99; break;
        case 0xEE: lo = 0xAA; break;
        case 0x44: lo = 0x55; break;
        }
        return (remap & 0xFF00) | lo;
    }
    default:
        return remap;
    }
}

VkComponentMapping apply_remap(const std::array<VkComponentSwizzle, 4>& base, u32 remap) {
    VkComponentSwizzle out[4];
    for (u32 ch = 0; ch < 4; ++ch) {
        const u32 sel = (remap >> (2 * ch)) & 3;
        const u32 ctl = (remap >> (8 + 2 * ch)) & 3;
        out[ch] = ctl == 0 ? ZERO : ctl == 1 ? ONE : base[sel];  // 2 (and undefined 3) = REMAP
    }
    return {out[1], out[2], out[3], out[0]};
}

VkComponentMapping texture_mapping(u32 format_reg, u32 remap) {
    const u32 f = tex_base_format(format_reg);
    if (is_depth_format(f)) return {R, R, R, R};  // RPCS3: depth formats ignore remap
    return apply_remap(base_mapping_argb(f), effective_remap(f, remap));
}

VkComponentMapping surface_sample_mapping(u32 format_reg, u32 remap) {
    const u32 f = tex_base_format(format_reg);
    std::array<VkComponentSwizzle, 4> base;  // ARGB order, in terms of the natural-RGBA surface image
    switch (f) {
    case TEX_DEPTH24_D8: case TEX_DEPTH24_D8_FLOAT: case TEX_DEPTH16: case TEX_DEPTH16_FLOAT:
        return {R, R, R, R};
    case TEX_B8: base = {ONE, B, B, B}; break;           // b8 surface: data is the fragment output .b
    case TEX_G8B8: base = {G, B, G, B}; break;           // memory {G,R,G,R} with R=B byte, G=G byte
    case TEX_D8R8G8B8: case TEX_D1R5G5B5: base = {ONE, R, G, B}; break;
    case TEX_X32_FLOAT: base = {R, R, R, R}; break;
    case TEX_A4R4G4B4: base = {A, R, G, B}; break;      // logical ARGB, not the packed host layout
    case TEX_X16: case TEX_Y16_X16: case TEX_Y16_X16_FLOAT: case TEX_HILO8: case TEX_HILO_S8:
        base = base_mapping_argb(f); break;              // first 16-bit word in memory == surface R
    default: base = {A, R, G, B}; break;                 // A8R8G8B8, R5G6B5, A1R5G5B5, W16/W32 (R = W = first word)
    }
    return apply_remap(base, effective_remap(f, remap));
}

u32 texture_guest_size(const TexDesc& t) {
    Layout L;
    if (!compute_layout(t, L)) return 0;
    return L.total;
}

bool decode_texture(const TexDesc& t, DecodedTex& out) { return decode_texture_from(t, gptr(t.ea), out); }

bool decode_texture_from(const TexDesc& t, const u8* data, DecodedTex& out) {
    Layout L;
    out = DecodedTex{};
    if (!compute_layout(t, L)) {
        LOG_ERR("rsx_tex: unsupported texture format 0x%02x (FORMAT=0x%08x) at 0x%08x", (t.format >> 8) & 0xFF, t.format, t.ea);
        return false;
    }
    if (u64(t.ea) + L.total > (1ull << 32)) {
        LOG_ERR("rsx_tex: texture 0x%08x size 0x%x wraps the address space", t.ea, L.total);
        return false;
    }
    const u32 fmt = L.fmt;
    const BlockInfo bi = L.bi;
    const u32 hbytes = host_block_bytes(fmt, bi);
    const bool dxt = is_compressed_dxt(fmt);
    const bool deswz = !L.linear && deswizzle_applies(fmt);

    out.format = host_format(fmt);
    out.layers = L.layers;
    out.view_type = L.cube ? VK_IMAGE_VIEW_TYPE_CUBE : L.dim == 3 ? VK_IMAGE_VIEW_TYPE_3D
                  : L.dim == 1 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
    out.mapping = texture_mapping(t.format, t.remap);

    size_t total = 0;
    for (const auto& s : L.levels) total += size_t(s.wb) * s.hb * s.d * hbytes;
    out.data.resize(total);
    out.levels.reserve(L.levels.size());

    std::vector<u8> padded, blocks, tmp;
    size_t dst_off = 0;
    for (const auto& s : L.levels) {
        const u8* src = data + s.offset;
        const size_t row_bytes = size_t(s.wb) * bi.bytes;
        const size_t nblocks = size_t(s.wb) * s.hb * s.d;
        blocks.resize(nblocks * bi.bytes);

        if (deswz) {
            // Z-order is applied to units of at most 4 bytes; wider texels are consecutive units.
            const u32 unit = std::min(bi.bytes, 4u);
            const u32 k = bi.bytes / unit;
            const u32 pw = s.pitch / bi.bytes;  // padded width in blocks
            padded.resize(size_t(pw) * s.full_h * s.d * bi.bytes);
            deswizzle(src, padded.data(), pw * k, s.full_h, s.d, unit, s.size);
            for (u32 z = 0; z < s.d; ++z)
                for (u32 y = 0; y < s.hb; ++y)
                    std::memcpy(blocks.data() + (size_t(z) * s.hb + y) * row_bytes,
                                padded.data() + ((size_t(z) * s.full_h + y + s.border) * pw + s.border) * bi.bytes,
                                row_bytes);
        } else {
            for (u32 z = 0; z < s.d; ++z)
                for (u32 y = 0; y < s.hb; ++y)
                    std::memcpy(blocks.data() + (size_t(z) * s.hb + y) * row_bytes,
                                src + (size_t(z) * s.full_h + y + s.border) * s.pitch + size_t(s.border) * bi.bytes,
                                row_bytes);
            if (dxt && s.d > 1 && std::has_single_bit(s.w) && std::has_single_bit(s.h)) {
                tmp.resize(blocks.size());
                undo_vtc(blocks.data(), tmp.data(), s.wb * s.hb, s.d, bi.bytes);
                blocks.swap(tmp);
            }
        }

        const size_t out_size = nblocks * hbytes;
        convert_blocks(fmt, blocks.data(), out.data.data() + dst_off, nblocks);
        TexLevel lv{};
        lv.offset = u32(dst_off);
        lv.width = s.w;
        lv.height = s.h;
        lv.depth = s.d;
        lv.row_length = s.wb * bi.bw;
        lv.size = u32(out_size);
        out.levels.push_back(lv);
        dst_off += out_size;
    }
    return true;
}

// ---- hashing ----------------------------------------------------------------------------
namespace {
constexpr u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
              P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
inline u64 rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
inline u64 rd64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
inline u64 round1(u64 acc, u64 in) { acc += in * P2; acc = rotl(acc, 31); return acc * P1; }
inline u64 merge(u64 acc, u64 v) { acc ^= round1(0, v); return acc * P1 + P4; }
}  // namespace

u64 xxh64(const void* data, size_t len, u64 seed) {
    const u8* p = static_cast<const u8*>(data);
    const u8* end = p + len;
    u64 h;
    if (len >= 32) {
        u64 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const u8* limit = end - 32;
        do {
            v1 = round1(v1, rd64(p)); v2 = round1(v2, rd64(p + 8));
            v3 = round1(v3, rd64(p + 16)); v4 = round1(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += u64(len);
    while (p + 8 <= end) { h ^= round1(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { u32 v; std::memcpy(&v, p, 4); h ^= u64(v) * P1; h = rotl(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p++) * P5; h = rotl(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

u64 hash_guest_range(u32 ea, u32 size) {
    if (u64(ea) + size > (1ull << 32)) size = u32((1ull << 32) - ea);
    return xxh64(gptr(ea), size, 0);
}

}  // namespace rsx
