// sysPrxForUser: TLS, lightweight mutexes, PPU threads, mmapper and process exit.
#include "sync.h"

#include <chrono>
#include <unordered_map>

namespace {
// ---------------------------------------------------------------- TLS
std::mutex g_tls_lock;
u32 g_tls_image = 0, g_tls_file = 0, g_tls_zero = 0, g_tls_size = 0, g_tls_area = 0, g_tls_max = 0;
std::vector<bool> g_tls_used;

u32 tls_block() {
    std::lock_guard lk(g_tls_lock);
    u32 addr = 0;
    for (u32 i = 0; i < g_tls_max; ++i)
        if (!g_tls_used[i]) { g_tls_used[i] = true; addr = g_tls_area + i * g_tls_size; break; }
    if (!addr) addr = mem::alloc_main(g_tls_size, 0x10);
    std::memset(mem::base + addr, 0, 0x30);
    std::memcpy(mem::base + addr + 0x30, mem::base + g_tls_image, g_tls_file);
    std::memset(mem::base + addr + 0x30 + g_tls_file, 0, g_tls_zero);
    return addr;
}

// ---------------------------------------------------------------- lwmutex
struct LwMutex {
    std::mutex m;
    std::condition_variable cv;
    u32 owner = 0, count = 0;
    bool recursive = false;
};
std::mutex g_lw_lock;
std::unordered_map<u32, std::shared_ptr<LwMutex>> g_lw;
u32 g_lw_next = 0x95000100;

std::shared_ptr<LwMutex> lw(u32 addr) {
    std::lock_guard lk(g_lw_lock);
    auto it = g_lw.find(addr);
    return it == g_lw.end() ? nullptr : it->second;
}
void lw_sync_guest(u32 addr, const LwMutex& m) {
    st32(mem::base, addr, m.owner ? m.owner : 0xffffffffu);
    st32(mem::base, addr + 12, m.count);
}

void sys_lwmutex_create(PPUContext& c) {
    const u32 addr = u32(ARG(0)), attr = u32(ARG(1));
    auto m = std::make_shared<LwMutex>();
    const u32 protocol = ld32(mem::base, attr), recursive = ld32(mem::base, attr + 4);
    m->recursive = recursive == 0x10;
    u32 id;
    {
        std::lock_guard lk(g_lw_lock);
        g_lw[addr] = m;
        id = g_lw_next;
        g_lw_next += 0x100;
    }
    st32(mem::base, addr, 0xffffffff); st32(mem::base, addr + 4, 0);
    st32(mem::base, addr + 8, protocol | recursive); st32(mem::base, addr + 12, 0); st32(mem::base, addr + 16, id);
    RET(CELL_OK);
}
void sys_lwmutex_destroy(PPUContext& c) {
    std::lock_guard lk(g_lw_lock);
    RET(g_lw.erase(u32(ARG(0))) ? CELL_OK : CELL_ESRCH);
}
void sys_lwmutex_lock(PPUContext& c) {
    const u32 addr = u32(ARG(0));
    auto m = lw(addr);
    if (!m) { RET(CELL_ESRCH); return; }
    const u32 tid = cur_tid();
    std::unique_lock lk(m->m);
    if (m->owner == tid) {
        if (!m->recursive) { RET(CELL_EDEADLK); return; }
        ++m->count; lw_sync_guest(addr, *m); RET(CELL_OK); return;
    }
    if (!wait_for(m->cv, lk, ARG(1), [&] { return m->owner == 0; })) { RET(CELL_ETIMEDOUT); return; }
    m->owner = tid; m->count = 1;
    lw_sync_guest(addr, *m);
    RET(CELL_OK);
}
void sys_lwmutex_trylock(PPUContext& c) {
    const u32 addr = u32(ARG(0));
    auto m = lw(addr);
    if (!m) { RET(CELL_ESRCH); return; }
    const u32 tid = cur_tid();
    std::lock_guard lk(m->m);
    if (m->owner == tid) { if (!m->recursive) { RET(CELL_EDEADLK); return; } ++m->count; lw_sync_guest(addr, *m); RET(CELL_OK); return; }
    if (m->owner) { RET(CELL_EBUSY); return; }
    m->owner = tid; m->count = 1;
    lw_sync_guest(addr, *m);
    RET(CELL_OK);
}
void sys_lwmutex_unlock(PPUContext& c) {
    const u32 addr = u32(ARG(0));
    auto m = lw(addr);
    if (!m) { RET(CELL_ESRCH); return; }
    std::lock_guard lk(m->m);
    if (m->owner != cur_tid()) { RET(CELL_EPERM); return; }
    if (--m->count == 0) { m->owner = 0; m->cv.notify_one(); }
    lw_sync_guest(addr, *m);
    RET(CELL_OK);
}

// ---------------------------------------------------------------- threads / process
void sys_ppu_thread_create(PPUContext& c) {
    // (thread_id*, entry_opd, arg, prio, stacksize, flags, name*)
    const u32 r13 = tls_block() + 0x7030;
    const u32 id = thread_create(u32(ARG(1)), ARG(2), s32(ARG(3)), u32(ARG(4)), ARG(5), read_cstr(u32(ARG(6)), 32), r13);
    st64(mem::base, u32(ARG(0)), id);
    RET(CELL_OK);
}
void sys_ppu_thread_get_id(PPUContext& c) { st64(mem::base, u32(ARG(0)), cur_tid()); RET(CELL_OK); }
void sys_ppu_thread_exit(PPUContext& c) { throw ThreadExit{ARG(0)}; }
void sys_process_exit(PPUContext& c) { throw ProcessExit{int(s32(ARG(0)))}; }
void sys_game_process_exitspawn(PPUContext& c) {
    LOG_NOTE("exitspawn %s", read_cstr(u32(ARG(0))).c_str());
    throw ProcessExit{0};
}
void sys_prx_exitspawn_with_level(PPUContext&) { throw ProcessExit{0}; }
void sys_process_is_stack(PPUContext& c) { const u32 a = u32(ARG(0)); RET(a >= 0xD0000000 && a < 0xE0000000 ? 1 : 0); }
void sys_time_get_system_time(PPUContext& c) {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    RET64(duration_cast<microseconds>(steady_clock::now() - t0).count() + 1000000);
}
std::mutex g_atexit_lock;
u32 g_atexitspawn = 0, g_at_exitspawn = 0;
void _sys_process_atexitspawn(PPUContext& c) { std::lock_guard lk(g_atexit_lock); if (!g_atexitspawn) g_atexitspawn = u32(ARG(0)); }
void _sys_process_at_Exitspawn(PPUContext& c) { std::lock_guard lk(g_atexit_lock); if (!g_at_exitspawn) g_at_exitspawn = u32(ARG(0)); }

// ---------------------------------------------------------------- mmapper (memory objects mapped into areas)
struct MemObj : KObject { u32 size = 0, mapped_at = 0; };
void sys_mmapper_allocate_memory(PPUContext& c) {
    auto m = std::make_shared<MemObj>();
    m->size = u32(ARG(0));
    st32(mem::base, u32(ARG(2)), idm::add(m, 0x08000000));
    RET(CELL_OK);
}
void sys_mmapper_map_memory(PPUContext& c) {
    auto m = idm::get_as<MemObj>(u32(ARG(1)));
    if (!m) { RET(CELL_ESRCH); return; }
    mem::map(u32(ARG(0)), m->size);
    m->mapped_at = u32(ARG(0));
    LOG_NOTE("mmapper map 0x%x bytes at 0x%x", m->size, m->mapped_at);
    RET(CELL_OK);
}
void sys_mmapper_unmap_memory(PPUContext& c) { RET(CELL_OK); }
void sys_mmapper_free_memory(PPUContext& c) { RET(idm::remove(u32(ARG(0))) ? CELL_OK : CELL_ESRCH); }
}  // namespace

