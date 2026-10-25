// On-screen overlay (Dear ImGui) drawn over the presented frame: the cheat menu and short notices.
// Input arrives on the window thread; the UI is built and drawn on the render thread at present.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

union SDL_Event;
struct _SDL_GameController;

namespace overlay {

// Window thread. True when the event belongs to the overlay (the game must not see it).
bool handle_event(const SDL_Event& e, float to_pixels_x, float to_pixels_y);
// Window thread, once per pump: controller state for menu navigation and the open/close combo.
void poll_controller(_SDL_GameController* pad);
// While the menu is open the game gets no keyboard or controller input.
bool menu_open();

struct VkInfo {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t queue_family;
    VkQueue queue;
    uint32_t image_count;
};
// Render thread, between the game picture's blit and the present barrier: `img` is in
// TRANSFER_DST_OPTIMAL and stays in it. Does nothing when there is nothing to show.
void draw(const VkInfo& vk, VkCommandBuffer cmd, VkImage img, VkFormat fmt, VkExtent2D ext);

// Render thread, while the menu is built: a PNG as an ImGui texture (cached; scaled down to at most 256
// pixels), or null when it can't be read. w/h get its size.
void* thumbnail(const std::string& png, int& w, int& h);

// Any thread: a line shown for a few seconds at the top of the screen.
void notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace overlay
