// Direct3D 8 (the XDK's D3D8 library), replaced natively over the Vulkan
// renderer (recomp/runtime/gfx). docs/graphics.md describes the design.
//
// What stays in guest memory, in the original layout:
// - D3D's state tables and dirty flags (D3D__RenderState at 0x181B70 and the
//   tables around it): the game's inline SetRenderState / SetTextureStageState
//   write them directly, so the renderer reads them from there when it draws.
// - Resources (surfaces, textures, buffers): the game passes their headers
//   around and may read them. Each header the runtime makes is in contiguous
//   guest memory; its pixels live in the renderer.
// The device object itself is the runtime's: the game never reads it. The
// lifted D3D resource code (recomp/src/main.cpp, kLiftedD3d) reads a few of
// its fields, which mirror_device keeps current in the original layout.
//
// Anything not modelled yet (a Swap flag, a partial colour clear, a call
// that isn't registered) stops the runtime with its name.
#include <sstream>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <bit>
#include <cmath>
#include <algorithm>
#include <array>
#include <string_view>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "gfx.hpp"
#include "nv2a_shaders.hpp"
#include "texture.hpp"
#include "vsh.hpp"
#include "host.hpp"
#include "sched.hpp"
#include "window.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "settings.hpp"
#include "strf.hpp"

namespace qilin::host {
namespace {

// ---------------------------------------------------------------- guest layout
constexpr uint32_t kGlobalDevice = 0x181E08;  // D3D__pDevice
constexpr uint32_t kDeviceStorage = 0x181E10;  // the static device object it points to
constexpr uint32_t kPushBufferSize = 0x1845B0, kKickOffSize = 0x1845AC;  // Direct3D_SetPushBufferSize's globals

// D3DResource.Common
constexpr uint32_t kRefMask = 0xFFFF, kTypeMask = 0x70000, kTypeSurface = 0x50000, kTypePushBuffer = 0x20000;
constexpr uint32_t kIntRef1 = 0x80000, kIntRefMask = 0x780000, kD3DCreated = 0x01000000;

// D3DFORMAT values (D3DPixelContainer.Format bits 8-15)
constexpr uint32_t kMultisampleNone = 0x11;  // D3DMULTISAMPLE_NONE on the Xbox (1x1 samples)
// Device fields the lifted resource code reads (offsets in the original device object).
constexpr uint32_t kDevMultisample = 0x195C, kDevRenderTarget = 0x19F4, kDevDepth = 0x19F8, kDevFrameBuffers = 0x1A04;
constexpr uint32_t kDirtyFlags = 0x181968;      // D3D__DirtyFlags
constexpr uint32_t kRenderStateTable = 0x181B70;  // D3D__RenderState (the pixel shader's registers first)
constexpr uint32_t kDevFence = 0x2C;  // the device's current fence
constexpr uint32_t kDevViewport = 0xED0;
constexpr uint32_t kDevRefs = 0x934;  // the device object's reference count
constexpr uint32_t kDevVertexShader = 0x790, kFixedFunctionShader = 0x184478;
constexpr uint32_t kFmtA8R8G8B8 = 0x06, kFmtX8R8G8B8 = 0x07, kFmtLinA8R8G8B8 = 0x12, kFmtLinX8R8G8B8 = 0x1E,
                   kFmtD24S8 = 0x2A, kFmtLinD24S8 = 0x2E;

// D3DCLEAR
constexpr uint32_t kClearTarget = 0xF0, kClearZBuffer = 0x01, kClearStencil = 0x02;

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }
uint32_t ld(const rt::Context& c, uint32_t a) { return rt::ld32(mem(c), a, 0); }
void st(const rt::Context& c, uint32_t a, uint32_t v) { rt::st32(mem(c), a, v, 0); }

[[noreturn]] void unmodelled(const char* fn, const std::string& what) {
    static std::string why;
    why = strf("%s: %s isn't modelled yet", fn, what.c_str());
    rt::trap(0, why.c_str());
}

// ---------------------------------------------------------------- device state
struct Device {
    std::unique_ptr<gfx::Renderer> renderer;
    std::unique_ptr<ui::Menu> menu;  // the settings menu, once the window has shown a frame
    gfx::Picture picture;            // how frames are shown (the settings)
    bool settings_changed = false;   // by the menu, since the last frame
    float aspect = 0;                // the shape the game draws for and frames are shown at (0: not yet set)
    D3dConfig config;
    uint32_t width = 0, height = 0, interval = 1;
    // The frame rate (the settings'): original (the game's interval) and 60
    // keep the console's vblank clock; display and unlimited give the game
    // its time step from the host's clock (frame_clock) and pace frames by
    // the display (frame_pacing).
    platform::Settings::FrameRate frame_rate = platform::Settings::FrameRate::Original;
    std::chrono::steady_clock::time_point clock_last{};  // the frame clock's last call (unlocked rates)
    std::chrono::steady_clock::time_point next_frame{};  // the next frame's time, pacing without vsync
    std::chrono::steady_clock::time_point last_refresh{};
    std::chrono::steady_clock::time_point last_shown{};
    // The flat draws' extents in the frame buffer (x0, x1, y0, y1, the
    // game's pixels), this frame's and the last's: a line of text the game
    // draws in pieces moves as one when the HUD is re-anchored.
    std::vector<std::array<float, 4>> flat_now, flat_before;  // the last Swap's present (the frame rate counter)  // when the last early acquire returned (the display's refresh)
    std::vector<uint32_t> frame_buffers;  // guest surfaces; frame_buffers[i] is the renderer's buffer i
    // Frame buffers the CPU locked (cpu_frames.cpp below): the renderer's
    // buffer index and the hash of guest memory as the image was copied there.
    struct CpuFrame {
        uint32_t index;
        uint64_t hash;
    };
    std::vector<CpuFrame> cpu_frames;
    std::chrono::steady_clock::time_point cpu_frames_checked;
    uint32_t depth = 0;                   // guest surface, or 0
    uint32_t vblank_callback = 0, vblank_data = 0;
    uint32_t vblanks = 0, swaps = 0;
    // Bumped whenever the CPU may have changed a resource's memory: a lock
    // or a destroy (both reach D3D_BlockOnResource), or a CopyRects. A
    // render target's memory is compared again only after one of these,
    // or once a frame (as textures are).
    uint32_t memory_epoch = 0;
    // Vertex buffer locks: a count of them, and for each buffer locked (by
    // its data's address) the count at its last lock. A settled vertex run
    // is compared again after a lock of the buffer it lies in, not after
    // any lock: a fight locks its particle and HUD buffers dozens of times
    // a frame, and each lock had every run compared again (12% of the game
    // thread, with the textures hashed again for the same reason).
    uint32_t vertex_epoch = 0;
    std::map<uint32_t, uint32_t> vertex_locked;
    void vertex_buffer_locked(uint32_t data) { vertex_locked[data & 0x0FFFFFFFu] = ++vertex_epoch; }
    // The count at the last lock of the buffer `at` lies in: the locked buffer with the nearest address at or below it.
    uint32_t vertex_lock_of(uint32_t at) const {
        auto it = vertex_locked.upper_bound(at & 0x0FFFFFFFu);
        return it == vertex_locked.begin() ? 0 : std::prev(it)->second;
    }
    // The count at the last lock of anything whose data starts in [at, at + bytes): a texture's levels.
    uint32_t locked_within(uint32_t at, uint64_t bytes) const {
        const uint32_t lo = at & 0x0FFFFFFFu;
        uint32_t latest = 0;
        for (auto it = vertex_locked.lower_bound(lo); it != vertex_locked.end() && it->first < lo + bytes; ++it) latest = std::max(latest, it->second);
        return latest;
    }
    std::chrono::steady_clock::time_point frame_began{};  // for the long-frame log
    std::chrono::steady_clock::time_point epoch;  // vblank 0
    uint8_t gamma[3][256];
    uint32_t heap = 0, heap_used = 0, heap_size = 0;  // guest memory for resource headers
    uint32_t push_sink = 0;  // where D3D's own code (lifted helpers) writes GPU methods; never read
    // The vertex program constants as the GPU holds them (D3D's registers
    // -96..95, biased by 96): what the push buffer would have loaded.
    uint32_t vs_constants[192][4] = {};
    uint32_t vs_program[136][4] = {};  // the GPU's vertex program memory, as Load(Vertex)Shader fills it
    // Pixel shader constant 0 as given (the GPU's fog/specular factor input
    // the original also loads from it).
    uint32_t ps_constant0[3] = {};
    // The game's own push-buffer writes (BeginPush/EndPush): a guest buffer.
    uint32_t push = 0, push_dwords = 0, push_begun = 0;
    // Immediate mode (Begin, SetVertexData*, End): the GPU's attribute
    // registers, which ones were ever written, and the vertices since Begin.
    uint32_t immediate = 0;  // the primitive between Begin and End, 0 outside
    float attribute_values[16][4] = {};
    uint32_t attributes_written = 0;
    // The transforms SetModelView gives the GPU (D3D's matrix layout, row
    // vectors), which the fixed-function pipeline uses in place of
    // World × View × Projection while D3D's dirty flag 0x80000000 is set.
    float model_view[16] = {}, inverse_model_view[12] = {}, composite[16] = {};
    std::vector<float> immediate_vertices;  // 16 vec4s a vertex
    // Draws decoded since the last Swap, until the renderer draws them.
    // Shaders made so far, by key, and textures uploaded, by their header and address.
    std::unordered_map<uint64_t, std::vector<uint32_t>> shaders;
    // What each was made from, for the renderer's pipeline library (gfx::DrawCmd::vs_program).
    std::unordered_map<uint64_t, gfx::vsh::Program> vs_programs;
    std::unordered_map<uint64_t, gfx::nv2a::CombinerSetup> fs_setups;
    // Programs that place vertices by one matrix, oPos = (v0·c[n], …, v0·c[n+3])
    // as the first four instructions: n (0-191), or -1 (flat_overlay).
    std::unordered_map<uint64_t, int> position_matrix;
    struct CachedTexture {
        uint32_t id = 0;
        std::vector<uint8_t> texels;  // the guest's texels when uploaded, compared byte for byte
        uint32_t checked = ~0u;  // the frame the texels were last compared
        uint64_t sample = 0;     // sample_bytes of them
        uint32_t changed = 0;    // the frame they last changed in
        uint32_t checked_lock = 0;  // the lock count when they were last compared
        uint32_t dims = 2;
        bool linear = false;
        uint32_t width = 0, height = 0, depth = 1;
    };
    std::unordered_map<uint64_t, CachedTexture> textures;
    // Vertex streams converted once and kept in the renderer's vertex arena
    // (vertex_arrays): one stream's attributes for one run of vertices, with
    // the guest bytes they came from, compared at every draw that uses them.
    struct VertexRun {
        uint32_t at = 0, stride = 0, count = 0;
        uint64_t layout = 0;
        std::vector<uint8_t> src;
        uint64_t offset = 0, bytes = 0;  // in the arena; bytes 0 for none
        uint32_t attr_offset[16] = {};
        uint32_t used = 0;               // the swap it was last drawn in
        uint32_t changes = 0;            // its bytes changed this many draws in a row
        uint32_t unstable_until = 0;     // a stream that keeps changing goes the old way until this swap
        uint32_t changed = 0;            // the swap its bytes last changed in
        uint32_t checked_swap = UINT32_MAX, checked_epoch = 0;  // when they were last found the same
        uint64_t sample = 0;             // sample_bytes of src
    };
    std::unordered_map<uint64_t, VertexRun> vertex_runs;
    // Textures the game renders into: the renderer's image for each, by the
    // guest address of its texels. Their texels live on the GPU; guest
    // memory keeps what the CPU last put there, hashed, so a CPU write (the
    // memory reused for another resource) takes over from the GPU's image.
    struct TargetTexture {
        CachedTexture texture;  // the renderer's id, the shape
        uint32_t format = 0;
        uint64_t guest = 0;     // hash of the guest bytes when last rendered
        uint32_t pitch = 0;     // bytes a row, for a linear one
        uint32_t checked_swap = ~0u, checked_epoch = ~0u;  // when guest was last compared with the memory
    };
    std::map<uint32_t, TargetTexture> targets;
    // Depth surfaces other than the device's, as the renderer's depth
    // targets, by the guest address of their data.
    struct DepthTarget {
        uint32_t id = 0, width = 0, height = 0;
        bool linear = false;
        uint64_t drawn = 0;   // bumped each time it's bound: what it holds may have changed
        uint64_t guest = 0;   // hash of its guest memory, which the GPU never writes
        uint32_t checked_swap = ~0u, checked_epoch = ~0u;
        // Its depth and stencil as the console's memory holds them, for draws
        // that read the surface as an A8R8G8B8 texture (depth_texture).
        CachedTexture packed;
        uint64_t packed_drawn = ~0ull;
    };
    std::map<uint32_t, DepthTarget> depth_targets;
    std::vector<std::string> said;  // notices given once
};
Device* g_device = nullptr;
void apply_settings(Device& d, const platform::Settings& s);
// A trace of what the game draws, for finding rendering bugs (docs/graphics.md):
// QILIN_TRACE_DRAWS=N[,M] logs every clear, draw and copy of frames N to M
// (default N) on stderr, with the state that matters; QILIN_TRACE_MATCH=a|b
// keeps only the lines with one of them in; QILIN_TRACE_DUMP=dir also writes
// those frames there (frame_NNNNN.png) and what each render target a logged
// draw samples holds (swap_n_address.pam).
bool trace_draws(const Device& d);
bool trace_wanted(const std::string& line);
bool g_trace_matched = false;  // a line was logged this frame: QILIN_TRACE_DUMP keeps it

// A capture (F9): the next kCaptureFrames frames as the draw trace gives
// them, with every detail, the frames themselves and what each render
// target a draw samples holds, in one .tar in the data folder's captures/.
// It's how a picture fault on a GPU this was never run on is seen: the
// player presses F9 while it shows and sends the file.
constexpr uint32_t kCaptureFrames = 12;
struct Capture {
    std::filesystem::path dir;
    FILE* log = nullptr;  // open while capturing
    uint32_t until = 0;   // the last swap captured
    std::unordered_map<uint32_t, uint32_t> version, dumped;  // a render target's draws so far, and at its last dump
    uint32_t files = 0;
} g_capture;
void update_aspect(rt::Context& c, Device& d, bool settings_changed);

Device& device(const char* fn) {
    if (!g_device) unmodelled(fn, "a call before Direct3D_CreateDevice");
    return *g_device;
}

// Guest memory for the runtime's resource headers: a contiguous block, bump-allocated.
uint32_t guest_alloc(Device& d, uint32_t bytes) {
    bytes = (bytes + 15) & ~15u;
    if (!d.heap || d.heap_used + bytes > d.heap_size) {
        d.heap_size = 0x10000;
        d.heap = contiguous(d.heap_size, 0, kPhysicalLimit - 1, 0x1000, 4);
        d.heap_used = 0;
        if (!d.heap) rt::trap(0, "D3D: out of guest memory for resource headers");
    }
    const uint32_t at = d.heap + d.heap_used;
    d.heap_used += bytes;
    return at;
}

// A D3DSurface header: Common, Data, Lock, Format, Size, Parent (24 bytes).
// Frame buffers are linear, so the size goes in Size: width - 1, height - 1
// and the pitch in 64-byte units, less one.
uint32_t make_surface(rt::Context& c, Device& d, uint32_t format, uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
    const uint32_t s = guest_alloc(d, 24), pitch = width * bytes_per_pixel;
    // The pixels: contiguous memory, as the original allocates frame
    // buffers, so a CPU lock (the disc-error screen draws into the front
    // buffer) writes guest memory. The renderer's image is what's drawn and shown.
    const uint32_t data = contiguous(pitch * height, 0, kPhysicalLimit - 1, 0x10000, 4);
    if (!data) rt::trap(0, "D3D: out of contiguous memory for a frame buffer");
    st(c, s, kTypeSurface | kIntRef1 | kD3DCreated);  // held by the device; no outside reference yet
    st(c, s + 4, data);                                // Data: the physical address
    st(c, s + 8, 0);
    st(c, s + 12, (format << 8) | (2u << 4) | 1u);     // 2 dimensions, DMA context A
    st(c, s + 16, (width - 1) | ((height - 1) << 12) | ((pitch / 64 - 1) << 24));
    st(c, s + 20, 0);
    return s;
}

// The device fields the lifted resource code reads: the frame buffers (the
// one being drawn, the front buffer, the next), the render target, the
// depth buffer and the multisample type.
void mirror_device(rt::Context& c, const Device& d) {
    const uint32_t n = uint32_t(d.frame_buffers.size()), back = d.renderer->back_index();
    st(c, kDeviceStorage + kDevFrameBuffers, d.frame_buffers[back]);
    st(c, kDeviceStorage + kDevFrameBuffers + 4, d.frame_buffers[d.renderer->front_index()]);
    st(c, kDeviceStorage + kDevFrameBuffers + 8, n > 2 ? d.frame_buffers[(back + 1) % n] : 0);
    st(c, kDeviceStorage + kDevMultisample, kMultisampleNone);
    // The multisample type's horizontal and vertical sample factors as
    // floats (+0x968, +0x96C), as the original's frame-buffer set-up keeps
    // them: D3D_CommonSetMultiSampleModeAndScale derives the scale from them.
    st(c, kDeviceStorage + 0x968, std::bit_cast<uint32_t>(float(kMultisampleNone >> 4 & 0xF)));
    st(c, kDeviceStorage + 0x96C, std::bit_cast<uint32_t>(float(kMultisampleNone & 0xF)));
}

uint32_t add_ref(rt::Context& c, uint32_t r) {  // D3DResource_AddRef, as the original
    const uint32_t common = ld(c, r);
    if ((common & kRefMask) == 0 && (common & kTypeMask) == kTypeSurface && ld(c, r + 20)) add_ref(c, ld(c, r + 20));
    st(c, r, common + 1);
    return (common + 1) & kRefMask;
}

// ---------------------------------------------------------------- vblanks
// The display's vertical blank: 60/1.001 Hz for NTSC, which the console's
// settings select (ExQueryNonVolatileSetting). Swap waits for the vblank
// after the presentation interval, then the game's vblank callback runs for
// each vblank that went by, the last one flagged D3DVBLANK_SWAPDONE: the game
// takes its time step from that count.
constexpr double kVblankHz = 60000.0 / 1001.0;

uint32_t vblanks_elapsed(const Device& d) {
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - d.epoch).count();
    return uint32_t(s * kVblankHz);
}

void run_vblanks(rt::Context& c, Device& d, uint32_t until, bool swap_done) {
    while (d.vblanks < until) {
        ++d.vblanks;
        if (!d.vblank_callback) continue;
        // D3DVBLANKDATA: VBlank (the count), Swap (swaps so far), Flags.
        st(c, d.vblank_data, d.vblanks);
        st(c, d.vblank_data + 4, d.swaps);
        st(c, d.vblank_data + 8, swap_done && d.vblanks == until ? 1u : 0u);
        call_guest(c, d.vblank_callback, {d.vblank_data});
    }
}

bool g_capture_holds_step = false;  // a capture is being made, or its last frame has just been

// The game's frame clock at 0xFC030 (its per-frame function's first call):
// the time step in seconds (0x5096C0) and the 300 Hz ticks it makes (count
// 0x5096C4, carry 0x5096C8), as the original computes them: the vblanks
// since the last present (0x5096D0, the vblank callback's) over the rate
// (50 for PAL, else 60), the unclamped value at 0x5096B8, clamped to five
// vblanks (5.0 at 0x32B184 over the rate), times 300 (0x32B180) plus the
// carry, truncated; and the performance counter since start-up's low word
// at 0x5096BC. With the frame rate unlocked (60, display, unlimited) the
// time since its last call stands in for the vblanks, so the game moves by
// the time that passed at any frame rate (it was written for a varying step;
// the console never gave it less than 1/30 s, and the few places that
// assumed so are fixed in game_fixes.cpp). Its ticks then run as they would:
// whatever is done per tick happens as often as before.
void frame_clock(rt::Context& c) {
    Device* dp = g_device;
    const rt::Mem m = mem(c);
    auto f32 = [&](uint32_t a) { return std::bit_cast<float>(rt::ld32(m, a, 0)); };
    auto put = [&](uint32_t a, float v) { rt::st32(m, a, std::bit_cast<uint32_t>(v), 0); };
    const int rate = rt::ld32(m, 0x5096E0, 0) && !rt::ld32(m, 0x5096E4, 0) ? 50 : 60;
    const float most = f32(0x32B184) / float(rate);
    rt::st32(m, 0x5096BC, uint32_t(tsc_now()) - rt::ld32(m, 0x50E0D8, 0), 0);
    float step;
    const bool unlocked = dp && dp->frame_rate != platform::Settings::FrameRate::Original;
    if (unlocked) {
        const auto now = std::chrono::steady_clock::now();
        const auto last = std::exchange(dp->clock_last, now);
        // The first frame: two vblanks, as the console's first.
        step = last.time_since_epoch().count() ? std::chrono::duration<float>(now - last).count() : 2.0f / float(rate);
        // QILIN_FIXED_STEP=seconds[,after]: that step every frame whatever the
        // time, from `after` seconds in (to try a high refresh rate's step on
        // a slower machine).
        static const auto fixed = [] {
            std::pair<float, float> f{0.0f, 0.0f};
            if (const char* v = std::getenv("QILIN_FIXED_STEP")) {
                f.first = std::strtof(v, nullptr);
                if (const char* comma = std::strchr(v, ',')) f.second = std::strtof(comma + 1, nullptr);
            }
            return f;
        }();
        static const auto started = now;
        if (fixed.first > 0 && std::chrono::duration<float>(now - started).count() >= fixed.second) step = fixed.first;
        // A capture's frames take far longer than the game's (each is read
        // back and written out): they get the step the game had before it, so
        // the capture shows what the game draws at the player's frame rate.
        static float before_capture = 0.0f;
        if (g_capture_holds_step && before_capture > 0) step = before_capture;
        else before_capture = step;
    } else {
        // fild of the count (unsigned: 2^32 added to a negative one, 0x32B144), over the rate.
        const uint32_t vblanks = rt::ld32(m, 0x5096D0, 0);
        step = float(double(vblanks) / double(rate));
    }
    put(0x5096B8, step);
    const float dt = step > most ? most : step;
    put(0x5096C0, dt);
    const float x = dt * f32(0x32B180) + f32(0x5096C8);
    const int32_t ticks = int32_t(x);  // cvttss2si
    rt::st32(m, 0x5096C4, uint32_t(ticks), 0);
    put(0x5096C8, x - float(ticks));
    ret(c, c.eax, 0);
}

// Waiting for the next frame, at the end of Swap. Original: the console's
// vblank after the game's presentation interval, by the clock, as the
// console's Swap waits. 60: sixty frames a second by the clock (fewer on a
// slower display). Display and unlimited, with vsync and a window: the
// window's next image is taken here
// (acquire_early), which waits for the display's refresh when it has no
// free one, so the game reads its input for the next frame just after the
// refresh. Without vsync, the display's refresh rate is kept by the clock
// (display), or there's no wait (unlimited). The vblank callback then runs
// for each vblank that went by.
//
// The guest's other threads run only while this one waits (one processor:
// the game's loader has a lower priority than its main thread, and on the
// console ran in Swap's wait for the vblank), so a frame always gives up the
// processor for kYield at least, and the wait for the display is a sleep
// until just before the refresh (from when the last acquire returned), then
// the acquire: the processor is held a millisecond or so at most.
constexpr auto kYield = std::chrono::milliseconds(2);
void frame_pacing(rt::Context& c, Device& d) {
    using Rate = platform::Settings::FrameRate;
    using Clock = std::chrono::steady_clock;
    if (d.frame_rate == Rate::Original) {
        const uint32_t target = std::max(d.vblanks + d.interval, vblanks_elapsed(d) + 1);
        // The thread blocks until then, as on the console: other guest threads run.
        host::sched::sleep_until(d.epoch + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(double(target) / kVblankHz)));
        mirror_device(c, d);
        run_vblanks(c, d, target, true);
        return;
    }
    float hz = d.config.window ? d.config.window->refresh_rate() : 0.0f;
    if (hz < 24.0f || hz > 1000.0f) hz = float(kVblankHz);
    if (d.frame_rate == Rate::Sixty) hz = std::min(hz, float(kVblankHz));
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    const auto now = Clock::now();
    bool by_display = false;
    if (d.picture.vsync && d.config.window && d.frame_rate != Rate::Sixty) {
        const auto before_refresh = d.last_refresh + period - std::chrono::microseconds(1500);
        host::sched::sleep_until(std::max(now + kYield, before_refresh));
        by_display = d.renderer->acquire_early();
        if (by_display) d.last_refresh = Clock::now();
    }
    if (!by_display) {
        auto wake = now + kYield;
        if (d.frame_rate == Rate::Display || d.frame_rate == Rate::Sixty) {
            d.next_frame += period;
            if (d.next_frame < now - period || d.next_frame > now + 2 * period) d.next_frame = now;  // fell behind, or a new start
            wake = std::max(wake, d.next_frame);
        }
        if (wake > Clock::now()) host::sched::sleep_until(wake);
    }
    mirror_device(c, d);
    run_vblanks(c, d, std::max(d.vblanks, vblanks_elapsed(d)), true);
}

// ---------------------------------------------------------------- frames out
void deliver_frame(Device& d) {
    const uint64_t n = d.renderer->frames_presented();
    static const char* trace_dir = std::getenv("QILIN_TRACE_DUMP");
    if (!d.config.dump_dir.empty() || g_capture.log || (trace_dir && trace_draws(d) && std::exchange(g_trace_matched, false))) {
        gfx::Image im = d.renderer->read_front_buffer();
        for (size_t i = 0; i < im.rgba.size(); i += 4) {  // as the TV gets it: through the gamma ramp, alpha unused
            for (int k = 0; k < 3; ++k) im.rgba[i + k] = d.gamma[k][im.rgba[i + k]];
            im.rgba[i + 3] = 255;
        }
        const std::string path = (g_capture.log ? g_capture.dir.string() : d.config.dump_dir.empty() ? std::string(trace_dir) : d.config.dump_dir) +
                                 strf("/frame_%05llu.png", (unsigned long long)n);
        if (!gfx::write_png(path, im)) std::fprintf(stderr, "qilin: can't write %s\n", path.c_str());
        // A capture's first four frames at the render resolution too (full_NNNNN.png): what the
        // reduction to the game's size averages away (thin stripes, edges) shows there.
        if (g_capture.log && g_capture.until - d.swaps > kCaptureFrames - 4) {
            gfx::Image full = d.renderer->read_front_buffer_full();
            for (size_t i = 0; i < full.rgba.size(); i += 4) {
                for (int k = 0; k < 3; ++k) full.rgba[i + k] = d.gamma[k][full.rgba[i + k]];
                full.rgba[i + 3] = 255;
            }
            gfx::write_png(g_capture.dir.string() + strf("/full_%05llu.png", (unsigned long long)n), full);
        }
    }
    if (d.config.max_frames && n >= d.config.max_frames) {
        std::fflush(stderr);
        std::printf("qilin_run: %llu frames presented; stopping as asked (--frames)\n", (unsigned long long)n);
        std::fflush(stdout);
        std::exit(0);
    }
}

// ---------------------------------------------------------------- the API
// VOID Direct3D_SetPushBufferSize(DWORD PushBufferSize, DWORD KickOffSize):
// sizes of the GPU command buffer, which the original keeps for CreateDevice.
// The runtime has no push buffer; the values are kept where the original keeps them.
void set_push_buffer_size(rt::Context& c) {
    st(c, kPushBufferSize, arg(c, 0));
    st(c, kKickOffSize, arg(c, 1));
    ret(c, c.eax, 8);
}

// HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE, HWND, DWORD BehaviorFlags,
//     D3DPRESENT_PARAMETERS*, IDirect3DDevice8** ppReturnedDeviceInterface)
void init_push_sink(rt::Context& c, Device& d);
void absent_colour_registers(rt::Context& c, uint32_t vs);

// The guest-visible part of the original's device initialisation, after the
// frame buffers (0x17AB79 on): the fixed-function vertex shader for
// XYZRHW and then XYZ, the passthrough shader object at 0x184360 cleared
// (flags 0x10), then the default state (the lifted 0x179FE0: identity
// transforms, the render and texture-stage state defaults, no textures).
// The render target the original sets here is mirror_device's.
void init_device_state(rt::Context& c) {
    st(c, kDeviceStorage + 0x790, 0x184478);
    host::call_guest(c, 0x1739F0, {4});  // D3DFVF_XYZRHW
    host::call_guest(c, 0x1739F0, {2});  // D3DFVF_XYZ
    absent_colour_registers(c, kFixedFunctionShader);  // what that call pushed of them
    for (uint32_t k = 0; k < 0x46; ++k) st(c, 0x184360 + 4 * k, 0);
    st(c, 0x184364, 0x10);
    // SetRenderTarget(back buffer, depth buffer), which also sets the
    // multisample scale and the viewport the vertex programs depend on.
    st(c, kDeviceStorage + kDevRenderTarget, 0);
    st(c, kDeviceStorage + kDevDepth, 0);
    host::call_guest(c, 0x174170, {ld(c, kDeviceStorage + kDevFrameBuffers), g_device->depth});
    host::call_guest(c, 0x179FE0, {});
}

