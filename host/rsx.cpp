#include <array>
// RSX command processor: FIFO parsing, method register file and CPU-visible side effects.
#include "rsx.h"
#include "rsx_state.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>

unsigned g_vc_stamp[468], g_vc_ea[468], g_draw_no, g_cmd_ea;  // debug: the draw that last wrote each vertex constant
namespace rsx {
std::function<void(u32)> on_flip;
Frontend g_fe;
u32 g_regs[0x10000 / 4];
u64 g_reg_dirty[0x4000 / 64];
namespace { Renderer g_null_renderer; Renderer* g_renderer = &g_null_renderer; }
void set_renderer(Renderer* r) { g_renderer = r ? r : &g_null_renderer; }

namespace {
// ---- method ids (register index = byte offset / 4) -------------------------------------
enum : u32 {
    NV406E_SET_REFERENCE = 0x0050 >> 2,
    NV406E_SET_CONTEXT_DMA_SEMAPHORE = 0x0060 >> 2,
    NV406E_SEMAPHORE_OFFSET = 0x0064 >> 2,
    NV406E_SEMAPHORE_ACQUIRE = 0x0068 >> 2,
    NV406E_SEMAPHORE_RELEASE = 0x006c >> 2,
    NV4097_SET_CONTEXT_DMA_SEMAPHORE = 0x01a4 >> 2,
    NV4097_SET_CONTEXT_DMA_REPORT = 0x01a8 >> 2,
    NV4097_GET_REPORT = 0x1800 >> 2,
    NV4097_SET_BEGIN_END = 0x1808 >> 2,
    NV4097_SET_SEMAPHORE_OFFSET = 0x1d6c >> 2,
    NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1d70 >> 2,
    NV4097_TEXTURE_READ_SEMAPHORE_RELEASE = 0x1d74 >> 2,
    NV4097_CLEAR_SURFACE = 0x1d94 >> 2,
    NV0039_SET_CONTEXT_DMA_BUFFER_IN = 0x2184 >> 2,
    NV0039_SET_CONTEXT_DMA_BUFFER_OUT = 0x2188 >> 2,
    NV0039_OFFSET_IN = 0x230C >> 2,
    NV0039_OFFSET_OUT = 0x2310 >> 2,
    NV0039_PITCH_IN = 0x2314 >> 2,
    NV0039_PITCH_OUT = 0x2318 >> 2,
    NV0039_LINE_LENGTH_IN = 0x231C >> 2,
    NV0039_LINE_COUNT = 0x2320 >> 2,
    NV0039_FORMAT = 0x2324 >> 2,
    NV0039_BUFFER_NOTIFY = 0x2328 >> 2,
    GCM_SET_USER_COMMAND = 0xEB00 >> 2,
    GCM_FLIP_COMMAND = 0xFEAC >> 2,
};
enum : u32 {
    CELL_GCM_LOCATION_LOCAL = 0, CELL_GCM_LOCATION_MAIN = 1,
    DMA_FRAME_BUFFER = 0xFEED0000, DMA_HOST_BUFFER = 0xFEED0001,
    DMA_REPORT_LOCAL = 0x66626660, DMA_REPORT_MAIN = 0xBAD68000,
    DMA_SEMAPHORE_RW = 0x66606660, DMA_SEMAPHORE_R = 0x66616661,
    DMA_DEVICE_RW = 0x56616660, DMA_DEVICE_R = 0x56616661,
};

// ---- state -------------------------------------------------------------------------------
std::once_flag g_init_once;
std::mutex g_io_lock;
u16 g_io_of_ea[4096], g_ea_of_io[4096];  // 1 MiB pages; 0xFFFF = unmapped

std::atomic<u32> g_flip_mode{2}, g_current_buffer{0};
std::atomic<u64> g_flips{0}, g_vblanks{0};
std::atomic<u32> g_vblank_handler{0}, g_flip_handler{0}, g_user_handler{0};
std::mutex g_disp_lock;
DisplayBuffer g_display[8];

// interrupt thread queue
std::mutex g_intr_lock;
std::condition_variable g_intr_cv;
std::vector<std::pair<u32, u32>> g_intr_queue;  // (handler opd, arg)

FILE* g_trace = [] { const char* p = std::getenv("TOD_RSX_TRACE"); return p ? std::fopen(p, "w") : nullptr; }();

std::map<u32, u64> g_method_hist;  // RSX thread only
u64 g_commands = 0, g_draws = 0;

inline u32 ld_be_atomic(u32 ea) { return __builtin_bswap32(__atomic_load_n(reinterpret_cast<u32*>(mem::base + ea), __ATOMIC_ACQUIRE)); }
inline void st_be_atomic(u32 ea, u32 v) { __atomic_store_n(reinterpret_cast<u32*>(mem::base + ea), __builtin_bswap32(v), __ATOMIC_RELEASE); }

u64 timestamp_ns() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<nanoseconds>(steady_clock::now() - t0).count();
}

void queue_interrupt(u32 opd, u32 arg) {
    if (!opd) return;
    {
        std::lock_guard lk(g_intr_lock);
        g_intr_queue.emplace_back(opd, arg);
    }
    g_intr_cv.notify_one();
}

// ---- method side effects -----------------------------------------------------------------
u32 sema_addr_406e() { return address(g_regs[NV406E_SEMAPHORE_OFFSET], g_regs[NV406E_SET_CONTEXT_DMA_SEMAPHORE]); }
u32 sema_addr_4097() { return address(g_regs[NV4097_SET_SEMAPHORE_OFFSET], g_regs[NV4097_SET_CONTEXT_DMA_SEMAPHORE]); }

void semaphore_acquire(u32 value) {
    const u32 addr = sema_addr_406e();
    if (addr == ~0u) { LOG_ERR("rsx: semaphore acquire at bad address (offset 0x%x)", g_regs[NV406E_SEMAPHORE_OFFSET]); return; }
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    for (unsigned spins = 0; ld_be_atomic(addr) != value; ++spins) {
        if (spins < 1000) { std::this_thread::yield(); continue; }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            LOG_ERR("rsx: semaphore acquire waiting >5s at 0x%x for 0x%x (now 0x%x)", addr, value, ld_be_atomic(addr));
            warned = true;
        }
    }
}

