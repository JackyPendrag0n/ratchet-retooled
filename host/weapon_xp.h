// Weapon XP in the pause menu: the weapons page's description starts with how much XP the selected weapon
// has and how much its next level needs (see weapon_xp.cpp).
#pragma once
#include "guest.h"

namespace weapon_xp {
void configure(bool on);          // settings.ini weapon_xp
bool enabled();
void set_enabled(bool on);
bool on_hook(PPUContext& c, u32 site);   // HOOKS 0x2F938, 0x33B50, 0x74F0F4
}  // namespace weapon_xp
