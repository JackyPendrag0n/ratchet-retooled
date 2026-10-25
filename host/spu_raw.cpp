// Raw SPUs (sys_raw_spu_*): the game runs its collision code (igcollspu) on one. Each raw SPU's
// local store is guest memory at 0xE0000000 + id * 0x100000, so the PPU's direct LS reads and
// writes need nothing special. 32-bit accesses to the problem-state area (+0x40000..) arrive in
// ppu_mmio_ld32 / ppu_mmio_st32: MFC proxy DMA, mailboxes, run control, NPC, signals.
#include "guest.h"
#include "spu_ops.h"

#include <chrono>
#include <condition_variable>

namespace {
constexpr u32 kRawBase = 0xE0000000, kRawStride = 0x100000, kRawCount = 5;
enum : u32 {
    SPU_STATUS_RUNNING = 0x1, SPU_STATUS_STOPPED_BY_STOP = 0x2, SPU_STATUS_STOPPED_BY_HALT = 0x4,
};

struct RawSpu {
    u32 id = 0;
    SPUThread spu;
    std::mutex lock;
    std::condition_variable cv;
    bool run_requested = false;
    std::atomic<u32> status{0};
    u32 npc = 0;
    // MFC proxy parameters
    u32 lsa = 0, eah = 0, eal = 0, size_tag = 0, cmd_status = 0, query_mask = 0, query_type = 0;
    // interrupt class 2 (mailbox / stop) status bits for sys_raw_spu_get_int_stat
    std::atomic<u64> int_stat2{0};
    u64 int_mask[3]{};
    std::thread thread;
};
RawSpu* g_raw[kRawCount];
std::mutex g_raw_lock;

bool raw_stop(SPUThread& s, u32 code) {
    RawSpu& r = *static_cast<RawSpu*>(s.user);
    // Stop-and-signal: the SPU stops with the code in the status register, NPC after the stop.
    r.npc = s.pc + 4;
    r.status = (code << 16) | SPU_STATUS_STOPPED_BY_STOP;
    r.int_stat2.fetch_or(2);  // stop-and-signal interrupt
    static std::atomic<int> logged{0};
    if (logged++ < 8) LOG_NOTE("raw SPU %u: stop 0x%x at 0x%05x", r.id, code, s.pc);
    else LOG_TRACE("raw SPU %u: stop 0x%x at 0x%05x", r.id, code, s.pc);
    return false;
}

void raw_intr_mbox(SPUThread& s) {
    RawSpu& r = *static_cast<RawSpu*>(s.user);
    r.int_stat2.fetch_or(1);  // mailbox interrupt
}

void raw_thread(RawSpu* r) {
    _mm_setcsr(_mm_getcsr() | 0x8040);
    SPUThread& s = r->spu;
    for (;;) {
        {
            std::unique_lock lk(r->lock);
            r->cv.wait(lk, [&] { return r->run_requested; });
            r->run_requested = false;
            s.stop_request = false;
            s.halted = false;
            s.pc = r->npc & 0x3fffc;
            // The PPU may have stored new code straight into local store with any access size,
            // which does not bump page generations; recheck all code on each start.
            for (auto& g : s.page_gen) ++g;
            s.int_enabled = r->npc & 1;
            r->status = SPU_STATUS_RUNNING;
        }
        try {
            spu_run(s);
            if (s.halted || s.stop_request) {  // stopped through SPU_RunCntl
                r->npc = s.pc;
                r->status = 0;
            }
        } catch (const std::exception& e) {
            LOG_ERR("raw SPU %u stopped: %s; %s", r->id, e.what(), spu_where(s).c_str());
            r->npc = s.pc;
            r->status = SPU_STATUS_STOPPED_BY_HALT;
        }
    }
}

RawSpu* raw_get(u32 ea, u32& off) {
    const u32 id = (ea - kRawBase) / kRawStride;
    off = (ea - kRawBase) % kRawStride;
    return id < kRawCount ? g_raw[id] : nullptr;
}

void proxy_command(RawSpu& r, u32 cmd) {
    SPUThread& s = r.spu;
    const u32 size = r.size_tag >> 16;
    const u32 lsa = r.lsa & kLsMask;
    switch (cmd & 0xff) {
    case 0x20: case 0x21: case 0x22:  // PUT: LS -> main memory
        if (mem::is_mapped(r.eal, size) && lsa + size <= kLsSize) std::memcpy(mem::base + r.eal, s.ls + lsa, size);
        else LOG_ERR("raw SPU %u: bad proxy PUT lsa 0x%x ea 0x%x size 0x%x", r.id, lsa, r.eal, size);
        break;
    case 0x40: case 0x41: case 0x42:  // GET: main memory -> LS
        if (mem::is_mapped(r.eal, size) && lsa + size <= kLsSize) { std::memcpy(s.ls + lsa, mem::base + r.eal, size); spu_ls_written(s, lsa, size); }
        else LOG_ERR("raw SPU %u: bad proxy GET lsa 0x%x ea 0x%x size 0x%x", r.id, lsa, r.eal, size);
        break;
    default:
        LOG_ERR("raw SPU %u: unsupported proxy command 0x%x", r.id, cmd);
    }
    r.cmd_status = 0;
}
}  // namespace

