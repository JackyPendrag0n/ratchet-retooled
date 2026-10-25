// RSX texture decoding: guest memory (big-endian, swizzled or pitched) -> tightly packed host data
// ready for vkCmdCopyBufferToImage, plus the Vulkan format / component mapping (incl. CONTROL1 remap).
//
// Semantics follow RPCS3 (TextureUtils.cpp, RSXTexture.cpp decoded_remap, VKFormats.cpp); the
// implementation here is our own.
#pragma once
#include "guest.h"

#include <vulkan/vulkan.h>

#include <array>
#include <vector>

namespace rsx {

// CELL_GCM_TEXTURE_* base formats (FORMAT register bits 8-15 with the LN/UN flags removed)
enum : u32 {
    TEX_B8 = 0x81, TEX_A1R5G5B5 = 0x82, TEX_A4R4G4B4 = 0x83, TEX_R5G6B5 = 0x84, TEX_A8R8G8B8 = 0x85,
    TEX_DXT1 = 0x86, TEX_DXT23 = 0x87, TEX_DXT45 = 0x88, TEX_G8B8 = 0x8B,
    TEX_B8R8_G8R8 = 0x8D, TEX_R8B8_R8G8 = 0x8E, TEX_R6G5B5 = 0x8F,
    TEX_DEPTH24_D8 = 0x90, TEX_DEPTH24_D8_FLOAT = 0x91, TEX_DEPTH16 = 0x92, TEX_DEPTH16_FLOAT = 0x93,
    TEX_X16 = 0x94, TEX_Y16_X16 = 0x95, TEX_R5G5B5A1 = 0x97, TEX_HILO8 = 0x98, TEX_HILO_S8 = 0x99,
    TEX_W16_Z16_Y16_X16_FLOAT = 0x9A, TEX_W32_Z32_Y32_X32_FLOAT = 0x9B, TEX_X32_FLOAT = 0x9C,
    TEX_D1R5G5B5 = 0x9D, TEX_D8R8G8B8 = 0x9E, TEX_Y16_X16_FLOAT = 0x9F,
    TEX_LN = 0x20, TEX_UN = 0x40,
};

struct TexDesc {
    u32 ea = 0;            // guest address of level 0 (already translated)
    u32 format = 0;        // raw FORMAT register
    u32 width = 0, height = 0, depth = 1;
    u32 pitch = 0;         // CONTROL3 pitch (0 for swizzled)
    u32 mipmaps = 1;       // 0 -> taken from FORMAT bits 16-31
    u32 remap = 0xAAE4;    // CONTROL1
    bool operator==(const TexDesc&) const = default;
};

struct TexLevel {
    u32 offset;      // byte offset into DecodedTex::data
    u32 width, height, depth;  // texel extent of this level
    u32 row_length;  // texels per row in data (bufferRowLength); >= width (block-aligned for BC / packed formats)
    u32 size;        // bytes
};

struct DecodedTex {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;  // 1D, 2D, CUBE or 3D
    u32 layers = 1;                       // 6 for cube
    std::vector<u8> data;                 // all layers/levels tightly packed, ready for vkCmdCopyBufferToImage
    std::vector<TexLevel> levels;         // layer-major: index = layer * mip_count + level
    VkComponentMapping mapping{};         // channel order + remap applied via the image view
    u32 mip_count() const { return layers ? u32(levels.size() / layers) : 0; }
};

// Field helpers for the raw FORMAT register
inline u32 tex_base_format(u32 format_reg) { return (format_reg >> 8) & 0xFF & ~(TEX_LN | TEX_UN); }
inline bool tex_is_linear(u32 format_reg) { return (format_reg >> 8) & TEX_LN; }
inline bool tex_is_cubemap(u32 format_reg) { return (format_reg >> 2) & 1; }
inline u32 tex_dimension(u32 format_reg) { return (format_reg >> 4) & 0xF; }
// FORMAT bit 3 is the border type: 0 = border texels stored in the texture, 1 = border colour.
inline bool tex_has_border(u32 format_reg) { return !((format_reg >> 3) & 1); }

bool is_supported_format(u32 base_format);
bool is_depth_format(u32 base_format);
bool is_compressed_dxt(u32 base_format);

u32 texture_guest_size(const TexDesc&);                   // bytes the texture occupies in guest memory (all levels/faces)
bool decode_texture(const TexDesc&, DecodedTex& out);     // false + reason via a log line on unsupported formats
// Same, reading the texture's bytes from `data` (a copy of guest memory at desc.ea) instead of guest memory.
bool decode_texture_from(const TexDesc&, const u8* data, DecodedTex& out);

// Host Vulkan format used for a texture of this base format (decode_texture output format).
VkFormat host_format(u32 base_format);
// Base component mapping in ARGB order (RPCS3 get_component_mapping semantics) for data decoded by decode_texture.
std::array<VkComponentSwizzle, 4> base_mapping_argb(u32 base_format);
// Effective CONTROL1 after RPCS3's per-format overrides (float formats, 16-bit-channel formats).
u32 effective_remap(u32 base_format, u32 remap);
// Apply a remap (already effective) to an ARGB-ordered base mapping; returns a VkComponentMapping (rgba).
VkComponentMapping apply_remap(const std::array<VkComponentSwizzle, 4>& base_argb, u32 remap);
// Full view mapping for a texture decoded from memory: base_mapping_argb + effective remap.
VkComponentMapping texture_mapping(u32 format_reg, u32 remap);

// Mapping for sampling a render target surface as a texture: the surface image holds RGBA in natural order
// (fragment output .r -> R etc.); return the view mapping that makes sampling it as texture format `fmt`
// with `remap` behave like the RSX.
VkComponentMapping surface_sample_mapping(u32 format_reg, u32 remap);

u64 hash_guest_range(u32 ea, u32 size);                   // xxh64 over guest memory, for change detection
u64 xxh64(const void* data, size_t len, u64 seed = 0);

// Exposed for tests: RSX swizzle (Z-order) <-> linear for a W x H x D grid of `unit`-byte elements
// (unit 1, 2 or 4). Dimensions need not be powers of two (index space is the next POT).
void deswizzle(const u8* src, u8* dst, u32 w, u32 h, u32 d, u32 unit, size_t src_size);
void swizzle(const u8* src, u8* dst, u32 w, u32 h, u32 d, u32 unit);

}  // namespace rsx