u32 tls_alloc() { return tls_block() + 0x7030; }

void hle_sys_initialize_tls(PPUContext& c) {
    if (c.r[13] != 0) return;  // already initialised for this thread
    {
        std::lock_guard lk(g_tls_lock);
        if (!g_tls_area) {
            g_tls_image = u32(ARG(1));
            g_tls_file = u32(ARG(2));
            g_tls_zero = u32(ARG(3)) - u32(ARG(2));
            g_tls_size = u32(ARG(3)) + 0x30;
            g_tls_area = mem::alloc_main(0x40000) + 0x30;
            g_tls_max = (0x40000 - 0x30) / g_tls_size;
            g_tls_used.assign(g_tls_max, false);
        }
    }
    c.r[13] = tls_block() + 0x7030;
}

void register_sysPrxForUser() {
    register_import(0x744680a2, "sys_initialize_tls", hle_sys_initialize_tls);
    register_import(0x2f85c0ef, "sys_lwmutex_create", sys_lwmutex_create);
    register_import(0xc3476d0c, "sys_lwmutex_destroy", sys_lwmutex_destroy);
    register_import(0x1573dc3f, "sys_lwmutex_lock", sys_lwmutex_lock);
    register_import(0xaeb78725, "sys_lwmutex_trylock", sys_lwmutex_trylock);
    register_import(0x1bc200f4, "sys_lwmutex_unlock", sys_lwmutex_unlock);
    register_import(0x24a1ea07, "sys_ppu_thread_create", sys_ppu_thread_create);
    register_import(0x350d454e, "sys_ppu_thread_get_id", sys_ppu_thread_get_id);
    register_import(0xaff080a4, "sys_ppu_thread_exit", sys_ppu_thread_exit);
    register_import(0xe6f2c1e7, "sys_process_exit", sys_process_exit);
    register_import(0xfc52a7a9, "sys_game_process_exitspawn", sys_game_process_exitspawn);
    register_import(0xa2c7ba64, "sys_prx_exitspawn_with_level", sys_prx_exitspawn_with_level);
    register_import(0x4f7172c9, "sys_process_is_stack", sys_process_is_stack);
    register_import(0x8461e528, "sys_time_get_system_time", sys_time_get_system_time);
    register_import(0x2c847572, "_sys_process_atexitspawn", _sys_process_atexitspawn);
    register_import(0x96328741, "_sys_process_at_Exitspawn", _sys_process_at_Exitspawn);
    register_import(0xb257540b, "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    register_import(0xdc578057, "sys_mmapper_map_memory", sys_mmapper_map_memory);
    register_import(0x4643ba6e, "sys_mmapper_unmap_memory", sys_mmapper_unmap_memory);
    register_import(0x409ad939, "sys_mmapper_free_memory", sys_mmapper_free_memory);
}
