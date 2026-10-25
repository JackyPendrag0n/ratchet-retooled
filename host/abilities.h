// Gameplay that comes with a skin pack (abilities.cpp): the glide.
#pragma once
#include "guest.h"
#include "ppu.h"

#include <string>

namespace abilities {
void on_hero_update(u32 hero);          // HOOKS 0x23E608, every hero (player 1 and co-op player 2)
void command(const std::string& args);  // "!glide trace n [1|2] | snap | diff | dump <off> <len>" (debugging)
}  // namespace abilities
