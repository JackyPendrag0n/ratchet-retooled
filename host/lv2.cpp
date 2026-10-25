// lv2 syscalls issued directly by the EBOOT (see ppu-recomp/lv2_syscalls.md).
#include "fs.h"
#include "sync.h"

#include <ctime>
#include <sys/time.h>
#include <unistd.h>

u32 cur_tid() { return current_thread() ? current_thread()->id : 0x01000000; }
u64 thread_join(u32 id, bool& ok);

namespace {
using namespace std::chrono;

// ---------------------------------------------------------------- sync
u32 sys_mutex_create(u32 id_ptr, u32 attr) {
    auto m = std::make_shared<LvMutex>();
    m->recursive = ld32(mem::base, attr + 4) == 0x10;
    const u32 id = idm::add(m, 0x85000000);
    st32(mem::base, id_ptr, id);
    return CELL_OK;
}

u32 sys_cond_create(u32 id_ptr, u32 mutex_id, u32) {
    auto m = idm::get_as<LvMutex>(mutex_id);
    if (!m) return CELL_ESRCH;
    auto cv = std::make_shared<LvCond>();
    cv->mtx = m;
    { std::lock_guard lk(m->m); ++m->cond_refs; }
    st32(mem::base, id_ptr, idm::add(cv, 0x86000000));
    return CELL_OK;
}

u32 sys_cond_wait(u32 id, u64 timeout) {
    auto cv = idm::get_as<LvCond>(id);
    if (!cv) return CELL_ESRCH;
    auto& m = *cv->mtx;
    const u32 tid = cur_tid();
    std::unique_lock lk(m.m);
    if (m.owner != tid) return CELL_EPERM;
    // release the lv2 mutex completely, wait for a signal, then get it back
    LvCond::Waiter self{tid, m.count};
    m.release_locked();
    cv->waiters.push_back(&self);
    bool ok;
    if (timeout) ok = cv->cv.wait_for(lk, microseconds(timeout), [&] { return self.signaled; });
    else { cv->cv.wait(lk, [&] { return self.signaled; }); ok = true; }
    if (!ok) {
        std::erase(cv->waiters, &self);
        m.acquire_locked(lk, tid, self.count, 0);
    } else {
        m.wait_owned(lk, tid, 0);  // the signal handed us the mutex or queued us for it
    }
    return ok ? CELL_OK : CELL_ETIMEDOUT;
}

u32 sys_cond_signal(u32 id, bool all) {
    auto cv = idm::get_as<LvCond>(id);
    if (!cv) return CELL_ESRCH;
    auto& m = *cv->mtx;
    std::lock_guard lk(m.m);
    if (cv->waiters.empty()) return CELL_OK;
    auto wake = [&](LvCond::Waiter* w) {
        w->signaled = true;
        if (m.owner == 0 && m.queue.empty()) { m.owner = w->tid; m.count = w->count; }
        else m.enqueue_locked(w->tid, w->count);
    };
    if (all) { for (auto* w : cv->waiters) wake(w); cv->waiters.clear(); }
    else { wake(cv->waiters.front()); cv->waiters.pop_front(); }
    cv->cv.notify_all();
    m.cv.notify_all();
    return CELL_OK;
}

u32 sys_rwlock_op(u32 id, int op, u64 timeout) {
    auto rw = idm::get_as<LvRwlock>(id);
    if (!rw) return CELL_ESRCH;
    const u32 tid = cur_tid();
    std::unique_lock lk(rw->m);
    switch (op) {
    case 0:  // rlock
        if (!wait_for(rw->cv, lk, timeout, [&] { return !rw->writer && !rw->waiting_writers; })) return CELL_ETIMEDOUT;
        ++rw->readers;
        return CELL_OK;
    case 1:  // runlock
        if (!rw->readers) return CELL_EPERM;
        if (--rw->readers == 0) rw->cv.notify_all();
        return CELL_OK;
    case 2: {  // wlock
        if (rw->writer == tid) return CELL_EDEADLK;
        ++rw->waiting_writers;
        const bool ok = wait_for(rw->cv, lk, timeout, [&] { return !rw->writer && !rw->readers; });
        --rw->waiting_writers;
        if (!ok) { rw->cv.notify_all(); return CELL_ETIMEDOUT; }
        rw->writer = tid;
        return CELL_OK;
    }
    case 3:  // wunlock
        if (rw->writer != tid) return CELL_EPERM;
        rw->writer = 0;
        rw->cv.notify_all();
        return CELL_OK;
    }
    return CELL_EINVAL;
}

// ---------------------------------------------------------------- events
u32 sys_event_queue_create(u32 id_ptr, u32 attr, u64 key, s32 size) {
    auto q = std::make_shared<LvEventQueue>();
    q->size = size > 0 ? size : 127;
    q->key = key;
    q->type = attr ? ld32(mem::base, attr + 4) : 1;
    st32(mem::base, id_ptr, idm::add(q, 0x8d000000));
    return CELL_OK;
}

u32 sys_event_queue_receive(PPUContext& c, u32 id, u64 timeout) {
    auto q = idm::get_as<LvEventQueue>(id);
    if (!q) return CELL_ESRCH;
    std::unique_lock lk(q->m);
    if (!wait_for(q->cv, lk, timeout, [&] { return !q->events.empty() || q->destroyed; })) return CELL_ETIMEDOUT;
    if (q->events.empty()) return CELL_ECANCELED;
    auto [src, d1, d2, d3] = q->events.front();
    q->events.pop_front();
    c.r[4] = src; c.r[5] = d1; c.r[6] = d2; c.r[7] = d3;
    return CELL_OK;
}

u32 event_queue_push(const std::shared_ptr<LvEventQueue>& q, u64 src, u64 d1, u64 d2, u64 d3) {
    std::lock_guard lk(q->m);
    if (q->events.size() >= q->size) return CELL_EBUSY;
    q->events.emplace_back(src, d1, d2, d3);
    q->cv.notify_one();
    return CELL_OK;
}

// ---------------------------------------------------------------- tty
u32 sys_tty_write(u32 ch, u32 buf, u32 len, u32 pwritelen) {
    std::fwrite(mem::base + buf, 1, len, ch == 0 ? stdout : stderr);
    std::fflush(stdout);
    if (pwritelen) st32(mem::base, pwritelen, len);
    return CELL_OK;
}
}  // namespace

