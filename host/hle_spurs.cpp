// cellSpurs. There is no SPU emulation: SPU tasks this game uses are recognised when they are
// created and their work runs natively on the host (see native jobs below). Everything else is
// accepted and ignored for now.
#include "guest.h"
#include "skins.h"
#include "spu_ops.h"

#include <condition_variable>
#include <cstring>
#include <map>
#include <zlib.h>

void report_raw_spus();  // spu_raw.cpp

namespace {
#define SPURS_STUB(fname, out_arg)                                                         \
    void fname(PPUContext& c) {                                                            \
        static std::atomic<u32> calls{0};                                                  \
        if (calls++ < 8)                                                                   \
            LOG_NOTE("[spurs stub] " #fname "(0x%llx, 0x%llx, 0x%llx)", (unsigned long long)ARG(0), \
                     (unsigned long long)ARG(1), (unsigned long long)ARG(2));              \
        if (out_arg >= 0 && ARG(out_arg)) st32(mem::base, u32(ARG(out_arg)), next_id());   \
        RET(CELL_OK);                                                                      \
    }
u32 g_id = 0x100;
u32 next_id() { return g_id++; }

// ------------------------------------------------------------------ event flags (host side)
enum : u32 { CELL_SPURS_TASK_ERROR_INVAL = 0x80410902, CELL_SPURS_TASK_ERROR_NULL_POINTER = 0x80410911,
             CELL_SPURS_TASK_ERROR_BUSY = 0x8041090A };
struct EventFlag { u16 events = 0; u32 clear_mode = 0; std::condition_variable cv; };
std::mutex g_ef_lock;
std::map<u32, std::unique_ptr<EventFlag>> g_event_flags;

EventFlag& event_flag(u32 addr) {
    auto& e = g_event_flags[addr];
    if (!e) e = std::make_unique<EventFlag>();
    return *e;
}
void event_flag_set(u32 addr, u16 bits) {
    std::lock_guard lk(g_ef_lock);
    EventFlag& e = event_flag(addr);
    e.events |= bits;
    e.cv.notify_all();
}

void _cellSpursEventFlagInitialize(PPUContext& c) {
    // (spurs, taskset, eventFlag, clearMode, direction)
    std::lock_guard lk(g_ef_lock);
    EventFlag& e = event_flag(u32(ARG(2)));
    e.events = 0;
    e.clear_mode = u32(ARG(3));
    std::memset(mem::base + u32(ARG(2)), 0, 128);
    RET(CELL_OK);
}
void cellSpursEventFlagClear(PPUContext& c) {
    std::lock_guard lk(g_ef_lock);
    event_flag(u32(ARG(0))).events &= ~u16(ARG(1));
    RET(CELL_OK);
}
void cellSpursEventFlagWait(PPUContext& c) {
    const u32 addr = u32(ARG(0)), mask_ea = u32(ARG(1)), mode = u32(ARG(2));  // mode 0 = AND, 1 = OR
    if (!addr || !mask_ea) { RET(CELL_SPURS_TASK_ERROR_NULL_POINTER); return; }
    if (mode > 1) { RET(CELL_SPURS_TASK_ERROR_INVAL); return; }
    const u16 mask = u16(ld16(mem::base, mask_ea));
    std::unique_lock lk(g_ef_lock);
    EventFlag& e = event_flag(addr);
    e.cv.wait(lk, [&] { const u16 r = e.events & mask; return mode ? r != 0 : r == mask; });
    const u16 got = e.events & mask;
    if (e.clear_mode == 0) e.events &= ~got;  // CELL_SPURS_EVENT_FLAG_CLEAR_AUTO
    st16(mem::base, mask_ea, got);
    RET(CELL_OK);
}

// ------------------------------------------------------------------ native SPU jobs
enum class NativeTask { none, edge_zlib_inflate };
std::mutex g_task_lock;
std::map<u32, NativeTask> g_taskset_kind;     // taskset -> job its task runs
struct LFQueue { u32 taskset = 0, entry_size = 0, depth = 0; };
std::map<u32, LFQueue> g_lfqueues;

NativeTask identify_task(u32 elf) {
    // Recognise the SPU ELF by the source names compiled into it.
    if (ld32(mem::base, elf) != 0x7f454c46) return NativeTask::none;
    const u8* p = mem::base + elf;
    static const char kZlib[] = "edgezlib_inflate_task.cpp";
    for (u32 i = 0; i < 0x40000; ++i)
        if (p[i] == 'e' && std::memcmp(p + i, kZlib, sizeof kZlib - 1) == 0) return NativeTask::edge_zlib_inflate;
    return NativeTask::none;
}

// One Edge zlib queue element: {eaSrc, eaDst, srcSize, dstBufferSize, eaCounter|inflate, eaEventFlag,
// eventFlagBits<<16, eaUncompressedSize}. Raw deflate (no zlib header).
void edge_zlib_inflate(u32 elem) {
    const u32 src = ld32(mem::base, elem), dst = ld32(mem::base, elem + 4);
    const u32 src_size = ld32(mem::base, elem + 8), dst_size = ld32(mem::base, elem + 12);
    const u32 counter_word = ld32(mem::base, elem + 16), ef = ld32(mem::base, elem + 20);
    const u16 bits = u16(ld32(mem::base, elem + 24) >> 16);
    const u32 out_size_ea = ld32(mem::base, elem + 28);
    const bool is_inflate = counter_word & 1;
    const u32 counter = counter_word & ~0xfu;
    u32 produced = 0;
    const bool sane = mem::is_mapped(src, std::max<u32>(src_size, 1)) && mem::is_mapped(dst, std::max<u32>(dst_size, 1)) &&
                      (!counter || mem::is_mapped(counter, 4));
    if (!sane) {
        // a malformed element would fault the host; report it and finish it empty so waiters go on
        static int reported = 0;
        if (reported++ < 20)
            LOG_ERR("edge zlib: element at 0x%x names unmapped memory (src 0x%x+%u dst 0x%x+%u counter 0x%x out 0x%x ef 0x%x)",
                    elem, src, src_size, dst, dst_size, counter_word, out_size_ea, ef);
        if (!counter || !mem::is_mapped(counter, 4)) { if (ef) event_flag_set(ef, bits); return; }
    } else if (is_inflate) {
        z_stream z{};
        inflateInit2(&z, -15);
        z.next_in = mem::base + src; z.avail_in = src_size;
        z.next_out = mem::base + dst; z.avail_out = dst_size;
        const int r = inflate(&z, Z_FINISH);
        produced = u32(z.total_out);
        inflateEnd(&z);
        if (r != Z_STREAM_END) LOG_ERR("edge zlib: inflate error %d (src 0x%x+%u -> dst 0x%x, %u bytes out)", r, src, src_size, dst, produced);
    } else {
        produced = std::min(src_size, dst_size);
        std::memmove(mem::base + dst, mem::base + src, produced);
    }
    if (sane && out_size_ea && mem::is_mapped(out_size_ea, 4)) st32(mem::base, out_size_ea, produced);  // sometimes a stale field
    bool done = true;
    if (counter) {
        u32* w = reinterpret_cast<u32*>(mem::base + counter);
        std::lock_guard line(ppu_line_lock(counter));
        u32 old = __atomic_load_n(w, __ATOMIC_ACQUIRE), next;
        do {
            if (__builtin_bswap32(old) == 0) { LOG_ERR("edge zlib: work counter at 0x%x already zero", counter); break; }
            next = __builtin_bswap32(__builtin_bswap32(old) - 1);
        } while (!__atomic_compare_exchange_n(w, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
        done = __builtin_bswap32(__atomic_load_n(w, __ATOMIC_ACQUIRE)) == 0;
    }
    if (done && ef) event_flag_set(ef, bits);
}

void cellSpursCreateTask(PPUContext& c) {
    // (taskset, *id, elf, context, size, lsPattern, argument)
    const u32 taskset = u32(ARG(0)), elf = u32(ARG(2));
    const NativeTask kind = identify_task(elf);
    {
        std::lock_guard lk(g_task_lock);
        if (kind != NativeTask::none) g_taskset_kind[taskset] = kind;
    }
    LOG_NOTE("cellSpursCreateTask(taskset=0x%x, elf=0x%x) -> %s", taskset, elf,
             kind == NativeTask::edge_zlib_inflate ? "native Edge zlib inflate" : "unknown SPU task (ignored)");
    if (ARG(1)) st32(mem::base, u32(ARG(1)), next_id());
    RET(CELL_OK);
}

void _cellSpursLFQueueInitialize(PPUContext& c) {
    // (taskset, queue, buffer, entrySize, depth, direction)
    std::lock_guard lk(g_task_lock);
    g_lfqueues[u32(ARG(1))] = LFQueue{u32(ARG(0)), u32(ARG(3)), u32(ARG(4))};
    RET(CELL_OK);
}

void _cellSpursLFQueuePushBody(PPUContext& c) {
    // (queue, data, isBlocking). The consumer task runs inline, so the queue never fills.
    const u32 queue = u32(ARG(0)), data = u32(ARG(1));
    NativeTask kind = NativeTask::none;
    {
        std::lock_guard lk(g_task_lock);
        auto q = g_lfqueues.find(queue);
        if (q != g_lfqueues.end()) { auto t = g_taskset_kind.find(q->second.taskset); if (t != g_taskset_kind.end()) kind = t->second; }
    }
    if (kind == NativeTask::edge_zlib_inflate) edge_zlib_inflate(data);
    else LOG_ERR("LFQueue push to 0x%x with no native consumer: element dropped", queue);
    RET(CELL_OK);
}

// ------------------------------------------------------------------ SPURS kernel (host side)
// Each SPURS instance gets host threads standing in for its SPUs. A thread picks a workload whose
// ready count (CellSpurs+0x00, the byte the policy modules update with lock-line atomics) is
// non-zero, loads the workload's SPU policy module at LS 0xA00 and runs it like the real kernel
// would, with the kernel context at LS 0x100. The policy module calls back into the kernel through
// two LS addresses, which hold stop instructions handled here.
constexpr u32 kKernelExit = 0x400, kKernelSelect = 0x410;
constexpr u32 kStopExit = 0x3f01, kStopSelect = 0x3f02;

struct Workload {
    bool active = false, shutdown = false;
    u32 pm = 0, pm_size = 0;
    u64 data = 0;
    u8 priority[8]{};
    u32 min = 0, max = 1;
    std::string name;
    int running = 0;
    u32 unique_id = 0;
};

struct Spurs {
    u32 ea = 0;
    u32 nspus = 0;
    Workload wkl[16];
    std::mutex lock;
    std::condition_variable cv;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<SPUThread>> spus;
    u32 next_unique = 1;
};
std::mutex g_spurs_lock;
std::map<u32, std::unique_ptr<Spurs>> g_spurs;

Spurs* spurs_get(u32 ea) {
    std::lock_guard lk(g_spurs_lock);
    auto it = g_spurs.find(ea);
    return it == g_spurs.end() ? nullptr : it->second.get();
}

u8 ready_count(Spurs& sp, u32 wid) { return __atomic_load_n(mem::base + sp.ea + wid, __ATOMIC_ACQUIRE); }

bool kernel_stop(SPUThread& s, u32 code) {
    Spurs& sp = *static_cast<Spurs*>(s.user);
    if (code == kStopExit) return false;
    if (code == kStopSelect) {
        // Poll: keep the current workload unless it was shut down.
        const u32 wid = ls_read32(s, 0x1dc);
        u32 result = wid;
        { std::lock_guard lk(sp.lock); if (wid < 16 && sp.wkl[wid].shutdown) result = 0xffffffffu; }
        s.gpr[3] = spu::pref_only(result);
        s.pc = s.gpr[0].w[3] & 0x3fffc;
        return true;
    }
    LOG_ERR("SPU%d: stop 0x%x in workload code at 0x%05x (ignored)", s.index, code, s.pc);
    return true;
}

void run_policy_module(Spurs& sp, SPUThread& s, u32 wid, const Workload& w) {
    std::memset(s.ls, 0, kLsSize);
    std::memcpy(s.ls + 0xa00, mem::base + w.pm, w.pm_size);
    ls_write32(s, kKernelExit, 0x00000000u | kStopExit);
    ls_write32(s, kKernelSelect, 0x00000000u | kStopSelect);
    // kernel context
    ls_write32(s, 0x1c0, 0); ls_write32(s, 0x1c4, sp.ea);
    ls_write32(s, 0x1c8, u32(s.index));
    ls_write32(s, 0x1cc, 0x1f);  // kernel DMA tag
    ls_write32(s, 0x1d0, 0); ls_write32(s, 0x1d4, w.pm);
    ls_write32(s, 0x1d8, w.unique_id);
    ls_write32(s, 0x1dc, wid);
    ls_write32(s, 0x1e0, kKernelExit);
    ls_write32(s, 0x1e4, kKernelSelect);
    for (auto& r : s.gpr) r.q = 0;
    s.gpr[0] = spu::pref_only(kKernelExit);
    s.gpr[1] = spu::pref_only(0x3ffd0);
    s.gpr[3] = spu::pref_only(0x100);
    s.gpr[4].d[1] = w.data;
    s.gpr[5] = spu::pref_only(0);
    s.int_enabled = false;
    s.event_mask = 0;
    s.event_stat = 0;
    s.tag_mask = 0; s.tag_update = ~0u; s.tag_stat_valid = false;
    s.stall_stat = s.stall_notify = 0;
    for (auto& l : s.stalled) l.active = false;
    s.rvalid = false;
    s.pc = 0xa00;
    spu_run(s);
}

void spu_kernel_thread(Spurs* sp, SPUThread* s) {
    _mm_setcsr(_mm_getcsr() | 0x8040);  // flush denormals like the SPU
    u32 rr = 0;
    for (;;) {
        int pick = -1;
        Workload snapshot;
        {
            std::unique_lock lk(sp->lock);
            for (u32 i = 0; i < 16 && pick < 0; ++i) {
                const u32 wid = (rr + i) & 15;
                Workload& w = sp->wkl[wid];
                if (!w.active || w.shutdown || w.running >= int(w.max)) continue;
                if (ready_count(*sp, wid) == 0) continue;
                pick = int(wid);
            }
            if (pick < 0) {
                sp->cv.wait_for(lk, std::chrono::microseconds(500));
                continue;
            }
            rr = u32(pick) + 1;
            sp->wkl[pick].running++;
            snapshot = sp->wkl[pick];
        }
        s->workload = pick;
        try {
            run_policy_module(*sp, *s, u32(pick), snapshot);
        } catch (const std::exception& e) {
            LOG_ERR("SPU%d: workload %d (%s) stopped: %s; %s", s->index, pick, snapshot.name.c_str(), e.what(), spu_where(*s).c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        s->workload = -1;
        {
            std::lock_guard lk(sp->lock);
            sp->wkl[pick].running--;
        }
        sp->cv.notify_all();
    }
}

void report_spus() {
    report_raw_spus();
    std::lock_guard lk(g_spurs_lock);
    for (auto& [ea, sp] : g_spurs) {
        for (auto& s : sp->spus) {
            const int w = s->workload;
            std::fprintf(stderr, "[E]   SPU%d (SPURS 0x%x): %s%s, %llu instructions\n", s->index, ea,
                         w < 0 ? "idle" : ("workload " + std::to_string(w) + " '" + sp->wkl[w].name + "' ").c_str(),
                         w < 0 ? "" : spu_where(*s).c_str(), (unsigned long long)s->insn_count);
        }
        std::string rc;
        for (u32 i = 0; i < 16; ++i) if (sp->wkl[i].active) rc += " " + std::to_string(i) + ":" + std::to_string(ready_count(*sp, i));
        std::fprintf(stderr, "[E]   SPURS 0x%x ready counts:%s\n", ea, rc.c_str());
    }
}

// local store: 128-byte aligned, never freed
void* aligned_alloc_128(size_t n) {
#ifdef _WIN32
    return _aligned_malloc(n, 128);
#else
    return std::aligned_alloc(128, n);
#endif
}

void spurs_start(Spurs& sp) {
    g_watchdog_extra = report_spus;
    static int next_index = 0;
    for (u32 i = 0; i < sp.nspus; ++i) {
        auto s = std::make_unique<SPUThread>();
        s->ls = static_cast<u8*>(aligned_alloc_128(kLsSize + 0x100));
        s->index = next_index++;
        s->name = "SPURS " + std::to_string(i);
        s->stop_handler = kernel_stop;
        s->user = &sp;
        sp.threads.emplace_back(spu_kernel_thread, &sp, s.get());
        sp.spus.push_back(std::move(s));
    }
}

void _cellSpursAttributeInitialize(PPUContext& c) {
    // (attr, revision, sdkVersion, nSpus, spuPriority, ppuPriority, exitIfNoWork)
    const u32 attr = u32(ARG(0));
    std::memset(mem::base + attr, 0, 512);
    st32(mem::base, attr + 0, ARG(1)); st32(mem::base, attr + 4, ARG(2)); st32(mem::base, attr + 8, ARG(3));
    st32(mem::base, attr + 0xc, ARG(4)); st32(mem::base, attr + 0x10, ARG(5)); st8(mem::base, attr + 0x14, ARG(6));
    RET(CELL_OK);
}
void cellSpursInitializeWithAttribute(PPUContext& c) {
    const u32 spurs = u32(ARG(0)), attr = u32(ARG(1));
    auto sp = std::make_unique<Spurs>();
    sp->ea = spurs;
    sp->nspus = ld32(mem::base, attr + 8);
    if (sp->nspus == 0 || sp->nspus > 6) sp->nspus = 6;
    std::memset(mem::base + spurs, 0, 0x80);  // ready counts and contention bytes
    LOG_NOTE("cellSpursInitializeWithAttribute spurs=0x%x nSpus=%u spuPrio=%d", spurs, sp->nspus, s32(ld32(mem::base, attr + 0xc)));
    Spurs* p = sp.get();
    { std::lock_guard lk(g_spurs_lock); g_spurs[spurs] = std::move(sp); }
    spurs_start(*p);
    RET(CELL_OK);
}
SPURS_STUB(cellSpursFinalize, -1)
SPURS_STUB(cellSpursAttachLv2EventQueue, -1)
SPURS_STUB(cellSpursDetachLv2EventQueue, -1)
SPURS_STUB(cellSpursSetExceptionEventHandler, -1)
SPURS_STUB(cellSpursSetPriorities, -1)
void cellSpursGetInfo(PPUContext& c) {
    const u32 spurs = u32(ARG(0)), info = u32(ARG(1));
    std::memset(mem::base + info, 0, 0x280);
    if (Spurs* sp = spurs_get(spurs)) {
        st32(mem::base, info + 0, sp->nspus);  // nSpus
        st32(mem::base, info + 4, 250);         // spuThreadGroupPriority
        st32(mem::base, info + 8, 1000);        // ppuThreadPriority
    }
    RET(CELL_OK);
}
SPURS_STUB(cellSpursRequestIdleSpu, -1)
void _cellSpursWorkloadAttributeInitialize(PPUContext& c) {
    // (attr, revision, sdkVersion, pm, size, data, priority[8], minContention, maxContention)
    const u32 attr = u32(ARG(0));
    std::memset(mem::base + attr, 0, 0x200);
    st32(mem::base, attr + 0, ARG(1)); st32(mem::base, attr + 4, ARG(2));
    st32(mem::base, attr + 8, ARG(3)); st32(mem::base, attr + 0xc, ARG(4));
    st64(mem::base, attr + 0x10, ARG(5));
    if (ARG(6)) std::memcpy(mem::base + attr + 0x18, mem::base + u32(ARG(6)), 8);
    st32(mem::base, attr + 0x20, ARG(7)); st32(mem::base, attr + 0x24, ARG(8));
    RET(CELL_OK);
}
void cellSpursWorkloadAttributeSetName(PPUContext& c) {
    st32(mem::base, u32(ARG(0)) + 0x28, ARG(1));
    st32(mem::base, u32(ARG(0)) + 0x2c, ARG(2));
    RET(CELL_OK);
}
void cellSpursAddWorkloadWithAttribute(PPUContext& c) {
    const u32 spurs = u32(ARG(0)), wid_out = u32(ARG(1)), attr = u32(ARG(2));
    Spurs* sp = spurs_get(spurs);
    if (!sp) { RET(0x80410711); return; }  // CELL_SPURS_CORE_ERROR_INVAL
    std::lock_guard lk(sp->lock);
    for (u32 wid = 0; wid < 16; ++wid) {
        Workload& w = sp->wkl[wid];
        if (w.active) continue;
        w = Workload{};
        w.active = true;
        w.pm = ld32(mem::base, attr + 8);
        w.pm_size = ld32(mem::base, attr + 0xc);
        w.data = ld64(mem::base, attr + 0x10);
        std::memcpy(w.priority, mem::base + attr + 0x18, 8);
        w.min = ld32(mem::base, attr + 0x20);
        w.max = std::max(1u, ld32(mem::base, attr + 0x24));
        const u32 cls = ld32(mem::base, attr + 0x28), inst = ld32(mem::base, attr + 0x2c);
        w.name = (cls ? read_cstr(cls, 64) : std::string()) + (inst ? read_cstr(inst, 64) : std::string());
        w.unique_id = sp->next_unique++;
        st8(mem::base, spurs + wid, 0);
        st32(mem::base, wid_out, wid);
        LOG_NOTE("SPURS 0x%x: workload %u '%s' pm=0x%x size=0x%x data=0x%llx max=%u", spurs, wid, w.name.c_str(), w.pm,
                 w.pm_size, (unsigned long long)w.data, w.max);
        RET(CELL_OK);
        return;
    }
    RET(0x80410702);  // CELL_SPURS_CORE_ERROR_AGAIN
}
void cellSpursShutdownWorkload(PPUContext& c) {
    if (Spurs* sp = spurs_get(u32(ARG(0)))) {
        std::lock_guard lk(sp->lock);
        if (ARG(1) < 16) sp->wkl[ARG(1)].shutdown = true;
    }
    RET(CELL_OK);
}
void cellSpursWaitForWorkloadShutdown(PPUContext& c) {
    if (Spurs* sp = spurs_get(u32(ARG(0)))) {
        std::unique_lock lk(sp->lock);
        const u32 wid = u32(ARG(1)) & 15;
        sp->cv.wait(lk, [&] { return sp->wkl[wid].running == 0; });
    }
    RET(CELL_OK);
}
void cellSpursRemoveWorkload(PPUContext& c) {
    if (Spurs* sp = spurs_get(u32(ARG(0)))) {
        std::lock_guard lk(sp->lock);
        if (ARG(1) < 16) sp->wkl[ARG(1)].active = false;
    }
    RET(CELL_OK);
}
void cellSpursReadyCountStore(PPUContext& c) {
    const u32 spurs = u32(ARG(0)), wid = u32(ARG(1)), value = u32(ARG(2));
    if (wid >= 16 || value > 0xff) { RET(0x80410702 + 0); return; }
    {
        std::lock_guard line(ppu_line_lock(spurs + wid));
        __atomic_store_n(mem::base + spurs + wid, u8(value), __ATOMIC_RELEASE);
    }
    if (Spurs* sp = spurs_get(spurs)) sp->cv.notify_all();
    RET(CELL_OK);
}
SPURS_STUB(_cellSpursTasksetAttributeInitialize, -1)
SPURS_STUB(cellSpursTasksetAttributeSetName, -1)
SPURS_STUB(cellSpursCreateTasksetWithAttribute, -1)
SPURS_STUB(cellSpursShutdownTaskset, -1)
SPURS_STUB(cellSpursJoinTaskset, -1)
SPURS_STUB(cellSpursEventFlagAttachLv2EventQueue, -1)
SPURS_STUB(cellSpursEventFlagDetachLv2EventQueue, -1)
SPURS_STUB(cellSpursLFQueueAttachLv2EventQueue, -1)
SPURS_STUB(cellSpursLFQueueDetachLv2EventQueue, -1)
SPURS_STUB(cellSyncLFQueueGetDirection, -1)
}  // namespace

void register_spurs() {
    register_import(0x95180230, "_cellSpursAttributeInitialize", _cellSpursAttributeInitialize);
    register_import(0xaa6269a8, "cellSpursInitializeWithAttribute", cellSpursInitializeWithAttribute);
    register_import(0xca4c4600, "cellSpursFinalize", cellSpursFinalize);
    register_import(0xb9bc6207, "cellSpursAttachLv2EventQueue", cellSpursAttachLv2EventQueue);
    register_import(0x4e66d483, "cellSpursDetachLv2EventQueue", cellSpursDetachLv2EventQueue);
    register_import(0xd2e23fa9, "cellSpursSetExceptionEventHandler", cellSpursSetExceptionEventHandler);
    register_import(0x80a29e27, "cellSpursSetPriorities", cellSpursSetPriorities);
    register_import(0x1f402f8f, "cellSpursGetInfo", cellSpursGetInfo);
    register_import(0x182d9890, "cellSpursRequestIdleSpu", cellSpursRequestIdleSpu);
    register_import(0xefeb2679, "_cellSpursWorkloadAttributeInitialize", _cellSpursWorkloadAttributeInitialize);
    register_import(0x4a5eab63, "cellSpursWorkloadAttributeSetName", cellSpursWorkloadAttributeSetName);
    register_import(0xc0158d8b, "cellSpursAddWorkloadWithAttribute", cellSpursAddWorkloadWithAttribute);
    register_import(0x98d5b343, "cellSpursShutdownWorkload", cellSpursShutdownWorkload);
    register_import(0x5fd43fe4, "cellSpursWaitForWorkloadShutdown", cellSpursWaitForWorkloadShutdown);
    register_import(0x57e4dec3, "cellSpursRemoveWorkload", cellSpursRemoveWorkload);
    register_import(0xf843818d, "cellSpursReadyCountStore", cellSpursReadyCountStore);
    register_import(0x16394a4e, "_cellSpursTasksetAttributeInitialize", _cellSpursTasksetAttributeInitialize);
    register_import(0x652b70e2, "cellSpursTasksetAttributeSetName", cellSpursTasksetAttributeSetName);
    register_import(0xc10931cb, "cellSpursCreateTasksetWithAttribute", cellSpursCreateTasksetWithAttribute);
    register_import(0xbeb600ac, "cellSpursCreateTask", cellSpursCreateTask);
    register_import(0xa789e631, "cellSpursShutdownTaskset", cellSpursShutdownTaskset);
    register_import(0x9f72add3, "cellSpursJoinTaskset", cellSpursJoinTaskset);
    register_import(0x5ef96465, "_cellSpursEventFlagInitialize", _cellSpursEventFlagInitialize);
    register_import(0x87630976, "cellSpursEventFlagAttachLv2EventQueue", cellSpursEventFlagAttachLv2EventQueue);
    register_import(0x22aab31d, "cellSpursEventFlagDetachLv2EventQueue", cellSpursEventFlagDetachLv2EventQueue);
    register_import(0x373523d4, "cellSpursEventFlagWait", cellSpursEventFlagWait);
    register_import(0x4ac7bae4, "cellSpursEventFlagClear", cellSpursEventFlagClear);
    register_import(0x011ee38b, "_cellSpursLFQueueInitialize", _cellSpursLFQueueInitialize);
    register_import(0x1656d49f, "cellSpursLFQueueAttachLv2EventQueue", cellSpursLFQueueAttachLv2EventQueue);
    register_import(0x73e06f91, "cellSpursLFQueueDetachLv2EventQueue", cellSpursLFQueueDetachLv2EventQueue);
    register_import(0x8a85674d, "_cellSpursLFQueuePushBody", _cellSpursLFQueuePushBody);
    register_import(0xd59aa307, "cellSyncLFQueueGetDirection", cellSyncLFQueueGetDirection);
}
