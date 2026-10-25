// Native renderer, render thread: replays the capture side's packets (vkn.h) with Vulkan.
// Surfaces (render targets) are images keyed by guest address; textures are decoded from the
// copies the capture side put in the data ring; vertices are pulled and decoded by the vertex
// shader straight from the ring.
#include "vkn.h"

#include "rsx_fp.h"
#include "rsx_vp.h"
#include "overlay.h"
#include "vk_backend.h"
#include "gfx.h"
#include "overlay_notify.h"
#include "tex_pack.h"

#include <shaderc/shaderc.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace rsx::vkn {
namespace {

[[noreturn]] void vk_fail(const char* what, VkResult r) {
    LOG_ERR("vulkan: %s failed (VkResult %d)", what, int(r));
    std::fflush(stderr);
    std::abort();
}
#define VKC(x) do { const VkResult r_ = (x); if (r_ != VK_SUCCESS) vk_fail(#x, r_); } while (0)

constexpr u32 kFrames = 3;
constexpr VkDeviceSize kStreamSize = 48ull << 20;   // per frame: draw state, texture staging
constexpr VkDeviceSize kRingSize = 256ull << 20;

u64 fnv(const void* p, size_t n, u64 h = 0xcbf29ce484222325ull) {
    const u8* b = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

void warn_once(const std::string& what) {
    static std::map<std::string, bool> seen;
    if (!seen[what]) { seen[what] = true; LOG_ERR("vkn: %s", what.c_str()); }
}

struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; u8* ptr = nullptr; VkDeviceSize size = 0; };

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    u32 w = 0, h = 0, depth = 1, layers = 1, mips = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
};

struct Surface {
    Image img;                       // gw x gh guest pixels, rendered at resolution scale
    u32 gw = 0, gh = 0;
    u32 ea = 0, rsx_format = 0, pitch = 0;
    bool depth = false;
    u64 last_frame = 0;
    std::map<u64, VkImageView> sample_views;
    // feedback copy: what a draw samples when it also renders to this surface (see do_draw)
    Image copy;
    std::map<u64, VkImageView> copy_views;
    u64 write_serial = 0, copy_serial = ~0ull;
};

struct Texture { Image img; TexDesc desc; };

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    Buffer stream;
    VkDeviceSize stream_off = 0;
    std::vector<std::function<void()>> garbage;
    bool pending = false;
    u64 release_pos = 0;     // data ring position the GPU is done with once this frame's fence signals
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
        return ncolor == o.ncolor && depth == o.depth && w == o.w && h == o.h && std::equal(color, color + ncolor, o.color);
    }
};

struct Targets {
    PassState pass;
    Surface* color[4] = {};
    Surface* depth = nullptr;
    u32 clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    u32 aa_x = 1, aa_y = 1;
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

}  // namespace

// ======================================================================================
class RenderThread {
public:
    RenderThread(Shared& sh) : sh_(sh) {}
    bool init(const VkPresentTarget* target);
    void run() { for (;;) { Batch* b = sh_.queue.pop(); process(b); sh_.queue.recycle(b); } }
    void process(Batch* b);

private:
    Shared& sh_;
    u32 S_ = 1;                      // resolution scale: surfaces are S_ times the guest size
    u32 regs_[0x800] = {};
    u32 R(u32 byte) const { return regs_[byte >> 2]; }
    float RF(u32 byte) const { float f; std::memcpy(&f, &regs_[byte >> 2], 4); return f; }

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
    Frame& F() { return frames_[fi_]; }
    VkCommandBuffer cmd() { return frames_[fi_].cmd; }
    Buffer ring_;

    // pipeline objects
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache pcache_ = VK_NULL_HANDLE;
    shaderc_compiler_t compiler_ = nullptr;
    shaderc_compile_options_t copts_ = nullptr;
    // Shaders and pipelines are built on worker threads (async_shaders): a draw whose pipeline is
    // not ready yet is skipped instead of stalling the frame. state: 0 building, 1 ready, 2 failed.
    struct Built { std::atomic<int> state{0}; VkShaderModule module = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; };
    struct Vp { std::vector<u32> ucode; u32 start = 0; VpInfo info; std::shared_ptr<Built> vs; };
    std::unordered_map<u64, Vp> vps_;
    struct Fp { std::vector<u8> bytes; FpInfo info; };
    std::unordered_map<u64, Fp> fps_;
    std::unordered_map<u64, std::shared_ptr<Built>> fs_cache_;
    std::unordered_map<u64, std::shared_ptr<Built>> pipelines_;
    bool async_ = true;
    std::mutex jobs_lock_;
    std::condition_variable jobs_cv_;
    std::deque<std::function<void()>> jobs_;
    void start_workers();
    void run_job(std::function<void()> f);   // on a worker, or right here without async_
    VkPipeline create_pipeline(const PipelineKey& k);
    std::unordered_map<u64, VkSampler> samplers_;
    Image dummy_[3];  // 2D, cube, 3D
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    // caches
    std::unordered_map<u64, std::unique_ptr<Surface>> surfaces_;
    std::unordered_map<u64, Surface*> latest_;
    Surface* bound_surf_[16] = {};   // bind_view: the colour surface each unit samples (nullptr: none)
    u64 bound_mk_[16] = {};
    u64 draw_serial_ = 0;
    void destroy_surface_later(Surface* s);
    VkImageView feedback_view(Surface* s, u64 mk);
    void evict_surfaces();
    std::unordered_map<u32, std::unique_ptr<Texture>> textures_;
    Image display_upload_;

