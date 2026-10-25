// PPU guest state and helpers used by generated code (BCUS98127 static recompilation).
#pragma once
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u128 = unsigned __int128;
using s128 = __int128;

static_assert(std::endian::native == std::endian::little, "host must be little-endian");

// Vector register: element i (big-endian numbering) lives in w[i] / f[i] natively.
// Byte and halfword elements are addressed through b()/h() to keep BE numbering.
struct alignas(16) v128 {
    union {
        u32 w[4];
        s32 sw[4];
        float f[4];
        u8 raw[16];
        u16 rawh[8];
    };
    u8& b(int i) { return raw[(i & ~3) | (3 - (i & 3))]; }
    u8 b(int i) const { return raw[(i & ~3) | (3 - (i & 3))]; }
    u16& h(int i) { return rawh[i ^ 1]; }
    u16 h(int i) const { return rawh[i ^ 1]; }
};

struct CRField {
    u8 lt, gt, eq, so;
};

// Thrown by the translated longjmp; caught by the function that called setjmp on that buffer.
struct GuestLongjmp { u32 buf; };

struct PPUContext {
    u64 r[32];
    double f[32];
    v128 v[32];
    u64 lr, ctr;
    CRField cr[8];
    u8 xer_so, xer_ov, xer_ca, xer_cnt;
    u32 fpscr;
    u32 vscr;
    u32 vrsave;
    // lwarx/ldarx reservation
    u32 reserve_addr;
    u64 reserve_value;
    bool reserve;
    // Host bookkeeping
    u32 cia;  // last address recorded by a probe or error path
};

using PPUFunc = void (*)(PPUContext&, u8*);

// ---- runtime services (ppu_runtime.cpp / HLE) ---------------------------------
PPUFunc ppu_lookup(u32 addr);
// Host functions reachable from guest function pointers: returns a fake code address
// (below the image, never real code) that ppu_lookup resolves to fn.
u32 ppu_register_thunk(PPUFunc fn);
void ppu_dispatch(PPUContext& c, u8* base, u32 target);
[[noreturn]] void ppu_bad_jump(PPUContext& c, u32 site, u32 target);
void ppu_unimplemented(PPUContext& c, u8* base, u32 addr, u32 word);
void ppu_trap(PPUContext& c, u8* base, u32 addr);
void ppu_syscall(PPUContext& c, u8* base, u32 addr);
void ppu_import(PPUContext& c, u8* base, u32 index);
void ppu_probe(PPUContext& c, u8* base, u32 addr);
// A guard in a HOOKS entry fired: logged the first few times per site.
void ppu_note(u32 site, const char* what);
// ppu_note plus a dump of the guest bytes around addr (first few occurrences)
void ppu_note_rec(u32 lr, u32 a, u32 b, u32 c);
void ppu_note_vals(u32 site, u32 a, u32 b, u32 c);
void ppu_note_mem(u32 site, const char* what, u8* base, u32 addr);
void ppu_wait_slot_helper(u8* base);
// Per-128-byte-line spinlocks shared by PPU store-conditional and SPU lock-line commands, so an
// SPU PUTLLC (check the whole line, then write) and a PPU stwcx/stdcx never interleave.
struct PPULineLock {
    std::atomic<bool> held{false};
    void lock() { while (held.exchange(true, std::memory_order_acquire)) while (held.load(std::memory_order_relaxed)) __builtin_ia32_pause(); }
    void unlock() { held.store(false, std::memory_order_release); }
};
PPULineLock& ppu_line_lock(u32 ea);
// TOD_SLOTLOG: numbered trace line shared by PPU hooks and SPU DMA (diagnostics only)
u32 ppu_watch_ea();
bool ppu_sortcheck_on();
void ppu_watch_ctx(u8* base, u32 pp, u32 cx, u32 lr);
void ppu_sort_before(u8* base, u32 pass);
void ppu_sort_after(u8* base, u32 pass);
extern std::atomic<bool> ppu_watch_armed;
void ppu_watch(u32 site, u32 lo, u32 len, u32 a, u32 b);
void ppu_trace(u32 site, u32 a, u32 b);
unsigned long long ppu_trace_seq();
bool ppu_trace_on();
// desc is a function descriptor in the image (.opd or data) whose code address is known.
bool ppu_valid_opd(u8* base, u32 desc);
// Movie skipping (host/hle_io.cpp): a Bink handle was opened; true once the player asked to skip it.
void ppu_movie_opened(u32 hbink);
bool ppu_movie_skip(u32 hbink);
// Host hooks (host/cheats.cpp) at the HOOKS sites that call it. A hook may change registers or
// memory; true asks the site to return from the guest function (sites that allow it say so).
bool ppu_host_hook(PPUContext& c, u8* base, u32 site);
u64 ppu_timebase();
bool ppu_store_conditional32(PPUContext& c, u8* base, u32 ea, u32 value);
bool ppu_store_conditional64(PPUContext& c, u8* base, u32 ea, u64 value);