void create_device(rt::Context& c) {
    const uint32_t pp = arg(c, 4), out = arg(c, 5);
    if (g_device) unmodelled("Direct3D_CreateDevice", "creating a second device");
    // D3DPRESENT_PARAMETERS: BackBufferWidth, BackBufferHeight, BackBufferFormat,
    // BackBufferCount, MultiSampleType, SwapEffect, hDeviceWindow, Windowed,
    // EnableAutoDepthStencil, AutoDepthStencilFormat, Flags,
    // FullScreen_RefreshRateInHz, FullScreen_PresentationInterval, BufferSurfaces[3],
    // DepthStencilSurface.
    const uint32_t width = ld(c, pp), height = ld(c, pp + 4), format = ld(c, pp + 8), count = std::max<uint32_t>(ld(c, pp + 12), 1);
    const uint32_t multisample = ld(c, pp + 16), auto_depth = ld(c, pp + 32), depth_format = ld(c, pp + 36);
    const uint32_t interval = ld(c, pp + 48);
    if (ld(c, pp + 52) || ld(c, pp + 56) || ld(c, pp + 60) || ld(c, pp + 64))
        unmodelled("Direct3D_CreateDevice", "frame buffers the title allocates itself (BufferSurfaces)");
    if (format != kFmtA8R8G8B8 && format != kFmtX8R8G8B8 && format != kFmtLinA8R8G8B8 && format != kFmtLinX8R8G8B8)
        unmodelled("Direct3D_CreateDevice", strf("back buffer format %#x", format));
    if (auto_depth && depth_format != kFmtD24S8 && depth_format != kFmtLinD24S8)
        unmodelled("Direct3D_CreateDevice", strf("depth buffer format %#x", depth_format));
    if (count > 2) unmodelled("Direct3D_CreateDevice", strf("%u back buffers", count));
    if (interval & 0x80000000u) unmodelled("Direct3D_CreateDevice", "D3DPRESENT_INTERVAL_IMMEDIATE");

    auto d = std::make_unique<Device>();
    d->config = d3d_config();
    d->width = width;
    d->height = height;
    d->interval = interval == 0 ? 1 : interval == 4 ? 3 : interval == 8 ? 4 : interval;  // ONE, TWO, THREE (4), FOUR (8)
    gfx::Config gc;
    gc.width = width;
    gc.height = height;
    gc.back_buffers = count;
    gc.depth_stencil = auto_depth != 0;
    gc.gpu = d->config.gpu;
    gc.pipeline_cache = d->config.pipeline_cache;
    if (platform::Window* w = d->config.window) {
        gc.surface.instance_extensions = w->vulkan_instance_extensions();
        gc.surface.loader = w->vulkan_loader();
        gc.surface.create = [w](void* instance, std::string* error) { return w->create_surface(instance, error); };
        gc.surface.size = [w](uint32_t* pw, uint32_t* ph) {
            if (w->minimized()) *pw = *ph = 0;
            else w->pixel_size(pw, ph);
        };
    }
    std::string error;
    d->renderer = gfx::Renderer::create(gc, &error);
    if (!d->renderer) {
        std::fflush(stdout);
        std::fprintf(stderr, "qilin: can't start the renderer: %s\n", error.c_str());
        std::exit(3);
    }
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 256; ++i) d->gamma[k][i] = uint8_t(i);
    if (d->config.settings) apply_settings(*d, *d->config.settings);
    update_aspect(c, *d, true);

    // The frame buffers as guest surfaces (linear A8R8G8B8 / X8R8G8B8, as the
    // original makes them), and the depth buffer (linear D24S8).
    const uint32_t linear = format == kFmtX8R8G8B8 || format == kFmtLinX8R8G8B8 ? kFmtLinX8R8G8B8 : kFmtLinA8R8G8B8;
    for (uint32_t i = 0; i < count + 1; ++i) d->frame_buffers.push_back(make_surface(c, *d, linear, width, height, 4));
    if (auto_depth) d->depth = make_surface(c, *d, kFmtLinD24S8, width, height, 4);
    d->vblank_data = guest_alloc(*d, 12);
    d->epoch = std::chrono::steady_clock::now();

    std::printf("qilin_run: D3D device %ux%u, %u back buffer%s, %s, presentation interval %u, multisample type %#x; Vulkan on %s\n",
                width, height, count, count == 1 ? "" : "s", auto_depth ? "D24S8 depth" : "no depth buffer", d->interval,
                multisample, d->renderer->device_name().c_str());
    if (multisample != kMultisampleNone && multisample != 0)
        unmodelled("Direct3D_CreateDevice", strf("multisample type %#x", multisample));
    st(c, kDeviceStorage + kDevRefs, 1);  // the device's reference count, which Release reads
    g_device = d.release();
    st(c, kGlobalDevice, kDeviceStorage);
    mirror_device(c, *g_device);
    init_push_sink(c, *g_device);
    init_device_state(c);
    if (out) st(c, out, kDeviceStorage);
    ret(c, 0, 24);  // D3D_OK
}

// D3DSurface* D3DDevice_GetBackBuffer2(INT BackBuffer): -1 is the front
// buffer, 0 the buffer being drawn, 1 the next one. Adds a reference.
void get_back_buffer2(rt::Context& c) {
    Device& d = device("D3DDevice_GetBackBuffer2");
    const int32_t i = int32_t(arg(c, 0));
    const uint32_t n = uint32_t(d.frame_buffers.size());
    uint32_t index = 0;
    if (i == -1) index = d.renderer->front_index();
    else if (i >= 0 && uint32_t(i) < n - 1) index = (d.renderer->back_index() + uint32_t(i)) % n;
    else unmodelled("D3DDevice_GetBackBuffer2", strf("back buffer %d", i));
    const uint32_t s = d.frame_buffers[index];
    add_ref(c, s);
    ret(c, s, 4);
}

// D3DSurface* D3DDevice_GetDepthStencilSurface2(): adds a reference; null without one.
void get_depth_stencil_surface2(rt::Context& c) {
    Device& d = device("D3DDevice_GetDepthStencilSurface2");
    if (d.depth) add_ref(c, d.depth);
    ret(c, d.depth, 0);
}

// D3DSurface* D3DDevice_GetRenderTarget2(): the back buffer until SetRenderTarget exists.
void get_render_target2(rt::Context& c) {
    Device& d = device("D3DDevice_GetRenderTarget2");
    const uint32_t s = d.frame_buffers[d.renderer->back_index()];
    add_ref(c, s);
    ret(c, s, 0);
}

// VOID D3D_BlockOnResource(D3DResource*): waits until the GPU is done with
// the resource, before the CPU writes it. The renderer copies what it draws
// from guest memory when it draws, so the GPU never holds guest memory.
// A frame buffer is the exception: the CPU locks it to read or write
// pixels (the disc-error screen draws its message into the front buffer),
// so its image is copied into guest memory first (cpu_frame_to_guest).
void cpu_frame_to_guest(rt::Context& c, Device& d, uint32_t index);
void sweep_vertex_runs(Device& d, uint32_t idle);
void flush_cpu_frames(rt::Context& c, Device& d);
void hand_back_target(rt::Context& c, Device& d, uint32_t data);
Device::TargetTexture& texture_target(rt::Context& c, Device& d, uint32_t data, uint32_t w, uint32_t h, uint32_t fmt, uint32_t pitch);

void block_on_resource(rt::Context& c) {
    if (g_device) ++g_device->memory_epoch;
    if (g_device) g_device->vertex_buffer_locked(ld(c, arg(c, 0) + 4));  // should it be a vertex buffer
    // A texture the GPU drew or copied into: its texels come back to guest
    // memory before the CPU touches them (a lock or a destroy reaches here).
    if (g_device) hand_back_target(c, *g_device, ld(c, arg(c, 0) + 4));
    if (g_device)
        for (uint32_t i = 0; i < g_device->frame_buffers.size(); ++i)
            if (g_device->frame_buffers[i] == arg(c, 0)) cpu_frame_to_guest(c, *g_device, i);
    ret(c, c.eax, 4);
}

// BYTE* D3DVertexBuffer_Lock2(D3DVertexBuffer*, DWORD Flags): the data's
// address in the contiguous-memory window, as the original returns it. (The
// original also puts a wait for the GPU in the command buffer; see BlockOnResource.)
void vertex_buffer_lock2(rt::Context& c) {
    if (g_device) g_device->vertex_buffer_locked(ld(c, arg(c, 0) + 4));  // its vertices may change: compared again at their next draw
    ret(c, ld(c, arg(c, 0) + 4) | rt::kRamCachedAlias, 8);
}

// ULONG D3DResource_Release(D3DResource*): as the original. A surface's
// parent texture is released with it, and a resource the device still holds
// (internal references) stays. One with neither is destroyed by D3D's own
// D3D_DestroyResource (lifted), which frees it to the game's allocator.
uint32_t release(rt::Context& c, uint32_t r) {
    const uint32_t common = ld(c, r);
    if ((common & kRefMask) == 0) unmodelled("D3DResource_Release", strf("releasing %08X, which has no references", r));
    if ((common & kRefMask) == 1) {
        // The last reference: a surface of a texture releases its texture,
        // and a resource the device no longer holds either is destroyed.
        if ((common & kTypeMask) == kTypeSurface && ld(c, r + 20)) release(c, ld(c, r + 20));
        if (!(ld(c, r) & kIntRefMask)) {
            host::call_guest(c, 0x175890, {r});  // D3D_DestroyResource, lifted
            return 0;
        }
    }
    const uint32_t now = ld(c, r) - 1;
    st(c, r, now);
    return now & kRefMask;
}

void resource_release(rt::Context& c) {
    const uint32_t refs = release(c, arg(c, 0));
    ret(c, refs, 4);
}

// VOID D3DResource_Register(D3DResource*, PVOID pBase): a resource loaded
// from a file (an XPR bundle) has its data offset made an address. As the
// original: Data += pBase, masked to a physical address except for push buffers.
void resource_register(rt::Context& c) {
    const uint32_t r = arg(c, 0);
    uint32_t data = ld(c, r + 4) + arg(c, 1);
    if ((ld(c, r) & kTypeMask) != kTypePushBuffer) data &= 0x0FFFFFFF;
    st(c, r + 4, data);
    ret(c, c.eax, 8);
}

// ULONG D3DDevice_Release(): as the original (0x173EF0), the device's count
// down by one, returning it; the last reference tears the device down
// (frees its resources, zeroes it), which the game does only when it
// shuts down, and stops the runtime here.
void device_release(rt::Context& c) {
    device("D3DDevice_Release");
    const uint32_t refs = ld(c, kDeviceStorage + kDevRefs);
    if (refs <= 1) unmodelled("D3DDevice_Release", "releasing the device's last reference (tearing it down)");
    st(c, kDeviceStorage + kDevRefs, refs - 1);
    ret(c, refs - 1, 0);
}

// BOOL D3DResource_IsBusy(D3DResource*): as the original (0x175AD0), busy
// while the device holds it (an internal reference on it, or for a surface
// on its texture); else the GPU is done with it (draws copy what they
// read, so it never runs behind the CPU here), and its lock fence is cleared.
void resource_is_busy(rt::Context& c) {
    uint32_t r = arg(c, 0);
    auto held = [&](uint32_t x) { return (ld(c, x) & kIntRefMask) != 0; };
    if ((ld(c, r) & kTypeMask) == kTypeSurface && ld(c, r + 20)) {
        if (held(r) || held(ld(c, r + 20))) return ret(c, 1, 4);
        r = ld(c, r + 20);
    }
    if (held(r)) return ret(c, 1, 4);
    st(c, r + 8, 0);
    ret(c, 0, 4);
}

// VOID D3DDevice_SetVerticalBlankCallback(D3DVBLANKCALLBACK)
void set_vertical_blank_callback(rt::Context& c) {
    device("D3DDevice_SetVerticalBlankCallback").vblank_callback = arg(c, 0);
    ret(c, c.eax, 4);
}

// VOID D3DDevice_SetGammaRamp(DWORD Flags, CONST D3DGAMMARAMP*): the
// display's ramp, 256 entries per channel (red, green, blue, a byte each).
// It applies to what's shown from the next frame (or at once with
// D3DSGR_IMMEDIATE, the same thing here): frames written with --dump-frames
// go through it.
void set_gamma_ramp(rt::Context& c) {
    Device& d = device("D3DDevice_SetGammaRamp");
    const uint32_t ramp = arg(c, 1);
    for (uint32_t k = 0; k < 3; ++k)
        for (uint32_t i = 0; i < 256; ++i) d.gamma[k][i] = rt::ld8(mem(c), ramp + k * 256 + i, 0);
    d.renderer->set_gamma_ramp(d.gamma);  // the window shows frames through it too
    ret(c, c.eax, 8);
}

// VOID D3DDevice_BlockUntilIdle(): waits for the GPU.
void block_until_idle(rt::Context& c) {
    device("D3DDevice_BlockUntilIdle").renderer->flush();
    ret(c, c.eax, 0);
}

// HRESULT D3DDevice_Clear(DWORD Count, CONST D3DRECT*, DWORD Flags, D3DCOLOR,
//     float Z, DWORD Stencil): the render target and depth buffer within the
//     viewport, as the original (0x176970) clears them: each rectangle given
//     (D3DRECT: x1, y1, x2, y2) clipped to the viewport, or with none, the
//     viewport itself. The game packs its characters' shadows into tiles of
//     one 256×256 texture and clears each tile through its viewport.
std::pair<uint32_t, uint32_t> apply_target(rt::Context& c, Device& d, const char* fn);
void hand_back_target(rt::Context& c, Device& d, uint32_t data);
bool trace_draws(const Device& d) {
    static const std::pair<uint32_t, uint32_t> range = [] {
        const char* e = std::getenv("QILIN_TRACE_DRAWS");
        if (!e || !*e) return std::pair<uint32_t, uint32_t>{1, 0};
        char* end = nullptr;
        const uint32_t a = uint32_t(std::strtoul(e, &end, 10));
        return std::pair<uint32_t, uint32_t>{a, *end == ',' ? uint32_t(std::strtoul(end + 1, nullptr, 10)) : a};
    }();
    return g_capture.log || (d.swaps >= range.first && d.swaps <= range.second);
}

// QILIN_TRACE_MATCH=a|b: only the lines with one of them in.
bool trace_wanted(const std::string& line) {
    static const char* match = std::getenv("QILIN_TRACE_MATCH");
    if (!match) return true;
    for (std::string_view m = match; !m.empty();) {
        const size_t bar = m.find('|');
        if (line.find(m.substr(0, bar)) != std::string::npos) return true;
        m = bar == std::string_view::npos ? std::string_view() : m.substr(bar + 1);
    }
    return false;
}
void trace_line(const std::string& line) {
    if (g_capture.log) {
        std::fputs(line.c_str(), g_capture.log);
        g_trace_matched = true;
        // What a render target holds changes with each draw and clear into it.
        if (line.starts_with("draw ") || line.starts_with("clear "))
            if (const size_t at = line.find(" rt="); at != std::string::npos) ++g_capture.version[uint32_t(std::strtoul(line.c_str() + at + 4, nullptr, 16))];
        return;
    }
    if (trace_wanted(line)) std::fputs(line.c_str(), stderr), g_trace_matched = true;
}

// A capture's files packed into one (a ustar archive, nothing compressed),
// and the folder removed.
void pack_capture(const std::filesystem::path& dir) {
    const std::filesystem::path tar = std::filesystem::path(dir).concat(".tar");
    std::ofstream out(tar, std::ios::binary);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        char h[512] = {};
        std::snprintf(h, 100, "%s", entry.path().filename().string().c_str());
        std::snprintf(h + 100, 8, "%07o", 0644u);
        std::snprintf(h + 108, 8, "%07o", 0u);
        std::snprintf(h + 116, 8, "%07o", 0u);
        std::snprintf(h + 124, 12, "%011llo", (unsigned long long)data.size());
        std::snprintf(h + 136, 12, "%011o", 0u);
        std::memset(h + 148, ' ', 8);
        h[156] = '0';
        std::memcpy(h + 257, "ustar", 6);
        std::memcpy(h + 263, "00", 2);
        unsigned sum = 0;
        for (unsigned char ch : h) sum += ch;
        std::snprintf(h + 148, 8, "%06o", sum);
        out.write(h, 512).write(data.data(), std::streamsize(data.size()));
        static const char zeros[512] = {};
        if (data.size() % 512) out.write(zeros, std::streamsize(512 - data.size() % 512));
    }
    const char end[1024] = {};
    out.write(end, sizeof end);
    out.close();
    std::filesystem::remove_all(dir, ec);
}

void clear(rt::Context& c) {
    Device& d = device("D3DDevice_Clear");
    const uint32_t count = arg(c, 0), rects = arg(c, 1), flags = arg(c, 2), color = arg(c, 3), stencil = arg(c, 5);
    const uint32_t zbits = arg(c, 4);
    float z;
    std::memcpy(&z, &zbits, 4);
    if ((flags & kClearTarget) && (flags & kClearTarget) != kClearTarget)
        unmodelled("D3DDevice_Clear", strf("clearing some colour channels only (flags %#x)", flags));
    if (flags & ~(kClearTarget | kClearZBuffer | kClearStencil)) unmodelled("D3DDevice_Clear", strf("flags %#x", flags));
    const int32_t vx1 = int32_t(ld(c, kDeviceStorage + kDevViewport)), vy1 = int32_t(ld(c, kDeviceStorage + kDevViewport + 4));
    const int32_t vx2 = vx1 + int32_t(ld(c, kDeviceStorage + kDevViewport + 8)), vy2 = vy1 + int32_t(ld(c, kDeviceStorage + kDevViewport + 12));
    std::vector<gfx::Rect> list;
    const uint32_t n = rects && count ? count : 1;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t x1 = vx1, y1 = vy1, x2 = vx2, y2 = vy2;
        if (rects && count) {
            x1 = std::max(vx1, int32_t(ld(c, rects + 16 * i))), y1 = std::max(vy1, int32_t(ld(c, rects + 16 * i + 4)));
            x2 = std::min(vx2, int32_t(ld(c, rects + 16 * i + 8))), y2 = std::min(vy2, int32_t(ld(c, rects + 16 * i + 12)));
        }
        if (x2 > x1 && y2 > y1) list.push_back({x1, y1, uint32_t(x2 - x1), uint32_t(y2 - y1)});
    }
    if (list.empty()) {  // nothing inside the viewport: nothing to do
        ret(c, 0, 24);
        return;
    }
    uint32_t f = 0;
    if (flags & kClearTarget) f |= gfx::kClearColor;
    if (flags & kClearZBuffer) f |= gfx::kClearDepth;
    if (flags & kClearStencil) f |= gfx::kClearStencil;
    const float rgba[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f,
                           float(color >> 24) / 255.0f};
    apply_target(c, d, "D3DDevice_Clear");
    if (trace_draws(d))
        trace_line(strf("clear %u rt=%08X flags=%x color=%08X z=%g s=%u rects=%zu first=%d,%d %ux%u\n", d.swaps,
                     ld(c, ld(c, kDeviceStorage + kDevRenderTarget) + 4), flags, color, double(z), stencil & 0xFF, list.size(), list[0].x, list[0].y,
                     list[0].w, list[0].h));
    d.renderer->clear(list.data(), uint32_t(list.size()), f, rgba, z, stencil & 0xFF);
    ret(c, 0, 24);
}

// The settings in effect: the picture and texture filtering, the volume,
// the keyboard's keys and rumble (the settings menu calls it as they change).
void apply_settings(Device& d, const platform::Settings& s) {
    gfx::Picture& p = d.picture;
    p.scaling = gfx::Picture::Scaling(uint8_t(s.scaling));
    p.smooth = s.smooth;
    p.fsr = s.fsr;
    p.sharpness = float(s.sharpness) / 100.0f;
    p.smaa = s.smaa;
    p.look = gfx::Picture::Look(uint8_t(s.look));
    p.scanlines = float(s.scanlines) / 100.0f;
    p.mask = float(s.mask) / 100.0f;
    p.vsync = s.vsync;
    p.low_latency = s.low_latency;
    d.frame_rate = s.frame_rate;
    p.aspect = d.aspect ? d.aspect : 16.0f / 9.0f;  // a new shape comes with the game's next frame (update_aspect)
    d.renderer->set_picture(p);
    d.renderer->set_anisotropy(s.anisotropy);
    set_output_volume(float(s.volume) / 100.0f);
    if (platform::Input* in = d.config.input) {
        in->set_keys(s.keys);
        in->set_rumble(s.rumble);
    }
}

// The shape the settings ask for (auto: the screen the window is on, looked
// at every 64 frames, so moving the window to another screen follows it),
// given to the game between frames: the frame it draws next is the first
// at the new shape, and is shown at it.
//
// The render resolution with it (auto: the size the picture is shown at,
// looked at as often), the renderer's images made again at it when it changes.
void update_aspect(rt::Context& c, Device& d, bool settings_changed) {
    const platform::Settings* s = d.config.settings;
    if (!s || (!settings_changed && d.aspect && d.swaps % 64)) return;
    const float screen = d.config.window ? d.config.window->screen_aspect() : 0.0f;
    const float aspect = game_aspect_ratio(platform::aspect_ratio(*s, screen));
    if (aspect != d.aspect) {
        set_game_aspect(mem(c), aspect, true);
        d.aspect = aspect;
        d.picture.aspect = aspect;
        d.renderer->set_picture(d.picture);
    }
    uint32_t shown = 480;
    if (d.config.window) {
        uint32_t ww = 0, wh = 0;
        d.config.window->pixel_size(&ww, &wh);
        if (ww && wh) shown = std::min(wh, uint32_t(float(ww) / aspect));
    }
    uint32_t sx = 1, sy = 1;
    platform::resolution_scale(*s, aspect, shown, &sx, &sy);
    d.renderer->set_resolution_scale(sx, sy);  // nothing if it's the same
}

// The window's events (the settings menu's first); closing the window, or
// the menu's Quit, stops the runtime.
void pump_window(const Device& d) {
    platform::Window* w = d.config.window;
    if (!w || !host::sched::on_main_thread()) return;  // SDL's events belong to the first thread (macOS)
    if (ui::Menu* m = d.menu.get()) w->pump(d.config.input, [m](const SDL_Event& e) { return m->handle(e); });
    else w->pump(d.config.input);
    if (w->quit_requested() || (d.menu && d.menu->quit_requested())) {
        std::fflush(stderr);
        std::printf("qilin_run: the window was closed after %u frames\n", d.swaps);
        std::fflush(stdout);
        std::exit(0);
    }
}

// Long frames are logged (stderr, which the player kit keeps in run.log),
// with where their time went: the game's own work up to Swap (and, within
// it, pipelines the GPU's compiler made for its draws and textures made
// or written), and presenting and pacing. A stutter report then says
// which it was. Long is over 50 ms, or over 10 ms and two and a half times
// what frames have been taking (a dropped frame at a high refresh rate);
// at most 40 of the second kind a second, and 2000 a run.
void long_frames(Device& d, std::chrono::steady_clock::time_point present_from) {
    const auto now = std::chrono::steady_clock::now();
    const gfx::Renderer::Costs costs = d.renderer->take_costs();
    const double read_ms = double(take_file_read_ns()) / 1e6;
    static double usual = 0;  // ms, a running average
    static uint32_t logged = 0, this_second = 0;
    static auto second = now;
    if (now - second >= std::chrono::seconds(1)) second = now, this_second = 0;
    if (d.frame_began != std::chrono::steady_clock::time_point{}) {
        auto ms = [](auto t) { return std::chrono::duration<double, std::milli>(t).count(); };
        const double total = ms(now - d.frame_began);
        const bool dropped = usual > 0 && total > 10 && total > usual * 2.5 && this_second < 40 && logged < 2000;
        if ((total > 50 && total > usual * 1.5) || dropped) {
            ++logged, ++this_second;
            std::fprintf(stderr,
                         "qilin: frame %u took %.1f ms (usually %.1f): the game %.1f ms (reading and writing files %.1f ms; %u pipelines made for draws, %.1f "
                         "ms; %u textures made or written, %.1f ms), presenting %.1f ms; ended at %.3f s on the steady clock\n",
                         d.swaps, total, usual, ms(present_from - d.frame_began), read_ms, costs.pipelines, costs.pipeline_ms, costs.textures,
                         costs.texture_ms, ms(now - present_from), std::chrono::duration<double>(now.time_since_epoch()).count());
        }
        usual = usual > 0 ? usual + (std::min(total, usual * 4) - usual) * 0.05 : total;
    }
    d.frame_began = now;
}

bool g_capture_wanted = false;  // a test run asks for a capture

// A test run: if the data folder has selftest.txt when the game starts, its
// lines are played, each "SECONDS what": `key NAME [held SECONDS]` holds a
// key (SDL's name, as in the settings file; a quarter of a second unless
// said), `capture` is F9, `quit` ends the run. Seconds count from the first
// frame. The file is renamed selftest.ran first, so it runs once. It lets a
// build be tried on a player's machine, on their GPU, without them playing
// it: the log and the captures say what happened.
struct TestStep {
    double at = 0;
    enum What : uint8_t { Down, Up, Capture, Quit } what = Down;
    std::string key;
};
void test_run(Device& d) {
    static std::vector<TestStep> steps;
    static size_t next = 0;
    static const auto began = std::chrono::steady_clock::now();
    static const bool loaded = [&] {
        const std::filesystem::path file = std::filesystem::path(d.config.settings_folder) / "selftest.txt";
        std::ifstream in(file);
        if (!in) return false;
        for (std::string line; std::getline(in, line);) {
            std::istringstream words(line);
            TestStep s;
            std::string what;
            if (!(words >> s.at >> what)) continue;
            if (what == "key") {
                double held = 0.25;
                std::string name, more;
                if (!(words >> name)) continue;
                // A name of two words ("Keypad 4", "Left Shift"), then perhaps the time held.
                while (words >> more) {
                    if (more == "held") words >> held;
                    else name += " " + more;
                }
                steps.push_back({s.at, TestStep::Down, name});
                steps.push_back({s.at + held, TestStep::Up, name});
            } else if (what == "capture") {
                steps.push_back({s.at, TestStep::Capture, {}});
            } else if (what == "quit") {
                steps.push_back({s.at, TestStep::Quit, {}});
            }
        }
        in.close();
        std::ranges::stable_sort(steps, {}, &TestStep::at);
        std::error_code ec;
        std::filesystem::rename(file, std::filesystem::path(file).replace_extension(".ran"), ec);
        std::printf("qilin_run: a test run from %s: %zu steps\n", file.string().c_str(), steps.size());
        std::fflush(stdout);
        return true;
    }();
    if (!loaded) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    for (; next < steps.size() && steps[next].at <= now; ++next) {
        const TestStep& s = steps[next];
        switch (s.what) {
            case TestStep::Down:
            case TestStep::Up:
                if (!platform::hold_key(s.key, s.what == TestStep::Down)) std::printf("qilin_run: the test run names a key SDL doesn't know: %s\n", s.key.c_str());
                break;
            case TestStep::Capture: g_capture_wanted = true; break;
            case TestStep::Quit:
                std::fflush(stderr);
                std::printf("qilin_run: the test run ended after %u frames\n", d.swaps);
                std::fflush(stdout);
                std::exit(0);
        }
    }
}

// A capture begins at the Swap after F9 and ends kCaptureFrames later.
void capture_step(rt::Context& c, Device& d) {
    // The frame after a capture's last still spans the time it took to pack.
    static bool g_capture_ended = false;
    if (std::exchange(g_capture_ended, false)) g_capture_holds_step = false;
    if (g_capture.log && d.swaps >= g_capture.until) {
        std::fclose(g_capture.log);
        g_capture.log = nullptr;
        pack_capture(g_capture.dir);
        const std::string where = std::filesystem::path(g_capture.dir).concat(".tar").string();
        std::printf("qilin_run: captured %u frames to %s\n", kCaptureFrames, where.c_str());
        std::fflush(stdout);
        if (d.menu) d.menu->banner("Captured " + std::to_string(kCaptureFrames) + " frames to " + where);
        d.frame_began = {};  // its frames aren't long frames
        g_capture_ended = true;
    } else if (g_capture.log) {
        std::fprintf(g_capture.log, "swap %u: time step %.5f s\n", d.swaps, double(std::bit_cast<float>(ld(c, 0x5096C0))));
    } else if ((d.menu && d.menu->take_capture()) || std::exchange(g_capture_wanted, false)) {
        std::error_code ec;
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32] = "capture";
        if (const std::tm* tm = std::localtime(&now)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
        g_capture.dir = std::filesystem::path(d.config.settings_folder) / "captures" / stamp;
        std::filesystem::create_directories(g_capture.dir, ec);
        g_capture.log = std::fopen((g_capture.dir / "trace.txt").string().c_str(), "w");
        if (!g_capture.log) {
            std::printf("qilin_run: can't write a capture to %s\n", g_capture.dir.string().c_str());
            return;
        }
        g_capture.until = d.swaps + kCaptureFrames;
        g_capture_holds_step = true;
        g_capture.version.clear(), g_capture.dumped.clear(), g_capture.files = 0;
        const platform::Settings& s = *d.config.settings;
        std::fprintf(g_capture.log,
                     "GBTGhoulies--QilinRecomp " QILIN_VERSION ", %s; render scale %ux%u, aspect %u:%u, frame rate setting %d, vsync %d, "
                     "smaa %d, fsr %d; draws numbered %u to %u, each shown in the frame numbered one more\n",
                     d.renderer->device_name().c_str(), d.renderer->resolution_scale_x(), d.renderer->resolution_scale_y(), s.aspect_w, s.aspect_h,
                     int(s.frame_rate), int(s.vsync), int(s.smaa), int(s.fsr), d.swaps,
                     g_capture.until - 1);
    }
}

