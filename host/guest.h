// Host-side services shared by the kernel (lv2) and library (HLE) layers.
#pragma once
#include "ppu.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---- error codes (CELL_E*) -----------------------------------------------------------
enum : u32 {
    CELL_OK = 0,
    CELL_EAGAIN = 0x80010001, CELL_EINVAL = 0x80010002, CELL_ENOSYS = 0x80010003, CELL_ENOMEM = 0x80010004,
    CELL_ESRCH = 0x80010005, CELL_ENOENT = 0x80010006, CELL_ENOEXEC = 0x80010007, CELL_EDEADLK = 0x80010008,
    CELL_EPERM = 0x80010009, CELL_EBUSY = 0x8001000A, CELL_ETIMEDOUT = 0x8001000B, CELL_EABORT = 0x8001000C,
    CELL_EFAULT = 0x8001000D, CELL_ESTAT = 0x8001000F, CELL_EALIGN = 0x80010010, CELL_EISDIR = 0x80010012,
    CELL_ECANCELED = 0x80010013, CELL_EEXIST = 0x80010014, CELL_ENOTCONN = 0x80010016, CELL_EBADF = 0x8001002A,
    CELL_EIO = 0x8001002B, CELL_EMFILE = 0x8001002C, CELL_ENOTDIR = 0x8001002E, CELL_ENOTEMPTY = 0x80010036,
};

// ---- logging -------------------------------------------------------------------------
extern int g_log_level;  // 0 errors, 1 notices (default), 2 every syscall/import
// One write per line, tagged with the guest thread id, so lines from threads never interleave.
void log_line(char tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG_ERR(...) log_line('E', __VA_ARGS__)
#define LOG_NOTE(...) do { if (g_log_level >= 1) log_line('N', __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (g_log_level >= 2) log_line('T', __VA_ARGS__); } while (0)

// ---- guest memory --------------------------------------------------------------------
namespace mem {
extern u8* base;
void reserve();
void map(u32 addr, u32 size);          // make [addr, addr+size) readable/writable (page granular)
bool is_mapped(u32 addr, u32 size);
// Region allocators (addresses follow RPCS3's layout so pointers look familiar in traces).
u32 alloc_main(u32 size, u32 align = 0x10000);     // 0x20000000.. (TLS, HLE scratch)
u32 alloc_user(u32 size, u32 page);                // 0x30000000.. sys_memory_allocate
u32 alloc_stack(u32 size);                         // 0xD0000000.. thread stacks
u32 alloc_area(u32 size, u32 align);               // 0x50000000.. mmapper address areas
void free_user(u32 addr);
u32 user_used();
constexpr u32 kUserMemorySize = 0x0D500000;        // sys_memory_get_user_memory_size: 213 MiB
}  // namespace mem

static inline u8* gptr(u32 ea) { return mem::base + ea; }
std::string read_cstr(u32 ea, size_t max = 4096);
void write_cstr(u32 ea, const std::string& s);

// ---- threads -------------------------------------------------------------------------
struct ThreadExit { u64 value; };
struct ProcessExit { int code; };

struct PPUThread {
    u32 id = 0;
    std::string name;
    PPUContext ctx{};
    u32 entry_opd = 0;
    u32 stack_addr = 0, stack_size = 0;
    s32 prio = 0;
    u64 exit_value = 0;
    bool joinable = true;
    std::atomic<bool> finished{false};
    std::thread host;
    // Diagnostics for the hang watchdog: the HLE call in progress (if any) and where from.
    pthread_t pt{};
    void* os_thread = nullptr;  // Windows thread handle, for the watchdog's samples
    std::atomic<const char*> in_call{nullptr};
    std::atomic<u32> call_num{0}, call_lr{0};
    std::atomic<uintptr_t> sampled_rip{0};
};

PPUThread* current_thread();
PPUThread* thread_by_id(u32 id);
u32 thread_create(u32 entry_opd, u64 arg, s32 prio, u32 stacksize, u64 flags, const std::string& name, u32 tls_r13);
// A guest-visible thread (id, stack, TLS) whose body is host code, e.g. interrupt handlers.
u32 host_thread_create(const std::string& name, std::function<void(PPUContext&)> body);
// Starts the thread that reports what every guest thread is doing when frames stop (once).
void start_watchdog();
extern void (*g_watchdog_extra)();  // more state for the freeze report (SPUs)
int run_main_thread(u32 entry_opd, const std::vector<std::string>& argv, const std::vector<std::string>& envp);

// TLS (owned by sysPrxForUser HLE, used by thread creation)
u32 tls_alloc();
void tls_free(u32 r13);

// ---- kernel id manager ----------------------------------------------------------------
struct KObject { virtual ~KObject() = default; };
namespace idm {
u32 add(std::shared_ptr<KObject> obj, u32 base_id);
std::shared_ptr<KObject> get(u32 id);
bool remove(u32 id);
template <class T> std::shared_ptr<T> get_as(u32 id) { return std::dynamic_pointer_cast<T>(get(id)); }
}  // namespace idm

// ---- dispatch tables ------------------------------------------------------------------
using HleFunc = void (*)(PPUContext& c);
void register_import(u32 nid, const char* name, HleFunc fn);
void register_all_modules();
void lv2_syscall(PPUContext& c, u32 num, u32 addr);

// Call a guest function (OPD address) from host code on the current thread.
void guest_call(PPUContext& c, u32 opd, std::initializer_list<u64> args);

// ---- configuration --------------------------------------------------------------------
struct Config {
    std::string game_dir;   // folder containing PS3_GAME/ (or the PS3_GAME folder itself)
    std::string data_dir;   // host folder backing /dev_hdd0, /dev_hdd1
    std::string save_dir;   // host folder for save data (/dev_hdd0/home/00000001/savedata)
    std::string elf_path;
    // settings.ini in the data folder
    bool skip_intro = true;  // hide the logo movies and skip the legal screens
    int framerate = 30;      // 30, 60 (the game's own 60 fps mode), 61-240 (-capfps), 0 = uncapped
    float aspect = 16.f / 9.f;  // widescreen: the game's projection and the shown picture
    bool native_renderer = true;  // renderer = native (vkn_*.cpp) or legacy (vk_backend.cpp)
    int resolution_scale = 1;     // native renderer: render at 1-4x the PS3 resolution
    bool async_shaders = true;    // native renderer: build shaders on worker threads, skipping draws until ready
    bool cheat_menu = true;       // F1 / L3+R3 opens the cheat and debug menu
};
extern Config g_config;
std::string vfs_host_path(const std::string& guest);

// Arguments 0-7 are in r3-r10; later ones are in the caller's parameter save area (sp + 48 + 8n).
#define ARG(n) ((n) < 8 ? c.r[3 + (n)] : ld64(mem::base, c.r[1] + 48 + 8 * (n)))
#define RET(v) (c.r[3] = static_cast<u64>(static_cast<s64>(static_cast<s32>(v))))
#define RET64(v) (c.r[3] = static_cast<u64>(v))
