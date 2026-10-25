// Dear ImGui overlay on the presented frame. SDL events are queued on the window thread and fed to
// ImGui on the render thread, which builds the UI and records it into the frame's command buffer.
#include "overlay.h"
#include "guest.h"
#include "overlay_notify.h"
#include "tex_pack.h"

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
#include <cstdarg>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace cheat_menu { void build(); void build_hud(); bool hud_wanted(); void show_skins(); }

namespace overlay {
namespace {

struct InputEvent {
    enum Type { MousePos, MouseButton, Wheel, Key, Text, Focus } type;
    float x = 0, y = 0;
    int code = 0;
    bool down = false;
    std::string text;
};

std::mutex g_in_lock;
std::deque<InputEvent> g_input;
std::atomic<bool> g_open{false};
std::atomic<bool> g_mouse_moved{false};

struct Notice { std::string text; std::chrono::steady_clock::time_point until; };
std::mutex g_notice_lock;
std::deque<Notice> g_notices;

void push(InputEvent e) {
    std::lock_guard lk(g_in_lock);
    if (g_input.size() < 512) g_input.push_back(std::move(e));
}

ImGuiKey to_imgui_key(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return ImGuiKey(ImGuiKey_A + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9) return ImGuiKey(ImGuiKey_0 + (k - SDLK_0));
    switch (k) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_MINUS: return ImGuiKey_Minus;
    case SDLK_PERIOD: return ImGuiKey_Period;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

void set_open(bool open) {
    if (g_open.exchange(open) != open) push({InputEvent::Focus, 0, 0, 0, open});
}

// ---- render-thread state ----
bool g_ready = false;
VkFormat g_format = VK_FORMAT_UNDEFINED;
VkDevice g_dev = VK_NULL_HANDLE;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
struct ViewKey { VkImage img; VkFormat fmt; u32 w, h; };
std::map<VkImage, std::pair<ViewKey, VkImageView>> g_views;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();
u32 g_font_px = 0;
VkInfo g_vk{};

// skin picker thumbnails: one sampled image per PNG, uploaded once
struct Thumb { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDescriptorSet ds = VK_NULL_HANDLE; int w = 0, h = 0; };
std::map<std::string, Thumb> g_thumbs;
VkSampler g_sampler = VK_NULL_HANDLE;
VkCommandPool g_upload_pool = VK_NULL_HANDLE;

void check(VkResult r) { if (r != VK_SUCCESS) LOG_ERR("overlay: Vulkan error %d", int(r)); }

bool init_backend(const VkInfo& vk, VkFormat fmt) {
    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 6;
        st.FrameRounding = 4;
        st.GrabRounding = 4;
        st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    }
    if (g_ready) {
        vkDeviceWaitIdle(g_dev);
        // thumbnails' descriptor sets belong to the backend being shut down
        for (auto& [k, t] : g_thumbs) {
            if (t.ds) ImGui_ImplVulkan_RemoveTexture(t.ds);
            if (t.view) vkDestroyImageView(g_dev, t.view, nullptr);
            if (t.img) vkDestroyImage(g_dev, t.img, nullptr);
            if (t.mem) vkFreeMemory(g_dev, t.mem, nullptr);
        }
        g_thumbs.clear();
        ImGui_ImplVulkan_Shutdown();
        g_ready = false;
    }
    g_dev = vk.dev;
    if (!g_pool) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 80};   // the font + skin thumbnails
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 80;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(vk.dev, &pi, nullptr, &g_pool) != VK_SUCCESS) return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = vk.instance;
    ii.PhysicalDevice = vk.phys;
    ii.Device = vk.dev;
    ii.QueueFamily = vk.queue_family;
    ii.Queue = vk.queue;
    ii.DescriptorPool = g_pool;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, vk.image_count);
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format;
    ii.CheckVkResultFn = check;
    g_format = fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return false;
    g_ready = true;
    return true;
}

