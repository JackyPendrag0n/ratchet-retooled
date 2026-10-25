#include <cstdarg>
// Guest memory, kernel id manager, PPU threads, and the glue between generated
// code (ppu_import / ppu_syscall) and the HLE / lv2 tables.
#include "guest.h"
#include "ppu_tables.h"
#include "rsx.h"
#include "sync.h"

#include "platform.h"

#include <signal.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/ucontext.h>
#if defined(__APPLE__)
#define UC_RIP(uc) (static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rip)
#define UC_EFL(uc) (static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rflags)
#else
#define UC_RIP(uc) (static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP])
#define UC_EFL(uc) (static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_EFL])
#endif
#endif

int g_log_level = 1;
Config g_config;

// ============================================================================ memory
namespace mem {
u8* base = nullptr;
namespace {
std::mutex g_lock;
std::atomic<u8> g_page_mapped[0x100000];  // one flag per 4 KiB page
struct Bump { u32 next, end; };
Bump g_main{0x20000000, 0x30000000}, g_user{0x30000000, 0x40000000}, g_stack{0xD0000000, 0xE0000000},
    g_area{0x50000000, 0xC0000000};
std::map<u32, u32> g_user_blocks;
u32 g_user_used = 0;

u32 bump(Bump& b, u32 size, u32 align) {
    std::lock_guard lk(g_lock);
    const u32 a = (b.next + align - 1) & ~(align - 1);
    if (u64(a) + size > b.end) return 0;
    b.next = a + size;
    return a;
}
}  // namespace

void reserve() {
    void* p = plat::vm_reserve(0x100000000ull);
    if (!p) throw std::runtime_error("cannot reserve 4 GiB of address space for guest memory");
    base = static_cast<u8*>(p);
}

void map(u32 addr, u32 size) {
    const u64 lo = addr & ~u64(0xfff), hi = (u64(addr) + size + 0xfff) & ~u64(0xfff);
    if (!plat::vm_protect(base + lo, hi - lo, plat::kReadWrite)) throw std::runtime_error("cannot map guest memory");
    for (u64 p = lo >> 12; p < hi >> 12; ++p) g_page_mapped[p].store(1, std::memory_order_release);
}

bool is_mapped(u32 addr, u32 size) {
    const u64 end = u64(addr) + (size ? size : 1);
    if (end > 0x100000000ull) return false;
    for (u64 p = addr >> 12; p < (end + 0xfff) >> 12; ++p)
        if (!g_page_mapped[p].load(std::memory_order_acquire)) return false;
    return true;
}

u32 alloc_main(u32 size, u32 align) { const u32 a = bump(g_main, size, align); if (a) map(a, size); return a; }
u32 alloc_stack(u32 size) { const u32 a = bump(g_stack, size + 0x10000, 0x10000); if (a) map(a, size); return a; }  // gap = guard
u32 alloc_area(u32 size, u32 align) { return bump(g_area, size, align); }
u32 alloc_user(u32 size, u32 page) {
    if (g_user_used + size > kUserMemorySize) return 0;
    const u32 a = bump(g_user, size, page);
    if (!a) return 0;
    map(a, size);
    std::lock_guard lk(g_lock);
    g_user_blocks[a] = size;
    g_user_used += size;
    return a;
}
void free_user(u32 addr) {
    std::lock_guard lk(g_lock);
    auto it = g_user_blocks.find(addr);
    if (it == g_user_blocks.end()) return;
    g_user_used -= it->second;  // address space is not reused (bump allocator)
    std::memset(base + addr, 0, it->second);
    g_user_blocks.erase(it);
}
u32 user_used() { return g_user_used; }
}  // namespace mem