void write_label(u32 addr, u32 value) {
    if (addr == ~0u) { LOG_ERR("rsx: label write to bad address"); return; }
    st_be_atomic(addr, value);
}

void get_report(u32 arg) {
    const u32 type = arg >> 24, offset = arg & 0xffffff;
    const u32 addr = address(offset, g_regs[NV4097_SET_CONTEXT_DMA_REPORT]);
    if (addr == ~0u) { LOG_ERR("rsx: GET_REPORT bad offset 0x%x", offset); return; }
    // CellGcmReportData {u64 timer; u32 value; u32 padding}. No occlusion data yet: report
    // "everything visible" for ZPASS so visibility queries never cull.
    const u64 t = timestamp_ns();
    st32(mem::base, addr + 8, type == 1 ? 0xFFFFu : 0u);
    st32(mem::base, addr + 12, 0);
    st32(mem::base, addr + 4, u32(t));
    st_be_atomic(addr, u32(t >> 32));
}

void buffer_notify() {
    const u32 in = address(g_regs[NV0039_OFFSET_IN], g_regs[NV0039_SET_CONTEXT_DMA_BUFFER_IN]);
    const u32 out = address(g_regs[NV0039_OFFSET_OUT], g_regs[NV0039_SET_CONTEXT_DMA_BUFFER_OUT]);
    const s32 pin = s32(g_regs[NV0039_PITCH_IN]), pout = s32(g_regs[NV0039_PITCH_OUT]);
    const u32 len = g_regs[NV0039_LINE_LENGTH_IN], lines = g_regs[NV0039_LINE_COUNT];
    const u32 fmt = g_regs[NV0039_FORMAT], in_step = fmt & 0xff, out_step = (fmt >> 8) & 0xff;
    if (in == ~0u || out == ~0u) { LOG_ERR("rsx: NV0039 copy with bad address"); return; }
    if (in_step <= 1 && out_step <= 1) {
        for (u32 y = 0; y < lines; ++y) std::memmove(mem::base + out + y * pout, mem::base + in + y * pin, len);
    } else {
        for (u32 y = 0; y < lines; ++y)
            for (u32 x = 0; x < len; ++x) mem::base[out + y * pout + x * out_step] = mem::base[in + y * pin + x * in_step];
    }
    g_renderer->invalidate(out, lines * u32(std::abs(pout)));
}

