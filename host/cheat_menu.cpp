// The cheat and debug menu (F1 / L3+R3), built with Dear ImGui on the render thread.
#include "cheats.h"
#include "coop.h"
#include "weapon_xp.h"
#include "gfx.h"
#include "guest.h"

#include "imgui.h"
#include "overlay.h"
#include "overlay_notify.h"
#include "skins.h"
#include "tex_pack.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace cheat_menu {
namespace {

int g_give_level = 1;
std::map<int, int> g_dragging;

const char* level_name(const cheats::Snapshot& s, int v) {
    for (auto& l : s.levels) if (l.value == v) return l.label.c_str();
    return "?";
}

bool is_gadget_name(const std::string& n) {
    // devices and gadgets are listed after weapons in the game's own order; group by name
    static const char* kGadgets[] = {"SWINGSHOT", "DECRYPTOR", "VISICOPTER", "ROBOWINGS", "MAGCYCLE", "GELANATOR", "INFLATOPOD",
                                     "PIRATEGUISE", "TORNADO_LAUNCHER", "HYPERSHOT", "GRAV_BOOTS", "GEO_LASER", "CUTTING_LASER"};
    for (const char* g : kGadgets) if (n.find(g) != std::string::npos) return true;
    return false;
}
bool is_device_name(const std::string& n) {
    static const char* kDevices[] = {"GROOVITRON", "MINI_LEECH", "MAXI_LEECH", "CONFUSION_GAS", "ZURKON", "MORPH_BOMB", "SLINKONATOR", "SPLIT_SHOT"};
    for (const char* g : kDevices) if (n.find(g) != std::string::npos) return true;
    return false;
}

void weapons_tab(const cheats::Snapshot& s) {
    ImGui::SliderInt("Level for new items", &g_give_level, 1, 10, "V%d");
    if (ImGui::Button("Give everything")) cheats::give_all_weapons(g_give_level);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu owned)", s.weapon_level.size());
    if (s.weapons.empty()) { ImGui::TextDisabled("Waiting for a level to load..."); return; }
    auto section = [&](const char* title, auto pred) {
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) return;
        if (!ImGui::BeginTable(title, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("Owned", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 3);
        for (auto& w : s.weapons) {
            if (!pred(w.name) || w.name == "WPN_COUNT" || w.name == "WPN_NONE") continue;
            ImGui::PushID(w.value);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(w.label.c_str());
            ImGui::TableNextColumn();
            auto it = s.weapon_level.find(w.value);
            bool owned = it != s.weapon_level.end();
            if (w.value == 0) ImGui::BeginDisabled();   // the wrench stays
            if (ImGui::Checkbox("##own", &owned)) {
                if (owned) cheats::give_weapon(w.value, g_give_level);
                else cheats::remove_weapon(w.value);
            }
            if (w.value == 0) ImGui::EndDisabled();
            ImGui::TableNextColumn();
            auto mx = s.max_level.find(w.value);
            const int maxl = mx != s.max_level.end() ? mx->second : 1;
            if (it != s.weapon_level.end() && maxl > 1) {
                // the value being dragged is kept here and applied on release
                auto d = g_dragging.find(w.value);
                int lv = d != g_dragging.end() ? d->second : std::clamp(it->second, 1, maxl);
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##lvl", &lv, 1, maxl, "V%d");
                if (ImGui::IsItemActive()) g_dragging[w.value] = lv;
                if (ImGui::IsItemDeactivatedAfterEdit()) cheats::give_weapon(w.value, lv);
                if (!ImGui::IsItemActive()) g_dragging.erase(w.value);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    };
    section("Weapons", [](const std::string& n) { return !is_gadget_name(n) && !is_device_name(n); });
    section("Gadgets", is_gadget_name);
    section("Devices", is_device_name);
    if (ImGui::CollapsingHeader("Raritanium upgrades", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Raritanium: %d", std::max(0, s.raritanium));
        ImGui::SameLine();
        if (ImGui::Button("+1,000 raritanium")) cheats::add_raritanium(1000);
        ImGui::TextDisabled("Tick or untick single upgrades; changes are free and don't touch your raritanium.");
        if (s.mods.empty()) ImGui::TextDisabled("Reading upgrades...");
        for (auto& w : s.weapons) {
            auto it = s.mods.find(w.value);
            if (it == s.mods.end() || it->second.empty()) continue;
            int have = 0;
            int total = 0;
            for (auto& m : it->second) if (!m.name.empty()) { have += m.owned; ++total; }
            ImGui::PushID(1000 + w.value);
            const std::string title = w.label + "  (" + std::to_string(have) + "/" + std::to_string(total) + ")###mods";
            if (ImGui::TreeNode(title.c_str())) {
                if (ImGui::SmallButton("All")) cheats::set_all_mods(w.value, true);
                ImGui::SameLine();
                if (ImGui::SmallButton("None")) cheats::set_all_mods(w.value, false);
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (it->second[i].name.empty()) continue;   // slot 0: the grid's starting node, no upgrade of its own
                    bool on = it->second[i].owned;
                    ImGui::PushID(int(i));
                    if (ImGui::Checkbox(it->second[i].name.c_str(), &on)) cheats::set_mod(w.value, int(i), on);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void player_tab(const cheats::Snapshot& s) {
    cheats::Toggles t = cheats::toggles();
    if (ImGui::Checkbox("God mode", &t.god)) cheats::set_god(t.god);
    if (ImGui::Checkbox("Infinite ammo", &t.infinite_ammo)) cheats::set_infinite_ammo(t.infinite_ammo);
    if (ImGui::Checkbox("Remove Clank (Ratchet alone, like on Reepor)", &t.no_clank)) cheats::set_no_clank(t.no_clank);
    if (ImGui::Checkbox("No clip (fly through walls)", &t.noclip)) cheats::set_noclip(t.noclip);
    if (ImGui::Checkbox("Free camera", &t.freecam)) cheats::set_freecam(t.freecam);
    if (ImGui::Checkbox("Gelanator mod (always yours, always full of gel)", &t.gelanator)) cheats::set_gelanator(t.gelanator);
    if (t.noclip || t.freecam)
        ImGui::TextDisabled("Move: left stick / WASD, up/down: R1/L1 (E/Q), look: right stick / IJKL, faster: R2 (3)");
    ImGui::Separator();
    ImGui::Text("Bolts: %d", s.bolts);
    ImGui::SameLine();
    if (ImGui::Button("+100,000 bolts")) cheats::add_bolts(100000);
    ImGui::Separator();
    ImGui::TextUnformatted("Armor");
    for (auto& a : s.armors) {
        if (ImGui::RadioButton(a.label.c_str(), s.armor == a.value)) cheats::set_armor(a.value);
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void levels_tab(const cheats::Snapshot& s) {
    ImGui::Text("Current: %s", s.level >= 0 ? level_name(s, s.level) : "-");
    ImGui::TextDisabled("Warping loads the planet from its start, like travelling there with the ship.");
    if (s.levels.empty()) { ImGui::TextDisabled("Waiting for a level to load..."); return; }
    for (auto& l : s.levels) {
        if (l.name == "LEVEL_COUNT" || l.name == "LEVEL_NONE") continue;
        if (ImGui::Selectable(l.label.c_str(), l.value == s.level)) cheats::warp_to_level(l.value);
    }
}

// ---- Fun: the game's own cheats, game speed, photo mode, practice tools, skill points
bool g_speedometer = false;
std::atomic<bool> g_photo{false};   // also switched from the window thread (F4)
float g_photo_speed_before = 1.f;
bool g_photo_freecam_before = false;

void set_photo_mode(bool on) {
    if (on == g_photo) return;
    g_photo = on;
    if (on) {
        g_photo_speed_before = cheats::game_speed();
        g_photo_freecam_before = cheats::toggles().freecam;
        cheats::set_game_speed(0.001f);
        if (!g_photo_freecam_before) cheats::set_freecam(true);
        overlay_set_menu_open(false);
        overlay_notify("Photo mode: the action is frozen. Fly the camera, F12 takes a picture, F4 to leave.");
    } else {
        cheats::set_game_speed(g_photo_speed_before);
        if (!g_photo_freecam_before) cheats::set_freecam(false);
    }
}

void fun_tab(const cheats::Snapshot& s) {
    ImGui::SeparatorText("Co-op (experimental)");
    bool coop = gfx::g.coop.load();
    if (ImGui::Checkbox("Two players, split screen", &coop)) {
        gfx::g.coop = coop;
        gfx::save_setting("coop", coop ? "true" : "false");
        overlay_notify(coop ? "Co-op on: player 2 joins on the second controller in a moment"
                            : "Co-op off: player 2 leaves at the next planet or restart");
    }
    ImGui::TextDisabled("Player 2 uses the second controller (with one controller, the keyboard is player 1).");
    ImGui::TextDisabled("Weapons, bolts, XP and health are shared; you can't both hold the same weapon.");
    ImGui::TextDisabled("D-pad Down: teleport to your partner.");
    ImGui::SeparatorText("Pause menu");
    bool xp = weapon_xp::enabled();
    if (ImGui::Checkbox("Weapon XP on the weapons page", &xp)) {
        weapon_xp::set_enabled(xp);
        gfx::save_setting("weapon_xp", xp ? "true" : "false");
        overlay_notify(xp ? "Weapon XP on: the weapons page shows XP / next level" : "Weapon XP off");
    }
    ImGui::TextDisabled("First line of the weapon's description, e.g. XP: 700 / 1,000.");
    ImGui::SeparatorText("The game's own cheats");
    if (s.game_cheats.empty()) ImGui::TextDisabled("Waiting for a level to load...");
    for (auto& c : s.game_cheats) {
        if (c.states.size() < 2) continue;   // the Extras menu's videos and art galleries
        ImGui::PushID(c.id);
        ImGui::TextUnformatted(c.name.c_str());
        for (size_t k = 0; k < c.states.size(); ++k) {
            ImGui::SameLine();
            if (ImGui::RadioButton(c.states[k].c_str(), c.state == int(k))) cheats::set_game_cheat(c.id, int(k));
        }
        ImGui::PopID();
    }
    bool st = cheats::stache();
    if (ImGui::Checkbox("Ratchet's moustache", &st)) cheats::set_stache(st);
    if (ImGui::Button("Unlock everything in the game's Extras menu")) cheats::unlock_all_game_cheats();
    ImGui::SameLine();
    ImGui::TextDisabled("(videos, concept art)");
    ImGui::TextDisabled("These are the cheats the game normally unlocks with skill points; they work without unlocking.");

    ImGui::SeparatorText("Game speed");
    {
        float sp = g_photo ? g_photo_speed_before : cheats::game_speed();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::SliderFloat("##speed", &sp, 0.1f, 3.f, "%.2fx", ImGuiSliderFlags_Logarithmic) && !g_photo) cheats::set_game_speed(sp);
        ImGui::SameLine();
        if (ImGui::Button("Normal") && !g_photo) cheats::set_game_speed(1.f);
        float sm = cheats::slowmo_preset();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::SliderFloat("F2 slow motion", &sm, 0.1f, 0.9f, "%.2fx")) cheats::set_slowmo_preset(sm);
        ImGui::TextDisabled("F2 switches slow motion on and off. The game's own slow-downs (Clank's time pads) still work at 1x.");
    }

    ImGui::SeparatorText("Photo mode");
    {
        bool ph = g_photo.load();
        if (ImGui::Checkbox("Photo mode (F4): freeze the action and fly the camera", &ph)) set_photo_mode(ph);
        ImGui::TextDisabled("F12 saves a screenshot at the render resolution (Graphics tab: 3x or 4x for big pictures),");
        ImGui::TextDisabled("to Pictures/TOD Recomp.");
    }

    ImGui::SeparatorText("Practice");
    {
        if (ImGui::Button("Save position (F5)")) cheats::save_position();
        ImGui::SameLine();
        if (!cheats::have_saved_position()) ImGui::BeginDisabled();
        if (ImGui::Button("Go back (F9)")) cheats::restore_position();
        if (!cheats::have_saved_position()) ImGui::EndDisabled();
        ImGui::Checkbox("Speedometer and position on screen", &g_speedometer);
    }

    if (!s.skills.empty()) {
        int done = 0;
        for (auto& k : s.skills) done += k.done;
        const std::string title = "Skill points  (" + std::to_string(done) + "/" + std::to_string(s.skills.size()) + ")###skills";
        if (ImGui::CollapsingHeader(title.c_str())) {
            ImGui::TextDisabled("Tick one to award it. Hover for how to earn it the normal way.");
            for (auto& k : s.skills) {
                bool d = k.done;
                ImGui::PushID(2000 + k.id);
                if (d) ImGui::BeginDisabled();
                if (ImGui::Checkbox(k.name.c_str(), &d) && d) cheats::set_skill_done(k.id);
                if (k.done) ImGui::EndDisabled();
                if (!k.desc.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", k.desc.c_str());
                ImGui::PopID();
            }
        }
    }
}

void graphics_tab() {
    ImGui::Text("%.0f fps", ImGui::GetIO().Framerate);
    ImGui::SeparatorText("Resolution");
    {
        static const char* kScales[] = {"1x (1280x720, as on PS3)", "2x (2560x1440)", "3x (3840x2160)", "4x (5120x2880)"};
        const int cur = gfx::g.resolution_scale.load();
        for (int i = 1; i <= 4; ++i)
            if (ImGui::RadioButton(kScales[i - 1], cur == i)) {
                gfx::g.resolution_scale = i;
                gfx::save_setting("resolution_scale", std::to_string(i));
            }
        if (!g_config.native_renderer) ImGui::TextDisabled("Needs renderer = native in settings.ini.");
        ImGui::TextWrapped("The game is drawn at this size and scaled to your window, which also smooths edges (supersampling). "
                           "Higher costs more GPU time.");
    }
    ImGui::SeparatorText("Anti-aliasing");
    {
        int aa = gfx::g.aa.load();
        if (ImGui::RadioButton("Off", aa == gfx::AA_OFF)) aa = gfx::AA_OFF;
        ImGui::SameLine();
        if (ImGui::RadioButton("FXAA", aa == gfx::AA_FXAA)) aa = gfx::AA_FXAA;
        if (aa != gfx::g.aa.load()) { gfx::g.aa = aa; gfx::save_setting("antialiasing", aa == gfx::AA_FXAA ? "fxaa" : "off"); }
        ImGui::TextDisabled("FXAA applies at 1x; at 2x and above the downscale already smooths edges.");
    }
    ImGui::SeparatorText("Textures and shadows");
    {
        static const int kAniso[] = {0, 2, 4, 8, 16};
        const int cur = gfx::g.anisotropy.load();
        ImGui::TextUnformatted("Anisotropic filtering");
        for (int a : kAniso) {
            ImGui::SameLine();
            const std::string label = a ? std::to_string(a) + "x" : std::string("Game");
            if (ImGui::RadioButton(label.c_str(), cur == a)) { gfx::g.anisotropy = a; gfx::save_setting("anisotropic_filtering", std::to_string(a)); }
        }
        bool soft = gfx::g.soft_shadows.load();
        if (ImGui::Checkbox("Soft shadows", &soft)) { gfx::g.soft_shadows = soft; gfx::save_setting("soft_shadows", soft ? "true" : "false"); }
    }
    ImGui::SeparatorText("Texture pack");
    {
        bool dump = gfx::g.texture_dump.load();
        if (ImGui::Checkbox("Save textures the game draws (textures/dump in the data folder)", &dump)) {
            gfx::g.texture_dump = dump;
            gfx::save_setting("texture_dump", dump ? "true" : "false");
        }
        ImGui::TextWrapped("Edit a saved PNG and put it in the textures folder (keep the start of its name): it replaces "
                           "that texture from the next level load. %zu replacement(s) loaded.", texpack::replacement_count());
    }
    ImGui::SeparatorText("Frame rate limit");
    {
        static const int kRates[] = {30, 60, 120, 144, 165, 240, 0};
        const int cur = cheats::frame_limit();
        for (int r : kRates) {
            const std::string label = r ? std::to_string(r) + " fps" : std::string("Uncapped");
            if (r != 30) ImGui::SameLine();
            if (ImGui::RadioButton(label.c_str(), cur == r)) {
                cheats::set_frame_limit(r);
                gfx::save_setting("framerate", std::to_string(r));
            }
        }
        ImGui::TextWrapped("30 and 60 are the game's own modes. Above 60 the game measures each frame's time instead of "
                           "assuming a fixed step. Turn vsync off in settings.ini to go past your monitor's refresh rate.");
    }
    ImGui::TextDisabled("Changes here are saved to settings.ini.");
}


// ---- Skins: a card for every skin (the game's nine, then the packs in <data>/skins), click to wear
std::atomic<bool> g_show_skins{false};   // F3: open on this tab
std::atomic<bool> g_wide{false};         // and widen the window for the cards
int g_skin_player = 0;
char g_skin_filter[64] = "";
int g_skin_kind = 0;                     // 0 all, 1 packs, 2 game skins
int g_skin_focus = -2;                   // the card the details line describes (hovered, else the worn one)

std::string lower_ascii(std::string s) { for (char& ch : s) ch = char(std::tolower(u8(ch))); return s; }

std::string initials(const std::string& t) {
    std::string r;
    bool start = true;
    for (char ch : t) {
        if (std::isalnum(u8(ch)) && start && r.size() < 2) r += char(std::toupper(u8(ch)));
        start = !std::isalnum(u8(ch));
    }
    return r.empty() ? "?" : r;
}

ImU32 hue_of(const std::string& t, float s, float v, float a = 1.f) {
    u32 h = 2166136261u;
    for (char ch : t) h = (h ^ u8(ch)) * 16777619u;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(float(h % 360) / 360.f, s, v, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

std::string ability_names(u32 a) {
    std::string r;
    if (a & skins::kAbilityGlide) r += "Long glide";
    if (a & ~skins::kAbilityGlide) r += r.empty() ? "Abilities" : ", more";
    return r;
}

// one card; true when clicked
bool skin_card(int e, const skins::EntryInfo& in, float w, bool worn_p1, bool worn_p2, bool locked) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float pic = w, lh = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 size(w, pic + lh * 2.6f);
    ImGui::PushID(e);
    const bool clicked = ImGui::InvisibleButton("card", size) && !locked;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool worn = g_skin_player == 0 ? worn_p1 : worn_p2;
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 8.f);
    // the picture: the pack's preview.png, or its initials on a colour of its own
    const ImVec2 q0(p0.x + 6, p0.y + 6), q1(p1.x - 6, p0.y + pic - 6);
    int tw = 0, th = 0;
    void* tex = in.preview.empty() ? nullptr : overlay::thumbnail(in.preview, tw, th);
    if (tex && tw > 0 && th > 0) {
        // fill the square, cropping the longer side
        const float ar = float(tw) / float(th);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ar > 1) { uv0.x = 0.5f - 0.5f / ar; uv1.x = 0.5f + 0.5f / ar; } else { uv0.y = 0.5f - 0.5f * ar; uv1.y = 0.5f + 0.5f * ar; }
        dl->AddImageRounded(ImTextureID(tex), q0, q1, uv0, uv1, IM_COL32_WHITE, 6.f);
    } else {
        dl->AddRectFilledMultiColor(q0, q1, hue_of(in.title, 0.55f, 0.55f), hue_of(in.title, 0.55f, 0.55f),
                                    hue_of(in.title, 0.7f, 0.25f), hue_of(in.title, 0.7f, 0.25f));
        const std::string ini = initials(in.title);
        ImFont* f = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * 2.6f;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, ini.c_str());
        dl->AddText(f, fs, ImVec2((q0.x + q1.x - ts.x) * 0.5f, (q0.y + q1.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 220), ini.c_str());
    }
    if (locked) {
        dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, 150), 6.f);
        const char* t = "Locked";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2((q0.x + q1.x - ts.x) * 0.5f, (q0.y + q1.y - ts.y) * 0.5f), IM_COL32(255, 220, 120, 255), t);
    }
    // badges along the picture's top: who wears it, and the abilities
    float bx = q0.x + 4;
    auto badge = [&](const char* t, ImU32 col) {
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(bx, q0.y + 4), ImVec2(bx + ts.x + 8, q0.y + 6 + ts.y), col, 4.f);
        dl->AddText(ImVec2(bx + 4, q0.y + 5), IM_COL32_WHITE, t);
        bx += ts.x + 12;
    };
    if (worn_p1) badge("P1", IM_COL32(40, 120, 230, 230));
    if (worn_p2) badge("P2", IM_COL32(220, 70, 60, 230));
    if (in.abilities & skins::kAbilityGlide) badge("Glide", IM_COL32(230, 130, 30, 230));
    // the name, and what it is
    ImGui::PushClipRect(ImVec2(p0.x + 6, p0.y), ImVec2(p1.x - 6, p1.y), true);
    dl->AddText(ImVec2(p0.x + 8, p0.y + pic), ImGui::GetColorU32(ImGuiCol_Text), in.title.c_str());
    const std::string sub = in.pack ? "Custom skin" : e == 0 ? "The original" : "Game skin";
    dl->AddText(ImVec2(p0.x + 8, p0.y + pic + lh), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
    ImGui::PopClipRect();
    if (worn) dl->AddRect(p0, p1, g_skin_player == 0 ? IM_COL32(70, 150, 255, 255) : IM_COL32(255, 90, 80, 255), 8.f, 0, 3.f);
    else if (hov) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 8.f, 0, 1.5f);
    if (hov) g_skin_focus = e;
    return clicked;
}

void skins_tab(const cheats::Snapshot& s) {
    const bool coop_on = coop::status().active;
    if (!coop_on) g_skin_player = 0;
    if (coop_on) {
        ImGui::TextUnformatted("Choose for");
        ImGui::SameLine();
        ImGui::RadioButton("Player 1", &g_skin_player, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Player 2", &g_skin_player, 1);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##find", "Search skins", g_skin_filter, sizeof g_skin_filter);
    ImGui::SameLine();
    ImGui::RadioButton("All", &g_skin_kind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Custom", &g_skin_kind, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Game", &g_skin_kind, 2);
    if (!s.in_level) ImGui::TextDisabled("The skin you pick goes on once you are in a level.");

    const int n = skins::entry_count();
    const int p1 = skins::worn(0), p2 = skins::worn(1);
    const std::string find = lower_ascii(g_skin_filter);
    const int focus_was = g_skin_focus;
    g_skin_focus = -2;

    // the cards, as many across as fit; the details line stays below
    const float detail_h = ImGui::GetTextLineHeightWithSpacing() * 4.2f;
    ImGui::BeginChild("cards", ImVec2(0, -detail_h), false);
    const float avail = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x;
    const float want = std::max(110.f, ImGui::GetFontSize() * 9.f);
    const int cols = std::max(1, int((avail + gap) / (want + gap)));
    const float w = std::floor((avail - gap * float(cols - 1)) / float(cols));
    int col = 0;
    auto place = [&] { if (col++ % cols) ImGui::SameLine(); };
    // player 2 can also look like player 1
    if (g_skin_player == 1 && g_skin_kind != 1 && (find.empty() || std::string("same as player 1").find(find) != std::string::npos)) {
        skins::EntryInfo same;
        same.title = "Same as player 1";
        place();
        if (skin_card(-1, same, w, false, p2 < 0, false)) skins::wear(1, -1);
    }
    // the custom skins first, then the game's own
    std::vector<int> order;
    for (int e = skins::kGameSkinCount; e < n; ++e) order.push_back(e);
    for (int e = 0; e < std::min(n, skins::kGameSkinCount); ++e) order.push_back(e);
    for (int e : order) {
        const skins::EntryInfo in = skins::entry_info(e);
        if (g_skin_kind == 1 && !in.pack) continue;
        if (g_skin_kind == 2 && in.pack) continue;
        if (!find.empty() && lower_ascii(in.title + " " + in.folder + " " + in.desc).find(find) == std::string::npos) continue;
        place();
        if (skin_card(e, in, w, p1 == e, p2 == e, !in.owned)) {
            skins::wear(g_skin_player, e);
            overlay::notify("Player %d: %s", g_skin_player + 1, in.title.c_str());
        }
    }
    if (col == 0) ImGui::TextDisabled("No skin matches.");
    ImGui::EndChild();

    // details of the hovered card, else of the one this player wears
    ImGui::Separator();
    int fe = g_skin_focus != -2 ? g_skin_focus : focus_was;
    if (fe == -2) fe = g_skin_player == 0 ? p1 : p2;
    if (fe == -1 && g_skin_player == 1) { ImGui::TextUnformatted("Same as player 1"); ImGui::TextDisabled("Player 2 looks like player 1."); return; }
    if (fe < 0 || fe >= n) return;
    const skins::EntryInfo in = skins::entry_info(fe);
    ImGui::TextUnformatted(in.title.c_str());
    if (in.pack) {
        ImGui::SameLine();
        ImGui::TextDisabled("  skins/%s  -  %u vertices, %u triangles  -  drawn as %s", in.folder.c_str(), in.vertices, in.triangles,
                            skins::entry_info(in.carrier).title.c_str());
    }
    if (!in.desc.empty()) ImGui::TextWrapped("%s", in.desc.c_str());
    else if (!in.owned) ImGui::TextDisabled("Unlock it in the game's Extras > Skins menu first.");
    if (in.abilities) ImGui::TextColored(ImVec4(1, 0.65f, 0.25f, 1), "Gameplay: %s", ability_names(in.abilities).c_str());
}

}  // namespace

void show_skins() { g_show_skins = true; g_wide = true; }

void build() {
    const cheats::Snapshot s = cheats::snapshot();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.05f, ds.y * 0.06f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ds.x * 0.42f, ds.y * 0.86f), ImGuiCond_FirstUseEver);
    if (g_wide.exchange(false)) {
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.08f, ds.y * 0.06f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(ds.x * 0.84f, ds.y * 0.88f), ImGuiCond_Always);
    }
    ImGui::Begin("TOD Recomp  (F1 / L3+R3 to close, F3 skins)", nullptr, ImGuiWindowFlags_NoCollapse);
    if (!s.in_level) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Most cheats work once you are playing in a level.");
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Player")) { player_tab(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Weapons")) { weapons_tab(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Fun")) { fun_tab(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Level warp")) { levels_tab(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Graphics")) { graphics_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Skins", nullptr, g_show_skins.exchange(false) ? ImGuiTabItemFlags_SetSelected : 0)) { skins_tab(s); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void toggle_photo_mode() { set_photo_mode(!g_photo); }

bool hud_wanted() { return g_speedometer || g_photo || cheats::game_speed() != 1.f; }

// Always on screen (menu open or not): the speedometer and the game speed when it isn't 1x.
void build_hud() {
    if (!g_speedometer && !g_photo && cheats::game_speed() == 1.f) return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x - 12.f, ds.y - 12.f), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (!ImGui::Begin("##hud", nullptr, fl)) { ImGui::End(); return; }
    if (g_photo) ImGui::TextUnformatted("PHOTO MODE  (F12 picture, F4 to leave)");
    else if (cheats::game_speed() != 1.f) ImGui::Text("Game speed %.2fx  (F2)", cheats::game_speed());
    if (g_speedometer) {
        float p[3], v = 0;
        if (cheats::hero_motion(p, v)) {
            ImGui::Text("%5.1f m/s  %5.1f km/h", v, v * 3.6f);
            ImGui::Text("x %.1f  y %.1f  z %.1f", p[0], p[1], p[2]);
        } else ImGui::TextDisabled("not in a level");
    }
    ImGui::End();
}

}  // namespace cheat_menu
