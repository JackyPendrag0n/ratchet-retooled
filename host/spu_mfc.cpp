// SPU channels and the MFC: DMA (single, list with stall-and-notify), lock-line atomics,
// tag status, events, decrementer. Transfers complete immediately; only list stalls defer work.
#include "guest.h"
#include "spu_core.h"

#include <atomic>
#include <cstdlib>
#include <set>

#include <chrono>
#include <thread>

namespace {
enum : u32 {
    SPU_RdEventStat = 0, SPU_WrEventMask = 1, SPU_WrEventAck = 2, SPU_RdSigNotify1 = 3, SPU_RdSigNotify2 = 4,
    SPU_WrDec = 7, SPU_RdDec = 8, MFC_WrMSSyncReq = 9, SPU_RdEventMask = 11, MFC_RdTagMask = 12,
    SPU_RdMachStat = 13, SPU_WrSRR0 = 14, SPU_RdSRR0 = 15, MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18,
    MFC_Size = 19, MFC_TagID = 20, MFC_Cmd = 21, MFC_WrTagMask = 22, MFC_WrTagUpdate = 23, MFC_RdTagStat = 24,
    MFC_RdListStallStat = 25, MFC_WrListStallAck = 26, MFC_RdAtomicStat = 27, SPU_WrOutMbox = 28,
    SPU_RdInMbox = 29, SPU_WrOutIntrMbox = 30,
};

constexpr u32 kNoUpdate = ~0u;

PPULineLock& line_lock(u32 ea) { return ppu_line_lock(ea); }

bool ea_ok(SPUThread& s, u32 ea, u32 size, const char* what) {
    if (size == 0 || mem::is_mapped(ea, size)) return true;
    LOG_ERR("SPU%d: %s of %u bytes at unmapped ea 0x%08x (pc 0x%05x)", s.index, what, size, ea, s.pc);
    return false;
}

struct GetRec { u32 ea, lsa, size, pc; };
thread_local GetRec g_get_ring[32];
thread_local u32 g_get_pos = 0;

void dma(SPUThread& s, bool get, u32 lsa, u32 ea, u32 size) {
    if (!size) return;
    if (size > 0x4000) { LOG_ERR("SPU%d: DMA size 0x%x too large (pc 0x%05x)", s.index, size, s.pc); return; }
    lsa &= kLsMask;
    if (lsa + size > kLsSize) { LOG_ERR("SPU%d: DMA past local store (lsa 0x%x size 0x%x)", s.index, lsa, size); return; }
    if (!ea_ok(s, ea, size, get ? "GET" : "PUT")) return;
    if (get) {
        std::memcpy(s.ls + lsa, mem::base + ea, size); spu_ls_written(s, lsa, size);
        g_get_ring[g_get_pos++ & 31] = {ea, lsa, size, s.pc};
        static const u32 watch_g = [] { const char* e = std::getenv("TOD_WATCH_EA"); return e ? u32(std::strtoul(e, nullptr, 16)) : 0u; }();
        if (watch_g && ea <= watch_g && ea + size > watch_g) {
            static std::atomic<int> ng{0};
            if (ng.fetch_add(1) < 60)
                std::fprintf(stderr, "[T %llu] SPU%d wkl %d GET ea %08x size %x lsa %05x pc %05x\n", ppu_trace_seq(), s.index, s.workload.load(), ea, size, lsa, s.pc);
        }
        // a job slot fetched into the job buffer: its command (u16 at byte 12) indexes a table of 11
        if (lsa == 0x23000 && size == 0xa0) {
            const u32 cmd = u32(s.ls[lsa + 12]) << 8 | s.ls[lsa + 13];
            static std::atomic<int> reported{0};
            if (cmd > 10 && cmd < 0xf000 && reported.fetch_add(1) < 8) {
                char hex[3 * 32 + 1];
                for (int i = 0; i < 32; ++i) std::snprintf(hex + 3 * i, 4, "%02x ", s.ls[lsa + i]);
                LOG_ERR("SPU%d: job slot at ea 0x%08x has command %u (pc 0x%05x): %s", s.index, ea, cmd, s.pc, hex);
            }
        }
    }
    else {
        std::memcpy(mem::base + ea, s.ls + lsa, size);
        if (ppu_sortcheck_on()) {
            // bucket-6 draw records (u16 6 at +0, word 0xC at +8)
            for (u32 o = (16 - (ea & 15)) & 15; o + 16 <= size; o += 16) {
                const u8* r = mem::base + ea + o;
                if (r[0] == 0 && r[1] == 6 && r[8] == 0 && r[9] == 0 && r[10] == 0 && r[11] == 0xc) {
                    static std::atomic<int> nb{0};
                    if (nb.fetch_add(1) < 12)
                        std::fprintf(stderr, "[T] SPU%d wkl %d PUT of a bucket-6 record at ea %08x (dma ea %08x size %x lsa %05x pc %05x)\n", s.index, s.workload.load(), ea + o, ea, size, lsa, s.pc);
                    break;
                }
            }
        }
        static const u32 watch = [] { const char* e = std::getenv("TOD_WATCH_EA"); return e ? u32(std::strtoul(e, nullptr, 16)) : 0u; }();
        if (watch && ea <= watch && ea + size > watch) {
            ppu_watch_armed = true;
            static std::atomic<int> n{0};
            if (n.fetch_add(1) < 60)
                std::fprintf(stderr, "[T %llu] SPU%d wkl %d PUT ea %08x size %x lsa %05x pc %05x\n", ppu_trace_seq(), s.index, s.workload.load(), ea, size, lsa, s.pc);
            if (n.load() < 2)
                for (u32 i = 0; i < 32; ++i) {
                    const GetRec& r = g_get_ring[(g_get_pos + i) & 31];
                    if (r.size) std::fprintf(stderr, "[T]     earlier GET ea %08x lsa %05x size %x pc %05x\n", r.ea, r.lsa, r.size, r.pc);
                }
        }
    }
    if (ppu_trace_on() && ea < 0x10612d90 && ea + size > 0x10612d80) {
        const u8* h = mem::base + 0x10612d88;
        std::fprintf(stderr, "[T %llu] SPU%d %s ea %08x size %x pc %05x: %02x%02x%02x%02x %02x%02x%02x%02x\n", ppu_trace_seq(), s.index,
                     get ? "GET" : "PUT", ea, size, s.pc, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    }
    static const bool log = std::getenv("TOD_SPU_DMALOG") != nullptr;
    if (log) {
        // first sighting of each (SPU, direction, EA page): where SPU programs read code and write results
        static std::mutex m;
        static std::set<u64> seen;
        const u64 key = u64(s.index) << 40 | u64(get) << 39 | (ea >> 12);
        std::lock_guard lk(m);
        if (seen.insert(key).second)
            LOG_NOTE("SPU%d %s ea 0x%08x lsa 0x%05x size 0x%x (pc 0x%05x)", s.index, get ? "GET" : "PUT", ea, lsa, size, s.pc);
    }
}

u32 completed_tags(const SPUThread& s) { return ~s.stall_stat; }

void update_tag_stat(SPUThread& s) {
    if (s.tag_update == kNoUpdate) return;
    const u32 done = completed_tags(s) & s.tag_mask;
    bool ready = false;
    if (s.tag_update == 0) ready = true;
    else if (s.tag_update == 1) ready = done != 0 || s.tag_mask == 0;
    else ready = (s.tag_mask & ~done) == 0;
    if (ready) {
        s.tag_stat = done;
        s.tag_stat_valid = true;
        s.tag_update = kNoUpdate;
        s.event_stat.fetch_or(SPU_EVENT_TG);
    }
}

// Continues a DMA list from its saved position until it ends or reaches a stall-and-notify element.
void run_list(SPUThread& s, u32 tag) {
    SPUThread::StalledList& l = s.stalled[tag];
    const bool get = (l.cmd & 0xf0) == 0x40;
    while (l.list_size >= 8) {
        const u32 w0 = ls_read32(s, l.list_lsa), w1 = ls_read32(s, l.list_lsa + 4);
        const bool stall = w0 & 0x80000000u;
        const u32 size = w0 & 0x7fff;
        dma(s, get, l.lsa, w1, size);
        l.lsa = (l.lsa + ((size + 15) & ~15u)) & kLsMask;
        l.list_lsa += 8;
        l.list_size -= 8;
        if (stall) {
            l.active = true;
            s.stall_stat |= 1u << tag;
            s.stall_notify |= 1u << tag;
            s.event_stat.fetch_or(SPU_EVENT_SN);
            return;
        }
    }
    l.active = false;
}

// The lock-line reservation is lost when anyone else writes the line. Hardware raises the LR
// event at that moment whatever the event mask; here it is noticed whenever the SPU looks.
void reservation_check(SPUThread& s) {
    if (s.rvalid && std::memcmp(mem::base + s.raddr, s.rdata, 128) != 0) {
        s.rvalid = false;
        s.event_stat.fetch_or(SPU_EVENT_LR);
    }
}

void getllar(SPUThread& s, u32 lsa, u32 ea) {
    ea &= ~127u;
    if (!ea_ok(s, ea, 128, "GETLLAR")) return;
    reservation_check(s);  // a reservation lost before this one still raises its event
    {
        std::lock_guard lk(line_lock(ea));
        std::memcpy(s.rdata, mem::base + ea, 128);
    }
    std::memcpy(s.ls + (lsa & 0x3ff80), s.rdata, 128);
    spu_ls_written(s, lsa & 0x3ff80, 128);
    s.raddr = ea;
    s.rvalid = true;
    s.atomic_stat = 4;
    s.atomic_stat_valid = true;
}

// Writes a 128-byte line if it still holds the reserved data. Called with the line's lock held,
// which PPU stwcx/stdcx and host-side atomics also take, so the check and the write are one step
// for every atomic writer. Changed doublewords go in with CAS so a plain store that lands between
// the check and the write (a writer that takes no lock) fails the command instead of being lost.
bool cas_line(u8* line, const u8* expect, const u8* desired) {
    u64* p = reinterpret_cast<u64*>(line);
    const u64* e = reinterpret_cast<const u64*>(expect);
    const u64* d = reinterpret_cast<const u64*>(desired);
    for (int i = 0; i < 16; ++i)
        if (__atomic_load_n(&p[i], __ATOMIC_ACQUIRE) != e[i]) return false;
    for (int i = 0; i < 16; ++i) {
        if (e[i] == d[i]) continue;
        u64 x = e[i];
        if (__atomic_compare_exchange_n(&p[i], &x, d[i], false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        for (int j = 0; j < i; ++j)
            if (e[j] != d[j]) { u64 y = d[j]; __atomic_compare_exchange_n(&p[j], &y, e[j], false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
        return false;
    }
    return true;
}

void putllc(SPUThread& s, u32 lsa, u32 ea) {
    ea &= ~127u;
    bool ok = false;
    if (s.rvalid && s.raddr == ea && ea_ok(s, ea, 128, "PUTLLC")) {
        std::lock_guard lk(line_lock(ea));
        ok = cas_line(mem::base + ea, s.rdata, s.ls + (lsa & 0x3ff80));
    }
    // On hardware a PUTLLC only fails once the reservation is lost, which raises LR. Here a failed
    // CAS may also have raced with a write that was undone, so raise it on every failure.
    if (!ok) s.event_stat.fetch_or(SPU_EVENT_LR);
    if (ok && ppu_trace_on() && ea == 0x10612e80)
        std::fprintf(stderr, "[T %llu] SPU%d PUTLLC pc %05x lock %08x %08x flags %08x\n", ppu_trace_seq(), s.index, s.pc,
                     ld32(mem::base, ea), ld32(mem::base, ea + 12), ld32(mem::base, ea + 0x30));
    s.rvalid = false;
    s.atomic_stat = ok ? 0 : 1;
    s.atomic_stat_valid = true;
}

void putlluc(SPUThread& s, u32 lsa, u32 ea) {
    ea &= ~127u;
    if (ea_ok(s, ea, 128, "PUTLLUC")) {
        std::lock_guard lk(line_lock(ea));
        u64* p = reinterpret_cast<u64*>(mem::base + ea);
        const u64* d = reinterpret_cast<const u64*>(s.ls + (lsa & 0x3ff80));
        for (int i = 0; i < 16; ++i) __atomic_store_n(&p[i], d[i], __ATOMIC_RELEASE);
    }
    if (s.rvalid && s.raddr == ea) s.rvalid = false;
    s.atomic_stat = 2;
    s.atomic_stat_valid = true;
}

void mfc_command(SPUThread& s, u32 cmd) {
    const u32 tag = s.mfc_tag & 31;
    const u32 lsa = s.mfc_lsa & kLsMask, ea = s.mfc_eal, size = s.mfc_size;
    switch (cmd & 0xff) {
    case 0x20: case 0x21: case 0x22:  // PUT, PUTB, PUTF
    case 0x40: case 0x41: case 0x42:  // GET, GETB, GETF
        dma(s, (cmd & 0xf0) == 0x40, lsa, ea, size);
        break;
    case 0x24: case 0x25: case 0x26:  // PUTL*
    case 0x44: case 0x45: case 0x46: {  // GETL*
        SPUThread::StalledList& l = s.stalled[tag];
        if (l.active) LOG_ERR("SPU%d: new list on tag %u while a list is stalled", s.index, tag);
        l = {true, cmd & 0xff, tag, lsa, ea & 0x3fff8, size, s.mfc_eah};
        run_list(s, tag);
        break;
    }
    case 0xd0: getllar(s, lsa, ea); break;
    case 0xb4: putllc(s, lsa, ea); break;
    case 0xb0: case 0xb8: putlluc(s, lsa, ea); break;
    case 0xc0: case 0xc8: case 0xcc: break;  // barrier, eieio, sync
    default:
        LOG_ERR("SPU%d: unsupported MFC command 0x%x (lsa 0x%x ea 0x%x size 0x%x tag %u) at pc 0x%05x",
                s.index, cmd, lsa, ea, size, tag, s.pc);
    }
    update_tag_stat(s);
}

u64 dec_now(const SPUThread& s) { return ppu_timebase() - s.dec_start; }
}  // namespace

u32 spu_event_pending(SPUThread& s) {
    if (s.dec_tm_armed && dec_now(s) > s.dec_value) {
        s.dec_tm_armed = false;
        s.event_stat.fetch_or(SPU_EVENT_TM);
    }
    reservation_check(s);
    return s.event_stat.load();
}

u32 spu_channel_count(SPUThread& s, u32 ch) {
    switch (ch) {
    case SPU_RdEventStat: return (spu_event_pending(s) & s.event_mask) ? 1 : 0;
    case SPU_RdSigNotify1: return s.signal1 ? 1 : 0;
    case SPU_RdSigNotify2: return s.signal2 ? 1 : 0;
    case MFC_Cmd: return 16;
    case MFC_RdTagStat: update_tag_stat(s); return s.tag_stat_valid ? 1 : 0;
    case MFC_RdListStallStat: return s.stall_notify ? 1 : 0;
    case MFC_RdAtomicStat: return s.atomic_stat_valid ? 1 : 0;
    case SPU_WrOutMbox: { std::lock_guard lk(s.mbox_lock); return s.out_mbox_valid ? 0 : 1; }
    case SPU_WrOutIntrMbox: { std::lock_guard lk(s.mbox_lock); return s.out_intr_mbox_valid ? 0 : 1; }
    case SPU_RdInMbox: { std::lock_guard lk(s.mbox_lock); return s.in_mbox_count; }
    default: return 1;
    }
}

u32 spu_read_channel(SPUThread& s, u32 ch) {
    switch (ch) {
    case SPU_RdEventStat: {
        for (int spins = 0;; ++spins) {
            const u32 e = spu_event_pending(s) & s.event_mask;
            if (e) return e;
            if (!s.event_mask) { LOG_ERR("SPU%d: waiting on events with an empty mask at 0x%05x", s.index, s.pc); }
            if (s.stop_request.load(std::memory_order_relaxed)) { s.halted = true; return 0; }
            if (spins < 64) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(spins < 1000 ? 20 : 200));
        }
    }
    case SPU_RdSigNotify1: case SPU_RdSigNotify2: {
        u32& sig = ch == SPU_RdSigNotify1 ? s.signal1 : s.signal2;
        while (!__atomic_load_n(&sig, __ATOMIC_ACQUIRE)) std::this_thread::sleep_for(std::chrono::microseconds(50));
        return __atomic_exchange_n(&sig, 0, __ATOMIC_ACQ_REL);
    }
    case SPU_RdDec: return u32(s.dec_value - dec_now(s));
    case SPU_RdEventMask: return s.event_mask;
    case MFC_RdTagMask: return s.tag_mask;
    case SPU_RdMachStat: return s.int_enabled ? 1 : 0;
    case SPU_RdSRR0: return s.srr0;
    case MFC_RdTagStat:
        for (;;) {
            update_tag_stat(s);
            if (s.tag_stat_valid) { s.tag_stat_valid = false; return s.tag_stat; }
            if (s.tag_update == kNoUpdate) {
                LOG_ERR("SPU%d: MFC_RdTagStat without an update request at 0x%05x", s.index, s.pc);
                return completed_tags(s) & s.tag_mask;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));  // stalled list never acked
        }
    case MFC_RdListStallStat: {
        // Reading clears the notification; the tags stay stalled until acknowledged.
        const u32 v = s.stall_notify;
        if (!v) LOG_ERR("SPU%d: MFC_RdListStallStat with no stalled list at 0x%05x", s.index, s.pc);
        s.stall_notify = 0;
        return v;
    }
    case MFC_RdAtomicStat: s.atomic_stat_valid = false; return s.atomic_stat;
    case SPU_RdInMbox:
        for (int spins = 0;; ++spins) {
            {
                std::lock_guard lk(s.mbox_lock);
                if (s.in_mbox_count) {
                    const u32 v = s.in_mbox[0];
                    for (u32 i = 1; i < s.in_mbox_count; ++i) s.in_mbox[i - 1] = s.in_mbox[i];
                    --s.in_mbox_count;
                    return v;
                }
            }
            if (s.stop_request.load(std::memory_order_relaxed)) { s.halted = true; return 0; }
            if (spins < 64) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(spins < 2000 ? 20 : 200));
        }
    default:
        LOG_ERR("SPU%d: read of unsupported channel %u at 0x%05x", s.index, ch, s.pc);
        return 0;
    }
}

void spu_write_channel(SPUThread& s, u32 ch, u32 v) {
    switch (ch) {
    case SPU_WrEventMask: s.event_mask = v; break;
    case SPU_WrEventAck: s.event_stat.fetch_and(~v); break;
    case SPU_WrDec: s.dec_value = v; s.dec_start = ppu_timebase(); s.dec_tm_armed = true; s.event_stat.fetch_and(~SPU_EVENT_TM); break;
    case MFC_WrMSSyncReq: break;
    case SPU_WrSRR0: s.srr0 = v; break;
    case MFC_LSA: s.mfc_lsa = v; break;
    case MFC_EAH: s.mfc_eah = v; break;
    case MFC_EAL: s.mfc_eal = v; break;
    case MFC_Size: s.mfc_size = v & 0xffff; break;
    case MFC_TagID: s.mfc_tag = v & 0x1f; break;
    case MFC_Cmd: mfc_command(s, v); break;
    case MFC_WrTagMask: s.tag_mask = v; break;
    case MFC_WrTagUpdate:
        s.tag_stat_valid = false;
        s.tag_update = v & 3;
        update_tag_stat(s);
        break;
    case MFC_WrListStallAck: {
        const u32 tag = v & 31;
        s.stall_stat &= ~(1u << tag);
        if (s.stalled[tag].active) run_list(s, tag);
        update_tag_stat(s);
        break;
    }
    case SPU_WrOutMbox:
        for (;;) {
            { std::lock_guard lk(s.mbox_lock); if (!s.out_mbox_valid) { s.out_mbox = v; s.out_mbox_valid = true; break; } }
            if (s.stop_request.load(std::memory_order_relaxed)) { s.halted = true; break; }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        break;
    case SPU_WrOutIntrMbox:
        for (;;) {
            { std::lock_guard lk(s.mbox_lock); if (!s.out_intr_mbox_valid) { s.out_intr_mbox = v; s.out_intr_mbox_valid = true; break; } }
            if (s.stop_request.load(std::memory_order_relaxed)) { s.halted = true; break; }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        if (s.out_intr_hook) s.out_intr_hook(s);
        else LOG_ERR("SPU%d: interrupt mailbox write 0x%x at 0x%05x (not routed)", s.index, v, s.pc);
        break;
    default:
        LOG_ERR("SPU%d: write of unsupported channel %u (0x%x) at 0x%05x", s.index, ch, v, s.pc);
    }
}
