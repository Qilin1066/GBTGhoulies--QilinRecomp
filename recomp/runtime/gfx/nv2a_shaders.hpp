// The NV2A's programmable stages as SPIR-V: vertex programs (decoded by
// vsh.*) and the register combiners (a D3DPIXELSHADERDEF's settings, as
// D3D__RenderState holds them). docs/graphics.md, "Drawing".
#pragma once

#include <cstdint>
#include <vector>

#include "vsh.hpp"

namespace qilin::gfx::nv2a {

// Varyings, vertex to fragment: locations.
enum Varying : uint32_t { kVaryD0 = 0, kVaryD1 = 1, kVaryFog = 2, kVaryT0 = 3, kVaryB0 = 7, kVaryB1 = 8 };

// Vertex uniforms (set 0, binding 0): the 192 program constants, then the
// renderer's own: [192] = (2 / width, 2 / height, 1 / depth scale, 0), the
// screen-space to clip-space factors.
// kVsPixels: x, the render resolution's scale (points are drawn that many times larger);
// y and z, a squeeze across: screen x becomes z + (x - z) * y (the HUD at wide shapes; 1 and 0 otherwise).
inline constexpr uint32_t kVsConstants = 192, kVsScreen = 192, kVsPixels = 193, kVsUniformVec4s = 194;

// A vertex program: `inputs` has bit n set for each attribute v[n] the draw
// supplies (location n, vec4); the others read (0, 0, 0, 1).
std::vector<uint32_t> vertex_program(const vsh::Program& program, uint32_t inputs);

// Fragment uniforms (set 0, binding 1).
enum FsUniform : uint32_t {
    kFsC0 = 0,         // [8] each combiner stage's C0 (stage 0's when not unique)
    kFsC1 = 8,         // [8] C1
    kFsFinalC0 = 16,
    kFsFinalC1 = 17,
    kFsFogColor = 18,  // rgb
    kFsAlphaRef = 19,  // x: the alpha test's reference, 0..1
    kFsTexScale = 20,  // [4] per stage: xy multiply the coordinates (texel units for linear textures, else 1)
    kFsFogParams = 24, // xy: the fog unit's two parameters (fog_mode)
    kFsBumpMat = 25,   // [4] per stage: the bump-environment matrix's four entries, in the GPU's order
    kFsBumpLum = 29,   // [4] per stage: x the luminance scale, y its offset (BUMPENVMAP_LUM)
    kFsUniformVec4s = 33,
};

// What decides a fragment shader (the constants are uniforms, so they don't).
struct CombinerSetup {
    uint32_t alpha_inputs[8], alpha_outputs[8], rgb_inputs[8], rgb_outputs[8];
    uint32_t final_abcd = 0, final_efgh = 0;
    uint32_t combiner_count = 0;  // PSCombinerCount: count and flags
    uint32_t texture_modes = 0;   // PSTextureModes: 5 bits a stage
    uint32_t input_texture = 0, dot_mapping = 0, compare_mode = 0;
    uint32_t alpha_func = 0;      // 0 none (alpha test off), else D3DCMP_* (0x200 + n)
    // The fog unit: 0 off (the fog register's alpha is the interpolated fog
    // value), else the NV2A's mode: 0x2601 linear, 0x800 exp, 0x801 exp2,
    // computed per pixel from the fog coordinate with kFsFogParams.
    uint32_t fog_mode = 0;
    uint8_t texture_dims[4] = {}; // per stage: 0 none bound, 2 2D, 3 3D, 6 cube
    // Point sprites (D3DRS_POINTSPRITEENABLE on a point list): stages 0-2
    // take the position within the point, (0, 0) top left to (1, 1), for
    // their texture coordinates; stage 3 keeps its own.
    uint32_t point_sprite = 0;
    // D3DTSS_ALPHAKILL per stage (bit n for stage n): pixels whose texel there has alpha 0 are discarded.
    uint32_t alpha_kill = 0;

    bool operator==(const CombinerSetup&) const = default;
    uint64_t hash() const;
};

// The register combiners, the final combiner and the alpha test, with the
// textures the setup's texture modes sample (set 0, bindings 2-5).
std::vector<uint32_t> combiners(const CombinerSetup& s);

// Unsupported settings, named, for the caller to report; empty if supported.
const char* combiners_unsupported(const CombinerSetup& s);

}  // namespace qilin::gfx::nv2a
