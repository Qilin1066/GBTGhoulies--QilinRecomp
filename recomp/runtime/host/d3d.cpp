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
#include <utility>
#include <vector>

#include "gfx.hpp"
#include "nv2a_shaders.hpp"
#include "texture.hpp"
#include "vsh.hpp"
#include "host.hpp"
#include "window.hpp"
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
    D3dConfig config;
    uint32_t width = 0, height = 0, interval = 1;
    std::vector<uint32_t> frame_buffers;  // guest surfaces; frame_buffers[i] is the renderer's buffer i
    uint32_t depth = 0;                   // guest surface, or 0
    uint32_t vblank_callback = 0, vblank_data = 0;
    uint32_t vblanks = 0, swaps = 0;
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
    // Draws decoded since the last Swap, until the renderer draws them.
    // Shaders made so far, by key, and textures uploaded, by their header and address.
    std::unordered_map<uint64_t, std::vector<uint32_t>> shaders;
    struct CachedTexture {
        uint32_t id = 0;
        uint64_t content = 0;  // a hash of the texels when uploaded
        uint32_t checked = ~0u;  // the frame the texels were last compared
        uint32_t dims = 2;
        bool linear = false;
        uint32_t width = 0, height = 0, depth = 1;
    };
    std::unordered_map<uint64_t, CachedTexture> textures;
    std::vector<std::string> said;  // notices given once
};
Device* g_device = nullptr;

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

// ---------------------------------------------------------------- frames out
void deliver_frame(Device& d) {
    const uint64_t n = d.renderer->frames_presented();
    if (!d.config.dump_dir.empty()) {
        gfx::Image im = d.renderer->read_front_buffer();
        for (size_t i = 0; i < im.rgba.size(); i += 4) {  // as the TV gets it: through the gamma ramp, alpha unused
            for (int k = 0; k < 3; ++k) im.rgba[i + k] = d.gamma[k][im.rgba[i + k]];
            im.rgba[i + 3] = 255;
        }
        const std::string path = d.config.dump_dir + strf("/frame_%05llu.png", (unsigned long long)n);
        if (!gfx::write_png(path, im)) std::fprintf(stderr, "qilin: can't write %s\n", path.c_str());
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
void block_on_resource(rt::Context& c) { ret(c, c.eax, 4); }

// BYTE* D3DVertexBuffer_Lock2(D3DVertexBuffer*, DWORD Flags): the data's
// address in the contiguous-memory window, as the original returns it. (The
// original also puts a wait for the GPU in the command buffer; see BlockOnResource.)
void vertex_buffer_lock2(rt::Context& c) { ret(c, ld(c, arg(c, 0) + 4) | rt::kRamCachedAlias, 8); }

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
    ret(c, c.eax, 8);
}

// VOID D3DDevice_BlockUntilIdle(): waits for the GPU.
void block_until_idle(rt::Context& c) {
    device("D3DDevice_BlockUntilIdle").renderer->flush();
    ret(c, c.eax, 0);
}

// HRESULT D3DDevice_Clear(DWORD Count, CONST D3DRECT*, DWORD Flags, D3DCOLOR,
//     float Z, DWORD Stencil): the render target and depth buffer, all of
//     them or the rectangles given (D3DRECT: x1, y1, x2, y2).
void clear(rt::Context& c) {
    Device& d = device("D3DDevice_Clear");
    const uint32_t count = arg(c, 0), rects = arg(c, 1), flags = arg(c, 2), color = arg(c, 3), stencil = arg(c, 5);
    const uint32_t zbits = arg(c, 4);
    float z;
    std::memcpy(&z, &zbits, 4);
    if ((flags & kClearTarget) && (flags & kClearTarget) != kClearTarget)
        unmodelled("D3DDevice_Clear", strf("clearing some colour channels only (flags %#x)", flags));
    if (flags & ~(kClearTarget | kClearZBuffer | kClearStencil)) unmodelled("D3DDevice_Clear", strf("flags %#x", flags));
    std::vector<gfx::Rect> list;
    for (uint32_t i = 0; rects && i < count; ++i) {
        const int32_t x1 = int32_t(ld(c, rects + 16 * i)), y1 = int32_t(ld(c, rects + 16 * i + 4));
        const int32_t x2 = int32_t(ld(c, rects + 16 * i + 8)), y2 = int32_t(ld(c, rects + 16 * i + 12));
        if (x2 > x1 && y2 > y1) list.push_back({x1, y1, uint32_t(x2 - x1), uint32_t(y2 - y1)});
    }
    if (rects && count && list.empty()) {  // only empty rectangles: nothing to do
        ret(c, 0, 24);
        return;
    }
    uint32_t f = 0;
    if (flags & kClearTarget) f |= gfx::kClearColor;
    if (flags & kClearZBuffer) f |= gfx::kClearDepth;
    if (flags & kClearStencil) f |= gfx::kClearStencil;
    const float rgba[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f, float(color & 0xFF) / 255.0f,
                           float(color >> 24) / 255.0f};
    d.renderer->clear(list.data(), uint32_t(list.size()), f, rgba, z, stencil & 0xFF);
    ret(c, 0, 24);
}

// DWORD D3DDevice_Swap(DWORD Flags): shows the frame at the vblank the
// presentation interval asks for (D3DSWAP_DEFAULT) and returns the swap count.
void swap(rt::Context& c) {
    Device& d = device("D3DDevice_Swap");
    const uint32_t flags = arg(c, 0);
    if (flags & ~4u) unmodelled("D3DDevice_Swap", strf("flags %#x", flags));  // D3DSWAP_FINISH (4) only waits
    const uint32_t old_back = d.frame_buffers[d.renderer->back_index()];
    d.renderer->present();
    ++d.swaps;
    // A render target that was the back buffer becomes the new one (the
    // device's internal reference moves with it).
    if (ld(c, kDeviceStorage + kDevRenderTarget) == old_back) {
        const uint32_t now = d.frame_buffers[d.renderer->back_index()];
        host::call_guest(c, 0x175970, {now});
        st(c, old_back + 8, ld(c, kDeviceStorage + kDevFence));
        host::call_guest(c, 0x175B90, {old_back});
        st(c, kDeviceStorage + kDevRenderTarget, now);
    }
    // Wait for the vblank the interval asks for, counted from the last swap's.
    const uint32_t target = std::max(d.vblanks + d.interval, vblanks_elapsed(d) + 1);
    std::this_thread::sleep_until(d.epoch + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(double(target) / kVblankHz)));
    mirror_device(c, d);
    run_vblanks(c, d, target, true);
    deliver_frame(d);
    if (platform::Window* w = d.config.window) {
        w->pump();
        if (w->quit_requested()) {
            std::fflush(stderr);
            std::printf("qilin_run: the window was closed after %u frames\n", d.swaps);
            std::fflush(stdout);
            std::exit(0);
        }
    }
    ret(c, d.swaps, 4);
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
};

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

