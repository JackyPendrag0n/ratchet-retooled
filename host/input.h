// Host input state shared between the window/input backend and cellPad.
#pragma once
#include "guest.h"

struct PadState {
    u16 digital1 = 0;  // CELL_PAD_CTRL_LEFT/DOWN/RIGHT/UP/START/R3/L3/SELECT
    u16 digital2 = 0;  // CELL_PAD_CTRL_SQUARE/CROSS/CIRCLE/TRIANGLE/R1/L1/R2/L2
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    bool connected = true;
};

// Called by the input backend whenever the host controller/keyboard state changes.
void pad_set_state(u32 port, const PadState& s);
PadState pad_get_state(u32 port);
// Rumble the game asked for on port 0 (cellPadSetActDirect): small motor 0/255, large 0-255.
void pad_get_rumble(u8& small, u8& large);