std::string read_cstr(u32 ea, size_t max) {
    std::string s;
    if (!ea) return s;
    for (size_t i = 0; i < max; ++i) {
        const char ch = static_cast<char>(mem::base[ea + i]);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}
void write_cstr(u32 ea, const std::string& s) { std::memcpy(mem::base + ea, s.c_str(), s.size() + 1); }

// ============================================================================ id manager
namespace idm {
namespace {
std::mutex g_lock;
std::unordered_map<u32, std::shared_ptr<KObject>> g_objs;
std::map<u32, u32> g_next;  // base -> next id
}  // namespace
u32 add(std::shared_ptr<KObject> obj, u32 base_id) {
    std::lock_guard lk(g_lock);
    u32& n = g_next[base_id];
    if (!n) n = base_id;
    const u32 id = n;
    n += 0x100;
    g_objs[id] = std::move(obj);
    return id;
}
std::shared_ptr<KObject> get(u32 id) {
    std::lock_guard lk(g_lock);
    auto it = g_objs.find(id);
    return it == g_objs.end() ? nullptr : it->second;
}
bool remove(u32 id) { std::lock_guard lk(g_lock); return g_objs.erase(id) != 0; }
}  // namespace idm

// ============================================================================ imports
namespace {
struct ImportBinding { const char* name = nullptr; HleFunc fn = nullptr; };
std::unordered_map<u32, ImportBinding>& import_map() { static std::unordered_map<u32, ImportBinding> m; return m; }
}  // namespace

void register_import(u32 nid, const char* name, HleFunc fn) {
    auto& b = import_map()[nid];
    if (fn || !b.name) b.name = name;
    if (fn) b.fn = fn;
}

void ppu_import(PPUContext& c, u8*, u32 index) {
    const PPUImportEntry& imp = ppu_import_table[index];
    auto it = import_map().find(imp.nid);
    if (it == import_map().end() || !it->second.fn) {
        char b[200];
        std::snprintf(b, sizeof b, "unimplemented import %s (%s 0x%08X) called from 0x%llX",
                      it == import_map().end() ? "?" : it->second.name, imp.library, imp.nid, (unsigned long long)c.lr);
        throw std::runtime_error(b);
    }
    PPUThread* self = current_thread();
    if (self) { self->call_lr = u32(c.lr); self->in_call = it->second.name; }
    LOG_TRACE("%s(0x%llx, 0x%llx, 0x%llx, 0x%llx) lr=0x%llx", it->second.name, (unsigned long long)c.r[3],
              (unsigned long long)c.r[4], (unsigned long long)c.r[5], (unsigned long long)c.r[6], (unsigned long long)c.lr);
    it->second.fn(c);
    if (self) self->in_call = nullptr;
    LOG_TRACE("  -> 0x%llx", (unsigned long long)c.r[3]);
}

void ppu_syscall(PPUContext& c, u8*, u32 addr) {
    PPUThread* self = current_thread();
    if (self) { self->call_lr = addr; self->call_num = u32(c.r[11]); self->in_call = "syscall"; }
    lv2_syscall(c, static_cast<u32>(c.r[11]), addr);
    if (self) self->in_call = nullptr;
}

void ppu_probe(PPUContext&, u8*, u32) {}

void log_line(char tag, const char* fmt, ...) {
    char buf[1024];
    const PPUThread* t = current_thread();
    int n = std::snprintf(buf, sizeof buf, "[%c %x] ", tag, t ? t->id : 0);
    va_list ap;
    va_start(ap, fmt);
    n += std::vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (n > int(sizeof buf) - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    std::fwrite(buf, 1, n, stderr);
}

void guest_call(PPUContext& c, u32 opd, std::initializer_list<u64> args) {
    const u32 code = ld32(mem::base, opd), toc = ld32(mem::base, opd + 4);
    PPUFunc f = ppu_lookup(code);
    if (!f) throw std::runtime_error("guest_call: no recompiled function at descriptor target");
    const u64 saved_lr = c.lr, saved_r2 = c.r[2];
    int i = 3;
    for (u64 a : args) c.r[i++] = a;
    c.r[2] = toc;
    c.lr = 0;
    f(c, mem::base);
    c.lr = saved_lr;
    c.r[2] = saved_r2;
}

// ============================================================================ threads
namespace {
thread_local PPUThread* t_current = nullptr;
std::mutex g_threads_lock;
std::map<u32, std::shared_ptr<PPUThread>> g_threads;
u32 g_next_thread_id = 0x01000000;

struct ThreadObj : KObject { std::shared_ptr<PPUThread> t; };

u32 guest_function_at(uintptr_t rip);

// A handle the watchdog can sample the thread through (Windows; POSIX signals the pthread instead).
void* current_os_thread() {
#ifdef _WIN32
    return OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
#else
    return nullptr;
#endif
}

std::atomic<u32> g_watch_addr{0};
std::atomic<bool> g_watch_step{false};
std::atomic<u32> g_watch_skip{0};
std::atomic<int> g_watch_prot{plat::kRead};   // kNone: a read watch (each guest function reading it is reported once)
u32 g_watch_seen[32]; int g_watch_nseen = 0;

// Where a fatal signal hit: guest thread, the HLE call in progress, or the guest function.
void describe_fault_site(uintptr_t rip) {
    const PPUThread* t = t_current;
    if (!t) { std::fprintf(stderr, "[E]   on a host thread (renderer, audio or window)\n"); return; }
    const u32 fn = rip ? guest_function_at(rip) : 0;
    if (const char* call = t->in_call) std::fprintf(stderr, "[E]   thread %s, inside %s called from 0x%X\n", t->name.c_str(), call, t->call_lr.load());
    else if (fn) std::fprintf(stderr, "[E]   thread %s, in guest function sub_%X\n", t->name.c_str(), fn);
    else std::fprintf(stderr, "[E]   thread %s, host code at %p\n", t->name.c_str(), reinterpret_cast<void*>(rip));
}

// The last words of a crash: where it hit and the guest thread's registers and callers.
[[noreturn]] void report_fatal(int sig, uintptr_t a, uintptr_t rip) {
    const auto b = reinterpret_cast<uintptr_t>(mem::base);
    if (sig == SIGABRT || sig == SIGILL || sig == SIGFPE)
        std::fprintf(stderr, "[E] fatal signal %d\n", sig);
    else if (a >= b && a < b + 0x100000000ull)
        std::fprintf(stderr, "[E] guest memory fault at 0x%08llX\n", (unsigned long long)(a - b));
    else
        std::fprintf(stderr, "[E] host fault at %p\n", reinterpret_cast<void*>(a));
    describe_fault_site(rip);
    if (const PPUThread* t = t_current) {  // last register values the recompiled code stored
        for (int i = 0; i < 32; i += 4)
            std::fprintf(stderr, "[E]   r%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)t->ctx.r[i],
                         (unsigned long long)t->ctx.r[i + 1], (unsigned long long)t->ctx.r[i + 2], (unsigned long long)t->ctx.r[i + 3]);
        std::fprintf(stderr, "[E]   lr %016llx ctr %016llx\n", (unsigned long long)t->ctx.lr, (unsigned long long)t->ctx.ctr);
        // guest back chain: the saved LR of each frame
        std::fprintf(stderr, "[E]   callers:");
        u64 sp = t->ctx.r[1];
        for (int i = 0; i < 16 && sp >= t->stack_addr && sp < t->stack_addr + t->stack_size; ++i) {
            const u64 next = ld64(mem::base, u32(sp));
            if (next <= sp || next >= t->stack_addr + t->stack_size) break;
            std::fprintf(stderr, " 0x%X", u32(ld64(mem::base, u32(next) + 16)));
            sp = next;
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    plat::show_stopped(128 + sig);
    std::_Exit(128 + sig);
}

#ifdef _WIN32
// Windows: no signals for faults; an unhandled exception filter gives the same report.
LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const uintptr_t rip = uintptr_t(ep->ContextRecord->Rip);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        report_fatal(SIGSEGV, uintptr_t(er->ExceptionInformation[1]), rip);
    std::fprintf(stderr, "[E] exception 0x%08lX\n", (unsigned long)er->ExceptionCode);
    report_fatal(SIGILL, 0, rip);
}
#else
void segv_handler(int sig, siginfo_t* si, void* uc) {
    const auto a = reinterpret_cast<uintptr_t>(si->si_addr), b = reinterpret_cast<uintptr_t>(mem::base);
    if (sig == SIGTRAP && g_watch_step) {
        // single step after an ignored write to the watched page: protect it again
        g_watch_step = false;
        UC_EFL(uc) &= ~0x100;
        if (const u32 w = g_watch_addr.load()) plat::vm_protect(mem::base + (w & ~0xfffu), 0x1000, g_watch_prot.load());
        return;
    }
    if (const u32 w = g_watch_addr.load(); w && sig == SIGSEGV && a >= b + (w & ~0xfffu) && a < b + (w & ~0xfffu) + 0x1000) {
        // debug write watch (cheats "!watch"): report the writer of [w, w+16) and stop watching;
        // other writes to the page are single-stepped past
        plat::vm_protect(mem::base + (w & ~0xfffu), 0x1000, plat::kReadWrite);
        const uintptr_t rip = uintptr_t(UC_RIP(uc));
        if (g_watch_prot.load() == plat::kNone) {
            const u32 fn = guest_function_at(rip);
            bool seen = !fn || a < b + w || a >= b + w + 16;
            for (int i = 0; i < g_watch_nseen && !seen; ++i) seen = g_watch_seen[i] == fn;
            if (!seen && g_watch_nseen < 32) {
                g_watch_seen[g_watch_nseen++] = fn;
                std::fprintf(stderr, "[N] watch: read of 0x%08llX in sub_%X, lr %08X\n", (unsigned long long)(a - b), fn, t_current ? u32(t_current->ctx.lr) : 0);
            }
            g_watch_step = true;
            UC_EFL(uc) |= 0x100;
            return;
        }
        if (a < b + w || a >= b + w + 16 || (g_watch_skip && guest_function_at(rip) == g_watch_skip)) {
            g_watch_step = true;
            UC_EFL(uc) |= 0x100;
            return;
        }
        g_watch_addr = 0;
        std::fprintf(stderr, "[N] watch: write to 0x%08llX (watching 0x%08X)\n", (unsigned long long)(a - b), w);
        describe_fault_site(UC_RIP(uc));
        if (const PPUThread* t = t_current) {
            std::fprintf(stderr, "[N]   lr %08X callers:", u32(t->ctx.lr));
            u64 sp = t->ctx.r[1];
            for (int i = 0; i < 8 && sp >= t->stack_addr && sp < t->stack_addr + t->stack_size; ++i) {
                const u64 next = ld64(mem::base, u32(sp));
                if (next <= sp || next >= t->stack_addr + t->stack_size) break;
                std::fprintf(stderr, " 0x%X", u32(ld64(mem::base, u32(next) + 16)));
                sp = next;
            }
            std::fprintf(stderr, "\n");
        }
        return;
    }
    report_fatal(sig, a, uintptr_t(UC_RIP(uc)));
}
#endif

void thread_body(std::shared_ptr<PPUThread> t) {
    t_current = t.get();
    t->pt = pthread_self();
    t->os_thread = current_os_thread();
    try {
        const u32 code = ld32(mem::base, t->entry_opd);
        PPUFunc f = ppu_lookup(code);
        if (!f) throw std::runtime_error("thread entry has no recompiled function");
        f(t->ctx, mem::base);
        t->exit_value = t->ctx.r[3];  // returning from the entry acts like sys_ppu_thread_exit(r3)
    } catch (const ThreadExit& e) {
        t->exit_value = e.value;
    } catch (const ProcessExit& e) {
        LOG_NOTE("process exit(%d) from thread %s", e.code, t->name.c_str());
        std::fflush(stdout);
        std::_Exit(e.code);
    } catch (const std::exception& e) {
        LOG_ERR("thread %s (0x%x) stopped: %s", t->name.c_str(), t->id, e.what());
        std::fflush(stdout);
        std::_Exit(1);
    }
    t->finished = true;
    if (!t->joinable) {  // detached threads disappear on exit (later id lookups give ESRCH)
        std::lock_guard lk(g_threads_lock);
        g_threads.erase(t->id);
    }
}
}  // namespace

PPUThread* current_thread() { return t_current; }
PPUThread* thread_by_id(u32 id) {
    std::lock_guard lk(g_threads_lock);
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second.get();
}

static std::shared_ptr<PPUThread> new_thread(u32 entry_opd, s32 prio, u32 stacksize, const std::string& name) {
    auto t = std::make_shared<PPUThread>();
    {
        std::lock_guard lk(g_threads_lock);
        t->id = g_next_thread_id++;
        g_threads[t->id] = t;
    }
    t->name = name;
    t->entry_opd = entry_opd;
    t->prio = prio;
    t->stack_size = (std::max<u32>(stacksize, 0x4000) + 0xfff) & ~0xfffu;
    t->stack_addr = mem::alloc_stack(t->stack_size);
    if (!t->stack_addr) throw std::runtime_error("out of guest stack space");
    t->ctx.r[1] = t->stack_addr + t->stack_size - 0x70;
    t->ctx.r[2] = entry_opd ? ld32(mem::base, entry_opd + 4) : 0;
    t->ctx.fpscr = 0;
    t->ctx.vscr = 0x10000;  // NJ
    return t;
}

u32 thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stacksize, u64 flags, const std::string& name, u32 tls_r13) {
    auto t = new_thread(entry_opd, prio, stacksize, name);
    t->ctx.r[3] = arg;
    t->ctx.r[13] = tls_r13;
    t->joinable = (flags & 1) != 0;  // SYS_PPU_THREAD_CREATE_JOINABLE
    LOG_NOTE("thread create '%s' id=0x%x entry=0x%x arg=0x%llx prio=%d stack=0x%x", name.c_str(), t->id,
             ld32(mem::base, entry_opd), (unsigned long long)arg, prio, t->stack_size);
    t->host = std::thread(thread_body, t);
    if (!t->joinable) t->host.detach();
    return t->id;
}

u32 host_thread_create(const std::string& name, std::function<void(PPUContext&)> body) {
    auto t = new_thread(0, 1000, 0x10000, name);
    t->ctx.r[13] = tls_alloc();
    t->joinable = false;
    LOG_NOTE("host thread '%s' id=0x%x", name.c_str(), t->id);
    t->host = std::thread([t, body = std::move(body)] {
        t_current = t.get();
        try {
            t->pt = pthread_self();
            t->os_thread = current_os_thread();
        body(t->ctx);
        } catch (const ProcessExit& e) {
            std::fflush(stdout);
            std::_Exit(e.code);
        } catch (const std::exception& e) {
            LOG_ERR("thread %s (0x%x) stopped: %s", t->name.c_str(), t->id, e.what());
            std::fflush(stdout);
            std::_Exit(1);
        }
        t->finished = true;
    });
    t->host.detach();
    return t->id;
}

u64 thread_join(u32 id, bool& ok) {
    std::shared_ptr<PPUThread> t;
    {
        std::lock_guard lk(g_threads_lock);
        auto it = g_threads.find(id);
        if (it == g_threads.end()) { ok = false; return 0; }
        t = it->second;
    }
    if (!t->joinable || !t->host.joinable()) { ok = false; return 0; }
    t->host.join();
    ok = true;
    std::lock_guard lk(g_threads_lock);
    g_threads.erase(id);
    return t->exit_value;
}

int run_main_thread(u32 entry_opd, const std::vector<std::string>& argv, const std::vector<std::string>& envp) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_filter);
    signal(SIGABRT, [](int sig) { report_fatal(sig, 0, 0); });
#else
    struct sigaction sa{};
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP}) sigaction(sig, &sa, nullptr);
#endif
    std::set_terminate([] {
        const char* what = "unknown";
        try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception& e) { what = e.what(); } catch (...) {}
        std::fprintf(stderr, "[E] uncaught exception on a host thread: %s\n", what);
        describe_fault_site(0);
        std::fflush(stderr);
        plat::show_stopped(134);
        std::_Exit(134);
    });

    auto t = new_thread(entry_opd, 1001, 0x10000, "main_thread");
    // Arguments are placed at the top of the primary stack, as lv2 does.
    u32 ptrs = 8 * ((argv.size() + envp.size() + 2 + 1) & ~size_t(1));
    u32 total = ptrs;
    for (auto& s : argv) total += (s.size() + 1 + 15) & ~15u;
    for (auto& s : envp) total += (s.size() + 1 + 15) & ~15u;
    const u32 area = t->stack_addr + t->stack_size - total;
    u32 p = area, data = area + ptrs;
    auto put = [&](const std::vector<std::string>& v) {
        const u32 start = p;
        for (auto& s : v) { write_cstr(data, s); st64(mem::base, p, data); p += 8; data += (s.size() + 1 + 15) & ~15u; }
        st64(mem::base, p, 0); p += 8;
        return start;
    };
    const u32 argv_ea = put(argv), envp_ea = put(envp);
    PPUContext& c = t->ctx;
    c.r[1] = area - 0x70;
    c.r[3] = argv.size(); c.r[4] = argv_ea; c.r[5] = envp_ea; c.r[6] = envp.size();
    c.r[7] = t->id;
    // r8..r10: TLS segment (PT_TLS of the EBOOT); r11: entry; r12: malloc page size (process param).
    c.r[8] = 0x8ad4e0; c.r[9] = 0x8; c.r[10] = 0x1f8;
    c.r[11] = 0x859418; c.r[12] = 0x100000;
    t_current = t.get();
    t->pt = pthread_self();
    t->os_thread = current_os_thread();
    start_watchdog();
    try {
        // lv2 runs sys_initialize_tls before the entry point.
        PPUContext init = c;
        init.r[3] = t->id; init.r[4] = 0x8ad4e0; init.r[5] = 0x8; init.r[6] = 0x1f8;
        extern void hle_sys_initialize_tls(PPUContext&);
        hle_sys_initialize_tls(init);
        c.r[13] = init.r[13];
        PPUFunc f = ppu_lookup(ld32(mem::base, entry_opd));
        f(c, mem::base);
        LOG_NOTE("main thread returned r3=0x%llx", (unsigned long long)c.r[3]);
        return 0;
    } catch (const ThreadExit& e) {
        LOG_NOTE("main thread exited (0x%llx)", (unsigned long long)e.value);
        return 0;
    } catch (const ProcessExit& e) {
        LOG_NOTE("process exit(%d)", e.code);
        return e.code;
    } catch (const std::exception& e) {
        LOG_ERR("main thread stopped: %s", e.what());
        return 1;
    }
}

