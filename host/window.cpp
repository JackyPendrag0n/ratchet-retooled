// SDL2 front end: window + Vulkan surface for the renderer, keyboard/gamepad -> cellPad,
// cellAudio mixer -> SDL audio. The guest runs on its own thread; this thread pumps events.
#include "audio.h"
#include "cheats.h"
#include "coop.h"
#include "gfx.h"
#include "input.h"
#include "overlay.h"
#include "rsx.h"
#include "vk_backend.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace cheat_menu { void toggle_photo_mode(); }

namespace {

SDL_Window* g_window = nullptr;
SDL_AudioDeviceID g_audio = 0;
SDL_GameController* g_pad = nullptr;
SDL_GameController* g_pad2 = nullptr;   // co-op: player 2's controller

struct KeyBind { SDL_Scancode key; u16 d1, d2; };
// Digital1: SELECT 0x01 L3 0x02 R3 0x04 START 0x08 UP 0x10 RIGHT 0x20 DOWN 0x40 LEFT 0x80
// Digital2: L2 0x01 R2 0x02 L1 0x04 R1 0x08 TRIANGLE 0x10 CIRCLE 0x20 CROSS 0x40 SQUARE 0x80
const KeyBind kKeys[] = {
    {SDL_SCANCODE_UP, 0x10, 0},       {SDL_SCANCODE_RIGHT, 0x20, 0},  {SDL_SCANCODE_DOWN, 0x40, 0},
    {SDL_SCANCODE_LEFT, 0x80, 0},     {SDL_SCANCODE_RETURN, 0x08, 0}, {SDL_SCANCODE_TAB, 0x01, 0},
    {SDL_SCANCODE_SPACE, 0, 0x40},    {SDL_SCANCODE_C, 0, 0x20},      {SDL_SCANCODE_F, 0, 0x80},
    {SDL_SCANCODE_R, 0, 0x10},        {SDL_SCANCODE_Q, 0, 0x04},      {SDL_SCANCODE_E, 0, 0x08},
    {SDL_SCANCODE_1, 0, 0x01},        {SDL_SCANCODE_3, 0, 0x02},      {SDL_SCANCODE_LCTRL, 0x02, 0},
    {SDL_SCANCODE_V, 0x04, 0},
};

// Rumble from the game (cellPadSetActDirect) to the controller, scaled by the rumble setting.
// SDL rumble runs for a set time, so a held value is sent again before it runs out.
void update_rumble() {
    if (!g_pad) return;
    static u16 last_lo = 0, last_hi = 0;
    static u32 last_ms = 0;
    u8 small = 0, large = 0;
    pad_get_rumble(small, large);
    const int pct = std::clamp(gfx::g.rumble.load(), 0, 100);
    const u16 lo = u16(large * 257 * pct / 100), hi = u16(small * 257 * pct / 100);
    const u32 now = SDL_GetTicks();
    if (lo == last_lo && hi == last_hi && (!(lo | hi) || now - last_ms < 150)) return;
    SDL_GameControllerRumble(g_pad, lo, hi, 250);
    last_lo = lo; last_hi = hi; last_ms = now;
}

u8 axis_to_u8(int v) { return u8(std::clamp((v + 32768) >> 8, 0, 255)); }

void read_controller(SDL_GameController* pad, PadState& st) {
    auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) st.digital1 |= 0x10;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) st.digital1 |= 0x20;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) st.digital1 |= 0x40;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) st.digital1 |= 0x80;
    if (btn(SDL_CONTROLLER_BUTTON_START)) st.digital1 |= 0x08;
    if (btn(SDL_CONTROLLER_BUTTON_BACK)) st.digital1 |= 0x01;
    if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK)) st.digital1 |= 0x02;
    if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) st.digital1 |= 0x04;
    if (btn(SDL_CONTROLLER_BUTTON_A)) st.digital2 |= 0x40;
    if (btn(SDL_CONTROLLER_BUTTON_B)) st.digital2 |= 0x20;
    if (btn(SDL_CONTROLLER_BUTTON_X)) st.digital2 |= 0x80;
    if (btn(SDL_CONTROLLER_BUTTON_Y)) st.digital2 |= 0x10;
    if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) st.digital2 |= 0x04;
    if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) st.digital2 |= 0x08;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) st.digital2 |= 0x01;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) st.digital2 |= 0x02;
    auto ax = [&](SDL_GameControllerAxis a) { return int(SDL_GameControllerGetAxis(pad, a)); };
    auto dead = [](int v) { return std::abs(v) < 6000 ? 0 : v; };
    const int glx = dead(ax(SDL_CONTROLLER_AXIS_LEFTX)), gly = dead(ax(SDL_CONTROLLER_AXIS_LEFTY));
    const int grx = dead(ax(SDL_CONTROLLER_AXIS_RIGHTX)), gry = dead(ax(SDL_CONTROLLER_AXIS_RIGHTY));
    if (glx || gly) { st.lx = axis_to_u8(glx); st.ly = axis_to_u8(gly); }
    if (grx || gry) { st.rx = axis_to_u8(grx); st.ry = axis_to_u8(gry); }
}

