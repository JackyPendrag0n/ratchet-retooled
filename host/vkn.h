// Native Vulkan renderer: shared definitions between the capture side (RSX command thread) and
// the render thread.
//
// The command thread runs the game's GPU command stream as before (registers, labels,
// semaphores, memory copies) and, for every draw, clear, blit and flip, writes a packet into a
// batch for the render thread. Everything a packet needs from guest memory is copied at that
// moment into a host-visible Vulkan buffer (the data ring): raw big-endian vertex arrays, byte-
// swapped indices, constants, changed textures. The game may reuse that memory as soon as the
// command stream passes the draw, exactly as on the console, while the render thread records
// and submits Vulkan work in parallel. Vertex formats are decoded by the vertex shader (it pulls
// attributes from the ring as a storage buffer), so the CPU never converts vertices.
//
// The render thread sees the register file as it was at each packet: the capture side sends
// every register written since the previous packet (P_REGS) before it.
#pragma once
#include "rsx_iface.h"
#include "rsx_state.h"
#include "rsx.h"
#include "rsx_tex.h"
#include "vk_backend.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace rsx::vkn {

// ---- packets ------------------------------------------------------------------------------
enum PacketType : u32 {
    P_REGS = 1,       // u32 count, then count x {u32 reg index, u32 value}
    P_VP,             // VpUpload: a vertex program the render thread has not seen
    P_FP,             // FpUpload + program bytes
    P_TEX,            // TexUpload: raw texture bytes in the data ring
    P_TEX_FREE,       // u32 id
    P_DRAW,           // DrawPacket + ranges
    P_CLEAR,          // ClearPacket
    P_BLIT,           // BlitParams (source is a surface)
    P_DROP_SURFACE,   // u32 ea, u32 depth: guest memory there was rewritten; stop sampling the surface
    P_FLIP,           // FlipPacket
    P_SUBMIT,         // u64 ring position: submit what was recorded so far (the ring is filling up)
};

struct PacketHeader { u32 type, size; };  // size of the payload in bytes (multiple of 4)

struct VpUpload {
    u64 hash;
    u32 start;
    u32 ucode[kVpInstructions * 4];
};

struct FpUpload {
    u64 key;          // capture-side id of this program (hash of its instruction words + bytes)
    u32 size;         // bytes that follow
    u32 pad;
};

struct TexUpload {
    u32 id;
    u32 ring_off;     // raw guest bytes (texture_guest_size) in the data ring
    u32 size;
    u32 pad;
    u64 hash;         // xxh64 of the bytes (TOD_VKN_CHECK verifies the copy)
    TexDesc desc;     // desc.ea is the guest address the data came from
};

// One binding of a fragment texture unit.
enum TexBindKind : u8 { TB_NONE = 0, TB_TEXTURE, TB_COLOR_SURFACE, TB_DEPTH_SURFACE };
struct TexBinding {
    u8 kind;
    u8 view;          // TexKind (2D / cube / 3D)
    u8 pad[2];
    u32 id_or_ea;     // texture id, or the surface address
};

// Per-attribute vertex fetch description, as the vertex shader reads it (std140 uvec4).
//   x: byte offset of vertex 0 in the vertex data window (wraps modulo 2^32; vertex 0 may lie
//      before the copied range), or the index into `cval` for constant attributes
//   y: stride in bytes
//   z: type (bits 0-3, RSX vertex type, 0 = constant) | size (bits 4-7) | flags (bit 8: data is
//      little-endian float vec4, immediate mode)
//   w: frequency divider (bits 0-15) | modulo (bit 16)
struct VertexLayout {
    u32 attr[16][4];
    float cval[16][4];
};

enum DrawFlags : u32 {
    DF_INDEXED = 1,       // index buffer in the ring (index_off, index_type)
    DF_IMMEDIATE = 2,
    DF_NEW_VC = 4,        // vc_off holds fresh vertex constants
};