Vertices vertex_arrays(rt::Context& c, const char* fn, uint32_t base, uint32_t first, uint32_t count) {
    if (ld(c, kDirtyFlags) & 0x40000000u) unmodelled(fn, "a draw from SetVertexShaderInput streams");
    Vertices v;
    v.count = count;
    const auto sl = slots(c, fn);
    std::vector<uint8_t> buf;
    for (uint32_t n = 0; n < 16; ++n) {
        if (!sl[n].bytes) continue;
        const uint32_t entry = 0x1842A0 + 12 * sl[n].stream;
        const uint32_t stride = ld(c, entry), vb = ld(c, entry + 8);
        if (!vb) unmodelled(fn, strf("vertex attribute %u reads stream %u, which has no buffer", n, sl[n].stream));
        const uint32_t at = ld(c, vb + 4) + ld(c, entry + 4) + sl[n].offset + stride * (base + first);
        v.mask |= 1u << n;
        v.attr[n].resize(size_t(count) * 4);
        buf.resize(sl[n].bytes);
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t b = 0; b < sl[n].bytes; ++b) buf[b] = rt::ld8(mem(c), at + stride * i + b, 0);
            convert_attribute(buf.data(), sl[n].format, &v.attr[n][size_t(i) * 4]);
        }
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
    Vertices v;
    v.count = uint32_t(dwords.size() * 4 / stride);
    const auto* bytes = reinterpret_cast<const uint8_t*>(dwords.data());
    for (uint32_t n = 0, off = 0; n < 16; ++n) {
        if (!sl[n].bytes) continue;
        v.mask |= 1u << n;
        v.attr[n].resize(size_t(v.count) * 4);
        for (uint32_t i = 0; i < v.count; ++i) convert_attribute(bytes + size_t(i) * stride + off, sl[n].format, &v.attr[n][size_t(i) * 4]);
        off += sl[n].bytes;
    }
    return v;
}

