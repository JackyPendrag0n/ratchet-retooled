// Graphics state the command processor accumulates for the renderer: the method register
// file plus the pieces that do not live in single registers (transform program and
// constants, immediate-mode vertex data, draw ranges). Owned by the RSX thread.
#pragma once
#include "guest.h"

#include <cstring>
#include <vector>

namespace rsx {

constexpr u32 kVpInstructions = 512, kVpConstants = 468;

enum class Prim : u32 { None = 0, Points, Lines, LineLoop, LineStrip, Triangles, TriangleStrip, TriangleFan, Quads, QuadStrip, Polygon };

struct DrawRange { u32 first, count; };

struct Frontend {
    u32 vp_ucode[kVpInstructions * 4] = {};
    float vc[kVpConstants][4] = {};
    u64 vp_gen = 1, vc_gen = 1;            // bumped on every program / constant write
    float attr_value[16][4] = {};          // current value registers (VERTEX_DATA*_M outside arrays)
    std::vector<float> push[16];           // immediate-mode vertices, vec4 per vertex
    u32 push_vertices = 0;                 // vertices completed (driven by attribute 0)
    u16 push_mask = 0;                     // attributes written in this BEGIN/END
    std::vector<DrawRange> ranges;
    bool indexed = false;
    Prim prim = Prim::None;
};

extern Frontend g_fe;
extern u32 g_regs[0x10000 / 4];
extern u64 g_reg_dirty[0x4000 / 64];   // registers written since the native renderer last sent them

inline u32 reg(u32 byte_offset) { return g_regs[byte_offset >> 2]; }
inline float regf(u32 byte_offset) { float f; std::memcpy(&f, &g_regs[byte_offset >> 2], 4); return f; }

// NV3089 scaled image copy into an NV3062 surface, decoded from the registers. Rectangles are in
// pixels; the source rectangle may be fractional.
struct BlitParams {
    u32 src_ea, src_pitch, src_w, src_h, src_bpp;
    float sx, sy, sw, sh;          // source rectangle
    u32 dst_ea, dst_pitch, dst_bpp, dst_fmt;  // dst_fmt: NV3062 color format
    u32 dx, dy, dw, dh;            // destination rectangle (already clipped)
    bool linear;
};

// Renderer interface, called on the RSX thread. Default implementations do nothing so the
// command processor runs headless without a backend.
struct Renderer {
    virtual ~Renderer() = default;
    virtual void draw() {}                       // g_fe holds the draw; registers are current
    virtual void clear(u32 mask) { (void)mask; } // NV4097_CLEAR_SURFACE argument
    virtual void flip(u32 buffer) { (void)buffer; }
    // Memory at [ea, ea+size) was written by the RSX outside the renderer (DMA/inline copy);
    // the renderer drops or refreshes anything cached from it.
    virtual void invalidate(u32 ea, u32 size) { (void)ea; (void)size; }
    // NV3089 scaled image copy; returns false to let the command processor copy memory.
    virtual bool blit(const struct BlitParams& b) { (void)b; return false; }
};
void set_renderer(Renderer* r);

}  // namespace rsx
