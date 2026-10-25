// Overlay entry points usable without the renderer (weak no-op defaults in cheat_state.cpp).
#pragma once

void overlay_notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool overlay_menu_open();
void overlay_set_menu_open(int open);   // 2: open on the Skins tab
void overlay_set_menu_open(int open);   // 2: open on the Skins tab