// ============================================================================ watchdog
void (*g_watchdog_extra)() = nullptr;
// When the game stops presenting frames, report what each guest thread is doing: the HLE
// call it is blocked in, or (for threads still running guest code) the recompiled function
// it is executing, plus a guest call chain from its stack back-chain.
namespace {
#ifndef _WIN32
void sample_handler(int, siginfo_t*, void* uc) {
    if (t_current) t_current->sampled_rip = uintptr_t(UC_RIP(uc));
}
#endif

std::atomic<bool> g_dump_requested{false};

u32 guest_function_at(uintptr_t rip) {
    static std::vector<std::pair<uintptr_t, u32>> table = [] {
        std::vector<std::pair<uintptr_t, u32>> v;
        v.reserve(ppu_function_count);
        for (unsigned i = 0; i < ppu_function_count; ++i)
            v.emplace_back(reinterpret_cast<uintptr_t>(ppu_function_table[i].fn), ppu_function_table[i].address);
        std::sort(v.begin(), v.end());
        return v;
    }();
    if (table.empty() || rip < table.front().first || rip > table.back().first + 0x100000) return 0;
    auto it = std::upper_bound(table.begin(), table.end(), std::make_pair(rip, ~0u));
    return std::prev(it)->second;
}

void dump_threads(double stalled_s) {
    std::vector<std::shared_ptr<PPUThread>> threads;
    {
        std::lock_guard lk(g_threads_lock);
        for (auto& [id, t] : g_threads) if (!t->finished) threads.push_back(t);
    }
    for (auto& t : threads) {
        t->sampled_rip = 0;
#ifdef _WIN32
        // stop the thread for a moment and read its instruction pointer
        if (!t->in_call && t->os_thread) {
            const HANDLE h = static_cast<HANDLE>(t->os_thread);
            if (SuspendThread(h) != DWORD(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &ctx)) t->sampled_rip = uintptr_t(ctx.Rip);
                ResumeThread(h);
            }
        }
#else
        if (!t->in_call && t->pt) pthread_kill(t->pt, SIGUSR2);
#endif
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::fprintf(stderr, "[E] watchdog: no frame for %.0f s (flip #%llu, rsx put 0x%x get 0x%x). Guest threads:\n", stalled_s,
                 (unsigned long long)rsx::flip_count(), ld32(mem::base, rsx::kControlAddr), ld32(mem::base, rsx::kControlAddr + 4));
    for (auto& t : threads) {
        char what[160];
        if (const char* call = t->in_call) {
            if (std::strcmp(call, "syscall") == 0) {
                const u32 num = t->call_num.load();
                int n = std::snprintf(what, sizeof what, "blocked in syscall %u at 0x%X", num, t->call_lr.load());
                if (num == 102 || num == 107) {  // mutex lock / cond wait: which object, and who holds the mutex
                    const u32 id = u32(t->ctx.r[3]);
                    auto mx = idm::get_as<LvMutex>(num == 102 ? id : 0);
                    if (auto cv = idm::get_as<LvCond>(num == 107 ? id : 0)) mx = cv->mtx;
                    std::snprintf(what + n, sizeof what - n, " (id 0x%x, mutex owner 0x%x)", id, mx ? mx->owner : 0);
                }
            }
            else std::snprintf(what, sizeof what, "blocked in %s (called from 0x%X)", call, t->call_lr.load());
        } else if (const uintptr_t rip = t->sampled_rip) {
            const u32 fn = guest_function_at(rip);
            if (fn) std::snprintf(what, sizeof what, "running guest code in sub_%X", fn);
            else std::snprintf(what, sizeof what, "running host code (rip %p)", reinterpret_cast<void*>(rip));
        } else {
            std::snprintf(what, sizeof what, "no sample");
        }
        std::string chain;
        u64 sp = t->ctx.r[1];
        for (int i = 0; i < 12 && sp >= t->stack_addr && sp < t->stack_addr + t->stack_size; ++i) {
            const u64 next = ld64(mem::base, u32(sp));
            if (next <= sp || next >= t->stack_addr + t->stack_size) break;
            char b[16];
            std::snprintf(b, sizeof b, " 0x%X", u32(ld64(mem::base, u32(next) + 16)));
            chain += b;
            sp = next;
        }
        std::fprintf(stderr, "[E]   0x%x %-28s %s; callers:%s\n", t->id, t->name.c_str(), what, chain.c_str());
    }
    if (g_watchdog_extra) g_watchdog_extra();
    std::fflush(stderr);
}
}  // namespace

