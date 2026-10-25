// RSX (NV40) vertex program microcode -> GLSL 450 (Vulkan) translator.
// Semantics follow RPCS3's VertexProgramDecompiler; the generated shader follows the
// interface contract in rsx_iface.h (bindings, attribute names, varying locations).
#pragma once
#include "rsx_state.h"  // kVpInstructions, kVpConstants

#include <string>

namespace rsx {

struct VpInfo {
    u64 hash = 0;            // covers start + every instruction reachable from start
    u32 inputs_read = 0;     // bitmask of attributes 0..15 read
    u32 outputs_written = 0; // bitmask of output registers (dst_reg 0..15) written
    u32 instruction_count = 0;
    bool uses_vertex_textures = false;
    u32 vertex_textures = 0; // bitmask of vertex texture units (0..3) sampled by TXL
    bool has_flow_control = false;  // branches/calls present -> dispatcher loop in GLSL
};

// ucode: kVpInstructions * 4 host-endian words; start: TRANSFORM_PROGRAM_START.
VpInfo vp_analyze(const u32* ucode, u32 start);
// pull_vertices: read attributes with fetch_attr() from the vertex data storage buffer
// (kVertexPullGlsl in rsx_iface.h) instead of vertex input locations.
std::string vp_to_glsl(const u32* ucode, u32 start, const VpInfo& info, bool pull_vertices = false);

}  // namespace rsx
