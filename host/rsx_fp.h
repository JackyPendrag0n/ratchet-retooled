// RSX (NV40) fragment program -> GLSL 450 (Vulkan) translator.
//
// Semantics follow RPCS3's FragmentProgramDecompiler (non-native-fp16 path): H and R
// registers are separate vec4 variables; when a lane written through one view is read
// through the other (or exported through it), conversion code (packHalf2x16 /
// unpackHalf2x16) is emitted right after the write so both views stay coherent, which
// matches what RPCS3's RegisterDependencyPass achieves with barrier instructions.
//
// The generated shader follows rsx_iface.h: inline constants are read from
// fc[] (binding 1) in program order, samplers at binding 16+i, DrawState at binding 2.
// DrawState.fog_mode is the raw NV4097_SET_FOG_MODE value (0x2601 linear, 0x800 exp,
// 0x801 exp2, 0x802 exp_abs, 0x803 exp2_abs, 0x804 linear_abs).
#pragma once
#include "guest.h"

#include <string>
#include <vector>

namespace rsx {

enum class TexKind : u8 { Tex2D = 0, Cube = 1, Tex3D = 2, Shadow2D = 3, Tex1D = 4 };

struct FpKey {                 // state baked into the generated shader (part of the pipeline cache key)
    TexKind tex_kind[16] = {};
    u32 shader_control = 0;    // NV4097_SET_SHADER_CONTROL (0x1d60): 0x40 = 32-bit (R) register exports else 16-bit (H); 0x0e depth export
    u8 tex_gamma[16] = {};     // per unit: channels decoded sRGB -> linear after sampling (bit 0 r, 1 g, 2 b, 3 a), see fp_texture_gamma
    u8 tex_expand[16] = {};    // per unit: channels expanded from unsigned to signed (bit 0 r .. 3 a; bit 4 = 16-bit channels), see fp_texture_expand
    u32 srgb_output = 0;       // nonzero: encode colour exports linear -> sRGB (NV4097_SET_SHADER_PACKER on an 8-bit target), see fp_srgb_output
    u32 depth_rgba[16] = {};   // per unit: a D24S8 depth surface read through an 8-bit colour format: bit 16 on, bit 17 A reads as
                               // one (D8R8G8B8), bits 0-15 CONTROL1 remap. The sampler returns depth in .r; the shader splits
                               // it into the bytes the RSX would read from memory (A = bits 23-16, R = 15-8, G = 7-0, B = stencil).
    u32 shadow_soft = 0;       // nonzero: depth-compare lookups take 9 taps (softer edges than the RSX's 2x2 filter)
    bool operator==(const FpKey&) const = default;
};

struct FpInfo {
    u32 size_bytes = 0;            // whole program including inline constants
    u64 hash = 0;                  // over instruction words only, excluding constant values
    std::vector<u32> const_offsets;// byte offset of each inline constant block, in order (fc[i])
    u16 textures_used = 0;         // bitmask of texture units sampled
    bool uses_kill = false, writes_depth = false;
    bool found_end = false;        // false: scan hit max_bytes without an end bit
};

// p = pointer to the program in host memory (guest bytes as stored); max_bytes bounds the scan.
FpInfo fp_analyze(const u8* p, u32 max_bytes);
std::string fp_to_glsl(const u8* p, const FpInfo& info, const FpKey& key);
// FpKey::tex_gamma for a texture unit from its FORMAT (0x1a04) and ADDRESS (0x1a08) registers:
// the gamma bits (ADDRESS bits 20-23, RGBA order, applied to the final channel values) on the
// formats that support them (not depth, 16-bit-channel, HILO or float formats). Follows RPCS3 format_ex();
// signed-channel reads (FILTER bits 28-31) are not emulated, so they do not mask gamma here either.
u8 fp_texture_gamma(u32 format_reg, u32 address_reg);
// FpKey::tex_expand for a texture unit from FORMAT, ADDRESS and CONTROL1 (remap, 0x1a10): with
// UNSIGNED_REMAP = BIASED (ADDRESS bits 12-15 = 1) every channel the remap reads from the texture
// (not a 0/1 constant) is expanded as (x*255 - 128)/127, except gamma channels. bx2 reads expand
// on their own. Follows RPCS3 format_ex() / _process_texel().
u8 fp_texture_expand(u32 format_reg, u32 address_reg, u32 remap_reg);
// FpKey::srgb_output from NV4097_SET_SHADER_PACKER (0x1fec), SET_SURFACE_FORMAT (0x208) and
// SET_SHADER_CONTROL (0x1d60): fixed-point colour targets only, and not from 32-bit exports.
bool fp_srgb_output(u32 shader_packer, u32 surface_format, u32 shader_control);
// helper for the renderer: decode inline constant i to 4 floats.
void fp_read_constant(const u8* p, u32 byte_offset, float out[4]);

}  // namespace rsx
