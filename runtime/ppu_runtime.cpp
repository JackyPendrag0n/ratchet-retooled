#include <set>
#include <mutex>
#include <array>
#include <algorithm>
#include <vector>
// Core runtime for recompiled PPU code: function lookup, indirect dispatch,
// atomics and error paths. HLE (imports, syscalls) lives with the host program.
#include "ppu.h"
#include "ppu_tables.h"
#include "platform.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr u32 kCodeLo = 0x10000, kCodeHi = 0x840000;
std::vector<PPUFunc>& direct() {
    static std::vector<PPUFunc> t = [] {
        std::vector<PPUFunc> v((kCodeHi - kCodeLo) / 4, nullptr);
        for (unsigned i = 0; i < ppu_function_count; ++i)
            v[(ppu_function_table[i].address - kCodeLo) / 4] = ppu_function_table[i].fn;
        return v;
    }();
    return t;
}
constexpr u32 kThunkLo = 0x1000, kThunkMax = 1024;
PPUFunc g_thunks[kThunkMax];
std::atomic<u32> g_thunk_count{0};
std::string hex(u32 x) { char b[16]; std::snprintf(b, sizeof b, "0x%X", x); return b; }
}  // namespace

u32 ppu_register_thunk(PPUFunc fn) {
    const u32 i = g_thunk_count.fetch_add(1);
    if (i >= kThunkMax) throw std::runtime_error("too many host thunks");
    g_thunks[i] = fn;
    return kThunkLo + 4 * i;
}

PPUFunc ppu_lookup(u32 addr) {
    if (addr >= kThunkLo && addr < kThunkLo + 4 * g_thunk_count.load() && !(addr & 3)) return g_thunks[(addr - kThunkLo) / 4];
    if (addr < kCodeLo || addr >= kCodeHi || (addr & 3)) return nullptr;
    return direct()[(addr - kCodeLo) / 4];
}

void ppu_dispatch(PPUContext& c, u8* base, u32 target) {
    if (PPUFunc f = ppu_lookup(target)) { f(c, base); return; }
    ppu_bad_jump(c, c.cia, target);
}

void ppu_bad_jump(PPUContext& c, u32 site, u32 target) {
    c.cia = site;
    throw std::runtime_error("indirect branch to unknown target " + hex(target) + " from " + hex(site));
}