void flip(u32 id) {
    if (g_trace) std::fflush(g_trace);
    g_current_buffer = id;
    ++g_flips;
    g_renderer->flip(id);
    if (on_flip) on_flip(id);
    // Flip done: release the flip semaphore (label 1) so SetWaitFlip passes.
    std::memset(mem::base + kLabelAddr + 0x10, 0, 16);
    queue_interrupt(g_flip_handler.load(), 1);
    {
        // Every 10 s: frame rate, slowest frame and draws per frame, so a log shows the speed.
        using clk = std::chrono::steady_clock;
        static clk::time_point t0 = clk::now(), last = t0;
        static u64 flips0 = 0, draws0 = 0;
        static double worst = 0;
        const auto now = clk::now();
        worst = std::max(worst, std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        const double el = std::chrono::duration<double>(now - t0).count();
        if (el >= 10) {
            const u64 n = g_flips - flips0;
            LOG_NOTE("perf: %.1f fps, slowest frame %.0f ms, %llu draws per frame", n / el, worst,
                     (unsigned long long)(n ? (g_draws - draws0) / n : 0));
            t0 = now; flips0 = g_flips; draws0 = g_draws; worst = 0;
        }
    }
    if (g_log_level >= 1 && (g_flips <= 3 || g_flips % 600 == 0))
        LOG_NOTE("rsx: flip #%llu buffer %u (%llu commands, %llu draws so far)", (unsigned long long)g_flips.load(), id,
                 (unsigned long long)g_commands, (unsigned long long)g_draws);
}


// ---- graphics frontend ----------------------------------------------------------------
enum : u32 {
    NV4097_SET_TRANSFORM_PROGRAM_LOAD = 0x1e9c >> 2,
    NV4097_SET_TRANSFORM_PROGRAM = 0x0b80 >> 2,
    NV4097_SET_TRANSFORM_CONSTANT_LOAD = 0x1efc >> 2,
    NV4097_SET_TRANSFORM_CONSTANT = 0x1f00 >> 2,
    NV4097_DRAW_ARRAYS = 0x1814 >> 2,
    NV4097_INLINE_ARRAY = 0x1818 >> 2,
    NV4097_DRAW_INDEX_ARRAY = 0x1824 >> 2,
    NV4097_SET_VERTEX_DATA_SCALED4S_M = 0x0a80 >> 2,
    NV4097_SET_VERTEX_DATA3F_M = 0x1500 >> 2,
    NV4097_SET_VERTEX_DATA2F_M = 0x1880 >> 2,
    NV4097_SET_VERTEX_DATA2S_M = 0x1900 >> 2,
    NV4097_SET_VERTEX_DATA4UB_M = 0x1940 >> 2,
    NV4097_SET_VERTEX_DATA4S_M = 0x1980 >> 2,
    NV4097_SET_VERTEX_DATA4F_M = 0x1c00 >> 2,
    NV4097_SET_VERTEX_DATA1F_M = 0x1e40 >> 2,
    NV4097_SET_SHADER_PROGRAM = 0x08e4 >> 2,
    NV3062_SET_COLOR_FORMAT = 0x6300 >> 2,
    NV3062_SET_PITCH = 0x6304 >> 2,
    NV3062_SET_OFFSET_DESTIN = 0x630c >> 2,
    NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN = 0x6188 >> 2,
    NV308A_POINT = 0xa304 >> 2,
    NV308A_SIZE_OUT = 0xa308 >> 2,
    NV308A_COLOR = 0xa400 >> 2,
    NV3089_SET_CONTEXT_DMA_IMAGE = 0xc184 >> 2,
    NV3089_SET_COLOR_FORMAT = 0xc300 >> 2,
    NV3089_CLIP_POINT = 0xc308 >> 2,
    NV3089_CLIP_SIZE = 0xc30c >> 2,
    NV3089_IMAGE_OUT_POINT = 0xc310 >> 2,
    NV3089_IMAGE_OUT_SIZE = 0xc314 >> 2,
    NV3089_DS_DX = 0xc318 >> 2,
    NV3089_DT_DY = 0xc31c >> 2,
    NV3089_IMAGE_IN_SIZE = 0xc400 >> 2,
    NV3089_IMAGE_IN_FORMAT = 0xc404 >> 2,
    NV3089_IMAGE_IN_OFFSET = 0xc408 >> 2,
    NV3089_IMAGE_IN = 0xc40c >> 2,
};

void warn_once(u32 reg, const char* what) {
    static std::map<u32, bool> seen;
    if (!seen[reg]) { seen[reg] = true; LOG_ERR("rsx: %s (method 0x%04x) not handled yet", what, reg << 2); }
}

// Immediate-mode attribute write: component `comp` of attribute `attr` as a float.
// Attribute 0 completes a vertex when its last component is written (glVertex semantics).
void push_attr(u32 attr, u32 comp, u32 ncomp, float v) {
    auto& fe = g_fe;
    float* cur = fe.attr_value[attr];
    if (comp == 0) { cur[1] = 0.f; cur[2] = 0.f; cur[3] = 1.f; }
    cur[comp] = v;
    if (fe.prim == Prim::None) return;
    if (comp + 1 != ncomp) return;
    for (u32 i = ncomp; i < 4; ++i) cur[i] = i == 3 ? 1.f : 0.f;
    auto& buf = fe.push[attr];
    const u32 slot = fe.push_vertices;
    if (buf.size() < 4 * (slot + 1)) buf.resize(4 * (slot + 1));
    std::memcpy(&buf[4 * slot], cur, 16);
    fe.push_mask |= u16(1u << attr);
    if (attr == 0) ++fe.push_vertices;
}

inline float f32_of(u32 bits) { float f; std::memcpy(&f, &bits, 4); return f; }

bool immediate_vertex_data(u32 reg, u32 arg) {
    auto in = [&](u32 base, u32 words_per_attr) { return reg >= base && reg < base + 16 * words_per_attr; };
    if (in(NV4097_SET_VERTEX_DATA4F_M, 4)) { const u32 i = reg - NV4097_SET_VERTEX_DATA4F_M; push_attr(i / 4, i % 4, 4, f32_of(arg)); return true; }
    if (in(NV4097_SET_VERTEX_DATA3F_M, 3)) { const u32 i = reg - NV4097_SET_VERTEX_DATA3F_M; push_attr(i / 3, i % 3, 3, f32_of(arg)); return true; }
    if (in(NV4097_SET_VERTEX_DATA2F_M, 2)) { const u32 i = reg - NV4097_SET_VERTEX_DATA2F_M; push_attr(i / 2, i % 2, 2, f32_of(arg)); return true; }
    if (in(NV4097_SET_VERTEX_DATA1F_M, 1)) { push_attr(reg - NV4097_SET_VERTEX_DATA1F_M, 0, 1, f32_of(arg)); return true; }
    if (in(NV4097_SET_VERTEX_DATA4UB_M, 1)) {
        const u32 a = reg - NV4097_SET_VERTEX_DATA4UB_M;
        for (u32 c = 0; c < 4; ++c) push_attr(a, c, 4, float((arg >> (8 * c)) & 0xff) / 255.f);
        return true;
    }
    if (in(NV4097_SET_VERTEX_DATA2S_M, 1)) {
        const u32 a = reg - NV4097_SET_VERTEX_DATA2S_M;
        push_attr(a, 0, 2, float(s16(arg & 0xffff)));
        push_attr(a, 1, 2, float(s16(arg >> 16)));
        return true;
    }
    if (in(NV4097_SET_VERTEX_DATA4S_M, 2) || in(NV4097_SET_VERTEX_DATA_SCALED4S_M, 2)) {
        const u32 base = in(NV4097_SET_VERTEX_DATA4S_M, 2) ? NV4097_SET_VERTEX_DATA4S_M : NV4097_SET_VERTEX_DATA_SCALED4S_M;
        const u32 i = reg - base, a = i / 2, c = (i % 2) * 2;
        push_attr(a, c, 4, float(s16(arg & 0xffff)));
        push_attr(a, c + 1, 4, float(s16(arg >> 16)));
        return true;
    }
    return false;
}

FILE* open_dump(const char* kind, u64 hash, const char* ext) {
    static const char* dir = std::getenv("TOD_DUMP_SHADERS");
    if (!dir) return nullptr;
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s/%016llx.%s", dir, kind, (unsigned long long)hash, ext);
    if (FILE* f = std::fopen(path, "rb")) { std::fclose(f); return nullptr; }
    return std::fopen(path, "wb");
}

u64 fnv(const void* p, size_t n, u64 h = 0xcbf29ce484222325ull) {
    const u8* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// TOD_DUMP_SHADERS=dir: write each distinct vertex/fragment program used by a draw to
// dir/vp and dir/fp (test corpus for the shader translators).
void dump_programs() {
    static const bool on = std::getenv("TOD_DUMP_SHADERS") != nullptr;
    if (!on) return;
    const u32 start = g_regs[0x1ea0 >> 2];
    const u64 vh = fnv(g_fe.vp_ucode, sizeof g_fe.vp_ucode, fnv(&start, 4));
    if (FILE* f = open_dump("vp", vh, "bin")) {
        std::fwrite(&start, 4, 1, f);
        std::fwrite(g_fe.vp_ucode, sizeof g_fe.vp_ucode, 1, f);
        std::fclose(f);
    }
    const u32 sp = g_regs[NV4097_SET_SHADER_PROGRAM];
    const u32 ea = address(sp & ~3u, (sp & 3) - 1);
    if (ea == ~0u) return;
    u32 len = 0;  // up to and including the instruction with the end bit, plus inline constants
    {
        u8* p = mem::base + ea;
        auto word = [&](u32 off) { u32 w = ld32(p, off); return (w << 16) | (w >> 16); };
        for (u32 off = 0; off < 0x8000; off += 16) {
            const u32 d0 = word(off);
            len = off + 16;
            bool has_const = false;
            for (int k = 1; k <= 3; ++k) if ((word(off + 4 * k) & 3) == 2) has_const = true;
            if (has_const) { off += 16; len = off + 16; }
            if (d0 & 1) break;
        }
    }
    const u64 fh = fnv(mem::base + ea, len);
    if (FILE* f = open_dump("fp", fh, "bin")) {
        std::fwrite(mem::base + ea, len, 1, f);
        std::fclose(f);
        if (FILE* t = open_dump("fp", fh, "txt")) {
            std::fprintf(t, "shader_control %08x\n", g_regs[0x1d60 >> 2]);
            for (u32 i = 0; i < 16; ++i)
                std::fprintf(t, "tex%u format %08x control0 %08x control1 %08x filter %08x rect %08x\n", i,
                             g_regs[(0x1a04 >> 2) + 8 * i], g_regs[(0x1a0c >> 2) + 8 * i], g_regs[(0x1a10 >> 2) + 8 * i],
                             g_regs[(0x1a14 >> 2) + 8 * i], g_regs[(0x1a18 >> 2) + 8 * i]);
            std::fclose(t);
        }
    }
}

void begin_end(u32 arg) {
    auto& fe = g_fe;
    if (arg) {
        fe.prim = arg <= 10 ? Prim(arg) : Prim::None;
        fe.ranges.clear();
        fe.indexed = false;
        fe.push_vertices = 0;
        fe.push_mask = 0;
        for (auto& p : fe.push) p.clear();
        return;
    }
    if (fe.prim == Prim::None) return;
    if (fe.ranges.empty() && fe.push_vertices) fe.ranges.push_back({0, fe.push_vertices});
    if (!fe.ranges.empty()) {
        ++g_draws;
        dump_programs();
        g_renderer->draw();
    }
    fe.prim = Prim::None;
}

void inline_color(u32 index, u32 arg) {
    // NV308A image-from-CPU into the NV3062 destination surface (used by inline transfers).
    const u32 point = g_regs[NV308A_POINT], x = (point & 0xffff) + index, y = point >> 16;
    const u32 fmt = g_regs[NV3062_SET_COLOR_FORMAT], pitch = g_regs[NV3062_SET_PITCH] >> 16;
    const u32 dst = address(g_regs[NV3062_SET_OFFSET_DESTIN], g_regs[NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN]);
    if (dst == ~0u) { LOG_ERR("rsx: inline transfer to bad address"); return; }
    if (fmt == 4) {  // R5G6B5: two pixels per word
        const u32 ea = dst + y * pitch + 2 * (x + index);  // x already includes index; word index covers 2 pixels
        (void)ea;
        warn_once(NV308A_COLOR, "16-bit inline transfer");
        return;
    }
    const u32 ea = dst + y * pitch + 4 * x;
    st32(mem::base, ea, arg);
    g_renderer->invalidate(ea, 4);
}

// NV3089_IMAGE_IN: all other parameters are set; scale the source image into the NV3062 surface.
void scaled_image_copy() {
    BlitParams b{};
    const u32 in_size = g_regs[NV3089_IMAGE_IN_SIZE], in_fmt = g_regs[NV3089_IMAGE_IN_FORMAT], in_pt = g_regs[NV3089_IMAGE_IN];
    b.src_ea = address(g_regs[NV3089_IMAGE_IN_OFFSET], g_regs[NV3089_SET_CONTEXT_DMA_IMAGE]);
    b.src_pitch = in_fmt & 0xffff;
    b.src_w = in_size & 0xffff; b.src_h = in_size >> 16;
    const u32 cfmt = g_regs[NV3089_SET_COLOR_FORMAT];
    b.src_bpp = (cfmt == 7 || cfmt == 1 || cfmt == 2) ? 2 : (cfmt == 8 || cfmt == 9) ? 1 : 4;
    b.linear = (in_fmt >> 24) == 1;
    b.dst_fmt = g_regs[NV3062_SET_COLOR_FORMAT];
    b.dst_pitch = g_regs[NV3062_SET_PITCH] >> 16;
    b.dst_ea = address(g_regs[NV3062_SET_OFFSET_DESTIN], g_regs[NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN]);
    b.dst_bpp = b.dst_fmt == 4 ? 2 : b.dst_fmt == 1 ? 1 : 4;
    const s32 ox = s16(g_regs[NV3089_IMAGE_OUT_POINT] & 0xffff), oy = s16(g_regs[NV3089_IMAGE_OUT_POINT] >> 16);
    const u32 ow = g_regs[NV3089_IMAGE_OUT_SIZE] & 0xffff, oh = g_regs[NV3089_IMAGE_OUT_SIZE] >> 16;
    const s32 cx = s16(g_regs[NV3089_CLIP_POINT] & 0xffff), cy = s16(g_regs[NV3089_CLIP_POINT] >> 16);
    const u32 cw = g_regs[NV3089_CLIP_SIZE] & 0xffff, ch = g_regs[NV3089_CLIP_SIZE] >> 16;
    const float dsdx = float(s32(g_regs[NV3089_DS_DX])) / 1048576.f, dtdy = float(s32(g_regs[NV3089_DT_DY])) / 1048576.f;
    const s32 x0 = std::max(ox, cx), y0 = std::max(oy, cy);
    const s32 x1 = std::min(ox + s32(ow), cx + s32(cw)), y1 = std::min(oy + s32(oh), cy + s32(ch));
    if (b.src_ea == ~0u || b.dst_ea == ~0u || x1 <= x0 || y1 <= y0 || x0 < 0 || y0 < 0) return;
    b.dx = x0; b.dy = y0; b.dw = x1 - x0; b.dh = y1 - y0;
    // source point is 12.4 fixed point; clipping moves the start by the clipped amount
    b.sx = float(in_pt & 0xffff) / 16.f + (x0 - ox) * dsdx;
    b.sy = float(in_pt >> 16) / 16.f + (y0 - oy) * dtdy;
    if ((in_fmt >> 16 & 0xff) == 1) { b.sx -= 0.5f; b.sy -= 0.5f; }  // center origin
    b.sx = std::max(0.f, b.sx); b.sy = std::max(0.f, b.sy);
    b.sw = b.dw * dsdx; b.sh = b.dh * dtdy;
    if (g_renderer->blit(b)) return;
    {
        static int logged = 0;
        if (logged < 8 && g_log_level >= 1) {
            ++logged;
            LOG_NOTE("rsx: scaled image copy done in memory (source 0x%08x %ux%u pitch %u fmt %u -> 0x%08x pitch %u fmt %u)", b.src_ea, b.src_w, b.src_h,
                     b.src_pitch, cfmt, b.dst_ea, b.dst_pitch, b.dst_fmt);
        }
    }
    // memory copy, nearest sample
    if (b.src_bpp != b.dst_bpp) { warn_once(NV3089_IMAGE_IN, "scaled image copy between formats"); return; }
    for (u32 y = 0; y < b.dh; ++y) {
        const u32 sy = std::min(b.src_h ? b.src_h - 1 : 0, u32(b.sy + (y + 0.5f) * dtdy));
        for (u32 x = 0; x < b.dw; ++x) {
            const u32 sx = std::min(b.src_w ? b.src_w - 1 : 0, u32(b.sx + (x + 0.5f) * dsdx));
            const u32 se = b.src_ea + sy * b.src_pitch + sx * b.src_bpp, de = b.dst_ea + (b.dy + y) * b.dst_pitch + (b.dx + x) * b.dst_bpp;
            if (mem::is_mapped(se, b.src_bpp) && mem::is_mapped(de, b.dst_bpp)) std::memcpy(mem::base + de, mem::base + se, b.src_bpp);
        }
    }
    g_renderer->invalidate(b.dst_ea + b.dy * b.dst_pitch, b.dh * b.dst_pitch);
}

int g_tc_fresh = 0;
bool graphics_method(u32 reg, u32 arg) {
    auto& fe = g_fe;
    if (reg >= NV4097_SET_TRANSFORM_PROGRAM && reg < NV4097_SET_TRANSFORM_PROGRAM + 32) {
        const u32 load = g_regs[NV4097_SET_TRANSFORM_PROGRAM_LOAD], sub = (reg - NV4097_SET_TRANSFORM_PROGRAM) % 4;
        if (load < kVpInstructions) fe.vp_ucode[load * 4 + sub] = arg;
        ++fe.vp_gen;
        if (sub == 3) g_regs[NV4097_SET_TRANSFORM_PROGRAM_LOAD] = load + 1;
        return true;
    }
    if (reg == NV4097_SET_TRANSFORM_CONSTANT_LOAD) { static const bool dbg = std::getenv("TOD_VP_ATTRS"); if (dbg) { extern int g_tc_fresh; g_tc_fresh = 1; } }
    if (reg >= NV4097_SET_TRANSFORM_CONSTANT && reg < NV4097_SET_TRANSFORM_CONSTANT + 32) {
        static const bool dbg = std::getenv("TOD_VP_ATTRS");
        if (dbg && reg == NV4097_SET_TRANSFORM_CONSTANT) {
            extern int g_tc_fresh;
            static u64 with = 0, without = 0, prev_nonzero = 0;
            (g_tc_fresh ? with : without)++;
            g_tc_fresh = 0;
            if ((with + without) % 20000 == 0)
                LOG_NOTE("transform constants: %llu packets after a load address write, %llu without", (unsigned long long)with, (unsigned long long)without);
            (void)prev_nonzero;
        }
        const u32 word = g_regs[NV4097_SET_TRANSFORM_CONSTANT_LOAD] * 4 + (reg - NV4097_SET_TRANSFORM_CONSTANT);
        if (word < kVpConstants * 4) { std::memcpy(&fe.vc[word / 4][word % 4], &arg, 4); ::g_vc_stamp[word / 4] = ::g_draw_no; ::g_vc_ea[word / 4] = ::g_cmd_ea; }
        ++fe.vc_gen;
        return true;
    }
    if (reg >= NV308A_COLOR && reg < NV308A_COLOR + 0x700) { inline_color(reg - NV308A_COLOR, arg); return true; }
    switch (reg) {
    case NV4097_SET_BEGIN_END: begin_end(arg); return true;
    case NV4097_DRAW_ARRAYS:
    case NV4097_DRAW_INDEX_ARRAY:
        fe.indexed = reg == NV4097_DRAW_INDEX_ARRAY;
        fe.ranges.push_back({arg & 0xffffff, (arg >> 24) + 1});
        return true;
    case NV4097_INLINE_ARRAY: warn_once(reg, "inline vertex array"); return true;
    case NV4097_CLEAR_SURFACE: g_renderer->clear(arg); return true;
    case NV3089_IMAGE_IN: scaled_image_copy(); return true;
    default: break;
    }
    return immediate_vertex_data(reg, arg);
}

void execute_method(u32 reg, u32 arg) {
    if (g_trace) std::fprintf(g_trace, "%04x %08x\n", reg << 2, arg);
    g_regs[reg] = arg;
    g_reg_dirty[reg >> 6] |= 1ull << (reg & 63);
    if (graphics_method(reg, arg)) return;
    switch (reg) {
    case NV406E_SET_REFERENCE: st_be_atomic(kControlAddr + 8, arg); break;
    case NV406E_SEMAPHORE_ACQUIRE: semaphore_acquire(arg); break;
    case NV406E_SEMAPHORE_RELEASE: write_label(sema_addr_406e(), arg); break;
    case NV4097_TEXTURE_READ_SEMAPHORE_RELEASE: write_label(sema_addr_4097(), arg); break;
    case NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE:
        write_label(sema_addr_4097(), (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff));
        break;
    case NV4097_GET_REPORT: get_report(arg); break;
    case NV0039_BUFFER_NOTIFY: buffer_notify(); break;
    case GCM_SET_USER_COMMAND: queue_interrupt(g_user_handler.load(), arg); break;
    case GCM_FLIP_COMMAND: flip(arg); break;
    default: break;
    }
}

// ---- FIFO --------------------------------------------------------------------------------
void fifo_thread() {
    u32 get = ld_be_atomic(kControlAddr + 4);
    u32 call_stack[16];
    int call_depth = 0;
    unsigned idle = 0;
    std::array<u32, 3> flow[24]{};
    std::array<u32, 2> recent[32]{};
    u32 flow_pos = 0, recent_pos = 0;
    for (;;) {
        const u32 put = ld_be_atomic(kControlAddr);
        if (get == put) {
            if (++idle < 2000) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        idle = 0;
        const u32 ea = io_to_ea(get);
        if (ea == ~0u) {
            LOG_ERR("rsx: FIFO get 0x%x is not IO-mapped (put 0x%x); stopping command processing", get, put);
            // recent FIFO control flow and the commands before the bad jump
            for (u32 i = 0; i < 24; ++i) {
                const auto& f = flow[(flow_pos + i) % 24];
                if (f[1]) std::fprintf(stderr, "[E]   flow: at 0x%08x cmd %08x (depth %u)\n", f[0], f[1], f[2]);
            }
            for (u32 i = 0; i < 32; ++i) {
                const auto& f = recent[(recent_pos + i) & 31];
                if (f[1]) std::fprintf(stderr, "[E]   cmd: at 0x%08x %08x\n", f[0], f[1]);
            }
            return;
        }
        const u32 cmd = ld32(mem::base, ea);
        ::g_cmd_ea = ea;
        recent[recent_pos++ & 31] = {get, cmd};
        if ((cmd & 0xe0000003) == 0x20000000 || (cmd & 3) == 1 || (cmd & 3) == 2 || (cmd & 0xffff0003) == 0x00020000)
            flow[flow_pos++ % 24] = {get, cmd, u32(call_depth)};
        ++g_commands;
        if ((cmd & 0xe0000003) == 0x20000000) {         // old jump
            get = cmd & 0x1ffffffc;
        } else if ((cmd & 3) == 1) {                    // new jump
            get = cmd & 0xfffffffc;
        } else if ((cmd & 3) == 2) {                    // call
            if (call_depth == 16) { LOG_ERR("rsx: FIFO call stack overflow at 0x%x", get); return; }
            call_stack[call_depth++] = get + 4;
            get = cmd & 0xfffffffc;
        } else if ((cmd & 0xffff0003) == 0x00020000) {  // return
            if (!call_depth) { LOG_ERR("rsx: FIFO return without call at 0x%x", get); return; }
            get = call_stack[--call_depth];
        } else if ((cmd & 0xbfff0003) == 0) {           // nop (count 0)
            get += 4;
        } else {
            const u32 count = (cmd >> 18) & 0x7ff, method = (cmd & 0xfffc) >> 2;
            const bool non_inc = (cmd & 0x40000000) != 0;
            for (u32 i = 0; i < count; ++i) {
                const u32 aea = io_to_ea(get + 4 + 4 * i);
                const u32 reg = non_inc ? method : method + i;
                if (aea == ~0u) { LOG_ERR("rsx: FIFO argument outside IO map at 0x%x", get + 4 + 4 * i); return; }
                if (reg >= 0x4000) { LOG_ERR("rsx: method 0x%x out of range at 0x%x", reg << 2, get); break; }
                if (g_log_level >= 2 && g_method_hist[reg]++ == 0) LOG_TRACE("rsx: first use of method 0x%04x", reg << 2);
                execute_method(reg, ld32(mem::base, aea));
            }
            get += 4 + 4 * count;
        }
        st_be_atomic(kControlAddr + 4, get);
    }
}

// ---- interrupts / vblank ---------------------------------------------------------------
void intr_thread(PPUContext& c) {
    using namespace std::chrono;
    const auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / 59.94));
    auto next = steady_clock::now() + period;
    for (;;) {
        std::vector<std::pair<u32, u32>> work;
        {
            std::unique_lock lk(g_intr_lock);
            g_intr_cv.wait_until(lk, next, [] { return !g_intr_queue.empty(); });
            work.swap(g_intr_queue);
        }
        if (steady_clock::now() >= next) {
            next += period;
            ++g_vblanks;
            if (u32 h = g_vblank_handler.load()) work.emplace_back(h, 1);
        }
        for (auto [opd, arg] : work) guest_call(c, opd, {arg});
    }
}

void reset_registers() {
    static const u32 defaults[][2] = {
#include "rsx_defaults.inc"
    };
    std::memset(g_regs, 0, sizeof g_regs);
    for (auto& d : defaults) g_regs[d[0]] = d[1];
    std::memset(g_reg_dirty, 0xff, sizeof g_reg_dirty);
    g_regs[0x08E4 >> 2] = 1;  // NV4097_SET_SHADER_PROGRAM: offset 0, local memory
}

void init_reports() {
    std::memset(mem::base + kLabelAddr, 0, 0x1400 + 2048 * 16);
    for (u32 i = 0; i < 1024; i += 4) {  // semaphores start with the system's marker pattern
        st32(mem::base, kLabelAddr + 4 * i + 0, 0x1337C0D3);
        st32(mem::base, kLabelAddr + 4 * i + 4, 0x1337BABE);
        st32(mem::base, kLabelAddr + 4 * i + 8, 0x1337BEEF);
        st32(mem::base, kLabelAddr + 4 * i + 12, 0x1337F001);
    }
    for (u32 i = 0; i < 64; ++i) st64(mem::base, kLabelAddr + 0x1000 + 16 * i, ~0ull);  // notify timestamps
    for (u32 i = 0; i < 2048; ++i) {
        st64(mem::base, kLabelAddr + 0x1400 + 16 * i, ~0ull);
        st32(mem::base, kLabelAddr + 0x1400 + 16 * i + 12, ~0u);
    }
    // driver info: version, frequencies and report offsets as the system reports them
    st32(mem::base, kDriverInfo + 0x00, 0x211);
    st32(mem::base, kDriverInfo + 0x04, 0x5c);
    st32(mem::base, kDriverInfo + 0x08, kLocalSize);
}
}  // namespace