struct DrawRange2 { u32 first, count; };

// Render targets of a draw or clear, resolved to guest addresses by the capture side.
struct TargetDesc {
    u32 clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    u32 aa_x = 1, aa_y = 1;          // multisampled surfaces store each sample as a pixel
    u32 w = 0, h = 0;                // surface size needed (clip end x aa)
    u32 color_fmt = 0, depth_fmt = 0;
    u32 ncolor = 0;
    u32 color_ea[4] = {}, color_pitch[4] = {};
    u32 zea = 0, zpitch = 0;
    u32 has_depth = 0;
    u32 valid = 0;
};

struct DrawPacket {
    TargetDesc targets;
    u32 prim;             // Prim after conversion (quads etc. arrive as indexed triangle lists)
    u32 flags;
    u64 vp_hash;
    u64 fp_key;
    u32 vp_start;
    u32 vc_off;           // ring offset of 468 vec4 vertex constants (valid with DF_NEW_VC)
    u32 fc_off, fc_size;  // fragment constants
    u32 layout_off;       // VertexLayout
    u32 vdata_off, vdata_size;  // storage buffer window holding the vertex arrays
    u32 index_off, index_type;  // 1 = u16, 2 = u32
    s32 vertex_offset;    // added to every index (base index)
    u32 restart;          // primitive restart enabled
    u32 nranges;          // DrawRange2 follow: vertices (non-indexed) or indices (indexed)
    TexBinding tex[16];
};

struct ClearPacket {
    TargetDesc targets;
    u32 mask;
    u32 pad;
};

struct FlipPacket {
    u32 buffer;
    DisplayBuffer db;
    u32 mem_off;          // ~0u: present the surface at the display buffer; else RGBA8 copy of guest memory in the ring
    u64 ring_pos;         // ring position when the flip was written
};

// ---- data ring ----------------------------------------------------------------------------
// Single producer (capture), single consumer (render thread + GPU). Positions grow without
// wrapping; offsets into the buffer are position % size.
struct DataRing {
    u8* ptr = nullptr;
    u64 size = 0;
    u64 align = 256;                 // storage/uniform offset alignment
    u64 head = 0;                    // capture side only
    std::atomic<u64> tail{0};        // released by the render thread once the GPU is done
    std::mutex lock;
    std::condition_variable released;
};

// ---- batches --------------------------------------------------------------------------------
struct Batch {
    std::vector<u8> bytes;
};

class BatchQueue {
public:
    void push(Batch* b) {
        { std::lock_guard lk(m_); q_.push_back(b); }
        cv_.notify_one();
    }
    Batch* pop() {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return !q_.empty(); });
        Batch* b = q_.front();
        q_.pop_front();
        return b;
    }
    void recycle(Batch* b) {
        b->bytes.clear();
        std::lock_guard lk(m_);
        free_.push_back(b);
    }
    Batch* get() {
        { std::lock_guard lk(m_); if (!free_.empty()) { Batch* b = free_.back(); free_.pop_back(); return b; } }
        return new Batch;
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Batch*> q_;
    std::vector<Batch*> free_;
};

// Frames the render thread has finished (presented), for pacing the capture side.
struct FrameCounter {
    std::atomic<u64> done{0};
    std::mutex lock;
    std::condition_variable cv;
};

// ---- render thread interface ----------------------------------------------------------------
struct Shared {
    DataRing ring;
    BatchQueue queue;
    FrameCounter frames;
};

class RenderThread;
// Creates the Vulkan device and the data ring (in `sh`); nullptr if no usable device.
RenderThread* create_render_thread(const VkPresentTarget* target, Shared& sh);
void start_render_thread(RenderThread* rt);           // batches from sh.queue are processed on a new thread
void process_batch(RenderThread* rt, Batch* b);       // synchronous processing (TOD_VKN_SYNC=1)

}  // namespace rsx::vkn
