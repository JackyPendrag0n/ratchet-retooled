// Free camera and no clip. Both are driven from the pad: while one is on, the sticks, L1, R1 and R2
// steer it instead of reaching the game (see fly_input, called by the window's pad code).
//   free camera: HOOKS 0x48C910 hands over the game camera's world matrix (4 rows: x, y and z axes,
//                position) just before the frame's view is built from it, so the view, culling
//                and the frustum all follow the overridden matrix.
//   no clip:     HOOKS 0x23E608 runs as the hero copies its position into its moby; the hero keeps
//                its position in several fields (+0x1250..+0x1280 and +0x1AA0), and all of them
//                have to change for a move to stick; its velocity (+0x1330) is zeroed.
#include "cheats.h"
#include "overlay_notify.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace cheats {
namespace {

struct V3 {
    float x = 0, y = 0, z = 0;
    V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) { const float l = std::sqrt(dot(a, a)); return l > 1e-6f ? a * (1 / l) : a; }
// rotate v about the unit axis k by angle a (Rodrigues)
V3 rotate(V3 v, V3 k, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}

float getf(u8* base, u32 a) { float v; u32 b = ld32(base, a); std::memcpy(&v, &b, 4); return v; }
void setf(u8* base, u32 a, float v) { u32 b; std::memcpy(&b, &v, 4); st32(base, a, b); }
V3 get3(u8* base, u32 a) { return {getf(base, a), getf(base, a + 4), getf(base, a + 8)}; }
void set3(u8* base, u32 a, V3 v) { setf(base, a, v.x); setf(base, a + 4, v.y); setf(base, a + 8, v.z); }

constexpr V3 kUp{0, 1, 0};                     // the game's world is y-up
constexpr float kMoveSpeed = 8, kFastFactor = 4, kTurnSpeed = 2.2f;  // m/s, rad/s
constexpr u32 kHeroPosFields[] = {0x1250, 0x1260, 0x1270, 0x1280, 0x1AA0};
constexpr u32 kHeroVelocity = 0x1330;

struct FlyInput { float lx = 0, ly = 0, rx = 0, ry = 0; bool rise = false, sink = false, fast = false; };

std::mutex g_lock;
FlyInput g_in;
PadState g_test;
bool g_test_held = false;
std::atomic<bool> g_flying{false};

// the camera as the game last set it (rows), and where it looks (+1: along row 2, -1: against it).
// Row 0 is the camera's x axis: screen right when it looks against row 2, screen left otherwise.
V3 g_cam_right, g_cam_up, g_cam_back, g_cam_pos;
float g_look_sign = -1;
bool g_have_cam = false;

// free camera state
bool g_free_active = false;
V3 g_fr, g_fu, g_ff, g_fp;   // screen right, up, forward, position

// no clip state
bool g_clip_active = false;
V3 g_hero_pos;

// practice tools: a remembered position (F5) to go back to (F9), and the hero's speed
std::atomic<int> g_pos_op{0};   // 1 save, 2 restore
std::atomic<bool> g_have_saved{false};
V3 g_saved_pos;
int g_restore_frames = 0;
struct Motion { V3 pos; float speed = 0; bool valid = false; };
Motion g_motion;
V3 g_last_pos;
std::chrono::steady_clock::time_point g_last_pos_time;

FlyInput input() { std::lock_guard lk(g_lock); return g_in; }

float step_time(std::chrono::steady_clock::time_point& last) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    return std::clamp(dt, 0.f, 0.1f);
}

// Move along the camera's view: left stick forward/back and sideways, R1/L1 up and down.
V3 move(const FlyInput& in, V3 right, V3 fwd, float dt) {
    V3 d = right * in.lx + fwd * -in.ly + kUp * (float(in.rise) - float(in.sink));
    return d * (kMoveSpeed * (in.fast ? kFastFactor : 1) * dt);
}

void free_camera(u8* base, u32 cam) {
    static auto last = std::chrono::steady_clock::now();
    const float dt = step_time(last);
    if (!g_free_active) {
        if (!g_have_cam) return;
        g_fr = g_cam_right * -g_look_sign; g_fu = g_cam_up; g_ff = g_cam_back * g_look_sign; g_fp = g_cam_pos;
        g_free_active = true;
    }
    const FlyInput in = input();
    // turn: right stick x yaws about world up, y pitches about the camera's right axis. The signs
    // are picked so that pushing right turns towards the right and pushing up looks up.
    if (in.rx != 0) {
        float a = in.rx * kTurnSpeed * dt;
        if (dot(rotate(g_ff, kUp, 0.01f) - g_ff, g_fr) < 0) a = -a;
        g_fr = rotate(g_fr, kUp, a); g_fu = rotate(g_fu, kUp, a); g_ff = rotate(g_ff, kUp, a);
    }
    if (in.ry != 0) {
        float a = -in.ry * kTurnSpeed * dt;
        if (dot(rotate(g_ff, g_fr, 0.01f) - g_ff, kUp) < 0) a = -a;
        const V3 f = rotate(g_ff, g_fr, a);
        if (std::fabs(dot(f, kUp)) < 0.97f) { g_ff = f; g_fu = rotate(g_fu, g_fr, a); }
    }
    // keep the basis orthonormal and the horizon level
    g_fr = norm(g_fr);
    g_ff = norm(g_ff);
    g_fu = norm(cross(g_fr, g_ff) * (dot(cross(g_fr, g_ff), g_fu) < 0 ? -1.f : 1.f));
    g_fp = g_fp + move(in, g_fr, g_ff, dt);
    set3(base, cam + 0x00, g_fr * -g_look_sign);
    set3(base, cam + 0x10, g_fu);
    set3(base, cam + 0x20, g_ff * g_look_sign);
    set3(base, cam + 0x30, g_fp);
}

