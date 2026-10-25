// Native renderer, capture side: runs on the RSX command thread. Turns draws, clears, blits and
// flips into packets for the render thread and copies what they read from guest memory into the
// data ring (see vkn.h).
#include "vkn.h"

#include "rsx.h"
#include "gfx.h"
#include "rsx_fp.h"
#include "rsx_vp.h"
#include "vk_backend.h"
#include "skins.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

extern unsigned g_vc_stamp[468], g_vc_ea[468], g_draw_no;
namespace rsx {
namespace vkn {
namespace {

constexpr u64 kMaxFramesAhead = 2;
constexpr size_t kBatchBytes = 256 * 1024;

inline u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

// Registers the render thread mirrors: the NV4097 3D class (byte offsets below 0x2000), minus
// the program/constant upload windows and immediate vertex data, which reach it another way.
struct RegMask {
    u64 bits[0x800 / 64] = {};
    RegMask() {
        for (u32 r = 0; r < 0x800; ++r) bits[r >> 6] |= 1ull << (r & 63);
        auto clear = [&](u32 byte_lo, u32 byte_hi) { for (u32 b = byte_lo; b < byte_hi; b += 4) bits[(b >> 2) >> 6] &= ~(1ull << ((b >> 2) & 63)); };
        clear(0x0b80, 0x0c00);  // transform program
        clear(0x1f00, 0x1f80);  // transform constants
        clear(0x0a80, 0x0b00);  // vertex data scaled4s
        clear(0x1500, 0x15c0);  // vertex data 3f
        clear(0x1880, 0x1980);  // vertex data 2f, 2s, 4ub
        clear(0x1980, 0x1a00);  // vertex data 4s
        clear(0x1c00, 0x1d00);  // vertex data 4f
        clear(0x1e40, 0x1e80);  // vertex data 1f
    }
};
const RegMask g_regmask;

u64 fnv(const void* p, size_t n, u64 h = 0xcbf29ce484222325ull) {
    const u8* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// Cheap fingerprint of a large guest range: 256 samples of 8 bytes plus the first 256 bytes.
u64 signature(u32 ea, u32 size) {
    if (!size || !mem::is_mapped(ea, size)) return 0;
    const u8* p = mem::base + ea;
    u64 h = xxh64(p, std::min<u32>(size, 256));
    if (size > 512) {
        const u32 step = (size - 8) / 256;
        for (u32 i = 0; i < 256; ++i) { u64 v; std::memcpy(&v, p + (u64(i) * step & ~7ull), 8); h = (h ^ v) * 0x9E3779B185EBCA87ull; h ^= h >> 29; }
    }
    return h ? h : 1;
}

u32 vertex_type_bytes(u32 type, u32 size) {
    switch (type) {
    case 1: case 3: case 5: return 2 * size;
    case 2: return 4 * size;
    case 4: case 7: return size;
    case 6: return 4;
    default: return 0;
    }
}

class Capture final : public Renderer {
public:
    Capture(Shared& sh, RenderThread* rt, bool sync) : sh_(sh), ring_(sh.ring), rt_(rt), sync_(sync) { batch_ = sh_.queue.get(); }
    void draw() override;
    void clear(u32 mask) override;
    void flip(u32 buffer) override;
    // An RSX write into guest memory (copy, inline transfer, blit fallback): textures over that
    // range are hashed again before their next use. CPU and SPU writes are caught by hashing
    // every used texture once per frame (flip bumps tex_epoch_).
    void invalidate(u32 ea, u32 size) override {
        if (!size) return;
        if (!dirty_.empty() && dirty_.back().second == ea) { dirty_.back().second = ea + size; return; }  // inline transfers come a word at a time
        if (dirty_.size() >= 256) { ++tex_epoch_; dirty_.clear(); return; }
        dirty_.push_back({ea, ea + size});
    }
    bool blit(const BlitParams& b) override;

private:
    Shared& sh_;
    DataRing& ring_;
    RenderThread* rt_;
    bool sync_;
    Batch* batch_;
    u64 flips_ = 0;
    u64 tex_epoch_ = 1;
    std::vector<std::pair<u32, u32>> dirty_;  // [begin, end) written by the RSX since tex_epoch_ last changed

    // ---- surfaces: which guest ranges currently hold a render target
    struct Surf { u32 ea, pitch, w, h, fmt; bool depth; u64 sig = 0, sig_frame = ~0ull, checked_frame = ~0ull, used_frame = 0; };
    std::unordered_map<u64, Surf> surfs_;  // key: ea << 1 | depth
    // Which colour surface holds an address (see containing): looked up once while the set of
    // surfaces stays the same. Split screen asks for every texture it binds, and per-object
    // shadow maps leave hundreds of surfaces behind over a session.
    u64 surf_gen_ = 0, contain_gen_ = ~0ull;
    std::unordered_map<u64, u64> contain_;   // (ea, pitch, strict) -> surface key, or ~0 for none

    // ---- programs
    u64 vp_gen_ = 0;
    u32 vp_start_ = ~0u;
    VpInfo vinfo_{};
    std::unordered_set<u64> vp_sent_;
    struct Prog { std::vector<u8> bytes; FpInfo info; u64 key = 0; u64 fc_pos = ~0ull; u32 fc_size = 0; };
    std::unordered_map<u32, Prog> fps_;
    std::unordered_set<u64> fp_sent_;
    u64 vc_gen_ = 0, vc_pos_ = ~0ull;

    // ---- textures
    struct Tex { TexDesc d; u32 id = 0, size = 0, dirty_seen = 0, skin_gen = 0; u64 hash = 0, epoch = 0, last_frame = 0; bool bad = false; };
    std::unordered_map<u64, Tex> texs_;
    u32 next_tex_id_ = 1;

    // ---- scratch
    std::vector<u32> idx_;

    // ring
    u64 alloc(u64 n, u64 align);
    // Data written at pos can be referenced by another packet: the render thread releases ring
    // space only up to the position of a flip or a submit, so data written after the last of
    // those stays until every packet sent since has finished on the GPU. Older data may be
    // overwritten before a new packet that names it is executed.
    u64 keep_from_ = 0;
    bool intact(u64 pos) const { return pos != ~0ull && pos >= keep_from_; }
    u8* at(u64 pos) const { return ring_.ptr + pos % ring_.size; }
    u32 off(u64 pos) const { return u32(pos % ring_.size); }

    // batches
    void* packet(u32 type, u32 size);
    void flush_batch();
    void send_regs();

    TargetDesc targets(bool for_clear);
    // gfx::g.passlog: one log line per run of draws/clears to the same targets, until the flip
    char pl_kind_ = 0; TargetDesc pl_t_{}; u32 pl_n_ = 0; u32 pl_vp_[11] = {};
    void passlog_note(char kind, const TargetDesc& t) {
        if (gfx::g.passlog.load(std::memory_order_relaxed) <= 0) return;
        const u32 vp[11] = {reg(0xa00), reg(0xa04), reg(0x8c0), reg(0x8c4), reg(0xa24), reg(0xa34), reg(0x1d88), reg(0x2b8), reg(0xa74), reg(0xa70), reg(0xa6c)};
        const bool same = kind == pl_kind_ && t.color_ea[0] == pl_t_.color_ea[0] && t.zea == pl_t_.zea && t.clip_x == pl_t_.clip_x &&
                          t.clip_y == pl_t_.clip_y && t.clip_w == pl_t_.clip_w && t.clip_h == pl_t_.clip_h && !std::memcmp(vp, pl_vp_, sizeof vp);
        if (same) { ++pl_n_; return; }
        if (pl_n_) LOG_NOTE("pass: %c x%u color %08x z %08x clip %u,%u %ux%u fmt %u/%u vp %08x %08x sc %08x %08x offy %08x scaley %08x win %08x woff %08x ztest %x zmask %x zfunc %x", pl_kind_, pl_n_, pl_t_.color_ea[0],
                            pl_t_.zea, pl_t_.clip_x, pl_t_.clip_y, pl_t_.clip_w, pl_t_.clip_h, pl_t_.color_fmt, pl_t_.depth_fmt, pl_vp_[0], pl_vp_[1], pl_vp_[2], pl_vp_[3], pl_vp_[4], pl_vp_[5], pl_vp_[6], pl_vp_[7], pl_vp_[8], pl_vp_[9], pl_vp_[10]);
        pl_kind_ = kind; pl_t_ = t; pl_n_ = 1; std::memcpy(pl_vp_, vp, sizeof vp);
        if (kind == 'F') { LOG_NOTE("pass: flip"); pl_n_ = 0; pl_kind_ = 0; gfx::g.passlog.fetch_sub(1); }
    }
    // Split screen (co-op, gfx::g.split_views): each view copies its scene into the display buffer
    // (from row 8) and its post effects read that copy later, after the next view has copied over
    // it. Each copy is kept as a surface of its own and the reads go to the copies in order.
    u32 vc_count_ = 0, vc_read_ = 0, vc_ea_ = 0, vc_dst_ = 0;
    u32 vc_miss_ = 0;   // this frame: why full-width draws at row 8 weren't taken as scene copies (bits, see draw)
    u32 skip_n_ = 0;   // the right view's draws so far this frame (gfx::g.skip_from/skip_to)
    void note_surface(u32 ea, bool depth, u32 fmt, u32 w, u32 h, u32 pitch);
    bool surface_current(Surf& s);
    void drop_surface(u64 key);
    Surf* containing(u32 ea, u32 pitch, bool strict, u64* key = nullptr);
    void evict_surfaces();
    TexBinding bind_texture(u32 unit);
    bool program(u32 fea, Prog*& out);
};

// ---- ring / batches -------------------------------------------------------------------------
u64 Capture::alloc(u64 n, u64 align) {
    if (n > ring_.size / 4) { LOG_ERR("vkn: %llu-byte upload does not fit the data ring", (unsigned long long)n); n = ring_.size / 4; }
    u64 pos = align_up(ring_.head, std::max<u64>(align, 4));
    if (pos % ring_.size + n > ring_.size) pos = align_up(pos, ring_.size);
    if (pos + n - ring_.tail.load(std::memory_order_acquire) > ring_.size) {
        // ask the render thread to submit what it has so the GPU can release ring space
        *static_cast<u64*>(packet(P_SUBMIT, 8)) = ring_.head;
        keep_from_ = ring_.head;
        flush_batch();
        std::unique_lock lk(ring_.lock);
        while (pos + n - ring_.tail.load(std::memory_order_acquire) > ring_.size)
            ring_.released.wait_for(lk, std::chrono::milliseconds(50));
    }
    ring_.head = pos + n;
    return pos;
}

void* Capture::packet(u32 type, u32 size) {
    size = (size + 3) & ~3u;
    auto& v = batch_->bytes;
    const size_t at = v.size();
    v.resize(at + sizeof(PacketHeader) + size);
    PacketHeader h{type, size};
    std::memcpy(v.data() + at, &h, sizeof h);
    return v.data() + at + sizeof h;
}

void Capture::flush_batch() {
    if (batch_->bytes.empty()) return;
    if (sync_) { process_batch(rt_, batch_); batch_->bytes.clear(); return; }
    sh_.queue.push(batch_);
    batch_ = sh_.queue.get();
}

void Capture::send_regs() {
    u32 n = 0;
    for (u32 w = 0; w < 0x800 / 64; ++w) n += __builtin_popcountll(g_reg_dirty[w] & g_regmask.bits[w]);
    if (!n) return;
    u32* p = static_cast<u32*>(packet(P_REGS, 4 + 8 * n));
    *p++ = n;
    for (u32 w = 0; w < 0x800 / 64; ++w) {
        u64 bits = g_reg_dirty[w] & g_regmask.bits[w];
        g_reg_dirty[w] = 0;
        while (bits) {
            const u32 r = w * 64 + __builtin_ctzll(bits);
            bits &= bits - 1;
            *p++ = r;
            *p++ = g_regs[r];
        }
    }
}

// ---- surfaces ---------------------------------------------------------------------------------
TargetDesc Capture::targets(bool for_clear) {
    TargetDesc t;
    const u32 fmt = reg(0x208);
    t.color_fmt = fmt & 0x1f;
    t.depth_fmt = (fmt >> 5) & 7;
    t.clip_x = reg(0x200) & 0xffff; t.clip_w = reg(0x200) >> 16;
    t.clip_y = reg(0x204) & 0xffff; t.clip_h = reg(0x204) >> 16;
    switch ((fmt >> 12) & 0xf) {
    case 3: t.aa_x = 2; break;
    case 4: case 5: t.aa_x = t.aa_y = 2; break;
    default: break;
    }
    t.w = (t.clip_x + t.clip_w) * t.aa_x;
    t.h = (t.clip_y + t.clip_h) * t.aa_y;
    if (!t.clip_w || !t.clip_h || t.w > 4096 || t.h > 4096) return t;
    static const u32 off_regs[4] = {0x210, 0x218, 0x288, 0x28c}, dma_regs[4] = {0x194, 0x18c, 0x1b4, 0x1b8},
                     pitch_regs[4] = {0x20c, 0x21c, 0x280, 0x284};
    u32 idx[4], n = 0;
    switch (reg(0x220)) {
    case 1: idx[n++] = 0; break;
    case 2: idx[n++] = 1; break;
    case 0x13: idx[n++] = 0; idx[n++] = 1; break;
    case 0x17: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; break;
    case 0x1f: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; idx[n++] = 3; break;
    default: break;
    }
    for (u32 k = 0; k < n; ++k) {
        const u32 i = idx[k], pitch = reg(pitch_regs[i]);
        const u32 ea = address(reg(off_regs[i]), reg(dma_regs[i]));
        if (pitch <= 64 || ea == ~0u) continue;
        t.color_ea[t.ncolor] = ea;
        t.color_pitch[t.ncolor] = pitch;
        ++t.ncolor;
        note_surface(ea, false, t.color_fmt, t.w, t.h, pitch);
    }
    const u32 zpitch = reg(0x22c);
    const u32 zea = address(reg(0x214), reg(0x198));
    const bool need_depth = for_clear || (reg(0xa74) & 1) || (reg(0x328) & 1);
    if (t.depth_fmt && zpitch > 64 && zea != ~0u && need_depth) {
        t.has_depth = 1; t.zea = zea; t.zpitch = zpitch;
        note_surface(zea, true, t.depth_fmt, t.w, t.h, zpitch);
    }
    t.valid = t.ncolor || t.has_depth;
    return t;
}

void Capture::note_surface(u32 ea, bool depth, u32 fmt, u32 w, u32 h, u32 pitch) {
    const u64 key = (u64(ea) << 1) | (depth ? 1 : 0);
    auto [it, fresh] = surfs_.try_emplace(key);
    Surf& s = it->second;
    if (fresh) { s.ea = ea; s.depth = depth; s.w = w; s.h = h; ++surf_gen_; }
    if (w > s.w || h > s.h || pitch != s.pitch) ++surf_gen_;
    s.w = std::max(s.w, w); s.h = std::max(s.h, h);
    s.pitch = pitch; s.fmt = fmt;
    s.used_frame = flips_;
    // fingerprint the guest memory under the surface once per frame it is drawn to: if it
    // changes later, something other than rendering wrote there
    if (s.sig_frame != flips_) { s.sig = signature(ea, pitch * s.h); s.sig_frame = flips_; s.checked_frame = flips_; }
}

bool Capture::surface_current(Surf& s) {
    static const bool check = std::getenv("TOD_VKN_SURFSIG") != nullptr;
    if (!check) return true;
    if (s.checked_frame == flips_) return true;
    s.checked_frame = flips_;
    return signature(s.ea, s.pitch * s.h) == s.sig;
}

void Capture::drop_surface(u64 key) {
    auto it = surfs_.find(key);
    if (it == surfs_.end()) return;
    u32* p = static_cast<u32*>(packet(P_DROP_SURFACE, 8));
    p[0] = it->second.ea;
    p[1] = it->second.depth;
    if (g_log_level >= 2) LOG_NOTE("vkn: guest memory under the %s surface at 0x%08x was rewritten", it->second.depth ? "depth" : "color", it->second.ea);
    surfs_.erase(it);
    ++surf_gen_;
}

// The colour surface that holds ea and starts nearest before it: one with that pitch starting before ea
// when strict, else any starting at or before it. Remembered until a surface comes, goes or grows.
Capture::Surf* Capture::containing(u32 ea, u32 pitch, bool strict, u64* key) {
    if (contain_gen_ != surf_gen_) { contain_.clear(); contain_gen_ = surf_gen_; }
    const u64 ck = (u64(ea) << 32) | (u64(pitch) << 1) | (strict ? 1 : 0);
    auto [c, fresh] = contain_.try_emplace(ck, ~0ull);
    if (fresh) {
        u32 best = 0;
        for (auto& [k, sf] : surfs_) {
            if (sf.depth || !sf.pitch) continue;
            if (strict ? (sf.pitch != pitch || ea <= sf.ea) : ea < sf.ea) continue;
            if (ea >= sf.ea + sf.pitch * sf.h) continue;
            if (c->second != ~0ull && sf.ea <= best) continue;   // of several, the one starting nearest
            c->second = k;
            best = sf.ea;
        }
    }
    if (c->second == ~0ull) return nullptr;
    auto it = surfs_.find(c->second);
    if (it == surfs_.end()) return nullptr;
    if (key) *key = c->second;
    return &it->second;
}

// Surfaces nothing drew to or sampled for a while are forgotten here, before the render thread
// frees its copy of them (after 3600 frames): a texture there reads guest memory again instead of a
// surface that is gone.
void Capture::evict_surfaces() {
    // with many surfaces (see RenderThread::evict_surfaces) after 100 frames, before the 120 there
    const bool crowded = surfs_.size() > 512;
    const u64 idle = crowded ? 100 : 3000;
    if (flips_ % (crowded ? 30 : 600) != (crowded ? 15 : 300)) return;
    std::vector<u64> old;
    for (auto& [k, sf] : surfs_)
        if (flips_ - sf.used_frame > idle) old.push_back(k);
    for (u64 k : old) {
        u32* p = static_cast<u32*>(packet(P_DROP_SURFACE, 8));
        p[0] = surfs_[k].ea;
        p[1] = surfs_[k].depth;
        surfs_.erase(k);
    }
    if (!old.empty()) { ++surf_gen_; if (g_log_level >= 1) LOG_NOTE("vkn: forgot %zu surfaces unused for %llu frames (%zu left)", old.size(), (unsigned long long)idle, surfs_.size()); }
}

// ---- textures ---------------------------------------------------------------------------------
// Copies of parts of surfaces are kept as surfaces at these (unmapped) addresses.
constexpr u32 kSubSurfaceBase = 0xF0000000u;
constexpr u32 kViewCopyBase = 0xF8000000u, kViewCopyStride = 0x00800000u;

TexBinding Capture::bind_texture(u32 unit) {
    TexBinding b{};
    const u32 base = 0x1a00 + 0x20 * unit;
    const u32 ctl0 = reg(base + 0x0c), fmt = reg(base + 0x04);
    const bool cube = fmt & 4;
    const u32 dim = (fmt >> 4) & 0xf;
    const TexKind kind = cube ? TexKind::Cube : dim == 3 ? TexKind::Tex3D : TexKind::Tex2D;
    b.view = u8(kind);
    if (!(ctl0 & 0x80000000u)) return b;
    TexDesc d;
    d.format = fmt;
    const u32 rect = reg(base + 0x18);
    d.width = rect >> 16;
    d.height = rect & 0xffff;
    d.depth = reg(0x1840 + 4 * unit) >> 20;
    if (!d.depth) d.depth = 1;
    d.pitch = reg(0x1840 + 4 * unit) & 0xfffff;
    d.mipmaps = std::max<u32>(1, fmt >> 16);
    d.remap = reg(base + 0x10);
    d.ea = address(reg(base), (fmt & 3) - 1);
    if (d.ea == ~0u || !d.width || !d.height) return b;
    if (const int pl = gfx::g.passlog.load(std::memory_order_relaxed); pl > 0 && (d.width >= 320 || pl >= 1000))
        LOG_NOTE("pass:   tex%u ea %08x %ux%u pitch %u fmt %08x", unit, d.ea, d.width, d.height, d.pitch, fmt);

    if (kind == TexKind::Tex2D) {   // render to texture: sample the surface
        // A pass may bind a colour and a depth surface at the same address (shadow maps render
        // depth with a dummy colour target there); a depth texture format reads the depth one.
        const int first = is_depth_format(tex_base_format(fmt)) ? 1 : 0;
        static const bool strace = std::getenv("TOD_SAMPLE_TRACE") != nullptr;
        if (strace && first) {
            static std::set<std::string> seen;
            char w[160];
            std::snprintf(w, sizeof w, "depth texture ea 0x%08x %ux%u fmt 0x%08x: color surf %d depth surf %d", d.ea, d.width, d.height, fmt,
                          int(surfs_.count(u64(d.ea) << 1)), int(surfs_.count((u64(d.ea) << 1) | 1)));
            if (seen.insert(w).second) LOG_NOTE("vkn: %s", w);
        }
        for (int n = 0; n < 2; ++n) {
            const int depth = n ^ first;
            const u64 key = (u64(d.ea) << 1) | u64(depth);
            auto it = surfs_.find(key);
            if (it == surfs_.end()) continue;
            if (!surface_current(it->second)) { drop_surface(key); continue; }
            it->second.used_frame = flips_;
            b.kind = depth ? TB_DEPTH_SURFACE : TB_COLOR_SURFACE;
            b.id_or_ea = d.ea;
            return b;
        }
        if (!first && vc_count_ && d.ea == vc_ea_) {
            static const bool vtrace = std::getenv("TOD_SPLIT_TRACE") != nullptr;
            if (vtrace) LOG_NOTE("vkn: copy read %u of %u: %ux%u fmt 0x%08x", vc_read_, vc_count_, d.width, d.height, d.format);
            b.kind = TB_COLOR_SURFACE;
            b.id_or_ea = kViewCopyBase + std::min(vc_read_, vc_count_ - 1) * kViewCopyStride;
            ++vc_read_;   // each view's post effects read the copy once
            return b;
        }
        // A texture that starts inside a colour surface: copy that part to a surface of its own
        // and sample it. (Split screen only for now: elsewhere these reads still come from guest memory.)
        if (!first && gfx::g.split_views.load(std::memory_order_relaxed))
            for (Surf* ps = containing(d.ea, d.pitch, true); ps; ps = nullptr) {
                const Surf& sf = *ps;
                if (sf.fmt != 8 && sf.fmt != 3) break;
                const u32 bpp = sf.fmt == 8 ? 4 : 2, off = d.ea - sf.ea;
                const u32 x = (off % sf.pitch) / bpp, y = off / sf.pitch;
                if (x + d.width > sf.w || y + d.height > sf.h) break;
                BlitParams bp{};
                bp.src_ea = d.ea; bp.src_pitch = sf.pitch; bp.src_w = d.width; bp.src_h = d.height; bp.src_bpp = bpp;
                bp.sx = 0; bp.sy = 0; bp.sw = float(d.width); bp.sh = float(d.height);
                bp.dst_ea = kSubSurfaceBase | (d.ea & 0x0FFFFFFFu); bp.dst_pitch = d.width * bpp; bp.dst_bpp = bpp;
                bp.dst_fmt = sf.fmt == 8 ? 0xa : 4;
                bp.dx = 0; bp.dy = 0; bp.dw = d.width; bp.dh = d.height;
                bp.linear = false;
                if (!blit(bp)) break;
                b.kind = TB_COLOR_SURFACE;
                b.id_or_ea = bp.dst_ea;
                return b;
            }
    }

    const u64 key = xxh64(&d, sizeof d);
    Tex& t = texs_[key];
    t.last_frame = flips_;
    if (!t.id) {
        t.d = d;
        t.id = next_tex_id_++;
        t.size = texture_guest_size(d);
        t.bad = !t.size || !mem::is_mapped(d.ea, t.size) || t.size > ring_.size / 8;
        if (t.bad) {
            // drawn with a blank texture: logged (a few times) so a black surface in a log can be traced
            static int logged = 0;
            if (logged < 24) {
                ++logged;
                LOG_NOTE("vkn: texture not drawn: ea 0x%08x %ux%u pitch %u fmt 0x%08x mips %u, %u bytes%s", d.ea, d.width, d.height, d.pitch, d.format,
                         d.mipmaps, t.size, !t.size ? " (unknown size)" : t.size > ring_.size / 8 ? " (too big)" : " (not all in mapped memory)");
            }
        }
    }
    if (t.bad) return b;
    static const bool rehash = std::getenv("TOD_VKN_REHASH") != nullptr;
    if (rehash) ++tex_epoch_;
    bool stale = t.epoch != tex_epoch_;
    if (stale) t.dirty_seen = 0;
    for (; t.dirty_seen < dirty_.size(); ++t.dirty_seen)
        if (dirty_[t.dirty_seen].first < d.ea + t.size && d.ea < dirty_[t.dirty_seen].second) stale = true;
    if (stale) {
        t.epoch = tex_epoch_;
        t.dirty_seen = u32(dirty_.size());
        const u64 h = xxh64(mem::base + d.ea, t.size);
        const u32 gen = skins::texture_generation();   // a skin pack put on or off: its replacements change
        if (h != t.hash || gen != t.skin_gen) {
            t.hash = h;
            t.skin_gen = gen;
            const u64 pos = alloc(t.size, 16);
            std::memcpy(at(pos), mem::base + d.ea, t.size);
            auto* u = static_cast<TexUpload*>(packet(P_TEX, sizeof(TexUpload)));
            u->id = t.id; u->ring_off = off(pos); u->size = t.size; u->pad = 0; u->hash = h; u->desc = d;
        }
    }
    b.kind = TB_TEXTURE;
    b.id_or_ea = t.id;
    return b;
}

// ---- programs -------------------------------------------------------------------------------
bool Capture::program(u32 fea, Prog*& out) {
    const u32 fmax = std::min<u32>(0x10000, fea >= kLocalBase ? kLocalBase + kLocalSize - fea : 0x10000);
    if (fea < kLocalBase && !mem::is_mapped(fea, 16)) return false;
    Prog& p = fps_[fea];
    const u8* g = mem::base + fea;
    if (p.bytes.empty() || p.bytes.size() > fmax || std::memcmp(g, p.bytes.data(), p.bytes.size()) != 0) {
        p.info = fp_analyze(g, fmax);
        p.bytes.assign(g, g + p.info.size_bytes);
        p.key = xxh64(p.bytes.data(), p.bytes.size(), p.info.hash);
        p.fc_pos = ~0ull;
        if (fp_sent_.insert(p.key).second) {
            auto* u = static_cast<FpUpload*>(packet(P_FP, sizeof(FpUpload) + u32(p.bytes.size())));
            u->key = p.key; u->size = u32(p.bytes.size()); u->pad = 0;
            std::memcpy(u + 1, p.bytes.data(), p.bytes.size());
        }
    }
    out = &p;
    return true;
}

// ---- draw ---------------------------------------------------------------------------------------
void Capture::draw() {
    Frontend& fe = g_fe;
    if (fe.ranges.size() > 1) {   // join ranges that continue each other (draw methods cover <= 256 vertices)
        std::vector<DrawRange> merged;
        for (const DrawRange& r : fe.ranges) {
            if (!merged.empty() && merged.back().first + merged.back().count == r.first) merged.back().count += r.count;
            else merged.push_back(r);
        }
        fe.ranges.swap(merged);
    }
    const TargetDesc t = targets(false);
    if (!t.valid) return;
    passlog_note('D', t);

    ++::g_draw_no;
    // vertex program
    const u32 vstart = reg(0x1ea0);
    if (vp_gen_ != fe.vp_gen || vp_start_ != vstart) {
        vinfo_ = vp_analyze(fe.vp_ucode, vstart);
        vp_gen_ = fe.vp_gen; vp_start_ = vstart;
        if (vp_sent_.insert(vinfo_.hash).second) {
            auto* u = static_cast<VpUpload*>(packet(P_VP, sizeof(VpUpload)));
            u->hash = vinfo_.hash; u->start = vstart;
            std::memcpy(u->ucode, fe.vp_ucode, sizeof u->ucode);
        }
    }
    if (reg(0x08c0) & 0xffff) {
        const u32 n = skip_n_++;
        const u32 sf = gfx::g.skip_from.load(std::memory_order_relaxed), st = gfx::g.skip_to.load(std::memory_order_relaxed);
        if (n >= sf && n < st) {
            static u32 logged = ~0u, logging = ~0u;
            if (n == sf && logged != sf) logging = sf;
            if (logging == sf) { LOG_NOTE("skipped draw %u: vp %016llx vps %u", n, (unsigned long long)vinfo_.hash, reg(0x1ea0)); if (n + 1 == st) { logged = sf; logging = ~0u; } }
            return;
        }
    }
    if (const int sk = gfx::g.skip_vps.load(std::memory_order_relaxed); sk != 0 && u32(sk) == u32(vinfo_.hash) && (reg(0x08c0) & 0xffff)) return;
    if (const int dv = gfx::g.dump_vps.load(std::memory_order_relaxed); dv != 0 && u32(dv) == u32(vinfo_.hash)) {
        static bool done[2];
        const int side = (reg(0x08c0) & 0xffff) ? 1 : 0;
        if (!done[side]) {
            done[side] = true;
            for (u32 r = 0; r < kVpConstants; ++r) {
                const float* f = fe.vc[r];
                if (f[0] != 0 || f[1] != 0 || f[2] != 0 || f[3] != 0) LOG_NOTE("vc view %d c%u: %g %g %g %g  age %u at %08x", side, r, f[0], f[1], f[2], f[3], ::g_draw_no - ::g_vc_stamp[r], ::g_vc_ea[r]);
            }
            LOG_NOTE("vp view %d attrs: 1738 %08x vps %u inputs %08x", side, reg(0x1738), reg(0x1ea0), reg(0x1ff0));
            for (u32 a = 0; a < 16; ++a) if (reg(0x1740 + 4 * a) & 0xf0) LOG_NOTE("  attr %u fmt %08x off %08x", a, reg(0x1740 + 4 * a), reg(0x1680 + 4 * a));
            if (done[1]) { done[0] = done[1] = false; gfx::g.dump_vps = 0; }
        }
    }
    // fragment program
    const u32 sp = reg(0x8e4);
    const u32 fea = address(sp & ~3u, (sp & 3) - 1);
    Prog* fp = nullptr;
    if (fea == ~0u || !program(fea, fp)) return;

    DrawPacket d{};
    d.targets = t;
    d.vp_hash = vinfo_.hash;
    d.vp_start = vstart;
    d.fp_key = fp->key;
    for (u32 i = 0; i < 16; ++i) {
        if (fp->info.textures_used & (1u << i)) d.tex[i] = bind_texture(i);
        else d.tex[i].kind = TB_NONE;
    }

    // ---- indices and vertex range
    Prim prim = fe.prim;
    const bool immediate = fe.push_vertices > 0;
    const bool convert = prim == Prim::Quads || prim == Prim::TriangleFan || prim == Prim::Polygon || prim == Prim::LineLoop;
    u32 vmin = ~0u, vmax = 0;
    std::vector<DrawRange2> out_ranges;
    u32 index_type = 0;
    u64 index_pos = ~0ull;
    s32 vertex_offset = 0;
    bool restart = false;
    if (immediate) {
        vmin = 0; vmax = fe.push_vertices - 1;
    }
    if (fe.indexed && !immediate) {
        const u32 ia = reg(0x181c), idma = reg(0x1820);
        const u32 itype = (idma >> 4) & 0xf;   // 0: u32, 1: u16
        const u32 iea = address(ia, idma & 0xf);
        if (iea == ~0u) return;
        const u32 isz = itype == 1 ? 2 : 4;
        u32 lo = ~0u, hi = 0;
        for (auto r : fe.ranges) { lo = std::min(lo, r.first); hi = std::max(hi, r.first + r.count); }
        if (hi <= lo || !mem::is_mapped(iea + lo * isz, (hi - lo) * isz)) return;
        const bool restart_en = reg(0x1dac) & 1;
        const u32 restart_idx = reg(0x1db0);
        const u32 base_index = reg(0x173c);
        // TOD_DRAW_TRACE=1: when <data>/drawtrace appears, every indexed draw of the next frame is logged (index
        // address and count, first four vertex arrays) and the file is removed; for model format research
        static const bool dtrace = std::getenv("TOD_DRAW_TRACE") != nullptr;
        static u64 trace_frame = ~0ull, checked = ~0ull;
        if (dtrace && checked != flips_) {
            checked = flips_;
            const std::string flag = g_config.data_dir + "/drawtrace";
            if (trace_frame != flips_ && std::remove(flag.c_str()) == 0) { trace_frame = flips_ + 1; LOG_NOTE("drawtrace: frame %llu", (unsigned long long)trace_frame); }
        }
        if (dtrace && flips_ == trace_frame) {
            {
                char va[200];
                int p = 0;
                for (int i = 0; i < 4; ++i) {
                    const u32 fmtr = reg(0x1740 + 4 * i), offr = reg(0x1680 + 4 * i);
                    p += std::snprintf(va + p, sizeof va - p, " a%d %08x/%08x", i, address(offr & 0x7fffffff, offr >> 31), fmtr);
                }
                u32 bad = 0, firstbad = ~0u;
                u64 h = 1469598103934665603ull;
                for (u32 r = 0; r < kVpConstants; ++r)
                    for (int k = 0; k < 4; ++k) {
                        const float f = fe.vc[r][k];
                        if (!(std::fabs(f) < 1e7f)) { ++bad; if (firstbad == ~0u) firstbad = r; }
                        u32 bits; std::memcpy(&bits, &f, 4); h = (h ^ bits) * 1099511628211ull;
                    }
                if (gfx::g.dump_idx.load(std::memory_order_relaxed) == iea + lo * isz) {
                    static int seen[2];
                    const int side = (reg(0x08c0) & 0xffff) ? 1 : 0;
                    if (seen[side]++ == 0)
                        for (u32 r = 0; r < kVpConstants; ++r) {
                            const float* f = fe.vc[r];
                            if (f[0] != 0 || f[1] != 0 || f[2] != 0 || f[3] != 0) LOG_NOTE("vc view %d c%u: %g %g %g %g  age %u at %08x", side, r, f[0], f[1], f[2], f[3], ::g_draw_no - ::g_vc_stamp[r], ::g_vc_ea[r]);
                        }
                }
                LOG_NOTE("drawtrace: idx %08x n %u type %u base %u ranges %zu%s sc %08x vps %u vbase %08x vp %016llx bad %u at %u vc %016llx", iea + lo * isz, hi - lo, isz, base_index, fe.ranges.size(), va,
                         reg(0x08c0), reg(0x1ea0), reg(0x1738), (unsigned long long)vinfo_.hash, bad, firstbad, (unsigned long long)h);
            }
        }
        u32 imin = ~0u, imax = 0;
        const u8* src = mem::base + iea + lo * isz;
        const u32 n = hi - lo;
        if (!convert) {
            // byte-swap into the ring; restart markers become all-ones
            const bool wide = isz == 4 || (restart_en && restart_idx != 0xffff);
            index_type = wide ? 2 : 1;
            index_pos = alloc(u64(n) * (wide ? 4 : 2), 4);
            u8* dst = at(index_pos);
            for (u32 k = 0; k < n; ++k) {
                u32 v;
                if (isz == 2) { u16 x; std::memcpy(&x, src + 2 * k, 2); v = __builtin_bswap16(x); }
                else { u32 x; std::memcpy(&x, src + 4 * k, 4); v = __builtin_bswap32(x); }
                if (restart_en && v == restart_idx) v = ~0u;
                else { imin = std::min(imin, v); imax = std::max(imax, v); }
                if (wide) std::memcpy(dst + 4 * k, &v, 4);
                else { const u16 x = u16(v); std::memcpy(dst + 2 * k, &x, 2); }
            }
            restart = restart_en;
            for (auto r : fe.ranges) out_ranges.push_back({r.first - lo, r.count});
        } else {
            // quads, fans, polygons and line loops become lists / strips with u32 indices
            idx_.clear();
            std::vector<u32> seq;
            for (auto r : fe.ranges) {
                const u8* s = mem::base + iea + r.first * isz;
                u32 k0 = 0;
                while (k0 < r.count) {
                    seq.clear();
                    u32 k = k0;
                    for (; k < r.count; ++k) {
                        u32 v;
                        if (isz == 2) { u16 x; std::memcpy(&x, s + 2 * k, 2); v = __builtin_bswap16(x); }
                        else { u32 x; std::memcpy(&x, s + 4 * k, 4); v = __builtin_bswap32(x); }
                        if (restart_en && v == restart_idx) break;
                        imin = std::min(imin, v); imax = std::max(imax, v);
                        seq.push_back(v);
                    }
                    const u32 m = u32(seq.size());
                    if (prim == Prim::Quads) {
                        for (u32 q = 0; q + 3 < m; q += 4) for (u32 j : {0u, 1u, 2u, 2u, 3u, 0u}) idx_.push_back(seq[q + j]);
                    } else if (prim == Prim::LineLoop) {
                        if (m) { if (!idx_.empty()) idx_.push_back(~0u); for (u32 v : seq) idx_.push_back(v); idx_.push_back(seq[0]); }
                    } else {
                        for (u32 q = 1; q + 1 < m; ++q) { idx_.push_back(seq[0]); idx_.push_back(seq[q]); idx_.push_back(seq[q + 1]); }
                    }
                    k0 = k + 1;
                }
            }
            if (idx_.empty()) return;
            index_type = 2;
            index_pos = alloc(4ull * idx_.size(), 4);
            std::memcpy(at(index_pos), idx_.data(), 4ull * idx_.size());
            out_ranges.push_back({0, u32(idx_.size())});
            restart = prim == Prim::LineLoop;
            prim = prim == Prim::LineLoop ? Prim::LineStrip : Prim::Triangles;
        }
        if (imin == ~0u) return;
        vertex_offset = s32(base_index);
        vmin = imin + base_index; vmax = imax + base_index;
    } else if (!immediate) {
        for (auto r : fe.ranges) if (r.count) { vmin = std::min(vmin, r.first); vmax = std::max(vmax, r.first + r.count - 1); }
        if (vmin == ~0u) return;
        if (convert) {
            idx_.clear();
            for (auto r : fe.ranges) {
                const u32 m = r.count;
                if (prim == Prim::Quads) {
                    for (u32 q = 0; q + 3 < m; q += 4) for (u32 j : {0u, 1u, 2u, 2u, 3u, 0u}) idx_.push_back(r.first + q + j);
                } else if (prim == Prim::LineLoop) {
                    if (m) { if (!idx_.empty()) idx_.push_back(~0u); for (u32 q = 0; q < m; ++q) idx_.push_back(r.first + q); idx_.push_back(r.first); }
                } else {
                    for (u32 q = 1; q + 1 < m; ++q) { idx_.push_back(r.first); idx_.push_back(r.first + q); idx_.push_back(r.first + q + 1); }
                }
            }
            if (idx_.empty()) return;
            index_type = 2;
            index_pos = alloc(4ull * idx_.size(), 4);
            std::memcpy(at(index_pos), idx_.data(), 4ull * idx_.size());
            out_ranges.push_back({0, u32(idx_.size())});
            restart = prim == Prim::LineLoop;
            prim = prim == Prim::LineLoop ? Prim::LineStrip : Prim::Triangles;
        } else {
            for (auto r : fe.ranges) out_ranges.push_back({r.first, prim == Prim::QuadStrip ? r.count & ~1u : r.count});
        }
    } else {
        // immediate mode: quads etc. as generated indices over 0..n-1
        const u32 m = fe.push_vertices;
        if (convert) {
            idx_.clear();
            if (prim == Prim::Quads) { for (u32 q = 0; q + 3 < m; q += 4) for (u32 j : {0u, 1u, 2u, 2u, 3u, 0u}) idx_.push_back(q + j); }
            else if (prim == Prim::LineLoop) { for (u32 q = 0; q < m; ++q) idx_.push_back(q); idx_.push_back(0); }
            else { for (u32 q = 1; q + 1 < m; ++q) { idx_.push_back(0); idx_.push_back(q); idx_.push_back(q + 1); } }
            if (idx_.empty()) return;
            index_type = 2;
            index_pos = alloc(4ull * idx_.size(), 4);
            std::memcpy(at(index_pos), idx_.data(), 4ull * idx_.size());
            out_ranges.push_back({0, u32(idx_.size())});
            prim = prim == Prim::LineLoop ? Prim::LineStrip : Prim::Triangles;
        } else {
            out_ranges.push_back({0, prim == Prim::QuadStrip ? m & ~1u : m});
        }
    }
    if (vmax < vmin || vmax - vmin > 4 * 1024 * 1024) return;

    // ---- vertex arrays: copy the bytes each used attribute reads, merging overlapping ranges
    VertexLayout L{};
    struct Span { u32 lo, hi; };
    Span spans[16], want[16];
    u32 nspans = 0;
    int span_of[16];
    const u32 base_offset = reg(0x1738), freq_ops = reg(0x1fc0);
    u32 attr_ea[16] = {};
    for (u32 i = 0; i < 16; ++i) {
        span_of[i] = -1;
        std::memcpy(L.cval[i], fe.attr_value[i], 16);
        L.attr[i][0] = i; L.attr[i][2] = 0;
        if (immediate) {
            if ((fe.push_mask & (1u << i)) && fe.push[i].size() >= 4ull * fe.push_vertices) {
                L.attr[i][1] = 16; L.attr[i][2] = 2 | (4 << 4) | 0x100;
                span_of[i] = 100;  // copied below
            }
            continue;
        }
        const u32 f = reg(0x1740 + 4 * i), o = reg(0x1680 + 4 * i);
        const u32 type = f & 0xf, size = (f >> 4) & 0xf, stride = (f >> 8) & 0xff, divider = f >> 16;
        if (!type || !size) continue;
        const u32 ea = address((o & 0x7fffffff) + base_offset, o >> 31);
        if (ea == ~0u) continue;
        const u32 eb = vertex_type_bytes(type, size);
        if (!eb) continue;
        const bool modulo = (freq_ops >> i) & 1;
        u32 v0 = vmin, v1 = vmax;
        if (divider > 1) { if (modulo) { v0 = 0; v1 = std::min(vmax, divider - 1); } else { v0 = vmin / divider; v1 = vmax / divider; } }
        if (!stride) v0 = v1 = 0;
        const u64 lo = u64(ea) + u64(v0) * stride, hi = u64(ea) + u64(v1) * stride + eb;
        if (hi > (1ull << 32) || hi - lo > 64ull * 1024 * 1024) continue;
        L.attr[i][1] = stride;
        L.attr[i][2] = type | (size << 4);
        L.attr[i][3] = (divider > 1 ? divider & 0xffff : 0) | (modulo ? 0x10000 : 0);
        attr_ea[i] = ea;
        want[i] = {u32(lo), u32(hi)};
        span_of[i] = 0;
    }
    {   // merge the attribute ranges that overlap or nearly touch (interleaved arrays) into spans
        u32 order[16], n = 0;
        for (u32 i = 0; i < 16; ++i) if (span_of[i] == 0) order[n++] = i;
        std::sort(order, order + n, [&](u32 a, u32 b) { return want[a].lo < want[b].lo; });
        for (u32 k = 0; k < n; ++k) {
            const Span& w = want[order[k]];
            if (nspans && u64(w.lo) <= u64(spans[nspans - 1].hi) + 256) spans[nspans - 1].hi = std::max(spans[nspans - 1].hi, w.hi);
            else spans[nspans++] = w;
            span_of[order[k]] = int(nspans - 1);
        }
    }
    u64 total = 0;
    u32 span_off[16];
    for (u32 k = 0; k < nspans; ++k) {
        if (!mem::is_mapped(spans[k].lo, spans[k].hi - spans[k].lo)) return;
        span_off[k] = u32(total);
        total = align_up(total + (spans[k].hi - spans[k].lo), 16);
    }
    u32 push_off = 0;
    if (immediate) {
        push_off = u32(total);
        for (u32 i = 0; i < 16; ++i) if (span_of[i] == 100) total += 16ull * fe.push_vertices;
    }
    total += 16;  // the shader may read one word past an attribute's last byte
    if (total > ring_.size / 8) return;
    const u64 vpos = alloc(total, ring_.align);
    u8* vdst = at(vpos);
    for (u32 k = 0; k < nspans; ++k) std::memcpy(vdst + span_off[k], mem::base + spans[k].lo, spans[k].hi - spans[k].lo);
    std::memset(vdst + total - 16, 0, 16);
    for (u32 i = 0; i < 16; ++i) {
        if (span_of[i] < 0) continue;
        if (span_of[i] == 100) {
            L.attr[i][0] = push_off;
            std::memcpy(vdst + push_off, fe.push[i].data(), 16ull * fe.push_vertices);
            push_off += 16 * fe.push_vertices;
            continue;
        }
        const Span& s = spans[span_of[i]];
        L.attr[i][0] = span_off[span_of[i]] + (attr_ea[i] - s.lo);   // byte of vertex 0, modulo 2^32
    }
    static const bool check = std::getenv("TOD_VKN_CHECK") != nullptr;
    if (check && !immediate) {
        // the bytes the shader will read for a few vertices must equal guest memory
        static u32 reported = 0;
        for (u32 i = 0; i < 16 && reported < 40; ++i) {
            if (span_of[i] < 0 || span_of[i] == 100) continue;
            const u32 stride = L.attr[i][1], div = L.attr[i][3] & 0xffff, eb = vertex_type_bytes(L.attr[i][2] & 15, (L.attr[i][2] >> 4) & 15);
            for (u32 v : {vmin, vmax, vmin + (vmax - vmin) / 2}) {
                u32 vv = v;
                if (div > 1) vv = (L.attr[i][3] & 0x10000) ? vv % div : vv / div;
                const u32 a = L.attr[i][0] + vv * stride;
                if (u64(a) + eb > total || std::memcmp(vdst + a, mem::base + attr_ea[i] + vv * stride, eb)) {
                    LOG_ERR("vkn check: vp %016llx attr %u vertex %u (range %u-%u) stride %u type %u: shader offset 0x%x of 0x%llx differs",
                            (unsigned long long)vinfo_.hash, i, v, vmin, vmax, stride, L.attr[i][2] & 15, a, (unsigned long long)total);
                    ++reported;
                    break;
                }
            }
        }
    }
    d.vdata_off = off(vpos);
    d.vdata_size = u32(total);
    const u64 lpos = alloc(sizeof L, ring_.align);
    std::memcpy(at(lpos), &L, sizeof L);
    d.layout_off = off(lpos);

    // ---- constants
    static const bool nocache = std::getenv("TOD_VKN_NOCACHE") != nullptr;
    if (nocache) { vc_gen_ = 0; fp->fc_pos = ~0ull; }
    if (vc_gen_ != fe.vc_gen || !intact(vc_pos_)) {
        vc_pos_ = alloc(sizeof fe.vc, ring_.align);
        std::memcpy(at(vc_pos_), fe.vc, sizeof fe.vc);
        vc_gen_ = fe.vc_gen;
    }
    d.vc_off = off(vc_pos_);
    const u32 nfc = std::max<u32>(1, u32(fp->info.const_offsets.size()));
    if (!intact(fp->fc_pos)) {
        fp->fc_pos = alloc(16ull * nfc, ring_.align);
        float* fc = reinterpret_cast<float*>(at(fp->fc_pos));
        std::memset(fc, 0, 16ull * nfc);
        for (u32 i = 0; i < fp->info.const_offsets.size() && i < kMaxFpConsts; ++i)
            fp_read_constant(fp->bytes.data(), fp->info.const_offsets[i], fc + 4 * i);
    }
    d.fc_off = off(fp->fc_pos);
    d.fc_size = 16 * nfc;

    d.prim = u32(prim);
    d.flags = (index_type ? DF_INDEXED : 0) | (immediate ? DF_IMMEDIATE : 0);
    d.index_off = index_pos == ~0ull ? 0 : off(index_pos);
    d.index_type = index_type;
    d.vertex_offset = vertex_offset;
    d.restart = restart;
    d.nranges = u32(out_ranges.size());

    send_regs();
    auto* p = static_cast<u8*>(packet(P_DRAW, u32(sizeof d + 8 * out_ranges.size())));
    std::memcpy(p, &d, sizeof d);
    std::memcpy(p + sizeof d, out_ranges.data(), 8 * out_ranges.size());
    if (gfx::g.split_views.load(std::memory_order_relaxed) && t.ncolor == 1) {
        const u32 row8 = t.color_ea[0] + 8 * t.color_pitch[0];
        auto sc = d.tex[0].kind == TB_COLOR_SURFACE ? surfs_.find(u64(d.tex[0].id_or_ea) << 1) : surfs_.end();
        if (t.clip_y == 8 && t.clip_w >= 1280 && sc != surfs_.end() && sc->second.w >= 960 && sc->second.h >= 700 &&
            d.tex[0].id_or_ea != t.color_ea[0] && vc_count_ < 4) {
            // this draw copied a view's scene: keep the copy
            BlitParams bp{};
            bp.src_ea = row8; bp.src_pitch = t.color_pitch[0]; bp.src_w = t.clip_w; bp.src_h = t.clip_h; bp.src_bpp = 4;
            bp.sx = 0; bp.sy = 0; bp.sw = float(t.clip_w); bp.sh = float(t.clip_h);
            bp.dst_ea = kViewCopyBase + vc_count_ * kViewCopyStride; bp.dst_pitch = t.color_pitch[0]; bp.dst_bpp = 4; bp.dst_fmt = 0xa;
            bp.dx = 0; bp.dy = 0; bp.dw = t.clip_w; bp.dh = t.clip_h;
            if (t.color_fmt == 8 && blit(bp)) { vc_ea_ = row8; vc_dst_ = t.color_ea[0]; ++vc_count_; }
            else vc_miss_ |= 16;
            if (vc_count_ == 1 && surfs_.count(u64(kViewCopyBase + kViewCopyStride) << 1)) {
                // the right half still holds last frame's finished picture; if the second view skips
                // its scene this frame its post effects would pile up on that (the "glow" that keeps
                // frames). Start it from the second view's last scene instead.
                const u32 hw = t.clip_w / 2;
                BlitParams r{};
                r.src_ea = kViewCopyBase + kViewCopyStride; r.src_pitch = t.color_pitch[0]; r.src_w = t.clip_w; r.src_h = t.clip_h; r.src_bpp = 4;
                r.sx = 0; r.sy = 0; r.sw = float(hw); r.sh = float(t.clip_h);
                r.dst_ea = t.color_ea[0]; r.dst_pitch = t.color_pitch[0]; r.dst_bpp = 4; r.dst_fmt = 0xa;
                r.dx = hw; r.dy = 8; r.dw = hw; r.dh = t.clip_h;
                blit(r);
            }
            if (vc_count_ == 2) {
                // both views copied their scene to the left half: put each in its own half
                const u32 hw = t.clip_w / 2;
                for (u32 k = 0; k < 2; ++k) {
                    BlitParams r{};
                    r.src_ea = kViewCopyBase + k * kViewCopyStride; r.src_pitch = t.color_pitch[0]; r.src_w = t.clip_w; r.src_h = t.clip_h; r.src_bpp = 4;
                    r.sx = 0; r.sy = 0; r.sw = float(hw); r.sh = float(t.clip_h);
                    r.dst_ea = t.color_ea[0]; r.dst_pitch = t.color_pitch[0]; r.dst_bpp = 4; r.dst_fmt = 0xa;
                    r.dx = k * hw; r.dy = 8; r.dw = hw; r.dh = t.clip_h;
                    blit(r);
                }
                // the 8 rows above the views keep what other passes drew there (a strip of picture
                // along the top): black, as with one view
                auto setr = [](u32 off, u32 v) { g_regs[off >> 2] = v; g_reg_dirty[(off >> 2) >> 6] |= 1ull << ((off >> 2) & 63); };
                const u32 keep[4] = {reg(0x200), reg(0x204), reg(0x8c0), reg(0x8c4)}, cc = reg(0x1d90);
                setr(0x200, t.clip_x | (t.clip_w << 16)); setr(0x204, 0u | (8u << 16));
                setr(0x8c0, t.clip_x | (t.clip_w << 16)); setr(0x8c4, 0u | (8u << 16));
                setr(0x1d90, 0xff000000u);
                clear(0xf0);
                setr(0x200, keep[0]); setr(0x204, keep[1]); setr(0x8c0, keep[2]); setr(0x8c4, keep[3]); setr(0x1d90, cc);
            }
        } else if (t.clip_y == 8 && t.clip_w >= 1280) {
            vc_miss_ |= d.tex[0].kind != TB_COLOR_SURFACE ? 1 : sc == surfs_.end() ? 2 : (sc->second.w < 960 || sc->second.h < 700) ? 4 :
                        d.tex[0].id_or_ea == t.color_ea[0] ? 8 : 32;
        }
    }
    if (batch_->bytes.size() > kBatchBytes) flush_batch();
}

// ---- clear / blit / flip ---------------------------------------------------------------------
void Capture::clear(u32 mask) {
    ClearPacket c{};
    c.targets = targets(true);
    if (!c.targets.valid) return;
    passlog_note('C', c.targets);
    c.mask = mask;
    send_regs();
    std::memcpy(packet(P_CLEAR, sizeof c), &c, sizeof c);
}

bool Capture::blit(const BlitParams& b) {
    // the source must lie inside a color surface (legacy behaviour: any surface containing it)
    u64 src_key = 0;
    Surf* src = containing(b.src_ea, 0, false, &src_key);
    if (!src) return false;
    if (!surface_current(*src)) { drop_surface(src_key); return false; }
    src->used_frame = flips_;
    if (b.dst_fmt != 0xa && b.dst_fmt != 4) return false;
    note_surface(b.dst_ea, false, b.dst_fmt == 4 ? 3 : 8, b.dx + b.dw, b.dy + b.dh, b.dst_pitch);
    std::memcpy(packet(P_BLIT, sizeof b), &b, sizeof b);
    return true;
}

void Capture::flip(u32 buffer) {
    FlipPacket f{};
    f.buffer = buffer;
    f.db = display_buffer(buffer);
    f.mem_off = ~0u;
    const u64 key = u64(kLocalBase + f.db.offset) << 1;
    auto it = surfs_.find(key);
    if ((it == surfs_.end() || !surface_current(it->second)) && f.db.width && f.db.height && f.db.pitch) {
        if (it != surfs_.end()) drop_surface(key);
        // the CPU drew this frame (movies): send guest memory as RGBA8
        const u64 pos = alloc(4ull * f.db.width * f.db.height, 16);
        u8* out = at(pos);
        for (u32 y = 0; y < f.db.height; ++y) {
            const u8* row = mem::base + kLocalBase + f.db.offset + y * f.db.pitch;
            u8* o = out + 4ull * f.db.width * y;
            for (u32 x = 0; x < f.db.width; ++x) { o[4 * x] = row[4 * x + 1]; o[4 * x + 1] = row[4 * x + 2]; o[4 * x + 2] = row[4 * x + 3]; o[4 * x + 3] = 255; }
        }
        f.mem_off = off(pos);
    }
    skip_n_ = 0;
    passlog_note('F', TargetDesc{});
    f.ring_pos = ring_.head;
    keep_from_ = ring_.head;
    std::memcpy(packet(P_FLIP, sizeof f), &f, sizeof f);
    flush_batch();
    ++flips_;
    ++tex_epoch_;
    dirty_.clear();
    if (gfx::g.split_views.load(std::memory_order_relaxed)) {
        // a frame where the views didn't both copy their scene leaves the other half as it was, and its
        // post effects pile up on it: counted, and logged every 600 frames when it happened
        // (by count: 0, 1, 3 or more), with what kept other full-width draws from counting as copies
        static u32 n0 = 0, n1 = 0, n3 = 0, miss = 0, reads = 0;
        if (vc_count_ == 0) ++n0; else if (vc_count_ == 1) ++n1; else if (vc_count_ > 2) ++n3;
        if (vc_count_ != 2) miss |= vc_miss_;
        if (vc_count_ == 2 && vc_read_ != 2) ++reads;
        static const bool vtrace = std::getenv("TOD_SPLIT_TRACE") != nullptr;
        if (vtrace) LOG_NOTE("vkn: frame %llu: %u scene copies, read %u times (missed %x)", (unsigned long long)flips_, vc_count_, vc_read_, vc_miss_);
        if (flips_ % 600 == 0) {
            if (n0 + n1 + n3 + reads)
                LOG_NOTE("vkn: split screen: of the last 600 frames %u had no scene copy, %u one, %u three or more (missed: %x); %u read the copies other than twice",
                         n0, n1, n3, miss, reads);
            n0 = n1 = n3 = miss = reads = 0;
        }
    }
    vc_count_ = vc_read_ = vc_miss_ = 0; vc_ea_ = vc_dst_ = 0;
    evict_surfaces();
    // forget textures not used for a while
    if (flips_ % 120 == 0) {
        for (auto it2 = texs_.begin(); it2 != texs_.end();) {
            if (flips_ - it2->second.last_frame > 600) {
                *static_cast<u32*>(packet(P_TEX_FREE, 4)) = it2->second.id;
                it2 = texs_.erase(it2);
            } else ++it2;
        }
    }
    // don't run more than a couple of frames ahead of the GPU
    if (!sync_) {
        std::unique_lock lk(sh_.frames.lock);
        sh_.frames.cv.wait(lk, [&] { return flips_ - sh_.frames.done.load() <= kMaxFramesAhead; });
    }
}

}  // namespace

}  // namespace vkn

Renderer* create_native_renderer(const VkPresentTarget* target) {
    static vkn::Shared shared;
    vkn::RenderThread* rt = vkn::create_render_thread(target, shared);
    if (!rt) return nullptr;
    const bool sync = std::getenv("TOD_VKN_SYNC") != nullptr;
    if (!sync) vkn::start_render_thread(rt);
    LOG_NOTE("vkn: native renderer%s", sync ? " (synchronous)" : "");
    return new vkn::Capture(shared, rt, sync);
}

}  // namespace rsx
