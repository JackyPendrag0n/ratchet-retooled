// Contract between the RSX shader translators (rsx_vp.cpp, rsx_fp.cpp) and the Vulkan
// backend (vk_backend.cpp). Everything that crosses that line is defined here.
//
// Shaders are GLSL 450 compiled to SPIR-V with shaderc at runtime. All resources live in
// descriptor set 0:
//
//   binding 0  uniform VertexConstants { vec4 vc[468]; }          (vertex stage, std140)
//   binding 1  uniform FragmentConstants { vec4 fc[kMaxFpConsts]; } (fragment stage, std140)
//   binding 2  uniform DrawState { ... kDrawStateGlsl ... }         (both stages, std140)
//   binding 16+i  sampler for texture unit i (i = 0..15), fragment stage. The GLSL type
//                 (sampler2D / samplerCube / sampler3D / sampler2DShadow) comes from
//                 FpKey::tex_kind[i].
//   binding 32+i  sampler2D for vertex texture unit i (i = 0..3), vertex stage.
//
// Vertex inputs: every vertex shader declares all 16 attributes as
//     layout(location = i) in vec4 in_attr<i>;
// The backend converts guest (big-endian, packed) vertex data to vec4 floats, so the
// shader never decodes formats. Missing components are already (0,0,0,1)-filled.
//
// Varyings (vertex out == fragment in), fixed locations independent of the program:
//     location 0  diff_color   (VP output reg 1, front diffuse)
//     location 1  spec_color   (VP output reg 2, front specular)
//     location 2  diff_color1  (VP output reg 3, back diffuse)
//     location 3  spec_color1  (VP output reg 4, back specular)
//     location 4  fog_c        (VP output reg 5 .x, broadcast)
//     location 5+n tc<n>, n = 0..9 (VP output regs 7..15 for tc0..tc8, reg 6 for tc9)
// The vertex shader always writes all 15 varyings (unwritten registers default to
// vec4(0,0,0,1)), so any vertex program links with any fragment program.
// Position: gl_Position = apply_viewport(dst_reg0) with the helper in kDrawStateGlsl.
//
// Fragment outputs: layout(location = n) out vec4 ocol<n>, n = 0..3. Depth replace writes
// gl_FragDepth. Alpha test is done in the fragment shader against DrawState.
#pragma once
#include "guest.h"

#include <string>

namespace rsx {

constexpr u32 kMaxFpConsts = 512;
constexpr u32 kVertexConstBinding = 0, kFragmentConstBinding = 1, kDrawStateBinding = 2;
constexpr u32 kTextureBindingBase = 16, kVertexTextureBindingBase = 32;

// std140 layout of binding 2; keep the C++ struct and GLSL text in sync.
struct DrawState {
    float scale_offset[16];   // row-major: clip = vec4(dot(row0,p), dot(row1,p), ...)
    u32 alpha_func;           // 0 = alpha test off, else GL enum 0x200..0x207
    float alpha_ref;          // 0..1
    u32 two_sided_lighting;   // nonzero: back faces take diff_color1/spec_color1
    u32 transform_branch_bits;
    float tex_scale[16][4];   // per unit: xy multiply on coordinates (1/w,1/h for unnormalized), zw unused
    float wpos_scale, wpos_bias, fog_param0, fog_param1;  // WPOS.y = y*scale+bias; fog params
    u32 fog_mode, shader_control;
    u32 alpha_to_coverage;    // 0 off, else samples per pixel of the (supersampled) target
    u32 alpha_to_one;
    float res_inv;            // 1 / resolution scale: gl_FragCoord * res_inv is in guest pixels
    float wpos_ax, wpos_ay;   // 1 / anti-aliasing samples across and down: WPOS counts pixels, not samples
    float pad_[1];
};
static_assert(sizeof(DrawState) == 64 + 16 + 256 + 16 + 16 + 16);

inline const char* kDrawStateGlsl = R"(
layout(std140, set = 0, binding = 2) uniform DrawState {
    mat4 scale_offset_rows;     // rows as stored; use apply_viewport()
    uint alpha_func;
    float alpha_ref;
    uint two_sided_lighting;
    uint transform_branch_bits;
    vec4 tex_scale[16];
    float wpos_scale, wpos_bias, fog_param0, fog_param1;
    uint fog_mode, shader_control, alpha_to_coverage, alpha_to_one;
    float res_inv, wpos_ax, wpos_ay;
};
vec4 apply_viewport(vec4 p) {
    // scale_offset_rows[i] is row i (column-major GLSL mat4 loaded from row-major C data).
    return vec4(dot(scale_offset_rows[0], p), dot(scale_offset_rows[1], p),
                dot(scale_offset_rows[2], p), dot(scale_offset_rows[3], p));
}
)";

