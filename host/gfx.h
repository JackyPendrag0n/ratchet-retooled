// Graphics options: settings.ini and the Graphics tab of the F1 menu. The menu changes them while
// the game runs; the native renderer applies a change at the next frame and the menu saves it to
// settings.ini.
#pragma once
#include <atomic>
#include <string>

namespace gfx {

enum Aa : int { AA_OFF = 0, AA_FXAA = 1 };

struct Options {
    std::atomic<int> resolution_scale{1};  // surfaces drawn at 1-4x the PS3's size; shown downsampled (supersampling)
    std::atomic<int> aa{AA_FXAA};          // post-process anti-aliasing of the shown picture
    std::atomic<int> anisotropy{8};        // at least this much anisotropic filtering on mipmapped textures (0 = as the game asks)
    std::atomic<bool> soft_shadows{true};  // 9-tap shadow map filtering instead of the RSX's 2x2
    std::atomic<bool> screenshot{false};   // F12: the renderer saves the next shown frame as a PNG
    std::atomic<int> rumble{100};          // controller rumble strength in percent (0 = off)
    std::atomic<bool> texture_dump{false};  // save every texture the game uploads to <data>/textures/dump
    std::atomic<bool> coop{false};         // two-player co-op (host/coop.cpp): a second Ratchet on the second controller
    std::atomic<bool> split_views{false};  // co-op split screen is up (the native renderer keeps each view's scene copy)
    std::atomic<int> passlog{0};           // debugging (!passlog): frames left for the native renderer to log its targets
    std::atomic<unsigned> shot_surf{0};    // debugging (!shotsurf <hex>): screenshots save this colour surface instead
    std::atomic<unsigned> skip_vps{0};     // debugging (!skipvp <hex>): split screen, drop the right view's draws with this vertex program (low 32 bits of its hash)
    std::atomic<unsigned> dump_vps{0};     // debugging (!dumpvp <hex>): log the vertex constants of the next such draw in each view
    std::atomic<unsigned> dump_idx{0};    // debugging (!dumpidx <hex>, with TOD_DRAW_TRACE): log the vertex constants of the first draw from this index address in each view
    std::atomic<unsigned> skip_from{0}, skip_to{0};   // debugging (!skipdraws a b): drop the right view's draws a..b-1 of each frame
};
extern Options g;

// Rewrites `key = value` in settings.ini (main.cpp). Any thread.
void save_setting(const std::string& key, const std::string& value);
// A new file name for a screenshot: ~/Pictures/TOD Recomp/ (or <data>/screenshots/), made if needed.
std::string screenshot_path();

}  // namespace gfx