// ---- public API -----------------------------------------------------------------------------
void init() {
    std::call_once(g_init_once, [] {
        std::memset(g_io_of_ea, 0xff, sizeof g_io_of_ea);
        std::memset(g_ea_of_io, 0xff, sizeof g_ea_of_io);
        mem::map(kDeviceAddr, kAreaSize);
        mem::map(kLocalBase, kLocalSize);
        init_reports();
        reset_registers();
        st32(mem::base, kControlAddr + 0, 0);
        st32(mem::base, kControlAddr + 4, 0);
        st32(mem::base, kControlAddr + 8, 0xffffffff);
        std::thread(fifo_thread).detach();
        host_thread_create("_gcm_intr_thread", intr_thread);
        LOG_NOTE("rsx: local memory 0x%x bytes at 0x%x, command processor running", kLocalSize, kLocalBase);
    });
}

bool map_io(u32 ea, u32 io, u32 size) {
    if (!size || (ea & 0xfffff) || (io & 0xfffff) || (size & 0xfffff) || io + size > kMainMemSize) return false;
    std::lock_guard lk(g_io_lock);
    for (u32 i = 0; i < size >> 20; ++i) {
        g_io_of_ea[(ea >> 20) + i] = u16((io >> 20) + i);
        g_ea_of_io[(io >> 20) + i] = u16((ea >> 20) + i);
    }
    LOG_NOTE("rsx: IO map ea 0x%x -> io 0x%x (0x%x bytes)", ea, io, size);
    return true;
}

