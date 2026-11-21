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
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <map>
#include <utility>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <span>
#include <unordered_map>

#include "fsr/fsr_constants.hpp"
#include "gfx.hpp"
#include "nv2a_shaders.hpp"
#include "spirv.hpp"
#include "texture.hpp"
#include "vsh.hpp"
#include "volk.h"

// Vulkan structures are zero-initialised by naming only their sType
// (VkFoo f{VK_STRUCTURE_TYPE_FOO}), which -Wextra would flag in GCC and Clang.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace qilin::gfx {
namespace fsr_spirv {
#include "fsr/fsr_spirv.inc"
}  // namespace fsr_spirv
namespace depth_pack_spirv {
#include "depth/depth_pack_spirv.inc"
}  // namespace depth_pack_spirv
namespace smaa_spirv {
#include "smaa/smaa_spirv.inc"
}  // namespace smaa_spirv
namespace smaa_tables {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../../../third_party/smaa/AreaTex.h"
#include "../../../third_party/smaa/SearchTex.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}  // namespace smaa_tables
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
    uint32_t width = 0, height = 0;    // the image's, in pixels
    uint32_t sx = 1, sy = 1;           // the render resolution's scale: the game's size is width / sx by height / sy
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
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
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:  // sampled by draws, or by FSR's passes
            stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  // a depth target sampled (pack_depth)
            stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:  // written by FSR's passes
            stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
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
    bool arena_alloc(uint64_t bytes, uint64_t* offset, void** write) override;
    void arena_free(uint64_t offset, uint64_t bytes) override;
    void draw(const DrawCmd& cmd) override;
    uint32_t create_render_target(uint32_t width, uint32_t height) override;
    void copy_frame_to_target(uint32_t frame_index, uint32_t target_id, const std::vector<CopyRegion>& regions) override;
    uint32_t create_depth_target(uint32_t width, uint32_t height) override;
    void set_render_target(uint32_t id, uint32_t depth) override;
    bool pack_depth(uint32_t depth_target, uint32_t render_target) override;
    Image read_front_buffer() override { return read(color_[front_]); }
    void present_again() override;
    bool acquire_early() override;
    void set_picture(const Picture& p) override {
        if (p.vsync != picture_.vsync || p.low_latency != picture_.low_latency) swap_stale_ = true;  // a new swapchain
        picture_ = p;
    }
    void set_anisotropy(uint32_t level) override { anisotropy_ = std::clamp<uint32_t>(level, 1, max_anisotropy_); }
    uint32_t max_anisotropy() const override { return max_anisotropy_; }
    bool overlay(Overlay& out) const override {
        if (!surface_ || !swapchain_ || !present_pass_) return false;
        out.instance = instance_;
        out.physical_device = gpu_;
        out.device = device_;
        out.queue_family = queue_family_;
        out.queue = queue_;
        out.format = uint32_t(swap_format_);
        out.images = uint32_t(swap_images_.size());
        out.min_images = std::max<uint32_t>(2, out.images - 1);
        return true;
    }
    void set_overlay(std::function<void(void*)> draw) override { overlay_ = std::move(draw); }
    const Picture& picture() const override { return picture_; }
    Picture picture_;
    uint32_t anisotropy_ = 1, max_anisotropy_ = 1;
    std::function<void(void*)> overlay_;
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
    Costs take_costs() override { return std::exchange(costs_, {}); }
    void set_resolution_scale(uint32_t x, uint32_t y) override;
    uint32_t resolution_scale_x() const override { return config_.scale_x; }
    uint32_t resolution_scale_y() const override { return config_.scale_y; }

private:
    bool create_instance(std::string* error);
    bool pick_device(std::string* error);
    void create_device();
    // An image of the game's width × height (0: the frame buffers'), made at the render resolution.
    Target create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width = 0, uint32_t height = 0);
    Target create_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width, uint32_t height);
    uint32_t present_scale_[2] = {0, 0};  // the render resolution the present pass's shaders were made for
    // FSR 1 (fsr/): EASU scales the frame to the size it's shown at, RCAS
    // sharpens that, both compute passes into images of that size, which
    // the present pass then samples one to one.
    VkDescriptorSetLayout fsr_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout fsr_layout_ = VK_NULL_HANDLE;
    VkPipeline fsr_pipelines_[2] = {};  // EASU, RCAS
    Target fsr_images_[2];              // their outputs
    void fsr_objects(uint32_t width, uint32_t height);  // made, at this size, before the frame's commands
    Target& run_fsr(Target& frame);
    // SMAA 1x (smaa/): edge detection, blending weights and neighbourhood
    // blending, three full-screen passes over the frame at its own size,
    // before FSR and the present pass.
    VkDescriptorSetLayout smaa_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout smaa_layout_ = VK_NULL_HANDLE;
    VkPipeline smaa_pipelines_[3] = {};
    Target smaa_images_[3];  // the edges, the blending weights, the result
    uint32_t smaa_area_ = 0, smaa_search_ = 0;  // SMAA's lookup textures
    void smaa_objects(const Target& frame);
    Target& run_smaa(Target& frame);
    // Depth targets packed as the console's D24S8 words (pack_depth): one
    // full-screen pass reading the depth and stencil aspects.
    VkDescriptorSetLayout pack_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pack_layout_ = VK_NULL_HANDLE;
    VkPipeline pack_pipeline_ = VK_NULL_HANDLE;
    bool depth_sampled_ = false;  // the depth format can be sampled
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
    // The window image acquire_early took, with the semaphore its acquire
    // signals (the next present uses both; none: -1).
    int64_t early_index_ = -1;
    VkSemaphore early_signal_ = VK_NULL_HANDLE;
    VkSemaphore present_wait_ = VK_NULL_HANDLE;  // what the present's submission waits on
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
    bool wide_lines() const override { return wide_lines_; }
    bool wide_lines_ = false;
    float line_width_max_ = 1;
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
        bool bias_on = false;
        float bias = 0, slope = 0, line = 0;
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
        uint32_t inputs = 0, constant_inputs = 0;
        uint8_t primitive = 0, blend = 0, write_mask = 0, factors[4] = {}, ops[2] = {};
        uint8_t depth = 0;  // the target has a depth-stencil attachment
        auto operator<=>(const PipelineKey&) const = default;
    };
    std::map<PipelineKey, VkPipeline> pipelines_;
    Costs costs_;
    VkPipeline make_pipeline(const PipelineKey& k, VkShaderModule vs, VkShaderModule fs) const;
    // The pipeline library: what each pipeline was made from (the vertex
    // program and the combiners, not their SPIR-V, which changes with the
    // translators), kept in a file beside the pipeline cache. A thread
    // makes them all again from it at the start, the shaders translated
    // afresh, so a draw finds its pipeline made rather than waiting for
    // the GPU's compiler (a stutter the first time each is seen).
    struct Library {
        std::filesystem::path path;
        std::ofstream out;  // new entries, appended
        // Each pipeline's shaders' sources (library_sources), by key, in
        // the order first seen; stale: one changed, so the file is
        // written again whole at the end.
        std::map<PipelineKey, std::string> sources;
        std::vector<PipelineKey> order;
        bool stale = false;
        std::thread worker;
        std::mutex lock;
        std::condition_variable done;
        std::map<PipelineKey, VkPipeline> made;  // by the worker, not yet taken
        std::set<PipelineKey> claimed;           // made, or being made, by the draws
        bool busy = false;                       // the worker is making `current`
        PipelineKey current{};
        std::atomic<bool> stop{false};
    } library_;
    void write_library_entry(std::ostream& out, const PipelineKey& k, const std::string& sources);
    void start_library();
    void record_in_library(const PipelineKey& k, const DrawCmd& c);
    void stop_library();
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
        std::vector<std::pair<uint64_t, uint64_t>> arena_freed;  // arena ranges given back while this submission was recorded
        bool pending = false;
        // Acquire semaphores this submission waits on: free again once it's done.
        std::vector<VkSemaphore> acquire_waited;
    };
    Slot slots_[kSlots];
    // Semaphores for vkAcquireNextImageKHR, each used again only when the
    // submission that waited on it has finished. (Before v0.42.2 an
    // acquire used the recording slot's own, and an image taken early with
    // one slot's and presented from the other, after a flush in between,
    // left that semaphore's wait still queued when its slot acquired again.)
    std::vector<VkSemaphore> acquire_free_;
    VkSemaphore take_acquire_semaphore();
    // The vertex arena: one buffer, first-fit over its free ranges.
    VkBuffer arena_ = VK_NULL_HANDLE;
    VkDeviceMemory arena_memory_ = VK_NULL_HANDLE;
    uint8_t* arena_mapped_ = nullptr;
    std::map<uint64_t, uint64_t> arena_free_;  // offset → bytes, merged with neighbours
    void arena_release(uint64_t offset, uint64_t bytes);
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
    void drop_early_image();
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
            depth_sampled_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
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
    // Lines wider than a pixel (D3D's LineWidth), where the device has them.
    f2.features.wideLines = have.wideLines;
    // Anisotropic filtering (a setting), where the device has it.
    f2.features.samplerAnisotropy = have.samplerAnisotropy;
    if (have.samplerAnisotropy) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu_, &props);
        max_anisotropy_ = std::clamp<uint32_t>(uint32_t(props.limits.maxSamplerAnisotropy), 1, 16);
    }
    wide_lines_ = have.wideLines;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu_, &props);
        line_width_max_ = wide_lines_ ? props.limits.lineWidthRange[1] : 1.0f;
    }
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
    Target t = create_image(format, usage, aspect, (width ? width : config_.width) * config_.scale_x,
                            (height ? height : config_.height) * config_.scale_y);
    t.sx = config_.scale_x;
    t.sy = config_.scale_y;
    return t;
}