void report_raw_spus() {
    for (u32 i = 0; i < kRawCount; ++i) {
        RawSpu* r = g_raw[i];
        if (!r) continue;
        SPUThread& s = r->spu;
        std::fprintf(stderr, "[E]   raw SPU %u: status 0x%x npc 0x%x, %s, %llu instructions, %llu blocks, out_mbox %s, in_mbox %u, int_stat 0x%llx, "
                     "events 0x%x/mask 0x%x, reservation 0x%x %s, signals 0x%x 0x%x\n",
                     i, r->status.load(), r->npc, spu_where(s).c_str(), (unsigned long long)s.insn_count,
                     (unsigned long long)s.block_count, s.out_mbox_valid ? "full" : "empty", s.in_mbox_count,
                     (unsigned long long)r->int_stat2.load(), s.event_stat.load(), s.event_mask, s.raddr,
                     s.rvalid ? "held" : "lost", s.signal1, s.signal2);
    }
}

u32 ppu_mmio_ld32(u32 ea) {
    u32 off;
    RawSpu* r = raw_get(ea, off);
    if (!r || off < 0x40000) {
        if (!mem::is_mapped(ea, 4)) { LOG_ERR("MMIO read of unmapped 0x%08x", ea); return 0; }
        u32 v; std::memcpy(&v, mem::base + ea, 4); return __builtin_bswap32(v);
    }
    SPUThread& s = r->spu;
    switch (off) {
    case 0x43014: return r->cmd_status;                 // MFC_Class_CMD (command status)
    case 0x43104: return 0x80000000u | 16;              // MFC_QStatus: empty, 16 free entries
    case 0x4322c: return r->query_mask;                 // Prxy_TagStatus: transfers finish at once
    case 0x44004: {                                     // SPU_Out_MBox
        std::lock_guard lk(s.mbox_lock);
        const u32 v = s.out_mbox;
        s.out_mbox_valid = false;
        return v;
    }
    case 0x44014: {                                     // SPU_MBox_Status
        std::lock_guard lk(s.mbox_lock);
        return (s.out_mbox_valid ? 1u : 0u) | ((4 - s.in_mbox_count) << 8) | ((s.out_intr_mbox_valid ? 1u : 0u) << 16);
    }
    case 0x4401c: return r->status & SPU_STATUS_RUNNING;   // SPU_RunCntl
    case 0x44024: return r->status;                     // SPU_Status
    case 0x44034: return r->npc;                        // SPU_NPC
    case 0x5400c: return s.signal1;
    case 0x5c00c: return s.signal2;
    default:
        LOG_ERR("raw SPU %u: read of problem-state register +0x%x", r->id, off);
        return 0;
    }
}

