// The Vulkan 1.3 renderer (gfx.hpp). The Vulkan library is loaded at run time
// through volk, so there's nothing to link against and a machine without
// Vulkan gets a message.
//
// One command buffer records a frame. A render pass (dynamic rendering) on
// the back buffer and the depth buffer is begun when the first thing needs
// it and ended before anything else uses those images. A clear of the whole
// buffer that arrives before the pass begins becomes the pass's load
// operation, which is what tile-based GPUs (Raspberry Pi, Apple) want; a
// clear inside a pass, or of a rectangle, is a vkCmdClearAttachments.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <map>
#include <unordered_map>

#include "gfx.hpp"
#include "nv2a_shaders.hpp"
#include "spirv.hpp"
#include "texture.hpp"
#include "volk.h"

// Vulkan structures are zero-initialised by naming only their sType
// (VkFoo f{VK_STRUCTURE_TYPE_FOO}), which -Wextra would flag in GCC and Clang.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace qilin::gfx {
namespace {

[[noreturn]] void fatal(const char* what, VkResult r) {
    std::fflush(stdout);
    std::fprintf(stderr, "qilin: Vulkan: %s failed (VkResult %d)\n", what, int(r));
    std::exit(3);
}
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) fatal(what, r);
}

struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    uint32_t width = 0, height = 0;
};

// The pipeline stages and accesses that go with an image layout, for barriers.
void layout_use(VkImageLayout layout, VkPipelineStageFlags2& stage, VkAccessFlags2& access) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            access = VK_ACCESS_2_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            break;
        default:  // undefined: nothing to wait for
            stage = VK_PIPELINE_STAGE_2_NONE;
            access = VK_ACCESS_2_NONE;
            break;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++*static_cast<uint32_t*>(user);
    std::fprintf(stderr, "qilin: Vulkan validation: %s\n", data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool has(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::ranges::any_of(list, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override { destroy(); }
    bool init(const Config& config, std::string* error);

    const std::string& device_name() const override { return device_name_; }
    const Config& config() const override { return config_; }
    void clear(const Rect* rects, uint32_t count, uint32_t flags, const float rgba[4], float depth, uint32_t stencil) override;
    void present() override;
    uint64_t frames_presented() const override { return frames_; }
    uint32_t back_index() const override { return back_; }
    uint32_t front_index() const override { return front_; }
    void flush() override {
        end_pass();
        submit();
    }
    uint32_t create_texture(const TextureData& data) override;
    void destroy_texture(uint32_t id) override;
    void draw(const DrawCmd& cmd) override;
    uint32_t create_render_target(uint32_t width, uint32_t height) override;
    void copy_frame_to_target(uint32_t frame_index, uint32_t target_id, const std::vector<CopyRegion>& regions) override;
    uint32_t create_depth_target(uint32_t width, uint32_t height) override;
    void set_render_target(uint32_t id, uint32_t depth) override;
    Image read_front_buffer() override { return read(color_[front_]); }
    Image read_back_buffer() override { return read(color_[back_]); }
    Image read_frame_buffer(uint32_t index) override { return read(color_.at(index)); }
    void write_frame_buffer(uint32_t index, const Image& image) override { write(color_.at(index), image); }
    Image read_render_target(uint32_t id) override { return read(render_targets_.at(id)); }
    void write_render_target(uint32_t id, const Image& image) override { write(render_targets_.at(id), image); }
    void show_front() override {
        end_pass();
        show(color_[front_]);
    }
    void set_gamma_ramp(const uint8_t ramp[3][256]) override;
    uint32_t validation_errors() const override { return validation_errors_; }

private:
    bool create_instance(std::string* error);
    bool pick_device(std::string* error);
    void create_device();
    Target create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width = 0, uint32_t height = 0);
    Target& target() { return current_rt_ ? render_targets_.at(current_rt_) : color_[back_]; }  // what draws and clears go to
    // The depth buffer draws and clears use, or null.
    Target* depth_target() {
        const uint32_t d = current_rt_ ? rt_depth_ : Renderer::kDeviceDepth;
        if (d == Renderer::kDeviceDepth) return depth_.image ? &depth_ : nullptr;
        auto it = depth_targets_.find(d);
        return it == depth_targets_.end() ? nullptr : &it->second;
    }
    bool target_has_depth() { return depth_target() != nullptr; }
    uint32_t memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted) const;
    void destroy();

    void record();                                   // makes sure the frame's command buffer is recording
    void transition(Target& t, VkImageLayout to);
    void begin_pass();                               // makes sure the render pass is open, applying pending clears
    void end_pass();
    // Ends the command buffer and submits it, moving to the next slot (which
    // waits for that slot's last submission); the submission can wait for a
    // semaphore before its transfers and signal one when done.
    void submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    void show(Target& frame);                        // to the window: scaled into the swapchain, then presented
    bool make_swapchain(uint32_t width, uint32_t height);
    void create_draw_objects();
    VkDeviceSize ring_alloc(VkDeviceSize bytes, VkDeviceSize align, const void* data);  // into this submission's ring
    VkShaderModule shader(uint64_t key, const std::vector<uint32_t>* code);
    VkPipeline pipeline(const DrawCmd& cmd);
    VkSampler sampler(const SamplerDesc& d);
    Image read(Target& t);
    void write(Target& t, const Image& image);

    Config config_;
    std::string device_name_;
    uint32_t validation_errors_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    bool portability_subset_ = false;
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    // The window, when there is one.
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swap_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;  // when the window's images can be drawn into (present_pass_)
    bool present_pass_ = false;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered_;  // one for each swapchain image, which its presentation waits for
    bool swap_stale_ = false;            // out of date or suboptimal: made again before the next frame

    std::vector<Target> color_;  // the flip chain: back_ is drawn to, front_ was presented last
    uint32_t back_ = 0, front_ = 0;
    Target depth_;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkDeviceSize readback_size_ = 0;

    bool recording_ = false, pass_open_ = false;
    // Vulkan's pipeline cache, loaded from config_.pipeline_cache and saved
    // there a few seconds after the last new pipeline (and at the end).
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    bool takes_blocks_ = false;  // BC1-3 textures sampled as they are
public:
    bool takes_blocks() const override { return takes_blocks_; }
    bool update_texture(uint32_t id, const TextureData& t) override;
private:
    uint32_t unsaved_pipelines_ = 0;
    std::chrono::steady_clock::time_point last_pipeline_{};
    void load_pipeline_cache();
    void save_pipeline_cache();
    // The state draw() last set in the command buffer being recorded, so a
    // draw sets only what changed. Forgotten when a command buffer begins
    // and when another pipeline (the present pass's) is bound, which sets
    // its own static state.
    struct DrawState {
        bool valid = false;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkViewport viewport{};
        VkRect2D scissor{};
        VkCullModeFlags cull = 0;
        VkFrontFace front = VK_FRONT_FACE_CLOCKWISE;
        bool depth_test = false, depth_write = false, stencil_test = false;
        VkCompareOp depth_compare = VK_COMPARE_OP_NEVER;
        VkStencilOp stencil_fail = VK_STENCIL_OP_KEEP, stencil_pass = VK_STENCIL_OP_KEEP, stencil_depth_fail = VK_STENCIL_OP_KEEP;
        VkCompareOp stencil_compare = VK_COMPARE_OP_NEVER;
        uint32_t stencil_read = 0, stencil_write = 0, stencil_ref = 0;
        float blend[4] = {};
    } drawn_;
    struct PendingClear {  // whole-buffer clears waiting to become the pass's load operations
        bool color = false, depth = false, stencil = false;
        VkClearColorValue color_value{};
        float depth_value = 0;
        uint32_t stencil_value = 0;
    } pending_;
    uint64_t frames_ = 0;

    // Drawing: one descriptor set layout (vertex and fragment uniforms as
    // dynamic uniform buffers, four combined image samplers), a ring buffer
    // for each submission's vertices, indices and uniforms, and caches.
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkBuffer ring_ = VK_NULL_HANDLE;
    VkDeviceMemory ring_memory_ = VK_NULL_HANDLE;
    uint8_t* ring_mapped_ = nullptr;
    VkDeviceSize ring_size_ = 0, ring_used_ = 0, uniform_align_ = 256;
    std::unordered_map<uint64_t, VkShaderModule> shaders_;
    struct PipelineKey {
        uint64_t vs = 0, fs = 0;
        uint32_t inputs = 0;
        uint8_t primitive = 0, blend = 0, write_mask = 0, factors[4] = {}, ops[2] = {};
        uint8_t depth = 0;  // the target has a depth-stencil attachment
        auto operator<=>(const PipelineKey&) const = default;
    };
    std::map<PipelineKey, VkPipeline> pipelines_;
    std::vector<std::pair<SamplerDesc, VkSampler>> samplers_;
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        // The shape it was made with: update_texture takes texels of the same.
        uint32_t width = 0, height = 0, depth = 1, levels = 1, faces = 1;
        Blocks blocks = Blocks::None;
    };
    // The texels into the image through the ring (or a staging buffer of
    // their own when larger than half of it), between barriers: from
    // nothing for a new image, else after the draws that sampled it.
    void upload_texels(VkImage image, const TextureData& t, bool fresh);
    std::unordered_map<uint32_t, Texture> textures_;
    uint32_t next_texture_ = 1;
    // Textures drawn into (create_render_target), by the same ids as textures;
    // current_rt_ is the one draws go to, 0 for the back buffer.
    std::unordered_map<uint32_t, Target> render_targets_;
    uint32_t current_rt_ = 0;
    uint32_t rt_depth_ = 0;  // what goes with the render target: 0, kDeviceDepth or a depth target
    std::unordered_map<uint32_t, Target> depth_targets_;  // by the same ids as textures
    std::vector<Texture> dead_textures_;                                  // freed once the GPU is done with them
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging_;            // likewise

    // Submissions in flight: the CPU records the next while the GPU runs
    // the last. Each slot has its own command buffer, fence, descriptor
    // pool, ring and acquire semaphore, and keeps what its submission used
    // (staging buffers, destroyed textures) until its fence says the GPU is
    // done; cmd_, fence_, descriptor_pool_, ring_* and acquired_ are the
    // current slot's.
    struct DescriptorKey {
        VkImageView views[4] = {};
        VkSampler samplers[4] = {};
        auto operator<=>(const DescriptorKey&) const = default;
    };
    static constexpr uint32_t kSlots = 2;
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceMemory ring_memory = VK_NULL_HANDLE;
        uint8_t* ring_mapped = nullptr;
        VkSemaphore acquired = VK_NULL_HANDLE;
        std::vector<Texture> dead;
        std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;
        std::map<DescriptorKey, VkDescriptorSet> sets;  // allocated from `pool` this submission
        bool pending = false;
    };
    Slot slots_[kSlots];
    uint32_t slot_ = 0;
    void use_slot(uint32_t i);
    void retire(uint32_t i);  // waits for the slot's submission and frees what it used
    void wait_idle();         // every submission done (before reading results back)

    // Presenting: the frame through the gamma ramp into the window's image,
    // a pass of its own (two samplers: the frame, the ramp as a 256×1 texture).
    VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
    VkPipeline present_pipeline_ = VK_NULL_HANDLE;
    VkFormat present_format_ = VK_FORMAT_UNDEFINED;
    VkShaderModule present_shaders_[2] = {};
    uint32_t gamma_texture_ = 0;
    uint8_t gamma_[3][256] = {};
    bool gamma_set_ = false;
    void present_through_pass(Target& frame, uint32_t index, int32_t dx, int32_t dy, uint64_t dw, uint64_t dh);
    VkPipeline present_pipeline();
    VkBuffer staging_buffer(const void* data, VkDeviceSize bytes);
};