// DWORD D3DDevice_Swap(DWORD Flags): shows the frame at the vblank the
// presentation interval asks for (D3DSWAP_DEFAULT) and returns the swap count.
void swap(rt::Context& c) {
    Device& d = device("D3DDevice_Swap");
    const uint32_t flags = arg(c, 0);
    if (flags & ~4u) unmodelled("D3DDevice_Swap", strf("flags %#x", flags));  // D3DSWAP_FINISH (4) only waits
    const uint32_t old_back = d.frame_buffers[d.renderer->back_index()];
    if (!d.cpu_frames.empty()) flush_cpu_frames(c, d);
    d.cpu_frames.clear();  // a lock lasts until the next Swap
    if (d.menu) d.menu->tick();  // what's drawn over this frame (the hint, the frame rate counter)
    const auto present_from = std::chrono::steady_clock::now();
    d.renderer->present();
    if (d.menu && d.config.settings && d.config.settings->performance_overlay) {
        const auto now = std::chrono::steady_clock::now();
        if (d.last_shown.time_since_epoch().count()) {
            using Rate = platform::Settings::FrameRate;
            const char* rate = d.frame_rate == Rate::Original ? "30, as the console"
                               : d.frame_rate == Rate::Sixty  ? "60"
                               : d.frame_rate == Rate::Display ? "the display's"
                                                               : "unlimited";
            const float hz = d.config.window ? d.config.window->refresh_rate() : 0.0f;
            std::vector<std::string> details;
            details.push_back(hz > 0 ? strf("target: %s  (display %.0f Hz)", rate, double(hz)) : strf("target: %s", rate));
            details.push_back(strf("vsync %s%s", d.picture.vsync ? "on" : "off", d.picture.vsync && d.picture.low_latency ? ", low latency" : ""));
            details.push_back(strf("game time step %.2f ms", double(std::bit_cast<float>(ld(c, 0x5096C0))) * 1000.0));
            d.menu->frame_shown(std::chrono::duration<double, std::milli>(now - d.last_shown).count(), std::move(details));
        }
        d.last_shown = now;
    } else {
        d.last_shown = {};
    }
    ++d.swaps;
    // The settings menu, once the window has shown a frame (it draws with the window's images).
    if (!d.menu && d.config.window && d.config.settings) {
        auto m = std::make_unique<ui::Menu>(*d.config.window, *d.renderer, *d.config.settings, d.config.settings_folder,
                                            [&d](const platform::Settings& s) {
                                                apply_settings(d, s);
                                                d.settings_changed = true;
                                            });
        if (m->ok()) d.menu = std::move(m);
    }
    // The menu open: the game stands still under it. Its frame is shown again
    // with the menu over it until it closes; the time it was open is taken off
    // the game's clock (its vblanks) and the sound's, so nothing jumps.
    if (d.menu && d.menu->open()) {
        const auto paused = std::chrono::steady_clock::now();
        pause_audio(true);
        while (d.menu->open()) {
            pump_window(d);
            d.menu->tick();
            d.renderer->present_again();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        pause_audio(false);
        d.epoch += std::chrono::steady_clock::now() - paused;
        d.clock_last += std::chrono::steady_clock::now() - paused;  // the unlocked clock too
        d.last_shown = {};
        d.menu->restart_frame_times();
        d.frame_began = {};
    }
    update_aspect(c, d, std::exchange(d.settings_changed, false));
    d.flat_before.swap(d.flat_now);
    d.flat_now.clear();
    if (d.swaps % 64 == 0) sweep_vertex_runs(d, 120);  // runs not drawn for 4 seconds leave the vertex arena
    // A render target that was the back buffer becomes the new one (the
    // device's internal reference moves with it).
    if (ld(c, kDeviceStorage + kDevRenderTarget) == old_back) {
        const uint32_t now = d.frame_buffers[d.renderer->back_index()];
        host::call_guest(c, 0x175970, {now});
        st(c, old_back + 8, ld(c, kDeviceStorage + kDevFence));
        host::call_guest(c, 0x175B90, {old_back});
        st(c, kDeviceStorage + kDevRenderTarget, now);
    }
    frame_pacing(c, d);
    long_frames(d, present_from);
    // QILIN_FPS=1: frames per second, and the game's time step, once a second.
    static const bool fps = std::getenv("QILIN_FPS") != nullptr;
    if (fps) {
        static auto since = std::chrono::steady_clock::now();
        static uint32_t from = d.swaps;
        const auto now = std::chrono::steady_clock::now();
        static float lo = 1e9f, hi = 0.0f;
        const float step = std::bit_cast<float>(ld(c, 0x5096C0));
        lo = std::min(lo, step), hi = std::max(hi, step);
        if (now - since >= std::chrono::seconds(1)) {
            const double s = std::chrono::duration<double>(now - since).count();
            std::fprintf(stderr, "qilin: %.1f frames a second, time step %.4f s (%.5f to %.5f)\n", (d.swaps - from) / s, double(step),
                         double(lo), double(hi));
            since = now, from = d.swaps, lo = 1e9f, hi = 0.0f;
        }
    }
    deliver_frame(d);
    test_run(d);
    capture_step(c, d);
    pump_audio(c);
    pump_window(d);
    ret(c, d.swaps, 4);
}

// ------------------------------------------------- copies between surfaces
std::pair<uint32_t, uint32_t> surface_size(const rt::Context& c, uint32_t surf);

// A 32-bit surface's pixels as A8R8G8B8 words, row by row, with its shape.
// Frame buffers are the renderer's images (read back from the GPU); any
// other surface is read from guest memory, swizzled or linear as its format says.
struct SurfacePixels {
    uint32_t width = 0, height = 0;
    bool linear = false;
    uint32_t pitch = 0;  // bytes per row, linear surfaces
    uint32_t data = 0;   // guest address of the pixels
    int frame_buffer = -1;
};
bool is_32bit(uint32_t fmt) { return fmt == kFmtA8R8G8B8 || fmt == kFmtX8R8G8B8 || fmt == kFmtLinA8R8G8B8 || fmt == kFmtLinX8R8G8B8; }

SurfacePixels surface_pixels(const rt::Context& c, const Device& d, const char* fn, uint32_t surf) {
    SurfacePixels p;
    const uint32_t format = ld(c, surf + 12), fmt = (format >> 8) & 0xFF, size = ld(c, surf + 16);
    if (!is_32bit(fmt)) unmodelled(fn, strf("a surface of format %#x (only 32-bit A8R8G8B8 and X8R8G8B8 are)", fmt));
    std::tie(p.width, p.height) = surface_size(c, surf);
    p.linear = fmt == kFmtLinA8R8G8B8 || fmt == kFmtLinX8R8G8B8;
    if (p.linear != (size != 0)) unmodelled(fn, "a surface whose Size word doesn't match its format");
    p.pitch = p.linear ? ((size >> 24) + 1) * 64 : p.width * 4;
    p.data = ld(c, surf + 4);
    for (size_t i = 0; i < d.frame_buffers.size(); ++i)
        if (d.frame_buffers[i] == surf) p.frame_buffer = int(i);
    return p;
}

uint32_t pixel_offset(const SurfacePixels& p, uint32_t x, uint32_t y) {
    return p.linear ? y * p.pitch + x * 4 : gfx::swizzle_offset(x, y, 0, p.width, p.height, 1) * 4;
}

// HRESULT D3DDevice_CopyRects(IDirect3DSurface8* pSourceSurface, CONST RECT*
//     pSourceRectsArray, UINT cRects, IDirect3DSurface8* pDestinationSurface,
//     CONST POINT* pDestPointsArray): copies rectangles of pixels, unscaled,
// between surfaces of one format (no rectangles: the whole source at 0, 0;
// no points: each rectangle to the same place). The game copies the frame
// it has drawn into textures this way (the book's pages show the screen
// they turn from). A frame buffer as the source is read back from the GPU,
// and a texture the GPU drew into is handed back to guest memory first;
// the destination is written in guest memory, where the texture is decoded
// from when it's next drawn (or a render target's image starts from when
// it's next drawn into). A frame buffer as the destination stops the runtime.
void copy_rects(rt::Context& c) {
    static constexpr const char* fn = "D3DDevice_CopyRects";
    Device& d = device(fn);
    const uint32_t src = arg(c, 0), rects = arg(c, 1), count = arg(c, 2), dst = arg(c, 3), points = arg(c, 4);
    const SurfacePixels from = surface_pixels(c, d, fn, src), to = surface_pixels(c, d, fn, dst);
    if (trace_draws(d))
        trace_line(strf("copy %u from=%08X %ux%u fb=%d to=%08X %ux%u count=%u\n", d.swaps, from.data, from.width, from.height, from.frame_buffer, to.data,
                        to.width, to.height, count));
    if (to.frame_buffer >= 0) unmodelled(fn, "a copy into a frame buffer");
    if (count == 0 && rects) unmodelled(fn, "a rectangle list with no rectangles");
    // The rectangles: source (left, top, right, bottom) and where each goes.
    struct Move {
        int32_t l, t, r, b, x, y;
    };
    std::vector<Move> moves;
    for (uint32_t k = 0; k < (rects ? count : 1); ++k) {
        Move mv{0, 0, int32_t(from.width), int32_t(from.height), 0, 0};
        if (rects) {
            mv.l = int32_t(ld(c, rects + 16 * k)), mv.t = int32_t(ld(c, rects + 16 * k + 4));
            mv.r = int32_t(ld(c, rects + 16 * k + 8)), mv.b = int32_t(ld(c, rects + 16 * k + 12));
        }
        mv.x = mv.l, mv.y = mv.t;
        if (points) mv.x = int32_t(ld(c, points + 8 * k)), mv.y = int32_t(ld(c, points + 8 * k + 4));
        if (mv.l < 0 || mv.t < 0 || mv.r > int32_t(from.width) || mv.b > int32_t(from.height) || mv.l > mv.r || mv.t > mv.b || mv.x < 0 ||
            mv.y < 0 || mv.x + (mv.r - mv.l) > int32_t(to.width) || mv.y + (mv.b - mv.t) > int32_t(to.height))
            unmodelled(fn, strf("a rectangle (%d,%d)-(%d,%d) to (%d,%d) outside a surface (%ux%u to %ux%u)", mv.l, mv.t, mv.r, mv.b, mv.x, mv.y,
                                from.width, from.height, to.width, to.height));
        moves.push_back(mv);
    }
    // The frame into a texture (the page turns and the pause screen, every
    // frame they run): on the GPU, from the frame buffer's image into the
    // texture's, which is then sampled as a texture the GPU drew into. No
    // waiting for the GPU and nothing through guest memory; a CPU lock of
    // the texture hands it back (block_on_resource). A texture of more than
    // one level goes the CPU's way: only its first level would be the image.
    const uint32_t parent = ld(c, dst + 0x14);
    const uint32_t parent_levels = parent ? std::max(1u, ld(c, parent + 12) >> 16 & 0xF) : 1;
    const uint32_t frame = uint32_t(from.frame_buffer);
    if (from.frame_buffer >= 0 && parent_levels == 1 && (frame == d.renderer->back_index() || frame == d.renderer->front_index())) {
        if (!d.cpu_frames.empty()) flush_cpu_frames(c, d);
        const uint32_t dst_fmt = (ld(c, dst + 12) >> 8) & 0xFF;
        Device::TargetTexture& t = texture_target(c, d, to.data, to.width, to.height, dst_fmt, to.pitch);
        std::vector<gfx::Renderer::CopyRegion> regions;
        for (const Move& mv : moves)
            regions.push_back({uint32_t(mv.l), uint32_t(mv.t), uint32_t(mv.r - mv.l), uint32_t(mv.b - mv.t), uint32_t(mv.x), uint32_t(mv.y)});
        d.renderer->copy_frame_to_target(frame, t.texture.id, regions);
        ret(c, 0, 20);
        return;
    }
    ++d.memory_epoch;  // the destination's memory changes
    // Textures the GPU drew into come back to guest memory first, so the
    // copy reads and writes what the console's memory would hold.
    hand_back_target(c, d, from.data);
    hand_back_target(c, d, to.data);

    std::vector<uint32_t> source;  // only for a frame buffer
    if (from.frame_buffer >= 0) {
        const uint32_t i = uint32_t(from.frame_buffer);
        gfx::Image im;
        if (i == d.renderer->back_index()) im = d.renderer->read_back_buffer();
        else if (i == d.renderer->front_index()) im = d.renderer->read_front_buffer();
        else unmodelled(fn, "a copy from a frame buffer that's neither the front nor the back buffer");
        source.resize(size_t(im.width) * im.height);
        for (size_t k = 0; k < source.size(); ++k)
            source[k] = uint32_t(im.rgba[4 * k + 2]) | uint32_t(im.rgba[4 * k + 1]) << 8 | uint32_t(im.rgba[4 * k]) << 16 |
                        uint32_t(im.rgba[4 * k + 3]) << 24;
    }
    const rt::Mem m = mem(c);
    auto read = [&](uint32_t x, uint32_t y) {
        return from.frame_buffer >= 0 ? source[size_t(y) * from.width + x] : rt::ld32(m, from.data + pixel_offset(from, x, y), 0);
    };
    for (const Move& mv : moves)
        for (int32_t y = mv.t; y < mv.b; ++y)
            for (int32_t x = mv.l; x < mv.r; ++x)
                rt::st32(m, to.data + pixel_offset(to, uint32_t(mv.x + x - mv.l), uint32_t(mv.y + y - mv.t)), read(uint32_t(x), uint32_t(y)), 0);
    ret(c, 0, 20);
}

// ------------------------------------------------- the push-buffer sink
// D3D's internal helpers the runtime runs lifted write GPU methods through
// the device's push pointer (+0) and limit (+4) as they go. Nothing reads
// them: the renderer takes its state from D3D's when it draws. So the
// device's push buffer is a sink, and MakeSpace and BeginStateBig (where
// the original waits for the GPU to make room) rewind it.
constexpr uint32_t kPushSinkBytes = 0x100000, kPushSinkLimit = 0x40000;
void init_push_sink(rt::Context& c, Device& d) {
    d.push_sink = contiguous(kPushSinkBytes, 0, kPhysicalLimit - 1, 0x1000, 4);
    if (!d.push_sink) rt::trap(0, "D3D: out of guest memory for the push-buffer sink");
    st(c, kDeviceStorage, d.push_sink);
    st(c, kDeviceStorage + 4, d.push_sink + kPushSinkLimit);
}
void make_space(rt::Context& c) {
    const Device& d = device("D3DDevice_MakeSpace");
    st(c, kDeviceStorage, d.push_sink);
    ret(c, d.push_sink, 0);
}
void begin_state_big(rt::Context& c) {
    const Device& d = device("D3DDevice_BeginStateBig");
    const uint32_t dwords = arg(c, 0);
    if (4 * dwords > kPushSinkBytes - kPushSinkLimit - 0x200) unmodelled("D3DDevice_BeginStateBig", strf("%u dwords", dwords));
    uint32_t at = ld(c, kDeviceStorage);
    if (at + 4 * dwords >= ld(c, kDeviceStorage + 4) + 0x200) st(c, kDeviceStorage, at = d.push_sink);
    ret(c, at, 4);
}

// VOID D3DDevice_SetRenderTarget(D3DSurface* pRenderTarget, D3DSurface* pNewZStencil):
// as the original: a null target keeps the current one; the device holds
// internal references on both (the ones it drops get its fence as their
// last use, their texture's when they are a level of one); the depth
// buffer's largest depth (+0x944) and format bits (+0x948), the device's
// "drawing to the frame buffer" flag (0x100 at +8, cleared when the target
// is the back buffer), the anti-aliasing mode (+0x19FC), the multisample
// scale and viewport (D3D_CommonSetMultiSampleModeAndScale, which resets
// the viewport to the whole target), and the DXT1 noise state re-applied,
// all with D3D's own helpers. The renderer draws to whatever +0x19F4 and
// +0x19F8 name when it draws.
void set_render_target(rt::Context& c) {
    const uint32_t dev = kDeviceStorage;
    uint32_t target = arg(c, 0);
    const uint32_t depth = arg(c, 1);
    if (!target) target = ld(c, dev + kDevRenderTarget);
    host::call_guest(c, 0x175970, {target});
    auto drop = [&](uint32_t surface) {
        const uint32_t parent = ld(c, surface + 0x14);
        st(c, (parent ? parent : surface) + 8, ld(c, dev + kDevFence));
        host::call_guest(c, 0x175B90, {surface});
    };
    if (const uint32_t old = ld(c, dev + kDevRenderTarget)) drop(old);
    const uint32_t old_depth = ld(c, dev + kDevDepth);
    st(c, dev + kDevRenderTarget, target);
    if (old_depth) drop(old_depth);
    st(c, dev + kDevDepth, depth);
    if (depth) {
        host::call_guest(c, 0x175970, {depth});
        host::call_guest(c, 0x17B080, {depth, dev + 0x944});
        host::call_guest(c, 0x174080, {depth});
        st(c, dev + 0x948, c.eax);
    }
    const uint32_t flags = ld(c, dev + 8);
    if ((flags & 0x100u) && target == ld(c, dev + kDevFrameBuffers)) st(c, dev + 8, flags & ~0x100u);
    host::call_guest(c, 0x17AF60, {target, depth});
    st(c, dev + 0x19FC, c.eax);
    c.ecx = dev;
    c.edx = 0x17FCF8;  // D3D's "whole target" viewport
    host::call_guest(c, 0x171F40, {});
    host::call_guest(c, 0x1715E0, {ld(c, 0x181DEC)});  // D3DRS_DXT1NOISEENABLE
    ret(c, c.eax, 8);
}

// ------------------------------------------------- fences

// The device's fence counter (+0x2C) steps by 2 for each fence, as the
// original's D3D_SetFence (which also clears +0x3C). The renderer finishes
// a frame's work before Swap returns and copies what a draw reads when it is
// recorded, so every fence the game can name has passed: blocking on one
// returns at once.
void insert_fence(rt::Context& c) {
    const uint32_t fence = ld(c, kDeviceStorage + kDevFence);
    st(c, kDeviceStorage + 0x3C, 0);
    st(c, kDeviceStorage + kDevFence, fence + 2);
    ret(c, fence, 0);
}
void block_on_fence(rt::Context& c) {
    const uint32_t fence = arg(c, 0);
    if (fence > ld(c, kDeviceStorage + kDevFence))
        unmodelled("D3DDevice_BlockOnFence", strf("fence %u not yet inserted", fence));
    ret(c, c.eax, 4);
}

// ------------------------------------------------- draws
// A draw does what the original's state flush (0x179A20) does to D3D's own
// bookkeeping (the dirty flags it consumes: all but 0xC0000020; the base
// vertex index it last used at +0x20; the device's has-drawn flag 0x800 at
// +8), and records the draw for the renderer with a copy of the vertices it
// reads, so the game may reuse its buffers as soon as the call returns.
// The vertex arrays come from the shader object's attribute slots ({stream,
// offset, format} at +0x14 + 16 * slot; format 2 is disabled) and D3D's
// stream table at 0x1842A0 ({stride, offset, vertex buffer} each).
uint32_t attribute_bytes(uint32_t format) {
    const uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    switch (type) {
        case 0: case 4: return size;        // UB_D3D, UB_OGL
        case 1: case 5: return 2 * size;    // S1, S32K
        case 2: return 4 * size;            // F
        case 6: return 4 * size;            // CMP (11:11:10)
        default: return 0;
    }
}

void consume_dirty_state(rt::Context& c, uint32_t base_vertex) {
    const uint32_t dirty = ld(c, kDirtyFlags);
    st(c, kDirtyFlags, dirty & 0xC0000020u);
    if (!(dirty & 0x40000000u) && ((dirty & 0x40u) || ld(c, kDeviceStorage + 0x20) != base_vertex))
        st(c, kDeviceStorage + 0x20, base_vertex);
}

void notice_once(Device& d, const std::string& what) {
    if (std::ranges::find(d.said, what) != d.said.end()) return;
    d.said.push_back(what);
    std::printf("qilin_run: D3D: %s isn't drawn yet\n", what.c_str());
}


// ---- Vertices: each hardware attribute as vec4 floats, NV2A formats
// converted as the hardware reads them; missing components (0, 0, 0, 1).
struct Vertices {
    uint32_t count = 0, mask = 0;
    std::vector<float> attr[16];
    uint64_t arena[16];  // an attribute in the vertex arena instead of `attr` (gfx::DrawCmd::arena)
    uint32_t constant = 0;  // bit n: attr[n] is one vec4 for every vertex (gfx::DrawCmd::constant_inputs)
    Vertices() { std::fill_n(arena, 16, gfx::DrawCmd::kNoArena); }
};

// The constant a program's position matrix starts at, if its first four
// instructions are dp4 oPos.x … oPos.w of v0 with four constants in a row;
// else -1.
int position_matrix_of(const gfx::vsh::Program& p) {
    if (p.code.size() < 4) return -1;
    const int n = p.code[0].constant;
    for (int i = 0; i < 4; ++i) {
        const gfx::vsh::Instruction& in = p.code[size_t(i)];
        const gfx::vsh::Operand& a = in.a;
        if (in.mac != gfx::vsh::Mac::Dp4 || in.out_is_const || in.out_from_ilu || in.out_address != gfx::vsh::kPos ||
            in.out_mask != (8 >> i) || in.relative || in.constant != n + i || a.kind != gfx::vsh::Src::Input || a.index != 0 ||
            a.negate || a.swizzle[0] != 0 || a.swizzle[1] != 1 || a.swizzle[2] != 2 || a.swizzle[3] != 3 || in.b.kind != gfx::vsh::Src::Const)
            return -1;
    }
    return n;
}

void convert_attribute(const uint8_t* p, uint32_t format, float* out) {
    const uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    out[0] = out[1] = out[2] = 0, out[3] = 1;
    switch (type) {
        case 0:  // UB_D3D: a D3DCOLOR (B, G, R, A in memory) reads as R, G, B, A
            if (size == 4) {
                out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f;
                break;
            }
            [[fallthrough]];
        case 4:  // UB_OGL
            for (uint32_t k = 0; k < size && k < 4; ++k) out[k] = p[k] / 255.0f;
            break;
        case 1:  // S1: signed normalized, (2s + 1) / 65535
            for (uint32_t k = 0; k < size && k < 4; ++k) {
                int16_t v;
                std::memcpy(&v, p + 2 * k, 2);
                out[k] = (2.0f * v + 1.0f) / 65535.0f;
            }
            break;
        case 5:  // S32K: signed, not normalized
            for (uint32_t k = 0; k < size && k < 4; ++k) {
                int16_t v;
                std::memcpy(&v, p + 2 * k, 2);
                out[k] = float(v);
            }
            break;
        case 2:  // F
            for (uint32_t k = 0; k < size && k < 4; ++k) std::memcpy(&out[k], p + 4 * k, 4);
            break;
        case 6: {  // CMP: 11, 11 and 10 signed bits, normalized
            uint32_t v;
            std::memcpy(&v, p, 4);
            auto sn = [](uint32_t bits, uint32_t n) {
                const int32_t s = int32_t(bits << (32 - n)) >> (32 - n);
                return (2.0f * float(s) + 1.0f) / float((1u << n) - 1);
            };
            out[0] = sn(v & 0x7FF, 11), out[1] = sn(v >> 11 & 0x7FF, 11), out[2] = sn(v >> 22, 10);
            break;
        }
        default: break;
    }
}

// A run of `count` attributes `stride` bytes apart, as convert_attribute
// converts each, with the format chosen once for the run.
void convert_attribute_run(const uint8_t* p, uint32_t stride, uint32_t count, uint32_t format, float* out) {
    const uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    if (type == 2 && size >= 1 && size <= 4) {  // F: the common case, floats copied
        for (uint32_t i = 0; i < count; ++i, p += stride, out += 4) {
            out[0] = out[1] = out[2] = 0, out[3] = 1;
            std::memcpy(out, p, 4 * size);
        }
    } else if (type == 0 && size == 4) {  // UB_D3D colour
        for (uint32_t i = 0; i < count; ++i, p += stride, out += 4)
            out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f;
    } else {
        for (uint32_t i = 0; i < count; ++i, p += stride, out += 4) convert_attribute(p, format, out);
    }
}

// Vertices' buffers are kept for the next draw rather than freed (a draw
// is several of them, many times a frame). One spare set per host thread.
thread_local Vertices t_spare_vertices;
thread_local std::vector<uint32_t> t_spare_sequence;
Vertices take_vertices() {
    Vertices v = std::move(t_spare_vertices);
    v.count = v.mask = v.constant = 0;
    std::fill_n(v.arena, 16, gfx::DrawCmd::kNoArena);
    return v;
}
std::vector<uint32_t> take_sequence(uint32_t n) {
    std::vector<uint32_t> s = std::move(t_spare_sequence);
    s.resize(n);
    return s;
}

// The shader object's hardware attributes in order: slot n's {stream, offset, format}.
struct Slot {
    uint32_t stream, offset, format, bytes;
};
std::array<Slot, 16> slots(rt::Context& c, const char* fn) {
    const uint32_t vs = ld(c, kDeviceStorage + kDevVertexShader);
    std::array<Slot, 16> out{};
    for (uint32_t n = 0; n < 16; ++n) {
        const uint32_t a = vs + 0x14 + 16 * n;
        out[n] = {ld(c, a), ld(c, a + 4), ld(c, a + 8), 0};
        if (out[n].format == 2) continue;
        out[n].bytes = attribute_bytes(out[n].format);
        if (!out[n].bytes || out[n].stream > 15)
            unmodelled(fn, strf("vertex attribute %u: format %#x, stream %u", n, out[n].format, out[n].stream));
    }
    return out;
}

// Draws of this many vertices or more keep their converted streams in the
// vertex arena; smaller ones cost less to convert than to look up.
constexpr uint32_t kArenaMinVertices = 16;
// A stream whose bytes change this many draws in a row is drawn the old
// way (converted into the ring each time, no comparison) for a while.
constexpr uint32_t kUnstableChanges = 3, kUnstableSwaps = 300;
void forget_vertex_run(Device& d, Device::VertexRun& r) {
    if (r.bytes) d.renderer->arena_free(r.offset, r.bytes);
    r.bytes = 0;
}

// Runs not drawn lately leave the arena (once a swap).
void sweep_vertex_runs(Device& d, uint32_t idle) {
    for (auto it = d.vertex_runs.begin(); it != d.vertex_runs.end();) {
        if (d.swaps - it->second.used > idle) {
            forget_vertex_run(d, it->second);
            it = d.vertex_runs.erase(it);
        } else {
            ++it;
        }
    }
}

// One stream's attributes for a run of vertices from the vertex arena:
// as converted before if the guest's bytes are the same, else converted
// into it now. False to convert them for this draw only (a small draw, a
// stream that keeps changing, or no room).
constexpr uint32_t kSettledSwaps = 60;

// Guest memory that hasn't changed for kSettledSwaps frames (a vertex run,
// a texture's texels) isn't compared whole with its copy every frame:
//   - after a lock of the resource it lies in, it is (the count of locks);
//   - one frame in sixteen it is, each on its own frame (quick_look);
//   - otherwise eight words spread over it are (sample_bytes), which a
//     scene loaded over the same memory fails at once.
// So a write the game makes without a lock, to memory still for a second,
// that misses all eight words, shows up to fifteen frames late; everything
// else is seen in the frame it happens, as before. A fight compared 12 MB
// of vertices and its textures whole every frame, an eighth of the game
// thread. QILIN_CHECK_SETTLED=1 compares whole as well, and reports what
// the quick look missed (none, in the story and the first fight).
uint64_t sample_bytes(const uint8_t* p, uint64_t bytes) {
    uint64_t h = bytes * 0x9E3779B97F4A7C15ull;
    if (bytes < 64) {
        for (uint64_t k = 0; k < bytes; ++k) h = (h ^ p[k]) * 0xFF51AFD7ED558CCDull;
        return h;
    }
    const uint64_t step = (bytes - 8) / 7;
    for (uint64_t k = 0; k < 8; ++k) {
        uint64_t word;
        std::memcpy(&word, p + k * step, 8);
        h = (h ^ word) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    return h;
}
// Off unless QILIN_QUICK_COMPARE=1 (v0.43.2): with it on, a player saw a
// fault when picking items up that wasn't there before, so everything is
// compared whole again until that's understood.
bool quick_look(const Device& d, uint64_t key) {
    static const bool on = std::getenv("QILIN_QUICK_COMPARE") != nullptr;
    return on && ((d.swaps + uint32_t(key >> 24)) & 15) != 0;
}
bool check_settled() {
    static const bool on = std::getenv("QILIN_CHECK_SETTLED") != nullptr;
    return on;
}

bool stream_from_arena(rt::Context& c, Device& d, const std::array<Slot, 16>& sl, uint32_t stream, uint32_t stream_at,
                       uint32_t stride, uint32_t count, Vertices& v) {
    static const bool off = [] { const char* e = std::getenv("QILIN_NO_VERTEX_CACHE"); return e && *e == '1'; }();
    if (off || count < kArenaMinVertices) return false;
    // The attributes it feeds, and the bytes they read.
    uint64_t layout = 0x9E3779B97F4A7C15ull;
    uint32_t lo = UINT32_MAX, hi = 0, n_attrs = 0;
    for (uint32_t n = 0; n < 16; ++n) {
        if (!sl[n].bytes || sl[n].stream != stream) continue;
        layout = (layout ^ (uint64_t(n) << 56 | uint64_t(sl[n].offset) << 24 | sl[n].format)) * 0xFF51AFD7ED558CCDull;
        lo = std::min(lo, sl[n].offset), hi = std::max(hi, sl[n].offset + sl[n].bytes);
        ++n_attrs;
    }
    const uint32_t src_at = stream_at + lo;
    const uint64_t span = uint64_t(stride) * (count - 1) + (hi - lo);
    if (uint64_t(src_at) + span > 0x100000000ull) return false;
    rt::check(mem(c), src_at, span, 0);
    const uint8_t* src = mem(c).base + src_at;
    const uint64_t key = (uint64_t(stream_at) << 32 | stride) * 0x9E3779B97F4A7C15ull ^ uint64_t(count) * 0xC2B2AE3D27D4EB4Full ^ layout;
    Device::VertexRun& r = d.vertex_runs[key];
    const bool same_run = r.at == stream_at && r.stride == stride && r.count == count && r.layout == layout;
    if (!same_run) {  // new, or another run under the same key: start over
        forget_vertex_run(d, r);
        r = Device::VertexRun{};
        r.at = stream_at, r.stride = stride, r.count = count, r.layout = layout;
    }
    r.used = d.swaps;
    if (r.unstable_until > d.swaps) return false;
    // A run that hasn't changed for kSettledSwaps is looked at once a frame,
    // at its first draw (the game draws the same floor once more for each
    // character's shadow: comparing every draw was 30 MB a frame in a
    // fight), and that look is the quick one above unless its buffer was
    // locked since or it's the run's frame for a whole comparison.
    // Any lock has a run compared again, as before v0.43.1 (only its own buffer's with QILIN_QUICK_COMPARE).
    static const bool per_buffer = std::getenv("QILIN_QUICK_COMPARE") != nullptr;
    const uint32_t locked = per_buffer ? d.vertex_lock_of(stream_at) : d.vertex_epoch;
    const bool kept = r.bytes && r.src.size() == span;
    const bool still = kept && d.swaps - r.changed >= kSettledSwaps && r.checked_epoch >= locked;
    const bool seen = still && r.checked_swap == d.swaps;
    const bool quick = still && !seen && quick_look(d, key);
    const bool same = seen || (quick ? sample_bytes(src, span) == r.sample : kept && std::memcmp(r.src.data(), src, size_t(span)) == 0);
    if (check_settled() && same && (seen || quick) && std::memcmp(r.src.data(), src, size_t(span)) != 0)
        std::fprintf(stderr, "qilin: a settled vertex run changed without a lock (%s): %08X stride %u count %u, swap %u\n",
                     seen ? "between draws of one frame" : "the quick look missed it", stream_at, stride, count, d.swaps);
    if (static const bool stats = std::getenv("QILIN_VERTEX_STATS") != nullptr; stats) {
        static uint64_t n[4], bytes[4];
        static uint32_t at_swap = 0;
        const int why = seen ? 0 : quick ? 1 : kept ? 2 : 3;
        ++n[why], bytes[why] += span;
        if (d.swaps >= at_swap + 600) {
            std::fprintf(stderr, "qilin: vertex runs over 600 frames: seen this frame %llu, a quick look %llu (%llu MB), compared whole %llu (%llu MB), new %llu (%llu MB); %zu runs\n",
                         (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)(bytes[1] >> 20), (unsigned long long)n[2],
                         (unsigned long long)(bytes[2] >> 20), (unsigned long long)n[3], (unsigned long long)(bytes[3] >> 20), d.vertex_runs.size());
            for (int k = 0; k < 4; ++k) n[k] = bytes[k] = 0;
            at_swap = d.swaps;
        }
    }
    if (same) {
        r.changes = 0;
        r.checked_swap = d.swaps, r.checked_epoch = d.vertex_epoch;
    } else {
        r.changed = d.swaps;
        if (r.bytes && ++r.changes >= kUnstableChanges) {  // changes every time: not worth keeping
            forget_vertex_run(d, r);
            r.src.clear();
            r.src.shrink_to_fit();
            r.changes = 0;
            r.unstable_until = d.swaps + kUnstableSwaps;
            return false;
        }
        // Converted afresh into new space (the old may still be read by draws in flight).
        forget_vertex_run(d, r);
        const uint64_t bytes = uint64_t(n_attrs) * count * 16;
        void* write = nullptr;
        if (!d.renderer->arena_alloc(bytes, &r.offset, &write)) {
            sweep_vertex_runs(d, 2);
            if (!d.renderer->arena_alloc(bytes, &r.offset, &write)) return false;
        }
        r.bytes = bytes;
        float* out = static_cast<float*>(write);
        uint32_t k = 0;
        for (uint32_t n = 0; n < 16; ++n) {
            if (!sl[n].bytes || sl[n].stream != stream) continue;
            r.attr_offset[n] = k * count * 16;
            convert_attribute_run(mem(c).base + stream_at + sl[n].offset, stride, count, sl[n].format, out + size_t(k) * count * 4);
            ++k;
        }
        r.src.assign(src, src + span);
        r.sample = sample_bytes(src, span);
    }
    for (uint32_t n = 0; n < 16; ++n) {
        if (!sl[n].bytes || sl[n].stream != stream) continue;
        v.mask |= 1u << n;
        v.arena[n] = r.offset + r.attr_offset[n];
    }
    return true;
}

Vertices vertex_arrays(rt::Context& c, const char* fn, uint32_t base, uint32_t first, uint32_t count) {
    if (ld(c, kDirtyFlags) & 0x40000000u) unmodelled(fn, "a draw from SetVertexShaderInput streams");
    Device& d = device(fn);
    Vertices v = take_vertices();
    v.count = count;
    const auto sl = slots(c, fn);
    uint32_t streams_done = 0;  // streams taken from the vertex arena
    for (uint32_t n = 0; n < 16; ++n) {
        if (!sl[n].bytes) continue;
        const uint32_t entry = 0x1842A0 + 12 * sl[n].stream;
        const uint32_t stride = ld(c, entry), vb = ld(c, entry + 8);
        if (!vb) unmodelled(fn, strf("vertex attribute %u reads stream %u, which has no buffer", n, sl[n].stream));
        const uint32_t stream_at = ld(c, vb + 4) + ld(c, entry + 4) + stride * (base + first);
        if (streams_done & (1u << sl[n].stream)) continue;
        if (stream_from_arena(c, d, sl, sl[n].stream, stream_at, stride, count, v)) {
            streams_done |= 1u << sl[n].stream;
            continue;
        }
        const uint32_t at = stream_at + sl[n].offset;
        v.mask |= 1u << n;
        v.attr[n].resize(size_t(count) * 4);
        if (!count) continue;
        // The whole run, checked once (as the byte loads it replaces were), then read in place.
        const uint64_t span = uint64_t(stride) * (count - 1) + sl[n].bytes;
        if (uint64_t(at) + span > 0x100000000ull) unmodelled(fn, strf("vertex attribute %u runs past 4 GB", n));
        rt::check(mem(c), at, span, 0);
        convert_attribute_run(mem(c).base + at, stride, count, sl[n].format, v.attr[n].data());
    }
    return v;
}

// Inline vertices (INLINE_ARRAY): whole vertices, the enabled attributes in
// hardware order, each its format's size.
Vertices inline_vertices(rt::Context& c, const char* fn, const std::vector<uint32_t>& dwords) {
    const auto sl = slots(c, fn);
    uint32_t stride = 0;
    for (const Slot& s : sl) stride += s.bytes;
    if (!stride || stride % 4) unmodelled(fn, strf("inline vertices of %u bytes", stride));
    Vertices v = take_vertices();
    v.count = uint32_t(dwords.size() * 4 / stride);
    const auto* bytes = reinterpret_cast<const uint8_t*>(dwords.data());
    for (uint32_t n = 0, off = 0; n < 16; ++n) {
        if (!sl[n].bytes) continue;
        v.mask |= 1u << n;
        v.attr[n].resize(size_t(v.count) * 4);
        convert_attribute_run(bytes + off, stride, v.count, sl[n].format, v.attr[n].data());
        off += sl[n].bytes;
    }
    return v;
}

// NV2A primitives (D3DPRIMITIVETYPE has the same numbers) as Vulkan's:
// loops, fans, quads, quad strips and polygons become indexed lists.
gfx::Primitive convert_primitive(const char* fn, uint32_t prim, std::vector<uint32_t>& seq, bool& indexed) {
    // The converted list is built in a buffer kept from the last draw, which
    // takes the old sequence's buffer in turn: no allocation per draw.
    thread_local std::vector<uint32_t> t_converted;
    std::vector<uint32_t> out = std::move(t_converted);
    out.clear();
    const size_t n = seq.size();
    auto done = [&] {
        t_converted = std::move(seq);
        seq = std::move(out);
        indexed = true;
    };
    // Kinds Vulkan draws as they are hand the buffer back unused (before,
    // it went with `out`, and the next fan or quad list allocated anew).
    auto as_is = [&](gfx::Primitive p) {
        t_converted = std::move(out);
        return p;
    };
    switch (prim) {
        case 1: return as_is(gfx::Primitive::Points);
        case 2: return as_is(gfx::Primitive::Lines);
        case 4: return as_is(gfx::Primitive::LineStrip);
        case 5: return as_is(gfx::Primitive::Triangles);
        case 6: return as_is(gfx::Primitive::TriangleStrip);
        case 3:
            for (size_t i = 0; i < n && n >= 2; ++i) out.push_back(seq[i]), out.push_back(seq[(i + 1) % n]);
            done();
            return gfx::Primitive::Lines;
        case 7: case 10:
            for (size_t i = 1; i + 1 < n; ++i) out.insert(out.end(), {seq[0], seq[i], seq[i + 1]});
            break;
        case 8:
            for (size_t i = 0; i + 3 < n; i += 4) out.insert(out.end(), {seq[i], seq[i + 1], seq[i + 2], seq[i], seq[i + 2], seq[i + 3]});
            break;
        case 9:
            for (size_t i = 0; i + 3 < n; i += 2)
                out.insert(out.end(), {seq[i], seq[i + 1], seq[i + 3], seq[i], seq[i + 3], seq[i + 2]});
            break;
        default: unmodelled(fn, strf("primitive type %u", prim));
    }
    done();
    return gfx::Primitive::Triangles;
}

// Four independent lanes over 32-byte blocks (a serial chain of multiplies
// is bound by their latency; the lanes overlap), folded into h, then the
// rest 8 bytes at a time.
uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull) {
    size_t i = 0;
    if (n >= 64) {
        uint64_t lane[4] = {h, h ^ 0x632BE59BD9B4E019ull, h ^ 0x85EBCA77C2B2AE63ull, h ^ 0x27D4EB2F165667C5ull};
        for (; i + 32 <= n; i += 32)
            for (int k = 0; k < 4; ++k) {
                uint64_t w;
                std::memcpy(&w, p + i + 8 * k, 8);
                lane[k] = (lane[k] ^ w) * 0xFF51AFD7ED558CCDull;
                lane[k] ^= lane[k] >> 32;
            }
        for (int k = 0; k < 4; ++k) h = (h ^ lane[k]) * 0xC4CEB9FE1A85EC53ull, h ^= h >> 29;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ n;
}

void color_to_float(uint32_t argb, float* out) {
    out[0] = float(argb >> 16 & 0xFF) / 255.0f, out[1] = float(argb >> 8 & 0xFF) / 255.0f, out[2] = float(argb & 0xFF) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

gfx::BlendFactor blend_factor(const char* fn, uint32_t v) {
    switch (v) {
        case 0: return gfx::BlendFactor::Zero;
        case 1: return gfx::BlendFactor::One;
        case 0x300: return gfx::BlendFactor::SrcColor;
        case 0x301: return gfx::BlendFactor::OneMinusSrcColor;
        case 0x302: return gfx::BlendFactor::SrcAlpha;
        case 0x303: return gfx::BlendFactor::OneMinusSrcAlpha;
        case 0x304: return gfx::BlendFactor::DstAlpha;
        case 0x305: return gfx::BlendFactor::OneMinusDstAlpha;
        case 0x306: return gfx::BlendFactor::DstColor;
        case 0x307: return gfx::BlendFactor::OneMinusDstColor;
        case 0x308: return gfx::BlendFactor::SrcAlphaSaturate;
        case 0x8001: return gfx::BlendFactor::ConstantColor;
        case 0x8002: return gfx::BlendFactor::OneMinusConstantColor;
        case 0x8003: return gfx::BlendFactor::ConstantAlpha;
        case 0x8004: return gfx::BlendFactor::OneMinusConstantAlpha;
        default: unmodelled(fn, strf("blend factor %#x", v));
    }
}
gfx::BlendOp blend_op(const char* fn, uint32_t v) {
    switch (v) {
        case 0x8006: return gfx::BlendOp::Add;
        case 0x800A: return gfx::BlendOp::Subtract;
        case 0x800B: return gfx::BlendOp::ReverseSubtract;
        case 0x8007: return gfx::BlendOp::Min;
        case 0x8008: return gfx::BlendOp::Max;
        default: unmodelled(fn, strf("blend operation %#x", v));
    }
}
gfx::Compare compare(const char* fn, uint32_t v) {
    if (v < 0x200 || v > 0x207) unmodelled(fn, strf("compare function %#x", v));
    return gfx::Compare(v - 0x200);
}
gfx::StencilOp stencil_op(const char* fn, uint32_t v) {
    switch (v) {
        case 0x1E00: return gfx::StencilOp::Keep;
        case 0: return gfx::StencilOp::Zero;
        case 0x1E01: return gfx::StencilOp::Replace;
        case 0x1E02: return gfx::StencilOp::IncrementClamp;
        case 0x1E03: return gfx::StencilOp::DecrementClamp;
        case 0x150A: return gfx::StencilOp::Invert;
        case 0x8507: return gfx::StencilOp::IncrementWrap;
        case 0x8508: return gfx::StencilOp::DecrementWrap;
        default: unmodelled(fn, strf("stencil operation %#x", v));
    }
}

constexpr float kOverlayBias = 8.0f;  // see D3DRS_ZBIAS below
// D3DRS_ZBIAS's slot, which SetRenderState_ZBias keeps.
constexpr uint32_t kRsZBiasSlot = 0x181DC4;
// D3DRS_LINEWIDTH's slot (a float), which SetRenderState_LineWidth keeps.
constexpr uint32_t kRsLineWidthSlot = 0x181DE4;
// D3D's render states this needs (XDK 5659 numbering).
enum Rs : uint32_t {
    kRsPsConstant0 = 10, kRsPsConstant1 = 18, kRsPsAlphaOutputs = 26, kRsPsRgbInputs = 34, kRsPsCompareMode = 42,
    kRsPsFinalC0 = 43, kRsPsFinalC1 = 44, kRsPsRgbOutputs = 45, kRsPsCombinerCount = 53, kRsPsDotMapping = 55, kRsPsInputTexture = 56,
    kRsZFunc = 57, kRsAlphaFunc = 58, kRsAlphaBlendEnable = 59, kRsAlphaTestEnable = 60, kRsAlphaRef = 61, kRsSrcBlend = 62,
    kRsDestBlend = 63, kRsZWriteEnable = 64, kRsColorWriteEnable = 67, kRsStencilZFail = 68, kRsStencilPass = 69, kRsStencilFunc = 70,
    kRsStencilRef = 71, kRsStencilMask = 72, kRsStencilWriteMask = 73, kRsBlendOp = 74, kRsBlendColor = 75,
    kRsPsTextureModes = 136, kRsFogColor = 138, kRsZEnable = 143, kRsStencilEnable = 144, kRsStencilFail = 145, kRsCullMode = 147,
};

// A texture stage's texture, decoded and uploaded once (and again when its
// texels change; they're compared once a frame).
const Device::CachedTexture* target_texture(rt::Context& c, Device& d, uint32_t tex);
const Device::CachedTexture* depth_texture(rt::Context& c, Device& d, uint32_t tex);
// A palettized (P8) texture reads the stage's palette, which the device
// keeps at +0xF88 (SetPalette is inline: the game stores it there): 256
// A8R8G8B8 entries at most (D3DPalette's size code: 256 >> code), part of
// what the cached texture is made from.
const Device::CachedTexture* stage_texture(rt::Context& c, Device& d, const char* fn, uint32_t tex, uint32_t stage) {
    if (const Device::CachedTexture* t = target_texture(c, d, tex)) return t;
    if (const Device::CachedTexture* t = depth_texture(c, d, tex)) return t;
    const uint32_t format = ld(c, tex + 12), size = ld(c, tex + 16), data = ld(c, tex + 4);
    gfx::TextureShape shape;
    std::string why;
    if (!gfx::texture_shape(format, size, &shape, &why)) unmodelled(fn, why);
    if (size_t(data) + shape.bytes > c.mem_size) unmodelled(fn, strf("a texture past the end of memory (%08X)", data));
    uint8_t palette[256 * 4] = {};
    uint64_t palette_hash = 0;
    if (shape.palettized) {
        const uint32_t pal = ld(c, kDeviceStorage + 0xF88 + 4 * stage);
        if (!pal) unmodelled(fn, strf("a palettized texture on stage %u with no palette", stage));
        const uint32_t entries = 256u >> ((ld(c, pal) >> 30) & 3), pdata = ld(c, pal + 4);
        std::memcpy(palette, c.mem + pdata, entries * 4);
        palette_hash = hash_bytes(palette, sizeof palette) | 1;
    }
    const uint64_t key = (uint64_t(data) << 32 ^ uint64_t(format) * 0x9E3779B1u ^ size) + palette_hash * 31;
    Device::CachedTexture& e = d.textures[key];
    if (e.id && e.checked == d.swaps) return &e;
    const uint8_t* texels = c.mem + data;
    // Texels still for kSettledSwaps frames get the quick look (above), unless one of the texture's levels was locked since.
    const bool kept = e.id && e.texels.size() == shape.bytes;
    const bool quick = kept && d.swaps - e.changed >= kSettledSwaps && e.checked_lock >= d.locked_within(data, shape.bytes) && quick_look(d, key);
    const bool same = quick ? sample_bytes(texels, shape.bytes) == e.sample : kept && std::memcmp(e.texels.data(), texels, shape.bytes) == 0;
    if (check_settled() && quick && same && std::memcmp(e.texels.data(), texels, shape.bytes) != 0)
        std::fprintf(stderr, "qilin: a settled texture changed without a lock (the quick look missed it): %08X, %zu bytes, swap %u\n", data, size_t(shape.bytes), d.swaps);
    e.checked_lock = d.vertex_epoch;
    if (!same) {
        e.changed = d.swaps;
        // DXT blocks go to the GPU as they are where it samples BC1-3 (a
        // quarter or an eighth of the memory, and no decoding here).
        const gfx::TextureData t = shape.compressed && shape.depth == 1 && d.renderer->takes_blocks()
                                       ? gfx::texture_blocks(shape, texels)
                                       : gfx::decode_texture(shape, texels, shape.palettized ? palette : nullptr);
        // Changed texels (a video frame, say) go into the image it has.
        if (!e.id || !d.renderer->update_texture(e.id, t)) {
            if (e.id) d.renderer->destroy_texture(e.id);
            e.id = d.renderer->create_texture(t);
        }
        e.texels.assign(texels, texels + shape.bytes);
        e.sample = sample_bytes(texels, shape.bytes);
        e.dims = t.dims(), e.linear = t.linear, e.width = t.width, e.height = t.height, e.depth = t.depth;
    }
    e.checked = d.swaps;
    return &e;
}

gfx::SamplerDesc stage_sampler(rt::Context& c, const char* fn, uint32_t stage, const Device::CachedTexture& t, uint32_t levels) {
    auto ts = [&](uint32_t i) { return ld(c, 0x181970 + 4 * (32 * stage + i)); };
    auto address = [&](uint32_t v) {
        switch (v) {
            case 1: return gfx::Address::Wrap;
            case 2: return gfx::Address::Mirror;
            case 3: case 5: return gfx::Address::Clamp;
            case 4: return gfx::Address::Border;
            default: unmodelled(fn, strf("texture address mode %u", v));
        }
    };
    auto filter = [](uint32_t v) { return v >= 2 ? gfx::Filter::Linear : gfx::Filter::Nearest; };
    gfx::SamplerDesc s;
    s.u = address(ts(0)), s.v = address(ts(1)), s.w = address(ts(2));
    if (t.linear) s.u = s.v = s.w = gfx::Address::Clamp;  // the hardware only clamps linear textures
    s.mag = filter(ts(3)), s.min = filter(ts(4));
    s.mipmaps = ts(5) != 0 && levels > 1;
    s.mip = ts(5) >= 2 ? gfx::Filter::Linear : gfx::Filter::Nearest;
    s.max_level = levels ? levels - 1 : 0;
    s.lod_bias = std::bit_cast<float>(ts(6));
    s.border = ts(29);
    return s;
}

// The derived state the original computes for the GPU, at draw time: the
// viewport's screen-space scale and offset (0x1710E0, which the vertex
// program reads as c-38 and c-37), from the viewport, the multisample
// scale, the screen-space offset and the depth scale.
void viewport_constants(rt::Context& c, float scale[4], float offset[4]) {
    const uint32_t dev = kDeviceStorage;
    auto f = [&](uint32_t a) { return std::bit_cast<float>(ld(c, a)); };
    auto i2f = [&](uint32_t a) { return float(ld(c, a)); };  // fild of an unsigned field
    const float sx = f(dev + 0x958), sy = f(dev + 0x95C), zscale = f(dev + 0x944);
    float ox = i2f(dev + 0xED0) * sx + f(dev + 0xEE8);
    float oy = i2f(dev + 0xED4) * sy + f(dev + 0xEEC);
    if ((ld(c, dev + 8) & 0x8000) && ld(c, 0x181DD0)) ox -= 0.5f, oy -= 0.5f;
    const float hw = i2f(dev + 0xED8) * sx * 0.5f, hh = i2f(dev + 0xEDC) * sy * -0.5f;
    const float minz = f(dev + 0xEE0), maxz = f(dev + 0xEE4);
    scale[0] = hw, scale[1] = hh, scale[2] = (maxz - minz) * zscale, scale[3] = 0;
    offset[0] = hw + ox, offset[1] = -hh + oy, offset[2] = minz * zscale, offset[3] = 0;
}

// ---------------------------------------------------------------- render targets
uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h);
std::pair<uint32_t, uint32_t> surface_size(const rt::Context& c, uint32_t surf);

// ---------------------------------------------------------------- CPU frames
// The Xbox's frame buffers are memory like any surface's: a title can lock
// one and read or write its pixels. Here the pixels are the renderer's
// images, so a lock (D3D_BlockOnResource) copies the image into guest
// memory, as the CPU would find it, and keeps a hash of it. Before the next
// draw or clear, at Swap, and while the title waits without drawing (the
// idle hook, at most every 16 ms), guest memory that changed is copied back
// into the image, and a changed front buffer is shown again. A frame buffer
// is watched until the Swap after its lock.
uint32_t surface_pitch(const rt::Context& c, uint32_t surf) { return ((ld(c, surf + 16) >> 24) + 1) * 64; }
uint64_t cpu_frame_hash(const rt::Context& c, const Device& d, uint32_t index) {
    const uint32_t surf = d.frame_buffers[index], data = ld(c, surf + 4), pitch = surface_pitch(c, surf);
    return hash_bytes(c.mem + data, size_t(pitch) * d.height);
}
void cpu_frame_to_guest(rt::Context& c, Device& d, uint32_t index) {
    const gfx::Image im = d.renderer->read_frame_buffer(index);
    const uint32_t surf = d.frame_buffers[index], data = ld(c, surf + 4), pitch = surface_pitch(c, surf);
    for (uint32_t y = 0; y < im.height; ++y)
        for (uint32_t x = 0; x < im.width; ++x) {
            const uint8_t* p = &im.rgba[(size_t(y) * im.width + x) * 4];
            uint8_t* q = c.mem + data + size_t(y) * pitch + size_t(x) * 4;
            q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = p[3];  // B, G, R, A in memory
        }
    const uint64_t h = cpu_frame_hash(c, d, index);
    for (auto& f : d.cpu_frames)
        if (f.index == index) return void(f.hash = h);
    d.cpu_frames.push_back({index, h});
}
void flush_cpu_frames(rt::Context& c, Device& d) {
    for (auto& f : d.cpu_frames) {
        const uint64_t h = cpu_frame_hash(c, d, f.index);
        if (h == f.hash) continue;
        f.hash = h;
        const uint32_t surf = d.frame_buffers[f.index], data = ld(c, surf + 4), pitch = surface_pitch(c, surf);
        gfx::Image im{d.width, d.height, std::vector<uint8_t>(size_t(d.width) * d.height * 4)};
        for (uint32_t y = 0; y < d.height; ++y)
            for (uint32_t x = 0; x < d.width; ++x) {
                const uint8_t* q = c.mem + data + size_t(y) * pitch + size_t(x) * 4;
                uint8_t* p = &im.rgba[(size_t(y) * d.width + x) * 4];
                p[0] = q[2], p[1] = q[1], p[2] = q[0], p[3] = q[3];
            }
        d.renderer->write_frame_buffer(f.index, im);
        if (f.index == d.renderer->front_index()) d.renderer->show_front();
    }
}

// Points the renderer at the device's render target before a draw or clear,
// and returns its size: the back buffer, or a texture's surface (made a
// render target the first time). Drawing into the front buffer and other
// formats stop the runtime.
// A 32-bit texture's texels (A8R8G8B8 or X8R8G8B8, swizzled or linear)
// as an image, and back.
uint32_t texel_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool linear, uint32_t pitch) {
    return linear ? y * pitch + x * 4 : gfx::swizzle_offset(x, y, 0, w, h, 1) * 4;
}
gfx::Image guest_image(const rt::Context& c, uint32_t data, uint32_t w, uint32_t h, bool linear, uint32_t pitch) {
    gfx::Image im;
    im.width = w, im.height = h;
    im.rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = c.mem + data + texel_offset(x, y, w, h, linear, pitch);
            uint8_t* o = &im.rgba[(size_t(y) * w + x) * 4];
            o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = p[3];
        }
    return im;
}
void image_to_guest(rt::Context& c, uint32_t data, bool linear, uint32_t pitch, const gfx::Image& im) {
    for (uint32_t y = 0; y < im.height; ++y)
        for (uint32_t x = 0; x < im.width; ++x) {
            uint8_t* p = c.mem + data + texel_offset(x, y, im.width, im.height, linear, pitch);
            const uint8_t* i = &im.rgba[(size_t(y) * im.width + x) * 4];
            p[0] = i[2], p[1] = i[1], p[2] = i[0], p[3] = i[3];
        }
}

