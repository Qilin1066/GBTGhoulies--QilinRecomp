// Exercise the host Direct3D device lifetime with real Vulkan, synthetic guest
// RAM and three no-op lifted startup helpers. No disc, drawing or game code.
#include "host.hpp"
#include "gfx.hpp"
#include "path.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
namespace {
void fvf(qilin::rt::Context& c) { qilin::host::ret(c, 0, 4); }
void targets(qilin::rt::Context& c) { qilin::host::ret(c, 0, 8); }
void defaults(qilin::rt::Context& c) { qilin::host::ret(c, 0, 0); }
}
int main() {
    using namespace qilin;
    gfx::Config probe;
    probe.width = 64; probe.height = 48; probe.validation = true;
    std::string error;
    auto renderer = gfx::Renderer::create(probe, &error);
    if (!renderer) {
        std::printf("Direct3D lifecycle test skipped: %s\n", error.c_str());
        const char* required = std::getenv("QILIN_REQUIRE_VULKAN");
        return required && std::string(required) == "1" ? 1 : 77;
    }
    renderer.reset();
    auto ram = std::make_unique<uint8_t[]>(16 * 1024 * 1024);
    rt::Context context{};
    context.mem = ram.get(); context.mem_size = 16 * 1024 * 1024; context.esp = 0x10000;
    rt::g_ram_aliases = false;
    host::vm().init(ram.get(), 0x600000, 0xe00000);
    host::virtual_memory().init(nullptr, 0, 0);
    host::set_memory_map({16 * 1024 * 1024});
    const auto dir = std::filesystem::temp_directory_path() / "qilin-d3d-lifetime";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    host::D3dConfig config;
    config.pipeline_cache = utf8_path(dir / "cache.bin");
    host::register_d3d(config);
    host::register_hle(0x1739f0, "fixture FVF initialisation", fvf);
    host::register_hle(0x174170, "fixture target initialisation", targets);
    host::register_hle(0x179fe0, "fixture state initialisation", defaults);
    const auto* create = host::xdk_symbol("Direct3D_CreateDevice");
    if (!create) return 1;
    std::array<uint32_t, 17> presentation{};
    presentation[0] = 64; presentation[1] = 48;
    presentation[2] = 0x12; presentation[3] = 1; // linear A8R8G8B8
    presentation[12] = 1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::memcpy(ram.get() + 0x2000, presentation.data(), sizeof(presentation));
        host::call_guest(context, create->address, {0, 0, 0, 0, 0x2000, 0x3000});
        if (context.eax) return 1;
        // Exercise the native viewport setter against a synthetic surface.
        // Origins outside a temporary target and overflowing extents must
        // yield an empty/clipped viewport, never enormous unsigned sizes.
        const auto* viewport = host::xdk_symbol("D3DDevice_SetViewport");
        if (!viewport) return 1;
        const auto read32 = [&](uint32_t a) { uint32_t v; std::memcpy(&v, ram.get() + a, 4); return v; };
        const auto write32 = [&](uint32_t a, uint32_t v) { std::memcpy(ram.get() + a, &v, 4); };
        constexpr uint32_t dev = 0x181E10, surface = 0x4000, vp = 0x5000;
        const uint32_t saved = read32(dev + 0x19F4);
        write32(surface + 12, 0x12u << 8);
        write32(surface + 16, 63u | 47u << 12);
        write32(dev + 0x19F4, surface);
        struct ViewCase { uint32_t x, y, w, h, cx, cy, cw, ch; };
        for (const auto& v : {ViewCase{0, 0, 64, 48, 0, 0, 64, 48},
                              {60, 45, 20, 20, 60, 45, 4, 3},
                              {100, 100, 8, 8, 64, 48, 0, 0},
                              {1, 1, UINT32_MAX, UINT32_MAX, 1, 1, 63, 47},
                              {UINT32_MAX, UINT32_MAX, 2, 2, 64, 48, 0, 0},
                              {32, 24, 0, 0, 32, 24, 0, 0}}) {
            const std::array<uint32_t, 6> values{v.x, v.y, v.w, v.h, 0, 0x3F800000};
            std::memcpy(ram.get() + vp, values.data(), sizeof(values));
            host::call_guest(context, viewport->address, {vp});
            if (read32(dev + 0xED0) != v.cx || read32(dev + 0xED4) != v.cy ||
                read32(dev + 0xED8) != v.cw || read32(dev + 0xEDC) != v.ch ||
                read32(dev + 0xEE0) != 0 || read32(dev + 0xEE4) != 0x3F800000) return 1;
        }
        write32(dev + 0x19F4, saved);
        host::shutdown_d3d();
        host::shutdown_d3d(); // idempotent; a subsequent device can be created
        // Cache streams are closed by renderer teardown, not left alive until
        // the OS kills the process (rename would fail for the open file on Windows).
        const auto library = dir / "pipeline_library.bin";
        if (!std::filesystem::exists(library)) return 1;
        const auto moved = dir / "closed.bin";
        std::filesystem::rename(library, moved);
        std::filesystem::remove(moved);
    }
    std::filesystem::remove_all(dir);
    host::vm().init(nullptr, 0, 0);
    std::puts("Direct3D device teardown, cache closure and repeated shutdown passed");
    return 0;
}
