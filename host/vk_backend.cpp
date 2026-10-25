// Vulkan renderer: surfaces (render targets) keyed by guest address, shader translation via
// rsx_vp/rsx_fp + shaderc, CPU texture decode via rsx_tex, vertex data converted to vec4
// streams, one command buffer per frame submitted on flip.
#include "overlay.h"
#include "vk_backend.h"

#include "rsx.h"
#include "rsx_fp.h"
#include "rsx_iface.h"
#include "rsx_tex.h"
#include "rsx_vp.h"

#include <shaderc/shaderc.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace rsx {
namespace {

[[noreturn]] void vk_fail(const char* what, VkResult r) {
    LOG_ERR("vulkan: %s failed (VkResult %d)", what, int(r));
    std::fflush(stderr);
    std::abort();
}
#define VKC(x) do { const VkResult r_ = (x); if (r_ != VK_SUCCESS) vk_fail(#x, r_); } while (0)

constexpr u32 kFrames = 2;
constexpr VkDeviceSize kStreamSize = 96ull << 20;

u64 fnv(const void* p, size_t n, u64 h = 0xcbf29ce484222325ull) {
    const u8* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

void warn_once(const std::string& what) {
    static std::map<std::string, bool> seen;
    if (!seen[what]) { seen[what] = true; LOG_ERR("vk: %s", what.c_str()); }
}

inline float f16_to_f32(u16 h) {
    const u32 s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    u32 bits;
    if (e == 0) {
        if (m == 0) bits = s << 31;
        else { int ex = -1; u32 mm = m; do { ++ex; mm <<= 1; } while (!(mm & 0x400)); bits = (s << 31) | u32(127 - 15 - ex) << 23 | (mm & 0x3ff) << 13; }
    } else if (e == 31) bits = (s << 31) | 0x7f800000 | (m << 13);
    else bits = (s << 31) | (e + 112) << 23 | (m << 13);
    float f; std::memcpy(&f, &bits, 4); return f;
}

// ---- small Vulkan object wrappers ---------------------------------------------------------
struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; u8* ptr = nullptr; VkDeviceSize size = 0; };

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;   // attachment / default view
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    u32 w = 0, h = 0, depth = 1, layers = 1, mips = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
};

struct Surface {
    Image img;
    u32 ea = 0, rsx_format = 0, pitch = 0;
    bool depth = false;
    u64 last_frame = 0;
    std::map<u64, VkImageView> sample_views;  // keyed by component mapping
};

struct Texture {
    Image img;
    TexDesc desc;
    u64 hash = 0, validated_frame = ~0ull, last_frame = 0;
    u32 guest_size = 0;
};

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    Buffer stream;
    VkDeviceSize stream_off = 0;
    std::vector<std::function<void()>> garbage;
    bool pending = false;
};

struct PipelineKey {
    VkShaderModule vs, fs;
    u32 topology_class;
    u32 ncolor;
    VkFormat color[4];
    VkFormat depth;
    u32 has_stencil;
    struct Blend { u32 enable, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a, mask; } blend[4];
};

struct PassState {
    VkImageView color[4] = {};
    VkFormat color_fmt[4] = {};
    u32 ncolor = 0;
    VkImageView depth = VK_NULL_HANDLE;
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    bool stencil = false;
    u32 w = 0, h = 0;
    bool operator==(const PassState& o) const {
        return ncolor == o.ncolor && depth == o.depth && w == o.w && h == o.h &&
               std::equal(color, color + ncolor, o.color);
    }
};

struct Targets {
    PassState pass;
    Surface* color[4] = {};
    Surface* depth = nullptr;
    u32 clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    u32 aa_x = 1, aa_y = 1;  // multisampled surfaces store each sample as a pixel: 2x1 or 2x2
    bool valid = false;
};