bool VulkanRenderer::create_instance(std::string* error) {
    const VkResult loaded = config_.surface.loader
                                ? (volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(config_.surface.loader)), VK_SUCCESS)
                                : volkInitialize();
    if (loaded != VK_SUCCESS) {
        *error = "no Vulkan library found: install a graphics driver with Vulkan 1.3 (on macOS, MoltenVK)";
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        *error = "the Vulkan library is older than Vulkan 1.3: update the graphics driver";
        return false;
    }
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
    available.resize(n);
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    layers.resize(n);

    std::vector<const char*> extensions, enabled_layers;
    VkInstanceCreateFlags flags = 0;
    // MoltenVK is a "portability" implementation, which the loader hides unless asked.
    if (has(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    const char* env = std::getenv("QILIN_VK_VALIDATION");
    bool validate = config_.validation || (env && *env && *env != '0');
    static constexpr char kValidation[] = "VK_LAYER_KHRONOS_validation";
    validate = validate && has(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
               std::ranges::any_of(layers, [](const VkLayerProperties& l) { return std::strcmp(l.layerName, kValidation) == 0; });
    if (validate) {
        enabled_layers.push_back(kValidation);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    for (const char* e : config_.surface.instance_extensions) {
        if (!has(available, e)) {
            *error = std::string("the Vulkan library has no ") + e + ", which the window needs";
            return false;
        }
        if (std::ranges::none_of(extensions, [&](const char* x) { return std::strcmp(x, e) == 0; })) extensions.push_back(e);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "qilin";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = uint32_t(enabled_layers.size());
    info.ppEnabledLayerNames = enabled_layers.data();
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (const VkResult r = vkCreateInstance(&info, nullptr, &instance_); r != VK_SUCCESS) {
        *error = "can't create a Vulkan instance (VkResult " + std::to_string(int(r)) + ")";
        return false;
    }
    volkLoadInstance(instance_);
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        m.pfnUserCallback = on_debug_message;
        m.pUserData = &validation_errors_;
        check(vkCreateDebugUtilsMessengerEXT(instance_, &m, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
    if (config_.surface.create) {
        surface_ = VkSurfaceKHR(config_.surface.create(instance_, error));
        if (!surface_) return false;
    }
    return true;
}

// The best device that has what the renderer needs: Vulkan 1.3 with dynamic
// rendering and synchronization2 (both required by 1.3) and a graphics queue.
// A discrete GPU beats an integrated one, which beats a software renderer.
bool VulkanRenderer::pick_device(std::string* error) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    devices.resize(n);
    if (devices.empty()) {
        *error = "Vulkan found no graphics device";
        return false;
    }
    std::string rejected;
    int best_score = -1;
    for (uint32_t i = 0; i < devices.size(); ++i) {
        if (config_.gpu >= 0 && uint32_t(config_.gpu) != i) continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        std::string why;
        uint32_t family = UINT32_MAX;
        if (props.apiVersion < VK_API_VERSION_1_3) {
            why = "its driver has Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
                  std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + ", not 1.3";
        } else {
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(devices[i], &f2);
            if (!f13.dynamicRendering) why = "no dynamic rendering";
            else if (!f13.synchronization2) why = "no synchronization2";
            uint32_t q = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &q, nullptr);
            std::vector<VkQueueFamilyProperties> families(q);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &q, families.data());
            for (uint32_t k = 0; k < q && family == UINT32_MAX; ++k) {
                VkBool32 presents = VK_TRUE;
                if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], k, surface_, &presents);
                if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) family = k;
            }
            if (why.empty() && family == UINT32_MAX) why = surface_ ? "no graphics queue that can show the window" : "no graphics queue";
            if (why.empty() && surface_) {
                uint32_t e = 0;
                vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &e, nullptr);
                std::vector<VkExtensionProperties> exts(e);
                vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &e, exts.data());
                exts.resize(e);
                if (!has(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) why = "no swapchains";
            }
        }
        if (!why.empty()) {
            rejected += std::string(rejected.empty() ? "" : "; ") + props.deviceName + ": " + why;
            continue;
        }
        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 4;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 3;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 2;
        if (score > best_score) {
            best_score = score;
            gpu_ = devices[i];
            queue_family_ = family;
            device_name_ = props.deviceName;
        }
    }
    if (gpu_ == VK_NULL_HANDLE) {
        *error = config_.gpu >= 0 && uint32_t(config_.gpu) >= devices.size()
                     ? "there is no Vulkan device number " + std::to_string(config_.gpu)
                     : "no graphics device can run the renderer (" + rejected + ")";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memory_);
    uint32_t e = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &e, nullptr);
    std::vector<VkExtensionProperties> extensions(e);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &e, extensions.data());
    extensions.resize(e);
    portability_subset_ = has(extensions, "VK_KHR_portability_subset");  // must be enabled where it's offered (MoltenVK)

    // 24-bit depth with stencil, as the Xbox's D24S8; Apple GPUs only have the 32-bit float one.
    for (const VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depth_format_ = f;
            break;
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED) {
        *error = device_name_ + " has no depth-stencil format the renderer can use";
        return false;
    }
    return true;
}

