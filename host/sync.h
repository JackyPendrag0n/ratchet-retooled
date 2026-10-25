// lv2 synchronisation objects implemented on host primitives.
#pragma once
#include "guest.h"

#include <chrono>
#include <deque>
#include <tuple>

// timeout in microseconds; 0 = infinite. Returns false on timeout.
template <class Pred>
bool wait_for(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, u64 timeout_us, Pred pred) {
    if (!timeout_us) { cv.wait(lk, pred); return true; }
    return cv.wait_for(lk, std::chrono::microseconds(timeout_us), pred);
}

// Like lv2, ownership passes straight to the next waiter on unlock (and to a signalled cond
// waiter when the mutex is free), so a thread that has been woken cannot lose the mutex to
// another thread's trylock before it gets to run.
struct LvMutex : KObject {
    std::mutex m;
    std::condition_variable cv;
    u32 owner = 0, count = 0;
    bool recursive = false;
    u32 cond_refs = 0;
    struct Queued { u32 tid, count; };
    std::deque<Queued> queue;

    // With m held: take the mutex now, or queue and wait until handed it.
    bool acquire_locked(std::unique_lock<std::mutex>& lk, u32 tid, u32 cnt, u64 timeout) {
        if (owner == 0 && queue.empty()) { owner = tid; count = cnt; return true; }
        enqueue_locked(tid, cnt);
        return wait_owned(lk, tid, timeout);
    }
    void enqueue_locked(u32 tid, u32 cnt) { queue.push_back({tid, cnt}); }
    bool wait_owned(std::unique_lock<std::mutex>& lk, u32 tid, u64 timeout) {
        if (wait_for(cv, lk, timeout, [&] { return owner == tid; })) return true;
        std::erase_if(queue, [&](const Queued& q) { return q.tid == tid; });
        return false;
    }
    // With m held: give the mutex up, to the first waiter if there is one.
    void release_locked() {
        owner = 0; count = 0;
        if (!queue.empty()) { owner = queue.front().tid; count = queue.front().count; queue.pop_front(); cv.notify_all(); }
    }

    u32 lock(u32 tid, u64 timeout) {
        std::unique_lock lk(m);
        if (owner == tid) {
            if (!recursive) return CELL_EDEADLK;
            ++count;
            return CELL_OK;
        }
        return acquire_locked(lk, tid, 1, timeout) ? CELL_OK : CELL_ETIMEDOUT;
    }
    u32 trylock(u32 tid) {
        std::lock_guard lk(m);
        if (owner == tid) { if (!recursive) return CELL_EDEADLK; ++count; return CELL_OK; }
        if (owner || !queue.empty()) return CELL_EBUSY;
        owner = tid; count = 1;
        return CELL_OK;
    }
    u32 unlock(u32 tid) {
        std::lock_guard lk(m);
        if (owner != tid) return CELL_EPERM;
        if (--count == 0) release_locked();
        return CELL_OK;
    }
};

struct LvCond : KObject {
    std::shared_ptr<LvMutex> mtx;
    std::condition_variable_any cv;  // waits on mtx->m
    // Threads currently waiting, in arrival order. A signal marks specific waiters, so a
    // thread that re-waits cannot steal a wake-up meant for another one.
    struct Waiter { u32 tid, count; bool signaled = false; };
    std::deque<Waiter*> waiters;
};

struct LvRwlock : KObject {
    std::mutex m;
    std::condition_variable cv;
    u32 readers = 0, writer = 0, waiting_writers = 0;
};

struct LvEventQueue : KObject {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::tuple<u64, u64, u64, u64>> events;
    u32 size = 127;
    u64 key = 0;
    bool destroyed = false;
    u32 type = 1;
};

struct LvEventPort : KObject {
    std::shared_ptr<LvEventQueue> queue;
    u64 name = 0;
    u32 id = 0;
};

struct LvSemaphore : KObject {
    std::mutex m;
    std::condition_variable cv;
    s32 value = 0, max = 0;
};

u32 cur_tid();