// Native renderer: vertex attributes are pulled from a storage buffer holding the guest's raw
// (big-endian) vertex arrays and decoded here. VertexLayout (binding 3) describes each attribute,
// see vkn.h; the data (binding 4) is a window of the renderer's data ring.
constexpr u32 kVertexLayoutBinding = 3, kVertexDataBinding = 4;
inline const char* kVertexPullGlsl = R"(
layout(std140, set = 0, binding = 3) uniform VertexLayout { uvec4 vl_attr[16]; vec4 vl_cval[16]; };
layout(std430, set = 0, binding = 4) readonly buffer VertexData { uint vdata[]; };
uint vd_word(uint a) {
    uint i = a >> 2u, s = (a & 3u) * 8u;
    uint lo = vdata[i];
    return s == 0u ? lo : (lo >> s) | (vdata[i + 1u] << (32u - s));
}
uint vd_be32(uint a) { uint w = vd_word(a); return (w >> 24u) | ((w >> 8u) & 0xff00u) | ((w << 8u) & 0xff0000u) | (w << 24u); }
uint vd_be16(uint a) { uint w = vd_word(a); return ((w & 0xffu) << 8u) | ((w >> 8u) & 0xffu); }
int vd_s16(uint a) { return int(vd_be16(a) << 16u) >> 16; }
uint vd_u8(uint a) { return (vdata[a >> 2u] >> ((a & 3u) * 8u)) & 0xffu; }
vec4 fetch_attr(uint i) {
    uvec4 L = vl_attr[i];
    uint type = L.z & 15u;
    if (type == 0u) return vl_cval[i];
    uint size = (L.z >> 4u) & 15u;
    uint v = uint(gl_VertexIndex);
    uint div = L.w & 0xffffu;
    if (div > 1u) v = (L.w & 0x10000u) != 0u ? v % div : v / div;
    uint a = L.x + v * L.y;
    if ((L.z & 0x100u) != 0u) return uintBitsToFloat(uvec4(vd_word(a), vd_word(a + 4u), vd_word(a + 8u), vd_word(a + 12u)));
    vec4 r = vec4(0.0, 0.0, 0.0, 1.0);
    switch (type) {
    case 1u: for (uint c = 0u; c < size; ++c) r[c] = max(-1.0, float(vd_s16(a + 2u * c)) / 32767.0); break;
    case 2u: for (uint c = 0u; c < size; ++c) r[c] = uintBitsToFloat(vd_be32(a + 4u * c)); break;
    case 3u: for (uint c = 0u; c < size; ++c) r[c] = unpackHalf2x16(vd_be16(a + 2u * c)).x; break;
    case 4u: for (uint c = 0u; c < size; ++c) r[c] = float(vd_u8(a + c)) / 255.0; break;
    case 5u: for (uint c = 0u; c < size; ++c) r[c] = float(vd_s16(a + 2u * c)); break;
    case 6u: {
        uint w = vd_be32(a);
        r.x = float(int(w << 21u) >> 21) / 1023.0;
        r.y = float(int((w >> 11u) << 21u) >> 21) / 1023.0;
        r.z = float(int(w) >> 22) / 511.0;
        break;
    }
    case 7u: for (uint c = 0u; c < size; ++c) r[c] = float(vd_u8(a + c)); break;
    default: break;
    }
    return r;
}
)";

}  // namespace rsx