void VulkanRenderer::create_device() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = queue_family_;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    std::vector<const char*> extensions;
    if (portability_subset_) extensions.push_back("VK_KHR_portability_subset");
    if (surface_) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // Points wider than a pixel (the game's sprites), where the device has them.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(gpu_, &have);
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.largePoints = have.largePoints;
    // BC1-3 sampled with filtering, where the device has them; QILIN_NO_BC=1 decodes them on the CPU instead.
    if (have.textureCompressionBC && !(std::getenv("QILIN_NO_BC") && *std::getenv("QILIN_NO_BC") == '1')) {
        bool all = true;
        for (VkFormat fmt : {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(gpu_, fmt, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            all = all && (fp.optimalTilingFeatures & need) == need;
        }
        takes_blocks_ = all;
        f2.features.textureCompressionBC = all;
    }
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &q;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(gpu_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    load_pipeline_cache();

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (Slot& sl : slots_) {
        check(vkAllocateCommandBuffers(device_, &alloc, &sl.cmd), "vkAllocateCommandBuffers");
        check(vkCreateFence(device_, &fence, nullptr, &sl.fence), "vkCreateFence");
    }
    cmd_ = slots_[0].cmd, fence_ = slots_[0].fence;
}

uint32_t VulkanRenderer::memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    fatal("finding a memory type", VK_ERROR_FEATURE_NOT_PRESENT);
}

Target VulkanRenderer::create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width,
                                     uint32_t height) {
    Target t;
    t.aspect = aspect;
    t.width = width ? width : config_.width;
    t.height = height ? height : config_.height;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {t.width, t.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &info, nullptr, &t.image), "vkCreateImage");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, t.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &t.memory), "vkAllocateMemory");
    check(vkBindImageMemory(device_, t.image, t.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &view, nullptr, &t.view), "vkCreateImageView");
    return t;
}

