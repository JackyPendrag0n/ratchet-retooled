// SPU core shared by the interpreter, the recompiled SPU code and the SPURS kernel layer.
//
// Registers are stored as host-order 128-bit integers holding the big-endian quadword, so the
// SPU's element 0 (the preferred slot) is the most significant part: word element i is w[3 - i],
// halfword element i is h[7 - i], byte element i is b[15 - i]. Lane-wise operations ignore the
// order, quadword shifts and rotates are plain 128-bit shifts, and a load from local store is one
// byte reversal.
#pragma once
#include "ppu.h"

#include <atomic>
#include <mutex>
#include <immintrin.h>
#include <string>

union alignas(16) spu_reg {
    u128 q;
    u64 d[2];
    s64 sd[2];
    u32 w[4];
    s32 sw[4];
    u16 h[8];
    s16 sh[8];
    u8 b[16];
    s8 sb[16];
    float f[4];
    double df[2];
    __m128i m;
    __m128 mf;
    __m128d md;
};
static_assert(sizeof(spu_reg) == 16);

// Element accessors in SPU (big-endian) numbering.
#define SPU_W(r, i) ((r).w[3 - (i)])
#define SPU_H(r, i) ((r).h[7 - (i)])
#define SPU_B(r, i) ((r).b[15 - (i)])

constexpr u32 kLsSize = 0x40000;
constexpr u32 kLsMask = 0x3ffff;

// SPU event bits (SPU_RdEventStat / SPU_WrEventMask)
enum : u32 {
    SPU_EVENT_MS = 0x1000, SPU_EVENT_A = 0x800, SPU_EVENT_LR = 0x400, SPU_EVENT_S1 = 0x200, SPU_EVENT_S2 = 0x100,
    SPU_EVENT_LE = 0x80, SPU_EVENT_ME = 0x40, SPU_EVENT_TM = 0x20, SPU_EVENT_MB = 0x10, SPU_EVENT_QV = 0x8,
    SPU_EVENT_SN = 0x2, SPU_EVENT_TG = 0x1,
};

struct SPUThread;
// A stop instruction the host handles. Returns true to continue at pc + 4 (or wherever the
// handler moved pc), false to leave the run loop.
using SPUStopHandler = bool (*)(SPUThread& s, u32 code);

struct SPUThread {
    spu_reg gpr[128];
    u32 pc = 0;
    u8* ls = nullptr;  // 256 KiB, big-endian bytes like guest memory
    int index = 0;     // physical SPU number
    std::string name;

    // interrupts
    bool int_enabled = false;
    u32 srr0 = 0;

    // events
    u32 event_mask = 0;
    std::atomic<u32> event_stat{0};

    // decrementer
    u32 dec_value = 0;
    u64 dec_start = 0;      // timebase when dec_value was written
    bool dec_tm_armed = false;

    // MFC parameters and tag state
    u32 mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
    u32 tag_mask = 0;
    u32 tag_stat = 0;
    bool tag_stat_valid = false;
    u32 stall_stat = 0;     // tags with a stalled list (not complete until acknowledged)
    u32 stall_notify = 0;   // MFC_RdListStallStat bits not read yet
    u32 tag_update = ~0u;   // pending MFC_WrTagUpdate mode
    struct StalledList { bool active = false; u32 cmd, tag, lsa, list_lsa, list_size, eah; } stalled[32];
    u32 atomic_stat = 0;
    bool atomic_stat_valid = false;
    alignas(128) u8 rdata[128];
    u32 raddr = 0;
    bool rvalid = false;

    // mailboxes (only what SPURS code uses)
    std::mutex mbox_lock;
    u32 out_mbox = 0; bool out_mbox_valid = false;
    u32 out_intr_mbox = 0; bool out_intr_mbox_valid = false;
    u32 in_mbox[4]{}; u32 in_mbox_count = 0;
    u32 signal1 = 0, signal2 = 0;
    // Called after the SPU writes SPU_WrOutIntrMbox (raw SPUs raise a PPU interrupt).
    void (*out_intr_hook)(SPUThread& s) = nullptr;

    // run control
    bool halted = false;
    std::atomic<bool> stop_request{false};  // leave the run loop at the next branch
    SPUStopHandler stop_handler = nullptr;
    void* user = nullptr;   // owner (SPURS kernel)
    std::atomic<int> workload{-1};
    u64 insn_count = 0;
    u64 block_count = 0;
    u64 block_insns = 0;   // instructions in the recompiled blocks entered (TOD_SPU_PROFILE; a block can leave early)
    // Write generation of each 1 KiB of local store, so cached recompiled blocks notice new code.
    u32 page_gen[256]{};
    struct CodeCacheEntry* code_cache = nullptr;  // per-instruction-slot block cache (recompiler)
};

// Interpreter: run from s.pc until a stop handler returns false or the thread halts (returns
// false then). With one_block it returns true after the first control transfer or channel/stop
// instruction, so the recompiled-code dispatcher can take over again.
bool spu_interpret(SPUThread& s, bool one_block = false);
// Executes one block starting at s.pc (recompiled code if available, else interpreter).
void spu_run(SPUThread& s);

// Channel and MFC interface used by both execution engines.
u32 spu_read_channel(SPUThread& s, u32 ch);
void spu_write_channel(SPUThread& s, u32 ch, u32 value);
u32 spu_channel_count(SPUThread& s, u32 ch);
// Checks pending interrupt-enabled events; if one is pending and interrupts are on, takes the
// interrupt (SRR0 = next_pc, pc = 0) and returns true.
bool spu_check_interrupt(SPUThread& s, u32 next_pc);
u32 spu_event_pending(SPUThread& s);   // refreshes time-based events and returns event_stat
std::string spu_where(const SPUThread& s);

// Local store access with big-endian quadword layout.
static inline spu_reg ls_read128(const SPUThread& s, u32 lsa) {
    spu_reg r;
    const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(s.ls + (lsa & 0x3fff0)));
    r.m = _mm_shuffle_epi8(v, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    return r;
}
static inline void spu_ls_written(SPUThread& s, u32 lsa, u32 size) {
    if (!size) return;
    const u32 first = (lsa & kLsMask) >> 10, last = ((lsa & kLsMask) + size - 1) >> 10;
    for (u32 p = first; p <= last && p < 256; ++p) ++s.page_gen[p];
}
static inline void ls_write128(SPUThread& s, u32 lsa, const spu_reg& r) {
    ++s.page_gen[(lsa & 0x3fff0) >> 10];
    const __m128i v = _mm_shuffle_epi8(r.m, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    _mm_store_si128(reinterpret_cast<__m128i*>(s.ls + (lsa & 0x3fff0)), v);
}
static inline u32 ls_read32(const SPUThread& s, u32 lsa) {
    u32 v; std::memcpy(&v, s.ls + (lsa & 0x3fffc), 4); return __builtin_bswap32(v);
}
static inline void ls_write32(SPUThread& s, u32 lsa, u32 x) {
    ++s.page_gen[(lsa & 0x3fffc) >> 10];
    x = __builtin_bswap32(x); std::memcpy(s.ls + (lsa & 0x3fffc), &x, 4);
}