// Co-op: D-pad Down is "teleport to partner" (pressed, not held) and is kept from the game.
void teleport_button(int player, PadState& st) {
    static bool held[2];
    const bool down = st.digital1 & 0x40;
    if (down && !held[player]) coop::request_teleport(player);
    held[player] = down;
    st.digital1 &= ~0x40;
}

void update_pad() {
    // co-op: the first controller is player 1; player 2 is the second controller and the keyboard
    // (the keyboard is player 1 only when no controller is connected)
    const bool coop = gfx::g.coop.load();
    if (g_config.cheat_menu) overlay::poll_controller(reinterpret_cast<_SDL_GameController*>(g_pad));
    // the cheat menu takes the keyboard and controller while it is open
    if (overlay::menu_open()) { pad_set_state(0, PadState{}); if (coop) pad_set_state(1, PadState{}); return; }
    PadState kb;
    const u8* k = SDL_GetKeyboardState(nullptr);
    for (const auto& b : kKeys) if (k[b.key]) { kb.digital1 |= b.d1; kb.digital2 |= b.d2; }
    auto stick = [&](SDL_Scancode neg, SDL_Scancode pos) -> int { return (k[pos] ? 127 : 0) - (k[neg] ? 127 : 0); };
    int lx = stick(SDL_SCANCODE_A, SDL_SCANCODE_D), ly = stick(SDL_SCANCODE_W, SDL_SCANCODE_S);
    int rx = stick(SDL_SCANCODE_J, SDL_SCANCODE_L), ry = stick(SDL_SCANCODE_I, SDL_SCANCODE_K);
    kb.lx = u8(128 + std::clamp(lx, -128, 127)); kb.ly = u8(128 + std::clamp(ly, -128, 127));
    kb.rx = u8(128 + std::clamp(rx, -128, 127)); kb.ry = u8(128 + std::clamp(ry, -128, 127));
    const bool kb_p2 = coop && g_pad;
    PadState st = kb_p2 ? PadState{} : kb;
    if (g_pad) read_controller(g_pad, st);
    cheats::filter_pad(st);
    if (coop) {
        PadState st2 = kb_p2 ? kb : PadState{};
        if (g_pad2) read_controller(g_pad2, st2);
        coop::filter_pad2(st2);
        teleport_button(0, st);
        teleport_button(1, st2);
        coop::note_pad1(st);
        coop::route_pads(st, st2);
        pad_set_state(1, st2);
    }
    pad_set_state(0, st);
    update_rumble();
}

void open_controller(int index) {
    SDL_GameController* c = SDL_GameControllerOpen(index);
    if (!c) return;
    if (!g_pad) { g_pad = c; LOG_NOTE("input: using %s", SDL_GameControllerName(c)); }
    else if (!g_pad2 && c != g_pad) { g_pad2 = c; LOG_NOTE("input: second controller (co-op player 2): %s", SDL_GameControllerName(c)); }
    else if (c != g_pad && c != g_pad2) SDL_GameControllerClose(c);
}

