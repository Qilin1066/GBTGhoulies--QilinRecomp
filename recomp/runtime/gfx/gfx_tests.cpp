// Renderer tests: clears and presents on whatever Vulkan device there is,
// checked by reading the frames back. Exits 77 (skipped) without Vulkan.
#include <cstdio>
#include <string>

#include "gfx.hpp"
#include "vsh.hpp"

using namespace qilin;

namespace {
int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

bool pixel_is(const gfx::Image& im, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* p = &im.rgba[(size_t(y) * im.width + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}
}  // namespace

int main(int argc, char** argv) {
    // Vertex programs, before the renderer (no GPU needed): one of the game's
    // shaders as the lifted XGAssembleShader assembles it. Its source:
    //   dp4 r0, v0, c5 / m4x4 oPos, v0, c0 / sge r1.w, r0, c10 / m4x4 oT0, v0, c6 / mul oD0, r1.w, c4
    // The assembler renames r1 (keeping it for the screen-space 1/w), narrows
    // masks and appends the screen-space transform with c-38 and c-37.
    {
        const uint32_t words[] = {
        0x000D2078u, 0x00000000u, 0x00ECA01Bu, 0x0836B800u, 0x28000000u,
        0x00000000u, 0x00EC001Bu, 0x08361800u, 0x20B08800u, 0x00000000u,
        0x00EC201Bu, 0x08363800u, 0x20B04800u, 0x00000000u, 0x00EC401Bu,
        0x08365800u, 0x20902800u, 0x00000000u, 0x00EC601Bu, 0x08367800u,
        0x20901800u, 0x00000000u, 0x018D4000u, 0x05FF5800u, 0x21B00000u,
        0x00000000u, 0x0647401Au, 0xC4355BFFu, 0x10B8E800u, 0x00000000u,
        0x00ECC01Bu, 0x0836D800u, 0x20B08848u, 0x00000000u, 0x00ECE01Bu,
        0x0836F800u, 0x20B04848u, 0x00000000u, 0x00ED001Bu, 0x08371800u,
        0x20A02848u, 0x00000000u, 0x00ED201Bu, 0x08373800u, 0x20A01848u,
        0x00000000u, 0x004C80FFu, 0xB4369800u, 0x20A0F818u, 0x00000000u,
        0x0087601Au, 0xC400286Au, 0xF0B0E801u};
        std::string error;
        const auto p = gfx::vsh::decode(words, sizeof words / 4, &error);
        CHECK(p.has_value() && p->code.size() == 13 && p->code.back().final);
        if (p) {
            const std::string text = gfx::vsh::disassemble(*p);
            CHECK(text == R"(dp4 r0.x, v0, c5
dp4 oPos.x, v0, c0
dp4 oPos.y, v0, c1
dp4 oPos.z, v0, c2
dp4 oPos.w, v0, c3
sge r11.w, r0.x, c10.w
mul oPos.xyz, oPos.xyzz, c-38.xyzz
  + rcc r1.x, oPos.w
dp4 oT0.x, v0, c6
dp4 oT0.y, v0, c7
dp4 oT0.z, v0, c8
dp4 oT0.w, v0, c9
mul oD0, r11.w, c4
mad oPos.xyz, oPos.xyzz, r1.x, c-37.xyzz
)");
            if (text != R"(dp4 r0.x, v0, c5
dp4 oPos.x, v0, c0
dp4 oPos.y, v0, c1
dp4 oPos.z, v0, c2
dp4 oPos.w, v0, c3
sge r11.w, r0.x, c10.w
mul oPos.xyz, oPos.xyzz, c-38.xyzz
  + rcc r1.x, oPos.w
dp4 oT0.x, v0, c6
dp4 oT0.y, v0, c7
dp4 oT0.z, v0, c8
dp4 oT0.w, v0, c9
mul oD0, r11.w, c4
mad oPos.xyz, oPos.xyzz, r1.x, c-37.xyzz
)") std::fprintf(stderr, "%s", text.c_str());
            CHECK(p->code[6].ilu == gfx::vsh::Ilu::Rcc && p->code[6].ilu_temp() == 1);  // the ILU writes r1 beside a MAC
        }
        const uint32_t not_vsh[] = {0x12345678u};
        CHECK(!gfx::vsh::decode(not_vsh, 1, &error).has_value());
    }

    gfx::Config cfg;
    cfg.width = 64;
    cfg.height = 48;
    cfg.validation = true;  // if the layers are installed
    std::string error;
    auto r = gfx::Renderer::create(cfg, &error);
    if (!r) {
        if (failures) {  // the GPU-free checks above still count
            std::fprintf(stderr, "%d check(s) failed\n", failures);
            return 1;
        }
        std::printf("renderer tests skipped: %s\n", error.c_str());
        return 77;
    }
    std::printf("device: %s\n", r->device_name().c_str());

    // Buffers start black.
    CHECK(pixel_is(r->read_back_buffer(), 0, 0, 0, 0, 0, 0));

    // A whole-buffer clear, then a rectangle (clipped at the edge) over it.
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    r->clear(nullptr, 0, gfx::kClearColor | gfx::kClearDepth | gfx::kClearStencil, blue, 1.0f, 0);
    const gfx::Rect rect{48, 40, 100, 100};
    r->clear(&rect, 1, gfx::kClearColor, red, 1.0f, 0);
    gfx::Image back = r->read_back_buffer();
    CHECK(pixel_is(back, 0, 0, 0, 0, 255, 255));
    CHECK(pixel_is(back, 47, 39, 0, 0, 255, 255));
    CHECK(pixel_is(back, 48, 40, 255, 0, 0, 255));
    CHECK(pixel_is(back, 63, 47, 255, 0, 0, 255));

    // Present flips: the frame becomes the front buffer; the next back buffer is the other one.
    CHECK(r->back_index() == 0 && r->front_index() == 1);
    r->present();
    CHECK(r->frames_presented() == 1 && r->back_index() == 1 && r->front_index() == 0);
    gfx::Image front = r->read_front_buffer();
    CHECK(pixel_is(front, 0, 0, 0, 0, 255, 255) && pixel_is(front, 50, 45, 255, 0, 0, 255));
    CHECK(pixel_is(r->read_back_buffer(), 0, 0, 0, 0, 0, 0));
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1};
    r->clear(nullptr, 0, gfx::kClearColor, grey, 0, 0);
    r->present();
    front = r->read_front_buffer();
    CHECK(pixel_is(front, 10, 10, 128, 128, 128, 255) || pixel_is(front, 10, 10, 127, 127, 127, 255));

    if (argc > 1) CHECK(gfx::write_png(argv[1], front));
    CHECK(r->validation_errors() == 0);
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all renderer tests passed\n");
    return failures != 0;
}
