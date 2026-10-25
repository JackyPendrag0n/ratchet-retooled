// TOD_PAD_SCRIPT="start@600+20,cross@900+10,up@1200+60": scripted pad presses keyed by flip
// count (button@first_frame+duration_frames), for headless testing without a controller.
// Stick directions push a stick fully: lsup, lsdown, lsleft, lsright, rsup, rsdown, rsleft, rsright.
#include "input.h"
#include "rsx.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Press { u16 d1, d2; u64 from, to; int stick = -1; u8 value = 0x80; };   // stick: 0 lx, 1 ly, 2 rx, 3 ry

bool stick_dir(const std::string& n, Press& p) {
    static const struct { const char* name; int stick; u8 value; } table[] = {
        {"lsleft", 0, 0x00}, {"lsright", 0, 0xff}, {"lsup", 1, 0x00}, {"lsdown", 1, 0xff},
        {"rsleft", 2, 0x00}, {"rsright", 2, 0xff}, {"rsup", 3, 0x00}, {"rsdown", 3, 0xff},
    };
    for (auto& t : table) if (n == t.name) { p.stick = t.stick; p.value = t.value; return true; }
    return false;
}

bool button_bits(const std::string& n, u16& d1, u16& d2) {
    static const struct { const char* name; u16 d1, d2; } table[] = {
        {"select", 1 << 0, 0}, {"l3", 1 << 1, 0}, {"r3", 1 << 2, 0}, {"start", 1 << 3, 0},
        {"up", 1 << 4, 0}, {"right", 1 << 5, 0}, {"down", 1 << 6, 0}, {"left", 1 << 7, 0},
        {"l2", 0, 1 << 0}, {"r2", 0, 1 << 1}, {"l1", 0, 1 << 2}, {"r1", 0, 1 << 3},
        {"triangle", 0, 1 << 4}, {"circle", 0, 1 << 5}, {"cross", 0, 1 << 6}, {"square", 0, 1 << 7},
    };
    for (auto& t : table) if (n == t.name) { d1 = t.d1; d2 = t.d2; return true; }
    return false;
}
}  // namespace

void start_pad_script() {
    const char* env = std::getenv("TOD_PAD_SCRIPT");
    if (!env) return;
    std::vector<Press> presses;
    std::string s = env;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t at = item.find('@');
        if (at == std::string::npos) continue;
        Press p{};
        if (!stick_dir(item.substr(0, at), p) && !button_bits(item.substr(0, at), p.d1, p.d2)) { LOG_ERR("pad script: unknown button in '%s'", item.c_str()); continue; }
        p.from = std::strtoull(item.c_str() + at + 1, nullptr, 10);
        const size_t plus = item.find('+');
        p.to = p.from + (plus == std::string::npos ? 10 : std::strtoull(item.c_str() + plus + 1, nullptr, 10));
        presses.push_back(p);
    }
    std::thread([presses] {
        u16 last1 = 0xffff, last2 = 0xffff;
        u32 last_sticks = ~0u;
        for (;;) {
            const u64 f = rsx::flip_count();
            PadState st;
            for (auto& p : presses) if (f >= p.from && f < p.to) {
                st.digital1 |= p.d1; st.digital2 |= p.d2;
                if (p.stick >= 0) (p.stick == 0 ? st.lx : p.stick == 1 ? st.ly : p.stick == 2 ? st.rx : st.ry) = p.value;
            }
            const u32 sticks = u32(st.lx) | u32(st.ly) << 8 | u32(st.rx) << 16 | u32(st.ry) << 24;
            if (st.digital1 != last1 || st.digital2 != last2 || sticks != last_sticks) {
                last_sticks = sticks;
                pad_set_state(0, st);
                if (st.digital1 || st.digital2) LOG_NOTE("pad script: frame %llu buttons %04x %04x", (unsigned long long)f, st.digital1, st.digital2);
                last1 = st.digital1; last2 = st.digital2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }).detach();
}
