// Renderer tests: clears and presents on whatever Vulkan device there is,
// checked by reading the frames back. Exits 77 (skipped) without Vulkan.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gfx.hpp"
#include "nv2a_shaders.hpp"
#include "texture.hpp"
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
            // The program as SPIR-V; QILIN_SPV_DIR writes it out (for spirv-val).
            const auto spv = gfx::nv2a::vertex_program(*p, 1);
            CHECK(spv.size() > 5 && spv[0] == 0x07230203u);
            if (const char* dir = std::getenv("QILIN_SPV_DIR"))
                if (std::FILE* f = std::fopen((std::string(dir) + "/test_vs.spv").c_str(), "wb")) {
                    std::fwrite(spv.data(), 4, spv.size(), f);
                    std::fclose(f);
                }
        }
        const uint32_t not_vsh[] = {0x12345678u};
        CHECK(!gfx::vsh::decode(not_vsh, 1, &error).has_value());
    }

    // A combiner setup (one stage: r0 = t0 * v0, final r0 with alpha test), as SPIR-V.
    {
        gfx::nv2a::CombinerSetup cs{};
        cs.combiner_count = 1;
        cs.rgb_inputs[0] = 0x08040000u;    // A = t0, B = v0, C = D = 0
        cs.alpha_inputs[0] = 0x18140000u;  // t0.a * v0.a
        cs.rgb_outputs[0] = 0x00C00u;      // sum to r0
        cs.alpha_outputs[0] = 0x00C00u;
        cs.final_abcd = 0x0000000Cu;       // D = r0
        cs.final_efgh = 0x00001C00u;       // G = r0.a
        cs.texture_modes = 1;              // stage 0 PROJECT2D
        cs.texture_dims[0] = 2;
        cs.alpha_func = 0x204;
        CHECK(gfx::nv2a::combiners_unsupported(cs) == nullptr);
        const auto spv = gfx::nv2a::combiners(cs);
        CHECK(spv.size() > 5 && spv[0] == 0x07230203u);
        if (const char* dir = std::getenv("QILIN_SPV_DIR"))
            if (std::FILE* f = std::fopen((std::string(dir) + "/test_ps.spv").c_str(), "wb")) {
                std::fwrite(spv.data(), 4, spv.size(), f);
                std::fclose(f);
            }
    }
    // Swizzling: a 4x4 texture interleaves x and y bits, x first.
    CHECK(gfx::swizzle_offset(1, 0, 0, 4, 4, 1) == 1 && gfx::swizzle_offset(0, 1, 0, 4, 4, 1) == 2 &&
          gfx::swizzle_offset(3, 3, 0, 4, 4, 1) == 15 && gfx::swizzle_offset(0, 1, 0, 8, 2, 1) == 2 &&
          gfx::swizzle_offset(4, 0, 0, 8, 2, 1) == 8);

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
    // A draw: a vertex program (oPos = v0, oD0 = v3) and the final combiner
    // passing v0 through, a triangle over the top-left of the buffer.
    {
        gfx::vsh::Program prog;
        gfx::vsh::Instruction mov_pos;
        mov_pos.mac = gfx::vsh::Mac::Mov;
        mov_pos.a.kind = gfx::vsh::Src::Input;
        mov_pos.a.index = 0;
        mov_pos.out_mask = 0xF;
        mov_pos.out_address = gfx::vsh::kPos;
        gfx::vsh::Instruction mov_d0 = mov_pos;
        mov_d0.a.index = 3;
        mov_d0.out_address = gfx::vsh::kD0;
        mov_d0.final = true;
        prog.code = {mov_pos, mov_d0};
        const auto vs = gfx::nv2a::vertex_program(prog, 0b1001);
        gfx::nv2a::CombinerSetup cs{};
        cs.final_abcd = 0x00000004u;  // D = v0
        cs.final_efgh = 0x00001400u;  // G = v0.a
        const auto fs = gfx::nv2a::combiners(cs);
        const float pos[3][4] = {{0, 0, 0.5f, 1}, {40, 0, 0.5f, 1}, {0, 40, 0.5f, 1}};
        const float col[3][4] = {{0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}};
        std::vector<float> vsu(gfx::nv2a::kVsUniformVec4s * 4, 0.0f), fsu(gfx::nv2a::kFsUniformVec4s * 4, 0.0f);
        vsu[gfx::nv2a::kVsScreen * 4 + 0] = 2.0f / float(cfg.width);
        vsu[gfx::nv2a::kVsScreen * 4 + 1] = 2.0f / float(cfg.height);
        vsu[gfx::nv2a::kVsScreen * 4 + 2] = 1.0f;
        gfx::DrawCmd d;
        d.vs_key = 1, d.fs_key = 2, d.vs_code = &vs, d.fs_code = &fs;
        d.inputs = 0b1001;
        d.vertex_count = 3;
        d.attributes[0] = &pos[0][0];
        d.attributes[3] = &col[0][0];
        d.vs_uniforms = vsu.data();
        d.fs_uniforms = fsu.data();
        d.scissor = {0, 0, cfg.width, cfg.height};
        const float black[4] = {0, 0, 0, 1};
        r->clear(nullptr, 0, gfx::kClearColor, black, 1, 0);
        r->draw(d);
        const gfx::Image im = r->read_back_buffer();
        CHECK(pixel_is(im, 5, 5, 0, 255, 0, 255));      // inside the triangle
        CHECK(pixel_is(im, 60, 40, 0, 0, 0, 255));      // outside
        // A texture: a 2x2 checker sampled by PASSTHRU-free PROJECT2D at texel centres.
        gfx::TextureData t;
        t.width = t.height = 2;
        t.rgba = {255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 255};
        const uint32_t tex = r->create_texture(t);
        CHECK(tex != 0);
        r->destroy_texture(tex);
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all renderer tests passed\n");
    return failures != 0;
}
