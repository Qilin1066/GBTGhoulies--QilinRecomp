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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gfx.hpp"
#include "vsh.hpp"
#include "host.hpp"
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
    st(c, kDeviceStorage + kDevRenderTarget, d.frame_buffers[back]);
    st(c, kDeviceStorage + kDevDepth, d.depth);
    st(c, kDeviceStorage + kDevMultisample, kMultisampleNone);
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
        for (size_t i = 0; i < im.rgba.size(); i += 4)  // the display's gamma ramp, as the TV gets it
            for (int k = 0; k < 3; ++k) im.rgba[i + k] = d.gamma[k][im.rgba[i + k]];
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
// (internal references) stays. Freeing one isn't modelled yet.
void resource_release(rt::Context& c) {
    const uint32_t r = arg(c, 0), common = ld(c, r);
    if ((common & kRefMask) == 0) unmodelled("D3DResource_Release", strf("releasing %08X, which has no references", r));
    if ((common & kRefMask) == 1) {
        if ((common & kTypeMask) == kTypeSurface && ld(c, r + 20)) unmodelled("D3DResource_Release", "a surface of a texture");
        if (!(common & kIntRefMask)) unmodelled("D3DResource_Release", strf("freeing resource %08X", r));
    }
    st(c, r, common - 1);
    ret(c, (common - 1) & kRefMask, 4);
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
    d.renderer->present();
    ++d.swaps;
    // Wait for the vblank the interval asks for, counted from the last swap's.
    const uint32_t target = std::max(d.vblanks + d.interval, vblanks_elapsed(d) + 1);
    std::this_thread::sleep_until(d.epoch + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(double(target) / kVblankHz)));
    mirror_device(c, d);
    run_vblanks(c, d, target, true);
    deliver_frame(d);
    ret(c, d.swaps, 4);
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
    Device& d = device("D3DDevice_CreateVertexShader");
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
    const uint32_t obj = guest_alloc(d, 16);
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
}

}  // namespace qilin::host