// A texture the GPU drew into, handed back to guest memory: its image read
// back into the texels, as the console's memory would hold them, and the
// renderer's image dropped, so the memory is the texture again.
void hand_back_target(rt::Context& c, Device& d, uint32_t data) {
    auto it = d.targets.find(data);
    if (it == d.targets.end()) return;
    const Device::TargetTexture& t = it->second;
    if (trace_draws(d)) trace_line(strf("handback %u rt=%08X\n", d.swaps, data));
    image_to_guest(c, data, t.texture.linear, t.pitch, d.renderer->read_render_target(t.texture.id));
    d.renderer->destroy_texture(t.texture.id);
    d.targets.erase(it);
}

// The renderer's image for a texture's texels at `data` that the GPU draws
// or copies into (by the guest address of its texels), made the first
// time, and starting from (or taking again) what the guest's memory holds
// when the CPU has written it since: on the Xbox the GPU writes over the
// texture's memory.
Device::TargetTexture& texture_target(rt::Context& c, Device& d, uint32_t data, uint32_t w, uint32_t h, uint32_t fmt, uint32_t pitch) {
    Device::TargetTexture& t = d.targets[data];
    const bool compare = !t.texture.id || t.checked_swap != d.swaps || t.checked_epoch != d.memory_epoch;
    const uint64_t guest = compare ? hash_bytes(c.mem + data, size_t(w) * h * 4, 0x9E3779B97F4A7C15ull) : t.guest;
    const bool linear = fmt == kFmtLinA8R8G8B8 || fmt == kFmtLinX8R8G8B8;
    if (!t.texture.id || t.texture.width != w || t.texture.height != h || t.format != fmt) {
        if (t.texture.id) d.renderer->destroy_texture(t.texture.id);
        // Its memory was a depth surface: that's over (as apply_target, the other way round).
        if (auto old = d.depth_targets.find(data); old != d.depth_targets.end()) {
            if (trace_draws(d)) trace_line(strf("dropped %u depth=%08X now colour\n", d.swaps, data));
            d.renderer->destroy_texture(old->second.id);
            if (old->second.packed.id) d.renderer->destroy_texture(old->second.packed.id);
            d.depth_targets.erase(old);
        }
        t.texture = {};
        t.texture.id = d.renderer->create_render_target(w, h);
        t.texture.width = w, t.texture.height = h, t.texture.dims = 2;
        t.texture.linear = linear;
        t.format = fmt;
        t.pitch = pitch;
        t.guest = ~guest;  // so its image starts as the memory's texels (below)
    }
    // The texels the image starts from, or that the CPU wrote since the GPU
    // last drew: on the Xbox the GPU draws over the texture's memory.
    if (t.guest != guest && trace_draws(d)) trace_line(strf("upload %u rt=%08X id=%u\n", d.swaps, data, t.texture.id));
    if (t.guest != guest) d.renderer->write_render_target(t.texture.id, guest_image(c, data, w, h, linear, pitch));
    t.guest = guest;
    t.checked_swap = d.swaps, t.checked_epoch = d.memory_epoch;
    return t;
}

