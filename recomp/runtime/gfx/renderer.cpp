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
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <map>
#include <unordered_map>

#include "gfx.hpp"
#include "nv2a_shaders.hpp"
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
    Image read_front_buffer() override { return read(color_[front_]); }
    Image read_back_buffer() override { return read(color_[back_]); }
    uint32_t validation_errors() const override { return validation_errors_; }

private:
    bool create_instance(std::string* error);
    bool pick_device(std::string* error);
    void create_device();
    Target create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    uint32_t memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted) const;
    void destroy();

    void record();                                   // makes sure the frame's command buffer is recording
    void transition(Target& t, VkImageLayout to);
    void begin_pass();                               // makes sure the render pass is open, applying pending clears
    void end_pass();
    void submit();                                   // ends the command buffer, submits it and waits for it
    void create_draw_objects();
    VkDeviceSize ring_alloc(VkDeviceSize bytes, VkDeviceSize align, const void* data);  // into this submission's ring
    VkShaderModule shader(uint64_t key, const std::vector<uint32_t>* code);
    VkPipeline pipeline(const DrawCmd& cmd);
    VkSampler sampler(const SamplerDesc& d);
    Image read(Target& t);

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

    std::vector<Target> color_;  // the flip chain: back_ is drawn to, front_ was presented last
    uint32_t back_ = 0, front_ = 0;
    Target depth_;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;

    bool recording_ = false, pass_open_ = false;
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
        auto operator<=>(const PipelineKey&) const = default;
    };
    std::map<PipelineKey, VkPipeline> pipelines_;
    std::vector<std::pair<SamplerDesc, VkSampler>> samplers_;
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::unordered_map<uint32_t, Texture> textures_;
    uint32_t next_texture_ = 1;
    std::vector<Texture> dead_textures_;                                  // freed once the GPU is done with them
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging_;            // likewise
};

bool VulkanRenderer::create_instance(std::string* error) {
    if (volkInitialize() != VK_SUCCESS) {
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
            for (uint32_t k = 0; k < q && family == UINT32_MAX; ++k)
                if (families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = k;
            if (why.empty() && family == UINT32_MAX) why = "no graphics queue";
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
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f13;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &q;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(gpu_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &alloc, &cmd_), "vkAllocateCommandBuffers");
    const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
}

uint32_t VulkanRenderer::memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    fatal("finding a memory type", VK_ERROR_FEATURE_NOT_PRESENT);
}

Target VulkanRenderer::create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Target t;
    t.aspect = aspect;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {config_.width, config_.height, 1};
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
        drop(depth_);
        auto drop_texture = [&](Texture& t) {
            if (t.view) vkDestroyImageView(device_, t.view, nullptr);
            if (t.image) vkDestroyImage(device_, t.image, nullptr);
            if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        };
        for (auto& [id, t] : textures_) drop_texture(t);
        for (Texture& t : dead_textures_) drop_texture(t);
        for (auto& [b, m] : staging_) vkDestroyBuffer(device_, b, nullptr), vkFreeMemory(device_, m, nullptr);
        for (auto& [k, pl] : pipelines_) vkDestroyPipeline(device_, pl, nullptr);
        for (auto& [k, m] : shaders_) vkDestroyShaderModule(device_, m, nullptr);
        for (auto& [d, smp] : samplers_) vkDestroySampler(device_, smp, nullptr);
        if (ring_) vkDestroyBuffer(device_, ring_, nullptr);
        if (ring_memory_) vkFreeMemory(device_, ring_memory_, nullptr);
        if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
        if (readback_memory_) vkFreeMemory(device_, readback_memory_, nullptr);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
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
    Target& back = color_[back_];
    transition(back, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = back.view;
    color.imageLayout = back.layout;
    color.loadOp = pending_.color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = pending_.color_value;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {config_.width, config_.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    if (depth_.image) {
        transition(depth_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth.imageView = stencil.imageView = depth_.view;
        depth.imageLayout = stencil.imageLayout = depth_.layout;
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
    if (!depth_.image) flags &= ~uint32_t(kClearDepth | kClearStencil);
    if (!(flags & (kClearColor | kClearDepth | kClearStencil))) return;
    // Clip the rectangles to the buffer; no rectangles means all of it.
    std::vector<VkClearRect> clipped;
    bool whole = count == 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t x0 = std::max<int64_t>(rects[i].x, 0), y0 = std::max<int64_t>(rects[i].y, 0);
        const int64_t x1 = std::min<int64_t>(int64_t(rects[i].x) + rects[i].w, config_.width);
        const int64_t y1 = std::min<int64_t>(int64_t(rects[i].y) + rects[i].h, config_.height);
        if (x1 <= x0 || y1 <= y0) continue;
        whole = whole || (x0 == 0 && y0 == 0 && x1 == config_.width && y1 == config_.height);
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
    if (whole) clipped.assign(1, {{{0, 0}, {config_.width, config_.height}}, 0, 1});
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

void VulkanRenderer::submit() {
    if (!recording_) return;
    check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    recording_ = false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd_;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &cb;
    check(vkQueueSubmit2(queue_, 1, &info, fence_), "vkQueueSubmit2");
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    // The GPU is done with everything this submission used.
    ring_used_ = 0;
    if (descriptor_pool_) vkResetDescriptorPool(device_, descriptor_pool_, 0);
    for (Texture& t : dead_textures_) {
        vkDestroyImageView(device_, t.view, nullptr);
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
    }
    dead_textures_.clear();
    for (auto& [b, m] : staging_) vkDestroyBuffer(device_, b, nullptr), vkFreeMemory(device_, m, nullptr);
    staging_.clear();
}

void VulkanRenderer::present() {
    end_pass();
    submit();
    front_ = back_;
    back_ = (back_ + 1) % uint32_t(color_.size());
    ++frames_;
}

Image VulkanRenderer::read(Target& t) {
    const VkDeviceSize bytes = VkDeviceSize(config_.width) * config_.height * 4;
    if (!readback_) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
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
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {config_.width, config_.height, 1};
    vkCmdCopyImageToBuffer(cmd_, t.image, t.layout, readback_, 1, &region);
    submit();

    Image out;
    out.width = config_.width;
    out.height = config_.height;
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
constexpr VkDeviceSize kRingBytes = 64ull << 20;

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
    check(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptor_pool_), "vkCreateDescriptorPool");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu_, &props);
    uniform_align_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
    ring_size_ = kRingBytes;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = ring_size_;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &bi, nullptr, &ring_), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, ring_, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &ring_memory_), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, ring_, ring_memory_, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    check(vkMapMemory(device_, ring_memory_, 0, ring_size_, 0, &mapped), "vkMapMemory");
    ring_mapped_ = static_cast<uint8_t*>(mapped);
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
    k.blend = c.blend, k.write_mask = c.write_mask;
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
    if (depth_.image) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = depth_format_;
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
    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &p), "vkCreateGraphicsPipelines");
    pipelines_[k] = p;
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
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
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

    // The texels through a staging buffer, freed after the submission.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(t.rgba.size(), 4);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateBuffer(device_, &bi, nullptr, &staging), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(device_, staging, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &staging_memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, staging, staging_memory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    check(vkMapMemory(device_, staging_memory, 0, bi.size, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, t.rgba.data(), t.rgba.size());
    vkUnmapMemory(device_, staging_memory);
    staging_.emplace_back(staging, staging_memory);

    const bool was_open = pass_open_;
    end_pass();
    record();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image;
    b.subresourceRange = view.subresourceRange;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t f = 0; f < t.faces; ++f)
        for (uint32_t l = 0; l < t.levels; ++l) {
            VkBufferImageCopy r{};
            r.bufferOffset = t.level_offset(f, l);
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
            r.imageExtent = {t.level_width(l), t.level_height(l), t.level_depth(l)};
            regions.push_back(r);
        }
    vkCmdCopyBufferToImage(cmd_, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd_, &dep);
    if (was_open) begin_pass();

    const uint32_t id = next_texture_++;
    textures_[id] = tex;
    return id;
}

