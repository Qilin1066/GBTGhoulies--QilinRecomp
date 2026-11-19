// The NV2A's register combiners to SPIR-V: the texture stages (by texture
// mode), up to eight general combiner stages, the final combiner and the
// alpha test. The register file and the input and output mappings are
// the hardware's, as D3D's D3DPIXELSHADERDEF encodes them (PS_* in the
// XDK's d3d8types.h). Values are floats, not the hardware's 9-bit signed
// fixed point: results agree to within its precision.
#include <cstring>
#include <stdexcept>
#include <string>

#include "nv2a_shaders.hpp"
#include "spirv.hpp"

namespace qilin::gfx::nv2a {

namespace {

using spv::Builder;
using spv::Id;
namespace op = spv::op;
namespace glsl = spv::glsl;

enum Reg : uint32_t { kZero = 0, kC0 = 1, kC1 = 2, kFog = 3, kV0 = 4, kV1 = 5, kT0 = 8, kR0 = 12, kR1 = 13, kV1R0Sum = 14, kEfProd = 15 };

// Texture modes (PS_TEXTUREMODES_*).
enum Mode : uint32_t { kNone = 0, kProject2d = 1, kProject3d = 2, kCubemap = 3, kPassthru = 4, kClipPlane = 5, kDotZw = 10, kDotProduct = 17 };

uint32_t mode_of(const CombinerSetup& s, uint32_t stage) { return (s.texture_modes >> (5 * stage)) & 0x1F; }
// The dot-product stages' source (PS_INPUTTEXTURE: stage 1 reads t0, stage 2
// bits 16-19, stage 3 bits 20-23) and how its colour reads as a vector
// (PS_DOTMAPPING: stage n's at bits 4(n-1) on).
uint32_t input_of(const CombinerSetup& s, uint32_t stage) { return stage < 2 ? 0 : (s.input_texture >> (16 + 4 * (stage - 2))) & 0xF; }
uint32_t dot_map_of(const CombinerSetup& s, uint32_t stage) { return stage ? (s.dot_mapping >> (4 * (stage - 1))) & 0x7 : 0; }

struct Ps {
    Builder b{spv::Stage::Fragment};
    Id f = b.t_float(), v3 = b.t_vec(3), v4 = b.t_vec(4), boolean = b.t_bool();
    Id uniforms = b.uniform_vec4_array(kFsUniformVec4s, 0, 1);
    Id regs[16] = {};  // vec4 each
    std::vector<Id> interface;
    Id zero4 = b.c_vec({0, 0, 0, 0}), one4 = b.c_vec({1, 1, 1, 1});

    Id uniform(uint32_t i) { return b.uniform_vec4(uniforms, b.c_int(int32_t(i))); }
    Id varying(uint32_t location, bool flat = false) {
        const Id v = b.input(v4, location);
        if (flat) b.decorate(v, spv::deco::Flat);
        interface.push_back(v);
        return b.load(v4, v);
    }
    Id comp(Id v, uint32_t k) { return b.extract(f, v, k); }
    Id clamp01(Id v) { return b.ext(glsl::FClamp, v4, {v, zero4, one4}); }

    // An input: register, channel (rgb, or alpha replicated; for the alpha
    // portion, blue or alpha), mapping. Returns a vec4 (alpha portion: the
    // value replicated).
    Id input(uint32_t code, bool alpha_portion) {
        const uint32_t reg = code & 0xF, channel_alpha = code & 0x10, mapping = (code >> 5) & 7;
        Id v = regs[reg] ? regs[reg] : zero4;
        if (alpha_portion) v = b.splat(comp(v, channel_alpha ? 3 : 2));
        else if (channel_alpha) v = b.splat(comp(v, 3));
        const Id pos = b.ext(glsl::FMax, v4, {v, zero4});
        const Id half = b.c_vec({0.5f, 0.5f, 0.5f, 0.5f}), two = b.c_vec({2, 2, 2, 2});
        switch (mapping) {
            case 0: return pos;                                                                   // UNSIGNED_IDENTITY
            case 1: return b.op(op::FSub, v4, {one4, clamp01(v)});                                // UNSIGNED_INVERT
            case 2: return b.op(op::FSub, v4, {b.op(op::FMul, v4, {pos, two}), one4});            // EXPAND_NORMAL
            case 3: return b.op(op::FSub, v4, {one4, b.op(op::FMul, v4, {pos, two})});            // EXPAND_NEGATE
            case 4: return b.op(op::FSub, v4, {pos, half});                                       // HALFBIAS_NORMAL
            case 5: return b.op(op::FSub, v4, {half, pos});                                       // HALFBIAS_NEGATE
            case 6: return v;                                                                     // SIGNED_IDENTITY
            default: return b.op(op::FNegate, v4, {v});                                           // SIGNED_NEGATE
        }
    }