// ---- memory (guest effective addresses are 32-bit) -------------------------------
#define PPU_EA(x) static_cast<u32>(x)

static inline u8 ld8(u8* base, u64 ea) { return base[PPU_EA(ea)]; }
static inline u16 ld16(u8* base, u64 ea) { u16 v; std::memcpy(&v, base + PPU_EA(ea), 2); return __builtin_bswap16(v); }
// Raw SPU problem-state registers live at 0xE0000000+; 32-bit accesses there go to the host.
u32 ppu_mmio_ld32(u32 ea);
void ppu_mmio_st32(u32 ea, u32 value);
static inline u32 ld32(u8* base, u64 ea) {
    if (__builtin_expect(PPU_EA(ea) >= 0xE0000000u, 0)) return ppu_mmio_ld32(PPU_EA(ea));
    u32 v; std::memcpy(&v, base + PPU_EA(ea), 4); return __builtin_bswap32(v);
}
static inline u64 ld64(u8* base, u64 ea) { u64 v; std::memcpy(&v, base + PPU_EA(ea), 8); return __builtin_bswap64(v); }
static inline void st8(u8* base, u64 ea, u64 x) { base[PPU_EA(ea)] = static_cast<u8>(x); }
static inline void st16(u8* base, u64 ea, u64 x) { u16 v = __builtin_bswap16(static_cast<u16>(x)); std::memcpy(base + PPU_EA(ea), &v, 2); }
static inline void st32(u8* base, u64 ea, u64 x) {
    if (__builtin_expect(PPU_EA(ea) >= 0xE0000000u, 0)) { ppu_mmio_st32(PPU_EA(ea), static_cast<u32>(x)); return; }
    u32 v = __builtin_bswap32(static_cast<u32>(x)); std::memcpy(base + PPU_EA(ea), &v, 4);
}
static inline void st64(u8* base, u64 ea, u64 x) { u64 v = __builtin_bswap64(x); std::memcpy(base + PPU_EA(ea), &v, 8); }

static inline float ldf32(u8* base, u64 ea) { return std::bit_cast<float>(ld32(base, ea)); }
static inline double ldf64(u8* base, u64 ea) { return std::bit_cast<double>(ld64(base, ea)); }
static inline void stf32(u8* base, u64 ea, double x) { st32(base, ea, std::bit_cast<u32>(static_cast<float>(x))); }
static inline void stf64(u8* base, u64 ea, double x) { st64(base, ea, std::bit_cast<u64>(x)); }

static inline void ldv(u8* base, u64 ea, v128& v) {
    const u32 a = PPU_EA(ea) & ~15u;
    for (int i = 0; i < 4; ++i) v.w[i] = ld32(base, a + 4 * i);
}
static inline void stv(u8* base, u64 ea, const v128& v) {
    const u32 a = PPU_EA(ea) & ~15u;
    for (int i = 0; i < 4; ++i) st32(base, a + 4 * i, v.w[i]);
}

// ---- integer helpers -------------------------------------------------------------
static inline u64 rotl64(u64 x, unsigned n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
static inline u64 dup32(u64 x) { const u64 lo = static_cast<u32>(x); return lo | (lo << 32); }
static inline u64 rotl32(u64 x, unsigned n) { return rotl64(dup32(x), n & 31); }

static inline void cr_set_s(CRField& f, s64 a, s64 b, u8 so) { f.lt = a < b; f.gt = a > b; f.eq = a == b; f.so = so; }
static inline void cr_set_u(CRField& f, u64 a, u64 b, u8 so) { f.lt = a < b; f.gt = a > b; f.eq = a == b; f.so = so; }
static inline void cr0(PPUContext& c, u64 r) { cr_set_s(c.cr[0], static_cast<s64>(r), 0, c.xer_so); }

static inline u32 cr_get(const PPUContext& c) {
    u32 v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 4) | (c.cr[i].lt << 3) | (c.cr[i].gt << 2) | (c.cr[i].eq << 1) | c.cr[i].so;
    return v;
}
static inline void cr_put_field(PPUContext& c, int i, u32 nib) {
    c.cr[i].lt = (nib >> 3) & 1; c.cr[i].gt = (nib >> 2) & 1; c.cr[i].eq = (nib >> 1) & 1; c.cr[i].so = nib & 1;
}
static inline u64 xer_get(const PPUContext& c) {
    return (u64(c.xer_so) << 31) | (u64(c.xer_ov) << 30) | (u64(c.xer_ca) << 29) | c.xer_cnt;
}
static inline void xer_put(PPUContext& c, u64 v) {
    c.xer_so = (v >> 31) & 1; c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1; c.xer_cnt = v & 0x7f;
}

