#pragma once
#include "ppu.h"
struct PPUFunctionEntry { u32 address; PPUFunc fn; };
struct PPUImportEntry { u32 index; const char* library; u32 nid; u32 stub; u32 slot; };
extern const PPUFunctionEntry ppu_function_table[];
extern const unsigned ppu_function_count;
extern const PPUImportEntry ppu_import_table[];
extern const unsigned ppu_import_count;
