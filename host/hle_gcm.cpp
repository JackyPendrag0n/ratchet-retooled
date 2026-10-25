// cellGcmSys: the 15 imports this game uses, on top of the RSX command processor.
#include "rsx.h"

#include <chrono>

namespace {
enum : u32 { CELL_GCM_ERROR_FAILURE = 0x802100ff, CELL_GCM_ERROR_INVALID_VALUE = 0x80210002,
             CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004, CELL_GCM_ERROR_INVALID_ENUM = 0x80210003 };
constexpr u32 OLD_JUMP_CMD = 0x20000000;
constexpr u32 NV406E_SEMAPHORE_OFFSET = 0x0064 >> 2, GCM_FLIP_COMMAND = 0xFEAC >> 2;

struct {
    u32 io_address = 0, io_size = 0;
    u32 cmd_begin = 0, cmd_fragments = 0;  // default command buffer, in 32 KiB fragments
    u32 display_info = 0, zcull_info = 0, tile_info = 0;
} g;

const u32 kTiledPitches[] = {
    0x00000000, 0x00000200, 0x00000300, 0x00000400, 0x00000500, 0x00000600, 0x00000700, 0x00000800,
    0x00000A00, 0x00000C00, 0x00000D00, 0x00000E00, 0x00001000, 0x00001400, 0x00001800, 0x00001A00,
    0x00001C00, 0x00002000, 0x00002800, 0x00003000, 0x00003400, 0x00003800, 0x00004000, 0x00005000,
    0x00006000, 0x00006800, 0x00007000, 0x00008000, 0x0000A000, 0x0000C000, 0x0000D000, 0x0000E000,
    0x00010000};

// CellGcmContextData {begin, end, current, callback} (big-endian u32 each)
u32 ctx_begin(u32 ctx) { return ld32(mem::base, ctx); }
u32 ctx_end(u32 ctx) { return ld32(mem::base, ctx + 4); }
u32 ctx_current(u32 ctx) { return ld32(mem::base, ctx + 8); }
void set_ctx_current(u32 ctx, u32 v) { st32(mem::base, ctx + 8, v); }

u32 io_offset(u32 ea) {
    const u32 io = rsx::ea_to_io(ea);
    if (io == ~0u) throw std::runtime_error("cellGcm: command buffer address is not IO-mapped");
    return io;
}

// The library callback: flush, jump to the next 32 KiB fragment and wait until the RSX
// has left it. Installed as the default context's callback through a host thunk.
void cellGcmCallback(PPUContext& c, u8*) {
    const u32 ctx = u32(c.r[3]);
    const u32 cur = ctx_current(ctx);
    __atomic_store_n(reinterpret_cast<u32*>(mem::base + rsx::kControlAddr), __builtin_bswap32(io_offset(cur)), __ATOMIC_RELEASE);

    const u32 range = (cur - g.cmd_begin) / 0x8000;
    u32 nb, ne;
    if (range >= g.cmd_fragments - 1) { nb = g.cmd_begin + 0x1000; ne = g.cmd_begin + 0x8000 - 4; }
    else { nb = g.cmd_begin + (range + 1) * 0x8000; ne = g.cmd_begin + (range + 2) * 0x8000 - 4; }

    st32(mem::base, cur, OLD_JUMP_CMD | io_offset(nb));
    st32(mem::base, ctx, nb);
    st32(mem::base, ctx + 4, ne);
    set_ctx_current(ctx, nb);

    const u32 lo = io_offset(nb), hi = io_offset(ne);
    for (unsigned spins = 0;; ++spins) {
        const u32 get = __builtin_bswap32(__atomic_load_n(reinterpret_cast<u32*>(mem::base + rsx::kControlAddr + 4), __ATOMIC_ACQUIRE));
        if (get < lo || get > hi) break;
        if (spins < 1000) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    c.r[3] = 0;
}

// Make room for `words` command words, calling the context's callback like libgcm does.
bool reserve(PPUContext& c, u32 ctx, u32 words) {
    if (ctx_current(ctx) + 4 * words < ctx_end(ctx)) return true;
    const u32 opd = ld32(mem::base, ctx + 12);
    PPUContext saved = c;
    guest_call(c, opd, {ctx, 8});
    const s32 res = s32(c.r[3]);
    c = saved;
    if (res) LOG_ERR("cellGcm: context callback failed 0x%x", res);
    return res == 0;
}

void put_command(u32 ctx, u32 method, std::initializer_list<u32> args) {
    u32 cur = ctx_current(ctx);
    st32(mem::base, cur, (u32(args.size()) << 18) | (method << 2));
    cur += 4;
    for (u32 a : args) { st32(mem::base, cur, a); cur += 4; }
    set_ctx_current(ctx, cur);
}

// ---- imports ---------------------------------------------------------------------------------
void _cellGcmInitBody(PPUContext& c) {
    const u32 context_pp = u32(ARG(0)), cmd_size = u32(ARG(1)), io_size = u32(ARG(2)), io_addr = u32(ARG(3));
    LOG_NOTE("_cellGcmInitBody(context=*0x%x, cmdSize=0x%x, ioSize=0x%x, ioAddress=0x%x)", context_pp, cmd_size, io_size, io_addr);
    rsx::init();
    if (!rsx::map_io(io_addr, 0, io_size)) { RET(CELL_GCM_ERROR_FAILURE); return; }
    g.io_address = io_addr;
    g.io_size = io_size;
    g.cmd_begin = io_addr;
    g.cmd_fragments = cmd_size / 0x8000;

    static const u32 callback_opd = [] {
        const u32 opd = mem::alloc_main(16, 16);
        st32(mem::base, opd, ppu_register_thunk(cellGcmCallback));
        st32(mem::base, opd + 4, 0);
        return opd;
    }();
    const u32 ctx = rsx::kDeviceAddr;
    st32(mem::base, ctx + 0, io_addr + 0x1000);    // 4 KiB reserved at the start
    st32(mem::base, ctx + 4, io_addr + 0x8000 - 4);  // last word kept for the jump
    st32(mem::base, ctx + 8, io_addr + 0x1000);
    st32(mem::base, ctx + 12, callback_opd);
    st32(mem::base, context_pp, ctx);

    g.display_info = mem::alloc_main(16 * 8, 16);
    g.zcull_info = mem::alloc_main(24 * 8, 16);
    g.tile_info = mem::alloc_main(16 * 15, 16);
    RET(CELL_OK);
}

void _cellGcmFunc15(PPUContext& c) { RET(CELL_OK); }

void cellGcmGetConfiguration(PPUContext& c) {
    const u32 cfg = u32(ARG(0));
    st32(mem::base, cfg + 0, rsx::kLocalBase);
    st32(mem::base, cfg + 4, g.io_address);
    st32(mem::base, cfg + 8, rsx::kLocalSize);
    st32(mem::base, cfg + 12, g.io_size);
    st32(mem::base, cfg + 16, 650000000);
    st32(mem::base, cfg + 20, 500000000);
}

void cellGcmAddressToOffset(PPUContext& c) {
    const u32 addr = u32(ARG(0));
    u32 off;
    if (addr - rsx::kLocalBase < rsx::kLocalSize) off = addr - rsx::kLocalBase;
    else if ((off = rsx::ea_to_io(addr)) == ~0u) { RET(CELL_GCM_ERROR_FAILURE); return; }
    st32(mem::base, u32(ARG(1)), off);
    RET(CELL_OK);
}

void cellGcmMapEaIoAddress(PPUContext& c) {
    RET(rsx::map_io(u32(ARG(0)), u32(ARG(1)), u32(ARG(2))) ? CELL_OK : CELL_GCM_ERROR_FAILURE);
}

void cellGcmGetLabelAddress(PPUContext& c) { RET(rsx::kLabelAddr + 0x10 * u32(ARG(0) & 0xff)); }
void cellGcmGetControlRegister(PPUContext& c) { RET(rsx::kControlAddr); }

void cellGcmGetTiledPitchSize(PPUContext& c) {
    const u32 size = u32(ARG(0));
    for (size_t i = 0; i + 1 < std::size(kTiledPitches); ++i)
        if (kTiledPitches[i] < size && size <= kTiledPitches[i + 1]) { RET(kTiledPitches[i + 1]); return; }
    RET(0);
}

void cellGcmSetFlipMode(PPUContext& c) { rsx::set_flip_mode(u32(ARG(0))); }
void cellGcmSetVBlankHandler(PPUContext& c) { rsx::set_vblank_handler(u32(ARG(0))); }

void cellGcmSetDisplayBuffer(PPUContext& c) {
    const u32 id = u32(ARG(0)) & 0xff;
    if (id > 7) { RET(CELL_GCM_ERROR_FAILURE); return; }
    const rsx::DisplayBuffer b{u32(ARG(1)), u32(ARG(2)), u32(ARG(3)), u32(ARG(4))};
    rsx::set_display_buffer(id, b);
    st32(mem::base, g.display_info + 16 * id + 0, b.offset);
    st32(mem::base, g.display_info + 16 * id + 4, b.pitch);
    st32(mem::base, g.display_info + 16 * id + 8, b.width);
    st32(mem::base, g.display_info + 16 * id + 12, b.height);
    LOG_NOTE("cellGcmSetDisplayBuffer(%u, offset=0x%x, pitch=%u, %ux%u)", id, b.offset, b.pitch, b.width, b.height);
    RET(CELL_OK);
}

void cellGcmSetFlip(PPUContext& c) {
    const u32 ctx = u32(ARG(0)), id = u32(ARG(1));
    if (id > 7 || !reserve(c, ctx, 2)) { RET(CELL_GCM_ERROR_FAILURE); return; }
    put_command(ctx, GCM_FLIP_COMMAND, {id});
    RET(CELL_OK);
}

void cellGcmSetWaitFlip(PPUContext& c) {
    const u32 ctx = u32(ARG(0));
    if (!reserve(c, ctx, 3)) return;
    put_command(ctx, NV406E_SEMAPHORE_OFFSET, {0x10, 0});  // offset 0x10, then acquire 0
}

void cellGcmSetTile(PPUContext& c) {
    const u32 index = u32(ARG(0)) & 0xff, location = u32(ARG(1)) & 0xff, offset = u32(ARG(2)), size = u32(ARG(3));
    const u32 pitch = u32(ARG(4)), comp = u32(ARG(5)) & 0xff, base = u32(ARG(6)) & 0xffff, bank = u32(ARG(7)) & 0xff;
    if (index >= 15 || base >= 2048 || bank >= 4) { RET(CELL_GCM_ERROR_INVALID_VALUE); return; }
    if ((offset & 0xffff) || (size & 0xffff) || (pitch & 0xff)) { RET(CELL_GCM_ERROR_INVALID_ALIGNMENT); return; }
    if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12))) { RET(CELL_GCM_ERROR_INVALID_ENUM); return; }
    // CellGcmTileInfo, packed the way the system library does.
    const u32 t = g.tile_info + 16 * index;
    st32(mem::base, t + 0, (location + 1) | (bank << 4) | ((offset / 0x10000) << 16) | (location << 31));
    st32(mem::base, t + 4, (((offset + size - 1) / 0x10000) << 16) | (location << 31));
    st32(mem::base, t + 8, (pitch / 0x100) << 8);
    st32(mem::base, t + 12, base | ((base + ((size - 1) / 0x10000)) << 13) | (comp << 26) | (1u << 30));
    LOG_NOTE("cellGcmSetTile(%u, loc=%u, offset=0x%x, size=0x%x, pitch=%u, comp=%u)", index, location, offset, size, pitch, comp);
    RET(CELL_OK);
}