static inline u64 add_ca(u64 a, u64 b, u8 cin, u8& cout) {
    const u128 r = u128(a) + b + cin;
    cout = static_cast<u8>(r >> 64);
    return static_cast<u64>(r);
}

static inline u64 sraw_(PPUContext& c, u64 rs, u64 rb) {
    const s32 s = static_cast<s32>(rs);
    const unsigned n = rb & 63;
    if (n >= 32) { c.xer_ca = s < 0; return static_cast<u64>(static_cast<s64>(s < 0 ? -1 : 0)); }
    c.xer_ca = s < 0 && (static_cast<u32>(s) & ((1u << n) - 1)) != 0;
    return static_cast<u64>(static_cast<s64>(s >> n));
}
static inline u64 srad_(PPUContext& c, u64 rs, u64 rb) {
    const s64 s = static_cast<s64>(rs);
    const unsigned n = rb & 127;
    if (n >= 64) { c.xer_ca = s < 0; return static_cast<u64>(s < 0 ? -1 : 0); }
    c.xer_ca = s < 0 && n && (static_cast<u64>(s) & ((u64(1) << n) - 1)) != 0;
    return static_cast<u64>(s >> n);
}
static inline u64 slw_(u64 rs, u64 rb) { const unsigned n = rb & 63; return n >= 32 ? 0 : static_cast<u32>(static_cast<u32>(rs) << n); }
static inline u64 srw_(u64 rs, u64 rb) { const unsigned n = rb & 63; return n >= 32 ? 0 : static_cast<u32>(rs) >> n; }
static inline u64 sld_(u64 rs, u64 rb) { const unsigned n = rb & 127; return n >= 64 ? 0 : rs << n; }
static inline u64 srd_(u64 rs, u64 rb) { const unsigned n = rb & 127; return n >= 64 ? 0 : rs >> n; }

// Division by zero / overflow results are undefined on hardware; these follow a fixed choice.
static inline u64 divw_(u64 a, u64 b) {
    const s32 x = static_cast<s32>(a), y = static_cast<s32>(b);
    if (y == 0 || (x == INT32_MIN && y == -1)) return 0;
    return static_cast<u32>(x / y);
}
static inline u64 divwu_(u64 a, u64 b) { const u32 y = static_cast<u32>(b); return y ? static_cast<u32>(a) / y : 0; }
static inline u64 divd_(u64 a, u64 b) {
    const s64 x = static_cast<s64>(a), y = static_cast<s64>(b);
    if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
    return static_cast<u64>(x / y);
}
static inline u64 divdu_(u64 a, u64 b) { return b ? a / b : 0; }
static inline u64 mulhw_(u64 a, u64 b) { return static_cast<u64>((s64(s32(a)) * s64(s32(b))) >> 32); }
static inline u64 mulhwu_(u64 a, u64 b) { return (u64(u32(a)) * u64(u32(b))) >> 32; }
static inline u64 mulhd_(u64 a, u64 b) { return static_cast<u64>((s128(s64(a)) * s64(b)) >> 64); }
static inline u64 mulhdu_(u64 a, u64 b) { return static_cast<u64>((u128(a) * b) >> 64); }
static inline u64 cntlzw_(u64 a) { const u32 x = static_cast<u32>(a); return x ? __builtin_clz(x) : 32; }
static inline u64 cntlzd_(u64 a) { return a ? __builtin_clzll(a) : 64; }

