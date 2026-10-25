#pragma once
#include "guest.h"
#include "ppu.h"

#include <string>

// Skin packs (skins.cpp): replacement models for Ratchet, listed in the skins menu as new skins after the game's own.
namespace skins {
void configure(const std::string& name, const std::string& data_dir);   // settings.ini skin = <name> (player 1)
void configure2(const std::string& name);        // settings.ini skin2 = <name> (co-op player 2)
void level_archive(const std::string& host_path);  // fs: a file is about to be opened
std::string movie_override(const std::string& guest_path, const std::string& host_path);  // a pack's menu movie
bool on_menu_hook(PPUContext& c, u32 site);      // the skin functions behind the skins menu; true = skip the function
void on_hero_update(u32 hero);                   // each hero update: writes the worn packs into Ratchet's model when needed
void command(int entry);                         // "!skin n": n < 9 a game skin, 9.. a pack (-1 lists them)
void command2(int entry);                        // "!skin2 n": the same for co-op player 2 (-1 = as player 1)
// texture pack lookups: the PNG a worn pack puts on a texture, and a counter that changes whenever that set does
bool texture_for(u64 hash, std::string& png);
u32 texture_generation();
size_t texture_count();
// the skins list (game skins 0..8, then the packs) for the overlay, and who wears what
constexpr int kGameSkinCount = 9;
int entry_count();
std::string entry_name(int entry);
int worn(int player);
// what the skin picker (cheat_menu.cpp) shows for an entry
struct EntryInfo {
    std::string title, desc, folder, preview;   // preview: <pack>/preview.png when there is one
    bool pack = false, owned = true;
    int carrier = -1;                            // a pack's game skin
    u32 abilities = 0, vertices = 0, triangles = 0;
};
EntryInfo entry_info(int entry);                            // player 1: the entry worn; player 2: -1 when it follows player 1
void wear(int player, int entry);
// gameplay a pack brings along (skin.bin flags, tod_skin.py --ability): what the pack a player wears has
constexpr u32 kAbilityGlide = 0x2;               // hold X after a double jump: a long glide (abilities.cpp)
u32 abilities(int player);
void clobber();   // testing: the game's bytes back under the layout
}  // namespace skins