void ppu_mmio_st32(u32 ea, u32 v) {
    u32 off;
    RawSpu* r = raw_get(ea, off);
    if (!r || off < 0x40000) {
        if (!mem::is_mapped(ea, 4)) { LOG_ERR("MMIO write of unmapped 0x%08x = 0x%x", ea, v); return; }
        if (r) spu_ls_written(r->spu, off, 4);
        v = __builtin_bswap32(v); std::memcpy(mem::base + ea, &v, 4); return;
    }
    SPUThread& s = r->spu;
    switch (off) {
    case 0x43004: r->lsa = v; break;
    case 0x43008: r->eah = v; break;
    case 0x4300c: r->eal = v; break;
    case 0x43010: r->size_tag = v; break;
    case 0x43014: proxy_command(*r, v); break;
    case 0x43204: r->query_type = v; break;
    case 0x4321c: r->query_mask = v; break;
    case 0x4400c: {                                     // SPU_In_MBox
        std::lock_guard lk(s.mbox_lock);
        if (s.in_mbox_count < 4) s.in_mbox[s.in_mbox_count++] = v;
        else s.in_mbox[3] = v;  // full: hardware overwrites the last entry
        break;
    }
    case 0x4401c:                                       // SPU_RunCntl
        if (v & 1) {
            std::lock_guard lk(r->lock);
            static std::atomic<int> logged{0};
            if (logged++ < 8) LOG_NOTE("raw SPU %u: run from 0x%05x", r->id, r->npc);
            if (!(r->status & SPU_STATUS_RUNNING)) { r->run_requested = true; r->status = SPU_STATUS_RUNNING; r->cv.notify_all(); }
        } else {
            s.stop_request = true;
            for (int i = 0; i < 1000 && (r->status & SPU_STATUS_RUNNING); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        break;
    case 0x44034: r->npc = v; break;                    // SPU_NPC
    case 0x5400c: __atomic_store_n(&s.signal1, v, __ATOMIC_RELEASE); break;
    case 0x5c00c: __atomic_store_n(&s.signal2, v, __ATOMIC_RELEASE); break;
    default:
        LOG_ERR("raw SPU %u: write of problem-state register +0x%x = 0x%x", r->id, off, v);
    }
}

// sys_raw_spu_* syscalls. Returns false for numbers it does not handle.
bool raw_spu_syscall(PPUContext& c, u32 num) {
    const u32 a0 = u32(ARG(0)), a1 = u32(ARG(1)), a2 = u32(ARG(2));
    switch (num) {
    case 160: {  // sys_raw_spu_create(id*, attr)
        std::lock_guard lk(g_raw_lock);
        u32 id = 0;
        while (id < kRawCount && g_raw[id]) ++id;
        if (id == kRawCount) { RET(CELL_EAGAIN); return true; }
        auto* r = new RawSpu();
        r->id = id;
        mem::map(kRawBase + id * kRawStride, kRawStride);
        r->spu.ls = mem::base + kRawBase + id * kRawStride;
        r->spu.index = 8 + int(id);
        r->spu.name = "raw SPU " + std::to_string(id);
        r->spu.user = r;
        r->spu.stop_handler = raw_stop;
        r->spu.out_intr_hook = raw_intr_mbox;
        g_raw[id] = r;
        r->thread = std::thread(raw_thread, r);
        st32(mem::base, a0, id);
        LOG_NOTE("sys_raw_spu_create -> raw SPU %u (LS at 0x%08x)", id, kRawBase + id * kRawStride);
        RET(CELL_OK);
        return true;
    }
    case 151: {  // sys_raw_spu_set_int_mask(id, class, mask)
        if (a0 >= kRawCount || !g_raw[a0] || a1 > 2) { RET(CELL_ESRCH); return true; }
        g_raw[a0]->int_mask[a1] = ARG(2);
        RET(CELL_OK);
        return true;
    }
    case 152: {  // sys_raw_spu_get_int_mask(id, class, mask*)
        if (a0 >= kRawCount || !g_raw[a0] || a1 > 2) { RET(CELL_ESRCH); return true; }
        st64(mem::base, a2, g_raw[a0]->int_mask[a1]);
        RET(CELL_OK);
        return true;
    }
    case 153: {  // sys_raw_spu_set_int_stat(id, class, stat): clears bits
        if (a0 >= kRawCount || !g_raw[a0] || a1 > 2) { RET(CELL_ESRCH); return true; }
        if (a1 == 2) g_raw[a0]->int_stat2.fetch_and(~ARG(2));
        RET(CELL_OK);
        return true;
    }
    case 154: {  // sys_raw_spu_get_int_stat(id, class, stat*)
        if (a0 >= kRawCount || !g_raw[a0] || a1 > 2) { RET(CELL_ESRCH); return true; }
        st64(mem::base, a2, a1 == 2 ? g_raw[a0]->int_stat2.load() : 0);
        RET(CELL_OK);
        return true;
    }
    case 163: {  // sys_raw_spu_read_puint_mb(id, value*)
        if (a0 >= kRawCount || !g_raw[a0]) { RET(CELL_ESRCH); return true; }
        SPUThread& s = g_raw[a0]->spu;
        std::lock_guard lk(s.mbox_lock);
        st32(mem::base, a1, s.out_intr_mbox);
        s.out_intr_mbox_valid = false;
        RET(CELL_OK);
        return true;
    }
    default:
        return false;
    }
}
