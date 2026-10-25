// Gameplay that comes with a skin pack (skin.bin flags, tod_skin.py --ability), for whichever player wears it.
//
// Glide (skins::kAbilityGlide): after a double jump, holding Cross keeps the hero in a long glide: the fall
// is held to a slow sink and the hero is carried forward at gliding speed, steered by the game's own air
// control. Letting go of Cross, landing, or anything that takes the hero out of the air ends it.
// It runs in HOOKS 0x23E608, as the hero copies its position into its moby: the frame's movement is done by
// then (the game's own glide, holding Cross while falling, caps the fall at 2 m/s and the speed at about
// 5 m/s), so the glide makes up the difference to its own motion in the hero's position and leaves its
// velocity (hero +0x1210) at the glide's. A Cross press on the ground is the first jump, the next one in the
// air the double jump; the hero's ground / air timer (+0x1334) says which.
#include "abilities.h"
#include "coop.h"
#include "input.h"
#include "rsx.h"
#include "skins.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace abilities {
namespace {

constexpr u32 kHeroVelocity = 0x1210;          // m/s, world y-up
constexpr u32 kHeroAirTimer = 0x1334;          // s: counts up on the ground, down from 0 in the air
constexpr u32 kHeroPosNow[] = {0x1260, 0x1280, 0x1AA0};   // this frame's position (+0x1250 / +0x1270: the last frame's)
constexpr u16 kCross = 0x40;                   // PadState::digital2

float getf(u32 a) { float v; u32 b = ld32(mem::base, a); std::memcpy(&v, &b, 4); return v; }
void setf(u32 a, float v) { u32 b; std::memcpy(&b, &v, 4); st32(mem::base, a, b); }

std::mutex g_lock;
// debugging: "!glide trace n" logs the hero's movement for n updates; "!glide snap" / "!glide diff" compare
// the hero object between two moments (finding state fields)
int g_trace = 0, g_trace_pl = 0;
std::vector<u8> g_snap;
u32 g_snap_hero = 0;
int g_snap_op = 0;   // 1 snapshot, 2 diff
u32 g_last_hero[2] = {};
// TOD_HERO_REC="<flip>,<updates>,<file>": player 1's hero object (0x2000 bytes) at every update from that flip on,
// each record headed by the flip count and the pad's digital2 (u32 each), for finding fields offline
FILE* g_rec = nullptr;
u64 g_rec_from = 0;
int g_rec_left = 0;
void rec_init() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* e = std::getenv("TOD_HERO_REC");
    if (!e) return;
    char path[512] = {};
    unsigned long long from = 0; int n = 0;
    if (std::sscanf(e, "%llu,%d,%511s", &from, &n, path) == 3 && (g_rec = std::fopen(path, "wb"))) { g_rec_from = from; g_rec_left = n; }
}
void rec(u32 hero) {
    rec_init();
    if (!g_rec || g_rec_left <= 0 || rsx::flip_count() < g_rec_from) return;
    const u32 head[2] = {u32(rsx::flip_count()), pad_get_state(0).digital2};
    std::fwrite(head, 4, 2, g_rec);
    std::fwrite(mem::base + hero, 1, 0x2000, g_rec);
    if (--g_rec_left == 0) { std::fclose(g_rec); g_rec = nullptr; LOG_NOTE("glide: hero recording done"); }
}

// per player: jumps seen since the hero left the ground, and the glide
struct Air {
    u32 hero = 0;
    bool cross = false;           // Cross held at the last update
    float timer = 0;              // the hero's ground / air timer at the last update
    float dt = 1 / 30.0f;
    int jumps = 0;                // Cross presses since leaving the ground (2 = the double jump)
    bool gliding = false;
    float speed = 0;              // the glide's forward speed
    float dir[2] = {0, 1};        // its direction (x, z)
};
Air g_air[2];

// glide tuning: the fall it is held to, the forward speed it builds up to, and how fast
constexpr float kSink = 0.9f, kGlideSpeed = 12.0f, kGlideAccel = 6.0f;