u32 lv2_event_port_send(u32 port_id, u64 d1, u64 d2, u64 d3) {
    auto p = idm::get_as<LvEventPort>(port_id);
    if (!p) return CELL_ESRCH;
    if (!p->queue) return CELL_ENOTCONN;
    const u64 src = p->name ? p->name : ((u64(0x01000500) << 32) | p->id);
    return event_queue_push(p->queue, src, d1, d2, d3);
}

std::shared_ptr<LvEventQueue> lv2_event_queue(u32 id) { return idm::get_as<LvEventQueue>(id); }
u32 lv2_event_queue_push(u32 id, u64 src, u64 d1, u64 d2, u64 d3) {
    auto q = idm::get_as<LvEventQueue>(id);
    return q ? event_queue_push(q, src, d1, d2, d3) : CELL_ESRCH;
}

bool raw_spu_syscall(PPUContext& c, u32 num);

void lv2_syscall(PPUContext& c, u32 num, u32 addr) {
    LOG_TRACE("syscall %u (0x%llx, 0x%llx, 0x%llx, 0x%llx) at 0x%x", num, (unsigned long long)ARG(0),
              (unsigned long long)ARG(1), (unsigned long long)ARG(2), (unsigned long long)ARG(3), addr);
    const u32 a0 = u32(ARG(0)), a1 = u32(ARG(1)), a2 = u32(ARG(2)), a3 = u32(ARG(3));
    switch (num) {
    // process / threads
    case 1: RET(0x01000500); return;  // sys_process_getpid
    case 43: std::this_thread::yield(); RET(CELL_OK); return;
    case 44: {  // sys_ppu_thread_join(id, vptr)
        bool ok;
        const u64 v = thread_join(a0, ok);
        if (!ok) { RET(CELL_ESRCH); return; }
        if (a1) st64(mem::base, a1, v);
        RET(CELL_OK);
        return;
    }
    case 47: if (auto* t = thread_by_id(a0)) t->prio = s32(a1); RET(CELL_OK); return;
    case 48: { auto* t = thread_by_id(a0); if (!t) { RET(CELL_ESRCH); return; } st32(mem::base, a1, u32(t->prio)); RET(CELL_OK); return; }
    case 88: RET(CELL_OK); return;  // sys_interrupt_thread_eoi

    // mutex / cond / rwlock
    case 100: RET(sys_mutex_create(a0, a1)); return;
    case 101: {
        auto m = idm::get_as<LvMutex>(a0);
        if (!m) { RET(CELL_ESRCH); return; }
        { std::lock_guard lk(m->m); if (m->owner || m->cond_refs) { RET(CELL_EBUSY); return; } }
        idm::remove(a0); RET(CELL_OK); return;
    }
    case 102: { auto m = idm::get_as<LvMutex>(a0); RET(m ? m->lock(cur_tid(), ARG(1)) : CELL_ESRCH); return; }
    case 103: { auto m = idm::get_as<LvMutex>(a0); RET(m ? m->trylock(cur_tid()) : CELL_ESRCH); return; }
    case 104: { auto m = idm::get_as<LvMutex>(a0); RET(m ? m->unlock(cur_tid()) : CELL_ESRCH); return; }
    case 105: RET(sys_cond_create(a0, a1, a2)); return;
    case 106: {
        auto cv = idm::get_as<LvCond>(a0);
        if (!cv) { RET(CELL_ESRCH); return; }
        { std::lock_guard lk(cv->mtx->m); if (!cv->waiters.empty()) { RET(CELL_EBUSY); return; } --cv->mtx->cond_refs; }
        idm::remove(a0); RET(CELL_OK); return;
    }
    case 107: RET(sys_cond_wait(a0, ARG(1))); return;
    case 108: RET(sys_cond_signal(a0, false)); return;
    case 109: RET(sys_cond_signal(a0, true)); return;
    case 120: st32(mem::base, a0, idm::add(std::make_shared<LvRwlock>(), 0x88000000)); RET(CELL_OK); return;
    case 121: RET(idm::remove(a0) ? CELL_OK : CELL_ESRCH); return;
    case 122: RET(sys_rwlock_op(a0, 0, ARG(1))); return;
    case 124: RET(sys_rwlock_op(a0, 1, 0)); return;
    case 125: RET(sys_rwlock_op(a0, 2, ARG(1))); return;
    case 127: RET(sys_rwlock_op(a0, 3, 0)); return;

    // events
    case 128: RET(sys_event_queue_create(a0, a1, ARG(2), s32(a3))); return;
    case 129: {
        auto q = idm::get_as<LvEventQueue>(a0);
        if (!q) { RET(CELL_ESRCH); return; }
        { std::lock_guard lk(q->m); q->destroyed = true; q->cv.notify_all(); }
        idm::remove(a0); RET(CELL_OK); return;
    }
    case 130: RET(sys_event_queue_receive(c, a0, ARG(2))); return;
    case 134: {
        auto p = std::make_shared<LvEventPort>();
        p->name = ARG(2);
        const u32 id = idm::add(p, 0x0e000000);
        p->id = id;
        st32(mem::base, a0, id);
        RET(CELL_OK); return;
    }
    case 135: RET(idm::remove(a0) ? CELL_OK : CELL_ESRCH); return;
    case 136: {
        auto p = idm::get_as<LvEventPort>(a0);
        auto q = idm::get_as<LvEventQueue>(a1);
        if (!p || !q) { RET(CELL_ESRCH); return; }
        p->queue = q; RET(CELL_OK); return;
    }
    case 137: { auto p = idm::get_as<LvEventPort>(a0); if (!p) { RET(CELL_ESRCH); return; } p->queue.reset(); RET(CELL_OK); return; }
    case 138: RET(lv2_event_port_send(a0, ARG(1), ARG(2), ARG(3))); return;

    // time
    case 141: std::this_thread::sleep_for(microseconds(ARG(0))); RET(CELL_OK); return;
    case 144: st32(mem::base, a0, 0); st32(mem::base, a1, 0); RET(CELL_OK); return;  // UTC, no DST
    case 145: {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        st64(mem::base, a0, ts.tv_sec); st64(mem::base, a1, ts.tv_nsec);
        RET(CELL_OK); return;
    }
    case 147: RET64(79800000); return;

    // memory
    case 330: {  // sys_mmapper_allocate_address(size, flags, alignment, addr*)
        const u32 size = a0, align = std::max<u32>(a2, 0x10000000);
        const u32 ea = mem::alloc_area(size, align);
        if (!ea) { RET(CELL_ENOMEM); return; }
        st32(mem::base, a3, ea); RET(CELL_OK); return;
    }
    case 341: st32(mem::base, a0, idm::add(std::make_shared<KObject>(), 0x3f000000)); RET(CELL_OK); return;
    case 342: idm::remove(a0); RET(CELL_OK); return;
    case 348: {  // sys_memory_allocate(size, flags, addr*)
        const u32 page = (ARG(1) & 0x400) ? 0x100000 : 0x10000;
        if (a0 % page) { RET(CELL_EALIGN); return; }
        const u32 ea = mem::alloc_user(a0, page);
        if (!ea) { RET(CELL_ENOMEM); return; }
        LOG_NOTE("sys_memory_allocate(0x%x) -> 0x%x", a0, ea);
        st32(mem::base, a2, ea); RET(CELL_OK); return;
    }
    case 351: {  // sys_memory_get_page_attribute(addr, attr*)
        st64(mem::base, a1, 0x40000ull); st64(mem::base, a1 + 8, 0xFull);
        st32(mem::base, a1 + 16, a0 >= 0x30000000 && a0 < 0x40000000 ? 0x100000 : 0x1000); st32(mem::base, a1 + 20, 0);
        RET(CELL_OK); return;
    }
    case 352: st32(mem::base, a0, mem::kUserMemorySize); st32(mem::base, a0 + 4, mem::kUserMemorySize - mem::user_used()); RET(CELL_OK); return;

    // spu (SPU execution is not implemented yet; see notes)
    case 169: LOG_NOTE("sys_spu_initialize(%u, %u)", a0, a1); RET(CELL_OK); return;

    // tty
    case 402: if (a3) st32(mem::base, a3, 0); RET(CELL_OK); return;
    case 403: RET(sys_tty_write(a0, a1, a2, a3)); return;

    default:
        if (num >= 801 && num <= 834) { fs_syscall(c, num); return; }
        if (raw_spu_syscall(c, num)) return;
        break;
    }
    char b[96];
    std::snprintf(b, sizeof b, "unimplemented syscall %u at 0x%X", num, addr);
    throw std::runtime_error(b);
}
