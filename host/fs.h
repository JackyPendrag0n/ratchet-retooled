#pragma once
#include "guest.h"

void fs_syscall(PPUContext& c, u32 num);
void fs_register_imports();

// Movie skipping (hle_io.cpp): fs_open reports each Bink file it opens.
void movie_file_opened(const std::string& guest_path);