void glide(Air& a, u32 hero, bool cross) {
    float vx = getf(hero + kHeroVelocity), vy = getf(hero + kHeroVelocity + 4), vz = getf(hero + kHeroVelocity + 8);
    // the timer counts up on the ground and down (below 0) in the air, by the frame's time step
    const float t = getf(hero + kHeroAirTimer);
    const float step = std::fabs(t - a.timer);
    if (step > 1e-4f && step < 0.2f && (t < 0) == (a.timer < 0)) a.dt = step;
    a.timer = t;
    const bool press = cross && !a.cross;
    a.cross = cross;
    if (t >= 0) {   // on the ground
        if (a.gliding) LOG_NOTE("glide: landed");
        a.gliding = false;
        // a press on the ground is the first jump (Cross can still be held as the hero leaves the ground)
        if (press) a.jumps = 1;
        else if (!cross) a.jumps = 0;
        return;
    }
    if (press) ++a.jumps;
    if (!cross || a.jumps < 2) {
        if (a.gliding) LOG_NOTE("glide: let go");
        a.gliding = false;
        return;
    }
    const float h = std::sqrt(vx * vx + vz * vz);
    if (!a.gliding) {
        // the double jump's own press: its lift only shows in the next frame's velocity
        if (press || vy > 0) return;   // still rising from the double jump
        a.gliding = true;
        a.speed = h;
        LOG_NOTE("glide: start (player %d, falling %.1f m/s, moving %.1f m/s)", hero == coop::hero2() ? 2 : 1, -vy, h);
    }
    // the game has already moved the hero this frame: make up the difference to the glide's motion
    float dx = 0, dy = 0, dz = 0;
    if (vy < -kSink) { dy = (-kSink - vy) * a.dt; vy = -kSink; }
    if (h > 1.0f) {
        a.dir[0] = vx / h; a.dir[1] = vz / h;
        a.speed = std::min(kGlideSpeed, std::max(a.speed, h) + kGlideAccel * a.dt);
        const float extra = (a.speed - h) * a.dt;
        dx = a.dir[0] * extra; dz = a.dir[1] * extra;
        vx = a.dir[0] * a.speed; vz = a.dir[1] * a.speed;
    } else {
        a.speed = h;   // stopped (a wall, or no stick): no push
    }
    for (u32 f : kHeroPosNow) {
        setf(hero + f, getf(hero + f) + dx); setf(hero + f + 4, getf(hero + f + 4) + dy); setf(hero + f + 8, getf(hero + f + 8) + dz);
    }
    setf(hero + kHeroVelocity, vx); setf(hero + kHeroVelocity + 4, vy); setf(hero + kHeroVelocity + 8, vz);
}

}  // namespace

void on_hero_update(u32 hero) {
    if (!hero || !mem::is_mapped(hero, 0x2000)) return;
    const int pl = hero == coop::hero2() && hero ? 1 : 0;
    std::lock_guard lk(g_lock);
    g_last_hero[pl] = hero;
    if (pl == 0) rec(hero);
    if (g_snap_op && pl == 0) {
        if (g_snap_op == 1) {
            g_snap.assign(mem::base + hero, mem::base + hero + 0x2000);
            g_snap_hero = hero;
            LOG_NOTE("glide: hero 0x%08x snapshot", hero);
        } else if (g_snap_hero == hero && !g_snap.empty()) {
            for (u32 o = 0; o < 0x2000; o += 4) {
                u32 a, b;
                std::memcpy(&a, &g_snap[o], 4); std::memcpy(&b, mem::base + hero + o, 4);
                if (a != b) {
                    const u32 x = __builtin_bswap32(a), y = __builtin_bswap32(b);
                    float fx, fy; std::memcpy(&fx, &x, 4); std::memcpy(&fy, &y, 4);
                    LOG_NOTE("glide: diff +0x%04x %08x -> %08x (%g -> %g)", o, x, y, fx, fy);
                }
            }
        }
        g_snap_op = 0;
    }
    if (g_trace > 0 && pl == g_trace_pl) {
        --g_trace;
        const PadState ps = pad_get_state(u32(pl));
        LOG_NOTE("glide: trace player %d vel %.2f %.2f %.2f pos y %.2f timer %.2f cross %d jumps %d abilities %x", pl + 1,
                 getf(hero + kHeroVelocity), getf(hero + kHeroVelocity + 4), getf(hero + kHeroVelocity + 8),
                 getf(hero + 0x1AA0 + 4), getf(hero + kHeroAirTimer), (ps.digital2 & kCross) ? 1 : 0, g_air[pl].jumps, skins::abilities(pl));
    }
    Air& a = g_air[pl];
    if (a.hero != hero) { a = Air{}; a.hero = hero; }
    static const bool off = std::getenv("TOD_GLIDE_OFF") != nullptr;   // testing: abilities stay off
    const u32 ab = off ? 0 : skins::abilities(pl);
    const u16 buttons = pad_get_state(u32(pl)).digital2;
    if (!(ab & skins::kAbilityGlide)) { a.gliding = false; return; }
    glide(a, hero, (buttons & kCross) != 0);
}

void command(const std::string& args) {
    std::lock_guard lk(g_lock);
    if (args.rfind(" trace", 0) == 0) {
        // "trace <updates> [player 1|2]"
        char* end = nullptr;
        g_trace = int(std::strtol(args.c_str() + 6, &end, 10));
        if (g_trace <= 0) g_trace = 120;
        g_trace_pl = std::strtol(end, nullptr, 10) == 2 ? 1 : 0;
        return;
    }
    if (args.rfind(" snap", 0) == 0) { g_snap_op = 1; return; }
    if (args.rfind(" diff", 0) == 0) { g_snap_op = 2; return; }
    if (args.rfind(" dump", 0) == 0) {
        const u32 h = g_last_hero[0];
        char* end = nullptr;
        const u32 off = u32(std::strtoul(args.c_str() + 5, &end, 16)), n = u32(std::strtoul(end, nullptr, 16));
        for (u32 o = off; h && o < off + (n ? n : 0x40); o += 16)
            LOG_NOTE("glide: hero+%04x %08x %08x %08x %08x", o, ld32(mem::base, h + o), ld32(mem::base, h + o + 4), ld32(mem::base, h + o + 8), ld32(mem::base, h + o + 12));
    }
}

}  // namespace abilities