// NV2A primitives (D3DPRIMITIVETYPE has the same numbers) as Vulkan's:
// loops, fans, quads, quad strips and polygons become indexed lists.
gfx::Primitive convert_primitive(const char* fn, uint32_t prim, std::vector<uint32_t>& seq, bool& indexed) {
    std::vector<uint32_t> out;
    const size_t n = seq.size();
    switch (prim) {
        case 1: return gfx::Primitive::Points;
        case 2: return gfx::Primitive::Lines;
        case 4: return gfx::Primitive::LineStrip;
        case 5: return gfx::Primitive::Triangles;
        case 6: return gfx::Primitive::TriangleStrip;
        case 3:
            for (size_t i = 0; i < n && n >= 2; ++i) out.push_back(seq[i]), out.push_back(seq[(i + 1) % n]);
            seq = std::move(out);
            indexed = true;
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
    seq = std::move(out);
    indexed = true;
    return gfx::Primitive::Triangles;
}

uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull) {
    size_t i = 0;
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
const Device::CachedTexture* stage_texture(rt::Context& c, Device& d, const char* fn, uint32_t tex) {
    const uint32_t format = ld(c, tex + 12), size = ld(c, tex + 16), data = ld(c, tex + 4);
    gfx::TextureShape shape;
    std::string why;
    if (!gfx::texture_shape(format, size, &shape, &why)) unmodelled(fn, why);
    if (size_t(data) + shape.bytes > c.mem_size) unmodelled(fn, strf("a texture past the end of memory (%08X)", data));
    const uint64_t key = uint64_t(data) << 32 ^ uint64_t(format) * 0x9E3779B1u ^ size;
    Device::CachedTexture& e = d.textures[key];
    if (e.id && e.checked == d.swaps) return &e;
    const uint8_t* texels = c.mem + data;
    const uint64_t h = hash_bytes(texels, shape.bytes);
    if (!e.id || e.content != h) {
        if (e.id) d.renderer->destroy_texture(e.id);
        const gfx::TextureData t = gfx::decode_texture(shape, texels);
        e.id = d.renderer->create_texture(t);
        e.content = h;
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

// A draw, as the original's state flush leaves the GPU, into the renderer.
void issue_draw(rt::Context& c, Device& d, const char* fn, uint32_t prim, Vertices&& v, std::vector<uint32_t> seq, bool indexed) {
    st(c, kDeviceStorage + 8, ld(c, kDeviceStorage + 8) | 0x800u);
    if (!v.count) return;
    const uint32_t dev = kDeviceStorage;
    auto rs = [&](uint32_t i) { return ld(c, kRenderStateTable + 4 * i); };

    // Only the back buffer is drawn to yet.
    const uint32_t target = ld(c, dev + kDevRenderTarget);
    if (target != d.frame_buffers[d.renderer->back_index()]) return notice_once(d, "drawing into a render target other than the back buffer");

    gfx::DrawCmd cmd;
    // The vertex program at the selected slot of the GPU's program memory.
    const uint32_t vs_flags = ld(c, ld(c, dev + kDevVertexShader) + 4);
    if (!(vs_flags & 0x10)) unmodelled(fn, "a draw with the fixed-function vertex pipeline");
    const uint32_t start = ld(c, dev + kDevVertexShader + 8);
    if (start >= 136) unmodelled(fn, strf("a vertex program at slot %u", start));
    std::vector<uint32_t> words{0x2078u};
    for (uint32_t i = start; i < 136; ++i) {
        words.insert(words.end(), d.vs_program[i], d.vs_program[i] + 4);
        if (d.vs_program[i][3] & 1) break;  // FINAL
    }
    words[0] |= uint32_t((words.size() - 1) / 4) << 16;  // the instruction count, as the header has it
    uint64_t vs_key = hash_bytes(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4) ^ (uint64_t(v.mask) << 40) ^ 1;
    auto& vs_code = d.shaders[vs_key];
    if (vs_code.empty()) {
        std::string error;
        const auto prog = gfx::vsh::decode(words.data(), words.size(), &error);
        if (!prog) unmodelled(fn, "a vertex program that doesn't decode (" + error + ")");
        try {
            vs_code = gfx::nv2a::vertex_program(*prog, v.mask);
        } catch (const std::exception& e) {
            unmodelled(fn, e.what());
        }
    }

    // The register combiners as the render states hold them.
    if (!ld(c, dev + 0x780)) unmodelled(fn, "a draw with the fixed-function pixel pipeline (texture stage states)");
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

    std::vector<float> fsu(gfx::nv2a::kFsUniformVec4s * 4, 0.0f);
    for (uint32_t stage = 0; stage < 4; ++stage) {
        fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 0] = fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 1] = 1;
        fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 2] = 1;
        const uint32_t mode = cs.texture_modes >> (5 * stage) & 0x1F;
        const uint32_t tex = ld(c, dev + 0xF78 + 4 * stage);
        if (!tex || !(mode == 1 || mode == 2 || mode == 3)) continue;
        const Device::CachedTexture* t = stage_texture(c, d, fn, tex);
        cs.texture_dims[stage] = uint8_t(t->dims);
        cmd.textures[stage] = t->id;
        cmd.samplers[stage] = stage_sampler(c, fn, stage, *t, std::max(1u, ld(c, tex + 12) >> 16 & 0xF));
        if (t->linear) {  // the hardware addresses linear textures in texels
            fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 0] = 1.0f / float(t->width);
            fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 1] = 1.0f / float(t->height);
            fsu[(gfx::nv2a::kFsTexScale + stage) * 4 + 2] = 1.0f / float(t->depth);
        }
    }
    if (const char* why = gfx::nv2a::combiners_unsupported(cs)) unmodelled(fn, why);
    const uint64_t fs_key = cs.hash() ^ 2;
    auto& fs_code = d.shaders[fs_key];
    if (fs_code.empty()) fs_code = gfx::nv2a::combiners(cs);
    for (uint32_t i = 0; i < 8; ++i) {
        color_to_float(rs(kRsPsConstant0 + i), &fsu[(gfx::nv2a::kFsC0 + i) * 4]);
        color_to_float(rs(kRsPsConstant1 + i), &fsu[(gfx::nv2a::kFsC1 + i) * 4]);
    }
    color_to_float(rs(kRsPsFinalC0), &fsu[gfx::nv2a::kFsFinalC0 * 4]);
    color_to_float(rs(kRsPsFinalC1), &fsu[gfx::nv2a::kFsFinalC1 * 4]);
    color_to_float(rs(kRsFogColor), &fsu[gfx::nv2a::kFsFogColor * 4]);
    fsu[gfx::nv2a::kFsAlphaRef * 4] = float(rs(kRsAlphaRef) & 0xFF);

    // Vertex constants, with the viewport's and the renderer's own.
    std::vector<float> vsu(gfx::nv2a::kVsUniformVec4s * 4, 0.0f);
    std::memcpy(vsu.data(), d.vs_constants, sizeof d.vs_constants);
    viewport_constants(c, &vsu[58 * 4], &vsu[59 * 4]);  // c-38, c-37
    const float zscale = std::bit_cast<float>(ld(c, dev + 0x944));
    vsu[gfx::nv2a::kVsScreen * 4 + 0] = 2.0f / float(d.width);
    vsu[gfx::nv2a::kVsScreen * 4 + 1] = 2.0f / float(d.height);
    vsu[gfx::nv2a::kVsScreen * 4 + 2] = zscale > 0 ? 1.0f / zscale : 1.0f;

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
    // The original sends CULLMODE to the GPU as GL's front/back against
    // FRONTFACE, which comes to culling the winding CULLMODE names in GL's
    // terms (y up); the screen here runs y down, so that winding is the
    // other one on screen: D3DCULL_CW (0x900) culls what looks
    // counter-clockwise, the faces turned away in a D3D scene.
    cmd.cull = cull == 0x900 ? gfx::Cull::CounterClockwise : cull == 0x901 ? gfx::Cull::Clockwise : gfx::Cull::None;
    // The viewport's rectangle bounds what's drawn.
    cmd.scissor.x = int32_t(ld(c, dev + kDevViewport));
    cmd.scissor.y = int32_t(ld(c, dev + kDevViewport + 4));
    cmd.scissor.w = ld(c, dev + kDevViewport + 8);
    cmd.scissor.h = ld(c, dev + kDevViewport + 12);

    cmd.primitive = convert_primitive(fn, prim, seq, indexed);
    cmd.vs_key = vs_key, cmd.fs_key = fs_key, cmd.vs_code = &vs_code, cmd.fs_code = &fs_code;
    cmd.inputs = v.mask;
    cmd.vertex_count = v.count;
    for (uint32_t n = 0; n < 16; ++n)
        if (v.mask & (1u << n)) cmd.attributes[n] = v.attr[n].data();
    if (indexed) cmd.indices = seq.data(), cmd.index_count = uint32_t(seq.size());
    cmd.vs_uniforms = vsu.data();
    cmd.fs_uniforms = fsu.data();
    d.renderer->draw(cmd);
}

std::vector<uint32_t> identity_sequence(uint32_t n) {
    std::vector<uint32_t> s(n);
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

// VOID D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE, UINT VertexCount, CONST WORD* pIndexData):
// relative to the base vertex index SetIndices left at +0x1C.
void draw_indexed_vertices(rt::Context& c) {
    const char* fn = "D3DDevice_DrawIndexedVertices";
    Device& d = device(fn);
    const uint32_t primitive = arg(c, 0), count = arg(c, 1), index_data = arg(c, 2);
    const uint32_t base = ld(c, kDeviceStorage + 0x1C);
    consume_dirty_state(c, base);
    std::vector<uint32_t> indices(count);
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
    std::vector<uint32_t> inline_data;
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
constexpr uint32_t kDevTransforms = 0xC50;      // the device's matrices, 64 bytes each

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
constexpr uint32_t kTextureStateTable = 0x181970;
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
};

D3dConfig& config_storage() {
    static D3dConfig cfg;
    return cfg;
}

}  // namespace

const D3dConfig& d3d_config() { return config_storage(); }

void register_d3d(const D3dConfig& config) {
    config_storage() = config;
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