std::pair<uint32_t, uint32_t> apply_target(rt::Context& c, Device& d, const char* fn) {
    if (!d.cpu_frames.empty()) flush_cpu_frames(c, d);
    const uint32_t target = ld(c, kDeviceStorage + kDevRenderTarget), depth = ld(c, kDeviceStorage + kDevDepth);
    if (target == d.frame_buffers[d.renderer->back_index()]) {
        d.renderer->set_render_target(0, gfx::Renderer::kDeviceDepth);
        return {d.width, d.height};
    }
    for (uint32_t fb : d.frame_buffers)
        if (fb == target) unmodelled(fn, "drawing into the front buffer");
    const uint32_t format = ld(c, target + 12), fmt = (format >> 8) & 0xFF;
    if (fmt != kFmtA8R8G8B8 && fmt != kFmtX8R8G8B8 && fmt != kFmtLinA8R8G8B8 && fmt != kFmtLinX8R8G8B8)
        unmodelled(fn, strf("rendering into a surface of format %#x", fmt));
    const auto [w, h] = surface_size(c, target);
    // The depth buffer with it, at least its size (as D3D requires): the
    // device's is the renderer's own; another D24S8 surface gets a depth
    // target of its own, made the first time, by the guest address of its
    // data. What a depth surface holds isn't in guest memory: a title that
    // reads or writes one with the CPU isn't modelled.
    uint32_t depth_id = 0;
    if (depth) {
        const auto [dw, dh] = surface_size(c, depth);
        const uint32_t dfmt = (ld(c, depth + 12) >> 8) & 0xFF;
        if (dw < w || dh < h || (dfmt != kFmtD24S8 && dfmt != kFmtLinD24S8))
            unmodelled(fn, strf("a depth buffer (%08X, %ux%u, format %#x) with a texture as the render target (%ux%u)", depth, dw, dh,
                                dfmt, w, h));
        if (depth == d.depth) {
            depth_id = gfx::Renderer::kDeviceDepth;
        } else {
            const uint32_t ddata = ld(c, depth + 4);
            // Its memory was a texture the GPU drew into, or a frame copied
            // into one: that image isn't what it holds any more (the game
            // reuses memory a page turn's copy had for a depth surface).
            if (auto old = d.targets.find(ddata); old != d.targets.end()) {
                if (trace_draws(d)) trace_line(strf("dropped %u rt=%08X now depth\n", d.swaps, ddata));
                d.renderer->destroy_texture(old->second.texture.id);
                d.targets.erase(old);
            }
            Device::DepthTarget& dt = d.depth_targets[ddata];
            if (!dt.id || dt.width != dw || dt.height != dh) {
                if (dt.id) d.renderer->destroy_texture(dt.id);
                if (dt.packed.id) d.renderer->destroy_texture(dt.packed.id);
                dt = {};
                dt.id = d.renderer->create_depth_target(dw, dh), dt.width = dw, dt.height = dh;
                dt.checked_swap = ~0u;
            }
            dt.linear = dfmt == kFmtLinD24S8;
            ++dt.drawn;
            if (dt.checked_swap != d.swaps || dt.checked_epoch != d.memory_epoch) {
                if (size_t(ddata) + size_t(dw) * dh * 4 <= c.mem_size)
                    dt.guest = hash_bytes(c.mem + ddata, size_t(dw) * dh * 4, 0x9E3779B97F4A7C15ull);
                dt.checked_swap = d.swaps, dt.checked_epoch = d.memory_epoch;
            }
            depth_id = dt.id;
        }
    }
    const uint32_t data = ld(c, target + 4);
    if (size_t(data) + size_t(w) * h * 4 > c.mem_size) unmodelled(fn, strf("a render target past the end of memory (%08X)", data));
    const bool linear = fmt == kFmtLinA8R8G8B8 || fmt == kFmtLinX8R8G8B8;
    const uint32_t pitch = linear ? ((ld(c, target + 16) >> 24) + 1) * 64 : w * 4;
    Device::TargetTexture& t = texture_target(c, d, data, w, h, fmt, pitch);
    d.renderer->set_render_target(t.texture.id, depth_id);
    return {w, h};
}

// A depth surface read as a 32-bit colour texture, or null. The renderer
// keeps a texture target's depth surface on the GPU (apply_target), not in
// guest memory; a draw that samples that memory as A8R8G8B8 (the game reads
// the depth of the scenes it renders into textures this way, through
// DOT_ZW, to write it into the depth and stencil buffer) gets the depth
// and stencil packed as the console's memory holds them, made again after
// the surface is drawn into. Memory the CPU wrote since (the surface freed
// and the memory reused) ends it.
const Device::CachedTexture* depth_texture(rt::Context& c, Device& d, uint32_t tex) {
    const uint32_t data = ld(c, tex + 4);
    auto it = d.depth_targets.find(data);
    if (it == d.depth_targets.end()) return nullptr;
    Device::DepthTarget& dt = it->second;
    const uint32_t format = ld(c, tex + 12), fmt = (format >> 8) & 0xFF;
    const bool linear = fmt == kFmtLinA8R8G8B8 || fmt == kFmtLinX8R8G8B8;
    if (!(linear || fmt == kFmtA8R8G8B8 || fmt == kFmtX8R8G8B8) || linear != dt.linear || ((format >> 16) & 0xF) > 1) return nullptr;
    const auto [w, h] = surface_size(c, tex);
    if (w > dt.width || h > dt.height) return nullptr;
    if (dt.checked_swap != d.swaps || dt.checked_epoch != d.memory_epoch) {
        const bool cpu_wrote = size_t(data) + size_t(dt.width) * dt.height * 4 > c.mem_size ||
                               hash_bytes(c.mem + data, size_t(dt.width) * dt.height * 4, 0x9E3779B97F4A7C15ull) != dt.guest;
        if (cpu_wrote) {  // the memory holds something else now
            if (trace_draws(d)) trace_line(strf("dropped %u depth=%08X\n", d.swaps, data));
            d.renderer->destroy_texture(dt.id);
            if (dt.packed.id) d.renderer->destroy_texture(dt.packed.id);
            d.depth_targets.erase(it);
            return nullptr;
        }
        dt.checked_swap = d.swaps, dt.checked_epoch = d.memory_epoch;
    }
    if (!dt.packed.id || dt.packed.width != w || dt.packed.height != h) {
        if (dt.packed.id) d.renderer->destroy_texture(dt.packed.id);
        dt.packed = {};
        dt.packed.id = d.renderer->create_render_target(w, h);
        dt.packed.width = w, dt.packed.height = h, dt.packed.dims = 2;
        dt.packed_drawn = ~0ull;
    }
    dt.packed.linear = linear;
    if (dt.packed_drawn != dt.drawn) {
        if (!d.renderer->pack_depth(dt.id, dt.packed.id)) return nullptr;  // the device can't sample depth: the memory, as before
        if (trace_draws(d)) trace_line(strf("packed %u depth=%08X %ux%u\n", d.swaps, data, w, h));
        dt.packed_drawn = dt.drawn;
    }
    return &dt.packed;
}

// The renderer's image for a texture whose texels were last drawn by the
// GPU, or null (the guest memory is the texture).
const Device::CachedTexture* target_texture(rt::Context& c, Device& d, uint32_t tex) {
    const uint32_t data = ld(c, tex + 4);
    auto it = d.targets.find(data);
    if (it == d.targets.end()) return nullptr;
    Device::TargetTexture& t = it->second;
    const uint32_t format = ld(c, tex + 12), fmt = (format >> 8) & 0xFF;
    const auto [w, h] = surface_size(c, tex);
    const bool same_shape = fmt == t.format && w == t.texture.width && h == t.texture.height && ((format >> 16) & 0xF) <= 1;
    const bool compare = t.checked_swap != d.swaps || t.checked_epoch != d.memory_epoch;
    const bool cpu_wrote = compare && hash_bytes(c.mem + data, size_t(t.texture.width) * t.texture.height * 4, 0x9E3779B97F4A7C15ull) != t.guest;
    if ((!same_shape || cpu_wrote) && trace_draws(d)) trace_line(strf("dropped %u rt=%08X shape=%d cpu=%d\n", d.swaps, data, same_shape, cpu_wrote));
    if (!same_shape || cpu_wrote) {  // the memory holds something else now
        d.renderer->destroy_texture(t.texture.id);
        d.targets.erase(it);
        return nullptr;
    }
    t.checked_swap = d.swaps, t.checked_epoch = d.memory_epoch;
    return &t.texture;
}

// ---------------------------------------------------------------- the fixed-function vertex pipeline
// The NV2A's fixed-function transform, for what the game uses of it, as an
// NV2A vertex program the renderer translates like any other: the position
// through the composite of the world, view and projection transforms (in
// c0-c3 here, columns of World × View × Projection), then to screen space
// as D3D does for programs (c-38, c-37), and the colours and texture
// coordinates passed through. Lighting, vertex blending, fog, texture
// coordinate generation and texture transforms stop the runtime until
// they're modelled. Pre-transformed (XYZRHW) vertices take the original's
// own passthrough program instead (passthrough_constants).
constexpr uint32_t kDevTransforms = 0xC50;          // the device's matrices, 64 bytes each
constexpr uint32_t kTextureStateTable = 0x181970;  // D3D__TextureState: 32 dwords per stage
constexpr uint32_t kRsFogEnable = 92, kRsFogTableMode = 93, kRsFogStart = 94, kRsFogEnd = 95, kRsFogDensity = 96, kRsRangeFogEnable = 97,
                   kRsLighting = 102, kRsVertexBlend = 137;
// The point states (D3DRS_POINTSIZE on, from 0x181D40): size, minimum,
// sprites, scaling, the attenuation's A, B and C, maximum.
constexpr uint32_t kRsPointSize = 116, kRsPointSizeMin = 117, kRsPointSpriteEnable = 118, kRsPointScaleEnable = 119,
                   kRsPointScaleA = 120, kRsPointScaleB = 121, kRsPointScaleC = 122, kRsPointSizeMax = 123;
constexpr uint32_t kTsTextureTransformFlags = 21, kTsTexCoordIndex = 28;
// The bump-environment states, in the order D3D pushes them to the GPU's
// matrix (method 0x1B28 on: entry k from state 22 + k), then the luminance
// scale and offset (0x1B38, 0x1B3C).
constexpr uint32_t kTsBumpEnvMat00 = 22, kTsBumpEnvLScale = 26, kTsBumpEnvLOffset = 27;
constexpr uint32_t kTsView = 0, kTsProjection = 1, kTsWorld = 6;

struct FixedFunction {
    gfx::vsh::Program program;
    uint64_t key = 0;
};

FixedFunction fixed_function_program(rt::Context& c, const char* fn, bool points) {
    using namespace gfx::vsh;
    const uint32_t fvf = ld(c, kDeviceStorage + kDevVertexShader + 4);
    auto rs = [&](uint32_t i) { return ld(c, kRenderStateTable + 4 * i); };
    auto ts = [&](uint32_t stage, uint32_t i) { return ld(c, kTextureStateTable + 4 * (32 * stage + i)); };
    const uint32_t position = fvf & 0xE, texcount = (fvf >> 8) & 0xF;
    if (fvf & 1u) unmodelled(fn, strf("FVF %#x: a vertex shader handle where an FVF was expected", fvf));
    if (position != 0x2) unmodelled(fn, strf("the fixed-function pipeline with FVF %#x (positions other than D3DFVF_XYZ)", fvf));
    if (fvf & ~0xFFFF0FE2u) unmodelled(fn, strf("FVF %#x", fvf));
    if (rs(kRsLighting)) unmodelled(fn, "fixed-function lighting (D3DRS_LIGHTING)");
    if (rs(kRsVertexBlend)) unmodelled(fn, "vertex blending (D3DRS_VERTEXBLEND)");
    for (uint32_t n = 0; n < texcount; ++n) {
        if (ts(n, kTsTexCoordIndex) != n) unmodelled(fn, strf("stage %u's texture coordinates from set %#x", n, ts(n, kTsTexCoordIndex)));
        if (ts(n, kTsTextureTransformFlags)) unmodelled(fn, strf("a texture transform on stage %u", n));
        if (((fvf >> (16 + 2 * n)) & 3) == 3) unmodelled(fn, strf("FVF %#x: one-component texture coordinates", fvf));
    }

    // The fog coordinate, as the original sets the fog unit's source: the
    // specular alpha without a table mode, else eye-space z (planar), or the
    // distance to the eye with D3DRS_RANGEFOGENABLE (radial); the model-view
    // transform's columns in c4-c7 (fixed_function_constants).
    const uint32_t fog_gen = rs(kRsFogEnable) ? (rs(kRsFogTableMode) == 0 ? 0u : rs(kRsRangeFogEnable) ? 1u : 2u) : 3u;

    FixedFunction f;
    const bool point_scale = points && rs(kRsPointScaleEnable);
    f.key = 0xF1F0000000000000ull | uint64_t(fvf) | uint64_t(fog_gen) << 32 | uint64_t(points) << 36 | uint64_t(point_scale) << 37;
    auto input = [](uint8_t i) { Operand o; o.kind = Src::Input; o.index = i; return o; };
    auto temp = [](uint8_t i) { Operand o; o.kind = Src::Temp; o.index = i; return o; };
    auto konst = [] { Operand o; o.kind = Src::Const; return o; };
    auto out = [](Instruction& in, uint8_t address, uint8_t mask) { in.out_address = address, in.out_mask = mask; };
    for (uint8_t row = 0; row < 4; ++row) {  // dp4 oPos.<row>, v0, c<row>
        Instruction in;
        in.mac = Mac::Dp4, in.a = input(0), in.b = konst(), in.constant = row;
        out(in, kPos, uint8_t(8 >> row));
        f.program.code.push_back(in);
    }
    {  // mul oPos.xyz, r12, c-38 + rcc r1.x, r12.w
        Instruction in;
        in.mac = Mac::Mul, in.a = temp(12), in.b = konst(), in.constant = 58;
        in.ilu = Ilu::Rcc, in.c = temp(12);
        for (auto& s : in.c.swizzle) s = 3;
        in.ilu_temp_mask = 8;
        out(in, kPos, 0xE);
        f.program.code.push_back(in);
    }
    {  // mad oPos.xyz, r12, r1.x, c-37
        Instruction in;
        in.mac = Mac::Mad, in.a = temp(12), in.b = temp(1), in.c = konst(), in.constant = 59;
        for (auto& s : in.b.swizzle) s = 0;
        out(in, kPos, 0xE);
        f.program.code.push_back(in);
    }
    auto mov = [&](uint8_t address, uint8_t from) {
        Instruction in;
        in.mac = Mac::Mov, in.a = input(from);
        out(in, address, 0xF);
        f.program.code.push_back(in);
    };
    mov(kD0, 3);  // the vertices' colours, or the registers' (absent_colour_registers)
    mov(kD1, 4);
    if (fog_gen == 0) {  // mov oFog.x, v4.w
        Instruction in;
        in.mac = Mac::Mov, in.a = input(4);
        for (auto& s : in.a.swizzle) s = 3;
        out(in, kFog, 8);
        f.program.code.push_back(in);
    } else if (fog_gen != 3) {
        for (uint8_t row = 0; row < 3; ++row) {  // dp4 r2.<row>, v0, c<4 + row>: the eye-space position
            Instruction in;
            in.mac = Mac::Dp4, in.a = input(0), in.b = konst(), in.constant = uint8_t(4 + row);
            in.temp = 2, in.mac_temp_mask = uint8_t(8 >> row);
            f.program.code.push_back(in);
        }
        Instruction in;
        if (fog_gen == 2) {  // mov oFog.x, r2.z
            in.mac = Mac::Mov, in.a = temp(2);
            for (auto& s : in.a.swizzle) s = 2;
        } else {  // dp3 r3.x, r2, r2 + rsq r1.x, r3.x, then mul oFog.x, r3.x, r1.x
            Instruction d;
            d.mac = Mac::Dp3, d.a = temp(2), d.b = temp(2);
            d.temp = 3, d.mac_temp_mask = 8;
            f.program.code.push_back(d);
            Instruction r;
            r.ilu = Ilu::Rsq, r.c = temp(3), r.temp = 1;  // the ILU alone: writes the instruction's temporary
            for (auto& s : r.c.swizzle) s = 0;
            r.ilu_temp_mask = 8;
            f.program.code.push_back(r);
            in.mac = Mac::Mul, in.a = temp(3), in.b = temp(1);
            for (auto& s : in.a.swizzle) s = 0;
            for (auto& s : in.b.swizzle) s = 0;
        }
        out(in, kFog, 8);
        f.program.code.push_back(in);
    }
    if (points) {
        // The point size (point_constants): D3DRS_POINTSIZE in pixels, or
        // with D3DRS_POINTSCALEENABLE the viewport height × the size ×
        // 1/sqrt(A + B·d + C·d²), d the distance to the eye; either way
        // clamped to D3DRS_POINTSIZE_MIN and _MAX, as the NV2A's point
        // parameters (LazySetPointParams, 0x1783E0) have it.
        Operand size = konst();
        for (auto& s : size.swizzle) s = 0;
        if (point_scale) {
            for (uint8_t row = 0; row < 3; ++row) {  // dp4 r2.<row>, v0, c<4 + row>
                Instruction in;
                in.mac = Mac::Dp4, in.a = input(0), in.b = konst(), in.constant = uint8_t(4 + row);
                in.temp = 2, in.mac_temp_mask = uint8_t(8 >> row);
                f.program.code.push_back(in);
            }
            Instruction d2;  // dp3 r3.x, r2, r2
            d2.mac = Mac::Dp3, d2.a = temp(2), d2.b = temp(2), d2.temp = 3, d2.mac_temp_mask = 8;
            f.program.code.push_back(d2);
            Instruction inv;  // rsq r1.x, r3.x
            inv.ilu = Ilu::Rsq, inv.c = temp(3), inv.temp = 1, inv.ilu_temp_mask = 8;
            for (auto& s : inv.c.swizzle) s = 0;
            f.program.code.push_back(inv);
            Instruction dist;  // mul r3.y, r3.x, r1.x: d
            dist.mac = Mac::Mul, dist.a = temp(3), dist.b = temp(1), dist.temp = 3, dist.mac_temp_mask = 4;
            for (auto& s : dist.a.swizzle) s = 0;
            for (auto& s : dist.b.swizzle) s = 0;
            f.program.code.push_back(dist);
            Instruction mov0;  // mov r3.z, c9.w (0)
            mov0.mac = Mac::Mov, mov0.a = konst(), mov0.constant = 9, mov0.temp = 3, mov0.mac_temp_mask = 2;
            for (auto& s : mov0.a.swizzle) s = 3;
            f.program.code.push_back(mov0);
            Instruction att;  // dph r4.x, r3.xyz, c8: C·d² + B·d + A
            att.mac = Mac::Dph, att.a = temp(3), att.b = konst(), att.constant = 8, att.temp = 4, att.mac_temp_mask = 8;
            f.program.code.push_back(att);
            Instruction root;  // rsq r1.y, r4.x
            root.ilu = Ilu::Rsq, root.c = temp(4), root.temp = 1, root.ilu_temp_mask = 4;
            for (auto& s : root.c.swizzle) s = 0;
            f.program.code.push_back(root);
            Instruction scaled;  // mul r4.y, r1.y, c9.x
            scaled.mac = Mac::Mul, scaled.a = temp(1), scaled.b = konst(), scaled.constant = 9, scaled.temp = 4, scaled.mac_temp_mask = 4;
            for (auto& s : scaled.a.swizzle) s = 1;
            for (auto& s : scaled.b.swizzle) s = 0;
            f.program.code.push_back(scaled);
            size = temp(4);
            for (auto& s : size.swizzle) s = 1;
        }
        Instruction lo;  // max r4.z, size, c9.y
        lo.mac = Mac::Max, lo.a = size, lo.b = konst(), lo.constant = 9, lo.temp = 4, lo.mac_temp_mask = 2;
        for (auto& s : lo.b.swizzle) s = 1;
        if (!point_scale) lo.a = lo.b, lo.a.swizzle[0] = lo.a.swizzle[1] = lo.a.swizzle[2] = lo.a.swizzle[3] = 0;
        f.program.code.push_back(lo);
        Instruction hi;  // min oPts.x, r4.z, c9.z
        hi.mac = Mac::Min, hi.a = temp(4), hi.b = konst(), hi.constant = 9;
        for (auto& s : hi.a.swizzle) s = 2;
        for (auto& s : hi.b.swizzle) s = 2;
        out(hi, kPts, 8);
        f.program.code.push_back(hi);
    }
    for (uint8_t n = 0; n < texcount; ++n) mov(uint8_t(kT0 + n), uint8_t(9 + n));
    f.program.code.back().final = true;
    f.program.header = 0x2078u | uint32_t(f.program.code.size()) << 16;
    return f;
}

// The constants the original loads with its passthrough program (0x173900),
// into the GPU's c-96 and c-95, where they stay as they do on the hardware:
// the multisample scale, the depth scale and the w scale (the depth scale
// times the device's 0x940 for w-buffering, else 1); the screen-space
// offset, less half a pixel where the device's flag 0x8000 and render state
// 152 ask for it.
void passthrough_constants(rt::Context& c, Device& d) {
    const uint32_t dev = kDeviceStorage;
    auto f = [&](uint32_t a) { return std::bit_cast<float>(ld(c, a)); };
    const float wscale = ld(c, kRenderStateTable + 4 * kRsZEnable) == 2 ? f(dev + 0x944) * f(dev + 0x940) : 1.0f;
    const float half = (ld(c, dev + 8) & 0x8000) && ld(c, kRenderStateTable + 4 * 152) ? 0.5f : 0.0f;
    const float c96[4] = {f(dev + 0x958), f(dev + 0x95C), f(dev + 0x944), wscale};
    const float c95[4] = {f(dev + 0xEE8) - half, f(dev + 0xEEC) - half, 0.0f, 0.0f};
    for (int k = 0; k < 4; ++k) {
        d.vs_constants[0][k] = std::bit_cast<uint32_t>(c96[k]);
        d.vs_constants[1][k] = std::bit_cast<uint32_t>(c95[k]);
    }
}

// The composite the fixed-function program reads in c0-c3: the columns of
// World × View × Projection (D3D's row vectors: clip = v · W · V · P).
void composite_constants(const rt::Context& c, float* out) {
    auto matrix = [&](uint32_t state, float m[4][4]) {
        for (int k = 0; k < 16; ++k) m[k / 4][k % 4] = std::bit_cast<float>(ld(c, kDeviceStorage + kDevTransforms + 64 * state + 4 * k));
    };
    float w[4][4], v[4][4], p[4][4], wv[4][4] = {}, wvp[4][4] = {};
    matrix(kTsWorld, w), matrix(kTsView, v), matrix(kTsProjection, p);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) wv[i][j] += w[i][k] * v[k][j];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) wvp[i][j] += wv[i][k] * p[k][j];
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) out[col * 4 + row] = wvp[row][col], out[16 + col * 4 + row] = wv[row][col];
}

// The fog unit, as the original's flush (0x178AC0) sets it from the render
// states: the mode (0 when fog is off) and the two parameters, linear with
// (1, 1) on the vertices' own fog value without a table mode; linear from
// D3DRS_FOGSTART and FOGEND (1 / (end − start), 8192 when they're equal);
// exp and exp2 from D3DRS_FOGDENSITY times the constants in the image.
struct FogUnit {
    uint32_t mode = 0;
    float p0 = 0, p1 = 0;
};
FogUnit fog_unit(const rt::Context& c) {
    auto rs = [&](uint32_t i) { return ld(c, kRenderStateTable + 4 * i); };
    auto rf = [&](uint32_t i) { return std::bit_cast<float>(rs(i)); };
    auto img = [&](uint32_t a) { return std::bit_cast<float>(ld(c, a)); };
    if (!rs(kRsFogEnable)) return {};
    switch (rs(kRsFogTableMode)) {
        case 0: return {0x2601, 1.0f, 1.0f};
        case 3: {
            const float start = rf(kRsFogStart), end = rf(kRsFogEnd);
            const float scale = start == end ? img(0x180144) : 1.0f / (end - start);
            return {0x2601, end * scale + 1.0f, -scale};
        }
        case 1: return {0x800, 1.5f, rf(kRsFogDensity) * img(0x203734)};
        default: return {0x801, 1.5f, rf(kRsFogDensity) * img(0x203730)};
    }
}

// ---------------------------------------------------------------- the fixed-function pixel pipeline
// D3D's texture-stage operations, as register-combiner settings: a general
// combiner stage for each texture stage up to the first one disabled,
// "current" in r0 (the diffuse colour at stage 0), "temp" in r1, the
// texture factor in each stage's C0; the final combiner passes r0 through.
// Operations and arguments beyond these stop the runtime until they're
// modelled. (The original does this conversion in its state flush.)
constexpr uint32_t kRsSpecularEnable = 103, kRsTextureFactor = 148;
constexpr uint32_t kTsColorOp = 12, kTsAlphaOp = 16, kTsResultArg = 20;
constexpr uint32_t kOne = 0x20;  // the zero register, inverted: 1

void fixed_function_combiners(rt::Context& c, const char* fn, gfx::nv2a::CombinerSetup& cs, std::vector<float>& fsu) {
    auto rs = [&](uint32_t i) { return ld(c, kRenderStateTable + 4 * i); };
    auto ts = [&](uint32_t stage, uint32_t i) { return ld(c, kTextureStateTable + 4 * (32 * stage + i)); };
    cs = {};
    uint32_t stages = 0, modes = 0;
    for (uint32_t stage = 0; stage < 4; ++stage) {
        if (ts(stage, kTsColorOp) == 1) break;  // D3DTOP_DISABLE: this stage and all after it
        const uint32_t tex = ld(c, kDeviceStorage + 0xF78 + 4 * stage);
        // An argument as a combiner input: the register, alpha replicated
        // (D3DTA_ALPHAREPLICATE, or the alpha portion), complemented.
        auto arg = [&](uint32_t a, bool alpha) -> uint32_t {
            uint32_t reg = 0;
            switch (a & 0xF) {
                case 0: reg = 4; break;                    // D3DTA_DIFFUSE
                case 1: reg = stage ? 12 : 4; break;      // D3DTA_CURRENT: the diffuse colour at stage 0
                case 2:                                    // D3DTA_TEXTURE
                    if (!tex) unmodelled(fn, strf("stage %u reads a texture, and none is set", stage));
                    reg = 8 + stage;
                    break;
                case 3: reg = 1; break;                    // D3DTA_TFACTOR: C0
                case 4: reg = 5; break;                    // D3DTA_SPECULAR
                case 5: reg = 13; break;                   // D3DTA_TEMP: r1
                default: unmodelled(fn, strf("texture-stage argument %#x", a));
            }
            if (a & ~0x3Fu) unmodelled(fn, strf("texture-stage argument %#x", a));
            return reg | ((alpha || (a & 0x20)) ? 0x10u : 0u) | ((a & 0x10) ? 0x20u : 0u);
        };
        auto operation = [&](bool alpha, uint32_t& inputs, uint32_t& outputs) {
            const uint32_t base = alpha ? kTsAlphaOp : kTsColorOp;
            uint32_t op = ts(stage, base);
            const uint32_t a1 = ts(stage, base + 2), a2 = ts(stage, base + 3);
            if (alpha && op == 1) op = 2, inputs = 0;  // alpha disabled with colour on: current's alpha
            const uint32_t result = ts(stage, kTsResultArg) == 5 ? 13u : 12u;  // D3DTA_TEMP, else current
            if (ts(stage, kTsResultArg) != 1 && ts(stage, kTsResultArg) != 5)
                unmodelled(fn, strf("stage %u's result argument %#x", stage, ts(stage, kTsResultArg)));
            auto ab = [&](uint32_t a, uint32_t b) { return a << 24 | b << 16; };
            switch (op) {
                case 2: inputs = ab(alpha && ts(stage, base) == 1 ? (stage ? 0x1Cu : 0x14u) : arg(a1, alpha), kOne); outputs = result << 4; break;  // SELECTARG1
                case 3: inputs = ab(arg(a2, alpha), kOne); outputs = result << 4; break;                      // SELECTARG2
                case 4: inputs = ab(arg(a1, alpha), arg(a2, alpha)); outputs = result << 4; break;           // MODULATE
                case 5: inputs = ab(arg(a1, alpha), arg(a2, alpha)); outputs = result << 4 | 2u << 15; break; // MODULATE2X
                case 6: inputs = ab(arg(a1, alpha), arg(a2, alpha)); outputs = result << 4 | 4u << 15; break; // MODULATE4X
                case 7: inputs = ab(arg(a1, alpha), kOne) | arg(a2, alpha) << 8 | kOne; outputs = result << 8; break;  // ADD
                default: unmodelled(fn, strf("stage %u's %s operation %u", stage, alpha ? "alpha" : "colour", op));
            }
        };
        operation(false, cs.rgb_inputs[stage], cs.rgb_outputs[stage]);
        operation(true, cs.alpha_inputs[stage], cs.alpha_outputs[stage]);
        if (tex) {
            const uint32_t dims = (ld(c, tex + 12) >> 4) & 0xF, cube = ld(c, tex + 12) & 4;
            modes |= (cube ? 3u : dims == 3 ? 2u : 1u) << (5 * stage);  // CUBEMAP, PROJECT3D, PROJECT2D
        }
        ++stages;
    }
    if (!stages) {  // stage 0 disabled: the diffuse colour
        cs.rgb_inputs[0] = 4u << 24 | kOne << 16, cs.rgb_outputs[0] = 12u << 4;
        cs.alpha_inputs[0] = 0x14u << 24 | kOne << 16, cs.alpha_outputs[0] = 12u << 4;
        stages = 1;
    }
    cs.combiner_count = stages | 0x1000u;  // each stage its own C0 (all the texture factor)
    cs.texture_modes = modes;
    // The final combiner, as the original's fog flush (0x178AC0) sets it:
    // with fog, rgb = fog.a × r0 + (1 − fog.a) × fog colour; without, r0;
    // r0 + the specular colour (V1R0_SUM) in place of r0 with
    // D3DRS_SPECULARENABLE; alpha r0's, the sum clamped.
    const bool specular = rs(kRsSpecularEnable) != 0;
    cs.final_abcd = rs(kRsFogEnable) ? 0x130C0300u | (specular ? 0x20000u : 0u) : (specular ? 0x0Eu : 0x0Cu);
    cs.final_efgh = 0x1C80u;
    for (uint32_t i = 0; i < 8; ++i) color_to_float(rs(kRsTextureFactor), &fsu[(gfx::nv2a::kFsC0 + i) * 4]);
}