bool VulkanRenderer::init(const Config& config, std::string* error) {
    config_ = config;
    if (config_.width == 0 || config_.height == 0 || config_.width > 16384 || config_.height > 16384 || config_.back_buffers == 0 ||
        config_.back_buffers > 3) {
        *error = "bad frame buffer size or count";
        return false;
    }
    if (!create_instance(error) || !pick_device(error)) return false;
    create_device();
    create_draw_objects();

    // B8G8R8A8: the byte order of the Xbox's A8R8G8B8 frame buffers.
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (uint32_t i = 0; i < config_.back_buffers + 1; ++i)
        color_.push_back(create_target(VK_FORMAT_B8G8R8A8_UNORM, color_usage, VK_IMAGE_ASPECT_COLOR_BIT));
    back_ = 0;
    front_ = uint32_t(color_.size()) - 1;
    if (config_.depth_stencil)
        depth_ = create_target(depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

    // Every buffer starts as zeros, so a frame never shows what the GPU's memory held before.
    record();
    const VkClearColorValue zero_color{};
    const VkClearDepthStencilValue zero_depth{};
    for (Target& t : color_) {
        transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{t.aspect, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd_, t.image, t.layout, &zero_color, 1, &range);
    }
    if (depth_.image) {
        transition(depth_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{depth_.aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd_, depth_.image, depth_.layout, &zero_depth, 1, &range);
    }
    submit();
    return true;
}

void VulkanRenderer::destroy() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        auto drop = [&](Target& t) {
            if (t.view) vkDestroyImageView(device_, t.view, nullptr);
            if (t.image) vkDestroyImage(device_, t.image, nullptr);
            if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        };
        for (Target& t : color_) drop(t);
        for (auto& [id, t] : render_targets_) drop(t);
        for (auto& [id, t] : depth_targets_) drop(t);
        drop(depth_);
        for (VkImageView v : swap_views_) vkDestroyImageView(device_, v, nullptr);
        if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
        if (present_layout_) vkDestroyPipelineLayout(device_, present_layout_, nullptr);
        if (present_set_layout_) vkDestroyDescriptorSetLayout(device_, present_set_layout_, nullptr);
        for (VkShaderModule m : present_shaders_) if (m) vkDestroyShaderModule(device_, m, nullptr);
        auto drop_texture = [&](Texture& t) {
            if (t.view) vkDestroyImageView(device_, t.view, nullptr);
            if (t.image) vkDestroyImage(device_, t.image, nullptr);
            if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        };
        for (auto& [id, t] : textures_) drop_texture(t);
        for (Texture& t : dead_textures_) drop_texture(t);
        for (auto& [b, m] : staging_) vkDestroyBuffer(device_, b, nullptr), vkFreeMemory(device_, m, nullptr);
        for (uint32_t i = 0; i < kSlots; ++i) retire(i);
        save_pipeline_cache();
        if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        for (auto& [k, pl] : pipelines_) vkDestroyPipeline(device_, pl, nullptr);
        for (auto& [k, m] : shaders_) vkDestroyShaderModule(device_, m, nullptr);
        for (auto& [d, smp] : samplers_) vkDestroySampler(device_, smp, nullptr);
        for (Slot& sl : slots_) {
            if (sl.ring) vkDestroyBuffer(device_, sl.ring, nullptr);
            if (sl.ring_memory) vkFreeMemory(device_, sl.ring_memory, nullptr);
            if (sl.pool) vkDestroyDescriptorPool(device_, sl.pool, nullptr);
            if (sl.fence) vkDestroyFence(device_, sl.fence, nullptr);
            if (sl.acquired) vkDestroySemaphore(device_, sl.acquired, nullptr);
        }
        if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
        if (readback_memory_) vkFreeMemory(device_, readback_memory_, nullptr);
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        for (VkSemaphore sem : rendered_) vkDestroySemaphore(device_, sem, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}

void VulkanRenderer::record() {
    if (recording_) return;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer");
    recording_ = true;
    drawn_.valid = false;
}

void VulkanRenderer::transition(Target& t, VkImageLayout to) {
    if (t.layout == to) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    layout_use(t.layout, b.srcStageMask, b.srcAccessMask);
    layout_use(to, b.dstStageMask, b.dstAccessMask);
    b.oldLayout = t.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {t.aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
    t.layout = to;
}

void VulkanRenderer::begin_pass() {
    if (pass_open_) return;
    record();
    Target& back = target();
    transition(back, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = back.view;
    color.imageLayout = back.layout;
    color.loadOp = pending_.color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = pending_.color_value;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {back.width, back.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    if (Target* dt = depth_target()) {
        transition(*dt, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth.imageView = stencil.imageView = dt->view;
        depth.imageLayout = stencil.imageLayout = dt->layout;
        depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.loadOp = pending_.depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        stencil.loadOp = pending_.stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = stencil.clearValue.depthStencil = {pending_.depth_value, pending_.stencil_value};
        info.pDepthAttachment = &depth;
        info.pStencilAttachment = &stencil;
    }
    vkCmdBeginRendering(cmd_, &info);
    pass_open_ = true;
    pending_ = {};
}

void VulkanRenderer::end_pass() {
    if (pending_.color || pending_.depth || pending_.stencil) begin_pass();  // clears nothing drew after still happen
    if (!pass_open_) return;
    vkCmdEndRendering(cmd_);
    pass_open_ = false;
}

void VulkanRenderer::clear(const Rect* rects, uint32_t count, uint32_t flags, const float rgba[4], float depth, uint32_t stencil) {
    if (!target_has_depth()) flags &= ~uint32_t(kClearDepth | kClearStencil);
    if (!(flags & (kClearColor | kClearDepth | kClearStencil))) return;
    const uint32_t tw = target().width, th = target().height;
    // Clip the rectangles to the buffer; no rectangles means all of it.
    std::vector<VkClearRect> clipped;
    bool whole = count == 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t x0 = std::max<int64_t>(rects[i].x, 0), y0 = std::max<int64_t>(rects[i].y, 0);
        const int64_t x1 = std::min<int64_t>(int64_t(rects[i].x) + rects[i].w, tw);
        const int64_t y1 = std::min<int64_t>(int64_t(rects[i].y) + rects[i].h, th);
        if (x1 <= x0 || y1 <= y0) continue;
        whole = whole || (x0 == 0 && y0 == 0 && x1 == tw && y1 == th);
        clipped.push_back({{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
    if (!whole && clipped.empty()) return;
    VkClearColorValue color{};
    for (int k = 0; k < 4; ++k) color.float32[k] = rgba[k];

    if (whole && !pass_open_) {  // becomes the load operation of the pass that follows
        if (flags & kClearColor) pending_.color = true, pending_.color_value = color;
        if (flags & kClearDepth) pending_.depth = true, pending_.depth_value = depth;
        if (flags & kClearStencil) pending_.stencil = true, pending_.stencil_value = stencil;
        return;
    }
    begin_pass();
    if (whole) clipped.assign(1, {{{0, 0}, {tw, th}}, 0, 1});
    VkClearAttachment attachments[2]{};
    uint32_t n = 0;
    if (flags & kClearColor) {
        attachments[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachments[n].colorAttachment = 0;
        attachments[n++].clearValue.color = color;
    }
    if (flags & (kClearDepth | kClearStencil)) {
        VkImageAspectFlags aspect = 0;
        if (flags & kClearDepth) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & kClearStencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        attachments[n].aspectMask = aspect;
        attachments[n++].clearValue.depthStencil = {depth, stencil};
    }
    vkCmdClearAttachments(cmd_, n, attachments, uint32_t(clipped.size()), clipped.data());
}

void VulkanRenderer::submit(VkSemaphore wait, VkSemaphore signal) {
    if (!recording_) return;
    check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    recording_ = false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd_;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &cb;
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, sg{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    if (wait) {
        w.semaphore = wait;
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        info.waitSemaphoreInfoCount = 1;
        info.pWaitSemaphoreInfos = &w;
    }
    if (signal) {
        sg.semaphore = signal;
        sg.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        info.signalSemaphoreInfoCount = 1;
        info.pSignalSemaphoreInfos = &sg;
    }
    check(vkQueueSubmit2(queue_, 1, &info, fence_), "vkQueueSubmit2");
    // In flight: what it uses is kept with its slot, and the next slot
    // (once its own submission is done) takes the recording.
    Slot& done = slots_[slot_];
    done.pending = true;
    for (Texture& t : dead_textures_) done.dead.push_back(t);
    dead_textures_.clear();
    for (auto& st : staging_) done.staging.push_back(st);
    staging_.clear();
    const uint32_t next = (slot_ + 1) % kSlots;
    retire(next);
    use_slot(next);
}

// The cache file is the driver's data as vkGetPipelineCacheData gives it.
// Its header names the vendor, device and driver build (the cache UUID);
// a file from another GPU or driver is left unread, as the driver would
// ignore it anyway, and some drivers don't take foreign data well.
void VulkanRenderer::load_pipeline_cache() {
    std::vector<char> data;
    if (!config_.pipeline_cache.empty()) {
        std::ifstream in(config_.pipeline_cache, std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu_, &props);
        uint32_t header[4] = {};
        if (data.size() >= 32) std::memcpy(header, data.data(), sizeof header);
        const bool ours = data.size() >= 32 && header[0] >= 32 && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                          header[2] == props.vendorID && header[3] == props.deviceID &&
                          std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        if (!ours) data.clear();
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) pipeline_cache_ = VK_NULL_HANDLE;
}

// Written to a new file and renamed over the old one, so a run stopped
// halfway leaves the last whole cache.
void VulkanRenderer::save_pipeline_cache() {
    if (!pipeline_cache_ || !unsaved_pipelines_ || config_.pipeline_cache.empty()) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    const std::filesystem::path to(config_.pipeline_cache), tmp = to.string() + ".new";
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(data.data(), std::streamsize(size))) return;
    }
    std::filesystem::rename(tmp, to, ec);
    if (!ec) unsaved_pipelines_ = 0;
}

void VulkanRenderer::present() {
    if (unsaved_pipelines_ && std::chrono::steady_clock::now() - last_pipeline_ > std::chrono::seconds(5)) save_pipeline_cache();
    end_pass();
    if (surface_) show(color_[back_]);
    else submit();
    front_ = back_;
    back_ = (back_ + 1) % uint32_t(color_.size());
    ++frames_;
}

// The swapchain for a drawable of width × height pixels: images the frame is
// copied into (8-bit UNORM, as the frame buffers, so the copy changes no
// value), and the present mode with the least latency that doesn't tear:
// MAILBOX where there is one, FIFO (always there) otherwise. The game paces
// itself, as on the console (Swap waits for its vblank), so either works.
bool VulkanRenderer::make_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {  // the window takes its size from the swapchain (Wayland)
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;  // minimised
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        std::fprintf(stderr, "qilin: Vulkan: the window's images can't be copied into on %s\n", device_name_.c_str());
        std::exit(3);
    }

    if (swap_format_ == VK_FORMAT_UNDEFINED) {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
        formats.resize(n);
        auto blits = [&](VkFormat f) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
            return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        };
        for (const VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32})
            for (const VkSurfaceFormatKHR& f : formats)
                if (swap_format_ == VK_FORMAT_UNDEFINED && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && blits(want))
                    swap_format_ = want;
        if (swap_format_ == VK_FORMAT_UNDEFINED) {  // an sRGB format only: the copy would encode, so the picture is brighter
            for (const VkSurfaceFormatKHR& f : formats)
                if (swap_format_ == VK_FORMAT_UNDEFINED && blits(f.format)) swap_format_ = f.format;
            if (swap_format_ == VK_FORMAT_UNDEFINED) {
                std::fprintf(stderr, "qilin: Vulkan: the window has no image format the renderer can copy into\n");
                std::exit(3);
            }
            std::fprintf(stderr, "qilin: the window has only sRGB formats; colours will be lighter than the console's\n");
        }
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
    const VkPresentModeKHR mode = std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                                                      : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");  // the old swapchain's images may still be presenting
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = count;
    info.imageFormat = swap_format_;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    present_pass_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    if (present_pass_) info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                           : caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) {
            info.compositeAlpha = a;
            break;
        }
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(device_, &info, nullptr, &made), "vkCreateSwapchainKHR");
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = made;
    swap_extent_ = extent;
    swap_stale_ = false;

    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swap_images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swap_images_.data());
    for (VkImageView v : swap_views_) vkDestroyImageView(device_, v, nullptr);
    swap_views_.clear();
    if (present_pass_)
        for (VkImage im : swap_images_) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swap_format_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v = VK_NULL_HANDLE;
            check(vkCreateImageView(device_, &vi, nullptr, &v), "vkCreateImageView");
            swap_views_.push_back(v);
        }
    const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (Slot& sl : slots_)
        if (!sl.acquired) check(vkCreateSemaphore(device_, &sem, nullptr, &sl.acquired), "vkCreateSemaphore");
    acquired_ = slots_[slot_].acquired;
    while (rendered_.size() < n) {
        VkSemaphore s = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device_, &sem, nullptr, &s), "vkCreateSemaphore");
        rendered_.push_back(s);
    }
    return true;
}

// The frame, scaled to fit the window with its shape kept (black bars on the
// sides or above and below), filtered linearly, then presented.
void VulkanRenderer::show(Target& frame) {
    uint32_t w = 0, h = 0;
    if (config_.surface.size) config_.surface.size(&w, &h);
    if (w == 0 || h == 0) return submit();  // minimised: nothing to show
    if (!swapchain_ || swap_stale_ || (w != swap_extent_.width || h != swap_extent_.height))
        if (!make_swapchain(w, h)) return submit();
    uint32_t index = 0;
    const VkResult got = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (got == VK_ERROR_OUT_OF_DATE_KHR) {
        swap_stale_ = true;
        return submit();
    }
    if (got == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    else check(got, "vkAcquireNextImageKHR");

    const uint64_t fw0 = config_.width, fh0 = config_.height, sw0 = swap_extent_.width, sh0 = swap_extent_.height;
    uint64_t dw0 = sw0, dh0 = sh0;
    if (sw0 * fh0 > sh0 * fw0) dw0 = std::max<uint64_t>(1, sh0 * fw0 / fh0);
    else dh0 = std::max<uint64_t>(1, sw0 * fh0 / fw0);
    if (present_pass_) return present_through_pass(frame, index, int32_t((sw0 - dw0) / 2), int32_t((sh0 - dh0) / 2), dw0, dh0);

    // Without a pass (images that can only be copied into): a scaled copy, no gamma ramp.
    record();
    transition(frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImage image = swap_images_[index];
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &dep);
    };
    // From UNDEFINED: the whole image is written, so what it held doesn't matter.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const uint64_t fw = config_.width, fh = config_.height, sw = swap_extent_.width, sh = swap_extent_.height;
    uint64_t dw = sw, dh = sh;
    if (sw * fh > sh * fw) dw = std::max<uint64_t>(1, sh * fw / fh);  // wider than the frame: bars at the sides
    else dh = std::max<uint64_t>(1, sw * fh / fw);
    const int32_t dx = int32_t((sw - dw) / 2), dy = int32_t((sh - dh) / 2);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(fw), int32_t(fh), 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + int32_t(dw), dy + int32_t(dh), 1};
    vkCmdBlitImage(cmd_, frame.image, frame.layout, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    submit(acquired_, rendered_[index]);

    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &rendered_[index];
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &index;
    const VkResult shown = vkQueuePresentKHR(queue_, &info);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    else check(shown, "vkQueuePresentKHR");
}

// ---- Presenting through the gamma ramp
namespace {
// A triangle over the destination rectangle: position xy, texture coordinate zw.
std::vector<uint32_t> present_vs() {
    spv::Builder b{spv::Stage::Vertex};
    const spv::Id f = b.t_float(), v4 = b.t_vec(4);
    const spv::Id in = b.input(v4, 0);
    const spv::Id v = b.load(v4, in);
    const spv::Id pos = b.builtin_var(spv::Storage::Output, v4, spv::builtin::Position);
    b.store(pos, b.vec4(b.extract(f, v, 0), b.extract(f, v, 1), b.c_float(0), b.c_float(1)));
    const spv::Id out = b.output(v4, 0);
    b.store(out, b.vec4(b.extract(f, v, 2), b.extract(f, v, 3), b.c_float(0), b.c_float(0)));
    return b.finish({in, pos, out});
}
// The frame, filtered, then each channel through its table (texel i at (i + 0.5) / 256).
// The frame (width × height) scaled into the window: sharp bilinear. Each
// window pixel samples the frame at its own position, moved towards the
// nearest texel centre so that whole texels come out as solid blocks and
// only the window pixels across a texel edge blend the two (by how much of
// the pixel each covers); at whole-number scales that is nearest-neighbour,
// at others no texel is wider than its neighbours by a pixel, and nothing
// is blurred. Then each channel through the gamma ramp.
std::vector<uint32_t> present_fs(uint32_t width, uint32_t height) {
    namespace op = spv::op;
    namespace glsl = spv::glsl;
    spv::Builder b{spv::Stage::Fragment};
    const spv::Id f = b.t_float(), v2 = b.t_vec(2), v4 = b.t_vec(4), s2 = b.t_sampler2d();
    const spv::Id in = b.input(v4, 0);
    const spv::Id uv = b.load(v4, in);
    const spv::Id frame = b.load(s2, b.sampler(s2, 0, 0)), ramp = b.load(s2, b.sampler(s2, 0, 1));
    auto axis = [&](uint32_t k, float size, uint16_t deriv) {
        const spv::Id texel = b.op(op::FMul, f, {b.extract(f, uv, k), b.c_float(size)});
        // Window pixels per texel along this axis, at least 1.
        const spv::Id per_pixel = b.ext(glsl::FAbs, f, {b.op(deriv, f, {texel})});
        const spv::Id scale = b.ext(glsl::FMax, f, {b.op(op::FDiv, f, {b.c_float(1), b.ext(glsl::FMax, f, {per_pixel, b.c_float(1e-6f)})}), b.c_float(1)});
        const spv::Id base = b.ext(glsl::Floor, f, {texel});
        const spv::Id centre = b.op(op::FSub, f, {b.op(op::FSub, f, {texel, base}), b.c_float(0.5f)});
        const spv::Id range = b.op(op::FSub, f, {b.c_float(0.5f), b.op(op::FDiv, f, {b.c_float(0.5f), scale})});
        const spv::Id held = b.ext(glsl::FClamp, f, {centre, b.op(op::FNegate, f, {range}), range});
        const spv::Id moved = b.op(op::FAdd, f, {b.op(op::FMul, f, {b.op(op::FSub, f, {centre, held}), scale}), b.c_float(0.5f)});
        return b.op(op::FDiv, f, {b.op(op::FAdd, f, {base, moved}), b.c_float(size)});
    };
    const spv::Id at = b.op(op::CompositeConstruct, v2, {axis(0, float(width), op::DPdx), axis(1, float(height), op::DPdy)});
    const spv::Id c = b.op(op::ImageSampleImplicitLod, v4, {frame, at});
    auto through = [&](uint32_t k) {
        const spv::Id x = b.op(op::FAdd, f, {b.op(op::FMul, f, {b.extract(f, c, k), b.c_float(255.0f / 256.0f)}), b.c_float(0.5f / 256.0f)});
        const spv::Id pos = b.op(op::CompositeConstruct, v2, {x, b.c_float(0.5f)});
        return b.extract(f, b.op(op::ImageSampleImplicitLod, v4, {ramp, pos}), k);
    };
    const spv::Id out = b.output(v4, 0);
    b.store(out, b.vec4(through(0), through(1), through(2), b.c_float(1)));
    return b.finish({in, out});
}
}  // namespace

void VulkanRenderer::set_gamma_ramp(const uint8_t ramp[3][256]) {
    if (gamma_set_ && std::memcmp(gamma_, ramp, sizeof gamma_) == 0) return;
    std::memcpy(gamma_, ramp, sizeof gamma_);
    gamma_set_ = true;
    if (gamma_texture_) destroy_texture(gamma_texture_);
    TextureData t;
    t.width = 256;
    t.height = 1;
    t.rgba.resize(256 * 4);
    for (uint32_t i = 0; i < 256; ++i) {
        t.rgba[i * 4 + 0] = ramp[0][i];
        t.rgba[i * 4 + 1] = ramp[1][i];
        t.rgba[i * 4 + 2] = ramp[2][i];
        t.rgba[i * 4 + 3] = 255;
    }
    gamma_texture_ = create_texture(t);
}

VkPipeline VulkanRenderer::present_pipeline() {
    if (present_pipeline_ && present_format_ == swap_format_) return present_pipeline_;
    if (!present_set_layout_) {
        VkDescriptorSetLayoutBinding b[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 2;
        sl.pBindings = b;
        check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &present_set_layout_), "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &present_set_layout_;
        check(vkCreatePipelineLayout(device_, &pl, nullptr, &present_layout_), "vkCreatePipelineLayout");
        const std::vector<uint32_t> code[2] = {present_vs(), present_fs(config_.width, config_.height)};
        for (int i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = code[i].size() * 4;
            mi.pCode = code[i].data();
            check(vkCreateShaderModule(device_, &mi, nullptr, &present_shaders_[i]), "vkCreateShaderModule");
        }
    }
    if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = present_shaders_[i];
        stages[i].pName = "main";
    }
    const VkVertexInputBindingDescription binding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &swap_format_;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dyn;
    info.layout = present_layout_;
    check(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &present_pipeline_), "vkCreateGraphicsPipelines");
    present_format_ = swap_format_;
    return present_pipeline_;
}