u32 io_to_ea(u32 io) {
    if (io >= kMainMemSize) return ~0u;
    const u16 p = g_ea_of_io[io >> 20];
    return p == 0xffff ? ~0u : (u32(p) << 20) | (io & 0xfffff);
}

u32 ea_to_io(u32 ea) {
    const u16 p = g_io_of_ea[ea >> 20];
    return p == 0xffff ? ~0u : (u32(p) << 20) | (ea & 0xfffff);
}

u32 address(u32 offset, u32 location) {
    switch (location) {
    case CELL_GCM_LOCATION_LOCAL:
    case DMA_FRAME_BUFFER: return offset < kLocalSize ? kLocalBase + offset : ~0u;
    case CELL_GCM_LOCATION_MAIN:
    case DMA_HOST_BUFFER: {  // Bink sets bit 31 on its texture offsets
        static const bool nomask = std::getenv("TOD_NO_BIT31MASK");
        return io_to_ea(nomask ? offset : offset & 0x0fffffff);
    }
    case DMA_REPORT_LOCAL: return offset < 2048 * 16 ? kLabelAddr + 0x1400 + offset : ~0u;
    case DMA_REPORT_MAIN: return offset < 0x1000000 ? io_to_ea(0x0e000000 + offset) : ~0u;
    case DMA_SEMAPHORE_RW:
    case DMA_SEMAPHORE_R: return offset < 0x1000 ? kLabelAddr + offset : ~0u;
    case DMA_DEVICE_RW:
    case DMA_DEVICE_R: return offset < 0x100000 ? kDeviceAddr + offset : ~0u;
    default: return ~0u;
    }
}

void set_display_buffer(u32 id, const DisplayBuffer& b) { std::lock_guard lk(g_disp_lock); g_display[id & 7] = b; }
DisplayBuffer display_buffer(u32 id) { std::lock_guard lk(g_disp_lock); return g_display[id & 7]; }
void set_flip_mode(u32 mode) { g_flip_mode = mode; }
void set_vblank_handler(u32 opd) { g_vblank_handler = opd; }
void set_flip_handler(u32 opd) { g_flip_handler = opd; }
void set_user_handler(u32 opd) { g_user_handler = opd; }
u64 flip_count() { return g_flips; }
u64 vblank_count() { return g_vblanks; }
u32 current_display_buffer() { return g_current_buffer; }

}  // namespace rsx