void cellGcmSetZcull(PPUContext& c) {
    // 12 arguments: the last four come from the parameter save area.
    const u32 index = u32(ARG(0)) & 0xff, offset = u32(ARG(1)), width = u32(ARG(2)), height = u32(ARG(3));
    LOG_NOTE("cellGcmSetZcull(%u, offset=0x%x, %ux%u)", index, offset, width, height);
    if (index < 8) {
        const u32 z = g.zcull_info + 24 * index;
        st32(mem::base, z + 0, (1 << 0) | ((u32(ARG(5)) & 0xf) << 4) | ((u32(ARG(6)) & 0xf) << 8));
        st32(mem::base, z + 4, ((width >> 6) << 22) | ((height >> 6) << 6));
        st32(mem::base, z + 8, u32(ARG(4)) & ~0xfffu);
        st32(mem::base, z + 12, offset);
    }
}
}  // namespace

void register_gcm() {
    register_import(0x15bae46b, "_cellGcmInitBody", _cellGcmInitBody);
    register_import(0x3a33c1fd, "_cellGcmFunc15", _cellGcmFunc15);
    register_import(0xe315a0b2, "cellGcmGetConfiguration", cellGcmGetConfiguration);
    register_import(0x21ac3697, "cellGcmAddressToOffset", cellGcmAddressToOffset);
    register_import(0x63441cb4, "cellGcmMapEaIoAddress", cellGcmMapEaIoAddress);
    register_import(0xf80196c1, "cellGcmGetLabelAddress", cellGcmGetLabelAddress);
    register_import(0xa547adde, "cellGcmGetControlRegister", cellGcmGetControlRegister);
    register_import(0x055bd74d, "cellGcmGetTiledPitchSize", cellGcmGetTiledPitchSize);
    register_import(0x4ae8d215, "cellGcmSetFlipMode", cellGcmSetFlipMode);
    register_import(0xa91b0402, "cellGcmSetVBlankHandler", cellGcmSetVBlankHandler);
    register_import(0xa53d12ae, "cellGcmSetDisplayBuffer", cellGcmSetDisplayBuffer);
    register_import(0xdc09357e, "cellGcmSetFlip", cellGcmSetFlip);
    register_import(0x983fb9aa, "cellGcmSetWaitFlip", cellGcmSetWaitFlip);
    register_import(0xd0b1d189, "cellGcmSetTile", cellGcmSetTile);
    register_import(0xd34a420d, "cellGcmSetZcull", cellGcmSetZcull);
}