    // The output mapping (bits 15-17 of a combiner output), then the
    // registers' range, [-1, 1].
    Id output_map(Id v, uint32_t outputs) {
        const uint32_t m = (outputs >> 15) & 7;
        const Id half = b.c_vec({0.5f, 0.5f, 0.5f, 0.5f});
        switch (m) {
            case 0: break;
            case 1: v = b.op(op::FSub, v4, {v, half}); break;                                                  // BIAS
            case 2: v = b.op(op::FMul, v4, {v, b.c_vec({2, 2, 2, 2})}); break;                                 // SHIFTLEFT_1
            case 3: v = b.op(op::FMul, v4, {b.op(op::FSub, v4, {v, half}), b.c_vec({2, 2, 2, 2})}); break;   // SHIFTLEFT_1_BIAS
            case 4: v = b.op(op::FMul, v4, {v, b.c_vec({4, 4, 4, 4})}); break;                                 // SHIFTLEFT_2
            case 6: v = b.op(op::FMul, v4, {v, half}); break;                                                  // SHIFTRIGHT_1
            default: throw std::runtime_error("combiner output mapping");
        }
        return b.ext(glsl::FClamp, v4, {v, b.c_vec({-1, -1, -1, -1}), one4});
    }

    // A colour as a vector for the dot-product stages, from its 8-bit
    // channels (as the renderer holds every texture): as is (ZERO_TO_ONE), or signed three ways: D3D's
    // (b - 128) / 127, OpenGL's ((2b + 1) / 255 - 1, two's complement), and
    // two's complement / 127.
    // HILO_1: the texel's 32 bits as two 16-bit halves, (hi, lo, 1): for an
    // A8R8G8B8 texel, hi is A and R, lo G and B.
    Id dot_map(Id col, uint32_t map) {
        if (map == 0) return col;
        auto byte = [&](uint32_t k) {
            return b.ext(glsl::RoundEven, f, {b.op(op::FMul, f, {b.ext(glsl::FClamp, f, {comp(col, k), b.c_float(0), b.c_float(1)}), b.c_float(255)})});
        };
        if (map == 4) {
            auto half = [&](uint32_t high, uint32_t low) {
                return b.op(op::FDiv, f, {b.op(op::FAdd, f, {b.op(op::FMul, f, {byte(high), b.c_float(256)}), byte(low)}), b.c_float(65535)});
            };
            return b.vec4(half(3, 0), half(1, 2), b.c_float(1), b.c_float(0));
        }
        auto ch = [&](uint32_t k) {
            const Id x = byte(k);
            if (map == 1) return b.op(op::FDiv, f, {b.op(op::FSub, f, {x, b.c_float(128)}), b.c_float(127)});
            const Id high = b.op(op::FOrdGreaterThanEqual, boolean, {x, b.c_float(128)});
            if (map == 2)
                return b.op(op::Select, f, {high, b.op(op::FDiv, f, {b.op(op::FSub, f, {x, b.c_float(255.5f)}), b.c_float(127.5f)}),
                                            b.op(op::FDiv, f, {b.op(op::FAdd, f, {x, b.c_float(0.5f)}), b.c_float(127.5f)})});
            return b.op(op::Select, f, {high, b.op(op::FDiv, f, {b.op(op::FSub, f, {x, b.c_float(256)}), b.c_float(127)}),
                                        b.op(op::FDiv, f, {x, b.c_float(127)})});
        };
        return b.vec4(ch(0), ch(1), ch(2), b.c_float(0));
    }

    Id dot3(Id a, Id c) {
        return b.splat(b.op(op::Dot, f, {b.shuffle(v3, a, a, {0, 1, 2}), b.shuffle(v3, c, c, {0, 1, 2})}));
    }