// The frame into the window's image `index` at (dx, dy, dw × dh), black
// around it, through the gamma ramp (the identity until the title sets one).
void VulkanRenderer::present_through_pass(Target& frame, uint32_t index, int32_t dx, int32_t dy, uint64_t dw, uint64_t dh) {
    if (!gamma_set_) {
        uint8_t identity[3][256];
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 256; ++i) identity[k][i] = uint8_t(i);
        set_gamma_ramp(identity);
    }
    const VkPipeline pl = present_pipeline();
    // The vertices first: a full ring submits, which must come before the window's image is touched.
    const float tri[12] = {-1, -1, 0, 0, 3, -1, 2, 0, -1, 3, 0, 2};
    const VkDeviceSize at = ring_alloc(sizeof tri, 16, tri);
    record();
    transition(frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImage image = swap_images_[index];
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &dep);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &present_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(device_, &ai, &set), "vkAllocateDescriptorSets");
    SamplerDesc linear, nearest;
    linear.u = linear.v = nearest.u = nearest.v = Address::Clamp;
    nearest.mag = nearest.min = Filter::Nearest;
    const VkDescriptorImageInfo images[2] = {{sampler(linear), frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {sampler(nearest), textures_.at(gamma_texture_).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swap_views_[index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = VkClearColorValue{};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, swap_extent_};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cmd_, &info);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    drawn_.valid = false;
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1, &set, 0, nullptr);
    vkCmdBindVertexBuffers(cmd_, 0, 1, &ring_, &at);
    const VkViewport viewport{float(dx), float(dy), float(dw), float(dh), 0, 1};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    const VkRect2D scissor{{dx, dy}, {uint32_t(dw), uint32_t(dh)}};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRendering(cmd_);
    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    submit(acquired_, rendered_[index]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    const VkResult shown = vkQueuePresentKHR(queue_, &pi);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    else check(shown, "vkQueuePresentKHR");
}

// A staging buffer with `data`, freed after the submission.
VkBuffer VulkanRenderer::staging_buffer(const void* data, VkDeviceSize bytes) {
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(bytes, 4);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateBuffer(device_, &bi, nullptr, &staging), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, staging, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, staging, memory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    check(vkMapMemory(device_, memory, 0, bi.size, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, data, size_t(bytes));
    vkUnmapMemory(device_, memory);
    staging_.emplace_back(staging, memory);
    return staging;
}

// The CPU's pixels into a frame buffer (RGBA in, the image's BGRA out).
void VulkanRenderer::write(Target& t, const Image& im) {
    if (im.width != t.width || im.height != t.height) return;
    std::vector<uint8_t> bgra(im.rgba.size());
    for (size_t i = 0; i < im.rgba.size(); i += 4) {
        bgra[i] = im.rgba[i + 2];
        bgra[i + 1] = im.rgba[i + 1];
        bgra[i + 2] = im.rgba[i];
        bgra[i + 3] = im.rgba[i + 3];
    }
    const VkBuffer staging = staging_buffer(bgra.data(), bgra.size());
    const bool was_open = pass_open_;
    end_pass();
    record();
    const VkImageLayout before = t.layout;  // a render target goes back to being sampled
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {t.width, t.height, 1};
    vkCmdCopyBufferToImage(cmd_, staging, t.image, t.layout, 1, &r);
    if (before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(t, before);
    if (was_open) begin_pass();
}

Image VulkanRenderer::read(Target& t) {
    // The read-back buffer, kept and grown to the largest image read.
    const VkDeviceSize bytes = VkDeviceSize(t.width) * t.height * 4;
    if (bytes > readback_size_) {
        if (readback_) {
            wait_idle();
            vkDestroyBuffer(device_, readback_, nullptr);
            vkFreeMemory(device_, readback_memory_, nullptr);
        }
        readback_size_ = std::max(bytes, VkDeviceSize(config_.width) * config_.height * 4);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = readback_size_;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &info, nullptr, &readback_), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, readback_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device_, &alloc, nullptr, &readback_memory_), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, readback_, readback_memory_, 0), "vkBindBufferMemory");
    }
    end_pass();
    record();
    const VkImageLayout before = t.layout;
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {t.width, t.height, 1};
    vkCmdCopyImageToBuffer(cmd_, t.image, t.layout, readback_, 1, &region);
    if (before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(t, before);
    submit();
    wait_idle();

    Image out;
    out.width = t.width;
    out.height = t.height;
    out.rgba.resize(size_t(bytes));
    void* mapped = nullptr;
    check(vkMapMemory(device_, readback_memory_, 0, bytes, 0, &mapped), "vkMapMemory");
    const uint8_t* bgra = static_cast<const uint8_t*>(mapped);
    for (size_t i = 0; i < size_t(bytes); i += 4) {
        out.rgba[i] = bgra[i + 2];
        out.rgba[i + 1] = bgra[i + 1];
        out.rgba[i + 2] = bgra[i];
        out.rgba[i + 3] = bgra[i + 3];
    }
    vkUnmapMemory(device_, readback_memory_);
    return out;
}