void no_clip(u8* base, u32 hero) {
    static auto last = std::chrono::steady_clock::now();
    const float dt = step_time(last);
    if (!g_clip_active) {
        g_hero_pos = get3(base, hero + 0x1AA0);
        g_clip_active = true;
    }
    // while the free camera is on, the pad flies the camera and the hero stays put
    if (!toggles().freecam && g_have_cam) g_hero_pos = g_hero_pos + move(input(), g_cam_right * -g_look_sign, g_cam_back * g_look_sign, dt);
    for (u32 f : kHeroPosFields) set3(base, hero + f, g_hero_pos);
    set3(base, hero + kHeroVelocity, V3{});   // or the fall speed builds up and the hero drops
}

}  // namespace

void filter_pad(PadState& st) {
    {
        // a held test input ("!pad" in the command file) stands in for the controller
        std::lock_guard lk(g_lock);
        if (g_test_held) st = g_test;
    }
    auto axis = [](u8 v) {
        const float f = std::clamp((int(v) - 128) / 127.f, -1.f, 1.f);
        return std::fabs(f) < 0.15f ? 0.f : f;
    };
    {
        std::lock_guard lk(g_lock);
        g_in = {axis(st.lx), axis(st.ly), axis(st.rx), axis(st.ry), (st.digital2 & 0x08) != 0, (st.digital2 & 0x04) != 0,
                (st.digital2 & 0x02) != 0};
    }
    if (!g_flying) return;
    st.lx = st.ly = st.rx = st.ry = 0x80;
    st.digital2 &= ~0x0E;   // R1, L1, R2
}

void hold_test_pad(const PadState& st) {
    std::lock_guard lk(g_lock);
    g_test = st;
    g_test_held = st.lx != 0x80 || st.ly != 0x80 || st.rx != 0x80 || st.ry != 0x80 || st.digital1 || st.digital2;
}

void on_camera(u8* base, u32 cam) {
    const Toggles t = toggles();
    g_flying = t.freecam || t.noclip;
    if (t.freecam) { free_camera(base, cam); return; }
    g_free_active = false;
    g_cam_right = get3(base, cam);
    g_cam_up = get3(base, cam + 0x10);
    g_cam_back = get3(base, cam + 0x20);
    g_cam_pos = get3(base, cam + 0x30);
    // the hero is in front of a follow camera: that tells which way row 2 points
    if (const u32 moby = hero_moby(); moby && mem::is_mapped(moby, 0x40)) {
        const float s = dot(get3(base, moby + 0x30) - g_cam_pos, g_cam_back);
        if (std::fabs(s) > 0.5f) g_look_sign = s > 0 ? 1 : -1;
    }
    g_have_cam = true;
}

void on_hero_update(u8* base, u32 hero) {
    const u32 moby = hero_moby();
    const bool is_hero = moby && ld32(base, moby + 0x40) == hero;
    if (is_hero) {
        const V3 p = get3(base, hero + 0x1AA0);
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lk(g_lock);
            const float dt = std::chrono::duration<float>(now - g_last_pos_time).count();
            if (g_motion.valid && dt > 0.05f) {
                const V3 d = p - g_last_pos;
                // horizontal speed, smoothed; a jump of more than 50 m is a warp, not movement
                const float v = std::sqrt(d.x * d.x + d.z * d.z) / dt;
                if (v < 1000) g_motion.speed = g_motion.speed * 0.6f + v * 0.4f;
                g_last_pos = p;
                g_last_pos_time = now;
            } else if (!g_motion.valid) {
                g_last_pos = p;
                g_last_pos_time = now;
            }
            g_motion.pos = p;
            g_motion.valid = true;
        }
        const int op = g_pos_op.exchange(0);
        if (op == 1) {
            g_saved_pos = p;
            g_have_saved = true;
        } else if (op == 2 && g_have_saved) {
            g_restore_frames = 3;   // held for a few updates, or the hero's own move can undo it
            if (g_clip_active) g_hero_pos = g_saved_pos;
        }
        if (g_restore_frames > 0) {
            --g_restore_frames;
            for (u32 f : kHeroPosFields) set3(base, hero + f, g_saved_pos);
            set3(base, hero + kHeroVelocity, V3{});
        }
    }
    if (!is_hero || !toggles().noclip) { g_clip_active = false; return; }
    no_clip(base, hero);
}

// a position belongs to the level it was saved in
std::atomic<int> g_saved_level{-1};
void save_position() {
    g_saved_level = snapshot().level;
    g_pos_op = 1;
    overlay_notify("Position saved (F9 to go back)");
}
void restore_position() {
    if (!g_have_saved) { overlay_notify("No saved position yet (F5 saves one)"); return; }
    if (snapshot().level != g_saved_level) { overlay_notify("The saved position is on another planet"); return; }
    g_pos_op = 2;
    overlay_notify("Back to the saved position");
}
bool have_saved_position() { return g_have_saved; }

bool hero_motion(float pos[3], float& speed) {
    std::lock_guard lk(g_lock);
    if (!g_motion.valid || !hero_moby()) return false;
    pos[0] = g_motion.pos.x; pos[1] = g_motion.pos.y; pos[2] = g_motion.pos.z;
    speed = g_motion.speed;
    return true;
}

}  // namespace cheats