void start_watchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifndef _WIN32
        struct sigaction sa{};
        sa.sa_sigaction = sample_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR2, &sa, nullptr);
        signal(SIGQUIT, [](int) { g_dump_requested = true; });  // Ctrl+\ in a terminal: report now
#endif
        std::thread([] {
            using clock = std::chrono::steady_clock;
            u64 last_flip = rsx::flip_count();
            auto last_change = clock::now(), next_dump = clock::time_point::max();
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const u64 f = rsx::flip_count();
                const auto now = clock::now();
                if (g_dump_requested.exchange(false)) dump_threads(std::chrono::duration<double>(now - last_change).count());
                if (f != last_flip) { last_flip = f; last_change = now; next_dump = clock::time_point::max(); continue; }
                if (f == 0) continue;  // still booting
                const double stalled = std::chrono::duration<double>(now - last_change).count();
                if (next_dump == clock::time_point::max()) next_dump = last_change + std::chrono::seconds(8);
                if (now >= next_dump) { dump_threads(stalled); next_dump = now + std::chrono::seconds(30); }
            }
        }).detach();
    });
}

// Debugging aid: the next write to [addr, addr+16) is reported (see segv_handler), except
// writes made inside the guest function skip_fn (e.g. memcpy).
void debug_watch_write(u32 addr, u32 skip_fn, bool reads) {
#ifdef _WIN32
    // needs the POSIX fault handler (single-stepping past other writes to the page)
    if (addr) std::fprintf(stderr, "[N] watch: not available on Windows\n");
    (void)skip_fn; (void)reads;
    return;
#endif
    if (const u32 old = g_watch_addr.exchange(0)) plat::vm_protect(mem::base + (old & ~0xfffu), 0x1000, plat::kReadWrite);
    g_watch_skip = skip_fn;
    g_watch_prot = reads ? plat::kNone : plat::kRead;
    g_watch_nseen = 0;
    if (!addr) return;
    g_watch_addr = addr;
    plat::vm_protect(mem::base + (addr & ~0xfffu), 0x1000, g_watch_prot.load());
}