// ---------------------------------------------------------------- drawing
constexpr uint32_t kSamplers = 4;
constexpr VkDeviceSize kRingBytes = 32ull << 20;  // a slot's (kSlots of them)

void VulkanRenderer::create_draw_objects() {
    VkDescriptorSetLayoutBinding b[2 + kSamplers]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[i].descriptorCount = 1;
        b[i].stageFlags = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (uint32_t i = 0; i < kSamplers; ++i) {
        b[2 + i].binding = 2 + i;
        b[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2 + i].descriptorCount = 1;
        b[2 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2 + kSamplers;
    sl.pBindings = b;
    check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    check(vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_), "vkCreatePipelineLayout");

    const uint32_t sets = 8192;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * sets},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSamplers * sets}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = sets;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    for (Slot& slot : slots_) check(vkCreateDescriptorPool(device_, &dp, nullptr, &slot.pool), "vkCreateDescriptorPool");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu_, &props);
    uniform_align_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
    ring_size_ = kRingBytes;
    for (Slot& slot : slots_) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = ring_size_;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // also texels on their way to an image
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &bi, nullptr, &slot.ring), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, slot.ring, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device_, &alloc, nullptr, &slot.ring_memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, slot.ring, slot.ring_memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        check(vkMapMemory(device_, slot.ring_memory, 0, ring_size_, 0, &mapped), "vkMapMemory");
        slot.ring_mapped = static_cast<uint8_t*>(mapped);
    }
    use_slot(0);
}

void VulkanRenderer::use_slot(uint32_t i) {
    slot_ = i;
    Slot& sl = slots_[i];
    cmd_ = sl.cmd, fence_ = sl.fence, descriptor_pool_ = sl.pool;
    ring_ = sl.ring, ring_memory_ = sl.ring_memory, ring_mapped_ = sl.ring_mapped;
    acquired_ = sl.acquired;
    ring_used_ = 0;
}

void VulkanRenderer::retire(uint32_t i) {
    Slot& sl = slots_[i];
    if (sl.pending) {
        check(vkWaitForFences(device_, 1, &sl.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(vkResetFences(device_, 1, &sl.fence), "vkResetFences");
        sl.pending = false;
        vkResetDescriptorPool(device_, sl.pool, 0);
        sl.sets.clear();
    }
    for (Texture& t : sl.dead) {
        vkDestroyImageView(device_, t.view, nullptr);
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
    }
    sl.dead.clear();
    for (auto& [b, m] : sl.staging) vkDestroyBuffer(device_, b, nullptr), vkFreeMemory(device_, m, nullptr);
    sl.staging.clear();
}

void VulkanRenderer::wait_idle() {
    for (uint32_t i = 0; i < kSlots; ++i) retire(i);
}

// Space in the ring for this submission; when it's full, the work so far is
// submitted (and waited for) and the ring starts again.
VkDeviceSize VulkanRenderer::ring_alloc(VkDeviceSize bytes, VkDeviceSize align, const void* data) {
    VkDeviceSize at = (ring_used_ + align - 1) / align * align;
    if (at + bytes > ring_size_) {
        if (bytes > ring_size_) fatal("a draw larger than the vertex ring", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        const bool was_open = pass_open_;
        end_pass();
        submit();
        record();
        if (was_open) begin_pass();
        at = 0;
    }
    if (data) std::memcpy(ring_mapped_ + at, data, size_t(bytes));
    ring_used_ = at + bytes;
    return at;
}

VkShaderModule VulkanRenderer::shader(uint64_t key, const std::vector<uint32_t>* code) {
    if (auto it = shaders_.find(key); it != shaders_.end()) return it->second;
    if (!code) fatal("a shader with no code", VK_ERROR_INITIALIZATION_FAILED);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code->size() * 4;
    info.pCode = code->data();
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &info, nullptr, &m), "vkCreateShaderModule");
    shaders_[key] = m;
    return m;
}

VkPipeline VulkanRenderer::pipeline(const DrawCmd& c) {
    PipelineKey k;
    k.vs = c.vs_key, k.fs = c.fs_key, k.inputs = c.inputs, k.primitive = uint8_t(c.primitive);
    k.blend = c.blend, k.write_mask = c.write_mask, k.depth = target_has_depth();
    if (c.blend) {
        k.factors[0] = uint8_t(c.src_color), k.factors[1] = uint8_t(c.dst_color), k.factors[2] = uint8_t(c.src_alpha), k.factors[3] = uint8_t(c.dst_alpha);
        k.ops[0] = uint8_t(c.color_op), k.ops[1] = uint8_t(c.alpha_op);
    }
    if (auto it = pipelines_.find(k); it != pipelines_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shader(c.vs_key, c.vs_code);
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader(c.fs_key, c.fs_code);
    stages[1].pName = "main";

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t n = 0; n < 16; ++n)
        if (c.inputs & (1u << n)) {
            bindings.push_back({n, 16, VK_VERTEX_INPUT_RATE_VERTEX});
            attributes.push_back({n, n, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
        }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();
    static constexpr VkPrimitiveTopology kTopology[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                       VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = kTopology[size_t(c.primitive)];
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = c.blend;
    att.srcColorBlendFactor = VkBlendFactor(c.src_color);
    att.dstColorBlendFactor = VkBlendFactor(c.dst_color);
    att.colorBlendOp = VkBlendOp(c.color_op);
    att.srcAlphaBlendFactor = VkBlendFactor(c.src_alpha);
    att.dstAlphaBlendFactor = VkBlendFactor(c.dst_alpha);
    att.alphaBlendOp = VkBlendOp(c.alpha_op);
    att.colorWriteMask = c.write_mask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                      VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = uint32_t(std::size(dynamic));
    dyn.pDynamicStates = dynamic;
    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    if (k.depth) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = depth_format_;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dyn;
    info.layout = pipeline_layout_;
    VkPipeline p = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &p), "vkCreateGraphicsPipelines");
    pipelines_[k] = p;
    ++unsaved_pipelines_;
    last_pipeline_ = std::chrono::steady_clock::now();
    return p;
}

VkSampler VulkanRenderer::sampler(const SamplerDesc& d) {
    for (auto& [desc, s] : samplers_)
        if (desc == d) return s;
    static constexpr VkSamplerAddressMode kAddress[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = d.mag == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = d.min == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = d.mip == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = kAddress[size_t(d.u)];
    info.addressModeV = kAddress[size_t(d.v)];
    info.addressModeW = kAddress[size_t(d.w)];
    info.mipLodBias = d.lod_bias;
    info.minLod = 0;
    info.maxLod = d.mipmaps ? float(d.max_level) : 0.25f;
    // Core Vulkan has only black or white borders: the nearest of the two.
    const uint32_t a = d.border >> 24, l = ((d.border >> 16 & 0xFF) + (d.border >> 8 & 0xFF) + (d.border & 0xFF)) / 3;
    info.borderColor = a < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : l < 128 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSampler s = VK_NULL_HANDLE;
    check(vkCreateSampler(device_, &info, nullptr, &s), "vkCreateSampler");
    samplers_.emplace_back(d, s);
    return s;
}

uint32_t VulkanRenderer::create_texture(const TextureData& t) {
    const uint32_t layers = t.faces;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = t.dims() == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = t.faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = t.blocks == Blocks::Bc1   ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
                  : t.blocks == Blocks::Bc2 ? VK_FORMAT_BC2_UNORM_BLOCK
                  : t.blocks == Blocks::Bc3 ? VK_FORMAT_BC3_UNORM_BLOCK
                                            : VK_FORMAT_R8G8B8A8_UNORM;
    if (t.blocks != Blocks::None && (!takes_blocks_ || t.dims() == 3)) {
        std::fprintf(stderr, "qilin: renderer: BC blocks for a device or shape that doesn't take them\n");
        std::exit(3);
    }
    info.extent = {t.width, t.height, t.depth};
    info.mipLevels = t.levels;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Texture tex;
    check(vkCreateImage(device_, &info, nullptr, &tex.image), "vkCreateImage");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, tex.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &tex.memory), "vkAllocateMemory");
    check(vkBindImageMemory(device_, tex.image, tex.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = tex.image;
    view.viewType = t.faces == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : t.dims() == 3 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, layers};
    check(vkCreateImageView(device_, &view, nullptr, &tex.view), "vkCreateImageView");

    upload_texels(tex.image, t, true);
    tex.width = t.width, tex.height = t.height, tex.depth = t.depth, tex.levels = t.levels, tex.faces = t.faces, tex.blocks = t.blocks;

    const uint32_t id = next_texture_++;
    textures_[id] = tex;
    return id;
}

bool VulkanRenderer::update_texture(uint32_t id, const TextureData& t) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return false;
    const Texture& x = it->second;
    if (x.width != t.width || x.height != t.height || x.depth != t.depth || x.levels != t.levels || x.faces != t.faces || x.blocks != t.blocks)
        return false;
    upload_texels(x.image, t, false);
    return true;
}

void VulkanRenderer::upload_texels(VkImage image, const TextureData& t, bool fresh) {
    const bool was_open = pass_open_;
    end_pass();
    record();
    // Where the texels come from: the ring, else a buffer freed after the submission.
    VkBuffer from = VK_NULL_HANDLE;
    VkDeviceSize base = 0;
    const VkDeviceSize bytes = std::max<VkDeviceSize>(t.rgba.size(), 4);
    if (bytes <= ring_size_ / 2) {
        base = ring_alloc(bytes, 16, t.rgba.empty() ? nullptr : t.rgba.data());  // may submit first: then this goes in the next
        from = ring_;
    } else {
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device_, &bi, nullptr, &from), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, from, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device_, &alloc, nullptr, &staging_memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, from, staging_memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        check(vkMapMemory(device_, staging_memory, 0, bytes, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, t.rgba.data(), t.rgba.size());
        vkUnmapMemory(device_, staging_memory);
        staging_.emplace_back(from, staging_memory);
    }
    record();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    if (!fresh) {  // after every earlier draw's sampling, in this submission or one before
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, t.faces};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t f = 0; f < t.faces; ++f)
        for (uint32_t l = 0; l < t.levels; ++l) {
            VkBufferImageCopy r{};
            r.bufferOffset = base + t.level_offset(f, l);
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
            r.imageExtent = {t.level_width(l), t.level_height(l), t.level_depth(l)};
            regions.push_back(r);
        }
    vkCmdCopyBufferToImage(cmd_, from, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd_, &dep);
    if (was_open) begin_pass();
}

