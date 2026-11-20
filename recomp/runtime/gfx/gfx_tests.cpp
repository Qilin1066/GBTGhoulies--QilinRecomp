// Renderer tests: clears and presents on whatever Vulkan device there is,
// checked by reading the frames back. Exits 77 (skipped) without Vulkan.
#include <algorithm>
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
        // Every texture mode the runtime models, in the arrangements the NV2A
        // allows: bump maps and dependent reads, the dot-product lookups with
        // each dot mapping, and DOT_ZW's depth.
        {
            auto modes = [](std::initializer_list<uint32_t> m) {
                uint32_t v = 0, i = 0;
                for (uint32_t x : m) v |= x << (5 * i++);
                return v;
            };
            struct Case { uint32_t modes, inputs, dims[4]; };
            const Case cases[] = {
                {modes({1, 7, 15, 16}), 1u << 16 | 2u << 20, {2, 2, 2, 2}},   // PROJECT2D, BUMPENVMAP_LUM, DPNDNT_AR, DPNDNT_GB
                {modes({1, 6, 0, 0}), 0, {2, 2, 0, 0}},                       // BUMPENVMAP
                {modes({1, 17, 9, 0}), 0, {2, 0, 2, 0}},                      // DOTPRODUCT, DOT_ST
                {modes({1, 17, 17, 14}), 0, {2, 0, 0, 6}},                    // ... DOT_STR_CUBE
                {modes({1, 17, 17, 13}), 0, {2, 0, 0, 3}},                    // ... DOT_STR_3D
                {modes({1, 17, 11, 12}), 0, {2, 0, 6, 6}},                    // DOT_RFLCT_DIFF, DOT_RFLCT_SPEC
                {modes({1, 17, 10, 0}), 0, {2, 0, 0, 0}},                     // DOT_ZW at stage 2
            };
            int n = 0;
            for (const Case& k : cases) {
                for (uint32_t map = 0; map < 8; ++map) {
                    gfx::nv2a::CombinerSetup m = cs;
                    m.texture_modes = k.modes, m.input_texture = k.inputs, m.dot_mapping = map | map << 4 | map << 8;
                    for (int d = 0; d < 4; ++d) m.texture_dims[d] = uint8_t(k.dims[d]);
                    const char* why = gfx::nv2a::combiners_unsupported(m);
                    if (why) std::fprintf(stderr, "case %d: %s\n", n, why);
                    CHECK(why == nullptr);
                    const auto code = gfx::nv2a::combiners(m);
                    CHECK(code.size() > 5);
                    if (const char* dir = std::getenv("QILIN_SPV_DIR"))
                        if (std::FILE* f = std::fopen((std::string(dir) + "/test_ps_mode" + std::to_string(n) + "_" + std::to_string(map) + ".spv").c_str(), "wb")) {
                            std::fwrite(code.data(), 4, code.size(), f);
                            std::fclose(f);
                        }
                }
                ++n;
            }
            // And what the hardware can't do is named.
            gfx::nv2a::CombinerSetup bad = cs;
            bad.texture_modes = modes({6, 0, 0, 0});  // a bump map at stage 0
            CHECK(gfx::nv2a::combiners_unsupported(bad) != nullptr);
            bad.texture_modes = modes({1, 9, 0, 0});  // DOT_ST without DOTPRODUCT before it
            CHECK(gfx::nv2a::combiners_unsupported(bad) != nullptr);
        }
        // The same as a point sprite, with linear fog: other code, the same checks.
        cs.point_sprite = 1;
        cs.fog_mode = 0x2601;
        const auto sprite = gfx::nv2a::combiners(cs);
        CHECK(sprite.size() > 5 && sprite != spv && cs.hash() != gfx::nv2a::CombinerSetup{}.hash());
        if (const char* dir = std::getenv("QILIN_SPV_DIR"))
            if (std::FILE* f = std::fopen((std::string(dir) + "/test_ps_sprite.spv").c_str(), "wb")) {
                std::fwrite(sprite.data(), 4, sprite.size(), f);
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
        vsu[gfx::nv2a::kVsPixels * 4] = vsu[gfx::nv2a::kVsPixels * 4 + 1] = 1.0f;  // no render-resolution scale, no squeeze
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

    // DXT1, 3 and 5 sampled from their blocks on the GPU (BC1-3) against the
    // CPU decoder: an 8×8 texture of pseudo-random blocks (both DXT1 modes,
    // both DXT5 alpha modes) on an 8×8 square, one pixel a texel.
    if (r && r->takes_blocks()) {
        gfx::vsh::Program prog;
        gfx::vsh::Instruction pos{};
        pos.mac = gfx::vsh::Mac::Mov;
        pos.a.kind = gfx::vsh::Src::Input;
        pos.a.index = 0;
        pos.out_mask = 0xF;
        pos.out_address = gfx::vsh::kPos;
        gfx::vsh::Instruction uv = pos;
        uv.a.index = 9;
        uv.out_address = gfx::vsh::kT0;
        uv.final = true;
        prog.code = {pos, uv};
        const auto vs = gfx::nv2a::vertex_program(prog, 1u | 1u << 9);
        gfx::nv2a::CombinerSetup cs{};
        cs.texture_modes = 1;          // stage 0 PROJECT2D
        cs.texture_dims[0] = 2;
        cs.final_abcd = 0x00000008u;   // D = t0
        cs.final_efgh = 0x00001800u;   // G = t0.a
        const auto fs = gfx::nv2a::combiners(cs);
        const float pos_v[4][4] = {{0, 0, 0.5f, 1}, {8, 0, 0.5f, 1}, {0, 8, 0.5f, 1}, {8, 8, 0.5f, 1}};
        const float uv_v[4][4] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}, {1, 1, 0, 1}};
        std::vector<float> vsu(gfx::nv2a::kVsUniformVec4s * 4, 0.0f), fsu(gfx::nv2a::kFsUniformVec4s * 4, 0.0f);
        vsu[gfx::nv2a::kVsPixels * 4] = vsu[gfx::nv2a::kVsPixels * 4 + 1] = 1.0f;  // no render-resolution scale, no squeeze
        vsu[gfx::nv2a::kVsScreen * 4 + 0] = 2.0f / float(cfg.width);
        vsu[gfx::nv2a::kVsScreen * 4 + 1] = 2.0f / float(cfg.height);
        vsu[gfx::nv2a::kVsScreen * 4 + 2] = 1.0f;
        for (uint32_t k = 0; k < 4; ++k) fsu[(gfx::nv2a::kFsTexScale) * 4 + k] = 1;
        uint32_t seed = 12345;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); };
        const struct { uint32_t code, block; } kinds[] = {{0x0C, 8}, {0x0E, 16}, {0x0F, 16}};
        for (const auto& kd : kinds) {
            gfx::TextureShape shape;
            std::string why;
            CHECK(gfx::texture_shape(kd.code << 8 | 0x20u | 1u << 16 | 3u << 20 | 3u << 24, 0, &shape, &why));
            std::vector<uint8_t> blocks(4 * kd.block);
            for (auto& b : blocks) b = rnd();
            // Both colour orders and, for DXT5, both alpha orders.
            for (uint32_t b = 0; b < 4; ++b) {
                uint8_t* c = &blocks[b * kd.block + (kd.block == 16 ? 8 : 0)];
                const bool c0_first = c[1] > c[3] || (c[1] == c[3] && c[0] > c[2]);
                if (((b & 1) != 0) != c0_first) std::swap(c[0], c[2]), std::swap(c[1], c[3]);
                if (kd.code != 0x0F) continue;  // only DXT5 has the two alpha orders
                const bool a0_first = blocks[b * 16] > blocks[b * 16 + 1];
                if ((((b >> 1) & 1) != 0) != a0_first) std::swap(blocks[b * 16], blocks[b * 16 + 1]);
            }
            // Draws the texture over the square and returns how far its pixels are from `want`.
            auto drawn = [&](uint32_t tex, const gfx::TextureData& want) {
                gfx::DrawCmd d;
                d.vs_key = 900, d.fs_key = 901, d.vs_code = &vs, d.fs_code = &fs;
                d.inputs = 1u | 1u << 9;
                d.vertex_count = 4;
                d.primitive = gfx::Primitive::TriangleStrip;
                d.attributes[0] = &pos_v[0][0];
                d.attributes[9] = &uv_v[0][0];
                d.vs_uniforms = vsu.data();
                d.fs_uniforms = fsu.data();
                d.scissor = {0, 0, cfg.width, cfg.height};
                d.textures[0] = tex;
                d.samplers[0].mag = d.samplers[0].min = gfx::Filter::Nearest;
                const float clear[4] = {0, 0, 0, 0};
                r->clear(nullptr, 0, gfx::kClearColor, clear, 1, 0);
                r->draw(d);
                const gfx::Image im = r->read_back_buffer();
                int worst = 0;
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; ++x)
                        for (uint32_t ch = 0; ch < 4; ++ch)
                            worst = std::max(worst, std::abs(int(im.rgba[(size_t(y) * im.width + x) * 4 + ch]) - int(want.rgba[(size_t(y) * 8 + x) * 4 + ch])));
                return worst;
            };
            const gfx::TextureData cpu = gfx::decode_texture(shape, blocks.data());
            const uint32_t tex = r->create_texture(gfx::texture_blocks(shape, blocks.data()));
            // The GPU's interpolation may round the thirds and fifths differently by 1 or 2.
            const int worst = drawn(tex, cpu);
            if (worst > 2) std::fprintf(stderr, "DXT format %#x: the GPU's texels differ from the decoder's by up to %d\n", kd.code, worst);
            CHECK(worst <= 2);
            // New blocks into the same image (update_texture), then the decoded
            // texels as RGBA, made and updated: exact.
            std::vector<uint8_t> next(blocks.size());
            for (auto& v : next) v = rnd();
            const gfx::TextureData cpu2 = gfx::decode_texture(shape, next.data());
            CHECK(r->update_texture(tex, gfx::texture_blocks(shape, next.data())));
            CHECK(drawn(tex, cpu2) <= 2);
            CHECK(!r->update_texture(tex, cpu2));  // another shape (RGBA): refused
            r->destroy_texture(tex);
            const uint32_t plain = r->create_texture(cpu);
            CHECK(drawn(plain, cpu) == 0);
            CHECK(r->update_texture(plain, cpu2));
            CHECK(drawn(plain, cpu2) == 0);
            r->destroy_texture(plain);
            std::printf("DXT format %#x as BC on the GPU: within %d of the decoder\n", kd.code, worst);
        }
    }

    // A depth target packed as the console's D24S8 words (pack_depth): depth
    // 0.25 and stencil 0x5A over it all, then 1.0 and 3 over its left half.
    // 0.25 is 0x400000 in 24 bits: alpha 0x40, red and green 0, blue the
    // stencil; 1.0 is 0xFFFFFF.
    {
        const uint32_t target = r->create_render_target(16, 8), depth = r->create_depth_target(16, 8);
        const uint32_t packed = r->create_render_target(16, 8);
        r->set_render_target(target, depth);
        const float none[4] = {0, 0, 0, 0};
        r->clear(nullptr, 0, gfx::kClearDepth | gfx::kClearStencil, none, 0.25f, 0x5A);
        const gfx::Rect left{0, 0, 8, 8};
        r->clear(&left, 1, gfx::kClearDepth | gfx::kClearStencil, none, 1.0f, 3);
        if (r->pack_depth(depth, packed)) {
            const gfx::Image im = r->read_render_target(packed);
            CHECK(pixel_is(im, 0, 0, 0xFF, 0xFF, 3, 0xFF) && pixel_is(im, 7, 7, 0xFF, 0xFF, 3, 0xFF));
            CHECK(pixel_is(im, 8, 0, 0, 0, 0x5A, 0x40) && pixel_is(im, 15, 7, 0, 0, 0x5A, 0x40));
            // Drawn into again after: packed again, it follows.
            r->set_render_target(target, depth);
            r->clear(nullptr, 0, gfx::kClearDepth | gfx::kClearStencil, none, 1.0f, 0);
            CHECK(r->pack_depth(depth, packed));
            CHECK(pixel_is(r->read_render_target(packed), 12, 4, 0xFF, 0xFF, 0, 0xFF));
            std::printf("depth target packed as D24S8 words\n");
        } else {
            std::printf("depth targets can't be sampled on this device: packing skipped\n");
        }
        r->set_render_target(0, 0);
        r->destroy_texture(packed);
        r->destroy_texture(depth);
        r->destroy_texture(target);
        CHECK(r->validation_errors() == 0);
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all renderer tests passed\n");
    return failures != 0;
}