void VulkanRenderer::destroy_texture(uint32_t id) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    dead_textures_.push_back(it->second);
    textures_.erase(it);
}

void VulkanRenderer::draw(const DrawCmd& c) {
    if (c.vertex_count == 0 || (c.indices && c.index_count == 0)) return;
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

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {  // the pool is spent: start a new submission
        end_pass();
        submit();
        record();
        begin_pass();
        check(vkAllocateDescriptorSets(device_, &ai, &set), "vkAllocateDescriptorSets");
    }
    VkDescriptorBufferInfo ub[2] = {{ring_, 0, vs_bytes}, {ring_, 0, fs_bytes}};
    VkDescriptorImageInfo images[kSamplers]{};
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
        auto it = textures_.find(c.textures[i]);
        if (it == textures_.end()) continue;
        images[i] = {sampler(c.samplers[i]), it->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[nw].dstSet = set;
        writes[nw].dstBinding = 2 + i;
        writes[nw].descriptorCount = 1;
        writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[nw++].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, nw, writes, 0, nullptr);

    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 2, uniform_offsets);
    for (uint32_t n = 0, k = 0; n < 16; ++n)
        if (c.inputs & (1u << n)) vkCmdBindVertexBuffers(cmd_, n, 1, &buffers[k], &offsets[k]), ++k;
    (void)first;
    const VkViewport viewport{0, 0, float(config_.width), float(config_.height), 0, 1};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    const int32_t sx0 = std::clamp(c.scissor.x, 0, int32_t(config_.width)), sy0 = std::clamp(c.scissor.y, 0, int32_t(config_.height));
    const int32_t sx1 = std::clamp(int32_t(int64_t(c.scissor.x) + c.scissor.w), sx0, int32_t(config_.width));
    const int32_t sy1 = std::clamp(int32_t(int64_t(c.scissor.y) + c.scissor.h), sy0, int32_t(config_.height));
    const VkRect2D scissor{{sx0, sy0}, {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)}};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    // Culling a winding: screen space runs y down as in the framebuffer, so
    // "clockwise" is the same on both.
    vkCmdSetCullMode(cmd_, c.cull == Cull::None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFace(cmd_, c.cull == Cull::Clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    const bool has_depth = depth_.image != VK_NULL_HANDLE;
    vkCmdSetDepthTestEnable(cmd_, has_depth && (c.depth_test || c.depth_write));
    vkCmdSetDepthWriteEnable(cmd_, has_depth && c.depth_write);
    vkCmdSetDepthCompareOp(cmd_, c.depth_test ? VkCompareOp(c.depth_compare) : VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cmd_, has_depth && c.stencil_test);
    vkCmdSetStencilOp(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(c.stencil_fail), VkStencilOp(c.stencil_pass),
                      VkStencilOp(c.stencil_depth_fail), VkCompareOp(c.stencil_compare));
    vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencil_read_mask);
    vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencil_write_mask);
    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, c.stencil_ref);
    vkCmdSetBlendConstants(cmd_, c.blend_constant);
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