bool ppu_trace_on() { static const bool on = std::getenv("TOD_SLOTLOG") != nullptr; return on; }
unsigned long long ppu_trace_seq() { static std::atomic<unsigned long long> n{0}; return n.fetch_add(1); }
std::atomic<bool> ppu_watch_armed{false};
// TOD_SORTCHECK: the draw-record sort (sub_4BEC08, an SPU job) must only reorder the 16-byte
// records of the pass's pool. Snapshot the pool before it and compare after.
namespace {
thread_local std::vector<std::array<u8, 16>> t_sort_in;
thread_local u32 t_sort_pool = 0;
}
bool ppu_sortcheck_on() { static const bool on = std::getenv("TOD_SORTCHECK") != nullptr; return on; }
void ppu_watch_ctx(u8* base, u32 pp, u32 cx, u32 lr) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) >= 4) return;
    std::fprintf(stderr, "[T] command context pointer 0x%x -> context 0x%x {%08x %08x %08x %08x} from 0x%x\n", pp, cx,
                 ld32(base, cx), ld32(base, cx + 4), ld32(base, cx + 8), ld32(base, cx + 12), lr);
}
void ppu_sort_before(u8* base, u32 pass) {
    const u32 a = ld32(base, pass), b = ld32(base, pass + 4);
    t_sort_in.clear(); t_sort_pool = 0;
    if (b <= a || b - a > 0x100000) return;
    t_sort_pool = pass;
    static std::atomic<int> nb{0};
    for (u32 p = a; p < b; p += 16)
        if (ld16(base, p) == 6 && !ppu_valid_opd(base, ld32(base, p + 12)) && nb.fetch_add(1) < 6) {
            std::fprintf(stderr, "[T] bucket-6 record without callback before the sort at 0x%x (pool 0x%x-0x%x)\n", p, a, b);
            break;
        }
    for (u32 p = a; p < b; p += 16) { std::array<u8, 16> r; std::memcpy(r.data(), base + p, 16); t_sort_in.push_back(r); }
}
void ppu_sort_after(u8* base, u32 pass) {
    {
        // which passes dispatch bucket 6, and with which handler
        static std::mutex m; static std::set<u64> seen;
        const u32 h6 = ld32(base, pass + 0x1098);
        const u32 bt = ld32(base, 0x89FF20 + 0x6F6C);
        const u32 n6 = ld32(base, bt + 0x110 + 6 * 16 + 4);
        std::lock_guard lk(m);
        const u32 tid = plat::thread_id();
        const u64 key = u64(pass) << 32 | u64(tid & 0xffff) << 2 | (h6 ? 1u : 0u) << 1 | (n6 ? 1u : 0u);
        if (seen.insert(key).second && seen.size() < 40)
            std::fprintf(stderr, "[T] pass 0x%x bucket-6 handler 0x%x, bucket-6 records %u, scene 0x%x, host thread %u\n", pass, h6, n6, ld32(base, pass + 0x10), tid);
    }
    {
        // two passes in the dispatcher at once would share the global record pool and buckets
        static std::atomic<int> inside{0};
        (void)inside;
    }
    if (t_sort_pool != pass) return;
    t_sort_pool = 0;
    const u32 a = ld32(base, pass);
    std::vector<std::array<u8, 16>> out(t_sort_in.size());
    for (size_t i = 0; i < out.size(); ++i) std::memcpy(out[i].data(), base + a + 16 * i, 16);
    auto in = t_sort_in;
    std::sort(in.begin(), in.end()); auto o2 = out; std::sort(o2.begin(), o2.end());
    static std::atomic<int> reports{0}, checks{0};
    const int nchk = checks.fetch_add(1);
    if (in == o2) { if (nchk % 600 == 0) std::fprintf(stderr, "[T] sort check %d ok (%zu records)\n", nchk, in.size()); return; }
    if (reports.fetch_add(1) >= 6) return;
    std::vector<std::array<u8, 16>> extra, missing;
    std::set_difference(o2.begin(), o2.end(), in.begin(), in.end(), std::back_inserter(extra));
    std::set_difference(in.begin(), in.end(), o2.begin(), o2.end(), std::back_inserter(missing));
    std::fprintf(stderr, "[W] draw sort changed records: pool 0x%x, %zu records, %zu new, %zu lost\n", a, in.size(), extra.size(), missing.size());
    auto hex = [](const std::array<u8, 16>& r) { char h[40]; for (int i = 0; i < 16; ++i) std::snprintf(h + 2 * i + i / 4, 4, i % 4 == 3 ? "%02x " : "%02x", r[i]); return std::string(h); };
    for (size_t i = 0; i < extra.size() && i < 6; ++i) {
        size_t at = 0; while (at < out.size() && out[at] != extra[i]) ++at;
        std::fprintf(stderr, "[W]   new  at %4zu: %s\n", at, hex(extra[i]).c_str());
    }
    for (size_t i = 0; i < missing.size() && i < 6; ++i) std::fprintf(stderr, "[W]   lost: %s\n", hex(missing[i]).c_str());
}
u32 ppu_watch_ea() {
    static const u32 w = [] { const char* e = std::getenv("TOD_WATCH_EA"); return e ? u32(std::strtoul(e, nullptr, 16)) : 0u; }();
    return w;
}
void ppu_watch(u32 site, u32 lo, u32 len, u32 a, u32 b) {
    const u32 w = ppu_watch_ea();
    if (!w || (len && (w < lo || w - lo >= len))) return;
    if (len) ppu_watch_armed = true;
    else if (!ppu_watch_armed) return;
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 400) std::fprintf(stderr, "[T %llu] ppu %06x %08x %08x %08x\n", ppu_trace_seq(), site, lo, a, b);
}
void ppu_trace(u32 site, u32 a, u32 b) {
    if (!ppu_trace_on()) return;
    std::fprintf(stderr, "[T %llu] ppu %06x %08x %08x\n", ppu_trace_seq(), site, a, b);
}

PPULineLock& ppu_line_lock(u32 ea) {
    static PPULineLock locks[1024];
    return locks[(ea >> 7) & 1023];
}

// The game's raw SPUs share "special" commands that the PPU posts in one fixed slot (sub_4A34E0).
// Raw SPU 1 takes the command (lock word 0x22) and raw SPU 0 joins as a helper; raw SPU 1 drops
// bit 0x02 when its share is done and marks the slot done, and the helper clears the busy bit
// 0x20 when it finishes. The PPU then rewrites the slot for the next command, but the helper
// re-reads the slot header part way through its share: when it runs late it picks up the next
// command, calls through a null table entry and stops at LS 0. Raw SPU 1 then waits on the busy
// bit forever (the freeze after a level loads) and the PPU uses the half-done job's output (the
// crashes in the draw lists). On hardware the helper always finishes first; here the PPU waits
// while the lock shows only the helper busy (0x20 without the normal-job bit 0x08), with a time
// limit in case no helper joined.
void ppu_wait_slot_helper(u8* base) {
    constexpr u32 kQueueLock = 0x10612e80;
    constexpr u32 kQueueFlags = 0x10612eb0;
    // Also wait while the master still owns the previous special command (flag 4, cleared at
    // 0x2b3c after it PUTs the done header): a helper that finished first lets the PPU post
    // early, and the late done header then overwrites the new command.
    auto helper_busy = [&] { return (ld32(base, kQueueLock) & 0x2a) == 0x20 || (ld32(base, kQueueFlags) & 4); };
    if (!helper_busy()) return;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (helper_busy()) {
        if (std::chrono::steady_clock::now() > until) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 4) std::fprintf(stderr, "[W] SPU job slot still busy; posting anyway\n");
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
}