// Default font rasterised at a size that suits the output height (crisper than scaling).
void ensure_font(u32 height) {
    const u32 px = std::max(13u, std::min(40u, height * 15 / 720));
    if (px == g_font_px) return;
    ImGuiIO& io = ImGui::GetIO();
    if (g_font_px) ImGui_ImplVulkan_DestroyFontsTexture();
    io.Fonts->Clear();
    ImFontConfig fc;
    fc.SizePixels = float(px);
    fc.OversampleH = fc.OversampleV = 1;
    fc.PixelSnapH = true;
    io.Fonts->AddFontDefault(&fc);
    ImGui_ImplVulkan_CreateFontsTexture();
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    st.ScaleAllSizes(float(px) / 13.f);
    g_font_px = px;
}

VkImageView view_for(VkImage img, VkFormat fmt, VkExtent2D ext) {
    auto it = g_views.find(img);
    if (it != g_views.end()) {
        const ViewKey& k = it->second.first;
        if (k.fmt == fmt && k.w == ext.width && k.h == ext.height) return it->second.second;
        // the swapchain was rebuilt (after a device wait), so the old view is idle
        vkDestroyImageView(g_dev, it->second.second, nullptr);
        g_views.erase(it);
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_dev, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (g_views.size() > 16) g_views.clear();  // stale entries from old swapchains
    g_views[img] = {ViewKey{img, fmt, ext.width, ext.height}, v};
    return v;
}

void feed_input() {
    ImGuiIO& io = ImGui::GetIO();
    std::deque<InputEvent> q;
    {
        std::lock_guard lk(g_in_lock);
        q.swap(g_input);
    }
    for (auto& e : q) {
        switch (e.type) {
        case InputEvent::MousePos: io.AddMousePosEvent(e.x, e.y); break;
        case InputEvent::MouseButton: io.AddMouseButtonEvent(e.code, e.down); break;
        case InputEvent::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
        case InputEvent::Key: io.AddKeyEvent(ImGuiKey(e.code), e.down); break;
        case InputEvent::Text: io.AddInputCharactersUTF8(e.text.c_str()); break;
        case InputEvent::Focus:
            io.AddFocusEvent(e.down);
            if (!e.down) io.ClearInputKeys();
            break;
        }
    }
}

void draw_notices() {
    std::lock_guard lk(g_notice_lock);
    const auto now = std::chrono::steady_clock::now();
    while (!g_notices.empty() && g_notices.front().until < now) g_notices.pop_front();
    if (g_notices.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.04f), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##notices", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    for (auto& n : g_notices) ImGui::TextUnformatted(n.text.c_str());
    ImGui::End();
}

bool has_notices() {
    std::lock_guard lk(g_notice_lock);
    return !g_notices.empty();
}


u32 memory_type(u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

// a PNG into a sampled image (staging buffer, one-off command buffer, waited for: thumbnails are few and small)
bool upload_thumb(const std::string& path, Thumb& t) {
    std::vector<u8> px;
    u32 w = 0, h = 0;
    if (!texpack::read_png(path, px, w, h) || !w || !h) return false;
    // box-filter down to at most 256 pixels on the long side
    while (std::max(w, h) > 256 && w > 1 && h > 1) {
        const u32 nw = w / 2, nh = h / 2;
        std::vector<u8> o(size_t(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&](u32 xx, u32 yy) { return u32(px[(size_t(yy) * w + xx) * 4 + c]); };
                    o[(size_t(y) * nw + x) * 4 + c] = u8((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
        px.swap(o); w = nw; h = nh;
    }
    const VkDevice d = g_dev;
    if (!g_sampler) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 1;
        if (vkCreateSampler(d, &si, nullptr, &g_sampler) != VK_SUCCESS) return false;
    }
    if (!g_upload_pool) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = g_vk.queue_family;
        if (vkCreateCommandPool(d, &ci, nullptr, &g_upload_pool) != VK_SUCCESS) return false;
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(d, &ii, nullptr, &t.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d, t.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(d, &ai, nullptr, &t.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(d, t.img, t.mem, 0);
    // staging
    VkBuffer sb = VK_NULL_HANDLE;
    VkDeviceMemory sm = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = px.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(d, &bi, nullptr, &sb) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(d, sb, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    bool ok = ai.memoryTypeIndex != ~0u && vkAllocateMemory(d, &ai, nullptr, &sm) == VK_SUCCESS;
    if (ok) {
        vkBindBufferMemory(d, sb, sm, 0);
        void* map = nullptr;
        vkMapMemory(d, sm, 0, px.size(), 0, &map);
        std::memcpy(map, px.data(), px.size());
        vkUnmapMemory(d, sm);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = g_upload_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(d, &cai, &cb);
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &cbi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, sb, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cb);
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cb;
        ok = vkQueueSubmit(g_vk.queue, 1, &sub, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(g_vk.queue) == VK_SUCCESS;
        vkFreeCommandBuffers(d, g_upload_pool, 1, &cb);
    }
    vkDestroyBuffer(d, sb, nullptr);
    if (sm) vkFreeMemory(d, sm, nullptr);
    if (!ok) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d, &vi, nullptr, &t.view) != VK_SUCCESS) return false;
    t.ds = ImGui_ImplVulkan_AddTexture(g_sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t.w = int(w); t.h = int(h);
    return t.ds != VK_NULL_HANDLE;
}

}  // namespace

void* thumbnail(const std::string& png, int& w, int& h) {
    if (!g_ready || png.empty()) return nullptr;
    auto it = g_thumbs.find(png);
    if (it == g_thumbs.end()) {
        Thumb t;
        if (!upload_thumb(png, t)) { LOG_ERR("overlay: can't show %s", png.c_str()); t.ds = VK_NULL_HANDLE; }
        it = g_thumbs.emplace(png, t).first;   // a failure is remembered too, so it isn't retried every frame
    }
    w = it->second.w; h = it->second.h;
    return it->second.ds ? reinterpret_cast<void*>(it->second.ds) : nullptr;
}
namespace {
}  // namespace

bool menu_open() { return g_open; }

bool handle_event(const SDL_Event& e, float sx, float sy) {
    switch (e.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const SDL_Keycode k = e.key.keysym.sym;
        if (e.type == SDL_KEYDOWN && !e.key.repeat && (k == SDLK_F1 || (k == SDLK_ESCAPE && g_open && !(SDL_GetModState() & KMOD_SHIFT)))) {
            if (k == SDLK_F1 || g_open) { set_open(k == SDLK_F1 ? !g_open : false); return true; }
        }
        // F3: straight to the skins
        if (e.type == SDL_KEYDOWN && !e.key.repeat && k == SDLK_F3) { if (!g_open) cheat_menu::show_skins(); set_open(!g_open); return true; }
        if (!g_open) return false;
        if (ImGuiKey ik = to_imgui_key(k); ik != ImGuiKey_None) push({InputEvent::Key, 0, 0, int(ik), e.type == SDL_KEYDOWN});
        return true;
    }
    case SDL_TEXTINPUT:
        if (!g_open) return false;
        push({InputEvent::Text, 0, 0, 0, false, e.text.text});
        return true;
    case SDL_MOUSEMOTION:
        push({InputEvent::MousePos, e.motion.x * sx, e.motion.y * sy});
        return g_open;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (!g_open) return false;
        const int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        push({InputEvent::MousePos, e.button.x * sx, e.button.y * sy});
        if (b >= 0) push({InputEvent::MouseButton, 0, 0, b, e.type == SDL_MOUSEBUTTONDOWN});
        return true;
    }
    case SDL_MOUSEWHEEL:
        if (!g_open) return false;
        push({InputEvent::Wheel, float(e.wheel.x), float(e.wheel.y)});
        return true;
    default:
        return false;
    }
}

void poll_controller(_SDL_GameController* p) {
    SDL_GameController* pad = reinterpret_cast<SDL_GameController*>(p);
    static bool combo_was = false;
    static u32 held = 0;
    if (!pad) return;
    auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
    // L3 + R3 together opens or closes the menu
    const bool combo = btn(SDL_CONTROLLER_BUTTON_LEFTSTICK) && btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (combo && !combo_was) set_open(!g_open);
    combo_was = combo;
    struct Map { SDL_GameControllerButton b; ImGuiKey k; };
    static const Map kMap[] = {
        {SDL_CONTROLLER_BUTTON_A, ImGuiKey_GamepadFaceDown},   {SDL_CONTROLLER_BUTTON_B, ImGuiKey_GamepadFaceRight},
        {SDL_CONTROLLER_BUTTON_X, ImGuiKey_GamepadFaceLeft},   {SDL_CONTROLLER_BUTTON_Y, ImGuiKey_GamepadFaceUp},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ImGuiKey_GamepadL1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ImGuiKey_GamepadR1},
        {SDL_CONTROLLER_BUTTON_START, ImGuiKey_GamepadStart},
    };
    u32 now = 0;
    if (g_open)
        for (size_t i = 0; i < std::size(kMap); ++i) if (btn(kMap[i].b)) now |= 1u << i;
    for (size_t i = 0; i < std::size(kMap); ++i)
        if (((now ^ held) >> i) & 1) push({InputEvent::Key, 0, 0, int(kMap[i].k), bool((now >> i) & 1)});
    held = now;
    // left stick as D-pad style navigation (analog nav keys)
    static int stick_was = 0;
    const int ly = g_open ? SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) : 0;
    const int st = ly < -16000 ? -1 : ly > 16000 ? 1 : 0;
    if (st != stick_was) {
        if (stick_was) push({InputEvent::Key, 0, 0, int(stick_was < 0 ? ImGuiKey_GamepadLStickUp : ImGuiKey_GamepadLStickDown), false});
        if (st) push({InputEvent::Key, 0, 0, int(st < 0 ? ImGuiKey_GamepadLStickUp : ImGuiKey_GamepadLStickDown), true});
        stick_was = st;
    }
}

void notify(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG_NOTE("overlay: %s", buf);
    std::lock_guard lk(g_notice_lock);
    if (g_notices.size() > 4) g_notices.pop_front();
    g_notices.push_back({buf, std::chrono::steady_clock::now() + std::chrono::milliseconds(2500)});
}

void draw(const VkInfo& vk, VkCommandBuffer cmd, VkImage img, VkFormat fmt, VkExtent2D ext) {
    static bool was_open = false;
    const bool open = g_open;
    if (!open && !was_open && !has_notices() && !cheat_menu::hud_wanted()) {
        // keep the queue short while nothing is shown
        std::lock_guard lk(g_in_lock);
        g_input.clear();
        return;
    }
    was_open = open;
    g_vk = vk;
    if (!g_ready || fmt != g_format) {
        if (!init_backend(vk, fmt)) { LOG_ERR("overlay: ImGui Vulkan init failed"); g_open = false; return; }
    }
    ensure_font(ext.height);
    const VkImageView view = view_for(img, fmt, ext);
    if (!view) return;

    ImGuiIO& io = ImGui::GetIO();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(1e-4f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    io.DisplaySize = ImVec2(float(ext.width), float(ext.height));
    io.MouseDrawCursor = open;
    feed_input();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (open) cheat_menu::build();
    cheat_menu::build_hud();
    draw_notices();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cmd);
    vkCmdEndRendering(cmd);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace overlay

void overlay_notify(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    overlay::notify("%s", buf);
}
bool overlay_menu_open() { return overlay::menu_open(); }
void overlay_set_menu_open(int open) { if (open == 2) cheat_menu::show_skins(); overlay::set_open(open != 0); }