// A draw, as the original's state flush leaves the GPU, into the renderer.
void issue_draw(rt::Context& c, Device& d, const char* fn, uint32_t prim, Vertices&& v, std::vector<uint32_t> seq, bool indexed) {
    st(c, kDeviceStorage + 8, ld(c, kDeviceStorage + 8) | 0x800u);
    if (!v.count) {
        t_spare_vertices = std::move(v), t_spare_sequence = std::move(seq);
        return;
    }
    // An attribute the vertices don't have reads the GPU's register for it:
    // the last value SetVertexData* wrote there (0, 0, 0, 1 if never written).
    for (uint32_t n = 0; n < 16; ++n) {
        if ((v.mask & (1u << n)) || !(d.attributes_written & (1u << n))) continue;
        v.mask |= 1u << n;
        static const bool off = [] { const char* e = std::getenv("QILIN_NO_VERTEX_CACHE"); return e && *e == '1'; }();
        if (off) {
            v.attr[n].resize(size_t(v.count) * 4);
            for (uint32_t i = 0; i < v.count; ++i) std::copy_n(d.attribute_values[n], 4, &v.attr[n][size_t(i) * 4]);
            continue;
        }
        v.attr[n].assign(d.attribute_values[n], d.attribute_values[n] + 4);  // one value, for every vertex
        v.constant |= 1u << n;
    }
    const uint32_t dev = kDeviceStorage;
    auto rs = [&](uint32_t i) { return ld(c, kRenderStateTable + 4 * i); };

    // The back buffer, or a texture the game renders into.
    const auto [target_w, target_h] = apply_target(c, d, fn);

    gfx::DrawCmd cmd;
    // The vertex program at the selected slot of the GPU's program memory,
    // or the fixed-function pipeline as one.
    const uint32_t vs_flags = ld(c, ld(c, dev + kDevVertexShader) + 4);
    const bool passthrough = vs_flags & 0x2;  // pre-transformed (XYZRHW) vertices
    const bool fixed_function = !(vs_flags & 0x10) && !passthrough;
    uint64_t vs_key = 0;
    std::optional<gfx::vsh::Program> prog;
    if (passthrough) {
        // The original's own program for these (0x173900, in its state flush):
        // one of three in its image by the fog table mode, scaling by c-96 and
        // offsetting by c-95, which it loads with it (passthrough_constants).
        const uint32_t start = rs(kRsFogTableMode) == 0 ? 0x17FC28u : (ld(c, dev + 8) & 0x2) ? 0x17FB78u : 0x17FAB8u;
        const uint32_t dwords = start == 0x17FB78u ? 0x2Cu : 0x30u;
        std::vector<uint32_t> words{0x2078u | (dwords / 4) << 16};
        for (uint32_t i = 0; i < dwords; ++i) words.push_back(ld(c, start + 4 * i));
        vs_key = 0xF2F0000000000000ull ^ uint64_t(start) ^ (uint64_t(v.mask) << 40);
        if (d.shaders[vs_key].empty()) {
            std::string error;
            prog = gfx::vsh::decode(words.data(), words.size(), &error);
            if (!prog) unmodelled(fn, "the passthrough program doesn't decode (" + error + ")");
        }
        passthrough_constants(c, d);
    } else if (fixed_function) {
        FixedFunction f = fixed_function_program(c, fn, prim == 1);
        vs_key = f.key ^ (uint64_t(v.mask) << 40);
        prog = std::move(f.program);
    } else {
        const uint32_t start = ld(c, dev + kDevVertexShader + 8);
        if (start >= 136) unmodelled(fn, strf("a vertex program at slot %u", start));
        thread_local std::vector<uint32_t> words;  // kept from draw to draw: no allocation each time
        words.assign(1, 0x2078u);
        for (uint32_t i = start; i < 136; ++i) {
            words.insert(words.end(), d.vs_program[i], d.vs_program[i] + 4);
            if (d.vs_program[i][3] & 1) break;  // FINAL
        }
        words[0] |= uint32_t((words.size() - 1) / 4) << 16;  // the instruction count, as the header has it
        vs_key = hash_bytes(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4) ^ (uint64_t(v.mask) << 40) ^ 1;
        if (d.shaders[vs_key].empty()) {
            std::string error;
            prog = gfx::vsh::decode(words.data(), words.size(), &error);
            if (!prog) unmodelled(fn, "a vertex program that doesn't decode (" + error + ")");
        }
    }
    if (prog && !passthrough) d.position_matrix[vs_key] = position_matrix_of(*prog);
    auto& vs_code = d.shaders[vs_key];
    if (vs_code.empty()) {
        try {
            vs_code = gfx::nv2a::vertex_program(*prog, v.mask);
        } catch (const std::exception& e) {
            unmodelled(fn, e.what());
        }
        d.vs_programs[vs_key] = *prog;
    }

    // The register combiners as the render states hold them.
    gfx::nv2a::CombinerSetup cs{};
    for (uint32_t i = 0; i < 8; ++i) {
        cs.alpha_inputs[i] = rs(i);
        cs.alpha_outputs[i] = rs(kRsPsAlphaOutputs + i);
        cs.rgb_inputs[i] = rs(kRsPsRgbInputs + i);
        cs.rgb_outputs[i] = rs(kRsPsRgbOutputs + i);
    }
    cs.final_abcd = rs(8), cs.final_efgh = rs(9);
    cs.combiner_count = rs(kRsPsCombinerCount);
    cs.texture_modes = rs(kRsPsTextureModes);
    cs.input_texture = rs(kRsPsInputTexture), cs.dot_mapping = rs(kRsPsDotMapping), cs.compare_mode = rs(kRsPsCompareMode);
    cs.alpha_func = rs(kRsAlphaTestEnable) ? rs(kRsAlphaFunc) : 0;

    thread_local std::vector<float> fsu;
    fsu.assign(gfx::nv2a::kFsUniformVec4s * 4, 0.0f);
    const bool fixed_pixels = !ld(c, dev + 0x780);  // no pixel shader: the texture-stage operations
    if (fixed_pixels) {
        fixed_function_combiners(c, fn, cs, fsu);
        cs.alpha_func = rs(kRsAlphaTestEnable) ? rs(kRsAlphaFunc) : 0;
    }
    const FogUnit fog = fog_unit(c);
    cs.fog_mode = fog.mode;
    cs.point_sprite = prim == 1 && rs(kRsPointSpriteEnable);
    fsu[gfx::nv2a::kFsFogParams * 4 + 0] = fog.p0;
    fsu[gfx::nv2a::kFsFogParams * 4 + 1] = fog.p1;
    for (uint32_t stage = 0; stage < 4; ++stage) {
        fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 0] = fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 1] = 1;
        fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 2] = 1;
        const uint32_t mode = cs.texture_modes >> (5 * stage) & 0x1F;
        const uint32_t tex = ld(c, dev + 0xF78 + 4 * stage);
        // The bump-environment matrix and luminance terms, from the table
        // entries of the stage D3D pushed them for: the stage itself under a
        // pixel shader, else the one before (SetTextureState_BumpEnv, 0x1719D0).
        if (stage && (mode == 6 || mode == 7)) {
            const uint32_t from = fixed_pixels ? stage - 1 : stage;
            auto tsf = [&](uint32_t i) { return std::bit_cast<float>(ld(c, kTextureStateTable + 4 * (32 * from + i))); };
            for (uint32_t k = 0; k < 4; ++k) fsu[(gfx::nv2a::kFsBumpMat + stage) * 4 + k] = tsf(kTsBumpEnvMat00 + k);
            fsu[(gfx::nv2a::kFsBumpLum + stage) * 4 + 0] = tsf(kTsBumpEnvLScale);
            fsu[(gfx::nv2a::kFsBumpLum + stage) * 4 + 1] = tsf(kTsBumpEnvLOffset);
        }
        // Every mode that samples its stage's texture: the projected ones,
        // the bump maps, the dependent reads and the dot-product lookups.
        constexpr uint32_t kSamples = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 6 | 1u << 7 | 1u << 9 | 1u << 11 | 1u << 12 | 1u << 13 | 1u << 14 | 1u << 15 | 1u << 16;
        if (!tex || !(kSamples >> mode & 1)) continue;
        const Device::CachedTexture* t = stage_texture(c, d, fn, tex, stage);
        cs.texture_dims[stage] = uint8_t(t->dims);
        cmd.textures[stage] = t->id;
        cmd.samplers[stage] = stage_sampler(c, fn, stage, *t, std::max(1u, ld(c, tex + 12) >> 16 & 0xF));
        if (t->linear) {  // the hardware addresses linear textures in texels
            fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 0] = 1.0f / float(t->width);
            fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 1] = 1.0f / float(t->height);
            fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 2] = 1.0f / float(t->depth);
        }
    }
    if (const char* why = gfx::nv2a::combiners_unsupported(cs)) {
        std::string formats;
        for (uint32_t stage = 0; stage < 4; ++stage)
            if (const uint32_t tex = ld(c, dev + 0xF78 + 4 * stage))
                formats += strf(" t%u format %#x size %08X data %08X;", stage, ld(c, tex + 12), ld(c, tex + 16), ld(c, tex + 4));
        unmodelled(fn, std::string(why) + formats);
    }
    // The setup's key: the one before's when it's the same setup (most draws; the hash is a byte at a time).
    thread_local gfx::nv2a::CombinerSetup last_setup;
    thread_local uint64_t last_key = 0;
    if (!last_key || !(cs == last_setup)) last_setup = cs, last_key = cs.hash() ^ 2;
    const uint64_t fs_key = last_key;
    auto& fs_code = d.shaders[fs_key];
    if (fs_code.empty()) fs_code = gfx::nv2a::combiners(cs), d.fs_setups[fs_key] = cs;
    if (!fixed_pixels) {
        for (uint32_t i = 0; i < 8; ++i) {
            color_to_float(rs(kRsPsConstant0 + i), &fsu[(gfx::nv2a::kFsC0 + i) * 4]);
            color_to_float(rs(kRsPsConstant1 + i), &fsu[(gfx::nv2a::kFsC1 + i) * 4]);
        }
        color_to_float(rs(kRsPsFinalC0), &fsu[gfx::nv2a::kFsFinalC0 * 4]);
        color_to_float(rs(kRsPsFinalC1), &fsu[gfx::nv2a::kFsFinalC1 * 4]);
    }
    color_to_float(rs(kRsFogColor), &fsu[gfx::nv2a::kFsFogColor * 4]);
    fsu[gfx::nv2a::kFsAlphaRef * 4] = float(rs(kRsAlphaRef) & 0xFF);

    // Vertex constants, with the viewport's and the renderer's own.
    thread_local std::vector<float> vsu;
    vsu.assign(gfx::nv2a::kVsUniformVec4s * 4, 0.0f);
    std::memcpy(vsu.data(), d.vs_constants, sizeof d.vs_constants);
    viewport_constants(c, &vsu[58 * 4], &vsu[59 * 4]);  // c-38, c-37
    if (fixed_function && (ld(c, kDirtyFlags) & 0x80000000u)) {
        // The title's composite (SetModelView), already to the screen: the
        // program's screen-space step only divides by w.
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) vsu[size_t(col) * 4 + size_t(row)] = d.composite[row * 4 + col];
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) vsu[16 + size_t(col) * 4 + size_t(row)] = d.model_view[row * 4 + col];
        const float one[4] = {1, 1, 1, 0}, zero[4] = {};
        std::copy_n(one, 4, &vsu[58 * 4]);
        std::copy_n(zero, 4, &vsu[59 * 4]);
    } else if (fixed_function) {
        composite_constants(c, vsu.data());  // c0-c3, and the model view in c4-c7 (the program's own constants aren't used)
    }
    if (prim == 1) {
        // The point size's constants (fixed_function_program): c8 the
        // attenuation (C, B, 0, A), c9 (viewport height × size, or the size,
        // minimum, maximum, 0); the screen uniform's w the size for programs
        // that don't write oPts.
        auto rf = [&](uint32_t i) { return std::bit_cast<float>(rs(i)); };
        const float vh = float(ld(c, dev + kDevViewport + 12));
        const float scale = rs(kRsPointScaleEnable) ? vh : 1.0f;
        const float c8[4] = {rf(kRsPointScaleC), rf(kRsPointScaleB), 0.0f, rf(kRsPointScaleA)};
        const float c9[4] = {scale * rf(kRsPointSize), rf(kRsPointSizeMin), std::min(rf(kRsPointSizeMax), 63.875f), 0.0f};
        if (fixed_function) std::copy_n(c8, 4, &vsu[8 * 4]), std::copy_n(c9, 4, &vsu[9 * 4]);
        vsu[gfx::nv2a::kVsScreen * 4 + 3] = rf(kRsPointSize);
    }
    const float zscale = std::bit_cast<float>(ld(c, dev + 0x944));
    vsu[gfx::nv2a::kVsScreen * 4 + 0] = 2.0f / float(target_w);
    vsu[gfx::nv2a::kVsScreen * 4 + 1] = 2.0f / float(target_h);
    vsu[gfx::nv2a::kVsScreen * 4 + 2] = zscale > 0 ? 1.0f / zscale : 1.0f;
    vsu[gfx::nv2a::kVsPixels * 4] = float(d.renderer->resolution_scale_y());
    vsu[gfx::nv2a::kVsPixels * 4 + 1] = 1.0f;  // the HUD's squeeze across, and the x it's about (flat_overlay)
    vsu[gfx::nv2a::kVsPixels * 4 + 2] = 0.0f;

    // Flat: placed by a position matrix with no perspective (the HUD, the
    // menus' and pages' pictures and text), and where on screen across.
    bool flat = false;
    float flat_x0 = 0, flat_x1 = 0, flat_y0 = 0, flat_y1 = 0;
    if (const auto it = d.position_matrix.find(vs_key); !passthrough && it != d.position_matrix.end() && it->second >= 0 && !v.attr[0].empty()) {
        const float* m = &vsu[size_t(it->second) * 4];  // the x row at m[0], the w row at m[12]
        if (m[12] == 0 && m[13] == 0 && m[14] == 0 && m[15] != 0) {
            const float sx = vsu[(96 - 38) * 4], ox = vsu[(96 - 37) * 4], sy = vsu[(96 - 38) * 4 + 1], oy = vsu[(96 - 37) * 4 + 1];
            flat_x0 = flat_y0 = INFINITY, flat_x1 = flat_y1 = -INFINITY;
            for (uint32_t i = 0; i < v.count; ++i) {
                const float* p = &v.attr[0][(v.constant & 1) ? 0 : size_t(i) * 4];
                const float x = (p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + p[3] * m[3]) * sx / (p[3] * m[15]) + ox;
                const float y = (p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + p[3] * m[7]) * sy / (p[3] * m[15]) + oy;
                flat_x0 = std::min(flat_x0, x), flat_x1 = std::max(flat_x1, x);
                flat_y0 = std::min(flat_y0, y), flat_y1 = std::max(flat_y1, y);
            }
            flat = v.count > 0;
        }
    }
    // The HUD at its own shape on pictures wider than 4:3. The game lays its
    // 2D out in 640×480 and, for 16:9, lets the TV stretch it with the 3D;
    // what it draws flat across the whole frame (its menus and pages draw in
    // a 4:3 viewport of their own, already their shape) is squeezed back by
    // 4:3 over the picture's shape, about the screen's edge it's nearest (a
    // part within the left or right third) or its middle. Pictures across
    // the whole frame (videos, fades) stay as they are. A part is the line
    // it's on: the flat draws of this frame and the last that share its
    // height and sit within 3% of the frame's width of it, as the game draws
    // a line of text in pieces (the pause menu's "Chapter 1, Scene 9:
    // Basement" in three), which would otherwise each go their own way.
    const platform::Settings* hs = d.config.settings;
    if (flat && hs && hs->hud != platform::Settings::Hud::Stretched && d.aspect > 4.0f / 3.0f + 1e-3f) {
        const uint32_t target = ld(c, dev + kDevRenderTarget);
        const bool frame_buffer = std::find(d.frame_buffers.begin(), d.frame_buffers.end(), target) != d.frame_buffers.end();
        const float w = float(target_w);
        if (frame_buffer && ld(c, dev + kDevViewport) == 0 && ld(c, dev + kDevViewport + 8) == target_w && flat_x1 - flat_x0 < 0.9f * w) {
            d.flat_now.push_back({flat_x0, flat_x1, flat_y0, flat_y1});
            float lo = flat_x0, hi = flat_x1;
            const float gap = 0.03f * w, h = flat_y1 - flat_y0;
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto* list : {&d.flat_before, &d.flat_now})
                    for (const auto& r : *list) {
                        const float shared = std::min(r[3], flat_y1) - std::max(r[2], flat_y0);
                        if (shared <= 0.5f * std::min(h, r[3] - r[2]) || r[0] > hi + gap || r[1] < lo - gap) continue;
                        if (r[0] < lo || r[1] > hi) lo = std::min(lo, r[0]), hi = std::max(hi, r[1]), grew = true;
                    }
            }
            const float centre = (lo + hi) * 0.5f;
            const float about = hs->hud == platform::Settings::Hud::Middle ? w * 0.5f : centre < w / 3 ? 0.0f : centre > w * 2 / 3 ? w : w * 0.5f;
            vsu[gfx::nv2a::kVsPixels * 4 + 1] = (4.0f / 3.0f) / d.aspect;
            vsu[gfx::nv2a::kVsPixels * 4 + 2] = about;
        }
    }

    // Fixed-function state.
    cmd.blend = rs(kRsAlphaBlendEnable);
    if (cmd.blend) {
        cmd.src_color = cmd.src_alpha = blend_factor(fn, rs(kRsSrcBlend));
        cmd.dst_color = cmd.dst_alpha = blend_factor(fn, rs(kRsDestBlend));
        cmd.color_op = cmd.alpha_op = blend_op(fn, rs(kRsBlendOp));
    }
    color_to_float(rs(kRsBlendColor), cmd.blend_constant);
    const uint32_t cw = rs(kRsColorWriteEnable);
    cmd.write_mask = uint8_t((cw >> 16 & 1) | (cw >> 8 & 1) << 1 | (cw & 1) << 2 | (cw >> 24 & 1) << 3);
    const uint32_t zenable = rs(kRsZEnable);
    if (zenable == 2) notice_once(d, "w-buffering (D3DZB_USEW, drawn as z)");
    cmd.depth_test = zenable != 0;
    cmd.depth_write = zenable != 0 && rs(kRsZWriteEnable);
    cmd.depth_compare = compare(fn, rs(kRsZFunc));
    cmd.stencil_test = rs(kRsStencilEnable);
    if (cmd.stencil_test) {
        cmd.stencil_compare = compare(fn, rs(kRsStencilFunc));
        cmd.stencil_fail = stencil_op(fn, rs(kRsStencilFail));
        cmd.stencil_depth_fail = stencil_op(fn, rs(kRsStencilZFail));
        cmd.stencil_pass = stencil_op(fn, rs(kRsStencilPass));
        cmd.stencil_ref = uint8_t(rs(kRsStencilRef));
        cmd.stencil_read_mask = uint8_t(rs(kRsStencilMask));
        cmd.stencil_write_mask = uint8_t(rs(kRsStencilWriteMask));
    }
    const uint32_t cull = rs(kRsCullMode);
    // D3D's cull modes name the winding culled as it appears on screen:
    // D3DCULL_CW culls clockwise triangles, D3DCULL_CCW counter-clockwise
    // ones. (Checked on the attract sequence's books: the other way round,
    // pages read mirrored and the desk behind them vanishes.)
    // D3DRS_ZBIAS, as SetRenderState_ZBias (0x171660) gives the GPU: a
    // polygon offset of factor -ZBias/4 and units -ZBias (toward the eye
    // for a positive bias), for points, lines and fills alike.
    if (const int32_t zbias = int32_t(ld(c, kRsZBiasSlot))) {
        cmd.depth_bias = -float(zbias);
        cmd.depth_slope = -float(zbias) * 0.25f;
        // A biased draw that doesn't write depth lays something on a surface
        // already drawn (the projected shadows: the floor again, by another
        // vertex program). Its depth and the surface's are computed by
        // different programs, and on NVIDIA (GTX 1080 Ti) they still differ
        // by more than the console's 4 units in bands across the floor: the
        // shadows striped and flickered at every resolution and frame rate.
        // Eight times the console's constant offset (32 steps of 24-bit
        // depth: under 2 micro-units, nothing in front comes that close).
        if (!cmd.depth_write) cmd.depth_bias *= kOverlayBias;
    }
    // D3DRS_LINEWIDTH, as SetRenderState_LineWidth (0x171570) gives the
    // GPU: the width times the supersampling scale, in eighths of a pixel
    // up to 63.875. The renderer draws at the guest's resolution, so the
    // width as set.
    if (prim >= 2 && prim <= 4) {
        cmd.line_width = std::clamp(std::bit_cast<float>(ld(c, kRsLineWidthSlot)), 0.0f, 63.875f);
        if (cmd.line_width > 1 && !d.renderer->wide_lines()) notice_once(d, "lines wider than a pixel (this GPU draws only thin lines)");
    }
    cmd.cull = cull == 0x900 ? gfx::Cull::Clockwise : cull == 0x901 ? gfx::Cull::CounterClockwise : gfx::Cull::None;
    // The viewport's rectangle bounds what's drawn.
    cmd.scissor.x = int32_t(ld(c, dev + kDevViewport));
    cmd.scissor.y = int32_t(ld(c, dev + kDevViewport + 4));
    cmd.scissor.w = ld(c, dev + kDevViewport + 8);
    cmd.scissor.h = ld(c, dev + kDevViewport + 12);

    cmd.primitive = convert_primitive(fn, prim, seq, indexed);
    cmd.vs_key = vs_key, cmd.fs_key = fs_key, cmd.vs_code = &vs_code, cmd.fs_code = &fs_code;
    if (auto it = d.vs_programs.find(vs_key); it != d.vs_programs.end()) cmd.vs_program = &it->second;
    if (auto it = d.fs_setups.find(fs_key); it != d.fs_setups.end()) cmd.fs_setup = &it->second;
    cmd.inputs = v.mask;
    cmd.vertex_count = v.count;
    for (uint32_t n = 0; n < 16; ++n)
        if (v.mask & (1u << n)) {
            cmd.attributes[n] = v.attr[n].data();
            cmd.arena[n] = v.arena[n];
            if (v.constant & (1u << n)) cmd.constant_inputs |= 1u << n;
        }
    if (indexed) cmd.indices = seq.data(), cmd.index_count = uint32_t(seq.size());
    cmd.vs_uniforms = vsu.data();
    cmd.fs_uniforms = fsu.data();
    if (trace_draws(d)) {
        std::string t;
        for (uint32_t stage = 0; stage < 4; ++stage)
            if (const uint32_t tex = ld(c, dev + 0xF78 + 4 * stage); tex && cmd.textures[stage])
                t += strf(" t%u=%08X/%x", stage, ld(c, tex + 4), (ld(c, tex + 12) >> 8) & 0xFF);
        const uint32_t target = ld(c, dev + kDevRenderTarget), depth = ld(c, dev + kDevDepth);
        const std::string line =
            strf("draw %u %s rt=%08X zs=%08X %ux%u vp=%d,%d %ux%u prim=%u n=%u %s%s blend=%u %x/%x/%x z=%u/%u/%x st=%u zb=%u cw=%08X a=%u/%u "
                 "cull=%x modes=%x c0=%08X/%08X vs=%08X%s\n",
                 d.swaps, fn, ld(c, target + 4), depth ? ld(c, depth + 4) : 0u, target_w, target_h, cmd.scissor.x, cmd.scissor.y,
                 cmd.scissor.w, cmd.scissor.h, prim, v.count, passthrough ? "xyzrhw" : fixed_function ? "ff" : "prog", t.c_str(),
                 rs(kRsAlphaBlendEnable), rs(kRsSrcBlend), rs(kRsDestBlend), rs(kRsBlendOp), zenable, rs(kRsZWriteEnable), rs(kRsZFunc),
                 rs(kRsStencilEnable), ld(c, kRsZBiasSlot), cw, rs(kRsAlphaTestEnable), rs(kRsAlphaRef), cull, cs.texture_modes,
                 rs(kRsPsRgbInputs), rs(kRsPsRgbOutputs), ld(c, dev + kDevVertexShader),
                 flat ? strf(" flat=%.0f..%.0f", double(flat_x0), double(flat_x1)).c_str() : "");
        // A capture has every draw of its first frame, and of the others
        // those that draw into a texture, sample one the game draws into, or
        // have a depth bias (the shadows, the effects), with their details:
        // a whole frame's details are 4 MB.
        bool noteworthy = true;
        if (g_capture.log) {
            noteworthy = ld(c, kRsZBiasSlot) != 0 || std::ranges::find(d.frame_buffers, target) == d.frame_buffers.end();
            for (uint32_t stage = 0; stage < 4 && !noteworthy; ++stage)
                for (const auto& [addr, tt] : d.targets)
                    if (cmd.textures[stage] && tt.texture.id == cmd.textures[stage]) noteworthy = true;
        }
        if (noteworthy || d.swaps + kCaptureFrames == g_capture.until) trace_line(line);
        // QILIN_TRACE_DETAIL=1: each logged draw's stencil, alpha test, combiners and positions too.
        static const bool detail = std::getenv("QILIN_TRACE_DETAIL") != nullptr;
        if (g_capture.log ? noteworthy : (detail && trace_wanted(line))) {
            std::string x = strf("  stencil f=%u ref=%u mask=%x/%x ops=%u/%u/%u alpha f=%u ref=%u combiners=%u final=%08X/%08X modes=%x in=%x dot=%x\n",
                                 rs(kRsStencilFunc), rs(kRsStencilRef), rs(kRsStencilMask), rs(kRsStencilWriteMask), rs(kRsStencilFail),
                                 rs(kRsStencilZFail), rs(kRsStencilPass), rs(kRsAlphaFunc), rs(kRsAlphaRef), cs.combiner_count, cs.final_abcd,
                                 cs.final_efgh, cs.texture_modes, cs.input_texture, cs.dot_mapping);
            for (uint32_t i = 0; i < 8 && i < (cs.combiner_count & 0xFF); ++i)
                x += strf("  stage %u rgb %08X>%08X alpha %08X>%08X c0=%08X c1=%08X\n", i, cs.rgb_inputs[i], cs.rgb_outputs[i], cs.alpha_inputs[i],
                          cs.alpha_outputs[i], rs(kRsPsConstant0 + i), rs(kRsPsConstant1 + i));
            if (g_capture.log) {  // the program's first constants and the viewport's (c-38, c-37)
                for (int k : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -38, -37}) {
                    const float* q = &vsu[size_t(96 + k) * 4];
                    x += strf("%sc%d=(%.9g %.9g %.9g %.9g)", k % 4 == 0 || k == -38 ? "\n  " : " ", k, double(q[0]), double(q[1]), double(q[2]), double(q[3]));
                }
                x += "\n";
            }
            for (uint32_t i = 0; i < v.count && i < 4; ++i) {
                x += strf("  v%u", i);
                for (uint32_t n = 0; n < 16; ++n)
                    if ((v.mask & (1u << n)) && v.attr[n].size() >= ((v.constant & (1u << n)) ? 0 : i * 4) + 4) {  // not those in the arena
                        const float* a = v.attr[n].data() + ((v.constant & (1u << n)) ? 0 : i * 4);
                        x += strf(" a%u=(%.3g %.3g %.3g %.3g)", n, a[0], a[1], a[2], a[3]);
                    }
                x += "\n";
            }
            std::fputs(x.c_str(), g_capture.log ? g_capture.log : stderr);
        }
        const std::string capture_dir = g_capture.log ? g_capture.dir.string() : std::string();
        if (const char* dir = g_capture.log ? capture_dir.c_str() : std::getenv("QILIN_TRACE_DUMP"); dir && *dir && (g_capture.log || trace_wanted(line))) {
            static uint32_t n = 0;
            for (uint32_t stage = 0; stage < 4; ++stage)
                for (const auto& [addr, tt] : d.targets)
                    if (cmd.textures[stage] && tt.texture.id == cmd.textures[stage]) {
                        if (g_capture.log) {  // once for each state of the target, and not without end
                            if (g_capture.dumped[addr] == g_capture.version[addr] + 1 || g_capture.files >= 400) continue;
                            g_capture.dumped[addr] = g_capture.version[addr] + 1, ++g_capture.files;
                            std::fprintf(g_capture.log, "  target %08X as %u_%04u_%08X.pam\n", addr, d.swaps, n, addr);
                        }
                        const gfx::Image im = d.renderer->read_render_target(tt.texture.id);
                        if (FILE* f = std::fopen(strf("%s/%u_%04u_%08X.pam", dir, d.swaps, n++, addr).c_str(), "wb")) {
                            std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", im.width, im.height);
                            std::fwrite(im.rgba.data(), 1, im.rgba.size(), f);
                            std::fclose(f);
                        }
                    }
        }
    }
    // QILIN_NO_DRAW=1: nothing drawn (clears and presents only), so a slow
    // test machine reaches the frame rates a fast one does.
    static const bool no_draw = std::getenv("QILIN_NO_DRAW") != nullptr;
    if (!no_draw) d.renderer->draw(cmd);
    t_spare_vertices = std::move(v);
    t_spare_sequence = std::move(seq);
}