    // pass and dynamic state
    bool in_pass_ = false;
    PassState pass_;
    struct Dyn {
        VkPipeline pipe = VK_NULL_HANDLE;
        VkViewport vp{}; VkRect2D sc{}; float lw = -1;
        int bias_en = -1; float bias[2] = {-1, -1}; u32 bc = ~0u;
        int cull = -1, front = -1, topo = -1, restart = -1, dt = -1, dw = -1, dc = -1, st = -1;
        u32 sops[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        u32 smask[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    } dyn_;

    // dumping
    std::string dump_dir_;
    u32 dump_every_ = 60;
    Buffer readback_;

    // stats
    u64 draws_in_frame_ = 0, pipelines_saved_ = 0, missing_textures_ = 0, missing_surfaces_ = 0, draws_waiting_ = 0, missing_blits_ = 0;
    std::atomic<u64> pipelines_created_{0};
    std::chrono::steady_clock::time_point last_cache_save_ = std::chrono::steady_clock::now();
    void save_pipeline_cache();

    // helpers
    u32 memory_type(u32 bits, VkMemoryPropertyFlags want);
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    Image create_image(VkFormat fmt, u32 w, u32 h, u32 depth, u32 layers, u32 mips, VkImageViewType vt, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkComponentMapping mapping = {});
    void destroy_image_later(Image img);
    VkImageView create_view(const Image& img, VkImageAspectFlags aspect, VkComponentMapping mapping);
    void barrier();
    void begin_frame();
    void submit_frame(VkSemaphore wait, VkSemaphore signal);
    void flush_and_wait();
    void release_ring(u64 pos);
    struct Alloc { VkBuffer buf; VkDeviceSize off; u8* ptr; };
    Alloc stream(VkDeviceSize size, VkDeviceSize align = 16);
    void end_pass();
    void begin_pass(const PassState& p);
    void transition_new(const Image& img);
    bool create_swapchain();
    VkShaderModule compile(const std::string& src, bool vertex, u64 hash);
    Surface* get_surface(u32 ea, bool depth, u32 rsx_fmt, u32 w, u32 h, u32 pitch);
    Targets prepare_targets(const TargetDesc& d);
    VkSampler get_sampler(u32 unit, u32 mips, int zfunc = -1);   // zfunc >= 0: depth compare sampler
    VkPipeline get_pipeline(const PipelineKey& k);
    VkImageView bind_view(u32 unit, const TexBinding& b, TexKind& kind, float scale[2], VkSampler& sampler, u32& depth_rgba);

    void do_texture(const TexUpload& u);
    void do_draw(const DrawPacket& d, const DrawRange2* ranges);
    void do_clear(const ClearPacket& c);
    void do_blit(const BlitParams& b);
    void do_flip(const FlipPacket& f);
    void present(const Image* src, const DisplayBuffer& db, u32 scale);
    void apply_options();

    // post-processing: the shown picture is drawn from the display surface by a full-screen pass that
    // downsamples it (resolution scale) or anti-aliases it (FXAA)
    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    VkShaderModule post_vs_ = VK_NULL_HANDLE, post_fs_ = VK_NULL_HANDLE;
    VkSampler post_sampler_ = VK_NULL_HANDLE;
    std::map<VkFormat, VkPipeline> post_pipes_;
    std::vector<VkImageView> swap_views_;
    Image post_target_;   // headless frame dumps
    Image shot_target_;   // screenshots at 1x with FXAA
    void init_post();
    // Draws the [0,sw)x[0,sh) part of src into `rect` of the target view (in `layout`).
    void post_process(const Image& src, u32 sw, u32 sh, VkImageView dst, VkFormat fmt, VkImageLayout layout, VkRect2D rect, VkExtent2D ext);
};

// ---- device setup -----------------------------------------------------------------------
bool RenderThread::init(const VkPresentTarget* target) {
    target_ = target;
    if (const char* e = std::getenv("TOD_RES_SCALE")) gfx::g.resolution_scale = std::clamp(std::atoi(e), 1, 4);
    if (const char* e = std::getenv("TOD_AA")) gfx::g.aa = std::atoi(e);
    S_ = u32(std::clamp(gfx::g.resolution_scale.load(), 1, 4));
    if (S_ > 1) LOG_NOTE("vkn: rendering at %ux the PS3 resolution", S_);
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
    enable.vertexPipelineStoresAndAtomics = VK_FALSE;
    if (!features_.textureCompressionBC) warn_once("the GPU has no BC texture support; compressed textures will be missing");
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

    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        const auto need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need) d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    // data ring shared with the capture side
    ring_ = create_buffer(kRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    sh_.ring.ptr = ring_.ptr;
    sh_.ring.size = kRingSize;
    sh_.ring.align = std::max<u64>({16, props_.limits.minUniformBufferOffsetAlignment, props_.limits.minStorageBufferOffsetAlignment});

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
        f.stream = create_buffer(kStreamSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }

    std::vector<VkDescriptorSetLayoutBinding> b;
    b.push_back({kVertexConstBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    b.push_back({kFragmentConstBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    b.push_back({kDrawStateBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    b.push_back({kVertexLayoutBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    b.push_back({kVertexDataBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
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
    {   // the driver's pipeline cache, kept on disk between runs
        std::vector<u8> blob;
        if (FILE* f = std::fopen((g_config.data_dir + "/cache/pipelines.bin").c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) { blob.resize(size_t(n)); if (std::fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear(); }
            std::fclose(f);
        }
        VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pcci.initialDataSize = blob.size();
        pcci.pInitialData = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(dev_, &pcci, nullptr, &pcache_) != VK_SUCCESS) {
            pcci.initialDataSize = 0; pcci.pInitialData = nullptr;
            VKC(vkCreatePipelineCache(dev_, &pcci, nullptr, &pcache_));
        }
    }

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
    init_post();
    if (surface_ && !create_swapchain()) return false;
    start_workers();
    return true;
}

u32 RenderThread::memory_type(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < memprops_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want) return i;
    vk_fail("memory_type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

Buffer RenderThread::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
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

VkImageView RenderThread::create_view(const Image& img, VkImageAspectFlags aspect, VkComponentMapping mapping) {
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

Image RenderThread::create_image(VkFormat fmt, u32 w, u32 h, u32 depth, u32 layers, u32 mips, VkImageViewType vt,
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

void RenderThread::destroy_image_later(Image img) {
    F().garbage.push_back([this, img] {
        if (img.view) vkDestroyImageView(dev_, img.view, nullptr);
        if (img.img) vkDestroyImage(dev_, img.img, nullptr);
        if (img.mem) vkFreeMemory(dev_, img.mem, nullptr);
    });
}

void RenderThread::transition_new(const Image& img) {
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

void RenderThread::barrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ---- frames ---------------------------------------------------------------------------------
void RenderThread::release_ring(u64 pos) {
    if (pos <= sh_.ring.tail.load(std::memory_order_relaxed)) return;
    { std::lock_guard lk(sh_.ring.lock); sh_.ring.tail.store(pos, std::memory_order_release); }
    sh_.ring.released.notify_all();
}

void RenderThread::begin_frame() {
    Frame& f = F();
    if (f.pending) {
        VKC(vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX));
        VKC(vkResetFences(dev_, 1, &f.fence));
        f.pending = false;
        release_ring(f.release_pos);
    }
    for (auto& g : f.garbage) g();
    f.garbage.clear();
    f.stream_off = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(f.cmd, 0));
    VKC(vkBeginCommandBuffer(f.cmd, &bi));
    dyn_ = Dyn{};
}

void RenderThread::submit_frame(VkSemaphore wait, VkSemaphore signal) {
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

// Submit what is recorded, wait for it and continue in the same frame slot.
void RenderThread::flush_and_wait() {
    submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
    Frame& f = F();
    VKC(vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(dev_, 1, &f.fence));
    f.pending = false;
    f.stream_off = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(f.cmd, 0));
    VKC(vkBeginCommandBuffer(f.cmd, &bi));
    dyn_ = Dyn{};
}

RenderThread::Alloc RenderThread::stream(VkDeviceSize size, VkDeviceSize align) {
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

// ---- passes ---------------------------------------------------------------------------------
void RenderThread::end_pass() {
    if (!in_pass_) return;
    vkCmdEndRendering(cmd());
    in_pass_ = false;
    barrier();
}

void RenderThread::begin_pass(const PassState& p) {
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

// ---- surfaces -------------------------------------------------------------------------------
Surface* RenderThread::get_surface(u32 ea, bool depth, u32 rsx_fmt, u32 w, u32 h, u32 pitch) {
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
        if (s->gw >= w && s->gh >= h) {
            s->rsx_format = rsx_fmt; s->pitch = pitch; s->last_frame = frame_no_;
            latest_[akey] = s;
            return s;
        }
        w = std::max(w, s->gw); h = std::max(h, s->gh);
        end_pass();
        destroy_surface_later(s);
        if (latest_[akey] == s) latest_.erase(akey);
        surfaces_.erase(it);
    }
    auto s = std::make_unique<Surface>();
    s->ea = ea; s->depth = depth; s->rsx_format = rsx_fmt; s->pitch = pitch; s->last_frame = frame_no_;
    const VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    s->gw = w; s->gh = h;
    s->img = create_image(vf, w * S_, h * S_, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D, usage, aspect);
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
    if (g_log_level >= 1) LOG_NOTE("vkn: new %s surface at 0x%08x %ux%u fmt %u", depth ? "depth" : "color", ea, w, h, rsx_fmt);
    Surface* raw = s.get();
    surfaces_[key] = std::move(s);
    latest_[akey] = raw;
    return raw;
}

void RenderThread::destroy_surface_later(Surface* s) {
    for (auto* views : {&s->sample_views, &s->copy_views})
        for (auto& [k, v] : *views) { VkImageView vv = v; F().garbage.push_back([this, vv] { vkDestroyImageView(dev_, vv, nullptr); }); }
    s->sample_views.clear();
    s->copy_views.clear();
    destroy_image_later(s->img);
    if (s->copy.img) destroy_image_later(s->copy);
    for (auto& b : bound_surf_) if (b == s) b = nullptr;
}

// A view of the surface's feedback copy (see do_draw), brought up to date first.
VkImageView RenderThread::feedback_view(Surface* s, u64 mk) {
    if (!s->copy.img || s->copy.w != s->img.w || s->copy.h != s->img.h) {
        if (s->copy.img) {
            for (auto& [k, v] : s->copy_views) { VkImageView vv = v; F().garbage.push_back([this, vv] { vkDestroyImageView(dev_, vv, nullptr); }); }
            s->copy_views.clear();
            destroy_image_later(s->copy);
        }
        s->copy = create_image(s->img.format, s->img.w, s->img.h, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        transition_new(s->copy);
        s->copy_serial = ~0ull;
        static bool logged = false;
        if (!logged) { logged = true; LOG_NOTE("vkn: a draw samples its own render target (0x%08x); using a copy", s->ea); }
    }
    if (s->copy_serial != s->write_serial) {
        end_pass();   // ends with a barrier: the draws so far are visible to the copy
        VkImageCopy r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.dstSubresource = r.srcSubresource;
        r.extent = {s->img.w, s->img.h, 1};
        vkCmdCopyImage(cmd(), s->img.img, VK_IMAGE_LAYOUT_GENERAL, s->copy.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        barrier();
        s->copy_serial = s->write_serial;
    }
    auto it = s->copy_views.find(mk);
    if (it == s->copy_views.end()) {
        const VkComponentMapping m{VkComponentSwizzle(mk & 0xff), VkComponentSwizzle((mk >> 8) & 0xff), VkComponentSwizzle((mk >> 16) & 0xff),
                                   VkComponentSwizzle((mk >> 24) & 0xff)};
        it = s->copy_views.emplace(mk, create_view(s->copy, VK_IMAGE_ASPECT_COLOR_BIT, m)).first;
    }
    return it->second;
}

// Surfaces not drawn to or sampled for a long while are freed (per-object shadow maps get a new
// surface for every size and place they are drawn at, so without this they pile up over a session).
void RenderThread::evict_surfaces() {
    // Some effects render to a new small surface at a new address every frame (Jacky's log: 13,700
    // surfaces after a few minutes with every weapon, then out of GPU memory): with that many, the
    // ones unused for 2 seconds go, checked every half second.
    const bool crowded = surfaces_.size() > 512;
    const u64 idle = crowded ? 120 : 3600;
    if (frame_no_ % (crowded ? 30 : 600) != 0) return;
    size_t n = 0;
    for (auto it = surfaces_.begin(); it != surfaces_.end();) {
        Surface* s = it->second.get();
        if (frame_no_ - s->last_frame < idle) { ++it; continue; }
        end_pass();
        const u64 akey = (u64(s->ea) << 1) | (s->depth ? 1 : 0);
        if (auto l = latest_.find(akey); l != latest_.end() && l->second == s) latest_.erase(l);
        destroy_surface_later(s);
        it = surfaces_.erase(it);
        ++n;
    }
    if (n && g_log_level >= 1) LOG_NOTE("vkn: freed %zu surfaces unused for %llu frames (%zu left)", n, (unsigned long long)idle, surfaces_.size());
}

Targets RenderThread::prepare_targets(const TargetDesc& d) {
    Targets t;
    t.clip_x = d.clip_x; t.clip_y = d.clip_y; t.clip_w = d.clip_w; t.clip_h = d.clip_h;
    t.aa_x = d.aa_x; t.aa_y = d.aa_y;
    if (!d.valid) return t;
    for (u32 i = 0; i < d.ncolor; ++i) {
        Surface* s = get_surface(d.color_ea[i], false, d.color_fmt, d.w, d.h, d.color_pitch[i]);
        t.color[t.pass.ncolor] = s;
        t.pass.color[t.pass.ncolor] = s->img.view;
        t.pass.color_fmt[t.pass.ncolor] = s->img.format;
        ++t.pass.ncolor;
    }
    if (d.has_depth) {
        Surface* s = get_surface(d.zea, true, d.depth_fmt, d.w, d.h, d.zpitch);
        t.depth = s;
        t.pass.depth = s->img.view;
        t.pass.depth_fmt = s->img.format;
        t.pass.stencil = d.depth_fmt != 1;
    }
    t.pass.w = ~0u; t.pass.h = ~0u;
    for (u32 i = 0; i < t.pass.ncolor; ++i) { t.pass.w = std::min(t.pass.w, t.color[i]->img.w); t.pass.h = std::min(t.pass.h, t.color[i]->img.h); }
    if (t.depth) { t.pass.w = std::min(t.pass.w, t.depth->img.w); t.pass.h = std::min(t.pass.h, t.depth->img.h); }
    t.valid = true;
    return t;
}

// ---- clear / blit -------------------------------------------------------------------------------
void RenderThread::do_clear(const ClearPacket& c) {
    Targets t = prepare_targets(c.targets);
    if (!t.valid) return;
    ++draw_serial_;
    for (u32 j = 0; j < t.pass.ncolor; ++j) t.color[j]->write_serial = draw_serial_;
    begin_pass(t.pass);
    const u32 sx = R(0x8c0) & 0xffff, sw = R(0x8c0) >> 16, sy = R(0x8c4) & 0xffff, sh = R(0x8c4) >> 16;
    const u32 x0 = std::max(sx, t.clip_x) * t.aa_x, y0 = std::max(sy, t.clip_y) * t.aa_y;
    const u32 x1 = std::min({(sx + sw) * t.aa_x, (t.clip_x + t.clip_w) * t.aa_x, t.pass.w / S_}),
              y1 = std::min({(sy + sh) * t.aa_y, (t.clip_y + t.clip_h) * t.aa_y, t.pass.h / S_});
    if (x1 <= x0 || y1 <= y0) return;
    VkClearRect rect{{{s32(x0 * S_), s32(y0 * S_)}, {(x1 - x0) * S_, (y1 - y0) * S_}}, 0, 1};
    VkClearAttachment atts[5];
    u32 n = 0;
    const u32 mask = c.mask;
    if (mask & 0xf0) {
        if ((mask & 0xf0) != 0xf0) warn_once("partial color-channel clears are treated as full clears");
        const u32 cc = R(0x1d90);
        VkClearValue v{};
        v.color.float32[0] = float((cc >> 16) & 0xff) / 255.f;
        v.color.float32[1] = float((cc >> 8) & 0xff) / 255.f;
        v.color.float32[2] = float(cc & 0xff) / 255.f;
        v.color.float32[3] = float(cc >> 24) / 255.f;
        for (u32 i = 0; i < t.pass.ncolor; ++i) atts[n++] = {VK_IMAGE_ASPECT_COLOR_BIT, i, v};
    }
    if (t.depth && (mask & 3)) {
        const u32 z = R(0x1d8c);
        VkClearValue v{};
        VkImageAspectFlags aspect = 0;
        if (mask & 1) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((mask & 2) && t.pass.stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (t.depth->rsx_format == 1) v.depthStencil.depth = float(z & 0xffff) / 65535.f;
        else { v.depthStencil.depth = float(z >> 8) / 16777215.f; v.depthStencil.stencil = z & 0xff; }
        if (aspect) atts[n++] = {aspect, 0, v};
    }
    if (n) vkCmdClearAttachments(cmd(), n, atts, 1, &rect);
}

void RenderThread::do_blit(const BlitParams& b) {
    // the surface holding the source: of several, the one drawn to last (the first in latest_ order
    // could be an older one whose memory a newer surface reuses, and it changed when the map rehashed)
    Surface* src = nullptr;
    for (auto& [k, v] : latest_) {
        Surface* s = v;
        if (s->depth || !s->pitch) continue;
        if (b.src_ea >= s->ea && b.src_ea < s->ea + s->pitch * s->gh && (!src || s->write_serial > src->write_serial)) src = s;
    }
    if (!src) { ++missing_blits_; return; }
    src->last_frame = frame_no_;
    const u32 bpp = src->img.format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 2;
    const u32 off = b.src_ea - src->ea;
    const float ox = float((off % src->pitch) / bpp), oy = float(off / src->pitch);
    Surface* dst = get_surface(b.dst_ea, false, b.dst_fmt == 4 ? 3 : 8, b.dx + b.dw, b.dy + b.dh, b.dst_pitch);
    if (dst == src) { warn_once("scaled image copy within one surface"); return; }
    dst->write_serial = ++draw_serial_;
    end_pass();
    VkImageBlit r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = r.srcSubresource;
    const float S = float(S_);
    auto clampx = [&](float v) { return s32(std::clamp(v * S, 0.f, float(src->img.w))); };
    auto clampy = [&](float v) { return s32(std::clamp(v * S, 0.f, float(src->img.h))); };
    r.srcOffsets[0] = {clampx(ox + b.sx), clampy(oy + b.sy), 0};
    r.srcOffsets[1] = {clampx(ox + b.sx + b.sw), clampy(oy + b.sy + b.sh), 1};
    r.dstOffsets[0] = {s32(b.dx * S_), s32(b.dy * S_), 0};
    r.dstOffsets[1] = {s32(std::min(b.dx + b.dw, dst->gw) * S_), s32(std::min(b.dy + b.dh, dst->gh) * S_), 1};
    if (r.srcOffsets[1].x <= r.srcOffsets[0].x || r.srcOffsets[1].y <= r.srcOffsets[0].y) return;
    vkCmdBlitImage(cmd(), src->img.img, VK_IMAGE_LAYOUT_GENERAL, dst->img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r,
                   b.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    barrier();
}

// ---- shaders and pipelines ------------------------------------------------------------------
VkShaderModule RenderThread::compile(const std::string& src, bool vertex, u64 hash) {
    char name[64];
    std::snprintf(name, sizeof name, "%s_%016llx", vertex ? "vpn" : "fp", (unsigned long long)hash);
    // SPIR-V cache on disk, keyed by the GLSL text: compiling GLSL is the slow part of a new shader
    static const std::string cache_dir = [] {
        std::string d = g_config.data_dir + "/cache/spirv";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    char cname[48];
    std::snprintf(cname, sizeof cname, "/%016llx.spv", (unsigned long long)xxh64(src.data(), src.size(), vertex ? 1 : 2));
    const std::string cpath = cache_dir + cname;
    if (FILE* f = std::fopen(cpath.c_str(), "rb")) {
        std::vector<u32> code;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 20 && n % 4 == 0) {
            code.resize(size_t(n) / 4);
            if (std::fread(code.data(), 1, size_t(n), f) != size_t(n) || code[0] != 0x07230203u) code.clear();
        }
        std::fclose(f);
        if (!code.empty()) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = code.size() * 4;
            ci.pCode = code.data();
            VkShaderModule m;
            if (vkCreateShaderModule(dev_, &ci, nullptr, &m) == VK_SUCCESS) return m;
        }
    }
    shaderc_compilation_result_t res = shaderc_compile_into_spv(compiler_, src.data(), src.size(),
                                                                vertex ? shaderc_vertex_shader : shaderc_fragment_shader, name, "main", copts_);
    if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
        LOG_ERR("vkn: %s failed to compile: %s", name, shaderc_result_get_error_message(res));
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
    {   // write to a temporary name and rename, so a crash never leaves a truncated file
        const std::string tmp = cpath + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            const bool ok = std::fwrite(ci.pCode, 1, ci.codeSize, f) == ci.codeSize;
            std::fclose(f);
            std::error_code ec;
            if (ok) std::filesystem::rename(tmp, cpath, ec);
            else std::filesystem::remove(tmp, ec);
        }
    }
    shaderc_result_release(res);
    if (std::getenv("TOD_DUMP_GLSL")) {
        const std::string dir = g_config.data_dir + "/shaders";
        std::filesystem::create_directories(dir);
        if (FILE* f = std::fopen((dir + "/" + name + (vertex ? ".vert" : ".frag")).c_str(), "w")) { std::fputs(src.c_str(), f); std::fclose(f); }
    }
    return m;
}

void RenderThread::start_workers() {
    const char* e = std::getenv("TOD_ASYNC_SHADERS");
    async_ = e ? std::atoi(e) != 0 : g_config.async_shaders;
    if (std::getenv("TOD_DUMP_FRAMES")) async_ = e && std::atoi(e) != 0;   // frame dumps should show every draw
    if (!async_) return;
    const u32 n = std::clamp<u32>(std::thread::hardware_concurrency() / 2, 1, 4);
    for (u32 i = 0; i < n; ++i)
        std::thread([this] {
            for (;;) {
                std::function<void()> f;
                {
                    std::unique_lock lk(jobs_lock_);
                    jobs_cv_.wait(lk, [&] { return !jobs_.empty(); });
                    f = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                f();
            }
        }).detach();
    LOG_NOTE("vkn: building shaders on %u background threads", n);
}

void RenderThread::run_job(std::function<void()> f) {
    if (!async_) { f(); return; }
    { std::lock_guard lk(jobs_lock_); jobs_.push_back(std::move(f)); }
    jobs_cv_.notify_one();
}

VkPipeline RenderThread::get_pipeline(const PipelineKey& k) {
    const u64 h = fnv(&k, sizeof k);
    auto& b = pipelines_[h];
    if (!b) {
        b = std::make_shared<Built>();
        run_job([this, k, b] {
            b->pipe = create_pipeline(k);
            b->state.store(b->pipe ? 1 : 2, std::memory_order_release);
        });
    }
    return b->state.load(std::memory_order_acquire) == 1 ? b->pipe : VK_NULL_HANDLE;
}

VkPipeline RenderThread::create_pipeline(const PipelineKey& k) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = k.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = k.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
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
    if (r != VK_SUCCESS) { LOG_ERR("vkn: pipeline creation failed (%d)", int(r)); p = VK_NULL_HANDLE; }
    ++pipelines_created_;
    return p;
}

// ---- textures ---------------------------------------------------------------------------------
VkSampler RenderThread::get_sampler(u32 unit, u32 mips, int zfunc) {
    const u32 base = 0x1a00 + 0x20 * unit;
    const u32 addr = R(base + 0x08), ctl0 = R(base + 0x0c), filter = R(base + 0x14), border = R(base + 0x1c);
    const u32 min_aniso = zfunc < 0 && mips > 1 ? u32(gfx::g.anisotropy.load(std::memory_order_relaxed)) : 0u;
    const u64 key = fnv(&addr, 4, fnv(&ctl0, 4, fnv(&filter, 4, fnv(&border, 4, mips + (u64(zfunc + 1) << 32) + (u64(min_aniso) << 40)))));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const u32 mag = (filter >> 24) & 0xf, min = (filter >> 16) & 0xff;
    s.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    s.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    s.mipmapMode = (min == 5 || min == 6) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = address_mode(addr & 0xf);
    s.addressModeV = address_mode((addr >> 8) & 0xf);
    s.addressModeW = address_mode((addr >> 16) & 0xf);
    s.mipLodBias = float(s32(filter << 19) >> 19) / 256.f;
    s.minLod = float((ctl0 >> 19) & 0xfff) / 256.f;
    s.maxLod = (min == 1 || min == 2 || mips <= 1) ? 0.25f : float((ctl0 >> 7) & 0xfff) / 256.f;
    if (s.maxLod < s.minLod) s.maxLod = s.minLod;
    const u32 aniso = (ctl0 >> 4) & 7;
    static const float levels[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    // the anisotropic_filtering option raises it on filtered, mipmapped textures
    const float want = std::max(aniso ? levels[aniso] : 1.f, s.minFilter == VK_FILTER_LINEAR && s.magFilter == VK_FILTER_LINEAR ? float(min_aniso) : 1.f);
    if (want > 1.f && features_.samplerAnisotropy) {
        s.anisotropyEnable = VK_TRUE;
        s.maxAnisotropy = std::min(want, props_.limits.maxSamplerAnisotropy);
    }
    s.borderColor = (border >> 24) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (zfunc >= 0) {
        // The RSX compares texel OP reference; Vulkan compares reference OP texel, so the
        // ordered functions swap direction (as in RPCS3). Texels outside a clamped-to-border
        // map read as the far plane, which leaves them unshadowed.
        static const VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                           VK_COMPARE_OP_LESS, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
        s.compareEnable = VK_TRUE;
        static const bool noswap = std::getenv("TOD_ZCMP_NOSWAP") != nullptr;
        s.compareOp = noswap ? VkCompareOp(zfunc & 7) : ops[zfunc & 7];
        s.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        s.anisotropyEnable = VK_FALSE;
        s.maxAnisotropy = 1.f;
    }
    VkSampler smp;
    VKC(vkCreateSampler(dev_, &s, nullptr, &smp));
    samplers_[key] = smp;
    return smp;
}

void RenderThread::do_texture(const TexUpload& u) {
    if (auto it = textures_.find(u.id); it != textures_.end()) { destroy_image_later(it->second->img); textures_.erase(it); }
    static const bool check = std::getenv("TOD_VKN_CHECK") != nullptr;
    if (check && xxh64(sh_.ring.ptr + u.ring_off, u.size) != u.hash)
        LOG_ERR("vkn check: texture %u at 0x%08x changed in the ring before it was decoded", u.id, u.desc.ea);
    DecodedTex dec;
    if (!decode_texture_from(u.desc, sh_.ring.ptr + u.ring_off, dec) || dec.levels.empty()) {
        char w[96];
        std::snprintf(w, sizeof w, "texture format reg 0x%08x (%ux%u) not decoded", u.desc.format, u.desc.width, u.desc.height);
        warn_once(w);
        return;
    }
    // texture packs: <data>/textures/<hash>.png replaces this texture, texture_dump saves it
    static const bool pack_ready = (texpack::init(g_config.data_dir), true);
    (void)pack_ready;
    if (gfx::g.texture_dump) texpack::dump(u.hash, dec);
    if (dec.layers == 1 && dec.view_type == VK_IMAGE_VIEW_TYPE_2D && texpack::replacement_count()) {
        std::vector<u8> px;
        u32 rw, rh, rm;
        if (texpack::replacement(u.hash, px, rw, rh, rm)) {
            DecodedTex r;
            r.format = VK_FORMAT_R8G8B8A8_UNORM;
            // the PNG is what shaders see, except channels the game forces to 0 or 1
            auto keep = [](VkComponentSwizzle c) {
                return c == VK_COMPONENT_SWIZZLE_ZERO || c == VK_COMPONENT_SWIZZLE_ONE ? c : VK_COMPONENT_SWIZZLE_IDENTITY;
            };
            r.mapping = {keep(dec.mapping.r), keep(dec.mapping.g), keep(dec.mapping.b), keep(dec.mapping.a)};
            r.data = std::move(px);
            u32 o = 0;
            for (u32 m = 0; m < rm; ++m) {
                const u32 mw = std::max<u32>(1, rw >> m), mh = std::max<u32>(1, rh >> m);
                r.levels.push_back({o, mw, mh, 1, mw, mw * mh * 4});
                o += mw * mh * 4;
            }
            dec = std::move(r);
        }
    }
    auto t = std::make_unique<Texture>();
    t->desc = u.desc;
    if (dec.view_type == VK_IMAGE_VIEW_TYPE_1D) dec.view_type = VK_IMAGE_VIEW_TYPE_2D;
    const u32 mips = u32(dec.levels.size()) / dec.layers;
    t->img = create_image(dec.format, dec.levels[0].width, dec.levels[0].height, dec.view_type == VK_IMAGE_VIEW_TYPE_3D ? dec.levels[0].depth : 1,
                          dec.layers, mips, dec.view_type, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, dec.mapping);
    transition_new(t->img);
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
    vkCmdCopyBufferToImage(cmd(), a.buf, t->img.img, VK_IMAGE_LAYOUT_GENERAL, u32(regions.size()), regions.data());
    barrier();
    textures_[u.id] = std::move(t);
}

VkImageView RenderThread::bind_view(u32 unit, const TexBinding& b, TexKind& kind, float scale[2], VkSampler& sampler, u32& depth_rgba) {
    kind = TexKind(b.view);
    depth_rgba = 0;
    scale[0] = scale[1] = 1.f;
    bound_surf_[unit] = nullptr;
    sampler = dummy_sampler_;
    VkImageView dummy = kind == TexKind::Cube ? dummy_[1].view : kind == TexKind::Tex3D ? dummy_[2].view : dummy_[0].view;
    const u32 base = 0x1a00 + 0x20 * unit;
    const u32 fmt = R(base + 0x04), rect = R(base + 0x18), remap = R(base + 0x10);
    const u32 tw = rect >> 16, th = rect & 0xffff;
    const bool unnorm = fmt & 0x4000;
    if (unnorm) { scale[0] = 1.f / std::max<u32>(1, tw); scale[1] = 1.f / std::max<u32>(1, th); }
    switch (b.kind) {
    case TB_TEXTURE: {
        auto it = textures_.find(b.id_or_ea);
        if (it == textures_.end()) { ++missing_textures_; return dummy; }
        sampler = get_sampler(unit, it->second->img.mips);
        return it->second->img.view;
    }
    case TB_COLOR_SURFACE: case TB_DEPTH_SURFACE: {
        const bool depth = b.kind == TB_DEPTH_SURFACE;
        auto it = latest_.find((u64(b.id_or_ea) << 1) | (depth ? 1 : 0));
        if (it == latest_.end()) { ++missing_surfaces_; return dummy; }
        Surface* s = it->second;
        s->last_frame = frame_no_;
        // A depth texture format with a depth function (ADDRESS bits 28-31) between NEVER and
        // ALWAYS samples as a shadow map: the RSX compares and filters (2x2 PCF when linear).
        const u32 zfunc = R(base + 0x08) >> 28;
        const bool shadow = depth && is_depth_format(tex_base_format(fmt)) && zfunc > 0 && zfunc < 7;
        if (shadow) kind = TexKind::Shadow2D;
        // D24S8 depth read through an 8-bit ARGB format: the shader rebuilds the bytes from the depth value
        const u32 bf = tex_base_format(fmt);
        if (depth && s->rsx_format == 2 && (bf == TEX_A8R8G8B8 || bf == TEX_D8R8G8B8))
            depth_rgba = 0x10000u | (bf == TEX_D8R8G8B8 ? 0x20000u : 0u) | (remap & 0xffff);
        const VkComponentMapping m = shadow ? VkComponentMapping{}
                                   : depth ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R}
                                           : surface_sample_mapping(fmt, remap);
        const u64 mk = u64(m.r) | u64(m.g) << 8 | u64(m.b) << 16 | u64(m.a) << 24;
        auto vit = s->sample_views.find(mk);
        if (vit == s->sample_views.end())
            vit = s->sample_views.emplace(mk, create_view(s->img, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, m)).first;
        if (!depth) { bound_surf_[unit] = s; bound_mk_[unit] = mk; }
        if (unnorm) { scale[0] = 1.f / s->gw; scale[1] = 1.f / s->gh; }
        else if (s->gw != tw || s->gh != th) { scale[0] = float(tw) / s->gw; scale[1] = float(th) / s->gh; }
        sampler = get_sampler(unit, 1, shadow ? int(zfunc) : -1);
        static const bool strace = std::getenv("TOD_SAMPLE_TRACE") != nullptr;
        if (strace && shadow) {
            char w[200];
            std::snprintf(w, sizeof w, "shadow sample ea 0x%08x surf %ux%u tex %ux%u unnorm %d zfunc %u", b.id_or_ea, s->gw, s->gh, tw, th, int(unnorm), zfunc);
            warn_once(w);
        }
        return vit->second;
    }
    default: return dummy;
    }
}

// ---- draw ---------------------------------------------------------------------------------------
void RenderThread::do_draw(const DrawPacket& d, const DrawRange2* ranges) {
    Targets t = prepare_targets(d.targets);
    if (!t.valid) return;

    // --- programs
    auto vit = vps_.find(d.vp_hash);
    if (vit == vps_.end()) return;
    Vp& vp = vit->second;
    if (!vp.vs) {
        auto b = vp.vs = std::make_shared<Built>();
        run_job([this, b, ucode = vp.ucode, start = vp.start, info = vp.info] {
            b->module = compile(vp_to_glsl(ucode.data(), start, info, true), true, info.hash);
            b->state.store(b->module ? 1 : 2, std::memory_order_release);
        });
    }
    auto fit = fps_.find(d.fp_key);
    if (fit == fps_.end()) return;
    const Fp& fp = fit->second;
    FpKey fkey;
    fkey.shader_control = R(0x1d60);
    TexKind kinds[16];
    float tscale[16][2];
    VkImageView views[16];
    VkSampler smps[16];
    for (u32 i = 0; i < 16; ++i) {
        if (fp.info.textures_used & (1u << i)) views[i] = bind_view(i, d.tex[i], kinds[i], tscale[i], smps[i], fkey.depth_rgba[i]);
        else { kinds[i] = TexKind::Tex2D; tscale[i][0] = tscale[i][1] = 1.f; views[i] = VK_NULL_HANDLE; smps[i] = VK_NULL_HANDLE; }
        fkey.tex_kind[i] = kinds[i];
        static const bool strace = std::getenv("TOD_SAMPLE_TRACE") != nullptr;
        if (strace && (fp.info.textures_used & (1u << i)) && d.tex[i].kind != TB_TEXTURE) {
            char w[200];
            std::snprintf(w, sizeof w, "sample unit %u kind %u ea 0x%08x fmt 0x%08x addr 0x%08x ctl0 0x%08x filt 0x%08x rect 0x%08x remap 0x%04x", i, d.tex[i].kind,
                          d.tex[i].id_or_ea, R(0x1a04 + 0x20 * i), R(0x1a08 + 0x20 * i), R(0x1a0c + 0x20 * i), R(0x1a14 + 0x20 * i), R(0x1a18 + 0x20 * i), R(0x1a10 + 0x20 * i) & 0xffff);
            warn_once(w);
        }
        if ((fp.info.textures_used & (1u << i)) && !fkey.depth_rgba[i]) {
            fkey.tex_gamma[i] = fp_texture_gamma(R(0x1a04 + 0x20 * i), R(0x1a08 + 0x20 * i));
            fkey.tex_expand[i] = fp_texture_expand(R(0x1a04 + 0x20 * i), R(0x1a08 + 0x20 * i), R(0x1a10 + 0x20 * i));
        }
    }
    fkey.srgb_output = fp_srgb_output(R(0x1fec), R(0x208), fkey.shader_control);
    if (gfx::g.soft_shadows.load(std::memory_order_relaxed))
        for (u32 i = 0; i < 16; ++i) if (kinds[i] == TexKind::Shadow2D) fkey.shadow_soft = 1;
    const u64 fhash = fnv(&fkey, sizeof fkey, fp.info.hash);
    auto& fsb = fs_cache_[fhash];
    if (!fsb) {
        auto b = fsb = std::make_shared<Built>();
        run_job([this, b, bytes = fp.bytes, info = fp.info, fkey, fhash] {
            b->module = compile(fp_to_glsl(bytes.data(), info, fkey), false, fhash);
            b->state.store(b->module ? 1 : 2, std::memory_order_release);
        });
    }
    const int vstate = vp.vs->state.load(std::memory_order_acquire), fstate = fsb->state.load(std::memory_order_acquire);
    if (vstate == 2 || fstate == 2) return;
    if (vstate == 0 || fstate == 0) { ++draws_waiting_; return; }
    const VkShaderModule fs = fsb->module;

    // --- draw state
    const VkDeviceSize ua = std::max<VkDeviceSize>(16, props_.limits.minUniformBufferOffsetAlignment);
    Alloc dsa = stream(sizeof(DrawState), ua);
    DrawState& ds = *reinterpret_cast<DrawState*>(dsa.ptr);
    std::memset(&ds, 0, sizeof ds);
    {
        const float cw = float(t.clip_w), ch = float(t.clip_h);
        const float sx = RF(0xa30) / (cw / 2.f), ox = (RF(0xa20) - cw / 2.f) / (cw / 2.f);
        const float sy = RF(0xa34) / (ch / 2.f), oy = (RF(0xa24) - ch / 2.f) / (ch / 2.f);
        const float m[16] = {sx, 0, 0, ox, 0, sy, 0, oy, 0, 0, RF(0xa38), RF(0xa3c), 0, 0, 0, 1};
        std::memcpy(ds.scale_offset, m, sizeof m);
    }
    ds.alpha_func = (R(0x304) & 1) ? R(0x308) : 0;
    ds.alpha_ref = float(R(0x30c) & 0xff) / 255.f;
    ds.two_sided_lighting = R(0x142c) & 1;
    {
        const u32 aa = R(0x1d7c);
        const bool a2c = (aa & 1) && (aa & 0x10);
        ds.alpha_to_coverage = a2c ? t.aa_x * t.aa_y : 0;
        ds.alpha_to_one = a2c && (aa & 0x100);
    }
    ds.transform_branch_bits = R(0x1ff8);
    for (u32 i = 0; i < 16; ++i) { ds.tex_scale[i][0] = tscale[i][0]; ds.tex_scale[i][1] = tscale[i][1]; ds.tex_scale[i][2] = ds.tex_scale[i][3] = 1.f; }
    {
        const u32 sw = R(0x1d88);
        const bool bottom = (sw >> 12) & 1;
        ds.wpos_scale = bottom ? -1.f : 1.f;
        ds.wpos_bias = bottom ? float(sw & 0xfff) : 0.f;
    }
    ds.fog_param0 = RF(0x8d0);
    ds.fog_param1 = RF(0x8d4);
    ds.fog_mode = R(0x8cc);
    ds.shader_control = R(0x1d60);
    ds.res_inv = 1.f / float(S_);
    // A multisampled surface is drawn here as a supersampled one (each sample its own pixel), but
    // on the RSX the shader runs once per pixel: WPOS is in pixels of the unexpanded surface.
    static const bool wpos_raw = std::getenv("TOD_WPOS_RAW") != nullptr;   // A/B testing
    ds.wpos_ax = wpos_raw ? 1.f : 1.f / float(t.aa_x);
    ds.wpos_ay = wpos_raw ? 1.f : 1.f / float(t.aa_y);

    // --- pipeline
    const Prim prim = Prim(d.prim);
    VkPrimitiveTopology topo;
    u32 topo_class;
    switch (prim) {
    case Prim::Points: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; topo_class = 0; break;
    case Prim::Lines: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; topo_class = 1; break;
    case Prim::LineLoop: case Prim::LineStrip: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; topo_class = 1; break;
    case Prim::TriangleStrip: case Prim::QuadStrip: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; topo_class = 2; break;
    default: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; topo_class = 2; break;
    }
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vs = vp.vs->module; k.fs = fs; k.topology_class = topo_class;
    k.ncolor = t.pass.ncolor;
    for (u32 i = 0; i < k.ncolor; ++i) {
        k.color[i] = t.pass.color_fmt[i];
        auto& b = k.blend[i];
        b.enable = i == 0 ? (R(0x310) & 1) : ((R(0x36c) >> i) & 1);
        b.src_rgb = R(0x314) & 0xffff; b.src_a = R(0x314) >> 16;
        b.dst_rgb = R(0x318) & 0xffff; b.dst_a = R(0x318) >> 16;
        b.eq_rgb = R(0x320) & 0xffff; b.eq_a = R(0x320) >> 16;
        if (i == 0) {
            const u32 cm = R(0x324);
            b.mask = ((cm >> 16) & 0xff ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm >> 8) & 0xff ? VK_COLOR_COMPONENT_G_BIT : 0) |
                     (cm & 0xff ? VK_COLOR_COMPONENT_B_BIT : 0) | ((cm >> 24) & 0xff ? VK_COLOR_COMPONENT_A_BIT : 0);
        } else {
            const u32 cm = R(0x370) >> (4 * i);
            b.mask = (cm & 2 ? VK_COLOR_COMPONENT_R_BIT : 0) | (cm & 4 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                     (cm & 8 ? VK_COLOR_COMPONENT_B_BIT : 0) | (cm & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
        }
        if (!b.enable) { b.src_rgb = b.src_a = b.dst_rgb = b.dst_a = b.eq_rgb = b.eq_a = 0; }
    }
    k.depth = t.pass.depth ? t.pass.depth_fmt : VK_FORMAT_UNDEFINED;
    k.has_stencil = t.pass.depth && t.pass.stencil;
    VkPipeline pipe = get_pipeline(k);
    if (!pipe) { if (pipelines_[fnv(&k, sizeof k)]->state.load(std::memory_order_acquire) == 0) ++draws_waiting_; return; }

    // --- feedback: the draw samples a surface it also renders to (the Zoni's shimmer, full-screen
    // effects that read the frame they draw over). The RSX reads what is in memory; in Vulkan that is
    // undefined (black on some GPUs). Sample a copy taken before the draw, made again only when the
    // surface has been drawn to since.
    static const bool no_feedback_copy = std::getenv("TOD_NO_FEEDBACK_COPY") != nullptr;
    if (!no_feedback_copy)
        for (u32 i = 0; i < 16; ++i) {
            if (!views[i] || !bound_surf_[i]) continue;
            for (u32 j = 0; j < t.pass.ncolor; ++j)
                if (t.color[j] == bound_surf_[i]) { views[i] = feedback_view(bound_surf_[i], bound_mk_[i]); break; }
        }
    ++draw_serial_;
    for (u32 j = 0; j < t.pass.ncolor; ++j) t.color[j]->write_serial = draw_serial_;

    // --- record
    begin_pass(t.pass);
    VkCommandBuffer cb = cmd();
    if (dyn_.pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); dyn_.pipe = pipe; }
    VkDescriptorBufferInfo bi[5] = {
        {ring_.buf, d.vc_off, sizeof(float) * 4 * kVpConstants},
        {ring_.buf, d.fc_off, d.fc_size},
        {dsa.buf, dsa.off, sizeof(DrawState)},
        {ring_.buf, d.layout_off, sizeof(VertexLayout)},
        {ring_.buf, d.vdata_off, d.vdata_size},
    };
    VkDescriptorImageInfo ii[20];
    VkWriteDescriptorSet w[25];
    u32 nw = 0;
    for (u32 i = 0; i < 5; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = i; w[nw].descriptorCount = 1;
        w[nw].descriptorType = i == kVertexDataBinding ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[nw].pBufferInfo = &bi[i];
        ++nw;
    }
    for (u32 i = 0; i < 16; ++i) {
        if (!views[i]) continue;
        ii[i] = {smps[i], views[i], VK_IMAGE_LAYOUT_GENERAL};
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = kTextureBindingBase + i;
        w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i];
        ++nw;
    }
    for (u32 i = 0; i < 4; ++i) {
        if (!(vp.info.vertex_textures & (1u << i))) continue;
        ii[16 + i] = {dummy_sampler_, dummy_[0].view, VK_IMAGE_LAYOUT_GENERAL};
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = kVertexTextureBindingBase + i;
        w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[16 + i];
        ++nw;
    }
    push_descriptors_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, nw, w);

    const VkViewport vpt{0.f, 0.f, float(t.clip_w * t.aa_x * S_), float(t.clip_h * t.aa_y * S_), 0.f, 1.f};
    if (std::memcmp(&vpt, &dyn_.vp, sizeof vpt)) { vkCmdSetViewport(cb, 0, 1, &vpt); dyn_.vp = vpt; }
    {
        const u32 sx = (R(0x8c0) & 0xffff) * t.aa_x, sw = (R(0x8c0) >> 16) * t.aa_x;
        const u32 sy = (R(0x8c4) & 0xffff) * t.aa_y, sh = (R(0x8c4) >> 16) * t.aa_y;
        const u32 pw = t.pass.w / S_, ph = t.pass.h / S_;
        const u32 x0 = std::min(sx, pw), y0 = std::min(sy, ph);
        const u32 x1 = std::min({sx + sw, (t.clip_x + t.clip_w) * t.aa_x, pw}),
                  y1 = std::min({sy + sh, (t.clip_y + t.clip_h) * t.aa_y, ph});
        const VkRect2D sc{{s32(x0 * S_), s32(y0 * S_)}, {x1 > x0 ? (x1 - x0) * S_ : 0, y1 > y0 ? (y1 - y0) * S_ : 0}};
        if (std::memcmp(&sc, &dyn_.sc, sizeof sc)) { vkCmdSetScissor(cb, 0, 1, &sc); dyn_.sc = sc; }
    }
    {
        const float lw = features_.wideLines ? std::min(8.f * S_, std::max(1.f, float(R(0x3b8)) / 8.f) * S_) : 1.f;
        if (lw != dyn_.lw) { vkCmdSetLineWidth(cb, lw); dyn_.lw = lw; }
    }
    const int bias_en = (R(0xa68) & 1) && t.pass.depth;
    if (bias_en != dyn_.bias_en) { vkCmdSetDepthBiasEnable(cb, bias_en); dyn_.bias_en = bias_en; }
    if (bias_en && (RF(0xa7c) != dyn_.bias[0] || RF(0xa78) != dyn_.bias[1])) {
        vkCmdSetDepthBias(cb, RF(0xa7c), 0.f, RF(0xa78));
        dyn_.bias[0] = RF(0xa7c); dyn_.bias[1] = RF(0xa78);
    }
    if (R(0x31c) != dyn_.bc) {
        const u32 bc = R(0x31c);
        const float c[4] = {float((bc >> 16) & 0xff) / 255.f, float((bc >> 8) & 0xff) / 255.f, float(bc & 0xff) / 255.f, float(bc >> 24) / 255.f};
        vkCmdSetBlendConstants(cb, c);
        dyn_.bc = bc;
    }
    int cull = VK_CULL_MODE_NONE;
    if (R(0x183c) & 1) {
        switch (R(0x1830)) {
        case 0x404: cull = VK_CULL_MODE_FRONT_BIT; break;
        case 0x405: cull = VK_CULL_MODE_BACK_BIT; break;
        case 0x408: cull = VK_CULL_MODE_FRONT_AND_BACK; break;
        default: break;
        }
    }
    if (cull != dyn_.cull) { vkCmdSetCullMode(cb, VkCullModeFlags(cull)); dyn_.cull = cull; }
    const int front = R(0x1834) == 0x900 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (front != dyn_.front) { vkCmdSetFrontFace(cb, VkFrontFace(front)); dyn_.front = front; }
    if (int(topo) != dyn_.topo) { vkCmdSetPrimitiveTopology(cb, topo); dyn_.topo = int(topo); }
    const int restart = d.restart ? 1 : 0;
    if (restart != dyn_.restart) { vkCmdSetPrimitiveRestartEnable(cb, restart); dyn_.restart = restart; }
    const int depth_on = t.pass.depth && (R(0xa74) & 1);
    if (depth_on != dyn_.dt) { vkCmdSetDepthTestEnable(cb, depth_on); dyn_.dt = depth_on; }
    const int dw = depth_on && (R(0xa70) & 1);
    if (dw != dyn_.dw) { vkCmdSetDepthWriteEnable(cb, dw); dyn_.dw = dw; }
    const int dc = int(compare_op(R(0xa6c)));
    if (dc != dyn_.dc) { vkCmdSetDepthCompareOp(cb, VkCompareOp(dc)); dyn_.dc = dc; }
    const int stencil_on = t.pass.depth && t.pass.stencil && (R(0x328) & 1);
    if (stencil_on != dyn_.st) { vkCmdSetStencilTestEnable(cb, stencil_on); dyn_.st = stencil_on; }
    if (stencil_on) {
        const bool two = R(0x348) & 1;
        const u32 fb = two ? 0x34c : 0x32c;
        const u32 so[8] = {R(0x33c), R(0x344), R(0x340), R(0x330), R(fb + 0x10), R(fb + 0x18), R(fb + 0x14), R(fb + 0x04)};
        if (std::memcmp(so, dyn_.sops, sizeof so)) {
            vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_op(so[0]), stencil_op(so[1]), stencil_op(so[2]), compare_op(so[3]));
            vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op(so[4]), stencil_op(so[5]), stencil_op(so[6]), compare_op(so[7]));
            std::memcpy(dyn_.sops, so, sizeof so);
        }
        const u32 sm[6] = {R(0x338) & 0xff, R(fb + 0x0c) & 0xff, R(0x32c) & 0xff, R(fb) & 0xff, R(0x334) & 0xff, R(fb + 0x08) & 0xff};
        if (std::memcmp(sm, dyn_.smask, sizeof sm)) {
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, sm[0]);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, sm[1]);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, sm[2]);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, sm[3]);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, sm[4]);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, sm[5]);
            std::memcpy(dyn_.smask, sm, sizeof sm);
        }
    } else if (dyn_.sops[0] == ~0u) {
        // stencil state must be valid once per command buffer even while the test is off
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        std::fill(std::begin(dyn_.sops), std::end(dyn_.sops), 0u);
        std::fill(std::begin(dyn_.smask), std::end(dyn_.smask), 0x100u);
    }
    if (dyn_.bias_en == 0 && dyn_.bias[0] == -1) { vkCmdSetDepthBias(cb, 0.f, 0.f, 0.f); dyn_.bias[0] = dyn_.bias[1] = 0.f; }

    static const bool no_draw = std::getenv("TOD_VKN_NODRAW") != nullptr;  // CPU-side benchmarking on software GPUs
    if (no_draw) {
    } else if (d.flags & DF_INDEXED) {
        vkCmdBindIndexBuffer(cb, ring_.buf, d.index_off, d.index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        for (u32 i = 0; i < d.nranges; ++i)
            if (ranges[i].count) vkCmdDrawIndexed(cb, ranges[i].count, 1, ranges[i].first, d.vertex_offset, 0);
    } else {
        for (u32 i = 0; i < d.nranges; ++i)
            if (ranges[i].count) vkCmdDraw(cb, ranges[i].count, 1, ranges[i].first, 0);
    }
    ++draws_in_frame_;
}

// ---- flip / present ---------------------------------------------------------------------------
void RenderThread::save_pipeline_cache() {
    last_cache_save_ = std::chrono::steady_clock::now();
    pipelines_saved_ = pipelines_created_;
    size_t n = 0;
    if (vkGetPipelineCacheData(dev_, pcache_, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<u8> blob(n);
    if (vkGetPipelineCacheData(dev_, pcache_, &n, blob.data()) != VK_SUCCESS) return;
    const std::string path = g_config.data_dir + "/cache/pipelines.bin", tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(blob.data(), 1, n, f) == n;
        std::fclose(f);
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, path, ec);
    }
}

bool RenderThread::create_swapchain() {
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
    for (auto v : swap_views_) vkDestroyImageView(dev_, v, nullptr);
    swap_views_.clear();
    for (VkImage im : swap_images_) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = im;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swap_format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        VKC(vkCreateImageView(dev_, &vci, nullptr, &v));
        swap_views_.push_back(v);
    }
    swap_done_.resize(n);
    for (auto& s : swap_done_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(dev_, &si, nullptr, &s));
    }
    swap_dirty_ = false;
    return true;
}

void RenderThread::do_flip(const FlipPacket& f) {
    end_pass();
    const DisplayBuffer& db = f.db;
    const Image* src = nullptr;
    u32 scale = 1;
    if (f.mem_off != ~0u) {
        if (!display_upload_.img || display_upload_.w != db.width || display_upload_.h != db.height) {
            if (display_upload_.img) destroy_image_later(display_upload_);
            display_upload_ = create_image(VK_FORMAT_R8G8B8A8_UNORM, db.width, db.height, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
            transition_new(display_upload_);
        }
        VkBufferImageCopy c{};
        c.bufferOffset = f.mem_off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {db.width, db.height, 1};
        vkCmdCopyBufferToImage(cmd(), ring_.buf, display_upload_.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier();
        src = &display_upload_;
    } else if (auto it = latest_.find(u64(kLocalBase + db.offset) << 1); it != latest_.end()) {
        src = &it->second->img;
        scale = S_;
    }
    F().release_pos = f.ring_pos;
    present(src, db, scale);
    ++frame_no_;
    if (g_log_level >= 1 && (frame_no_ <= 3 || frame_no_ % 600 == 0))
        LOG_NOTE("vkn: frame %llu: %llu draws, %zu surfaces, %zu textures, %llu pipelines, %llu draws waited for a shader (%llu/%llu bindings missing a texture/surface, %llu image copies from no surface so far)",
                 (unsigned long long)frame_no_, (unsigned long long)draws_in_frame_, surfaces_.size(), textures_.size(),
                 (unsigned long long)pipelines_created_, (unsigned long long)draws_waiting_, (unsigned long long)missing_textures_, (unsigned long long)missing_surfaces_, (unsigned long long)missing_blits_);
    draws_in_frame_ = 0;
    if (pipelines_created_ != pipelines_saved_ && std::chrono::steady_clock::now() - last_cache_save_ > std::chrono::seconds(20)) save_pipeline_cache();
    fi_ = (fi_ + 1) % kFrames;
    begin_frame();
    apply_options();
    evict_surfaces();
    { std::lock_guard lk(sh_.frames.lock); sh_.frames.done.fetch_add(1); }
    sh_.frames.cv.notify_all();
}

void RenderThread::present(const Image* src, const DisplayBuffer& db, u32 scale) {
    // debugging: TOD_VKN_SHOTSURF=<hex ea> makes screenshots save that colour surface instead
    static const u32 env_surf = [] { const char* e = std::getenv("TOD_VKN_SHOTSURF"); return e ? u32(std::strtoul(e, nullptr, 16)) : 0u; }();
    const u32 shot_surf = gfx::g.shot_surf.load() ? gfx::g.shot_surf.load() : env_surf;   // or "!shotsurf <hex>
    DisplayBuffer db2 = db;
    if (shot_surf && gfx::g.screenshot.load()) {
        auto it = latest_.find(u64(shot_surf) << 1);
        if (it != latest_.end() && it->second->img.format == VK_FORMAT_R8G8B8A8_UNORM) {
            src = &it->second->img; db2.width = it->second->gw; db2.height = it->second->gh;
        }
    }
    const DisplayBuffer& dbx = db2;
    const u32 sw = src ? std::min(src->w, dbx.width ? dbx.width * scale : src->w) : 0, sh = src ? std::min(src->h, dbx.height ? dbx.height * scale : src->h) : 0;
    const bool dump = !dump_dir_.empty() && src && frame_no_ % dump_every_ == 0 && src->format == VK_FORMAT_R8G8B8A8_UNORM;
    u32 dw = sw, dh = sh;
    // Screenshot (F12): the picture at the size it is rendered, so at resolution scale 2-4 a
    // high-resolution image; at 1x it goes through FXAA when that is on, as on screen.
    const bool shot = src && sw && sh && src->format == VK_FORMAT_R8G8B8A8_UNORM && 4ull * sw * sh <= 4096ull * 4096 * 4 &&
                      gfx::g.screenshot.exchange(false);
    if (shot) {
        if (!readback_.buf) readback_ = create_buffer(4096ull * 4096 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        const Image* from = src;
        if (scale == 1 && gfx::g.aa != gfx::AA_OFF) {
            if (!shot_target_.img || shot_target_.w != sw || shot_target_.h != sh) {
                if (shot_target_.img) destroy_image_later(shot_target_);
                shot_target_ = create_image(VK_FORMAT_R8G8B8A8_UNORM, sw, sh, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                transition_new(shot_target_);
            }
            post_process(*src, sw, sh, shot_target_.view, shot_target_.format, VK_IMAGE_LAYOUT_GENERAL, {{0, 0}, {sw, sh}}, {sw, sh});
            from = &shot_target_;
        }
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {sw, sh, 1};
        vkCmdCopyImageToBuffer(cmd(), from->img, VK_IMAGE_LAYOUT_GENERAL, readback_.buf, 1, &c);
        barrier();
    }
    // after the frame is submitted: wait for it and write the PNG on its own thread
    auto save_shot = [&] {
        if (!shot) return;
        VKC(vkWaitForFences(dev_, 1, &F().fence, VK_TRUE, UINT64_MAX));
        std::vector<u8> px(readback_.ptr, readback_.ptr + 4ull * sw * sh);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        std::thread([px = std::move(px), w = sw, h = sh] {
            const std::string path = gfx::screenshot_path();
            write_png(path, px.data(), w, h);
            LOG_NOTE("screenshot: %s (%ux%u)", path.c_str(), w, h);
            overlay_notify("Screenshot saved: %s", path.c_str());
        }).detach();
    };
    if (dump) {
        // dumped at the PS3's size, through the same downsampling / anti-aliasing as the window
        const Image* from = src;
        if (scale > 1 || gfx::g.aa != gfx::AA_OFF) {
            dw = std::max(1u, sw / scale); dh = std::max(1u, sh / scale);
            if (!post_target_.img || post_target_.w != dw || post_target_.h != dh) {
                if (post_target_.img) destroy_image_later(post_target_);
                post_target_ = create_image(VK_FORMAT_R8G8B8A8_UNORM, dw, dh, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                transition_new(post_target_);
            }
            post_process(*src, sw, sh, post_target_.view, post_target_.format, VK_IMAGE_LAYOUT_GENERAL, {{0, 0}, {dw, dh}}, {dw, dh});
            from = &post_target_;
        }
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {dw, dh, 1};
        vkCmdCopyImageToBuffer(cmd(), from->img, VK_IMAGE_LAYOUT_GENERAL, readback_.buf, 1, &c);
        barrier();
    }
    if (!surface_) {
        submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        save_shot();
        if (dump) {
            VKC(vkWaitForFences(dev_, 1, &F().fence, VK_TRUE, UINT64_MAX));
            std::vector<u8> px(readback_.ptr, readback_.ptr + 4ull * dw * dh);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            char name[64];
            std::snprintf(name, sizeof name, "/frame_%06llu.png", (unsigned long long)frame_no_);
            write_png(dump_dir_ + name, px.data(), dw, dh);
        }
        return;
    }
    if (swap_dirty_) create_swapchain();
    u32 index = 0;
    VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, F().acquire, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swap_dirty_ = true;
        submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        save_shot();
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
        const float aspect = std::abs(g_config.aspect - 16.f / 9.f) > 0.01f ? g_config.aspect : float(sw) / float(sh);
        u32 ow = swap_extent_.width, oh = u32(float(ow) / aspect);
        if (oh > swap_extent_.height) { oh = swap_extent_.height; ow = u32(float(oh) * aspect); }
        const s32 ox = s32(swap_extent_.width - ow) / 2, oy = s32(swap_extent_.height - oh) / 2;
        VkImageMemoryBarrier tb = b;
        tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        tb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        tb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
        post_process(*src, sw, sh, swap_views_[index], swap_format_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, {{ox, oy}, {ow, oh}}, swap_extent_);
        tb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        tb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
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
    save_shot();
}

// ---- options and post-processing ------------------------------------------------------------------
// Picks up graphics options changed from the menu. A new resolution scale drops every surface; they
// are made again at the new size as the game draws (the next frame or two may miss render-to-texture
// effects that are only redrawn occasionally).
void RenderThread::apply_options() {
    const u32 want = u32(std::clamp(gfx::g.resolution_scale.load(std::memory_order_relaxed), 1, 4));
    if (want == S_) return;
    end_pass();
    for (auto& [k, sp] : surfaces_) destroy_surface_later(sp.get());
    surfaces_.clear();
    latest_.clear();
    S_ = want;
    LOG_NOTE("vkn: rendering at %ux the PS3 resolution", S_);
}

namespace {
const char* kPostVs = R"(#version 450
layout(location = 0) out vec2 uv;
void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2. - 1., 0., 1.);
}
)";

// FXAA after Timothy Lottes' FXAA 3.11 (quality preset 12-ish), on the gamma-encoded picture.
const char* kPostFs = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    vec4 area;   // xy: part of src shown (uv), zw: 1 / src size
    vec4 out_;   // xy: 1 / output size, z: mode (0 plain, 1 fxaa), w: taps per axis when downsampling
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

float luma(vec3 c) { return sqrt(dot(c, vec3(0.299, 0.587, 0.114))); }
vec3 at(vec2 p) { return textureLod(src, p, 0.).rgb; }
float lat(vec2 p, vec2 off) { return luma(textureLod(src, p + off * pc.area.zw, 0.).rgb); }

vec3 fxaa(vec2 p) {
    vec3 cM = at(p);
    float lM = luma(cM);
    float lN = lat(p, vec2(0, -1)), lS = lat(p, vec2(0, 1)), lE = lat(p, vec2(1, 0)), lW = lat(p, vec2(-1, 0));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW))), lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) return cM;
    float lNW = lat(p, vec2(-1, -1)), lNE = lat(p, vec2(1, -1)), lSW = lat(p, vec2(-1, 1)), lSE = lat(p, vec2(1, 1));
    float lNS = lN + lS, lWE = lW + lE;
    float eH = abs(-2. * lW + lNW + lSW) + 2. * abs(-2. * lM + lNS) + abs(-2. * lE + lNE + lSE);
    float eV = abs(-2. * lN + lNW + lNE) + 2. * abs(-2. * lM + lWE) + abs(-2. * lS + lSW + lSE);
    bool horz = eH >= eV;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool steep1 = abs(g1) >= abs(g2);
    float gScaled = 0.25 * max(abs(g1), abs(g2));
    float step = horz ? pc.area.w : pc.area.z;
    float lLocal;
    if (steep1) { step = -step; lLocal = 0.5 * (l1 + lM); } else lLocal = 0.5 * (l2 + lM);
    vec2 cur = p;
    if (horz) cur.y += step * 0.5; else cur.x += step * 0.5;
    vec2 off = horz ? vec2(pc.area.z, 0.) : vec2(0., pc.area.w);
    vec2 u1 = cur - off, u2 = cur + off;
    float e1 = luma(at(u1)) - lLocal, e2 = luma(at(u2)) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    const float Q[10] = float[](1.5, 2., 2., 2., 2., 4., 8., 8., 8., 8.);
    for (int i = 0; i < 10 && !(r1 && r2); ++i) {
        if (!r1) { u1 -= off * Q[i]; e1 = luma(at(u1)) - lLocal; r1 = abs(e1) >= gScaled; }
        if (!r2) { u2 += off * Q[i]; e2 = luma(at(u2)) - lLocal; r2 = abs(e2) >= gScaled; }
    }
    float d1 = horz ? p.x - u1.x : p.y - u1.y, d2 = horz ? u2.x - p.x : u2.y - p.y;
    bool near1 = d1 < d2;
    float dist = min(d1, d2), len = d1 + d2;
    float pixOff = -dist / len + 0.5;
    bool mSmaller = lM < lLocal;
    bool correct = ((near1 ? e1 : e2) < 0.) != mSmaller;
    float fin = correct ? pixOff : 0.;
    float avg = (1. / 12.) * (2. * (lNS + lWE) + lNW + lNE + lSW + lSE);
    float sub = clamp(abs(avg - lM) / range, 0., 1.);
    sub = (-2. * sub + 3.) * sub * sub;
    fin = max(fin, sub * sub * 0.75);
    vec2 q = p;
    if (horz) q.y += fin * step; else q.x += fin * step;
    return at(q);
}

void main() {
    vec2 p = uv * pc.area.xy;
    int taps = int(pc.out_.w);
    vec3 c;
    if (taps > 1) {   // supersampled: average the source pixels under this output pixel
        vec2 fp = pc.out_.xy * pc.area.xy;
        c = vec3(0.);
        for (int y = 0; y < taps; ++y)
            for (int x = 0; x < taps; ++x) c += textureLod(src, p + ((vec2(x, y) + 0.5) / float(taps) - 0.5) * fp, 0.).rgb;
        c /= float(taps * taps);
    } else if (pc.out_.z > 0.5) {
        c = fxaa(p);
    } else {
        c = at(p);
    }
    o = vec4(c, 1.);
}
)";
}  // namespace

void RenderThread::init_post() {
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 1;
    dl.pBindings = &b;
    VKC(vkCreateDescriptorSetLayout(dev_, &dl, nullptr, &post_set_layout_));
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &post_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VKC(vkCreatePipelineLayout(dev_, &pl, nullptr, &post_layout_));
    post_vs_ = compile(kPostVs, true, fnv(kPostVs, std::strlen(kPostVs)));
    post_fs_ = compile(kPostFs, false, fnv(kPostFs, std::strlen(kPostFs)));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(dev_, &sci, nullptr, &post_sampler_));
}

void RenderThread::post_process(const Image& src, u32 sw, u32 sh, VkImageView dst, VkFormat fmt, VkImageLayout layout, VkRect2D rect, VkExtent2D ext) {
    if (!post_vs_ || !post_fs_ || !rect.extent.width || !rect.extent.height) return;
    VkPipeline& pipe = post_pipes_[fmt];
    if (!pipe) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = post_vs_; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = post_fs_; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cb{};
        cb.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo prc{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        prc.colorAttachmentCount = 1;
        prc.pColorAttachmentFormats = &fmt;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &prc;
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cbs; gp.pDynamicState = &dy;
        gp.layout = post_layout_;
        if (vkCreateGraphicsPipelines(dev_, pcache_, 1, &gp, nullptr, &pipe) != VK_SUCCESS) { pipe = VK_NULL_HANDLE; return; }
    }
    end_pass();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = dst;
    ca.imageLayout = layout;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    VkCommandBuffer cb = cmd();
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    dyn_.pipe = VK_NULL_HANDLE;
    const VkViewport v{float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height), 0.f, 1.f};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &rect);
    dyn_.vp = {}; dyn_.sc = {};
    VkDescriptorImageInfo ii{post_sampler_, src.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    push_descriptors_(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &w);
    // source pixels per output pixel: above ~1.3 average them (supersampling), else FXAA if asked
    const float fx = float(sw) / float(rect.extent.width), fy = float(sh) / float(rect.extent.height);
    const float f = std::max(fx, fy);
    const int taps = f > 1.3f ? std::min(4, int(std::ceil(f))) : 1;
    const float pc[8] = {float(sw) / float(src.w), float(sh) / float(src.h), 1.f / float(src.w), 1.f / float(src.h),
                         1.f / float(rect.extent.width), 1.f / float(rect.extent.height),
                         gfx::g.aa.load(std::memory_order_relaxed) == gfx::AA_FXAA ? 1.f : 0.f, float(taps)};
    vkCmdPushConstants(cb, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier();
}

// ---- packet loop --------------------------------------------------------------------------------
void RenderThread::process(Batch* b) {
    const u8* p = b->bytes.data();
    const u8* end = p + b->bytes.size();
    while (p + sizeof(PacketHeader) <= end) {
        PacketHeader h;
        std::memcpy(&h, p, sizeof h);
        const u8* body = p + sizeof h;
        p = body + h.size;
        switch (h.type) {
        case P_REGS: {
            u32 n;
            std::memcpy(&n, body, 4);
            const u32* kv = reinterpret_cast<const u32*>(body + 4);
            for (u32 i = 0; i < n; ++i) regs_[kv[2 * i] & 0x7ff] = kv[2 * i + 1];
            break;
        }
        case P_VP: {
            const auto* u = reinterpret_cast<const VpUpload*>(body);
            Vp& v = vps_[u->hash];
            v.ucode.assign(u->ucode, u->ucode + kVpInstructions * 4);
            v.start = u->start;
            v.info = vp_analyze(v.ucode.data(), v.start);
            break;
        }
        case P_FP: {
            const auto* u = reinterpret_cast<const FpUpload*>(body);
            Fp& f = fps_[u->key];
            const u8* bytes = reinterpret_cast<const u8*>(u + 1);
            f.bytes.assign(bytes, bytes + u->size);
            f.info = fp_analyze(f.bytes.data(), u32(f.bytes.size()));
            break;
        }
        case P_TEX: do_texture(*reinterpret_cast<const TexUpload*>(body)); break;
        case P_TEX_FREE: {
            u32 id;
            std::memcpy(&id, body, 4);
            if (auto it = textures_.find(id); it != textures_.end()) { destroy_image_later(it->second->img); textures_.erase(it); }
            break;
        }
        case P_DRAW: {
            DrawPacket d;
            std::memcpy(&d, body, sizeof d);
            do_draw(d, reinterpret_cast<const DrawRange2*>(body + sizeof d));
            break;
        }
        case P_CLEAR: { ClearPacket c; std::memcpy(&c, body, sizeof c); do_clear(c); break; }
        case P_BLIT: { BlitParams bp; std::memcpy(&bp, body, sizeof bp); do_blit(bp); break; }
        case P_DROP_SURFACE: {
            u32 v[2];
            std::memcpy(v, body, 8);
            latest_.erase((u64(v[0]) << 1) | (v[1] ? 1 : 0));
            break;
        }
        case P_FLIP: { FlipPacket f; std::memcpy(&f, body, sizeof f); do_flip(f); break; }
        case P_SUBMIT: {
            u64 pos;
            std::memcpy(&pos, body, 8);
            flush_and_wait();
            release_ring(pos);
            break;
        }
        default: LOG_ERR("vkn: unknown packet type %u", h.type); return;
        }
    }
}

RenderThread* create_render_thread(const VkPresentTarget* target, Shared& sh) {
    auto* rt = new RenderThread(sh);
    if (!rt->init(target)) { delete rt; return nullptr; }
    return rt;
}

void start_render_thread(RenderThread* rt) { std::thread([rt] { rt->run(); }).detach(); }
void process_batch(RenderThread* rt, Batch* b) { rt->process(b); }

}  // namespace rsx::vkn
