// RSX command processor tailored to this game. The PPU side (cellGcmSys HLE) writes
// commands into IO-mapped main memory and advances PUT; this thread consumes them,
// keeps the method register file, and performs the side effects the CPU can observe
// (labels, semaphores, reports, DMA copies, flips). Rendering hangs off the flip and
// draw hooks and is filled in by the graphics backend.
#pragma once
#include "guest.h"

#include <functional>

namespace rsx {

// Guest addresses of the RSX areas (laid out like RPCS3's so traces look familiar).
constexpr u32 kDeviceAddr = 0x40000000;   // CellGcmContextData of the default context
constexpr u32 kDmaAddr = 0x40100000;      // RsxDmaControl; CellGcmControl {put,get,ref} at +0x40
constexpr u32 kDriverInfo = 0x40200000;
constexpr u32 kLabelAddr = 0x40300000;    // RsxReports: semaphores (labels), notify @0x1000, reports @0x1400
constexpr u32 kAreaSize = 0x00400000;
constexpr u32 kControlAddr = kDmaAddr + 0x40;
constexpr u32 kLocalBase = 0xC0000000, kLocalSize = 0x0F900000;
constexpr u32 kMainMemSize = 0x10000000;  // 256 MiB of IO address space

struct DisplayBuffer { u32 offset = 0, pitch = 0, width = 0, height = 0; };

// One-time device setup (memory areas, reports, threads). Idempotent.
void init();
// IO mapping between main-memory EAs and RSX IO offsets (1 MiB granularity).
bool map_io(u32 ea, u32 io, u32 size);
u32 io_to_ea(u32 io);     // ~0u when unmapped
u32 ea_to_io(u32 ea);     // ~0u when unmapped
// Translate an (offset, DMA context or location) pair to a guest EA; ~0u if invalid.
u32 address(u32 offset, u32 location);

void set_display_buffer(u32 id, const DisplayBuffer& b);
DisplayBuffer display_buffer(u32 id);
void set_flip_mode(u32 mode);
void set_vblank_handler(u32 opd);
void set_flip_handler(u32 opd);
void set_user_handler(u32 opd);

u64 flip_count();
u64 vblank_count();
u32 current_display_buffer();

// Backend hooks (called on the RSX thread).
extern std::function<void(u32 buffer_id)> on_flip;

}  // namespace rsx