std::vector<uint32_t> identity_sequence(uint32_t n) {
    std::vector<uint32_t> s = take_sequence(n);
    for (uint32_t i = 0; i < n; ++i) s[i] = i;
    return s;
}

// VOID D3DDevice_DrawVertices(D3DPRIMITIVETYPE, UINT StartVertex, UINT VertexCount)
void draw_vertices(rt::Context& c) {
    const char* fn = "D3DDevice_DrawVertices";
    Device& d = device(fn);
    const uint32_t primitive = arg(c, 0), start = arg(c, 1), count = arg(c, 2);
    consume_dirty_state(c, 0);
    issue_draw(c, d, fn, primitive, vertex_arrays(c, fn, 0, start, count), identity_sequence(count), false);
    ret(c, c.eax, 12);
}

// VOID D3DDevice_DrawVerticesUP(D3DPRIMITIVETYPE, UINT VertexCount,
//     CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride): vertices
// from the caller's memory, which the original (0x1727B0) copies into the
// push buffer as inline vertices, each attribute from its slot's offset in
// the caller's vertex (the stream isn't read). Its state flush (0x179BC0)
// is the ordinary one (dirty flags left & 0xC0000070), then the inline
// layout: 0x20 off, 0x10 and 0x40 on, so the next ordinary draw sends the
// stream layout and base vertex again. The device's own copy of that layout
// (+0xBB8 on) only serves the original's copying, which this replaces.
void draw_vertices_up(rt::Context& c) {
    const char* fn = "D3DDevice_DrawVerticesUP";
    Device& d = device(fn);
    const uint32_t primitive = arg(c, 0), count = arg(c, 1), data = arg(c, 2), stride = arg(c, 3);
    uint32_t dirty = ld(c, kDirtyFlags);
    if (dirty & 0x3FFFFF8Fu) dirty &= 0xC0000070u;
    if (dirty & 0x20u) dirty = (dirty & ~0x20u) | 0x50u;
    st(c, kDirtyFlags, dirty);
    Vertices v;
    v.count = count;
    const auto sl = slots(c, fn);
    for (uint32_t n = 0; n < 16; ++n) {
        if (!sl[n].bytes) continue;
        v.mask |= 1u << n;
        v.attr[n].resize(size_t(count) * 4);
        uint8_t buf[16];
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t at = data + stride * i + sl[n].offset;
            for (uint32_t b = 0; b < sl[n].bytes; ++b) buf[b] = rt::ld8(mem(c), at + b, 0);
            convert_attribute(buf, sl[n].format, &v.attr[n][size_t(i) * 4]);
        }
    }
    issue_draw(c, d, fn, primitive, std::move(v), identity_sequence(count), false);
    const uint32_t flags = ld(c, kDeviceStorage + 8);
    st(c, kDeviceStorage + 8, flags & ~0x1800u);
    if (flags & 0x1000u) unmodelled(fn, "the device's flag 0x1000 (a kick-off the original makes after the draw)");
    ret(c, c.eax, 16);
}

// VOID D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE, UINT VertexCount, CONST WORD* pIndexData):
// relative to the base vertex index SetIndices left at +0x1C.
void draw_indexed_vertices(rt::Context& c) {
    const char* fn = "D3DDevice_DrawIndexedVertices";
    Device& d = device(fn);
    const uint32_t primitive = arg(c, 0), count = arg(c, 1), index_data = arg(c, 2);
    const uint32_t base = ld(c, kDeviceStorage + 0x1C);
    consume_dirty_state(c, base);
    std::vector<uint32_t> indices = take_sequence(count);
    uint32_t lo = 0xFFFF, hi = 0;
    for (uint32_t k = 0; k < count; ++k) {
        indices[k] = rt::ld16(mem(c), index_data + 2 * k, 0);
        lo = std::min(lo, indices[k]);
        hi = std::max(hi, indices[k]);
    }
    if (!count) lo = hi = 0;
    for (auto& i : indices) i -= lo;
    issue_draw(c, d, fn, primitive, vertex_arrays(c, fn, base, lo, count ? hi - lo + 1 : 0), std::move(indices), true);
    ret(c, c.eax, 12);
}

// ------------------------------------------------- the game's push-buffer writes
// D3DDevice_BeginPush(Count) returns room for Count dwords of raw NV2A
// methods, which the game fills and hands back with EndPush(pPush). The
// original first writes the dirty state out (0x179A20); here the renderer
// reads D3D's state when it draws. EndPush decodes what the game wrote:
// only the methods the game uses (SET_BEGIN_END, INLINE_ARRAY); anything
// else stops the run.
constexpr uint32_t kNv097SetBeginEnd = 0x17FC, kNv097InlineArray = 0x1818;
void begin_push(rt::Context& c) {
    Device& d = device("D3DDevice_BeginPush_4");
    const uint32_t count = arg(c, 0);
    if (d.push_begun) unmodelled("D3DDevice_BeginPush_4", "a BeginPush inside another");
    if (count > 0x40000) unmodelled("D3DDevice_BeginPush_4", strf("%u dwords", count));
    if (count + 1 > d.push_dwords) {
        d.push_dwords = std::max<uint32_t>(0x4000, (count + 1 + 0xFFF) & ~0xFFFu);
        d.push = contiguous(4 * d.push_dwords, 0, kPhysicalLimit - 1, 0x1000, 4);
        if (!d.push) rt::trap(0, "D3D: out of guest memory for the push buffer");
    }
    d.push_begun = count + 1;
    consume_dirty_state(c, 0);  // the original's BeginPush flushes the state (0x179A20 with base 0)
    ret(c, d.push, 4);
}

void decode_push(rt::Context& c, Device& d, uint32_t from, uint32_t to) {
    const char* fn = "D3DDevice_EndPush";
    uint32_t open = 0;  // the primitive between SET_BEGIN_END's
    thread_local std::vector<uint32_t> inline_data;  // kept from push to push: no allocation each time
    inline_data.clear();
    for (uint32_t at = from; at < to;) {
        const uint32_t header = ld(c, at);
        at += 4;
        if (header & 0xA0000003u) unmodelled(fn, strf("push-buffer jump or call %08X at %08X", header, at - 4));
        const uint32_t method = header & 0x1FFCu, count = (header >> 18) & 0x7FFu, subchannel = (header >> 13) & 7u;
        const bool same_method = header & 0x40000000u;
        if (subchannel) unmodelled(fn, strf("subchannel %u (method %04X)", subchannel, method));
        if (at + 4 * count > to) unmodelled(fn, strf("method %04X runs past the end", method));
        if (method == kNv097SetBeginEnd && count == 1) {
            const uint32_t primitive = ld(c, at);
            if (primitive) {
                if (open) unmodelled(fn, "SET_BEGIN_END inside another");
                open = primitive;
                inline_data.clear();
            } else {
                if (!open) unmodelled(fn, "SET_BEGIN_END(END) without a begin");
                Vertices v = inline_vertices(c, fn, inline_data);
                const uint32_t n = v.count;
                issue_draw(c, d, fn, open, std::move(v), identity_sequence(n), false);
                open = 0;
            }
        } else if (method == kNv097InlineArray && (same_method || count == 1)) {
            if (!open) unmodelled(fn, "INLINE_ARRAY outside SET_BEGIN_END");
            for (uint32_t k = 0; k < count; ++k) inline_data.push_back(ld(c, at + 4 * k));
        } else {
            unmodelled(fn, strf("NV2A method %04X x%u%s", method, count, same_method ? " (non-incrementing)" : ""));
        }
        at += 4 * count;
    }
    if (open) unmodelled(fn, "a push that leaves SET_BEGIN_END open");
}

void end_push(rt::Context& c) {
    Device& d = device("D3DDevice_EndPush");
    const uint32_t end = arg(c, 0);
    if (!d.push_begun) unmodelled("D3DDevice_EndPush", "an EndPush without BeginPush");
    if (end < d.push || end > d.push + 4 * d.push_begun || (end & 3))
        unmodelled("D3DDevice_EndPush", strf("end %08X outside the %u dwords at %08X", end, d.push_begun, d.push));
    d.push_begun = 0;
    decode_push(c, d, d.push, end);
    ret(c, c.eax, 4);
}

// ---------------------------------------------------------------- immediate mode
// D3DDevice_Begin, SetVertexData2f/4f/Color and End write NV2A methods
// themselves in the original: SET_BEGIN_END, then the GPU's attribute
// registers (VERTEX_DATA2F_M, 4F_M, 4UB, and SET_VERTEX4F for
// D3DVSDE_VERTEX). Writing attribute 0 emits a vertex with every
// register's current value. Here the registers are the device's, and End
// draws the vertices collected, with every attribute ever written.
constexpr uint32_t kDevStateFlags = 8;  // the device's flags word: 0x800 between Begin and End

void immediate_attribute(const char* fn, uint32_t reg, const float v[4]) {
    Device& d = device(fn);
    const bool provoke = reg == 0xFFFFFFFFu || reg == 0;  // D3DVSDE_VERTEX, or the position register
    // Outside Begin/End a register is a constant: draws whose vertices lack
    // that attribute read it (issue_draw).
    if (reg == 0xFFFFFFFFu) reg = 0;
    if (reg >= 16) unmodelled(fn, strf("attribute register %u", reg));
    if (provoke && !d.immediate) unmodelled(fn, "a vertex outside Begin/End");
    std::copy(v, v + 4, d.attribute_values[reg]);
    d.attributes_written |= 1u << reg;
    if (provoke)
        for (const auto& a : d.attribute_values) d.immediate_vertices.insert(d.immediate_vertices.end(), a, a + 4);
}

float arg_float(const rt::Context& c, int i) { return std::bit_cast<float>(arg(c, i)); }

// VOID D3DDevice_SetVertexData2f(INT Register, FLOAT a, FLOAT b): (a, b, 0, 1).
void set_vertex_data_2f(rt::Context& c) {
    const float v[4] = {arg_float(c, 1), arg_float(c, 2), 0.0f, 1.0f};
    immediate_attribute("D3DDevice_SetVertexData2f", arg(c, 0), v);
    ret(c, c.eax, 12);
}

// VOID D3DDevice_SetVertexData4f(INT Register, FLOAT a, FLOAT b, FLOAT c, FLOAT d).
void set_vertex_data_4f(rt::Context& c) {
    const float v[4] = {arg_float(c, 1), arg_float(c, 2), arg_float(c, 3), arg_float(c, 4)};
    immediate_attribute("D3DDevice_SetVertexData4f", arg(c, 0), v);
    ret(c, c.eax, 20);
}

// VOID D3DDevice_SetVertexDataColor(INT Register, D3DCOLOR Color): the
// original swaps red and blue into the 4UB method's byte order, so the
// register reads (R, G, B, A), as a D3DCOLOR vertex attribute does.
void set_vertex_data_color(rt::Context& c) {
    const uint32_t argb = arg(c, 1);
    const float v[4] = {float((argb >> 16) & 0xFF) / 255.0f, float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f,
                        float(argb >> 24) / 255.0f};
    immediate_attribute("D3DDevice_SetVertexDataColor", arg(c, 0), v);
    ret(c, c.eax, 8);
}

// VOID D3DDevice_Begin(D3DPRIMITIVETYPE PrimitiveType): flushes the dirty
// state as the original's 0x1799A0 does (keeping 0xC0000070) and sets the
// device's flag 0x800, as the original (draws set it too; End clears it).
void begin(rt::Context& c) {
    static constexpr const char* fn = "D3DDevice_Begin";
    Device& d = device(fn);
    const uint32_t prim = arg(c, 0);
    if (d.immediate) unmodelled(fn, "a Begin inside another");
    if (prim < 1 || prim > 10) unmodelled(fn, strf("primitive type %u", prim));
    st(c, kDirtyFlags, ld(c, kDirtyFlags) & 0xC0000070u);
    st(c, kDeviceStorage + kDevStateFlags, ld(c, kDeviceStorage + kDevStateFlags) | 0x800u);
    d.immediate = prim;
    d.immediate_vertices.clear();
    ret(c, c.eax, 4);
}

// VOID D3DDevice_End(): draws what was given since Begin.
void end(rt::Context& c) {
    static constexpr const char* fn = "D3DDevice_End";
    Device& d = device(fn);
    if (!d.immediate) unmodelled(fn, "an End without Begin");
    const uint32_t prim = d.immediate;
    d.immediate = 0;
    Vertices v;
    v.count = uint32_t(d.immediate_vertices.size() / 64);
    v.mask = d.attributes_written;
    for (uint32_t n = 0; n < 16; ++n) {
        if (!(v.mask & (1u << n))) continue;
        v.attr[n].resize(size_t(v.count) * 4);
        for (uint32_t i = 0; i < v.count; ++i)
            std::copy_n(&d.immediate_vertices[size_t(i) * 64 + n * 4], 4, &v.attr[n][size_t(i) * 4]);
    }
    if (v.count) {
        const uint32_t n = v.count;
        issue_draw(c, d, fn, prim, std::move(v), identity_sequence(n), false);
    }
    const uint32_t flags = ld(c, kDeviceStorage + kDevStateFlags);
    st(c, kDeviceStorage + kDevStateFlags, flags & ~0x1800u);
    if (flags & 0x1000u) unmodelled(fn, "the device's flag 0x1000 (a kick-off the original makes after End)");
    ret(c, c.eax, 0);
}

// ---------------------------------------------------------------- vertex shaders
struct VertexShader {
    std::vector<uint32_t> declaration;          // D3DVSD tokens, up to D3DVSD_END
    std::optional<gfx::vsh::Program> program;   // none: a declaration for the fixed-function pipeline
};
std::map<uint32_t, VertexShader>& vertex_shaders() {  // by handle
    static std::map<uint32_t, VertexShader> v;
    return v;
}

// HRESULT D3DDevice_CreateVertexShader(CONST DWORD* pDeclaration, CONST DWORD* pFunction,
//     DWORD* pHandle, DWORD Usage): the handle is the shader object's address
// with bit 0 set, as the original's (SetVertexShader tells programmable
// shaders from FVF codes by that bit). QILIN_DUMP_VSH=1 prints each one's
// declaration and disassembly, =2 its raw words too.
void create_vertex_shader(rt::Context& c) {
    device("D3DDevice_CreateVertexShader");
    const uint32_t decl = arg(c, 0), fn = arg(c, 1), out = arg(c, 2);
    VertexShader vs;
    for (uint32_t k = 0; decl && k < 256; ++k) {
        const uint32_t t = ld(c, decl + 4 * k);
        if (t == 0xFFFFFFFFu) break;  // D3DVSD_END
        vs.declaration.push_back(t);
    }
    if (fn) {
        const uint32_t n = std::min<uint32_t>(ld(c, fn) >> 16, 136);
        std::vector<uint32_t> words(1 + 4 * size_t(n));
        for (size_t k = 0; k < words.size(); ++k) words[k] = ld(c, fn + 4 * uint32_t(k));
        std::string error;
        vs.program = gfx::vsh::decode(words.data(), words.size(), &error);
        if (const char* v = std::getenv("QILIN_DUMP_VSH"); v && *v == '2') {
            std::fprintf(stderr, "vertex shader words:");
            for (uint32_t w : words) std::fprintf(stderr, " %08X", w);
            std::fprintf(stderr, "\n");
        }
        if (!vs.program) unmodelled("D3DDevice_CreateVertexShader", "a function that isn't a vertex program (" + error + ")");
    }
    // The shader object, as the original builds it (0x1732A0) with D3D's
    // own helpers: {refs 1, flags (Usage; 0x10 a program, 1 unless a plain
    // vertex program, 8 read/write), instructions, size in dwords, 16
    // attribute slots {stream, offset, format; 2 disabled} from the
    // declaration, then the program and the declaration's constant loads
    // in push-buffer form at +0x114}, from the game's allocator (0xF7270).
    const uint32_t usage = arg(c, 3);
    const uint32_t header = fn ? rt::ld16(mem(c), fn, 0) : 0, count = fn ? rt::ld16(mem(c), fn + 2, 0) : 0;
    const uint32_t program_bytes = fn ? count * 16 + 4 * ((count * 16 + 0x7F) >> 7) : 0;
    uint32_t decl_bytes = 0;
    if (decl) {
        host::call_guest(c, 0x1731A0, {decl, 0});
        decl_bytes = 4 * c.eax;
    }
    const uint32_t bytes = decl_bytes + program_bytes;
    host::call_guest(c, 0xF7270, {bytes + 0x118, 0x24800000});
    const uint32_t obj = c.eax;
    if (!obj) {
        ret(c, 0x8007000Eu, 16);  // E_OUTOFMEMORY
        return;
    }
    for (uint32_t k = 0; k < 0x46; ++k) st(c, obj + 4 * k, 0);
    st(c, obj + 4, usage);
    st(c, obj, 1);
    for (uint32_t k = 0; k < 16; ++k) st(c, obj + 0x1C + 16 * k, 2);
    if (decl) host::call_guest(c, 0x173000, {obj, decl, fn ? 0u : 1u});
    uint32_t program_dwords = 0;
    if (fn) {
        uint32_t flags = ld(c, obj + 4) | 0x10u;
        if (header != 0x2078) flags |= 1u;
        if (header == 0x7378) flags |= 8u;
        st(c, obj + 4, flags);
        st(c, obj + 8, count);
        host::call_guest(c, 0x172F90, {obj + 0x114, fn + 4, count * 4});
        program_dwords = (c.eax - (obj + 0x114)) >> 2;
    }
    if (decl) host::call_guest(c, 0x1731A0, {decl, obj + 0x114 + 4 * program_dwords});
    st(c, obj + 0xC, bytes >> 2);
    st(c, obj + 0x114 + 4 * (bytes >> 2), 0);
    const uint32_t handle = obj | 1u;
    if (const char* v = std::getenv("QILIN_DUMP_VSH"); v && *v && *v != '0') {
        std::fprintf(stderr, "vertex shader %08X: %zu declaration tokens", handle, vs.declaration.size());
        for (uint32_t t : vs.declaration) std::fprintf(stderr, " %08X", t);
        std::fprintf(stderr, "\n%s", vs.program ? gfx::vsh::disassemble(*vs.program).c_str() : "(no function)\n");
    }
    vertex_shaders()[handle] = std::move(vs);
    if (out) st(c, out, handle);
    ret(c, 0, 16);
}

// ---------------------------------------------------------------- state
// Setters store state where the original keeps it when the game (or
// lifted D3D code) reads it back; state only the GPU saw goes to the
// renderer's own copy (gfx state, filled in with drawing). None of them
// writes GPU commands: the draw calls turn the state into Vulkan.
constexpr uint32_t kTransformDirty = 0x17FD90;  // the dirty bits each transform sets (a table in D3D's data)

// VOID D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*): as the
// original, the matrix into the device (GetTransform, lifted, reads it
// there) and the transform's dirty bits. The values the original derives
// from the projection for fog and depth are the renderer's to compute.
void set_transform(rt::Context& c) {
    const uint32_t state = arg(c, 0), m = arg(c, 1);
    if (state > 21) unmodelled("D3DDevice_SetTransform", strf("transform state %u", state));
    for (uint32_t k = 0; k < 16; ++k) st(c, kDeviceStorage + kDevTransforms + 64 * state + 4 * k, ld(c, m + 4 * k));
    st(c, kDirtyFlags, ld(c, kDirtyFlags) | ld(c, kTransformDirty + 4 * state));
    ret(c, c.eax, 8);
}

// VOID D3DDevice_SetViewport(CONST D3DVIEWPORT8*): X, Y, Width, Height,
// MinZ, MaxZ. As the original: the right and bottom edges are clipped to the
// render target, the result stored in the device (+0xED0 …) and the
// viewport's dirty bit set. The fog, depth and projection values the
// original then derives are the renderer's to compute; the one the game can
// read back (GetProjectionViewportMatrix) stops the runtime until it is.
// A surface's width and height: from Size for linear surfaces, else from
// the log2 sizes in Format (swizzled), as D3D reads them.
std::pair<uint32_t, uint32_t> surface_size(const rt::Context& c, uint32_t surf) {
    const uint32_t size = ld(c, surf + 16), format = ld(c, surf + 12);
    if (size) return {(size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1};
    return {1u << ((format >> 20) & 0xF), 1u << ((format >> 24) & 0xF)};
}
void set_viewport(rt::Context& c) {
    Device& d = device("D3DDevice_SetViewport");
    const uint32_t vp = arg(c, 0);
    if (!vp) unmodelled("D3DDevice_SetViewport", "a null viewport");
    // The original measures the front buffer when the target is the back buffer (they're the same size).
    uint32_t target = ld(c, kDeviceStorage + kDevRenderTarget);
    if (target == ld(c, kDeviceStorage + kDevFrameBuffers)) target = ld(c, kDeviceStorage + kDevFrameBuffers + 4);
    const auto [tw, th] = surface_size(c, target);
    const uint32_t x = ld(c, vp), y = ld(c, vp + 4);
    const uint32_t right = std::min(x + ld(c, vp + 8), tw), bottom = std::min(y + ld(c, vp + 12), th);
    st(c, kDeviceStorage + kDevViewport, x);
    st(c, kDeviceStorage + kDevViewport + 4, y);
    st(c, kDeviceStorage + kDevViewport + 8, right - x);
    st(c, kDeviceStorage + kDevViewport + 12, bottom - y);
    st(c, kDeviceStorage + kDevViewport + 16, ld(c, vp + 16));  // MinZ
    st(c, kDeviceStorage + kDevViewport + 20, ld(c, vp + 20));  // MaxZ
    st(c, kDirtyFlags, ld(c, kDirtyFlags) | 0x100u);
    (void)d;
    ret(c, c.eax, 4);
}

// VOID D3DDevice_GetProjectionViewportMatrix(D3DMATRIX*): the original
// copies the device's composite of the projection and the viewport (+0x970),
// which D3D derives in its state code.
void get_projection_viewport_matrix(rt::Context& c) {
    unmodelled("D3DDevice_GetProjectionViewportMatrix", "the derived projection-viewport matrix");
    ret(c, c.eax, 4);
}

// VOID FASTCALL D3DDevice_SetRenderState_Simple(DWORD Method, DWORD Value):
// the GPU method and value for a render state that maps straight onto one.
// The game's inline SetRenderState has already stored the D3D value in
// D3D__RenderState, which the renderer reads; the method is kept for the
// states that have no table entry.
std::map<uint32_t, uint32_t>& simple_methods() {
    static std::map<uint32_t, uint32_t> m;
    return m;
}
void set_render_state_simple(rt::Context& c) {
    simple_methods()[c.ecx] = c.edx;
    ret(c, c.eax, 0);
}

// Drops the device's internal reference on a resource, as the original:
// Lock takes the current fence, and one with no references left is
// destroyed (D3D_DestroyResource, lifted).
void drop_internal_ref(rt::Context& c, uint32_t r, const char*) {
    st(c, r + 8, ld(c, kDeviceStorage + kDevFence));
    const uint32_t common = ld(c, r) - kIntRef1;
    st(c, r, common);
    if ((common & (kIntRefMask | kRefMask)) == 0) host::call_guest(c, 0x175890, {r});  // D3D_DestroyResource, lifted
}

// VOID D3DDevice_SetTexture(DWORD Stage, D3DBaseTexture*): as the original.
// The device holds the stage's texture (+0xF78) with an internal reference
// (Common + 0x80000), dropped from the one it replaces; the stage's format
// key (+0x0C: the format's swizzle/cube/linear bits, with bit 30 for the
// compressed and YUV formats 0x2A-0x31, or 0x80000000 for no texture)
// changes only when the format kind does, setting dirty bit 0x4000; the
// first texture on a stage also sets 0x800, no texture 0x4800.
constexpr uint32_t kDevTextures = 0xF78, kDevTextureKeys = 0x0C;
void set_texture(rt::Context& c) {
    const uint32_t stage = arg(c, 0), tex = arg(c, 1);
    if (stage > 3) unmodelled("D3DDevice_SetTexture", strf("texture stage %u", stage));
    const uint32_t slot = kDeviceStorage + kDevTextures + 4 * stage, key_slot = kDeviceStorage + kDevTextureKeys + 4 * stage;
    const uint32_t old = ld(c, slot);
    if (old) drop_internal_ref(c, old, "D3DDevice_SetTexture");
    st(c, slot, tex);
    if (!tex) {
        st(c, key_slot, 0x80000000u);
        st(c, kDirtyFlags, ld(c, kDirtyFlags) | 0x4800u);
        ret(c, c.eax, 8);
        return;
    }
    st(c, tex, ld(c, tex) + kIntRef1);
    const uint32_t format = ld(c, tex + 12);
    uint32_t key = format & 0x20F4u;
    if (ld(c, key_slot) != key) {
        if (key & 0x2000u) {
            const uint32_t code = format & 0xFF00u;
            key &= ~0x2000u;
            if (code >= 0x2A00u && code <= 0x3100u) key |= 0x40000000u;
        }
        st(c, key_slot, key);
        uint32_t dirty = ld(c, kDirtyFlags) | 0x4000u;
        if (!old) dirty |= 0x800u;
        st(c, kDirtyFlags, dirty);
    }
    ret(c, c.eax, 8);
}

// ------------------------------------------------- texture-stage state
// D3D__TextureState (0x181970): 32 dwords per stage. As the original, each
// setter stores the value there and marks the dirty bits the original does;
// the GPU methods it would push are left to the renderer.
uint32_t texture_state_slot(uint32_t stage, uint32_t state) { return kTextureStateTable + 4 * (32 * stage + state); }

void texture_stage_check(const char* fn, uint32_t stage) {
    if (stage > 3) unmodelled(fn, strf("texture stage %u", stage));
}

// VOID D3DDevice_SetTextureState_TexCoordIndex(DWORD Stage, DWORD Value):
// also, as the original, the texgen byte of the stage's push-buffer
// template (0x32CF41 + stage) and the device's mask of stages that generate
// normals or reflection vectors (+0x94C), dirty bit 0x200 when the first one
// appears, and 0x47F.
void tex_coord_index_impl(rt::Context& c, uint32_t stage, uint32_t value) {
    texture_stage_check("D3DDevice_SetTextureState_TexCoordIndex", stage);
    st(c, texture_state_slot(stage, 0x1C), value);
    const uint32_t mode = value & 0xFFFF0000u;
    const uint32_t needs_normals = mode == 0x10000u || mode == 0x30000u ? 1 : 0;  // CAMERASPACENORMAL, CAMERASPACEREFLECTIONVECTOR
    rt::st8(mem(c), 0x32CF41 + stage, uint8_t((mode ? stage : value) + 9), 0);
    const uint32_t mask = ld(c, kDeviceStorage + 0x94C);
    uint32_t dirty = ld(c, kDirtyFlags);
    if (!mask && needs_normals) dirty |= 0x200u;
    st(c, kDeviceStorage + 0x94C, (mask & ~(1u << stage)) | (needs_normals << stage));
    st(c, kDirtyFlags, dirty | 0x47Fu);
}
void set_tex_coord_index(rt::Context& c) {
    tex_coord_index_impl(c, arg(c, 0), arg(c, 1));
    ret(c, c.eax, 8);
}

// The bump-environment states (0x16-0x1B) and the border and colour-key
// colours only reach the GPU besides their table entries.
void set_bump_env(rt::Context& c) {
    const uint32_t stage = arg(c, 0), state = arg(c, 1);
    texture_stage_check("D3DDevice_SetTextureState_BumpEnv", stage);
    st(c, texture_state_slot(stage, state), arg(c, 2));
    ret(c, c.eax, 12);
}
void set_border_color(rt::Context& c) {
    texture_stage_check("D3DDevice_SetTextureState_BorderColor", arg(c, 0));
    st(c, texture_state_slot(arg(c, 0), 0x1D), arg(c, 1));
    ret(c, c.eax, 8);
}
void set_color_key_color(rt::Context& c) {
    texture_stage_check("D3DDevice_SetTextureState_ColorKeyColor", arg(c, 0));
    st(c, texture_state_slot(arg(c, 0), 0x1E), arg(c, 1));
    ret(c, c.eax, 8);
}

// VOID D3DDevice_SetTextureStageStateNotInline(DWORD Stage, D3DTEXTURESTAGESTATETYPE, DWORD):
// as the original, by state (the Xbox numbering): the sampler states
// (ADDRESSU to ALPHAKILL, 0-0xB) mark the stage's own dirty bit; COLOROP
// (0xC) marks 0x800, or 0x480F for the ops from 0x19 up (the bump-map ones,
// which change the texture modes); 0xD-0x15 mark their table entry's bits
// (0x20A260); BUMPENV*, TEXCOORDINDEX, BORDERCOLOR and COLORKEYCOLOR go to
// their own setters. Unknown states do nothing.
void set_texture_stage_state_not_inline(rt::Context& c) {
    const uint32_t stage = arg(c, 0), state = arg(c, 1), value = arg(c, 2);
    texture_stage_check("D3DDevice_SetTextureStageStateNotInline", stage);
    if (state == 0x1C) tex_coord_index_impl(c, stage, value);
    else if (state == 0x1D || state == 0x1E || (state >= 0x16 && state <= 0x1B)) st(c, texture_state_slot(stage, state), value);
    else if (state < 0x16) {
        uint32_t bits = 0;
        if (state < 0xC) bits = 1u << stage;
        else if (state == 0xC) bits = value < 0x19 ? 0x800u : 0x480Fu;
        else bits = ld(c, 0x20A260 + 4 * state);
        st(c, kDirtyFlags, ld(c, kDirtyFlags) | bits);
        st(c, texture_state_slot(stage, state), value);
    }
    ret(c, c.eax, 12);
}

// VOID D3DDevice_SetScreenSpaceOffset(FLOAT x, FLOAT y): as the original,
// the device keeps each plus D3D's pixel-centre bias (the float at 0x17FD10)
// at +0xEE8/+0xEEC; the viewport constants derived from them are the
// renderer's. (Single precision, as the x87 stores them: a float sum taken in
// double and rounded once is the correctly rounded float sum.)
void set_screen_space_offset(rt::Context& c) {
    const float bias = std::bit_cast<float>(ld(c, 0x17FD10));
    for (uint32_t k = 0; k < 2; ++k) {
        const float v = std::bit_cast<float>(arg(c, k));
        st(c, kDeviceStorage + 0xEE8 + 4 * k, std::bit_cast<uint32_t>(float(double(v) + double(bias))));
    }
    ret(c, c.eax, 8);
}

// ------------------------------------------------- vertex shader constants
// D3D's register numbers arrive biased by 96 (0-191). Every setter loads
// the GPU's constants (the renderer's copy here); all but the Fast one also
// update D3D's shadow (0x180D50, 16 bytes a register) as the original does,
// NotInline only when the device isn't recording (flag 0x10).
constexpr uint32_t kVsConstantShadow = 0x180D50;
void load_vs_constants(rt::Context& c, const char* fn, uint32_t reg, uint32_t data, uint32_t dwords, bool shadow) {
    Device& d = device(fn);
    if (reg >= 192 || dwords > 4 * (192 - reg)) unmodelled(fn, strf("%u dwords at constant register %u", dwords, reg));
    for (uint32_t k = 0; k < dwords; ++k) {
        const uint32_t v = ld(c, data + 4 * k);
        d.vs_constants[reg + k / 4][k % 4] = v;
        if (shadow) st(c, kVsConstantShadow + 16 * reg + 4 * k, v);
    }
}
// Fastcall: ecx the register, edx the data.
void set_vs_constant1(rt::Context& c) {
    load_vs_constants(c, "D3DDevice_SetVertexShaderConstant1", c.ecx, c.edx, 4, true);
    ret(c, c.eax, 0);
}
void set_vs_constant4(rt::Context& c) {
    load_vs_constants(c, "D3DDevice_SetVertexShaderConstant4", c.ecx, c.edx, 16, true);
    ret(c, c.eax, 0);
}
// ... and the number of dwords (4 a register) on the stack.
void set_vs_constant_not_inline(rt::Context& c) {
    const bool recording = rt::ld8(mem(c), kDeviceStorage + 8, 0) & 0x10;
    load_vs_constants(c, "D3DDevice_SetVertexShaderConstantNotInline", c.ecx, c.edx, arg(c, 0), !recording);
    ret(c, c.eax, 4);
}
void set_vs_constant_not_inline_fast(rt::Context& c) {
    load_vs_constants(c, "D3DDevice_SetVertexShaderConstantNotInlineFast", c.ecx, c.edx, arg(c, 0), false);
    ret(c, c.eax, 4);
}

// VOID D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE, DWORD): as the
// original, by range: the pixel-shader states (below 0x5C) go straight to the
// GPU and are stored; the simple ones (to 0x88) are stored and mark the dirty
// bits of their table entry (0x20A148); the rest go to their own setter
// through D3D's table (0x17F798), which the runtime replaces natively.
void set_render_state_not_inline(rt::Context& c) {
    const uint32_t state = arg(c, 0), value = arg(c, 1);
    if (state >= 0xA6) unmodelled("D3DDevice_SetRenderStateNotInline", strf("render state %#x", state));
    if (state >= 0x88) {
        host::call_guest(c, ld(c, 0x17F798 + 4 * state), {value});
    } else {
        if (state >= 0x5C) st(c, kDirtyFlags, ld(c, kDirtyFlags) | ld(c, 0x20A148 + 4 * state));
        st(c, kRenderStateTable + 4 * state, value);
    }
    ret(c, c.eax, 8);
}

// VOID D3DDevice_SetPixelShader(DWORD Handle): a pixel shader (a D3DPixelShader
// whose +8 is its D3DPIXELSHADERDEF), or 0 for the fixed function. As the
// original: the device keeps the handle (+0x780) and from the definition its
// final-combiner flag (+0x784), the texture-mode bit (+0x788) and the stage
// modes (+0x78C); unless the device is recording (flag 0x10 at +8) the
// definition's 0x39 registers are copied into D3D__RenderState, which the
// game can read back. The push-buffer writes are left to the renderer.
// Handle 0 also re-issues the texture factor, which only reaches the GPU.
void set_pixel_shader(rt::Context& c) {
    const uint32_t handle = arg(c, 0), dev = kDeviceStorage;
    st(c, dev + 0x780, handle);
    uint32_t dirty = ld(c, kDirtyFlags);
    if (!handle) {
        dirty |= 0x4800u;
        if (ld(c, dev + 0x784)) dirty |= 0x2000u;
        st(c, kDirtyFlags, dirty);
        ret(c, c.eax, 4);
        return;
    }
    const uint32_t def = ld(c, handle + 8);
    const uint32_t final_combiner = ld(c, def + 0x24) | ld(c, def + 0x20);
    if (!final_combiner && ld(c, dev + 0x784)) dirty |= 0x2000u;
    st(c, dev + 0x784, final_combiner);
    st(c, dev + 0x788, ld(c, def + 0xEC) & 0x100u);
    st(c, dev + 0x78C, ld(c, def + 0xD8));
    st(c, kDirtyFlags, dirty | 0x4000u);
    if (!(rt::ld8(mem(c), dev + 8, 0) & 0x10)) {
        for (uint32_t k = 0; k < 0x39; ++k) st(c, kRenderStateTable + 4 * k, ld(c, def + 4 * k));
        st(c, kRenderStateTable + 4 * 0x88, ld(c, def + 0xD8));  // D3DRS_PSTEXTUREMODES (0x181D90)
    }
    ret(c, c.eax, 4);
}

// VOID D3DDevice_SetPixelShaderConstant(DWORD Register, CONST void*, DWORD Count):
// as the original, each register's float4 becomes a D3DCOLOR (clamped to
// [0,1], times 255, rounded as cvtss2si does: to nearest even), kept at
// device +0x8E0, and copied into each combiner constant the current pixel
// shader maps to that register (the definition's PSC0Mapping, PSC1Mapping
// and final-combiner mapping against D3D's per-register pattern at
// 0x32CF80): D3DRS_PSCONSTANT0_0.., PSCONSTANT1_0.. and the two
// final-combiner constants in D3D__RenderState. Register 0's first three
// floats also reach the GPU as given.
void set_pixel_shader_constant(rt::Context& c) {
    const char* fn = "D3DDevice_SetPixelShaderConstant";
    Device& d = device(fn);
    const uint32_t first = arg(c, 0), data = arg(c, 1), count = arg(c, 2);
    const uint32_t shader = ld(c, kDeviceStorage + 0x780);
    if (count && !shader) unmodelled(fn, "constants with no pixel shader set");
    if (first + count > 16) unmodelled(fn, strf("registers %u..%u", first, first + count));
    const uint32_t def = count ? ld(c, shader + 8) : 0;
    auto f = [&](uint32_t a) { return std::bit_cast<float>(ld(c, a)); };
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t reg = first + k, src = data + 16 * k;
        uint32_t packed = 0;
        static constexpr int kShift[4] = {16, 8, 0, 24};  // x red, y green, z blue, w alpha
        for (int i = 0; i < 4; ++i) {
            float v = f(src + 4 * i);
            const float hi = f(0x32CF60 + 4 * i), lo = f(0x434240 + 4 * i), scale = f(0x32CF70 + 4 * i);
            v = v < hi ? v : hi;  // minps: the second operand unless the first is less
            v = v > lo ? v : lo;  // maxps
            v = v * scale;
            packed |= (uint32_t(int32_t(std::nearbyint(v))) & 0xFFu) << kShift[i];
        }
        st(c, kDeviceStorage + 0x8E0 + 4 * reg, packed);
        const uint32_t pattern = ld(c, 0x32CF80 + 4 * reg);
        const uint32_t c0 = pattern ^ ld(c, def + 0xE4), c1 = pattern ^ ld(c, def + 0xE8), fc = pattern ^ ld(c, def + 0xEC);
        for (uint32_t i = 0; i < 8; ++i) {
            if (!((c0 >> (4 * i)) & 0xF)) st(c, kRenderStateTable + 0x28 + 4 * i, packed);  // D3DRS_PSCONSTANT0_i (0x181B98)
            if (!((c1 >> (4 * i)) & 0xF)) st(c, kRenderStateTable + 0x48 + 4 * i, packed);  // D3DRS_PSCONSTANT1_i (0x181BB8)
        }
        if (!(fc & 0xF)) st(c, 0x181C1C, packed);          // D3DRS_PSFINALCOMBINERCONSTANT0
        if (!((fc >> 4) & 0xF)) st(c, 0x181C20, packed);   // D3DRS_PSFINALCOMBINERCONSTANT1
        if (k == 0 && first == 0)
            for (int i = 0; i < 3; ++i) d.ps_constant0[i] = ld(c, src + 4 * i);
    }
    ret(c, c.eax, 12);
}

