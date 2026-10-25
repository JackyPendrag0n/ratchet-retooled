// Split-screen co-op (experimental): see coop.cpp.
#pragma once
#include "guest.h"
#include "ppu.h"
#include "input.h"

#include <string>

namespace coop {
void command(const std::string& line);   // "!co ..." from the command file
void pad2_command(const std::string& line);   // "pad2 ..." right away (from the flip, also while paused)
void tick();                             // once per frame on the game's main thread (script update)
void on_slot_lookup(PPUContext& c);      // hook at the player-slot lookups (sub_1E1298, sub_1E1120)
void on_set_fov(PPUContext& c);          // hook where a view's field of view is set (sub_639948)
// sub_658E48 (the hero's view): player 2's hero, outside its own update, gets the second view
void on_hero_view(PPUContext& c);
// sub_4E8D18 (foliage wind upload): at its entry, and after its load of the table pointer (load = true)
void on_sway(PPUContext& c, bool load);
void on_level_change();
void on_level_file(const std::string& name);
void on_hud_draw(PPUContext& c, bool after);
void on_widget_draw(PPUContext& c);                  // hook in sub_2D16A0 once a level change goes ahead
void on_dispatch(PPUContext& c);         // hook at the draw bucket dispatch (sub_4BE238)
void filter_pad2(PadState& st);          // input thread: a held test state ("!co pad2") replaces player 2's controller
void note_pad1(const PadState& st);      // input thread: player 1's pad (the quick select goes to whoever holds triangle)
// Whose pause menu is open (or opening): 2 when player 2 pressed start, else 1. Player 2's pad
// drives the menu then (route_pads).
int pause_player();
void note_flip();   // at each flip: tells a paused game from a running one
void route_pads(PadState& p1, PadState& p2);   // input thread: player 2 borrows player 1's quick select
void request_teleport(int player);       // input thread: player 0/1 pressed "teleport to partner"

// What the overlay shows for each player while co-op is running (the game's HUD is player 1's).
struct Status {
    bool active = false;
    float health[2] = {}, max_health[2] = {};
    int weapon[2] = {-1, -1};
};
Status status();
bool in_p2_update();                     // true while player 2's update runs (their slot swapped into slot 0)
u32 hero2();                             // player 2's hero object, 0 when there is none
}  // namespace coop