void ppu_note_rec(u32 lr, u32 a, u32 b, u32 c) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8) std::fprintf(stderr, "[W] draw record for bucket 6 without a callback, from 0x%X: %08X %08X %08X\n", lr, a, b, c);
}

void ppu_note_vals(u32 site, u32 a, u32 b, u32 c) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 6) std::fprintf(stderr, "[W]   at 0x%X: %08X %08X %08X\n", site, a, b, c);
}

void ppu_note_mem(u32 site, const char* what, u8* base, u32 addr) {
    static std::atomic<int> n{0};
    const int k = n.fetch_add(1);
    if (k >= 16) return;
    std::fprintf(stderr, "[W] guest %s at 0x%X (data 0x%08X)\n", what, site, addr);
    if (k >= 6 || addr < 0x10000 || addr > 0xDFFFFFC0u) return;
    for (u32 a = (addr & ~15u) - 32; a < (addr & ~15u) + 48; a += 16)
        std::fprintf(stderr, "[W]   %08X: %08X %08X %08X %08X\n", a, ld32(base, a), ld32(base, a + 4), ld32(base, a + 8), ld32(base, a + 12));
}

void ppu_note(u32 site, const char* what) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 16) std::fprintf(stderr, "[W] guest %s at 0x%X\n", what, site);
}

bool ppu_valid_opd(u8* base, u32 desc) {
    if (desc & 3) return false;
    const bool in_image = (desc >= 0x840000 && desc + 8 <= 0x8AD4E8) || (desc >= 0x10000000 && desc + 8 <= 0x1088C570);
    return in_image && ppu_lookup(ld32(base, desc)) != nullptr;
}

// Defaults for programs without the host (the startup check); host/hle_io.cpp overrides them.
__attribute__((weak)) void ppu_movie_opened(u32) {}
__attribute__((weak)) bool ppu_movie_skip(u32) { return false; }
__attribute__((weak)) bool ppu_host_hook(PPUContext&, u8*, u32) { return false; }

void ppu_unimplemented(PPUContext& c, u8*, u32 addr, u32 word) {
    c.cia = addr;
    throw std::runtime_error("unimplemented instruction " + hex(word) + " at " + hex(addr));
}

void ppu_trap(PPUContext& c, u8*, u32 addr) {
    c.cia = addr;
    throw std::runtime_error("guest trap at " + hex(addr));
}

u64 ppu_timebase() {
    // Cell timebase runs at 79.8 MHz.
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return static_cast<u64>(static_cast<unsigned __int128>(ns) * 79800000 / 1000000000);
}

bool ppu_store_conditional32(PPUContext& c, u8* base, u32 ea, u32 value) {
    if (!c.reserve || c.reserve_addr != ea) { c.reserve = false; return false; }
    c.reserve = false;
    auto* p = reinterpret_cast<std::atomic<u32>*>(base + ea);
    u32 expected = __builtin_bswap32(static_cast<u32>(c.reserve_value));
    PPULineLock& l = ppu_line_lock(ea);
    l.lock();
    const bool ok = p->compare_exchange_strong(expected, __builtin_bswap32(value));
    l.unlock();
    return ok;
}

bool ppu_store_conditional64(PPUContext& c, u8* base, u32 ea, u64 value) {
    if (!c.reserve || c.reserve_addr != ea) { c.reserve = false; return false; }
    c.reserve = false;
    auto* p = reinterpret_cast<std::atomic<u64>*>(base + ea);
    u64 expected = __builtin_bswap64(c.reserve_value);
    PPULineLock& l = ppu_line_lock(ea);
    l.lock();
    const bool ok = p->compare_exchange_strong(expected, __builtin_bswap64(value));
    l.unlock();
    return ok;
}

// Defaults for builds without the raw SPU layer (startup_check), which never reach that range.
__attribute__((weak)) u32 ppu_mmio_ld32(u32 ea) { std::fprintf(stderr, "raw SPU access at 0x%x\n", ea); std::abort(); }
__attribute__((weak)) void ppu_mmio_st32(u32 ea, u32) { std::fprintf(stderr, "raw SPU access at 0x%x\n", ea); std::abort(); }
