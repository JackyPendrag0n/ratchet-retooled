// Vulkan renderer for the RSX command processor. Draws with the game's own state (surfaces,
// programs, textures) and presents the display buffer on flip, either to a window surface or
// headless (frames optionally dumped as PNG via TOD_DUMP_FRAMES=dir[,every_n]).
#pragma once
#include "rsx_state.h"

#include <vulkan/vulkan.h>

#include <functional>
#include <vector>

namespace rsx {

struct VkPresentTarget {
    std::vector<const char*> instance_extensions;
    std::function<VkSurfaceKHR(VkInstance)> create_surface;
    std::function<void(u32& w, u32& h)> drawable_size;
    bool vsync = true;
};

// Returns nullptr (after logging why) if no usable Vulkan 1.3 device exists.
Renderer* create_vulkan_renderer(const VkPresentTarget* target);   // legacy (vk_backend.cpp)
Renderer* create_native_renderer(const VkPresentTarget* target);   // native (vkn_*.cpp)
// The one settings.ini asks for (g_config.native_renderer; TOD_RENDERER=legacy|native overrides).
Renderer* create_renderer(const VkPresentTarget* target);

}  // namespace rsx