static inline bool trap_cond(unsigned to, u64 a, u64 b, bool wide) {
    s64 sa, sb; u64 ua, ub;
    if (wide) { sa = s64(a); sb = s64(b); ua = a; ub = b; }
    else { sa = s32(a); sb = s32(b); ua = u32(a); ub = u32(b); }
    return ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && sa == sb) ||
           ((to & 2) && ua < ub) || ((to & 1) && ua > ub);
}

// lswi/stswi: move n bytes between memory and consecutive registers, 4 per register.
static inline void lswi_(PPUContext& c, u8* base, u64 ea, unsigned rt, unsigned n) {
    unsigned r = rt - 1, i = 0;
    for (unsigned k = 0; k < n; ++k) {
        if (i == 0) { r = (r + 1) & 31; c.r[r] = 0; }
        c.r[r] |= u64(ld8(base, ea + k)) << (24 - i);
        i = (i + 8) & 31;
    }
}
static inline void stswi_(PPUContext& c, u8* base, u64 ea, unsigned rs, unsigned n) {
    unsigned r = rs - 1, i = 0;
    for (unsigned k = 0; k < n; ++k) {
        if (i == 0) r = (r + 1) & 31;
        st8(base, ea + k, c.r[r] >> (24 - i));
        i = (i + 8) & 31;
    }
}

// ---- floating point ---------------------------------------------------------------
static inline u64 fbits(double d) { return std::bit_cast<u64>(d); }
static inline double fdbl(u64 u) { return std::bit_cast<double>(u); }
static inline double rnd_s(double x) { return static_cast<double>(static_cast<float>(x)); }

static inline double fctiw_(double x, bool trunc) {
    s64 r;
    if (std::isnan(x)) r = INT32_MIN;
    else {
        const double t = trunc ? std::trunc(x) : std::nearbyint(x);
        r = t >= 2147483647.0 ? INT32_MAX : t <= -2147483648.0 ? INT32_MIN : static_cast<s32>(t);
    }
    return fdbl(0xfff8000000000000ull | static_cast<u32>(r));
}
static inline double fctid_(double x, bool trunc) {
    s64 r;
    if (std::isnan(x)) r = INT64_MIN;
    else {
        const double t = trunc ? std::trunc(x) : std::nearbyint(x);
        r = t >= 9223372036854775807.0 ? INT64_MAX : t <= -9223372036854775808.0 ? INT64_MIN : static_cast<s64>(t);
    }
    return fdbl(static_cast<u64>(r));
}
static inline void fcmp_(PPUContext& c, int crf, double a, double b) {
    CRField& f = c.cr[crf];
    const bool un = std::isnan(a) || std::isnan(b);
    f.lt = !un && a < b; f.gt = !un && a > b; f.eq = !un && a == b; f.so = un;
    c.fpscr = (c.fpscr & ~0xf000u) | (u32(f.lt) << 15) | (u32(f.gt) << 14) | (u32(f.eq) << 13) | (u32(un) << 12);
}
static inline double fsel_(double a, double b, double cc) { return a >= 0.0 ? cc : b; }

// ---- vector helpers ------------------------------------------------------------------
static inline v128 vperm_(const v128& a, const v128& b, const v128& c) {
    v128 r;
    for (int i = 0; i < 16; ++i) {
        const int s = c.b(i) & 31;
        r.b(i) = s < 16 ? a.b(s) : b.b(s - 16);
    }
    return r;
}
static inline v128 vsldoi_(const v128& a, const v128& b, int sh) {
    v128 r;
    for (int i = 0; i < 16; ++i) { const int s = i + sh; r.b(i) = s < 16 ? a.b(s) : b.b(s - 16); }
    return r;
}
static inline v128 vsplat_w(u32 x) { v128 r; for (int i = 0; i < 4; ++i) r.w[i] = x; return r; }
static inline u32 sat_s32(double x) { return x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? u32(INT32_MIN) : std::isnan(x) ? 0 : u32(s32(x)); }
static inline u32 sat_u32(double x) { return x >= 4294967295.0 ? 0xffffffffu : (x <= 0.0 || std::isnan(x)) ? 0 : u32(x); }
static inline void vcr6(PPUContext& c, const v128& r) {
    const bool all = r.w[0] == ~0u && r.w[1] == ~0u && r.w[2] == ~0u && r.w[3] == ~0u;
    const bool none = (r.w[0] | r.w[1] | r.w[2] | r.w[3]) == 0;
    c.cr[6].lt = all; c.cr[6].gt = 0; c.cr[6].eq = none; c.cr[6].so = 0;
}