    // r0.a's selector for a mux: its MSB (>= 0.5) or LSB (of the 8-bit value).
    Id mux_select(bool msb) {
        const Id a = comp(regs[kR0], 3);
        if (msb) return b.op(op::FOrdGreaterThanEqual, boolean, {a, b.c_float(0.5f)});
        const Id u = b.op(op::ConvertFToU, b.t_uint(), {b.ext(glsl::RoundEven, f, {b.op(op::FMul, f, {b.ext(glsl::FClamp, f, {a, b.c_float(0), b.c_float(1)}), b.c_float(255)})})});
        return b.op(op::INotEqual, boolean, {b.op(op::BitwiseAnd, b.t_uint(), {u, b.c_uint(1)}), b.c_uint(0)});
    }
};

}  // namespace

uint64_t CombinerSetup::hash() const {
    uint64_t h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const unsigned char*>(this);
    for (size_t i = 0; i < sizeof(*this); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

const char* combiners_unsupported(const CombinerSetup& s) {
    for (uint32_t t = 0; t < 4; ++t) {
        const uint32_t m = mode_of(s, t);
        if (m == kDotProduct && (t == 0 || t == 3 || mode_of(s, 3) != kDotZw))
            return "texture mode DOTPRODUCT other than at stage 2 before DOT_ZW";
        if (m == kDotZw && (t != 3 || mode_of(s, 2) != kDotProduct)) return "texture mode DOT_ZW other than at stage 3 after DOTPRODUCT";
        if ((m == kDotProduct || m == kDotZw) && (dot_map_of(s, t) > 4 || input_of(s, t) >= t)) {
            static thread_local std::string why;
            why = "a dot-product stage with a HILO dot mapping, or reading a later stage (stage " + std::to_string(t) + ", dot mapping " +
                  std::to_string(s.dot_mapping) + ", input texture " + std::to_string(s.input_texture) + ")";
            return why.c_str();
        }
        if (m > kClipPlane && m != kDotZw && m != kDotProduct) {
            static thread_local std::string why;
            why = "texture mode " + std::to_string(m) + " at stage " + std::to_string(t) + " (modes " + std::to_string(s.texture_modes) + ")" +
                  " (beyond NONE, PROJECT2D, PROJECT3D, CUBEMAP, PASSTHRU and CLIPPLANE: bump mapping, dot products)";
            return why.c_str();
        }
        if ((m == kProject2d || m == kProject3d || m == kCubemap) && s.texture_dims[t] &&
            !((m == kCubemap) == (s.texture_dims[t] == 6)))
            return "a texture whose kind doesn't match its stage's texture mode";
    }
    if ((s.combiner_count & 0xFF) > 8) return "more than 8 combiner stages";
    return nullptr;
}

std::vector<uint32_t> combiners(const CombinerSetup& s) {
    if (const char* why = combiners_unsupported(s)) throw std::runtime_error(why);
    Ps p;
    Builder& b = p.b;

    const Id d0 = p.varying(kVaryD0), d1 = p.varying(kVaryD1), fog = p.varying(kVaryFog);
    Id t[4];
    for (uint32_t i = 0; i < 4; ++i) t[i] = p.varying(kVaryT0 + i);

    // Texture stages.
    Id dots[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t m = mode_of(s, i);
        Id result = p.zero4;
        const Id scale = p.uniform(kFsTexScale + i);
        if (m == kDotProduct || m == kDotZw) {
            // The stage's (s, t, r) with an earlier stage's colour; the register reads 0.
            const Id src = p.dot_map(p.regs[kT0 + input_of(s, i)] ? p.regs[kT0 + input_of(s, i)] : p.zero4, dot_map_of(s, i));
            dots[i] = p.comp(p.dot3(t[i], src), 0);
        } else if (m == kPassthru) {
            result = p.clamp01(t[i]);
        } else if (m == kClipPlane) {
            // Each of s, t, r, q: discard below 0 (compare bit clear) or at 0 and above.
            Id any = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const bool ge = (s.compare_mode >> (4 * i + k)) & 1;
                const Id c = b.op(ge ? op::FOrdGreaterThanEqual : op::FOrdLessThan, p.boolean, {p.comp(t[i], k), b.c_float(0)});
                any = any ? b.op(op::LogicalOr, p.boolean, {any, c}) : c;
            }
            b.kill_if(any);
        } else if ((m == kProject2d || m == kProject3d || m == kCubemap) && s.texture_dims[i]) {
            const uint32_t dims = s.texture_dims[i];
            const Id sampler_type = dims == 6 ? b.t_sampler_cube() : dims == 3 ? b.t_sampler3d() : b.t_sampler2d();
            const Id sampler = b.sampler(sampler_type, 0, 2 + i);
            const Id smp = b.load(sampler_type, sampler);
            if (m == kCubemap) {
                result = b.op(op::ImageSampleImplicitLod, p.v4, {smp, b.shuffle(p.v3, t[i], t[i], {0, 1, 2})});
            } else {
                // Projected: (s, t[, r]) / q, times the stage's scale (texels for linear textures).
                const Id q = p.comp(t[i], 3);
                const Id scaled = b.op(op::FMul, p.v4, {t[i], b.vec4(p.comp(scale, 0), p.comp(scale, 1), p.comp(scale, 2), b.c_float(1))});
                const Id coords = b.vec4(p.comp(scaled, 0), p.comp(scaled, 1), dims == 3 ? p.comp(scaled, 2) : q, q);
                if (dims == 3) {
                    const Id div = b.op(op::FDiv, p.v3, {b.shuffle(p.v3, coords, coords, {0, 1, 2}), b.splat(q, 3)});
                    result = b.op(op::ImageSampleImplicitLod, p.v4, {smp, div});
                } else {
                    result = b.op(op::ImageSampleProjImplicitLod, p.v4, {smp, b.shuffle(p.v3, coords, coords, {0, 1, 3})});
                }
            }
        }
        p.regs[kT0 + i] = result;
    }

    // The register file.
    p.regs[kZero] = p.zero4;
    p.regs[kV0] = d0;
    p.regs[kV1] = d1;
    const Id fog_color = p.uniform(kFsFogColor);
    p.regs[kFog] = b.vec4(p.comp(fog_color, 0), p.comp(fog_color, 1), p.comp(fog_color, 2),
                          b.ext(glsl::FClamp, p.f, {p.comp(fog, 0), b.c_float(0), b.c_float(1)}));
    // r0's alpha starts as t0's; everything else in r0 and r1 as 0.
    p.regs[kR0] = b.vec4(b.c_float(0), b.c_float(0), b.c_float(0), p.comp(p.regs[kT0], 3));
    p.regs[kR1] = p.zero4;

    const uint32_t count = s.combiner_count & 0xFF;
    const bool msb = s.combiner_count & 0x100, unique_c0 = s.combiner_count & 0x1000, unique_c1 = s.combiner_count & 0x10000;
    for (uint32_t i = 0; i < count; ++i) {
        p.regs[kC0] = p.uniform(kFsC0 + (unique_c0 ? i : 0));
        p.regs[kC1] = p.uniform(kFsC1 + (unique_c1 ? i : 0));
        // Both portions read the registers as they were before the stage.
        struct Out { uint32_t reg; Id value; bool alpha; };
        std::vector<Out> writes;
        for (int portion = 0; portion < 2; ++portion) {
            const bool alpha = portion == 1;
            const uint32_t in = alpha ? s.alpha_inputs[i] : s.rgb_inputs[i];
            const uint32_t outs = alpha ? s.alpha_outputs[i] : s.rgb_outputs[i];
            const Id A = p.input(in >> 24 & 0xFF, alpha), B = p.input(in >> 16 & 0xFF, alpha), C = p.input(in >> 8 & 0xFF, alpha),
                     D = p.input(in & 0xFF, alpha);
            const bool ab_dot = !alpha && (outs & 0x2000), cd_dot = !alpha && (outs & 0x1000);
            const Id ab = ab_dot ? p.dot3(A, B) : b.op(op::FMul, p.v4, {A, B});
            const Id cd = cd_dot ? p.dot3(C, D) : b.op(op::FMul, p.v4, {C, D});
            Id sum = 0;
            if (outs & 0x4000) {  // mux: CD where r0.a selects, else AB
                const Id sel = p.mux_select(msb);
                const Id sel4 = b.op(op::CompositeConstruct, b.t_bvec(4), {sel, sel, sel, sel});
                sum = b.op(op::Select, p.v4, {sel4, cd, ab});
            } else {
                sum = b.op(op::FAdd, p.v4, {ab, cd});
            }
            const uint32_t cd_reg = outs & 0xF, ab_reg = outs >> 4 & 0xF, sum_reg = outs >> 8 & 0xF;
            if (ab_reg) writes.push_back({ab_reg, p.output_map(ab, outs), alpha});
            if (cd_reg) writes.push_back({cd_reg, p.output_map(cd, outs), alpha});
            if (sum_reg) writes.push_back({sum_reg, p.output_map(sum, outs), alpha});
            if (!alpha) {  // blue to alpha: the AB or CD result's blue also into its register's alpha
                if ((outs & 0x80000) && ab_reg) writes.push_back({ab_reg | 0x100, p.output_map(ab, outs), true});
                if ((outs & 0x40000) && cd_reg) writes.push_back({cd_reg | 0x100, p.output_map(cd, outs), true});
            }
        }
        for (const Out& w : writes) {
            const uint32_t reg = w.reg & 0xF;
            const Id old = p.regs[reg] ? p.regs[reg] : p.zero4;
            if (w.reg & 0x100)  // blue into alpha
                p.regs[reg] = b.vec4(p.comp(old, 0), p.comp(old, 1), p.comp(old, 2), p.comp(w.value, 2));
            else if (w.alpha)
                p.regs[reg] = b.vec4(p.comp(old, 0), p.comp(old, 1), p.comp(old, 2), p.comp(w.value, 0));
            else
                p.regs[reg] = b.vec4(p.comp(w.value, 0), p.comp(w.value, 1), p.comp(w.value, 2), p.comp(old, 3));
        }
    }

    // The final combiner: rgb = A*B + (1-A)*C + D, alpha = G, with
    // V1R0_SUM (V1 + R0, either complemented, optionally clamped) and
    // EF_PROD (E * F) as extra inputs.
    p.regs[kC0] = p.uniform(kFsFinalC0);
    p.regs[kC1] = p.uniform(kFsFinalC1);
    const uint32_t efgh = s.final_efgh;
    {
        Id v1 = p.regs[kV1], r0 = p.regs[kR0];
        if (efgh & 0x40) v1 = b.op(op::FSub, p.v4, {p.one4, p.clamp01(v1)});
        if (efgh & 0x20) r0 = b.op(op::FSub, p.v4, {p.one4, p.clamp01(r0)});
        Id sum = b.op(op::FAdd, p.v4, {b.ext(glsl::FMax, p.v4, {v1, p.zero4}), b.ext(glsl::FMax, p.v4, {r0, p.zero4})});
        if (efgh & 0x80) sum = p.clamp01(sum);
        p.regs[kV1R0Sum] = sum;
        p.regs[kEfProd] = p.zero4;
        const Id E = p.input(efgh >> 24 & 0xFF, false), F = p.input(efgh >> 16 & 0xFF, false);
        p.regs[kEfProd] = b.op(op::FMul, p.v4, {E, F});
    }
    const uint32_t abcd = s.final_abcd;
    auto fin = [&](uint32_t code, bool alpha) { return p.clamp01(p.input(code & 0xFF, alpha)); };  // unsigned mappings only
    const Id A = fin(abcd >> 24, false), B = fin(abcd >> 16, false), C = fin(abcd >> 8, false), D = fin(abcd, false);
    const Id G = fin(efgh >> 8, true);
    const Id rgb = b.op(op::FAdd, p.v4, {b.op(op::FAdd, p.v4, {b.op(op::FMul, p.v4, {A, B}),
                                                               b.op(op::FMul, p.v4, {b.op(op::FSub, p.v4, {p.one4, A}), C})}),
                                         D});
    Id color = p.clamp01(b.vec4(p.comp(rgb, 0), p.comp(rgb, 1), p.comp(rgb, 2), p.comp(G, 0)));
    // With no final combiner set up (both input words 0), the output is r0.
    if (!s.final_abcd && !s.final_efgh) color = p.clamp01(p.regs[kR0]);

    // The alpha test, on the 8-bit alpha against the reference.
    if (s.alpha_func && s.alpha_func != 0x207) {
        const Id a8 = b.ext(glsl::RoundEven, p.f, {b.op(op::FMul, p.f, {p.comp(color, 3), b.c_float(255)})});
        const Id ref = p.comp(p.uniform(kFsAlphaRef), 0);
        uint16_t pass_op = 0;
        bool never = false;
        switch (s.alpha_func) {
            case 0x200: never = true; break;
            case 0x201: pass_op = op::FOrdLessThan; break;
            case 0x202: pass_op = op::FOrdEqual; break;
            case 0x203: pass_op = op::FOrdLessThanEqual; break;
            case 0x204: pass_op = op::FOrdGreaterThan; break;
            case 0x205: pass_op = op::FOrdNotEqual; break;
            case 0x206: pass_op = op::FOrdGreaterThanEqual; break;
            default: throw std::runtime_error("alpha function");
        }
        const Id pass = never ? b.c_bool(false) : b.op(pass_op, p.boolean, {a8, ref});
        b.kill_if(b.op(op::LogicalNot, p.boolean, {pass}));
    }

    const Id out = b.output(p.v4, 0);
    b.store(out, color);
    p.interface.push_back(out);
    // DOT_ZW: the pixel's depth is stage 2's dot product over stage 3's (z / w).
    if (mode_of(s, 3) == kDotZw) {
        b.depth_replacing();
        const Id depth = b.builtin_var(spv::Storage::Output, p.f, spv::builtin::FragDepth);
        const Id zw = b.op(op::FDiv, p.f, {dots[2], dots[3]});
        b.store(depth, b.ext(glsl::FClamp, p.f, {zw, b.c_float(0), b.c_float(1)}));
        p.interface.push_back(depth);
    }
    return b.finish(p.interface);
}

}  // namespace qilin::gfx::nv2a