void open_audio() {
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_audio) { LOG_ERR("audio: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_audio, 0);
    audio_sink = [](const float* stereo, u32 frames) {
        // keep latency bounded: drop if more than ~100 ms is already queued
        if (SDL_GetQueuedAudioSize(g_audio) > 48000 * 8 / 10) return;
        SDL_QueueAudio(g_audio, stereo, frames * 8);
    };
}

}  // namespace

// Runs `game` on a new thread with a window; returns false if no window could be made.
bool run_windowed(const std::function<int()>& game, bool vsync) {
    // DualSense / DualShock 4 over Bluetooth only rumble in their enhanced report mode
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        LOG_ERR("SDL: %s", SDL_GetError());
        return false;
    }
    g_window = SDL_CreateWindow("Ratchet & Clank Future: Tools of Destruction", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) { LOG_ERR("SDL window: %s", SDL_GetError()); return false; }
    static rsx::VkPresentTarget target;
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(g_window, &n, nullptr);
    target.instance_extensions.resize(n);
    SDL_Vulkan_GetInstanceExtensions(g_window, &n, target.instance_extensions.data());
    target.create_surface = [](VkInstance inst) {
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(g_window, inst, &s)) LOG_ERR("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return s;
    };
    target.drawable_size = [](u32& w, u32& h) { int iw, ih; SDL_Vulkan_GetDrawableSize(g_window, &iw, &ih); w = u32(iw); h = u32(ih); };
    target.vsync = vsync;
    rsx::Renderer* r = rsx::create_renderer(&target);
    if (!r) { SDL_DestroyWindow(g_window); return false; }
    rsx::set_renderer(r);
    open_audio();
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_controller(i);

    std::atomic<bool> done{false};
    std::atomic<int> rc{0};
    std::thread guest([&] { rc = game(); done = true; });
    guest.detach();
    bool fullscreen = false;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (g_config.cheat_menu) {
                int ww, wh, dw, dh;
                SDL_GetWindowSize(g_window, &ww, &wh);
                SDL_Vulkan_GetDrawableSize(g_window, &dw, &dh);
                if (overlay::handle_event(e, ww ? float(dw) / ww : 1.f, wh ? float(dh) / wh : 1.f)) continue;
            }
            switch (e.type) {
            case SDL_QUIT: std::fflush(stderr); std::_Exit(0);
            case SDL_CONTROLLERDEVICEADDED:
                if (!g_pad || !g_pad2) open_controller(e.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_pad2 && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad2))) { SDL_GameControllerClose(g_pad2); g_pad2 = nullptr; }
                if (g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                    SDL_GameControllerClose(g_pad);
                    g_pad = g_pad2;   // the other controller becomes the first
                    g_pad2 = nullptr;
                }
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 || (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(g_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && fullscreen) { fullscreen = false; SDL_SetWindowFullscreen(g_window, 0); }
                if (e.key.repeat) break;
                // F12 screenshot; with the cheat menu on: F2 slow motion, F4 photo mode, F5 / F9 save and go back to a position
                if (e.key.keysym.scancode == SDL_SCANCODE_F12) gfx::g.screenshot = true;
                if (g_config.cheat_menu) {
                    if (e.key.keysym.scancode == SDL_SCANCODE_F2) cheats::toggle_slowmo();
                    if (e.key.keysym.scancode == SDL_SCANCODE_F4) cheat_menu::toggle_photo_mode();
                    if (e.key.keysym.scancode == SDL_SCANCODE_F5) cheats::save_position();
                    if (e.key.keysym.scancode == SDL_SCANCODE_F9) cheats::restore_position();
                }
                break;
            default: break;
            }
        }
        update_pad();
        SDL_Delay(4);
    }
    return true;
}