VkBlendFactor blend_factor(u32 f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(u32 e) {
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF006: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0xF007: return VK_BLEND_OP_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp compare_op(u32 f) { return (f >= 0x200 && f <= 0x207) ? VkCompareOp(f - 0x200) : VK_COMPARE_OP_ALWAYS; }

VkStencilOp stencil_op(u32 op) {
    switch (op) {
    case 0: return VK_STENCIL_OP_ZERO;
    case 0x1E00: return VK_STENCIL_OP_KEEP;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode address_mode(u32 w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: case 7: case 8: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// PNG writer for frame dumps (RGBA8).
void write_png(const std::string& path, const u8* rgba, u32 w, u32 h) {
    std::vector<u8> raw;
    raw.reserve(size_t(w * 4 + 1) * h);
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<u8> z(zlen);
    compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 3);
    z.resize(zlen);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto be32 = [](u8* p, u32 v) { p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); };
    auto chunk = [&](const char* type, const u8* data, u32 len) {
        u8 hdr[8]; be32(hdr, len); std::memcpy(hdr + 4, type, 4);
        std::fwrite(hdr, 8, 1, f);
        if (len) std::fwrite(data, len, 1, f);
        uLong crc = crc32(0, hdr + 4, 4);
        if (len) crc = crc32(crc, data, len);
        u8 c[4]; be32(c, u32(crc)); std::fwrite(c, 4, 1, f);
    };
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::fwrite(sig, 8, 1, f);
    u8 ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), u32(z.size()));
    chunk("IEND", nullptr, 0);
    std::fclose(f);
}

// ======================================================================================
class VkRenderer final : public Renderer {
public:
    bool init(const VkPresentTarget* target);
    void draw() override;
    void clear(u32 mask) override;
    void flip(u32 buffer) override;
    bool blit(const BlitParams& b) override;

private:
    // device
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memprops_{};
    VkPhysicalDeviceFeatures features_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR push_descriptors_ = nullptr;
    VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool bc_ = false;

    // presentation
    const VkPresentTarget* target_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swap_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkSemaphore> swap_done_;
    bool swap_dirty_ = false;

    // frames
    Frame frames_[kFrames];
    u32 fi_ = 0;
    u64 frame_no_ = 0;
    // Per-draw reuse: the stream buffer resets bump stream_epoch_, so an allocation made in an
    // older epoch is gone. Vertex constants and the analysed vertex program are kept while the
    // game hasn't written new ones.
    u64 stream_epoch_ = 0;
    u64 vc_upload_gen_ = 0, vc_upload_epoch_ = ~0ull;
    VkDeviceSize vc_upload_off_ = 0;
    VkBuffer vc_upload_buf_ = VK_NULL_HANDLE;
    u64 vinfo_gen_ = 0;
    u32 vinfo_start_ = ~0u;
    VpInfo vinfo_cached_{};
    Frame& F() { return frames_[fi_]; }
    VkCommandBuffer cmd() { return frames_[fi_].cmd; }

    // pipeline objects
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache pcache_ = VK_NULL_HANDLE;
    shaderc_compiler_t compiler_ = nullptr;
    shaderc_compile_options_t copts_ = nullptr;
    std::unordered_map<u64, VkShaderModule> vs_cache_, fs_cache_;
    std::unordered_map<u64, VkPipeline> pipelines_;
    std::unordered_map<u64, VkSampler> samplers_;
    Image dummy_[3];  // 2D, cube, 3D
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    // caches
    // keyed by address, depth flag and Vulkan format: the game renders several formats at one
    // address each frame, and recreating the image on every switch was costly and lost contents
    std::unordered_map<u64, std::unique_ptr<Surface>> surfaces_;
    std::unordered_map<u64, Surface*> latest_;  // address|depth -> surface rendered there last
    std::unordered_map<u64, std::unique_ptr<Texture>> textures_;
    Image display_upload_;

    // pass
    bool in_pass_ = false;
    PassState pass_;

    // dumping
    std::string dump_dir_;
    u32 dump_every_ = 60;
    Buffer readback_;

    // stats
    u64 draws_in_frame_ = 0, total_draws_ = 0, pipelines_created_ = 0;

    // helpers
    u32 memory_type(u32 bits, VkMemoryPropertyFlags want);
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void destroy_buffer(Buffer& b);
    Image create_image(VkFormat fmt, u32 w, u32 h, u32 depth, u32 layers, u32 mips, VkImageViewType vt, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkComponentMapping mapping = {});
    void destroy_image_later(Image img);
    VkImageView create_view(const Image& img, VkImageAspectFlags aspect, VkComponentMapping mapping);
    void barrier();
    void begin_frame();
    void submit_frame(VkSemaphore wait, VkSemaphore signal);
    void flush_and_wait();
    struct Alloc { VkBuffer buf; VkDeviceSize off; u8* ptr; };
    Alloc stream(VkDeviceSize size, VkDeviceSize align = 16);
    void end_pass();
    void begin_pass(const PassState& p);
    void transition_new(const Image& img);

    bool create_swapchain();
    VkShaderModule compile(const std::string& src, bool vertex, u64 hash);
    Surface* get_surface(u32 ea, bool depth, u32 rsx_fmt, u32 w, u32 h, u32 pitch);
    Surface* find_color_surface(u32 ea);
    Targets prepare_targets(bool for_clear);
    VkImageView texture_view(u32 unit, TexKind& kind, float scale[2], VkSampler& sampler);
    VkSampler get_sampler(u32 unit, u32 mips);
    VkPipeline get_pipeline(const PipelineKey& k);
    void present(Surface* s, const DisplayBuffer& db);
};

// ---- device setup -----------------------------------------------------------------------
bool VkRenderer::init(const VkPresentTarget* target) {
    target_ = target;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "tod";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> exts;
    if (target) exts = target->instance_extensions;
    std::vector<const char*> layers;
    if (std::getenv("TOD_VK_VALIDATE")) layers.push_back("VK_LAYER_KHRONOS_validation");
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifdef __APPLE__
    // MoltenVK is a "portability" driver: the loader lists it only when asked to
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = u32(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = u32(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        if (layers.empty()) { LOG_ERR("vulkan: no Vulkan 1.3 instance available"); return false; }
        ici.enabledLayerCount = 0;
        if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) { LOG_ERR("vulkan: no Vulkan 1.3 instance"); return false; }
        LOG_ERR("vulkan: validation layer not available");
    }
    if (target) surface_ = target->create_surface(instance_);

    u32 n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    int best_score = -1;
    const char* want = std::getenv("TOD_GPU");
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        u32 ne = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ep(ne);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, ep.data());
        auto has = [&](const char* e) { return std::any_of(ep.begin(), ep.end(), [&](auto& x) { return !std::strcmp(x.extensionName, e); }); };
        if (!has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
        if (surface_ && !has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want && std::strstr(p.deviceName, want)) score = 100;
        if (score > best_score) { best_score = score; phys_ = d; }
    }
    if (!phys_) { LOG_ERR("vulkan: no device with Vulkan 1.3 and VK_KHR_push_descriptor"); return false; }
    vkGetPhysicalDeviceProperties(phys_, &props_);
    vkGetPhysicalDeviceMemoryProperties(phys_, &memprops_);
    vkGetPhysicalDeviceFeatures(phys_, &features_);
    LOG_NOTE("vulkan: using %s (API %u.%u.%u)", props_.deviceName, VK_API_VERSION_MAJOR(props_.apiVersion),
             VK_API_VERSION_MINOR(props_.apiVersion), VK_API_VERSION_PATCH(props_.apiVersion));

    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &n, qf.data());
    bool found = false;
    for (u32 i = 0; i < n && !found; ++i) {
        VkBool32 present = VK_TRUE;
        if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { queue_family_ = i; found = true; }
    }
    if (!found) { LOG_ERR("vulkan: no graphics queue that can present"); return false; }

    VkPhysicalDeviceFeatures enable{};
    enable.textureCompressionBC = features_.textureCompressionBC;
    enable.samplerAnisotropy = features_.samplerAnisotropy;
    enable.independentBlend = features_.independentBlend;
    enable.depthClamp = features_.depthClamp;
    enable.fillModeNonSolid = features_.fillModeNonSolid;
    enable.wideLines = features_.wideLines;
    enable.shaderClipDistance = features_.shaderClipDistance;
    enable.largePoints = features_.largePoints;
    bc_ = features_.textureCompressionBC;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<const char*> dexts{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (surface_) dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
#ifdef __APPLE__
    dexts.push_back("VK_KHR_portability_subset");  // required on MoltenVK
#endif
    dci.enabledExtensionCount = u32(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pEnabledFeatures = &enable;
    VKC(vkCreateDevice(phys_, &dci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, queue_family_, 0, &queue_);
    push_descriptors_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(dev_, "vkCmdPushDescriptorSetKHR"));

    {   // depth-stencil format
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        const auto need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need) d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    VKC(vkCreateCommandPool(dev_, &pci, nullptr, &pool_));
    for (auto& f : frames_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(dev_, &ai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKC(vkCreateFence(dev_, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(dev_, &sci, nullptr, &f.acquire));
        f.stream = create_buffer(kStreamSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }

    // descriptor set layout (push descriptors) and pipeline layout
    std::vector<VkDescriptorSetLayoutBinding> b;
    b.push_back({kVertexConstBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    b.push_back({kFragmentConstBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    b.push_back({kDrawStateBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (u32 i = 0; i < 16; ++i) b.push_back({kTextureBindingBase + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (u32 i = 0; i < 4; ++i) b.push_back({kVertexTextureBindingBase + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = u32(b.size());
    dl.pBindings = b.data();
    VKC(vkCreateDescriptorSetLayout(dev_, &dl, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    VKC(vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_));
    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VKC(vkCreatePipelineCache(dev_, &pcci, nullptr, &pcache_));

    compiler_ = shaderc_compiler_initialize();
    copts_ = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(copts_, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_optimization_level(copts_, shaderc_optimization_level_performance);

    if (const char* d = std::getenv("TOD_DUMP_FRAMES")) {
        dump_dir_ = d;
        if (auto c = dump_dir_.find(','); c != std::string::npos) { dump_every_ = std::max(1, std::atoi(dump_dir_.c_str() + c + 1)); dump_dir_.resize(c); }
        std::filesystem::create_directories(dump_dir_);
        readback_ = create_buffer(4096ull * 4096 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }

    begin_frame();
    // dummy textures for unbound units
    const VkImageUsageFlags tu = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dummy_[0] = create_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D, tu, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy_[1] = create_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, 1, VK_IMAGE_VIEW_TYPE_CUBE, tu, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy_[2] = create_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, VK_IMAGE_VIEW_TYPE_3D, tu, VK_IMAGE_ASPECT_COLOR_BIT);
    for (auto& d : dummy_) {
        transition_new(d);
        VkClearColorValue c{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers};
        vkCmdClearColorImage(cmd(), d.img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    }
    barrier();
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    VKC(vkCreateSampler(dev_, &sci, nullptr, &dummy_sampler_));
    if (surface_ && !create_swapchain()) return false;
    return true;
}

u32 VkRenderer::memory_type(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < memprops_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want) return i;
    vk_fail("memory_type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

Buffer VkRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VKC(vkCreateBuffer(dev_, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(dev_, &mai, nullptr, &b.mem));
    VKC(vkBindBufferMemory(dev_, b.buf, b.mem, 0));
    if (host) VKC(vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.ptr)));
    return b;
}

void VkRenderer::destroy_buffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = {};
}

VkImageView VkRenderer::create_view(const Image& img, VkImageAspectFlags aspect, VkComponentMapping mapping) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.img;
    vci.viewType = img.view_type;
    vci.format = img.format;
    vci.components = mapping;
    vci.subresourceRange = {aspect, 0, img.mips, 0, img.layers};
    VkImageView v;
    VKC(vkCreateImageView(dev_, &vci, nullptr, &v));
    return v;
}

Image VkRenderer::create_image(VkFormat fmt, u32 w, u32 h, u32 depth, u32 layers, u32 mips, VkImageViewType vt,
                               VkImageUsageFlags usage, VkImageAspectFlags aspect, VkComponentMapping mapping) {
    Image im;
    im.format = fmt; im.w = w; im.h = h; im.depth = depth; im.layers = layers; im.mips = mips; im.view_type = vt; im.aspect = aspect;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = vt == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, depth};
    ici.mipLevels = mips;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(dev_, &ici, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(dev_, &mai, nullptr, &im.mem));
    VKC(vkBindImageMemory(dev_, im.img, im.mem, 0));
    im.view = create_view(im, aspect, mapping);
    return im;
}

void VkRenderer::destroy_image_later(Image img) {
    F().garbage.push_back([this, img] {
        if (img.view) vkDestroyImageView(dev_, img.view, nullptr);
        if (img.img) vkDestroyImage(dev_, img.img, nullptr);
        if (img.mem) vkFreeMemory(dev_, img.mem, nullptr);
    });
}

void VkRenderer::transition_new(const Image& img) {
    end_pass();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.img;
    b.subresourceRange = {img.aspect, 0, img.mips, 0, img.layers};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VkRenderer::barrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ---- frames ---------------------------------------------------------------------------------
void VkRenderer::begin_frame() {
    Frame& f = F();
    if (f.pending) {
        VKC(vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX));
        VKC(vkResetFences(dev_, 1, &f.fence));
        f.pending = false;
    }
    for (auto& g : f.garbage) g();
    f.garbage.clear();
    f.stream_off = 0;
    ++stream_epoch_;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(f.cmd, 0));
    VKC(vkBeginCommandBuffer(f.cmd, &bi));
}

void VkRenderer::submit_frame(VkSemaphore wait, VkSemaphore signal) {
    end_pass();
    Frame& f = F();
    VKC(vkEndCommandBuffer(f.cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.waitSemaphoreCount = wait ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = &signal;
    VKC(vkQueueSubmit(queue_, 1, &si, f.fence));
    f.pending = true;
}

void VkRenderer::flush_and_wait() {
    const bool was_in_pass = in_pass_;
    PassState p = pass_;
    submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
    Frame& f = F();
    VKC(vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(dev_, 1, &f.fence));
    f.pending = false;
    f.stream_off = 0;
    ++stream_epoch_;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(f.cmd, 0));
    VKC(vkBeginCommandBuffer(f.cmd, &bi));
    (void)was_in_pass; (void)p;  // the next draw re-begins its pass
}

VkRenderer::Alloc VkRenderer::stream(VkDeviceSize size, VkDeviceSize align) {
    Frame& f = F();
    VkDeviceSize off = (f.stream_off + align - 1) & ~(align - 1);
    if (off + size > f.stream.size) {
        if (size > f.stream.size) vk_fail("stream allocation too large", VK_ERROR_OUT_OF_HOST_MEMORY);
        flush_and_wait();
        off = 0;
    }
    f.stream_off = off + size;
    return {f.stream.buf, off, f.stream.ptr + off};
}

// ---- passes -----------------------------------------------------------------------------------
void VkRenderer::end_pass() {
    if (!in_pass_) return;
    vkCmdEndRendering(cmd());
    in_pass_ = false;
    barrier();
}

void VkRenderer::begin_pass(const PassState& p) {
    if (in_pass_ && pass_ == p) return;
    end_pass();
    VkRenderingAttachmentInfo ca[4]{};
    for (u32 i = 0; i < p.ncolor; ++i) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = p.color[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = p.depth;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {p.w, p.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = p.ncolor;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = p.depth ? &da : nullptr;
    ri.pStencilAttachment = p.depth && p.stencil ? &da : nullptr;
    vkCmdBeginRendering(cmd(), &ri);
    in_pass_ = true;
    pass_ = p;
}

// ---- surfaces -----------------------------------------------------------------------------
Surface* VkRenderer::get_surface(u32 ea, bool depth, u32 rsx_fmt, u32 w, u32 h, u32 pitch) {
    VkFormat vf;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth) {
        vf = rsx_fmt == 1 ? VK_FORMAT_D16_UNORM : d24s8_;
        aspect = rsx_fmt == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    } else {
        switch (rsx_fmt) {
        case 3: vf = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
        case 9: vf = VK_FORMAT_R8_UNORM; break;
        case 10: vf = VK_FORMAT_R8G8_UNORM; break;
        case 11: vf = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case 12: vf = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        case 13: vf = VK_FORMAT_R32_SFLOAT; break;
        case 1: case 2: vf = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
        default: vf = VK_FORMAT_R8G8B8A8_UNORM; break;
        }
    }
    const u64 akey = (u64(ea) << 1) | (depth ? 1 : 0), key = akey | (u64(vf) << 40);
    auto it = surfaces_.find(key);
    if (it != surfaces_.end()) {
        Surface* s = it->second.get();
        if (s->img.format == vf && s->img.w >= w && s->img.h >= h) {
            s->rsx_format = rsx_fmt; s->pitch = pitch; s->last_frame = frame_no_;
            latest_[akey] = s;
            return s;
        }
        if (s->img.format == vf) { w = std::max(w, s->img.w); h = std::max(h, s->img.h); }
        end_pass();
        for (auto& [k, v] : s->sample_views) { VkImageView vv = v; F().garbage.push_back([this, vv] { vkDestroyImageView(dev_, vv, nullptr); }); }
        destroy_image_later(s->img);
        if (latest_[akey] == s) latest_.erase(akey);
        surfaces_.erase(it);
    }
    auto s = std::make_unique<Surface>();
    s->ea = ea; s->depth = depth; s->rsx_format = rsx_fmt; s->pitch = pitch; s->last_frame = frame_no_;
    const VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    s->img = create_image(vf, w, h, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D, usage, aspect);
    transition_new(s->img);
    VkImageSubresourceRange r{aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue c{1.f, 0};
        vkCmdClearDepthStencilImage(cmd(), s->img.img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    } else {
        VkClearColorValue c{};
        vkCmdClearColorImage(cmd(), s->img.img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    }
    barrier();
    if (g_log_level >= 1)
        LOG_NOTE("vk: new %s surface at 0x%08x %ux%u fmt %u", depth ? "depth" : "color", ea, w, h, rsx_fmt);
    Surface* raw = s.get();
    surfaces_[key] = std::move(s);
    latest_[akey] = raw;
    return raw;
}

// NV3089 from a rendered surface: scale on the GPU into the destination surface. Copies whose
// source is not a surface are left to the command processor's memory copy.
bool VkRenderer::blit(const BlitParams& b) {
    Surface* src = nullptr;
    for (auto& [k, v] : latest_) {
        Surface* s = v;
        if (s->depth || !s->pitch) continue;
        if (b.src_ea >= s->ea && b.src_ea < s->ea + s->pitch * s->img.h) { src = s; break; }
    }
    if (!src) return false;
    if (b.dst_fmt != 0xa && b.dst_fmt != 4) return false;
    const u32 bpp = src->img.format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 2;
    const u32 off = b.src_ea - src->ea;
    const float ox = float((off % src->pitch) / bpp), oy = float(off / src->pitch);
    Surface* dst = get_surface(b.dst_ea, false, b.dst_fmt == 4 ? 3 : 8, b.dx + b.dw, b.dy + b.dh, b.dst_pitch);
    if (dst == src) { warn_once("scaled image copy within one surface"); return true; }
    end_pass();
    VkImageBlit r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = r.srcSubresource;
    auto clampx = [&](float v) { return s32(std::clamp(v, 0.f, float(src->img.w))); };
    auto clampy = [&](float v) { return s32(std::clamp(v, 0.f, float(src->img.h))); };
    r.srcOffsets[0] = {clampx(ox + b.sx), clampy(oy + b.sy), 0};
    r.srcOffsets[1] = {clampx(ox + b.sx + b.sw), clampy(oy + b.sy + b.sh), 1};
    r.dstOffsets[0] = {s32(b.dx), s32(b.dy), 0};
    r.dstOffsets[1] = {s32(std::min(b.dx + b.dw, dst->img.w)), s32(std::min(b.dy + b.dh, dst->img.h)), 1};
    if (r.srcOffsets[1].x <= r.srcOffsets[0].x || r.srcOffsets[1].y <= r.srcOffsets[0].y) return true;
    vkCmdBlitImage(cmd(), src->img.img, VK_IMAGE_LAYOUT_GENERAL, dst->img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r,
                   b.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    barrier();
    return true;
}

Surface* VkRenderer::find_color_surface(u32 ea) {
    auto it = latest_.find(u64(ea) << 1);
    return it == latest_.end() ? nullptr : it->second;
}

Targets VkRenderer::prepare_targets(bool for_clear) {
    Targets t;
    const u32 fmt = reg(0x208);
    const u32 color_fmt = fmt & 0x1f, depth_fmt = (fmt >> 5) & 7;
    t.clip_x = reg(0x200) & 0xffff; t.clip_w = reg(0x200) >> 16;
    t.clip_y = reg(0x204) & 0xffff; t.clip_h = reg(0x204) >> 16;
    // Antialiased surfaces hold every sample as its own pixel (2 across for the 2-sample mode,
    // 2x2 for the 4-sample modes) and are read back that way as textures, so they are rendered
    // supersampled at that size.
    switch ((fmt >> 12) & 0xf) {
    case 3: t.aa_x = 2; break;
    case 4: case 5: t.aa_x = t.aa_y = 2; break;
    default: break;
    }
    const u32 w = (t.clip_x + t.clip_w) * t.aa_x, h = (t.clip_y + t.clip_h) * t.aa_y;
    if (!t.clip_w || !t.clip_h || w > 4096 || h > 4096) return t;
    static const u32 off_regs[4] = {0x210, 0x218, 0x288, 0x28c}, dma_regs[4] = {0x194, 0x18c, 0x1b4, 0x1b8},
                     pitch_regs[4] = {0x20c, 0x21c, 0x280, 0x284};
    u32 idx[4], n = 0;
    switch (reg(0x220)) {
    case 1: idx[n++] = 0; break;
    case 2: idx[n++] = 1; break;
    case 0x13: idx[n++] = 0; idx[n++] = 1; break;
    case 0x17: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; break;
    case 0x1f: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; idx[n++] = 3; break;
    default: break;
    }
    for (u32 k = 0; k < n; ++k) {
        const u32 i = idx[k], pitch = reg(pitch_regs[i]);
        const u32 ea = address(reg(off_regs[i]), reg(dma_regs[i]));
        if (pitch <= 64 || ea == ~0u) continue;
        Surface* s = get_surface(ea, false, color_fmt, w, h, pitch);
        t.color[t.pass.ncolor] = s;
        t.pass.color[t.pass.ncolor] = s->img.view;
        t.pass.color_fmt[t.pass.ncolor] = s->img.format;
        ++t.pass.ncolor;
    }
    const u32 zpitch = reg(0x22c);
    const u32 zea = address(reg(0x214), reg(0x198));
    const bool need_depth = for_clear || (reg(0xa74) & 1) || (reg(0x328) & 1);
    if (depth_fmt && zpitch > 64 && zea != ~0u && need_depth) {
        Surface* s = get_surface(zea, true, depth_fmt, w, h, zpitch);
        t.depth = s;
        t.pass.depth = s->img.view;
        t.pass.depth_fmt = s->img.format;
        t.pass.stencil = depth_fmt != 1;
    }
    if (!t.pass.ncolor && !t.depth) return t;
    // render area: smallest attachment
    t.pass.w = ~0u; t.pass.h = ~0u;
    for (u32 i = 0; i < t.pass.ncolor; ++i) { t.pass.w = std::min(t.pass.w, t.color[i]->img.w); t.pass.h = std::min(t.pass.h, t.color[i]->img.h); }
    if (t.depth) { t.pass.w = std::min(t.pass.w, t.depth->img.w); t.pass.h = std::min(t.pass.h, t.depth->img.h); }
    t.valid = true;
    return t;
}

// ---- clear ----------------------------------------------------------------------------------
void VkRenderer::clear(u32 mask) {
    Targets t = prepare_targets(true);
    if (!t.valid) return;
    begin_pass(t.pass);
    // scissor-limited clear rectangle
    const u32 sx = reg(0x8c0) & 0xffff, sw = reg(0x8c0) >> 16, sy = reg(0x8c4) & 0xffff, sh = reg(0x8c4) >> 16;
    const u32 x0 = std::max(sx, t.clip_x) * t.aa_x, y0 = std::max(sy, t.clip_y) * t.aa_y;
    const u32 x1 = std::min({(sx + sw) * t.aa_x, (t.clip_x + t.clip_w) * t.aa_x, t.pass.w}),
              y1 = std::min({(sy + sh) * t.aa_y, (t.clip_y + t.clip_h) * t.aa_y, t.pass.h});
    if (x1 <= x0 || y1 <= y0) return;
    VkClearRect rect{{{s32(x0), s32(y0)}, {x1 - x0, y1 - y0}}, 0, 1};
    std::vector<VkClearAttachment> atts;
    if (mask & 0xf0) {
        if ((mask & 0xf0) != 0xf0) warn_once("partial color-channel clears are treated as full clears");
        const u32 c = reg(0x1d90);
        VkClearValue v{};
        v.color.float32[0] = float((c >> 16) & 0xff) / 255.f;
        v.color.float32[1] = float((c >> 8) & 0xff) / 255.f;
        v.color.float32[2] = float(c & 0xff) / 255.f;
        v.color.float32[3] = float(c >> 24) / 255.f;
        for (u32 i = 0; i < t.pass.ncolor; ++i) atts.push_back({VK_IMAGE_ASPECT_COLOR_BIT, i, v});
    }
    if (t.depth && (mask & 3)) {
        const u32 z = reg(0x1d8c);
        VkClearValue v{};
        VkImageAspectFlags aspect = 0;
        if (mask & 1) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((mask & 2) && t.pass.stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (t.depth->rsx_format == 1) v.depthStencil.depth = float(z & 0xffff) / 65535.f;
        else { v.depthStencil.depth = float(z >> 8) / 16777215.f; v.depthStencil.stencil = z & 0xff; }
        if (aspect) atts.push_back({aspect, 0, v});
    }
    if (!atts.empty()) vkCmdClearAttachments(cmd(), u32(atts.size()), atts.data(), 1, &rect);
}

// ---- shaders and pipelines ------------------------------------------------------------------
VkShaderModule VkRenderer::compile(const std::string& src, bool vertex, u64 hash) {
    char name[64];
    std::snprintf(name, sizeof name, "%s_%016llx", vertex ? "vp" : "fp", (unsigned long long)hash);
    shaderc_compilation_result_t res = shaderc_compile_into_spv(compiler_, src.data(), src.size(),
                                                                vertex ? shaderc_vertex_shader : shaderc_fragment_shader, name, "main", copts_);
    if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
        LOG_ERR("vk: %s failed to compile: %s", name, shaderc_result_get_error_message(res));
        const std::string dir = g_config.data_dir + "/shader_errors";
        std::filesystem::create_directories(dir);
        if (FILE* f = std::fopen((dir + "/" + name + (vertex ? ".vert" : ".frag")).c_str(), "w")) { std::fputs(src.c_str(), f); std::fclose(f); }
        shaderc_result_release(res);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = shaderc_result_get_length(res);
    ci.pCode = reinterpret_cast<const u32*>(shaderc_result_get_bytes(res));
    VkShaderModule m;
    VKC(vkCreateShaderModule(dev_, &ci, nullptr, &m));
    shaderc_result_release(res);
    if (std::getenv("TOD_DUMP_GLSL")) {
        const std::string dir = g_config.data_dir + "/shaders";
        std::filesystem::create_directories(dir);
        if (FILE* f = std::fopen((dir + "/" + name + (vertex ? ".vert" : ".frag")).c_str(), "w")) { std::fputs(src.c_str(), f); std::fclose(f); }
    }
    return m;
}

VkPipeline VkRenderer::get_pipeline(const PipelineKey& k) {
    const u64 h = fnv(&k, sizeof k);
    if (auto it = pipelines_.find(h); it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = k.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = k.fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    for (u32 i = 0; i < 16; ++i) {
        vb[i] = {i, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        va[i] = {i, i, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 16; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = 16; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k.topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k.topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.f;
    rs.depthClampEnable = features_.depthClamp;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[4]{};
    for (u32 i = 0; i < k.ncolor; ++i) {
        const auto& b = k.blend[i];
        cb[i].blendEnable = b.enable;
        cb[i].srcColorBlendFactor = blend_factor(b.src_rgb);
        cb[i].dstColorBlendFactor = blend_factor(b.dst_rgb);
        cb[i].colorBlendOp = blend_op(b.eq_rgb);
        cb[i].srcAlphaBlendFactor = blend_factor(b.src_a);
        cb[i].dstAlphaBlendFactor = blend_factor(b.dst_a);
        cb[i].alphaBlendOp = blend_op(b.eq_a);
        cb[i].colorWriteMask = b.mask;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.ncolor;
    cbs.pAttachments = cb;
    const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
    };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = u32(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = k.ncolor;
    pr.pColorAttachmentFormats = k.color;
    pr.depthAttachmentFormat = k.depth;
    pr.stencilAttachmentFormat = k.has_stencil ? k.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &pr;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs; gp.pDynamicState = &dy;
    gp.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(dev_, pcache_, 1, &gp, nullptr, &p);
    if (r != VK_SUCCESS) { LOG_ERR("vk: pipeline creation failed (%d)", int(r)); p = VK_NULL_HANDLE; }
    ++pipelines_created_;
    pipelines_[h] = p;
    return p;
}

// ---- textures ---------------------------------------------------------------------------------
VkSampler VkRenderer::get_sampler(u32 unit, u32 mips) {
    const u32 base = 0x1a00 + 0x20 * unit;
    const u32 addr = reg(base + 0x08), ctl0 = reg(base + 0x0c), filter = reg(base + 0x14), border = reg(base + 0x1c);
    const u64 key = fnv(&addr, 4, fnv(&ctl0, 4, fnv(&filter, 4, fnv(&border, 4, mips))));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const u32 mag = (filter >> 24) & 0xf, min = (filter >> 16) & 0xff;
    s.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    s.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    s.mipmapMode = (min == 5 || min == 6) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = address_mode(addr & 0xf);
    s.addressModeV = address_mode((addr >> 8) & 0xf);
    s.addressModeW = address_mode((addr >> 16) & 0xf);
    {
        // LOD bias: signed 1.4.8 fixed point in bits 0-12
        s32 bias = s32(filter << 19) >> 19;
        s.mipLodBias = float(bias) / 256.f;
    }
    s.minLod = float((ctl0 >> 19) & 0xfff) / 256.f;
    s.maxLod = (min == 1 || min == 2 || mips <= 1) ? 0.25f : float((ctl0 >> 7) & 0xfff) / 256.f;
    if (s.maxLod < s.minLod) s.maxLod = s.minLod;
    const u32 aniso = (ctl0 >> 4) & 7;
    if (aniso && features_.samplerAnisotropy) {
        static const float levels[8] = {1, 2, 4, 6, 8, 10, 12, 16};
        s.anisotropyEnable = VK_TRUE;
        s.maxAnisotropy = std::min(levels[aniso], props_.limits.maxSamplerAnisotropy);
    }
    s.borderColor = (border >> 24) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler smp;
    VKC(vkCreateSampler(dev_, &s, nullptr, &smp));
    samplers_[key] = smp;
    return smp;
}

VkImageView VkRenderer::texture_view(u32 unit, TexKind& kind, float scale[2], VkSampler& sampler) {
    const u32 base = 0x1a00 + 0x20 * unit;
    const u32 ctl0 = reg(base + 0x0c);
    kind = TexKind::Tex2D;
    scale[0] = scale[1] = 1.f;
    sampler = dummy_sampler_;
    static const long trace_frame = [] { const char* e = std::getenv("TOD_TEX_TRACE"); return e ? std::atol(e) : -1L; }();
    if (long(frame_no_) == trace_frame)
        LOG_NOTE("tex trace: unit %u ctl0 %08x fmt %08x offset %08x rect %08x", unit, ctl0, reg(base + 0x04), reg(base), reg(base + 0x18));
    if (!(ctl0 & 0x80000000u)) return dummy_[0].view;
    const u32 fmt = reg(base + 0x04);
    const u32 rect = reg(base + 0x18);
    TexDesc d;
    d.format = fmt;
    d.width = rect >> 16;
    d.height = rect & 0xffff;
    d.depth = reg(0x1840 + 4 * unit) >> 20;
    if (!d.depth) d.depth = 1;
    d.pitch = reg(0x1840 + 4 * unit) & 0xfffff;
    d.mipmaps = std::max<u32>(1, fmt >> 16);
    d.remap = reg(base + 0x10);
    const u32 loc = (fmt & 3) - 1;
    d.ea = address(reg(base), loc);
    const bool cube = fmt & 4;
    const u32 dim = (fmt >> 4) & 0xf;
    kind = cube ? TexKind::Cube : dim == 3 ? TexKind::Tex3D : TexKind::Tex2D;
    if (fmt & 0x4000) { scale[0] = 1.f / std::max<u32>(1, d.width); scale[1] = 1.f / std::max<u32>(1, d.height); }
    if (d.ea == ~0u || !d.width || !d.height) return kind == TexKind::Cube ? dummy_[1].view : kind == TexKind::Tex3D ? dummy_[2].view : dummy_[0].view;

    // render-to-texture: sample the surface image directly
    if (kind == TexKind::Tex2D) {
        Surface* s = find_color_surface(d.ea);
        if (!s) {
            auto it = latest_.find((u64(d.ea) << 1) | 1);
            if (it != latest_.end()) s = it->second;
        }
        if (s) {
            if (g_log_level >= 2) {
                static std::set<u32> seen;
                if (seen.insert(d.ea).second)
                    LOG_NOTE("vk: texture at 0x%08x (%ux%u fmt reg 0x%08x) sampled from a %s surface", d.ea, d.width, d.height, d.format, s->depth ? "depth" : "color");
            }
            if (s->img.w < d.width || s->img.h < d.height) warn_once("texture larger than its source surface");
            const VkComponentMapping m = s->depth ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R}
                                                  : surface_sample_mapping(fmt, d.remap);
            const u64 mk = u64(m.r) | u64(m.g) << 8 | u64(m.b) << 16 | u64(m.a) << 24;
            auto vit = s->sample_views.find(mk);
            if (vit == s->sample_views.end())
                vit = s->sample_views.emplace(mk, create_view(s->img, s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, m)).first;
            if (fmt & 0x4000) { scale[0] = 1.f / s->img.w; scale[1] = 1.f / s->img.h; }
            if (s->img.w != d.width || s->img.h != d.height) {
                // normalized coordinates address the texture's own size, a corner of the larger surface
                if (!(fmt & 0x4000)) { scale[0] = float(d.width) / s->img.w; scale[1] = float(d.height) / s->img.h; }
            }
            sampler = get_sampler(unit, 1);
            return vit->second;
        }
    }

    const u64 key = fnv(&d, sizeof d);
    Texture* t = nullptr;
    if (auto it = textures_.find(key); it != textures_.end()) t = it->second.get();
    if (t && t->validated_frame != frame_no_) {
        t->validated_frame = frame_no_;
        if (hash_guest_range(d.ea, t->guest_size) != t->hash) {
            destroy_image_later(t->img);
            textures_.erase(key);
            t = nullptr;
        }
    }
    if (!t) {
        DecodedTex dec;
        if (!decode_texture(d, dec) || dec.levels.empty()) {
            char w[96];
            std::snprintf(w, sizeof w, "texture format reg 0x%08x (%ux%u) not decoded", d.format, d.width, d.height);
            warn_once(w);
            return kind == TexKind::Cube ? dummy_[1].view : kind == TexKind::Tex3D ? dummy_[2].view : dummy_[0].view;
        }
        if (g_log_level >= 2) {
            static std::set<u32> seen;
            if (seen.insert(d.ea).second) {
                u32 nz = 0;
                for (u32 i = 0; i < std::min<u32>(dec.data.size(), 65536); ++i) nz += dec.data[i] != 0;
                LOG_NOTE("vk: texture at 0x%08x %ux%u fmt reg 0x%08x (%u of the first 64K bytes nonzero)", d.ea, d.width, d.height, d.format, nz);
            }
        }
        auto nt = std::make_unique<Texture>();
        nt->desc = d;
        nt->guest_size = texture_guest_size(d);
        nt->hash = hash_guest_range(d.ea, nt->guest_size);
        nt->validated_frame = frame_no_;
        if (dec.view_type == VK_IMAGE_VIEW_TYPE_1D) dec.view_type = VK_IMAGE_VIEW_TYPE_2D;
        const u32 mips = u32(dec.levels.size()) / dec.layers;
        nt->img = create_image(dec.format, dec.levels[0].width, dec.levels[0].height, dec.view_type == VK_IMAGE_VIEW_TYPE_3D ? dec.levels[0].depth : 1,
                               dec.layers, mips, dec.view_type, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, dec.mapping);
        transition_new(nt->img);
        Alloc a = stream(dec.data.size(), 16);
        std::memcpy(a.ptr, dec.data.data(), dec.data.size());
        std::vector<VkBufferImageCopy> regions;
        for (u32 layer = 0; layer < dec.layers; ++layer)
            for (u32 m = 0; m < mips; ++m) {
                const TexLevel& lv = dec.levels[layer * mips + m];
                VkBufferImageCopy c{};
                c.bufferOffset = a.off + lv.offset;
                c.bufferRowLength = lv.row_length;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, layer, 1};
                c.imageExtent = {lv.width, lv.height, std::max<u32>(1, lv.depth)};
                regions.push_back(c);
            }
        vkCmdCopyBufferToImage(cmd(), a.buf, nt->img.img, VK_IMAGE_LAYOUT_GENERAL, u32(regions.size()), regions.data());
        barrier();
        t = nt.get();
        textures_[key] = std::move(nt);
    }
    t->last_frame = frame_no_;
    sampler = get_sampler(unit, t->img.mips);
    return t->img.view;
}

// ---- draw ---------------------------------------------------------------------------------------
struct AttrSource { int kind = 0; /*0 const,1 array,2 push*/ u32 type = 0, size = 0, stride = 0, ea = 0, divider = 0; bool modulo = false; };

void fetch_attribute(const AttrSource& a, u32 vertex, float out[4]) {
    out[0] = out[1] = out[2] = 0.f; out[3] = 1.f;
    u32 v = vertex;
    if (a.divider > 1) v = a.modulo ? v % a.divider : v / a.divider;
    const u32 ea = a.ea + v * a.stride;
    const u8* p = mem::base + ea;
    switch (a.type) {
    case 1:  // S1: signed normalized 16-bit
        for (u32 i = 0; i < a.size; ++i) out[i] = std::max(-1.f, float(s16(ld16(const_cast<u8*>(p), 2 * i))) / 32767.f);
        break;
    case 2:  // F
        for (u32 i = 0; i < a.size; ++i) { u32 b = ld32(const_cast<u8*>(p), 4 * i); std::memcpy(&out[i], &b, 4); }
        break;
    case 3:  // SF (half)
        for (u32 i = 0; i < a.size; ++i) out[i] = f16_to_f32(ld16(const_cast<u8*>(p), 2 * i));
        break;
    case 4:  // UB normalized
        for (u32 i = 0; i < a.size; ++i) out[i] = float(p[i]) / 255.f;
        break;
    case 5:  // S32K: signed 16-bit, not normalized
        for (u32 i = 0; i < a.size; ++i) out[i] = float(s16(ld16(const_cast<u8*>(p), 2 * i)));
        break;
    case 6: {  // CMP: 11:11:10 signed normalized
        const u32 w = ld32(const_cast<u8*>(p), 0);
        out[0] = float(s32(w << 21) >> 21) / 1023.f;
        out[1] = float(s32((w >> 11) << 21) >> 21) / 1023.f;
        out[2] = float(s32(w) >> 22) / 511.f;
        break;
    }
    case 7:  // UB256: unsigned bytes, not normalized
        for (u32 i = 0; i < a.size; ++i) out[i] = float(p[i]);
        break;
    default: break;
    }
}

// A whole attribute array: the common formats (float, unsigned bytes) without the per-vertex switch.
void convert_attribute(const AttrSource& a, u32 first, u32 n, float* out) {
    if (a.divider <= 1 && (a.type == 2 || a.type == 4 || a.type == 7) && a.size >= 1 && a.size <= 4) {
        const u8* p = mem::base + a.ea + first * a.stride;
        const u32 sz = a.size;
        for (u32 v = 0; v < n; ++v, p += a.stride, out += 4) {
            out[0] = out[1] = out[2] = 0.f; out[3] = 1.f;
            if (a.type == 2) {
                for (u32 i = 0; i < sz; ++i) { u32 b; std::memcpy(&b, p + 4 * i, 4); b = __builtin_bswap32(b); std::memcpy(&out[i], &b, 4); }
            } else {
                const float k = a.type == 4 ? 1.f / 255.f : 1.f;
                for (u32 i = 0; i < sz; ++i) out[i] = float(p[i]) * k;
            }
        }
        return;
    }
    for (u32 v = 0; v < n; ++v) fetch_attribute(a, first + v, out + 4 * v);
}

void VkRenderer::draw() {
    Frontend& fe = g_fe;
    // A draw method covers at most 256 vertices, so one long strip or list arrives as several
    // ranges that continue each other: join them, or strips would restart (and flip winding)
    // at every 256th vertex
    if (fe.ranges.size() > 1) {
        std::vector<DrawRange> merged;
        for (const DrawRange& r : fe.ranges) {
            if (!merged.empty() && merged.back().first + merged.back().count == r.first) merged.back().count += r.count;
            else merged.push_back(r);
        }
        fe.ranges.swap(merged);
    }
    if (!(reg(0x1e98) & 0x01000000u) && reg(0x1e98) != 0) { /* conditional render: always draw */ }
    Targets t = prepare_targets(false);
    if (!t.valid) return;

    // --- programs
    const u32 vstart = reg(0x1ea0);
    if (vinfo_gen_ != fe.vp_gen || vinfo_start_ != vstart) {
        vinfo_cached_ = vp_analyze(fe.vp_ucode, vstart);
        vinfo_gen_ = fe.vp_gen; vinfo_start_ = vstart;
    }
    const VpInfo& vinfo = vinfo_cached_;
    VkShaderModule vs;
    if (auto it = vs_cache_.find(vinfo.hash); it != vs_cache_.end()) vs = it->second;
    else vs = vs_cache_[vinfo.hash] = compile(vp_to_glsl(fe.vp_ucode, vstart, vinfo), true, vinfo.hash);
    const u32 sp = reg(0x8e4);
    const u32 fea = address(sp & ~3u, (sp & 3) - 1);
    if (fea == ~0u || !vs) return;
    const u32 fmax = std::min<u32>(0x10000, fea >= kLocalBase ? kLocalBase + kLocalSize - fea : 0x10000);
    const FpInfo finfo = fp_analyze(mem::base + fea, fmax);
    FpKey fkey;
    fkey.shader_control = reg(0x1d60);
    TexKind kinds[16];
    float tscale[16][2];
    VkImageView views[16];
    VkSampler smps[16];
    for (u32 i = 0; i < 16; ++i) {
        if (finfo.textures_used & (1u << i)) views[i] = texture_view(i, kinds[i], tscale[i], smps[i]);
        else { kinds[i] = TexKind::Tex2D; tscale[i][0] = tscale[i][1] = 1.f; views[i] = dummy_[0].view; smps[i] = dummy_sampler_; }
        fkey.tex_kind[i] = kinds[i];
        if (finfo.textures_used & (1u << i)) {
            fkey.tex_gamma[i] = fp_texture_gamma(reg(0x1a04 + 0x20 * i), reg(0x1a08 + 0x20 * i));
            fkey.tex_expand[i] = fp_texture_expand(reg(0x1a04 + 0x20 * i), reg(0x1a08 + 0x20 * i), reg(0x1a10 + 0x20 * i));
        }
    }
    fkey.srgb_output = fp_srgb_output(reg(0x1fec), reg(0x208), fkey.shader_control);
    u64 fhash = fnv(&fkey, sizeof fkey, finfo.hash);
    VkShaderModule fs;
    if (auto it = fs_cache_.find(fhash); it != fs_cache_.end()) fs = it->second;
    else fs = fs_cache_[fhash] = compile(fp_to_glsl(mem::base + fea, finfo, fkey), false, fhash);
    if (!fs) return;

    // --- vertex sources
    AttrSource src[16];
    const bool immediate = fe.push_vertices > 0;
    const u32 base_offset = reg(0x1738), freq_ops = reg(0x1fc0);
    for (u32 i = 0; i < 16; ++i) {
        const u32 f = reg(0x1740 + 4 * i), o = reg(0x1680 + 4 * i);
        AttrSource& a = src[i];
        if (immediate && (fe.push_mask & (1u << i)) && fe.push[i].size() >= 4 * fe.push_vertices) { a.kind = 2; continue; }
        a.type = f & 0xf; a.size = (f >> 4) & 0xf; a.stride = (f >> 8) & 0xff; a.divider = f >> 16;
        a.modulo = (freq_ops >> i) & 1;
        if (a.size && a.type && !immediate) {
            a.ea = address((o & 0x7fffffff) + base_offset, o >> 31);
            if (a.ea != ~0u) { a.kind = 1; continue; }
        }
        a.kind = 0;
    }

    // --- indices / vertex range
    std::vector<u32> indices;
    Prim prim = fe.prim;
    bool use_indices = false, restart = false;
    u32 vmin = ~0u, vmax = 0;
    auto add_range_vertices = [&](u32 first, u32 count) { vmin = std::min(vmin, first); vmax = std::max(vmax, first + count - 1); };
    std::vector<std::vector<u32>> seqs;  // per range vertex-id sequences (indexed or converted)
    const bool convert = prim == Prim::Quads || prim == Prim::TriangleFan || prim == Prim::Polygon || prim == Prim::LineLoop;
    if (fe.indexed) {
        const u32 ia = reg(0x181c), idma = reg(0x1820);
        const u32 itype = (idma >> 4) & 0xf;
        const u32 iea = address(ia, idma & 0xf);
        if (iea == ~0u) return;
        const u32 base_index = reg(0x173c);
        const bool restart_en = reg(0x1dac) & 1;
        const u32 restart_idx = reg(0x1db0);
        for (auto r : fe.ranges) {
            std::vector<u32> s;
            s.reserve(r.count);
            for (u32 k = 0; k < r.count; ++k) {
                u32 v = itype == 1 ? ld16(mem::base, iea + 2 * (r.first + k)) : ld32(mem::base, iea + 4 * (r.first + k));
                if (restart_en && v == restart_idx) { s.push_back(~0u); continue; }
                v += base_index;
                s.push_back(v);
                vmin = std::min(vmin, v); vmax = std::max(vmax, v);
            }
            seqs.push_back(std::move(s));
        }
        use_indices = true;
    } else {
        for (auto r : fe.ranges) if (r.count) add_range_vertices(r.first, r.count);
        if (convert) {
            for (auto r : fe.ranges) {
                std::vector<u32> s(r.count);
                for (u32 k = 0; k < r.count; ++k) s[k] = r.first + k;
                seqs.push_back(std::move(s));
            }
            use_indices = true;
        }
    }
    if (vmin == ~0u || vmax < vmin) return;
    if (immediate) { vmin = 0; vmax = fe.push_vertices - 1; }
    const u32 nverts = vmax - vmin + 1;
    if (nverts > 4 * 1024 * 1024) { warn_once("draw with an implausible vertex range skipped"); return; }

    VkPrimitiveTopology topo;
    u32 topo_class;
    switch (prim) {
    case Prim::Points: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; topo_class = 0; break;
    case Prim::Lines: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; topo_class = 1; break;
    case Prim::LineLoop: case Prim::LineStrip: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; topo_class = 1; break;
    case Prim::TriangleStrip: case Prim::QuadStrip: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; topo_class = 2; break;
    default: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; topo_class = 2; break;
    }
    const bool strip = topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    if (use_indices) {
        for (auto& s : seqs) {
            // split at restart markers, convert each sub-sequence
            size_t i0 = 0;
            while (i0 < s.size()) {
                size_t i1 = i0;
                while (i1 < s.size() && s[i1] != ~0u) ++i1;
                const u32* q = s.data() + i0;
                const u32 n = u32(i1 - i0);
                auto emit = [&](u32 v) { indices.push_back(v - vmin); };
                if (n) {
                    if (strip && !indices.empty()) { indices.push_back(~0u); restart = true; }
                    switch (prim) {
                    case Prim::Quads:
                        for (u32 k = 0; k + 3 < n; k += 4) { emit(q[k]); emit(q[k + 1]); emit(q[k + 2]); emit(q[k + 2]); emit(q[k + 3]); emit(q[k]); }
                        break;
                    case Prim::TriangleFan: case Prim::Polygon:
                        for (u32 k = 1; k + 1 < n; ++k) { emit(q[0]); emit(q[k]); emit(q[k + 1]); }
                        break;
                    case Prim::LineLoop:
                        for (u32 k = 0; k < n; ++k) emit(q[k]);
                        emit(q[0]);
                        break;
                    case Prim::Triangles: for (u32 k = 0; k + 2 < n; k += 3) { emit(q[k]); emit(q[k + 1]); emit(q[k + 2]); } break;
                    case Prim::Lines: for (u32 k = 0; k + 1 < n; k += 2) { emit(q[k]); emit(q[k + 1]); } break;
                    default: for (u32 k = 0; k < n; ++k) emit(q[k]); break;
                    }
                }
                i0 = i1 + 1;
            }
        }
        if (indices.empty()) return;
    }

    // TOD_VP_ATTRS=1: once per vertex program, its array attributes and their first vertex
    static const bool log_attrs = std::getenv("TOD_VP_ATTRS");
    if (log_attrs) {
        static std::set<u64> seen;
        if (seen.insert(vinfo.hash).second) {
            for (u32 i = 0; i < 16; ++i) {
                const AttrSource& a = src[i];
                if (a.kind != 1) continue;
                const u8* p = mem::base + a.ea + vmin * a.stride;
                char hex[80] = {};
                for (u32 k = 0; k < std::min<u32>(a.stride ? a.stride : 16, 24); ++k) std::snprintf(hex + 2 * k, 3, "%02x", p[k]);
                float f[4];
                fetch_attribute(a, vmin, f);
                LOG_NOTE("vp %016llx attr%u type %u size %u stride %u div %u: %s -> %g %g %g %g", (unsigned long long)vinfo.hash, i,
                         a.type, a.size, a.stride, a.divider, hex, f[0], f[1], f[2], f[3]);
            }
        }
        // skinned meshes (UB256 bone indices in attr1, UB weights in attr2): vertices whose
        // weighted bones have empty matrices (bones at constant 32 + 3 * index)
        if (src[1].kind == 1 && src[1].type == 7 && src[2].kind == 1 && src[2].type == 4) {
            static std::map<u64, std::array<u64, 4>> st;  // verts, bad verts, max bone, draws
            auto& e = st[vinfo.hash];
            for (u32 v = vmin; v <= vmax; ++v) {
                const u8* ip = mem::base + src[1].ea + v * src[1].stride;
                const u8* wp = mem::base + src[2].ea + v * src[2].stride;
                bool bad = false;
                for (int k = 0; k < 4; ++k) {
                    if (!wp[k]) continue;
                    const u32 c0 = 32 + 3 * ip[k];
                    e[2] = std::max<u64>(e[2], ip[k]);
                    if (c0 + 2 >= 468) { bad = true; continue; }
                    float sum = 0;
                    for (int r = 0; r < 3; ++r) for (int q = 0; q < 4; ++q) sum += std::fabs(fe.vc[c0 + r][q]);
                    if (sum == 0) bad = true;
                }
                ++e[0]; e[1] += bad;
            }
            if (++e[3] % 500 == 0)
                LOG_NOTE("skin %016llx: %llu draws, %llu vertices, %llu with empty bones, max bone %llu", (unsigned long long)vinfo.hash,
                         (unsigned long long)e[3], (unsigned long long)e[0], (unsigned long long)e[1], (unsigned long long)e[2]);
        }
        static std::map<u64, int> nconst;
        if (vinfo.hash == 0x40bbc883d678dffeull || vinfo.hash == 0x6af7d606c5022321ull) {
            if (nconst[vinfo.hash]++ < 4)
                for (u32 r = 0; r < 468; ++r)
                    LOG_NOTE("vc %016llx draw %d c%u: %.6g %.6g %.6g %.6g", (unsigned long long)vinfo.hash, nconst[vinfo.hash], r,
                             fe.vc[r][0], fe.vc[r][1], fe.vc[r][2], fe.vc[r][3]);
        }
    }

    // --- vertex data
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16], vstrides[16];
    for (u32 i = 0; i < 16; ++i) {
        const AttrSource& a = src[i];
        if (a.kind == 0) {
            Alloc al = stream(16);
            std::memcpy(al.ptr, fe.attr_value[i], 16);
            vbufs[i] = al.buf; voffs[i] = al.off; vstrides[i] = 0;
        } else if (a.kind == 2) {
            Alloc al = stream(16ull * nverts);
            std::memcpy(al.ptr, fe.push[i].data(), 16ull * nverts);
            vbufs[i] = al.buf; voffs[i] = al.off; vstrides[i] = 16;
        } else if (a.stride == 0) {
            Alloc al = stream(16);
            fetch_attribute(a, 0, reinterpret_cast<float*>(al.ptr));
            vbufs[i] = al.buf; voffs[i] = al.off; vstrides[i] = 0;
        } else {
            Alloc al = stream(16ull * nverts);
            float* out = reinterpret_cast<float*>(al.ptr);
            convert_attribute(a, vmin, nverts, out);
            vbufs[i] = al.buf; voffs[i] = al.off; vstrides[i] = 16;
        }
    }
    Alloc ib{};
    if (use_indices) {
        ib = stream(4ull * indices.size(), 4);
        std::memcpy(ib.ptr, indices.data(), 4ull * indices.size());
    }

    // --- uniforms
    const VkDeviceSize ua = std::max<VkDeviceSize>(16, props_.limits.minUniformBufferOffsetAlignment);
    Alloc vc;
    if (vc_upload_gen_ == fe.vc_gen && vc_upload_epoch_ == stream_epoch_) {
        vc = {vc_upload_buf_, vc_upload_off_, nullptr};
    } else {
        vc = stream(sizeof fe.vc, ua);
        std::memcpy(vc.ptr, fe.vc, sizeof fe.vc);
        // stream() may have flushed (new epoch) before handing out this allocation
        vc_upload_gen_ = fe.vc_gen; vc_upload_epoch_ = stream_epoch_;
        vc_upload_buf_ = vc.buf; vc_upload_off_ = vc.off;
    }
    const u32 nfc = std::max<u32>(1, u32(finfo.const_offsets.size()));
    Alloc fc = stream(16ull * nfc, ua);
    std::memset(fc.ptr, 0, 16ull * nfc);
    for (u32 i = 0; i < finfo.const_offsets.size() && i < kMaxFpConsts; ++i)
        fp_read_constant(mem::base + fea, finfo.const_offsets[i], reinterpret_cast<float*>(fc.ptr) + 4 * i);
    Alloc dsa = stream(sizeof(DrawState), ua);
    DrawState& ds = *reinterpret_cast<DrawState*>(dsa.ptr);
    std::memset(&ds, 0, sizeof ds);
    {
        const float cw = float(t.clip_w), ch = float(t.clip_h);
        const float sx = regf(0xa30) / (cw / 2.f), ox = (regf(0xa20) - cw / 2.f) / (cw / 2.f);
        const float sy = regf(0xa34) / (ch / 2.f), oy = (regf(0xa24) - ch / 2.f) / (ch / 2.f);
        const float m[16] = {sx, 0, 0, ox, 0, sy, 0, oy, 0, 0, regf(0xa38), regf(0xa3c), 0, 0, 0, 1};
        std::memcpy(ds.scale_offset, m, sizeof m);
    }
    ds.alpha_func = (reg(0x304) & 1) ? reg(0x308) : 0;
    ds.alpha_ref = float(reg(0x30c) & 0xff) / 255.f;
    ds.two_sided_lighting = reg(0x142c) & 1;
    {
        const u32 aa = reg(0x1d7c);  // anti-aliasing control: enable, alpha to coverage, alpha to one
        const bool a2c = (aa & 1) && (aa & 0x10);
        ds.alpha_to_coverage = a2c ? t.aa_x * t.aa_y : 0;
        ds.alpha_to_one = a2c && (aa & 0x100);
        if (a2c) warn_once("alpha to coverage in use");
    }
    ds.transform_branch_bits = reg(0x1ff8);
    for (u32 i = 0; i < 16; ++i) { ds.tex_scale[i][0] = tscale[i][0]; ds.tex_scale[i][1] = tscale[i][1]; ds.tex_scale[i][2] = ds.tex_scale[i][3] = 1.f; }
    {
        const u32 sw = reg(0x1d88);
        const bool bottom = (sw >> 12) & 1;
        ds.wpos_scale = bottom ? -1.f : 1.f;
        ds.wpos_bias = bottom ? float(sw & 0xfff) : 0.f;
    }
    ds.fog_param0 = regf(0x8d0);
    ds.fog_param1 = regf(0x8d4);
    ds.fog_mode = reg(0x8cc);
    ds.shader_control = reg(0x1d60);
    ds.res_inv = 1.f;
    ds.wpos_ax = ds.wpos_ay = 1.f;

    // --- pipeline
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vs = vs; k.fs = fs; k.topology_class = topo_class;
    k.ncolor = t.pass.ncolor;
    for (u32 i = 0; i < k.ncolor; ++i) {
        k.color[i] = t.pass.color_fmt[i];
        auto& b = k.blend[i];
        b.enable = i == 0 ? (reg(0x310) & 1) : ((reg(0x36c) >> i) & 1);
        b.src_rgb = reg(0x314) & 0xffff; b.src_a = reg(0x314) >> 16;
        b.dst_rgb = reg(0x318) & 0xffff; b.dst_a = reg(0x318) >> 16;
        b.eq_rgb = reg(0x320) & 0xffff; b.eq_a = reg(0x320) >> 16;
        if (i == 0) {
            const u32 cm = reg(0x324);
            b.mask = ((cm >> 16) & 0xff ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm >> 8) & 0xff ? VK_COLOR_COMPONENT_G_BIT : 0) |
                     (cm & 0xff ? VK_COLOR_COMPONENT_B_BIT : 0) | ((cm >> 24) & 0xff ? VK_COLOR_COMPONENT_A_BIT : 0);
        } else {
            const u32 cm = reg(0x370) >> (4 * i);
            b.mask = (cm & 2 ? VK_COLOR_COMPONENT_R_BIT : 0) | (cm & 4 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                     (cm & 8 ? VK_COLOR_COMPONENT_B_BIT : 0) | (cm & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
        }
        if (!b.enable) { b.src_rgb = b.src_a = b.dst_rgb = b.dst_a = b.eq_rgb = b.eq_a = 0; }
    }
    k.depth = t.pass.depth ? t.pass.depth_fmt : VK_FORMAT_UNDEFINED;
    k.has_stencil = t.pass.depth && t.pass.stencil;
    VkPipeline pipe = get_pipeline(k);
    if (!pipe) return;

    // --- record
    begin_pass(t.pass);
    VkCommandBuffer cb = cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorBufferInfo bi[3] = {{vc.buf, vc.off, sizeof fe.vc}, {fc.buf, fc.off, 16ull * nfc}, {dsa.buf, dsa.off, sizeof(DrawState)}};
    VkDescriptorImageInfo ii[20];
    VkWriteDescriptorSet w[23];
    u32 nw = 0;
    for (u32 i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = i; w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[nw].pBufferInfo = &bi[i];
        ++nw;
    }
    for (u32 i = 0; i < 20; ++i) {
        const bool vtex = i >= 16;
        ii[i] = {vtex ? dummy_sampler_ : smps[i], vtex ? dummy_[0].view : views[i], VK_IMAGE_LAYOUT_GENERAL};
        if (!vtex && kinds[i] == TexKind::Cube && views[i] == dummy_[0].view) ii[i].imageView = dummy_[1].view;
        if (!vtex && kinds[i] == TexKind::Tex3D && views[i] == dummy_[0].view) ii[i].imageView = dummy_[2].view;
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = vtex ? kVertexTextureBindingBase + (i - 16) : kTextureBindingBase + i;
        w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i];
        ++nw;
    }
    push_descriptors_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, nw, w);

    VkViewport vpt{0.f, 0.f, float(t.clip_w * t.aa_x), float(t.clip_h * t.aa_y), 0.f, 1.f};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    {
        const u32 sx = (reg(0x8c0) & 0xffff) * t.aa_x, sw = (reg(0x8c0) >> 16) * t.aa_x;
        const u32 sy = (reg(0x8c4) & 0xffff) * t.aa_y, sh = (reg(0x8c4) >> 16) * t.aa_y;
        const u32 x0 = std::min(sx, t.pass.w), y0 = std::min(sy, t.pass.h);
        const u32 x1 = std::min({sx + sw, (t.clip_x + t.clip_w) * t.aa_x, t.pass.w}),
                  y1 = std::min({sy + sh, (t.clip_y + t.clip_h) * t.aa_y, t.pass.h});
        VkRect2D sc{{s32(x0), s32(y0)}, {x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0}};
        vkCmdSetScissor(cb, 0, 1, &sc);
    }
    {
        float lw = regf(0x3b8);
        if (!(lw > 0.f) || !features_.wideLines) lw = 1.f;
        u32 lwr = reg(0x3b8);  // 6.3 fixed point on RSX
        lw = features_.wideLines ? std::max(1.f, float(lwr) / 8.f) : 1.f;
        vkCmdSetLineWidth(cb, std::min(lw, 8.f));
    }
    const bool poly_offset = reg(0xa68) & 1;
    vkCmdSetDepthBiasEnable(cb, poly_offset && t.pass.depth);
    vkCmdSetDepthBias(cb, regf(0xa7c), 0.f, regf(0xa78));
    {
        const u32 bc = reg(0x31c);
        const float c[4] = {float((bc >> 16) & 0xff) / 255.f, float((bc >> 8) & 0xff) / 255.f, float(bc & 0xff) / 255.f, float(bc >> 24) / 255.f};
        vkCmdSetBlendConstants(cb, c);
    }
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (reg(0x183c) & 1) {
        switch (reg(0x1830)) {
        case 0x404: cull = VK_CULL_MODE_FRONT_BIT; break;
        case 0x405: cull = VK_CULL_MODE_BACK_BIT; break;
        case 0x408: cull = VK_CULL_MODE_FRONT_AND_BACK; break;
        default: break;
        }
    }
    vkCmdSetCullMode(cb, cull);
    vkCmdSetFrontFace(cb, reg(0x1834) == 0x900 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, topo);
    vkCmdSetPrimitiveRestartEnable(cb, restart);
    const bool depth_on = t.pass.depth && (reg(0xa74) & 1);
    vkCmdSetDepthTestEnable(cb, depth_on);
    vkCmdSetDepthWriteEnable(cb, depth_on && (reg(0xa70) & 1));
    vkCmdSetDepthCompareOp(cb, compare_op(reg(0xa6c)));
    const bool stencil_on = t.pass.depth && t.pass.stencil && (reg(0x328) & 1);
    vkCmdSetStencilTestEnable(cb, stencil_on);
    {
        const bool two = reg(0x348) & 1;
        const u32 fb = two ? 0x34c : 0x32c;  // back-face register block
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_op(reg(0x33c)), stencil_op(reg(0x344)), stencil_op(reg(0x340)), compare_op(reg(0x330)));
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op(reg(fb + 0x10)), stencil_op(reg(fb + 0x18)), stencil_op(reg(fb + 0x14)), compare_op(reg(fb + 0x04)));
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, reg(0x338) & 0xff);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, reg(fb + 0x0c) & 0xff);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, reg(0x32c) & 0xff);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, reg(fb) & 0xff);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, reg(0x334) & 0xff);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, reg(fb + 0x08) & 0xff);
    }
    vkCmdBindVertexBuffers2(cb, 0, 16, vbufs, voffs, nullptr, vstrides);
    if (use_indices) {
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, u32(indices.size()), 1, 0, 0, 0);
    } else {
        for (auto r : fe.ranges) {
            u32 count = r.count;
            if (prim == Prim::QuadStrip) count &= ~1u;
            if (count) vkCmdDraw(cb, count, 1, (immediate ? 0 : r.first - vmin), 0);
        }
    }
    ++draws_in_frame_;
    ++total_draws_;
}

// ---- flip / present ---------------------------------------------------------------------------
bool VkRenderer::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));
    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;
    swap_format_ = chosen.format;
    u32 w = 1280, h = 720;
    if (target_->drawable_size) target_->drawable_size(w, h);
    if (caps.currentExtent.width != ~0u) { w = caps.currentExtent.width; h = caps.currentExtent.height; }
    if (!w || !h) return false;
    swap_extent_ = {w, h};
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!target_->vsync) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR || (m == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)) mode = m;
    }
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = swap_extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR sc;
    VKC(vkCreateSwapchainKHR(dev_, &sci, nullptr, &sc));
    if (swapchain_) {
        vkDeviceWaitIdle(dev_);
        vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
        for (auto s : swap_done_) vkDestroySemaphore(dev_, s, nullptr);
    }
    swapchain_ = sc;
    vkGetSwapchainImagesKHR(dev_, sc, &n, nullptr);
    swap_images_.resize(n);
    vkGetSwapchainImagesKHR(dev_, sc, &n, swap_images_.data());
    swap_done_.resize(n);
    for (auto& s : swap_done_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(dev_, &si, nullptr, &s));
    }
    swap_dirty_ = false;
    return true;
}

void VkRenderer::flip(u32 buffer) {
    end_pass();
    const DisplayBuffer db = display_buffer(buffer);
    Surface* s = find_color_surface(kLocalBase + db.offset);
    if (!s && db.width && db.height && db.pitch) {
        // the CPU wrote the frame (or nothing rendered there yet): upload guest memory
        static bool told = false;
        if (!told) { told = true; LOG_NOTE("vk: display buffer %u has no rendered surface; presenting guest memory", buffer); }
        if (!display_upload_.img || display_upload_.w != db.width || display_upload_.h != db.height) {
            if (display_upload_.img) destroy_image_later(display_upload_);
            display_upload_ = create_image(VK_FORMAT_R8G8B8A8_UNORM, db.width, db.height, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
            transition_new(display_upload_);
        }
        Alloc a = stream(4ull * db.width * db.height);
        for (u32 y = 0; y < db.height; ++y) {
            const u8* row = mem::base + kLocalBase + db.offset + y * db.pitch;
            u8* out = a.ptr + 4ull * db.width * y;
            for (u32 x = 0; x < db.width; ++x) { out[4 * x] = row[4 * x + 1]; out[4 * x + 1] = row[4 * x + 2]; out[4 * x + 2] = row[4 * x + 3]; out[4 * x + 3] = 255; }
        }
        VkBufferImageCopy c{};
        c.bufferOffset = a.off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {db.width, db.height, 1};
        vkCmdCopyBufferToImage(cmd(), a.buf, display_upload_.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier();
    }
    present(s, db);
    ++frame_no_;
    if (g_log_level >= 1 && (frame_no_ <= 3 || frame_no_ % 600 == 0))
        LOG_NOTE("vk: frame %llu: %llu draws, %zu surfaces, %zu textures, %llu pipelines", (unsigned long long)frame_no_,
                 (unsigned long long)draws_in_frame_, surfaces_.size(), textures_.size(), (unsigned long long)pipelines_created_);
    draws_in_frame_ = 0;
    // evict textures unused for a while
    if (frame_no_ % 120 == 0) {
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (frame_no_ - it->second->last_frame > 600) { destroy_image_later(it->second->img); it = textures_.erase(it); }
            else ++it;
        }
    }
    fi_ = (fi_ + 1) % kFrames;
    begin_frame();
}

void VkRenderer::present(Surface* s, const DisplayBuffer& db) {
    const Image* src = s ? &s->img : (display_upload_.img ? &display_upload_ : nullptr);
    const u32 sw = src ? std::min(src->w, db.width ? db.width : src->w) : 0, sh = src ? std::min(src->h, db.height ? db.height : src->h) : 0;

    const bool dump = !dump_dir_.empty() && src && frame_no_ % dump_every_ == 0 && src->format == VK_FORMAT_R8G8B8A8_UNORM;
    if (dump) {
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {sw, sh, 1};
        vkCmdCopyImageToBuffer(cmd(), src->img, VK_IMAGE_LAYOUT_GENERAL, readback_.buf, 1, &c);
        barrier();
    }

    if (!surface_) {
        submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (dump) {
            VKC(vkWaitForFences(dev_, 1, &F().fence, VK_TRUE, UINT64_MAX));
            std::vector<u8> px(readback_.ptr, readback_.ptr + 4ull * sw * sh);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            char name[64];
            std::snprintf(name, sizeof name, "/frame_%06llu.png", (unsigned long long)frame_no_);
            write_png(dump_dir_ + name, px.data(), sw, sh);
        }
        return;
    }

    if (swap_dirty_) create_swapchain();
    u32 index = 0;
    VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, F().acquire, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swap_dirty_ = true;
        submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        return;
    }
    VkImage dst = swap_images_[index];
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue black{};
    vkCmdClearColorImage(cmd(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &b.subresourceRange);
    if (src && sw && sh) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        // letterbox to the window keeping the game's aspect (16:9 output at 1280x720, or the
        // widescreen setting, which widened the game's projection to match)
        const float aspect = std::abs(g_config.aspect - 16.f / 9.f) > 0.01f ? g_config.aspect : float(sw) / float(sh);
        u32 ow = swap_extent_.width, oh = u32(float(ow) / aspect);
        if (oh > swap_extent_.height) { oh = swap_extent_.height; ow = u32(float(oh) * aspect); }
        const s32 ox = s32(swap_extent_.width - ow) / 2, oy = s32(swap_extent_.height - oh) / 2;
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {s32(sw), s32(sh), 1};
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[0] = {ox, oy, 0};
        bl.dstOffsets[1] = {ox + s32(ow), oy + s32(oh), 1};
        vkCmdBlitImage(cmd(), src->img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    overlay::draw({instance_, phys_, dev_, queue_family_, queue_, u32(swap_images_.size())}, cmd(), dst, swap_format_, swap_extent_);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    submit_frame(F().acquire, swap_done_[index]);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &swap_done_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_dirty_ = true;
}

}  // namespace

Renderer* create_renderer(const VkPresentTarget* target) {
    bool native = g_config.native_renderer;
    if (const char* e = std::getenv("TOD_RENDERER")) native = std::strcmp(e, "legacy") != 0;
    return native ? create_native_renderer(target) : create_vulkan_renderer(target);
}

Renderer* create_vulkan_renderer(const VkPresentTarget* target) {
    auto* r = new VkRenderer();
    if (!r->init(target)) { delete r; return nullptr; }
    return r;
}

}  // namespace rsx
