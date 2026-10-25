// Texture packs: replace any of the game's textures with a PNG, and dump the textures the game uses
// so they can be edited. A texture is named by the hash of its guest data (all mip levels), so the
// same file matches wherever the game loads it.
//   <data>/textures/<hash>*.png         replacements (any size; subfolders work too)
//   <data>/textures/dump/<hash>_WxH.png what the game drew, when texture_dump is on
#pragma once
#include "rsx_tex.h"
#include <string>
#include <vector>

namespace texpack {
using rsx::DecodedTex;
using rsx::TexLevel;

// Scans <data>/textures for replacements. Called once before the first texture is uploaded.
void init(const std::string& data_dir);
// Number of replacement files found.
size_t replacement_count();
// Queues a PNG of the top mip level of a decoded texture (2D, 8-bit or BC formats) unless that hash
// was dumped already. Written on a worker thread.
void dump(u64 hash, const DecodedTex& dec);
// The replacement for a texture as RGBA8 with a full mip chain (levels packed one after another,
// level n is max(1, w >> n) x max(1, h >> n)), or false when there is none or it can't be read.
bool replacement(u64 hash, std::vector<u8>& rgba, u32& w, u32& h, u32& mips);

// PNG helpers (8-bit RGBA). read_png accepts 8-bit grey, grey+alpha, RGB, RGBA and palette images.
bool read_png(const std::string& path, std::vector<u8>& rgba, u32& w, u32& h);
void write_png_rgba(const std::string& path, const u8* rgba, u32 w, u32 h);

}  // namespace texpack