// LoadVertexShader(Handle, Address): as the original, unless the device is
// recording (flag 0x10), D3D's shadows of the program (device +0x1098, 16
// bytes a slot) and of the declaration's constants (0x180D50) (the lifted
// 0x172EE0); the GPU's program memory and constants get what the shader
// object's push-buffer form at +0x114 loads: program words (method 0x0B00)
// from Address on, and constants (0x0B80) from the register 0x1EA4 names.
void load_vertex_shader_impl(rt::Context& c, const char* fn, uint32_t handle, uint32_t address) {
    Device& d = device(fn);
    const uint32_t obj = handle - 1;
    if (!(rt::ld8(mem(c), kDeviceStorage + 8, 0) & 0x10)) host::call_guest(c, 0x172EE0, {obj, address});
    const uint32_t dwords = ld(c, obj + 0xC);
    uint32_t program_slot = address, constant = 0;
    for (uint32_t at = obj + 0x114, end = at + 4 * dwords; at < end;) {
        const uint32_t header = ld(c, at);
        const uint32_t count = header >> 18, method = header & 0xFFFFu;
        at += 4;
        if (method == 0x0B00) {
            for (uint32_t k = 0; k < count; ++k, ++program_slot) {
                if (program_slot >= 136 * 4) unmodelled(fn, "a program past the GPU's 136 slots");
                d.vs_program[program_slot / 4 % 136][program_slot % 4] = ld(c, at + 4 * k);
            }
        } else if (method == 0x1EA4 && count == 1) {
            constant = ld(c, at) * 4;
        } else if (method == 0x0B80) {
            for (uint32_t k = 0; k < count; ++k, ++constant) {
                if (constant >= 192 * 4) unmodelled(fn, "constants past the GPU's 192");
                d.vs_constants[constant / 4][constant % 4] = ld(c, at + 4 * k);
            }
        } else {
            unmodelled(fn, strf("shader object method %04X x%u", method, count));
        }
        at += 4 * count;
    }
}

// SelectVertexShader(Handle, Address): as the original, with a handle the
// device takes the object (+0x790) and handle (+0x794), dirty bits 0x70
// and 0x1000 when the flags that matter (0x13) change; then the program
// start (+0x798) in either case.
void select_vertex_shader_impl(rt::Context& c, uint32_t handle, uint32_t address) {
    const uint32_t dev = kDeviceStorage;
    if (handle) {
        const uint32_t old_flags = ld(c, ld(c, dev + kDevVertexShader) + 4), obj = handle - 1;
        st(c, dev + kDevVertexShader, obj);
        st(c, dev + kDevVertexShader + 4, handle);
        uint32_t dirty = ld(c, kDirtyFlags) | 0x70u;
        if ((ld(c, obj + 4) ^ old_flags) & 0x13u) dirty |= 0x1000u;
        st(c, kDirtyFlags, dirty);
    }
    st(c, dev + kDevVertexShader + 8, address);
}

void load_vertex_shader(rt::Context& c) {
    load_vertex_shader_impl(c, "D3DDevice_LoadVertexShader", arg(c, 0), arg(c, 1));
    ret(c, c.eax, 8);
}
void select_vertex_shader(rt::Context& c) {
    select_vertex_shader_impl(c, arg(c, 0), arg(c, 1));
    ret(c, c.eax, 8);
}

// VOID D3DDevice_SetVertexShader(DWORD Handle): a vertex shader (object | 1)
// or an FVF, which the original turns into its fixed-function shader object
// at 0x184478 (the lifted 0x172DA0). As the original: the device keeps the
// object (+0x790), the handle (+0x794) and clears +0x798; dirty bits 0x70,
// 0x400 when the attribute layout (+0x10) changes and 0x1600 when the
// shader's flags (+4) do. The program upload and the viewport constants it
// pushes are left to the renderer, which reads these at draw time.
// VOID D3DDevice_SetModelView(CONST D3DMATRIX* pModelView,
//     CONST D3DMATRIX* pInverseModelView, CONST D3DMATRIX* pComposite):
// the title's own transforms for the fixed-function pipeline, the composite
// as D3D's own is, through the viewport to the screen (its
// GetProjectionViewportMatrix). As the original (0x174BC0): with a
// composite, dirty flag 0x80000000 on and 0x200 off, the composite kept at
// the device's +0x9B0 unless its flag 0x10 is set, and the three pushed to
// the GPU (the inverse's 4×3 only if given), which the runtime keeps for
// draws; with none, 0x80000000 off and 0x200 on (D3D's own again).
void set_model_view(rt::Context& c) {
    const char* fn = "D3DDevice_SetModelView";
    Device& d = device(fn);
    const uint32_t mv = arg(c, 0), inv = arg(c, 1), comp = arg(c, 2);
    const uint32_t dirty = ld(c, kDirtyFlags);
    if (!comp) {
        st(c, kDirtyFlags, (dirty & 0x7FFFFDFFu) | 0x200u);
        ret(c, c.eax, 12);
        return;
    }
    st(c, kDirtyFlags, (dirty & ~0x200u) | 0x80000000u);
    if (!(ld(c, kDeviceStorage + 8) & 0x10))
        for (uint32_t k = 0; k < 16; ++k) st(c, kDeviceStorage + 0x9B0 + 4 * k, ld(c, comp + 4 * k));
    auto f = [&](uint32_t a) { return std::bit_cast<float>(ld(c, a)); };
    for (uint32_t k = 0; k < 16; ++k) d.model_view[k] = f(mv + 4 * k), d.composite[k] = f(comp + 4 * k);
    if (inv)
        for (uint32_t k = 0; k < 12; ++k) d.inverse_model_view[k] = f(inv + 4 * k);
    ret(c, c.eax, 12);
}

// What the original's SetVertexShader (0x1739F0) pushes for a fixed-function
// shader object, besides its program: the GPU's attribute registers for
// the colours its vertices lack (the object's flags at +4: 0x400 diffuse,
// 0x800 specular, 0x1000 back diffuse, 0x2000 back specular), diffuse ones
// white and specular ones zero (SET_VERTEX_DATA4UB). Draws without that
// attribute read the register (issue_draw), so a vertex with no colour is white.
void absent_colour_registers(rt::Context& c, uint32_t vs) {
    Device& d = device("D3DDevice_SetVertexShader");
    const uint32_t flags = ld(c, vs + 4);
    constexpr struct { uint32_t flag, reg; float value; } kColours[] = {{0x400, 3, 1}, {0x800, 4, 0}, {0x1000, 7, 1}, {0x2000, 8, 0}};
    for (const auto& k : kColours) {
        if (flags & k.flag) continue;
        std::fill_n(d.attribute_values[k.reg], 4, k.value);
        d.attributes_written |= 1u << k.reg;
    }
}

void set_vertex_shader(rt::Context& c) {
    const char* fn = "D3DDevice_SetVertexShader";
    const uint32_t handle = arg(c, 0), dev = kDeviceStorage;
    const uint32_t old = ld(c, dev + kDevVertexShader);
    if (!old) unmodelled(fn, "no current vertex shader (the device's init sets the fixed-function one)");
    const uint32_t old_flags = ld(c, old + 4), old_layout = ld(c, old + 0x10);
    uint32_t vs = 0;
    if (handle & 1) {
        if (rt::ld8(mem(c), handle + 3, 0) & 0x10) {  // the object's flags: a vertex program
            load_vertex_shader_impl(c, fn, handle, 0);
            select_vertex_shader_impl(c, handle, 0);
            ret(c, c.eax, 4);
            return;
        }
        vs = handle - 1;
    } else {
        vs = kFixedFunctionShader;
        host::call_guest(c, 0x172DA0, {vs, handle});
    }
    uint32_t dirty = ld(c, kDirtyFlags) | 0x70u;
    if (old_layout != ld(c, vs + 0x10)) dirty |= 0x400u;
    if (old_flags != ld(c, vs + 4)) dirty |= 0x1600u;
    st(c, kDirtyFlags, dirty);
    st(c, dev + kDevVertexShader, vs);
    st(c, dev + kDevVertexShader + 4, handle);
    st(c, dev + kDevVertexShader + 8, 0);
    absent_colour_registers(c, vs);
    ret(c, c.eax, 4);
}

// VOID D3DDevice_SetVertexShaderInput(DWORD Handle, DWORD StreamCount,
//     CONST D3DSTREAM_INPUT*): a shader with its own streams ({VertexBuffer,
// Stride, Offset} each), or with Handle 0 back to the device's streams. As
// the original: the device holds the buffers (internal references), keeps
// the handle (+0xAF0), the count (+0xAF4) and a copy of the inputs (+0xAF8),
// and swaps dirty bits 0x70 and 0x40000000.
constexpr uint32_t kDevInputHandle = 0xAF0, kDevInputCount = 0xAF4, kDevInputs = 0xAF8;
void set_vertex_shader_input(rt::Context& c) {
    const char* fn = "D3DDevice_SetVertexShaderInput";
    const uint32_t handle = arg(c, 0), count = arg(c, 1), inputs = arg(c, 2);
    if (handle && count > 16) unmodelled(fn, strf("%u streams", count));
    for (uint32_t k = 0; handle && k < count; ++k) {
        const uint32_t vb = ld(c, inputs + 12 * k);
        st(c, vb, ld(c, vb) + kIntRef1);
    }
    const uint32_t held = ld(c, kDeviceStorage + kDevInputCount);
    for (uint32_t k = 0; k < held; ++k) drop_internal_ref(c, ld(c, kDeviceStorage + kDevInputs + 12 * k), fn);
    const uint32_t dirty = ld(c, kDirtyFlags);
    if (!handle) {
        st(c, kDirtyFlags, (dirty & ~0x40000000u) | 0x70u);
        st(c, kDeviceStorage + kDevInputCount, 0);
        ret(c, c.eax, 12);
        return;
    }
    st(c, kDirtyFlags, (dirty & ~0x70u) | 0x40000000u);
    st(c, kDeviceStorage + kDevInputHandle, handle);
    st(c, kDeviceStorage + kDevInputCount, count);
    for (uint32_t k = 0; k < 3 * count; ++k) st(c, kDeviceStorage + kDevInputs + 4 * k, ld(c, inputs + 4 * k));
    ret(c, c.eax, 12);
}

// The render states D3D sets through functions (the "complex" ones, whose
// GPU encoding depends on other state): VOID D3DDevice_SetRenderState_X(DWORD
// Value). As the originals: the value into its slot of D3D's render-state
// table, and the dirty bits the original sets (TwoSidedLighting's only when
// enabled). The GPU state they derive is the renderer's to compute.
struct ComplexRenderState {
    const char* name;
    uint32_t slot;   // its D3D__RenderState address
    uint32_t dirty;  // dirty bits set
};
constexpr ComplexRenderState kComplexRenderStates[] = {
    {"D3DDevice_SetRenderState_PSTextureModes", 0x181D90, 0x0},
    {"D3DDevice_SetRenderState_EdgeAntiAlias", 0x181DCC, 0x0},
    {"D3DDevice_SetRenderState_ShadowFunc", 0x181DE0, 0x0},
    {"D3DDevice_SetRenderState_FogColor", 0x181D98, 0x0},
    {"D3DDevice_SetRenderState_CullMode", 0x181DBC, 0x0},
    {"D3DDevice_SetRenderState_FrontFace", 0x181DB8, 0x0},
    {"D3DDevice_SetRenderState_NormalizeNormals", 0x181DA8, 0x0},
    {"D3DDevice_SetRenderState_TextureFactor", 0x181DC0, 0x0},
    {"D3DDevice_SetRenderState_LineWidth", 0x181DE4, 0x0},
    {"D3DDevice_SetRenderState_Dxt1NoiseEnable", 0x181DEC, 0x0},
    {"D3DDevice_SetRenderState_ZBias", 0x181DC4, 0x0},
    {"D3DDevice_SetRenderState_LogicOp", 0x181DC8, 0x0},
    {"D3DDevice_SetRenderState_FillMode", 0x181D9C, 0x0},
    {"D3DDevice_SetRenderState_BackFillMode", 0x181DA0, 0x0},
    {"D3DDevice_SetRenderState_TwoSidedLighting", 0x181DA4, 0x9000},
    {"D3DDevice_SetRenderState_VertexBlend", 0x181D94, 0x200},
    {"D3DDevice_SetRenderState_ZEnable", 0x181DAC, 0x0},
    {"D3DDevice_SetRenderState_StencilEnable", 0x181DB0, 0x0},
    {"D3DDevice_SetRenderState_StencilFail", 0x181DB4, 0x0},
    {"D3DDevice_SetRenderState_YuvEnable", 0x181DF0, 0x0},
    {"D3DDevice_SetRenderState_OcclusionCullEnable", 0x181DF4, 0x0},
    {"D3DDevice_SetRenderState_StencilCullEnable", 0x181DF8, 0x0},
    {"D3DDevice_SetRenderState_RopZCmpAlwaysRead", 0x181DFC, 0x0},
    {"D3DDevice_SetRenderState_RopZRead", 0x181E00, 0x0},
    {"D3DDevice_SetRenderState_DoNotCullUncompressed", 0x181E04, 0x0},
    {"D3DDevice_SetRenderState_MultiSampleMode", 0x181DD8, 0x0},
    {"D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x181DDC, 0x0},
    {"D3DDevice_SetRenderState_MultiSampleAntiAlias", 0x181DD0, 0x0},
    {"D3DDevice_SetRenderState_MultiSampleMask", 0x181DD4, 0x0},
    {"D3DDevice_SetRenderState_SampleAlpha", 0x181DE8, 0x0},
};
void set_complex_render_state(rt::Context& c, const ComplexRenderState& r) {
    const uint32_t v = arg(c, 0);
    st(c, r.slot, v);
    const bool sets_dirty = r.dirty && (std::string_view(r.name) != "D3DDevice_SetRenderState_TwoSidedLighting" || v != 0);
    if (sets_dirty) st(c, kDirtyFlags, ld(c, kDirtyFlags) | r.dirty);
    ret(c, c.eax, 4);
}
template <size_t N> void complex_render_state(rt::Context& c) { set_complex_render_state(c, kComplexRenderStates[N]); }
template <size_t... I> constexpr std::array<HleFn, sizeof...(I)> complex_render_state_fns(std::index_sequence<I...>) {
    return {complex_render_state<I>...};
}
constexpr auto kComplexRenderStateFns = complex_render_state_fns(std::make_index_sequence<std::size(kComplexRenderStates)>());

struct D3dFn {
    const char* name;
    uint32_t arg_bytes;  // the original's stdcall argument bytes, as its ret n pops them (checked when written)
    HleFn fn;
};
constexpr D3dFn kD3d[] = {
    {"Direct3D_SetPushBufferSize", 8, set_push_buffer_size},
    {"Direct3D_CreateDevice", 24, create_device},
    {"D3DDevice_GetBackBuffer2", 4, get_back_buffer2},
    {"D3DDevice_GetDepthStencilSurface2", 0, get_depth_stencil_surface2},
    {"D3DDevice_GetRenderTarget2", 0, get_render_target2},
    {"D3D_BlockOnResource", 4, block_on_resource},
    {"D3DVertexBuffer_Lock2", 8, vertex_buffer_lock2},
    {"D3DResource_Release", 4, resource_release},
    {"D3DDevice_Release", 0, device_release},
    {"D3DResource_IsBusy", 4, resource_is_busy},
    {"D3DResource_Register", 8, resource_register},
    {"D3DDevice_SetVerticalBlankCallback", 4, set_vertical_blank_callback},
    {"D3DDevice_SetGammaRamp", 8, set_gamma_ramp},
    {"D3DDevice_BlockUntilIdle", 0, block_until_idle},
    {"D3DDevice_Clear", 24, clear},
    {"D3DDevice_CreateVertexShader", 16, create_vertex_shader},
    {"D3DDevice_SetTransform", 8, set_transform},
    {"D3DDevice_SetRenderState_Simple", 0, set_render_state_simple},
    {"D3DDevice_SetViewport", 4, set_viewport},
    {"D3DDevice_SetTexture", 8, set_texture},
    {"D3DDevice_SetVertexShaderInput", 12, set_vertex_shader_input},
    {"D3DDevice_SetPixelShader", 4, set_pixel_shader},
    {"D3DDevice_SetPixelShaderConstant", 12, set_pixel_shader_constant},
    {"D3DDevice_SetVertexShader", 4, set_vertex_shader},
    {"D3DDevice_SetModelView", 12, set_model_view},
    {"D3DDevice_LoadVertexShader", 8, load_vertex_shader},
    {"D3DDevice_SelectVertexShader", 8, select_vertex_shader},
    {"D3DDevice_SetRenderStateNotInline", 8, set_render_state_not_inline},
    {"D3DDevice_SetScreenSpaceOffset", 8, set_screen_space_offset},
    {"D3DDevice_SetVertexShaderConstant1", 0, set_vs_constant1},
    {"D3DDevice_BeginPush_4", 4, begin_push},
    {"D3DDevice_MakeSpace", 0, make_space},
    {"D3DDevice_BeginStateBig", 4, begin_state_big},
    {"D3DDevice_SetRenderTarget", 8, set_render_target},
    {"D3DDevice_DrawVertices", 12, draw_vertices},
    {"D3DDevice_DrawVerticesUP", 16, draw_vertices_up},
    {"D3DDevice_DrawIndexedVertices", 12, draw_indexed_vertices},
    {"D3DDevice_InsertFence", 0, insert_fence},
    {"D3DDevice_BlockOnFence", 4, block_on_fence},
    {"D3DDevice_EndPush", 4, end_push},
    {"D3DDevice_SetVertexShaderConstant4", 0, set_vs_constant4},
    {"D3DDevice_SetVertexShaderConstantNotInline", 4, set_vs_constant_not_inline},
    {"D3DDevice_SetVertexShaderConstantNotInlineFast", 4, set_vs_constant_not_inline_fast},
    {"D3DDevice_SetTextureStageStateNotInline", 12, set_texture_stage_state_not_inline},
    {"D3DDevice_SetTextureState_TexCoordIndex", 8, set_tex_coord_index},
    {"D3DDevice_SetTextureState_BumpEnv", 12, set_bump_env},
    {"D3DDevice_SetTextureState_BorderColor", 8, set_border_color},
    {"D3DDevice_SetTextureState_ColorKeyColor", 8, set_color_key_color},
    {"D3DDevice_GetProjectionViewportMatrix", 4, get_projection_viewport_matrix},
    {"D3DDevice_Swap", 4, swap},
    {"D3DDevice_CopyRects", 20, copy_rects},
    {"D3DDevice_Begin", 4, begin},
    {"D3DDevice_End", 0, end},
    {"D3DDevice_SetVertexData2f", 12, set_vertex_data_2f},
    {"D3DDevice_SetVertexData4f", 20, set_vertex_data_4f},
    {"D3DDevice_SetVertexDataColor", 8, set_vertex_data_color},
};

D3dConfig& config_storage() {
    static D3dConfig cfg;
    return cfg;
}

}  // namespace

const D3dConfig& d3d_config() { return config_storage(); }

void register_d3d(const D3dConfig& config) {
    config_storage() = config;
    add_idle_hook([](rt::Context& c) {
        if (!g_device) return;
        Device& d = *g_device;
        pump_window(d);
        // Pixels the title writes into a frame buffer while it waits (no draws, no Swap).
        const auto now = std::chrono::steady_clock::now();
        if (!d.cpu_frames.empty() && now - d.cpu_frames_checked >= std::chrono::milliseconds(16)) {
            d.cpu_frames_checked = now;
            flush_cpu_frames(c, d);
        }
    });
    register_hle(0x0FC030, "the game's frame clock", frame_clock);
    for (const D3dFn& f : kD3d) {
        const XdkSymbol* s = xdk_symbol(f.name);
        if (!s) {
            std::fprintf(stderr, "qilin: D3D replacement %s has no address in this XBE\n", f.name);
            std::abort();
        }
        register_hle(s->address, f.name, f.fn);
    }
    for (size_t k = 0; k < std::size(kComplexRenderStates); ++k) {
        const XdkSymbol* s = xdk_symbol(kComplexRenderStates[k].name);
        if (!s) {
            std::fprintf(stderr, "qilin: D3D replacement %s has no address in this XBE\n", kComplexRenderStates[k].name);
            std::abort();
        }
        register_hle(s->address, kComplexRenderStates[k].name, kComplexRenderStateFns[k]);
    }
}

}  // namespace qilin::host

