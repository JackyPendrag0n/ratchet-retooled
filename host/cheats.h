// Cheats and debug tools: Lua run inside the game, and the state the cheat menu shows.
#pragma once
#include "guest.h"
#include "input.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cheats {

// Queue a Lua chunk; it runs on the game's script thread before the next script update.
void run_lua(std::string chunk);
// True once a level's scripts have run (the hero and the bindings exist).
bool script_thread_seen();
// Headless tests: "!pad" / "!shot" in the command file, polled at each flip (works in the pause menu).
void poll_input_commands();

// The hero's moby (noted when a queued chunk calls get_pos(Ratchet1)); 0 before that.
u32 hero_moby();
// Call a guest function (descriptor) from a Hooks::frame callback, on the game's main thread.
u64 call(u32 opd, std::initializer_list<u64> args);
// Same for a function with no descriptor: its entry address and the TOC it expects in r2.
u64 call_code(u32 code, u32 toc, std::initializer_list<u64> args);
u64 call_code_f1(u32 code, u32 toc, std::initializer_list<u64> args, double f1);   // f1 set too
double call_code_f(u32 code, u32 toc, std::initializer_list<u64> args);   // result in f1

struct Hooks {
    std::function<void()> frame;                     // once per frame on the script thread
    std::function<void(const std::string&)> output;  // "@TOD..." lines from __tod_out() in queued chunks
};
void set_hooks(const Hooks& h);

// ---- game state for the menu (filled from Lua; see cheat_state.cpp) ----
struct Named { std::string name, label; int value = 0; };
struct Snapshot {
    bool in_level = false;            // a hero exists (level loaded, not in the front end)
    int level = -1, armor = -1, bolts = 0, health = 0;
    std::map<int, int> weapon_level;  // owned weapon -> level (1-based)
    std::map<int, int> max_level;     // weapon -> highest level (1 = no upgrades)
    std::vector<Named> weapons, levels, armors;
    struct Mod { std::string name; bool owned = false; };
    std::map<int, std::vector<Mod>> mods;  // owned weapon -> its raritanium upgrades, in the game's order
    int raritanium = -1;
    // the game's own cheats (Extras menu: big heads, mirrored levels, wrench skins...) and skill points
    struct GameCheat { int id = 0; std::string name; bool unlocked = false; int state = 0; std::vector<std::string> states; };
    std::vector<GameCheat> game_cheats;
    struct Skill { int id = 0; std::string name, desc; bool done = false; };
    std::vector<Skill> skills;
    u64 serial = 0;
};
Snapshot snapshot();

struct Toggles {
    bool god = false, infinite_ammo = false, no_clank = false, noclip = false, freecam = false;
    bool gelanator = false;   // fun mod: the Gelanator is always owned and always full of gel
};
Toggles toggles();
void set_god(bool on);
void set_infinite_ammo(bool on);
void set_no_clank(bool on);
void set_noclip(bool on);
void set_freecam(bool on);
void give_weapon(int weapon, int level);   // also changes the level of an owned weapon
void give_all_weapons(int level);
void remove_weapon(int weapon);           // not the wrench
void set_armor(int armor);
void set_mod(int weapon, int mod, bool owned);   // add or remove one raritanium upgrade, free
void set_all_mods(int weapon, bool owned);
void set_gelanator(bool on);
void add_raritanium(int n);
void warp_to_level(int level);
void add_bolts(int n);
void set_game_cheat(int id, int state);   // the game's own cheats (Extras menu)
void unlock_all_game_cheats();
void set_skill_done(int id);
void refresh_extras();                     // re-read game cheats and skill points
void set_stache(bool on);                  // Ratchet's moustache (the game's own ratchet_set_stache)
bool stache();

// Game speed (slow motion / fast forward), 1 = normal. toggle_slowmo switches between 1 and the
// slow-motion preset (F2).
float game_speed();
void set_game_speed(float s);
void toggle_slowmo();
float slowmo_preset();
void set_slowmo_preset(float s);

// Practice tools (cheat_fly.cpp): remember where the hero stands and put it back there (F5 / F9).
void save_position();
void restore_position();
bool have_saved_position();
// The hero's position and speed (m/s), for the speedometer; false outside a level.
bool hero_motion(float pos[3], float& speed);

// Frame rate: 30 and 60 are the game's fixed-step modes; above 60 the game runs with a
// measured (variable) time step, capped at that rate. 0 = no cap.
int frame_limit();
void set_frame_limit(int fps);

// Free camera and no clip (cheat_fly.cpp). Called with the pad state before the game sees it:
// while flying, the sticks, R1 (rise), L1 (sink) and R2 (fast) steer and are taken out of it.
void filter_pad(PadState& st);
void hold_test_pad(const PadState& st);   // scripted tests: stands in for the controller until released
// From the game's code (HOOKS): the camera's world matrix, and the hero about to update its moby.
void on_camera(u8* base, u32 cam);
void on_hero_update(u8* base, u32 hero);

// Reads the cheat settings from settings.ini values (called by main.cpp).
void apply_settings(const std::map<std::string, std::string>& kv);

}  // namespace cheats