void VulkanRenderer::destroy_texture(uint32_t id) {
    if (auto rt = render_targets_.find(id); rt != render_targets_.end()) {
        if (id == current_rt_) end_pass(), current_rt_ = 0;
        dead_textures_.push_back({rt->second.image, rt->second.memory, rt->second.view});
        render_targets_.erase(rt);
        return;
    }
    if (auto dt = depth_targets_.find(id); dt != depth_targets_.end()) {
        if (current_rt_ && id == rt_depth_) end_pass(), rt_depth_ = 0;
        dead_textures_.push_back({dt->second.image, dt->second.memory, dt->second.view});
        depth_targets_.erase(dt);
        return;
    }
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    dead_textures_.push_back(it->second);
    textures_.erase(it);
}

// A texture to draw into: B8G8R8A8 as the frame buffers, cleared to zero,
// then readable (draws sample it) until it's drawn into.
void VulkanRenderer::copy_frame_to_target(uint32_t frame_index, uint32_t target_id, const std::vector<CopyRegion>& regions) {
    auto it = render_targets_.find(target_id);
    if (frame_index >= color_.size() || it == render_targets_.end() || regions.empty()) return;
    Target& from = color_[frame_index];
    Target& to = it->second;
    if (target_id == current_rt_) end_pass(), current_rt_ = 0;  // not drawn into while it's copied into
    end_pass();
    record();
    transition(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkImageCopy> copies;
    for (const CopyRegion& r : regions) {
        if (!r.width || !r.height || r.sx + r.width > from.width || r.sy + r.height > from.height || r.dx + r.width > to.width ||
            r.dy + r.height > to.height)
            continue;
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.srcOffset = {int32_t(r.sx), int32_t(r.sy), 0};
        c.dstOffset = {int32_t(r.dx), int32_t(r.dy), 0};
        c.extent = {r.width, r.height, 1};
        copies.push_back(c);
    }
    if (!copies.empty()) vkCmdCopyImage(cmd_, from.image, from.layout, to.image, to.layout, uint32_t(copies.size()), copies.data());
    transition(to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

uint32_t VulkanRenderer::create_render_target(uint32_t width, uint32_t height) {
    Target t = create_target(VK_FORMAT_B8G8R8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, width, height);
    const bool was_open = pass_open_;
    end_pass();
    record();
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, t.image, t.layout, &zero, 1, &range);
    transition(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const uint32_t id = next_texture_++;
    render_targets_[id] = t;
    if (was_open) begin_pass();
    return id;
}

uint32_t VulkanRenderer::create_depth_target(uint32_t width, uint32_t height) {
    Target t = create_target(depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, width, height);
    const bool was_open = pass_open_;
    end_pass();
    record();
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue zero{};
    const VkImageSubresourceRange range{t.aspect, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd_, t.image, t.layout, &zero, 1, &range);
    const uint32_t id = next_texture_++;
    depth_targets_[id] = t;
    if (was_open) begin_pass();
    return id;
}

void VulkanRenderer::set_render_target(uint32_t id, uint32_t depth) {
    if (!id) depth = 0;
    if (id == current_rt_ && depth == rt_depth_) return;
    if (id && !render_targets_.count(id)) {
        std::fprintf(stderr, "qilin: renderer: no render target %u\n", id);
        std::exit(3);
    }
    if (depth) {
        const Target& rt = render_targets_.at(id);
        const Target* dt = depth == kDeviceDepth ? (depth_.image ? &depth_ : nullptr)
                                                  : (depth_targets_.count(depth) ? &depth_targets_.at(depth) : nullptr);
        if (!dt || dt->width < rt.width || dt->height < rt.height) {
            std::fprintf(stderr, "qilin: renderer: depth buffer %u doesn't fit render target %u\n", depth, id);
            std::exit(3);
        }
    }
    end_pass();
    current_rt_ = id;
    rt_depth_ = depth;
}

void VulkanRenderer::draw(const DrawCmd& c) {
    if (c.vertex_count == 0 || (c.indices && c.index_count == 0)) return;
    // A render target sampled here must be ready to read: its last pass
    // ended and the image made readable, outside any pass.
    for (uint32_t i = 0; i < kSamplers; ++i) {
        auto rt = render_targets_.find(c.textures[i]);
        if (rt == render_targets_.end()) continue;
        if (c.textures[i] == current_rt_) {
            std::fprintf(stderr, "qilin: renderer: a draw samples the texture it draws into\n");
            std::exit(3);
        }
        if (rt->second.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            end_pass();
            record();
            transition(rt->second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    const VkPipeline pl = pipeline(c);
    // Room for the whole draw in the ring first, so nothing below starts a
    // new submission halfway through its data.
    {
        VkDeviceSize need = VkDeviceSize(std::popcount(c.inputs)) * (VkDeviceSize(c.vertex_count) * 16 + 16) +
                            (c.indices ? VkDeviceSize(c.index_count) * 4 + 4 : 0) + 2 * (nv2a::kVsUniformVec4s * 16 + uniform_align_);
        if (ring_used_ + need > ring_size_) ring_alloc(need, 16, nullptr), ring_used_ = 0;
    }
    begin_pass();

    // Data into the ring: each attribute, the indices, the two uniform blocks.
    VkBuffer buffers[16];
    VkDeviceSize offsets[16];
    uint32_t first = 0, count = 0;
    for (uint32_t n = 0; n < 16; ++n)
        if (c.inputs & (1u << n)) {
            if (!count) first = n;
            buffers[count] = ring_;
            offsets[count++] = ring_alloc(VkDeviceSize(c.vertex_count) * 16, 16, c.attributes[n]);
        }
    VkDeviceSize index_at = 0;
    if (c.indices) index_at = ring_alloc(VkDeviceSize(c.index_count) * 4, 4, c.indices);
    const VkDeviceSize vs_bytes = nv2a::kVsUniformVec4s * 16, fs_bytes = nv2a::kFsUniformVec4s * 16;
    const uint32_t uniform_offsets[2] = {uint32_t(ring_alloc(vs_bytes, uniform_align_, c.vs_uniforms)),
                                         uint32_t(ring_alloc(fs_bytes, uniform_align_, c.fs_uniforms))};
    // (A ring flush above may have ended the pass: make sure it's open.)
    begin_pass();

    // The descriptor set: the ring's two uniform blocks (dynamic offsets per
    // draw) and the four textures. Draws with the same textures and samplers
    // in a submission share one set (the slot's cache, emptied with its pool).
    DescriptorKey key{};
    VkDescriptorImageInfo images[kSamplers]{};
    for (uint32_t i = 0; i < kSamplers; ++i) {
        VkImageView view = VK_NULL_HANDLE;
        if (auto it = textures_.find(c.textures[i]); it != textures_.end()) view = it->second.view;
        else if (auto rt = render_targets_.find(c.textures[i]); rt != render_targets_.end()) view = rt->second.view;
        if (!view) continue;
        images[i] = {sampler(c.samplers[i]), view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        key.views[i] = view, key.samplers[i] = images[i].sampler;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    auto& cache = slots_[slot_].sets;
    if (auto hit = cache.find(key); hit != cache.end()) {
        set = hit->second;
    } else {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptor_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {  // the pool is spent: start a new submission
            end_pass();
            submit();
            record();
            begin_pass();
            ai.descriptorPool = descriptor_pool_;
            check(vkAllocateDescriptorSets(device_, &ai, &set), "vkAllocateDescriptorSets");
        }
        VkDescriptorBufferInfo ub[2] = {{ring_, 0, vs_bytes}, {ring_, 0, fs_bytes}};
        VkWriteDescriptorSet writes[2 + kSamplers]{};
        uint32_t nw = 0;
        for (uint32_t i = 0; i < 2; ++i) {
            writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[nw].dstSet = set;
            writes[nw].dstBinding = i;
            writes[nw].descriptorCount = 1;
            writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[nw++].pBufferInfo = &ub[i];
        }
        for (uint32_t i = 0; i < kSamplers; ++i) {
            if (!images[i].imageView) continue;
            writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[nw].dstSet = set;
            writes[nw].dstBinding = 2 + i;
            writes[nw].descriptorCount = 1;
            writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[nw++].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_, nw, writes, 0, nullptr);
        slots_[slot_].sets.emplace(key, set);
    }

    // State the command buffer doesn't hold already.
    DrawState& was = drawn_;
    const bool all = !was.valid;
    if (all || was.pipeline != pl) vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl), was.pipeline = pl;
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 2, uniform_offsets);
    // The attributes' buffers in one call over the bindings they span (a
    // binding in a gap gets the ring at 0, unread).
    if (count) {
        const uint32_t last = 31 - uint32_t(std::countl_zero(c.inputs));
        VkBuffer span_buffers[16];
        VkDeviceSize span_offsets[16];
        for (uint32_t n = first, k = 0; n <= last; ++n) {
            span_buffers[n - first] = ring_;
            span_offsets[n - first] = (c.inputs & (1u << n)) ? offsets[k++] : 0;
        }
        vkCmdBindVertexBuffers(cmd_, first, last - first + 1, span_buffers, span_offsets);
    }
    (void)buffers;
    const int32_t tw = int32_t(target().width), th = int32_t(target().height);
    const VkViewport viewport{0, 0, float(tw), float(th), 0, 1};
    if (all || std::memcmp(&was.viewport, &viewport, sizeof viewport) != 0) vkCmdSetViewport(cmd_, 0, 1, &viewport), was.viewport = viewport;
    const int32_t sx0 = std::clamp(c.scissor.x, 0, tw), sy0 = std::clamp(c.scissor.y, 0, th);
    const int32_t sx1 = std::clamp(int32_t(int64_t(c.scissor.x) + c.scissor.w), sx0, tw);
    const int32_t sy1 = std::clamp(int32_t(int64_t(c.scissor.y) + c.scissor.h), sy0, th);
    const VkRect2D scissor{{sx0, sy0}, {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)}};
    if (all || std::memcmp(&was.scissor, &scissor, sizeof scissor) != 0) vkCmdSetScissor(cmd_, 0, 1, &scissor), was.scissor = scissor;
    // Culling a winding: screen space runs y down as in the framebuffer, so
    // "clockwise" is the same on both.
    const VkCullModeFlags cull = c.cull == Cull::None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    const VkFrontFace front = c.cull == Cull::Clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (all || was.cull != cull) vkCmdSetCullMode(cmd_, cull), was.cull = cull;
    if (all || was.front != front) vkCmdSetFrontFace(cmd_, front), was.front = front;
    const bool has_depth = target_has_depth();
    const bool depth_test = has_depth && (c.depth_test || c.depth_write), depth_write = has_depth && c.depth_write;
    const VkCompareOp depth_compare = c.depth_test ? VkCompareOp(c.depth_compare) : VK_COMPARE_OP_ALWAYS;
    if (all || was.depth_test != depth_test) vkCmdSetDepthTestEnable(cmd_, depth_test), was.depth_test = depth_test;
    if (all || was.depth_write != depth_write) vkCmdSetDepthWriteEnable(cmd_, depth_write), was.depth_write = depth_write;
    if (all || was.depth_compare != depth_compare) vkCmdSetDepthCompareOp(cmd_, depth_compare), was.depth_compare = depth_compare;
    const bool stencil_test = has_depth && c.stencil_test;
    if (all || was.stencil_test != stencil_test) vkCmdSetStencilTestEnable(cmd_, stencil_test), was.stencil_test = stencil_test;
    const VkStencilOp sf = VkStencilOp(c.stencil_fail), sp = VkStencilOp(c.stencil_pass), sd = VkStencilOp(c.stencil_depth_fail);
    const VkCompareOp sc = VkCompareOp(c.stencil_compare);
    if (all || was.stencil_fail != sf || was.stencil_pass != sp || was.stencil_depth_fail != sd || was.stencil_compare != sc) {
        vkCmdSetStencilOp(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, sf, sp, sd, sc);
        was.stencil_fail = sf, was.stencil_pass = sp, was.stencil_depth_fail = sd, was.stencil_compare = sc;
    }
    if (all || was.stencil_read != c.stencil_read_mask)
        vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencil_read_mask), was.stencil_read = c.stencil_read_mask;
    if (all || was.stencil_write != c.stencil_write_mask)
        vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencil_write_mask), was.stencil_write = c.stencil_write_mask;
    if (all || was.stencil_ref != c.stencil_ref)
        vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencil_ref), was.stencil_ref = c.stencil_ref;
    if (all || std::memcmp(was.blend, c.blend_constant, sizeof was.blend) != 0)
        vkCmdSetBlendConstants(cmd_, c.blend_constant), std::memcpy(was.blend, c.blend_constant, sizeof was.blend);
    was.valid = true;
    if (c.indices) {
        vkCmdBindIndexBuffer(cmd_, ring_, index_at, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_, c.index_count, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd_, c.vertex_count, 1, 0, 0);
    }
}

}  // namespace

std::unique_ptr<Renderer> Renderer::create(const Config& config, std::string* error) {
    std::string local;
    auto r = std::make_unique<VulkanRenderer>();
    if (!r->init(config, error ? error : &local)) return nullptr;
    return r;
}

}  // namespace qilin::gfx