Target VulkanRenderer::create_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width,
                                    uint32_t height) {
    Target t;
    t.aspect = aspect;
    t.format = format;
    t.usage = usage;
    t.width = width;
    t.height = height;
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
        config_.back_buffers > 3 || config_.scale_x - 1 > 7 || config_.scale_y - 1 > 7) {
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
    start_library();
    return true;
}

void VulkanRenderer::destroy() {
    if (device_) {
        stop_library();
        vkDeviceWaitIdle(device_);
        auto drop = [&](Target& t) {
            if (t.view) vkDestroyImageView(device_, t.view, nullptr);
            if (t.image) vkDestroyImage(device_, t.image, nullptr);
            if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        };
        for (Target& t : color_) drop(t);
        for (Target& t : fsr_images_) drop(t);
        for (Target& t : smaa_images_) drop(t);
        for (VkPipeline p : smaa_pipelines_) if (p) vkDestroyPipeline(device_, p, nullptr);
        if (pack_pipeline_) vkDestroyPipeline(device_, pack_pipeline_, nullptr);
        if (pack_layout_) vkDestroyPipelineLayout(device_, pack_layout_, nullptr);
        if (pack_set_layout_) vkDestroyDescriptorSetLayout(device_, pack_set_layout_, nullptr);
        if (smaa_layout_) vkDestroyPipelineLayout(device_, smaa_layout_, nullptr);
        if (smaa_set_layout_) vkDestroyDescriptorSetLayout(device_, smaa_set_layout_, nullptr);
        for (VkPipeline p : fsr_pipelines_) if (p) vkDestroyPipeline(device_, p, nullptr);
        if (fsr_layout_) vkDestroyPipelineLayout(device_, fsr_layout_, nullptr);
        if (fsr_set_layout_) vkDestroyDescriptorSetLayout(device_, fsr_set_layout_, nullptr);
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
        for (VkSemaphore sem : acquire_free_) vkDestroySemaphore(device_, sem, nullptr);
        acquire_free_.clear();
        if (early_signal_) vkDestroySemaphore(device_, early_signal_, nullptr), early_signal_ = VK_NULL_HANDLE;
        for (Slot& sl : slots_) {
            if (sl.ring) vkDestroyBuffer(device_, sl.ring, nullptr);
            if (sl.ring_memory) vkFreeMemory(device_, sl.ring_memory, nullptr);
            if (sl.pool) vkDestroyDescriptorPool(device_, sl.pool, nullptr);
            if (sl.fence) vkDestroyFence(device_, sl.fence, nullptr);
            if (sl.acquired) vkDestroySemaphore(device_, sl.acquired, nullptr);
            for (VkSemaphore sem : sl.acquire_waited) vkDestroySemaphore(device_, sem, nullptr);
        }
        if (arena_) vkDestroyBuffer(device_, arena_, nullptr);
        if (arena_memory_) vkFreeMemory(device_, arena_memory_, nullptr);
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
    const uint32_t tw = target().width, th = target().height, sx = target().sx, sy = target().sy;
    // Clip the rectangles (the game's pixels) to the buffer; no rectangles means all of it.
    std::vector<VkClearRect> clipped;
    bool whole = count == 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t x0 = std::max<int64_t>(int64_t(rects[i].x) * sx, 0), y0 = std::max<int64_t>(int64_t(rects[i].y) * sy, 0);
        const int64_t x1 = std::min<int64_t>((int64_t(rects[i].x) + rects[i].w) * sx, tw);
        const int64_t y1 = std::min<int64_t>((int64_t(rects[i].y) + rects[i].h) * sy, th);
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
        // The acquired image is first touched by a layout change at either of
        // these stages (the present pass's colour output, or a copy): both wait.
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
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
    if (wait) done.acquire_waited.push_back(wait);
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
    if (library_.out.is_open()) library_.out.flush();
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

void VulkanRenderer::present_again() {
    if (!surface_) return;
    end_pass();
    show(color_[front_]);
}

// The image acquire_early took isn't wanted (the swapchain is made again):
// its semaphore will be signalled, and a semaphore must be waited on before
// it's signalled again, so a submission of nothing waits on it.
void VulkanRenderer::drop_early_image() {
    if (early_index_ < 0) return;
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    w.semaphore = early_signal_;
    w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.waitSemaphoreInfoCount = 1;
    info.pWaitSemaphoreInfos = &w;
    check(vkQueueSubmit2(queue_, 1, &info, VK_NULL_HANDLE), "vkQueueSubmit2");
    check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");  // the wait has run: the semaphore is free again (a swapchain made again is rare)
    acquire_free_.push_back(early_signal_);
    early_index_ = -1;
    early_signal_ = VK_NULL_HANDLE;
}

VkSemaphore VulkanRenderer::take_acquire_semaphore() {
    if (!acquire_free_.empty()) {
        const VkSemaphore sem = acquire_free_.back();
        acquire_free_.pop_back();
        return sem;
    }
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem = VK_NULL_HANDLE;
    check(vkCreateSemaphore(device_, &info, nullptr, &sem), "vkCreateSemaphore");
    return sem;
}

bool VulkanRenderer::acquire_early() {
    if (!surface_ || !swapchain_ || swap_stale_ || early_index_ >= 0) return false;
    uint32_t w = 0, h = 0;
    if (config_.surface.size) config_.surface.size(&w, &h);
    if (w != swap_extent_.width || h != swap_extent_.height) return false;  // the next present makes the swapchain again
    uint32_t index = 0;
    const VkSemaphore sem = take_acquire_semaphore();
    const VkResult got = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
    if (got == VK_ERROR_OUT_OF_DATE_KHR) {
        acquire_free_.push_back(sem);  // nothing was acquired: it won't be signalled
        swap_stale_ = true;
        return false;
    }
    if (got == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    else check(got, "vkAcquireNextImageKHR");
    early_index_ = index;
    early_signal_ = sem;
    return true;
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
    // Vsync: FIFO, at the display's refresh. Off: MAILBOX (no tearing, the
    // newest frame at each refresh), else IMMEDIATE (may tear), else FIFO.
    auto has = [&](VkPresentModeKHR m) { return std::ranges::find(modes, m) != modes.end(); };
    const VkPresentModeKHR mode = picture_.vsync                       ? VK_PRESENT_MODE_FIFO_KHR
                                  : has(VK_PRESENT_MODE_MAILBOX_KHR)   ? VK_PRESENT_MODE_MAILBOX_KHR
                                  : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                       : VK_PRESENT_MODE_FIFO_KHR;
    // One more image than the least the surface takes lets the CPU run a
    // frame ahead of the display. With vsync and low latency, the least
    // (two at least): acquiring an image then waits for the display, so at
    // most one finished frame waits to be shown, and the game reads its
    // input just after a refresh. MAILBOX keeps the extra one, to have an
    // image to replace.
    uint32_t count = caps.minImageCount + 1;
    if (mode == VK_PRESENT_MODE_FIFO_KHR && picture_.low_latency) count = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    drop_early_image();
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
    if (early_index_ >= 0) {  // taken already (acquire_early)
        index = uint32_t(early_index_);
        present_wait_ = early_signal_;
        early_index_ = -1;
        early_signal_ = VK_NULL_HANDLE;
    } else {
        const VkSemaphore sem = take_acquire_semaphore();
        const VkResult got = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
        if (got == VK_ERROR_OUT_OF_DATE_KHR) {
            acquire_free_.push_back(sem);
            swap_stale_ = true;
            return submit();
        }
        if (got == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
        else check(got, "vkAcquireNextImageKHR");
        present_wait_ = sem;
    }

    // The frame's shape on screen: its aspect (4:3, or 16:9 for a
    // widescreen game), as a width over a height of 1000.
    const uint64_t fh0 = config_.height, fw0 = config_.width, sw0 = swap_extent_.width, sh0 = swap_extent_.height;
    const uint64_t aw = uint64_t(std::lround(std::clamp(picture_.aspect, 0.5f, 4.0f) * 1000.0f)), ah = 1000;
    uint64_t dw0 = sw0, dh0 = sh0;
    if (picture_.scaling != Picture::Scaling::Stretch) {
        if (sw0 * ah > sh0 * aw) dw0 = std::max<uint64_t>(1, sh0 * aw / ah);  // wider than the picture: bars at the sides
        else dh0 = std::max<uint64_t>(1, sw0 * ah / aw);
        if (picture_.scaling == Picture::Scaling::Integer && dh0 >= fh0) {  // whole multiples of the frame's lines
            dh0 = fh0 * (dh0 / fh0);
            dw0 = std::min<uint64_t>(sw0, std::max<uint64_t>(1, dh0 * aw / ah));
        }
    }
    (void)fw0;
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

    const uint64_t fw = frame.width, fh = frame.height, dw = dw0, dh = dh0;
    const int32_t dx = int32_t((sw0 - dw) / 2), dy = int32_t((sh0 - dh) / 2);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(fw), int32_t(fh), 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + int32_t(dw), dy + int32_t(dh), 1};
    vkCmdBlitImage(cmd_, frame.image, frame.layout, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   picture_.smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);  // before the semaphore present waits on
    submit(present_wait_, rendered_[index]);

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
// The picture's settings come in as a second attribute, the same at each
// vertex: (smooth, CRT, scanlines, mask).
std::vector<uint32_t> present_vs() {
    spv::Builder b{spv::Stage::Vertex};
    const spv::Id f = b.t_float(), v4 = b.t_vec(4);
    const spv::Id in = b.input(v4, 0), params_in = b.input(v4, 1);
    const spv::Id v = b.load(v4, in);
    const spv::Id pos = b.builtin_var(spv::Storage::Output, v4, spv::builtin::Position);
    b.store(pos, b.vec4(b.extract(f, v, 0), b.extract(f, v, 1), b.c_float(0), b.c_float(1)));
    const spv::Id out = b.output(v4, 0), params_out = b.output(v4, 1);
    b.store(out, b.vec4(b.extract(f, v, 2), b.extract(f, v, 3), b.c_float(0), b.c_float(0)));
    b.store(params_out, b.load(v4, params_in));
    return b.finish({in, params_in, pos, out, params_out});
}
// The frame, filtered, then each channel through its table (texel i at (i + 0.5) / 256).
// The frame (width × height) scaled into the window: sharp bilinear. Each
// window pixel samples the frame at its own position, moved towards the
// nearest texel centre so that whole texels come out as solid blocks and
// only the window pixels across a texel edge blend the two (by how much of
// the pixel each covers); at whole-number scales that is nearest-neighbour,
// at others no texel is wider than its neighbours by a pixel, and nothing
// is blurred. Then each channel through the gamma ramp.
// width × height: the frame image's size (sharp scaling snaps to its
// texels); lines: the game's frame lines (the CRT's scanlines).
std::vector<uint32_t> present_fs(uint32_t width, uint32_t height, uint32_t lines) {
    namespace op = spv::op;
    namespace glsl = spv::glsl;
    spv::Builder b{spv::Stage::Fragment};
    const spv::Id f = b.t_float(), v2 = b.t_vec(2), v4 = b.t_vec(4), s2 = b.t_sampler2d();
    const spv::Id frag_coord = b.builtin_var(spv::Storage::Input, v4, spv::builtin::FragCoord);
    const spv::Id in = b.input(v4, 0), params_in = b.input(v4, 1);
    b.decorate(params_in, spv::deco::Flat);
    const spv::Id uv = b.load(v4, in), params = b.load(v4, params_in);
    const spv::Id smooth = b.extract(f, params, 0), crt = b.extract(f, params, 1);
    const spv::Id scanlines = b.extract(f, params, 2), mask = b.extract(f, params, 3);
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
    const spv::Id sharp = b.op(op::CompositeConstruct, v2, {axis(0, float(width), op::DPdx), axis(1, float(height), op::DPdy)});
    // Smooth: the frame's own position, bilinear (also FSR's output, already
    // the shown size); else sharp, as for the CRT, its raster lines below.
    const spv::Id plain = b.shuffle(v2, uv, uv, {0, 1});
    const spv::Id at = b.ext(glsl::FMix, v2, {sharp, plain, b.splat(smooth, 2)});
    spv::Id c = b.op(op::ImageSampleImplicitLod, v4, {frame, at});
    // A CRT: each of the frame's lines lit across its middle and dark at its
    // edges (by `scanlines`), and the window's columns through red, green and
    // blue phosphor stripes in turn (by `mask`), brightened back to the
    // frame's average level.
    {
        const spv::Id line = b.op(op::FSub, f, {b.ext(glsl::Fract, f, {b.op(op::FMul, f, {b.extract(f, uv, 1), b.c_float(float(lines))})}), b.c_float(0.5f)});
        const spv::Id d2 = b.op(op::FMul, f, {b.c_float(4), b.op(op::FMul, f, {line, line})});
        const spv::Id scan = b.op(op::FSub, f, {b.c_float(1), b.op(op::FMul, f, {scanlines, d2})});
        const spv::Id coord = b.load(v4, frag_coord);
        const spv::Id column = b.op(op::FSub, f, {b.extract(f, coord, 0), b.c_float(0.5f)});
        const spv::Id third = b.op(op::FMul, f, {column, b.c_float(1.0f / 3.0f)});
        const spv::Id phase = b.op(op::FMul, f, {b.ext(glsl::Fract, f, {third}), b.c_float(3)});  // 0, 1, 2
        auto stripe = [&](float k) {  // 1 on its own column, 1 − mask on the others
            const spv::Id on = b.ext(glsl::FClamp, f, {b.op(op::FSub, f, {b.c_float(1), b.ext(glsl::FAbs, f, {b.op(op::FSub, f, {phase, b.c_float(k)})})}), b.c_float(0), b.c_float(1)});
            return b.op(op::FAdd, f, {b.op(op::FSub, f, {b.c_float(1), mask}), b.op(op::FMul, f, {mask, on})});
        };
        const spv::Id level = b.op(op::FMul, f, {b.op(op::FSub, f, {b.c_float(1), b.op(op::FDiv, f, {scanlines, b.c_float(3)})}),
                                                 b.op(op::FSub, f, {b.c_float(1), b.op(op::FMul, f, {mask, b.c_float(2.0f / 3.0f)})})});
        const spv::Id gain = b.op(op::FDiv, f, {scan, b.ext(glsl::FMax, f, {level, b.c_float(0.05f)})});
        const spv::Id tv = b.vec4(b.op(op::FMul, f, {b.extract(f, c, 0), b.op(op::FMul, f, {gain, stripe(0)})}),
                                  b.op(op::FMul, f, {b.extract(f, c, 1), b.op(op::FMul, f, {gain, stripe(1)})}),
                                  b.op(op::FMul, f, {b.extract(f, c, 2), b.op(op::FMul, f, {gain, stripe(2)})}), b.extract(f, c, 3));
        c = b.ext(glsl::FClamp, v4, {b.ext(glsl::FMix, v4, {c, tv, b.splat(crt)}), b.splat(b.c_float(0)), b.splat(b.c_float(1))});
    }
    auto through = [&](uint32_t k) {
        const spv::Id x = b.op(op::FAdd, f, {b.op(op::FMul, f, {b.extract(f, c, k), b.c_float(255.0f / 256.0f)}), b.c_float(0.5f / 256.0f)});
        const spv::Id pos = b.op(op::CompositeConstruct, v2, {x, b.c_float(0.5f)});
        return b.extract(f, b.op(op::ImageSampleImplicitLod, v4, {ramp, pos}), k);
    };
    const spv::Id out = b.output(v4, 0);
    b.store(out, b.vec4(through(0), through(1), through(2), b.c_float(1)));
    return b.finish({in, params_in, frag_coord, out});
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
    const bool rescaled = present_scale_[0] != config_.scale_x || present_scale_[1] != config_.scale_y;
    if (present_pipeline_ && present_format_ == swap_format_ && !rescaled) return present_pipeline_;
    if (rescaled && present_shaders_[1]) {  // the fragment shader samples the frame at its size
        vkDestroyShaderModule(device_, present_shaders_[1], nullptr);
        const std::vector<uint32_t> fs = present_fs(config_.width * config_.scale_x, config_.height * config_.scale_y, config_.height);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = fs.size() * 4;
        mi.pCode = fs.data();
        check(vkCreateShaderModule(device_, &mi, nullptr, &present_shaders_[1]), "vkCreateShaderModule");
    }
    present_scale_[0] = config_.scale_x, present_scale_[1] = config_.scale_y;
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
        const std::vector<uint32_t> code[2] = {present_vs(),
                                               present_fs(config_.width * config_.scale_x, config_.height * config_.scale_y, config_.height)};
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
    const VkVertexInputBindingDescription binding{0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attributes;
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
    // FSR where the frame is shown larger than it is (not at whole multiples:
    // there it would only blur).
    const bool fsr = picture_.fsr && picture_.scaling != Picture::Scaling::Integer && (dw > frame.width || dh > frame.height);
    if (fsr) fsr_objects(uint32_t(dw), uint32_t(dh));
    if (picture_.smaa) smaa_objects(frame);
    // The vertices first: a full ring submits, which must come before the window's image is touched.
    // Sampled as it is (smooth) where FSR made it the shown size, or with smooth pixels and no CRT.
    const float crt = picture_.look == Picture::Look::Crt ? 1.0f : 0.0f;
    const float smooth = fsr || (picture_.smooth && !crt) ? 1.0f : 0.0f;
    const float sl = std::clamp(picture_.scanlines, 0.0f, 1.0f), mk = std::clamp(picture_.mask, 0.0f, 1.0f);
    const float tri[24] = {-1, -1, 0, 0, smooth, crt, sl, mk, 3, -1, 2, 0, smooth, crt, sl, mk, -1, 3, 0, 2, smooth, crt, sl, mk};
    const VkDeviceSize at = ring_alloc(sizeof tri, 16, tri);
    record();
    transition(frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Target& smoothed = picture_.smaa ? run_smaa(frame) : frame;
    Target& source = fsr ? run_fsr(smoothed) : smoothed;
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
    linear.anisotropy = nearest.anisotropy = 0;  // the present pass's own: never anisotropic
    nearest.mag = nearest.min = Filter::Nearest;
    const VkDescriptorImageInfo images[2] = {{sampler(linear), source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
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
    if (overlay_) {  // the menu over it, on the whole window
        const VkViewport whole{0, 0, float(swap_extent_.width), float(swap_extent_.height), 0, 1};
        vkCmdSetViewport(cmd_, 0, 1, &whole);
        const VkRect2D all{{0, 0}, swap_extent_};
        vkCmdSetScissor(cmd_, 0, 1, &all);
        overlay_(cmd_);
    }
    vkCmdEndRendering(cmd_);
    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);  // before the semaphore present waits on
    submit(present_wait_, rendered_[index]);

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

void VulkanRenderer::fsr_objects(uint32_t width, uint32_t height) {
    if (!fsr_layout_) {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (auto& x : b) x.descriptorCount = 1, x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 2;
        sl.pBindings = b;
        check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &fsr_set_layout_), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &fsr_set_layout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device_, &pl, nullptr, &fsr_layout_), "vkCreatePipelineLayout");
        const std::span<const uint32_t> code[2] = {fsr_spirv::kFsr_easu, fsr_spirv::kFsr_rcas};
        for (int i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = code[i].size_bytes();
            mi.pCode = code[i].data();
            VkShaderModule module = VK_NULL_HANDLE;
            check(vkCreateShaderModule(device_, &mi, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName = "main";
            ci.layout = fsr_layout_;
            check(vkCreateComputePipelines(device_, pipeline_cache_, 1, &ci, nullptr, &fsr_pipelines_[i]), "vkCreateComputePipelines");
            vkDestroyShaderModule(device_, module, nullptr);
        }
    }
    if (fsr_images_[0].width == width && fsr_images_[0].height == height) return;
    wait_idle();  // the old ones may still be read by a frame on its way
    for (Target& t : fsr_images_) {
        if (t.image) {
            vkDestroyImageView(device_, t.view, nullptr);
            vkDestroyImage(device_, t.image, nullptr);
            vkFreeMemory(device_, t.memory, nullptr);
        }
        t = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         width, height);
    }
}

Target& VulkanRenderer::run_fsr(Target& frame) {
    SamplerDesc linear;
    linear.u = linear.v = Address::Clamp;
    linear.anisotropy = 0;
    const VkSampler smp = sampler(linear);
    Target* in = &frame;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        Target& out = fsr_images_[pass];
        transition(out, VK_IMAGE_LAYOUT_GENERAL);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptor_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &fsr_set_layout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        check(vkAllocateDescriptorSets(device_, &ai, &set), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo source{smp, in->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo target{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = i ? &target : &source;
        }
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
        uint32_t push[20] = {};
        if (pass == 0) fsr::easu_constants(frame.width, frame.height, out.width, out.height, push);
        else fsr::rcas_constants(picture_.sharpness, push);
        push[16] = out.width, push[17] = out.height;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_pipelines_[pass]);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd_, fsr_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(cmd_, (out.width + 15) / 16, (out.height + 15) / 16, 1);  // a 16×16 tile a group
        transition(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        in = &out;
    }
    return fsr_images_[1];
}

void VulkanRenderer::smaa_objects(const Target& frame) {
    if (!smaa_layout_) {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 3;
        sl.pBindings = b;
        check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &smaa_set_layout_), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &smaa_set_layout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device_, &pl, nullptr, &smaa_layout_), "vkCreatePipelineLayout");
        const std::span<const uint32_t> code[3][2] = {{smaa_spirv::kSmaa1_vert, smaa_spirv::kSmaa1_frag},
                                                      {smaa_spirv::kSmaa2_vert, smaa_spirv::kSmaa2_frag},
                                                      {smaa_spirv::kSmaa3_vert, smaa_spirv::kSmaa3_frag}};
        for (uint32_t pass = 0; pass < 3; ++pass) {
            VkShaderModule modules[2] = {};
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (int i = 0; i < 2; ++i) {
                VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                mi.codeSize = code[pass][i].size_bytes();
                mi.pCode = code[pass][i].data();
                check(vkCreateShaderModule(device_, &mi, nullptr, &modules[i]), "vkCreateShaderModule");
                stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                stages[i].module = modules[i];
                stages[i].pName = "main";
            }
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
            const VkFormat format = pass == 2 ? frame.format : VK_FORMAT_R8G8B8A8_UNORM;
            VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            ri.colorAttachmentCount = 1;
            ri.pColorAttachmentFormats = &format;
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
            info.layout = smaa_layout_;
            check(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &smaa_pipelines_[pass]), "vkCreateGraphicsPipelines");
            for (VkShaderModule m : modules) vkDestroyShaderModule(device_, m, nullptr);
        }
        // SMAA's tables: the area texture (two channels) and the search texture (one).
        TextureData area;
        area.width = 160, area.height = 560;
        area.rgba.resize(size_t(160) * 560 * 4);
        for (size_t i = 0; i < size_t(160) * 560; ++i)
            area.rgba[i * 4] = smaa_tables::areaTexBytes[i * 2], area.rgba[i * 4 + 1] = smaa_tables::areaTexBytes[i * 2 + 1];
        smaa_area_ = create_texture(area);
        TextureData search;
        search.width = 64, search.height = 16;
        search.rgba.resize(size_t(64) * 16 * 4);
        for (size_t i = 0; i < size_t(64) * 16; ++i) search.rgba[i * 4] = smaa_tables::searchTexBytes[i];
        smaa_search_ = create_texture(search);
    }
    if (smaa_images_[0].width == frame.width && smaa_images_[0].height == frame.height) return;
    wait_idle();  // the old ones may still be read by a frame on its way
    for (uint32_t i = 0; i < 3; ++i) {
        Target& t = smaa_images_[i];
        if (t.image) {
            vkDestroyImageView(device_, t.view, nullptr);
            vkDestroyImage(device_, t.image, nullptr);
            vkFreeMemory(device_, t.memory, nullptr);
        }
        t = create_image(i == 2 ? frame.format : VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, frame.width,
                         frame.height);
        t.sx = frame.sx, t.sy = frame.sy;
    }
}

Target& VulkanRenderer::run_smaa(Target& frame) {
    SamplerDesc linear;
    linear.u = linear.v = Address::Clamp;
    linear.anisotropy = 0;
    SamplerDesc point = linear;
    point.mag = point.min = Filter::Nearest;
    const VkSampler lin = sampler(linear), pnt = sampler(point);
    const VkImageView area = textures_.at(smaa_area_).view, search = textures_.at(smaa_search_).view;
    // What each pass reads: the colour; the edges, the area and search tables; the colour and the weights.
    const VkDescriptorImageInfo inputs[3][3] = {
        {{lin, frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {lin, area, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {pnt, search, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
        {{lin, smaa_images_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {lin, area, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {pnt, search, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
        {{lin, frame.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {lin, smaa_images_[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {pnt, search, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    const float metrics[4] = {1.0f / float(frame.width), 1.0f / float(frame.height), float(frame.width), float(frame.height)};
    for (uint32_t pass = 0; pass < 3; ++pass) {
        Target& out = smaa_images_[pass];
        transition(out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptor_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &smaa_set_layout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        check(vkAllocateDescriptorSets(device_, &ai, &set), "vkAllocateDescriptorSets");
        VkWriteDescriptorSet w[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &inputs[pass][i];
        }
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = out.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = pass == 2 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;  // edge detection discards the rest
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {out.width, out.height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd_, &ri);
        const VkViewport viewport{0, 0, float(out.width), float(out.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {out.width, out.height}};
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_pipelines_[pass]);
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd_, smaa_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof metrics, metrics);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRendering(cmd_);
        transition(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    drawn_.valid = false;  // its pipeline, viewport and scissor are SMAA's now
    return smaa_images_[2];
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
    // At the game's size: each pixel becomes sx × sy of the image's.
    if (im.width * t.sx != t.width || im.height * t.sy != t.height) return;
    std::vector<uint8_t> bgra(size_t(t.width) * t.height * 4);
    for (uint32_t y = 0; y < t.height; ++y) {
        const uint8_t* row = &im.rgba[size_t(y / t.sy) * im.width * 4];
        uint8_t* out = &bgra[size_t(y) * t.width * 4];
        for (uint32_t x = 0; x < t.width; ++x, out += 4) {
            const uint8_t* p = row + size_t(x / t.sx) * 4;
            out[0] = p[2], out[1] = p[1], out[2] = p[0], out[3] = p[3];
        }
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

    // At the game's size: each pixel the average of the sx × sy it became.
    Image out;
    out.width = t.width / t.sx;
    out.height = t.height / t.sy;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    void* mapped = nullptr;
    check(vkMapMemory(device_, readback_memory_, 0, bytes, 0, &mapped), "vkMapMemory");
    const uint8_t* bgra = static_cast<const uint8_t*>(mapped);
    const uint32_t n = t.sx * t.sy;
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sum[4] = {};
            for (uint32_t j = 0; j < t.sy; ++j) {
                const uint8_t* p = bgra + (size_t(y * t.sy + j) * t.width + size_t(x) * t.sx) * 4;
                for (uint32_t i = 0; i < t.sx; ++i, p += 4) sum[0] += p[2], sum[1] += p[1], sum[2] += p[0], sum[3] += p[3];
            }
            uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
            for (int k = 0; k < 4; ++k) o[k] = uint8_t((sum[k] + n / 2) / n);
        }
    vkUnmapMemory(device_, readback_memory_);
    return out;
}


// ---------------------------------------------------------------- drawing
constexpr uint32_t kSamplers = 4;
constexpr VkDeviceSize kRingBytes = 32ull << 20;  // a slot's (kSlots of them)
constexpr VkDeviceSize kArenaBytes = 64ull << 20;  // the vertex arena

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
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * sets},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSamplers * sets},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}};  // FSR's, two a frame
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = sets;
    dp.poolSizeCount = 3;
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
    // The vertex arena: device-local memory the CPU can write where the GPU
    // has it (integrated GPUs, Apple's, the Raspberry Pi's, resizable BAR),
    // else host memory, which the GPU reads as it reads the ring.
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kArenaBytes;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &bi, nullptr, &arena_), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, arena_, &req);
        const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_.memoryTypeCount && type == UINT32_MAX; ++i) {
            const VkMemoryPropertyFlags f = memory_.memoryTypes[i].propertyFlags;
            const VkDeviceSize heap = memory_.memoryHeaps[memory_.memoryTypes[i].heapIndex].size;
            if ((req.memoryTypeBits & (1u << i)) && (f & (visible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == (visible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                heap >= 4 * kArenaBytes)  // not the small 256 MB window a GPU without resizable BAR has
                type = i;
        }
        if (type == UINT32_MAX) type = memory_type(req.memoryTypeBits, visible);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &alloc, nullptr, &arena_memory_), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, arena_, arena_memory_, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        check(vkMapMemory(device_, arena_memory_, 0, kArenaBytes, 0, &mapped), "vkMapMemory");
        arena_mapped_ = static_cast<uint8_t*>(mapped);
        arena_free_ = {{0, kArenaBytes}};
        // Its pages touched now, not as the first scene's vertices go in (a
        // fault for each page then, mid-frame).
        std::memset(arena_mapped_, 0, kArenaBytes);
    }
    use_slot(0);
}

bool VulkanRenderer::arena_alloc(uint64_t bytes, uint64_t* offset, void** write) {
    bytes = (bytes + 15) & ~uint64_t(15);
    if (!bytes) return false;
    for (auto it = arena_free_.begin(); it != arena_free_.end(); ++it) {
        if (it->second < bytes) continue;
        const uint64_t at = it->first, left = it->second - bytes;
        arena_free_.erase(it);
        if (left) arena_free_[at + bytes] = left;
        *offset = at;
        *write = arena_mapped_ + at;
        return true;
    }
    return false;
}

// Given back now, but usable only once what was recorded so far has run:
// with this submission's slot, after its fence (retire).
void VulkanRenderer::arena_free(uint64_t offset, uint64_t bytes) {
    bytes = (bytes + 15) & ~uint64_t(15);
    if (bytes) slots_[slot_].arena_freed.push_back({offset, bytes});
}

void VulkanRenderer::arena_release(uint64_t offset, uint64_t bytes) {
    auto next = arena_free_.lower_bound(offset);
    if (next != arena_free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            offset = prev->first, bytes += prev->second;
            arena_free_.erase(prev);
        }
    }
    if (next != arena_free_.end() && offset + bytes == next->first) {
        bytes += next->second;
        arena_free_.erase(next);
    }
    arena_free_[offset] = bytes;
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
    acquire_free_.insert(acquire_free_.end(), sl.acquire_waited.begin(), sl.acquire_waited.end());
    sl.acquire_waited.clear();
    for (Texture& t : sl.dead) {
        vkDestroyImageView(device_, t.view, nullptr);
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
    }
    sl.dead.clear();
    for (auto& [b, m] : sl.staging) vkDestroyBuffer(device_, b, nullptr), vkFreeMemory(device_, m, nullptr);
    sl.staging.clear();
    for (auto [o, b] : sl.arena_freed) arena_release(o, b);
    sl.arena_freed.clear();
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

namespace {

// The library's record of a pipeline's shaders: the vertex program, field
// by field, and the combiners as they are.
constexpr char kLibraryMagic[8] = {'Q', 'I', 'L', 'I', 'N', 'P', 'L', '1'};
std::string library_sources(const vsh::Program& p, const nv2a::CombinerSetup& cs) {
    std::string out;
    auto put32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    put32(p.header);
    put32(uint32_t(p.code.size()));
    for (const vsh::Instruction& in : p.code) {
        out += char(in.mac), out += char(in.ilu);
        for (const vsh::Operand* o : {&in.a, &in.b, &in.c}) {
            out += char(o->kind), out += char(o->index);
            for (uint8_t k : o->swizzle) out += char(k);
            out += char(o->negate);
        }
        for (uint8_t v : {in.constant, uint8_t(in.relative), in.temp, in.mac_temp_mask, in.ilu_temp_mask, in.out_mask, uint8_t(in.out_is_const),
                          in.out_address, uint8_t(in.out_from_ilu), uint8_t(in.final)})
            out += char(v);
    }
    static_assert(std::is_trivially_copyable_v<nv2a::CombinerSetup>);
    out.append(reinterpret_cast<const char*>(&cs), sizeof cs);
    return out;
}
bool library_read(std::string_view& in, vsh::Program& p, nv2a::CombinerSetup& cs) {
    auto take = [&](void* to, size_t n) {
        if (in.size() < n) return false;
        std::memcpy(to, in.data(), n);
        in.remove_prefix(n);
        return true;
    };
    uint32_t count = 0;
    if (!take(&p.header, 4) || !take(&count, 4) || count > 136) return false;
    p.code.assign(count, {});
    for (vsh::Instruction& i : p.code) {
        uint8_t b[2 + 3 * 7 + 10];
        if (!take(b, sizeof b)) return false;
        const uint8_t* q = b;
        i.mac = vsh::Mac(*q++), i.ilu = vsh::Ilu(*q++);
        for (vsh::Operand* o : {&i.a, &i.b, &i.c}) {
            o->kind = vsh::Src(*q++), o->index = *q++;
            for (uint8_t& k : o->swizzle) k = *q++;
            o->negate = *q++ != 0;
        }
        i.constant = *q++, i.relative = *q++ != 0, i.temp = *q++, i.mac_temp_mask = *q++, i.ilu_temp_mask = *q++, i.out_mask = *q++;
        i.out_is_const = *q++ != 0, i.out_address = *q++, i.out_from_ilu = *q++ != 0, i.final = *q++ != 0;
    }
    return take(&cs, sizeof cs);
}

}  // namespace

VkPipeline VulkanRenderer::pipeline(const DrawCmd& c) {
    PipelineKey k;
    k.vs = c.vs_key, k.fs = c.fs_key, k.inputs = c.inputs, k.constant_inputs = c.constant_inputs & c.inputs, k.primitive = uint8_t(c.primitive);
    k.blend = c.blend, k.write_mask = c.write_mask, k.depth = target_has_depth();
    if (c.blend) {
        k.factors[0] = uint8_t(c.src_color), k.factors[1] = uint8_t(c.dst_color), k.factors[2] = uint8_t(c.src_alpha), k.factors[3] = uint8_t(c.dst_alpha);
        k.ops[0] = uint8_t(c.color_op), k.ops[1] = uint8_t(c.alpha_op);
    }
    if (auto it = pipelines_.find(k); it != pipelines_.end()) return it->second;
    // Made by the library's worker, or being made: taken (or waited for),
    // if it was made from what this draw's shaders are.
    if (library_.worker.joinable()) {
        std::unique_lock<std::mutex> hold(library_.lock);
        library_.done.wait(hold, [&] { return !library_.busy || library_.current != k; });
        if (auto it = library_.made.find(k); it != library_.made.end()) {
            const VkPipeline p = it->second;
            library_.made.erase(it);
            const auto known = library_.sources.find(k);
            if (c.vs_program && c.fs_setup && known != library_.sources.end() && known->second == library_sources(*c.vs_program, *c.fs_setup)) {
                pipelines_[k] = p;
                return p;
            }
            vkDestroyPipeline(device_, p, nullptr);  // never used
        }
        library_.claimed.insert(k);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const VkPipeline p = make_pipeline(k, shader(c.vs_key, c.vs_code), shader(c.fs_key, c.fs_code));
    // QILIN_TRACE_PIPELINES=1: each pipeline made as a draw needs it, and how long that took.
    static const bool trace = std::getenv("QILIN_TRACE_PIPELINES") != nullptr;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++costs_.pipelines, costs_.pipeline_ms += ms;
    if (trace) std::fprintf(stderr, "qilin: pipeline %zu made for a draw in %.2f ms\n", pipelines_.size() + 1, ms);
    pipelines_[k] = p;
    ++unsaved_pipelines_;
    last_pipeline_ = std::chrono::steady_clock::now();
    record_in_library(k, c);
    return p;
}

// A draw pipeline for the key's state and these shaders. Reads only what
// stays the same after init, so the library's worker makes them too.
VkPipeline VulkanRenderer::make_pipeline(const PipelineKey& k, VkShaderModule vs, VkShaderModule fs) const {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t n = 0; n < 16; ++n)
        if (k.inputs & (1u << n)) {
            // A constant attribute is one element at the instance rate: every
            // vertex of the draw's one instance reads the same vec4.
            bindings.push_back({n, 16, (k.constant_inputs >> n & 1) ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
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
    ia.topology = kTopology[k.primitive];
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = k.blend;
    if (k.blend) {
        att.srcColorBlendFactor = VkBlendFactor(k.factors[0]);
        att.dstColorBlendFactor = VkBlendFactor(k.factors[1]);
        att.srcAlphaBlendFactor = VkBlendFactor(k.factors[2]);
        att.dstAlphaBlendFactor = VkBlendFactor(k.factors[3]);
        att.colorBlendOp = VkBlendOp(k.ops[0]);
        att.alphaBlendOp = VkBlendOp(k.ops[1]);
    }
    att.colorWriteMask = k.write_mask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                      VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH};
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
    return p;
}

// The library file: a header (kLibraryMagic, then the sizes of the key's
// and the combiners' records, which tell an older layout), then entries,
// each the key's fields and its shaders' sources with their length.
void VulkanRenderer::write_library_entry(std::ostream& out, const PipelineKey& k, const std::string& sources) {
    out.write(reinterpret_cast<const char*>(&k.vs), 8).write(reinterpret_cast<const char*>(&k.fs), 8);
    out.write(reinterpret_cast<const char*>(&k.inputs), 4).write(reinterpret_cast<const char*>(&k.constant_inputs), 4);
    const uint8_t bytes[10] = {k.primitive, k.blend, k.write_mask, k.factors[0], k.factors[1], k.factors[2], k.factors[3], k.ops[0], k.ops[1], k.depth};
    out.write(reinterpret_cast<const char*>(bytes), sizeof bytes);
    const uint32_t n = uint32_t(sources.size());
    out.write(reinterpret_cast<const char*>(&n), 4).write(sources.data(), std::streamsize(n));
}

void VulkanRenderer::start_library() {
    if (config_.pipeline_cache.empty()) return;
    library_.path = std::filesystem::path(config_.pipeline_cache).replace_filename("pipeline_library.bin");
    const uint32_t layout[2] = {34, uint32_t(sizeof(nv2a::CombinerSetup))};
    std::string data;
    if (std::ifstream in(library_.path, std::ios::binary); in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    struct Entry {
        PipelineKey key;
        vsh::Program program;
        nv2a::CombinerSetup setup;
    };
    std::vector<Entry> entries;
    bool whole = data.size() >= 16 && std::memcmp(data.data(), kLibraryMagic, 8) == 0 && std::memcmp(data.data() + 8, layout, 8) == 0;
    if (whole) {
        std::string_view in(data);
        in.remove_prefix(16);
        while (!in.empty()) {
            Entry e;
            PipelineKey& k = e.key;
            uint8_t b[10];
            uint32_t n = 0;
            if (in.size() < 38) break;
            std::memcpy(&k.vs, in.data(), 8), std::memcpy(&k.fs, in.data() + 8, 8);
            std::memcpy(&k.inputs, in.data() + 16, 4), std::memcpy(&k.constant_inputs, in.data() + 20, 4);
            std::memcpy(b, in.data() + 24, 10), std::memcpy(&n, in.data() + 34, 4);
            in.remove_prefix(38);
            k.primitive = b[0], k.blend = b[1], k.write_mask = b[2], k.factors[0] = b[3], k.factors[1] = b[4], k.factors[2] = b[5];
            k.factors[3] = b[6], k.ops[0] = b[7], k.ops[1] = b[8], k.depth = b[9];
            if (in.size() < n || k.primitive > 4) break;
            std::string_view src = in.substr(0, n);
            in.remove_prefix(n);
            if (!library_read(src, e.program, e.setup) || !src.empty() || library_.sources.count(k)) continue;
            library_.sources[k] = library_sources(e.program, e.setup);
            library_.order.push_back(k);
            entries.push_back(std::move(e));
        }
        whole = in.empty();  // else what's after a damaged entry is dropped, and the file written again
    }
    if (!whole) {
        std::ofstream out(library_.path, std::ios::binary | std::ios::trunc);
        out.write(kLibraryMagic, 8).write(reinterpret_cast<const char*>(layout), 8);
        for (const PipelineKey& k : library_.order) write_library_entry(out, k, library_.sources[k]);
    }
    library_.out.open(library_.path, std::ios::binary | std::ios::app);
    if (entries.empty()) return;
    library_.worker = std::thread([this, entries = std::move(entries)] {
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t count = 0;
        for (const Entry& e : entries) {
            if (library_.stop) break;
            {
                std::lock_guard<std::mutex> hold(library_.lock);
                if (library_.claimed.count(e.key)) continue;
                library_.busy = true, library_.current = e.key;
            }
            VkPipeline p = VK_NULL_HANDLE;
            VkShaderModule modules[2] = {};
            try {
                const std::vector<uint32_t> code[2] = {nv2a::vertex_program(e.program, e.key.inputs), nv2a::combiners(e.setup)};
                for (int i = 0; i < 2; ++i) {
                    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    info.codeSize = code[i].size() * 4;
                    info.pCode = code[i].data();
                    if (vkCreateShaderModule(device_, &info, nullptr, &modules[i]) != VK_SUCCESS) throw std::runtime_error("vkCreateShaderModule");
                }
                p = make_pipeline(e.key, modules[0], modules[1]);
            } catch (const std::exception&) {
                p = VK_NULL_HANDLE;  // made when a draw wants it, or reported then
            }
            for (VkShaderModule m : modules)
                if (m) vkDestroyShaderModule(device_, m, nullptr);
            {
                std::lock_guard<std::mutex> hold(library_.lock);
                library_.busy = false;
                if (p) library_.made[e.key] = p, ++count;
            }
            library_.done.notify_all();
        }
        if (std::getenv("QILIN_TRACE_PIPELINES"))
            std::fprintf(stderr, "qilin: %u of the library's %zu pipelines made ahead, in %.2f s\n", count, entries.size(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    });
}

void VulkanRenderer::record_in_library(const PipelineKey& k, const DrawCmd& c) {
    if (!library_.out.is_open() || !c.vs_program || !c.fs_setup) return;
    std::string src = library_sources(*c.vs_program, *c.fs_setup);
    auto it = library_.sources.find(k);
    if (it == library_.sources.end()) {
        write_library_entry(library_.out, k, src);
        library_.sources.emplace(k, std::move(src));
        library_.order.push_back(k);
    } else if (it->second != src) {
        it->second = std::move(src), library_.stale = true;
    }
}

void VulkanRenderer::stop_library() {
    library_.stop = true;
    if (library_.worker.joinable()) library_.worker.join();
    for (auto& [k, p] : library_.made) vkDestroyPipeline(device_, p, nullptr);
    library_.made.clear();
    library_.out.close();
    if (library_.stale) {
        const uint32_t layout[2] = {34, uint32_t(sizeof(nv2a::CombinerSetup))};
        std::ofstream out(library_.path, std::ios::binary | std::ios::trunc);
        out.write(kLibraryMagic, 8).write(reinterpret_cast<const char*>(layout), 8);
        for (const PipelineKey& k : library_.order) write_library_entry(out, k, library_.sources[k]);
    }
}

VkSampler VulkanRenderer::sampler(const SamplerDesc& wanted) {
    SamplerDesc d = wanted;
    if (d.anisotropy == 0) d.anisotropy = 1;  // (the present pass's: never)
    else d.anisotropy = d.min == Filter::Linear ? anisotropy_ : 1;
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
    if (d.anisotropy > 1) info.anisotropyEnable = VK_TRUE, info.maxAnisotropy = float(d.anisotropy);
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
    struct Timed {  // into costs_
        Costs& c;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { ++c.textures, c.texture_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } timed{costs_};
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
    struct Timed {  // into costs_
        Costs& c;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { ++c.textures, c.texture_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } timed{costs_};
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
    for (CopyRegion r : regions) {  // the game's pixels; both images are at the render resolution
        r.sx *= from.sx, r.dx *= from.sx, r.width *= from.sx;
        r.sy *= from.sy, r.dy *= from.sy, r.height *= from.sy;
        if (!r.width || !r.height || r.sx + r.width > from.width || r.sy + r.height > from.height || r.dx + r.width > to.width ||
            r.dy + r.height > to.height || to.sx != from.sx || to.sy != from.sy)
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

void VulkanRenderer::set_resolution_scale(uint32_t x, uint32_t y) {
    x = std::clamp<uint32_t>(x, 1, 8), y = std::clamp<uint32_t>(y, 1, 8);
    if (x == config_.scale_x && y == config_.scale_y) return;
    end_pass();
    wait_idle();
    config_.scale_x = x, config_.scale_y = y;
    // Each image made again at the new size: colour scaled across (linear),
    // depth and stencil cleared (the game clears them each frame).
    std::vector<Target> old;
    record();
    auto remake = [&](Target& t) {
        Target n = create_target(t.format, t.usage, t.aspect, t.width / t.sx, t.height / t.sy);
        const VkImageLayout was = t.layout;
        transition(n, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (t.aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(t.width), int32_t(t.height), 1};
            blit.dstOffsets[1] = {int32_t(n.width), int32_t(n.height), 1};
            vkCmdBlitImage(cmd_, t.image, t.layout, n.image, n.layout, 1, &blit, VK_FILTER_LINEAR);
            if (was == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(n, was);  // sampled between passes, as it was
        } else {
            const VkClearDepthStencilValue zero{};
            const VkImageSubresourceRange range{n.aspect, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd_, n.image, n.layout, &zero, 1, &range);
        }
        old.push_back(t);
        t = n;
    };
    for (Target& t : color_) remake(t);
    if (depth_.image) remake(depth_);
    for (auto& [id, t] : render_targets_) remake(t);
    for (auto& [id, t] : depth_targets_) remake(t);
    submit();
    wait_idle();
    for (Target& t : old) {
        vkDestroyImageView(device_, t.view, nullptr);
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
    }
    for (Slot& s : slots_) s.sets.clear();  // they named the old images' views
    drawn_.valid = false;
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
    Target t = create_target(depth_format_,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 (depth_sampled_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
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

bool VulkanRenderer::pack_depth(uint32_t depth_id, uint32_t rt_id) {
    auto di = depth_targets_.find(depth_id);
    auto rti = render_targets_.find(rt_id);
    if (!depth_sampled_ || di == depth_targets_.end() || rti == render_targets_.end()) return false;
    Target& from = di->second;
    Target& to = rti->second;
    if (from.width < to.width || from.height < to.height) return false;
    if (!pack_pipeline_) {
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
        check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &pack_set_layout_), "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &pack_set_layout_;
        check(vkCreatePipelineLayout(device_, &pl, nullptr, &pack_layout_), "vkCreatePipelineLayout");
        const std::span<const uint32_t> code[2] = {depth_pack_spirv::kDepthPack_vert, depth_pack_spirv::kDepthPack_frag};
        VkShaderModule modules[2] = {};
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = code[i].size_bytes();
            mi.pCode = code[i].data();
            check(vkCreateShaderModule(device_, &mi, nullptr, &modules[i]), "vkCreateShaderModule");
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = modules[i];
            stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
        const VkFormat format = to.format;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &format;
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
        info.layout = pack_layout_;
        check(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &pack_pipeline_), "vkCreateGraphicsPipelines");
        for (VkShaderModule m : modules) vkDestroyShaderModule(device_, m, nullptr);
    }
    // Outside the pass: the next one takes the current target and depth back
    // into their attachment layouts.
    end_pass();
    record();
    // Views of each aspect alone (sampling reads one), freed with the submission.
    VkImageView aspect_view[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = from.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = from.format;
        view.subresourceRange = {i ? VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT), 0, 1, 0, 1};
        check(vkCreateImageView(device_, &view, nullptr, &aspect_view[i]), "vkCreateImageView");
        dead_textures_.push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, aspect_view[i]});
    }
    transition(from, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    transition(to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    SamplerDesc point;
    point.u = point.v = Address::Clamp;
    point.mag = point.min = Filter::Nearest;
    point.anisotropy = 0;
    const VkSampler pnt = sampler(point);
    const VkDescriptorImageInfo inputs[2] = {{pnt, aspect_view[0], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                             {pnt, aspect_view[1], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &pack_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(device_, &ai, &set), "vkAllocateDescriptorSets");
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &inputs[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = to.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every texel is written
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea = {{0, 0}, {to.width, to.height}};
    rinfo.layerCount = 1;
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd_, &rinfo);
    const VkViewport viewport{0, 0, float(to.width), float(to.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {to.width, to.height}};
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pack_pipeline_);
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pack_layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRendering(cmd_);
    transition(to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    drawn_.valid = false;  // its pipeline, viewport and scissor are this pass's now
    return true;
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
        uint32_t from_ring = 0;
        for (uint32_t n = 0; n < 16; ++n) from_ring += (c.inputs >> n & 1) && c.arena[n] == DrawCmd::kNoArena && !(c.constant_inputs >> n & 1);
        VkDeviceSize need = VkDeviceSize(from_ring) * (VkDeviceSize(c.vertex_count) * 16 + 16) + VkDeviceSize(std::popcount(c.constant_inputs)) * 32 +
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
            if (c.arena[n] != DrawCmd::kNoArena) {
                buffers[count] = arena_;
                offsets[count++] = c.arena[n];
            } else {
                buffers[count] = ring_;
                const VkDeviceSize elements = (c.constant_inputs >> n & 1) ? 1 : c.vertex_count;
                offsets[count++] = ring_alloc(elements * 16, 16, c.attributes[n]);
            }
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
            const bool used = c.inputs & (1u << n);
            span_buffers[n - first] = used ? buffers[k] : ring_;
            span_offsets[n - first] = used ? offsets[k] : 0;
            k += used;
        }
        vkCmdBindVertexBuffers(cmd_, first, last - first + 1, span_buffers, span_offsets);
    }
    const int32_t tw = int32_t(target().width), th = int32_t(target().height);
    const VkViewport viewport{0, 0, float(tw), float(th), 0, 1};
    if (all || std::memcmp(&was.viewport, &viewport, sizeof viewport) != 0) vkCmdSetViewport(cmd_, 0, 1, &viewport), was.viewport = viewport;
    const int64_t ssx = target().sx, ssy = target().sy;  // the scissor is in the game's pixels
    const int32_t sx0 = int32_t(std::clamp<int64_t>(int64_t(c.scissor.x) * ssx, 0, tw)), sy0 = int32_t(std::clamp<int64_t>(int64_t(c.scissor.y) * ssy, 0, th));
    const int32_t sx1 = int32_t(std::clamp<int64_t>((int64_t(c.scissor.x) + c.scissor.w) * ssx, sx0, tw));
    const int32_t sy1 = int32_t(std::clamp<int64_t>((int64_t(c.scissor.y) + c.scissor.h) * ssy, sy0, th));
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
    // ZBias (the NV2A's polygon offset): glPolygonOffset's factor and units
    // are Vulkan's slope and constant factors. The slope is the depth's
    // change per pixel, which at a render resolution s times the game's is
    // 1/s of the console's: the factor times s keeps the offset the
    // console's (or, with s across and down differing, no less), else a
    // floor's projected shadows sink into the floor in patches.
    const float slope = c.depth_slope * float(std::max(ssx, ssy));
    const bool bias_on = c.depth_bias != 0 || slope != 0;
    if (all || was.bias_on != bias_on) vkCmdSetDepthBiasEnable(cmd_, bias_on), was.bias_on = bias_on;
    if (bias_on && (all || was.bias != c.depth_bias || was.slope != slope))
        vkCmdSetDepthBias(cmd_, c.depth_bias, 0.0f, slope), was.bias = c.depth_bias, was.slope = slope;
    const float line = wide_lines_ ? std::clamp(c.line_width * float(ssy), 1.0f, line_width_max_) : 1.0f;
    if (all || was.line != line) vkCmdSetLineWidth(cmd_, line), was.line = line;
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
