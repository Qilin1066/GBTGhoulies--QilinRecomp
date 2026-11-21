// NV2A vertex programs to SPIR-V. Each instruction reads its operands, then
// the MAC and ILU results are written (temporaries, outputs) under their
// masks, as the hardware does. The program runs straight through, so
// registers are SSA values held here, not variables.
//
// The semantics follow the NV2A as documented by xemu's vertex shader
// translator (nv2a/vsh.c), which was checked against hardware.
#include <stdexcept>

#include "nv2a_shaders.hpp"
#include "spirv.hpp"

namespace qilin::gfx::nv2a {

namespace {

using spv::Builder;
using spv::Id;
namespace op = spv::op;
namespace glsl = spv::glsl;

struct Vs {
    Builder b{spv::Stage::Vertex};
    Id f = b.t_float(), v4 = b.t_vec(4), i32 = b.t_int(), boolean = b.t_bool();
    Id consts = b.uniform_vec4_array(kVsUniformVec4s, 0, 0);
    Id inputs[16] = {};
    Id temps[13] = {};    // r0-r11, r12 = oPos
    Id outputs[13] = {};  // o-registers by address (oPos is temps[12])
    Id a0 = 0;            // int
    std::vector<Id> interface;

    Id zero4 = b.c_vec({0, 0, 0, 0}), w1 = b.c_vec({0, 0, 0, 1});

    Id constant(uint32_t index, bool relative) {
        Id i = b.c_int(int32_t(index));
        if (relative) {
            i = b.op(op::IAdd, i32, {a0 ? a0 : b.c_int(0), i});
            i = b.ext(glsl::SClamp, i32, {i, b.c_int(0), b.c_int(kVsConstants - 1)});
        }
        return b.uniform_vec4(consts, i);
    }

    Id read(const vsh::Operand& o, const vsh::Instruction& in) {
        Id v = 0;
        switch (o.kind) {
            case vsh::Src::Temp: v = temps[o.index]; break;
            case vsh::Src::Input: v = inputs[o.index]; break;
            case vsh::Src::Const: v = constant(in.constant, in.relative); break;
        }
        if (o.swizzle[0] != 0 || o.swizzle[1] != 1 || o.swizzle[2] != 2 || o.swizzle[3] != 3)
            v = b.shuffle(v4, v, v, {o.swizzle[0], o.swizzle[1], o.swizzle[2], o.swizzle[3]});
        if (o.negate) v = b.op(op::FNegate, v4, {v});
        return v;
    }

    // Writes `value` into `old` where mask (bit 3 = x … bit 0 = w) is set.
    Id masked(Id old, Id value, uint8_t mask) {
        if (mask == 0xF) return value;
        auto pick = [&](uint32_t k) { return (mask >> (3 - k)) & 1 ? 4 + k : k; };
        return b.shuffle(v4, old, value, {pick(0), pick(1), pick(2), pick(3)});
    }

    Id x(Id v) { return b.extract(f, v, 0); }
    Id y(Id v) { return b.extract(f, v, 1); }
    Id z(Id v) { return b.extract(f, v, 2); }
    Id w(Id v) { return b.extract(f, v, 3); }
    // Dot products as products and sums in a fixed order, each rounded as
    // IEEE has it: OpDot's order and fusing are each GPU's own, and the
    // depths of a surface drawn twice by two programs (a floor and its
    // projected shadow) must round alike on every GPU.
    Id dot(Id a, Id c, uint32_t n) {
        const Id p = b.op(op::FMul, v4, {a, c});
        Id sum = b.extract(f, p, 0);
        for (uint32_t k = 1; k < n; ++k) sum = b.op(op::FAdd, f, {sum, b.extract(f, p, k)});
        return sum;
    }
    Id dot3(Id a, Id c) { return dot(a, c, 3); }
    // 1/s, refined by a Newton step: Vulkan's division may be 2.5 ULP out,
    // a GPU's own amount; the step brings it to within about one (left
    // alone for zeros, infinities and the extremes, where it can't help).
    Id reciprocal(Id s) {
        const Id r = b.op(op::FDiv, f, {b.c_float(1), s});
        const Id e = b.op(op::FSub, f, {b.c_float(1), b.op(op::FMul, f, {s, r})});
        const Id refined = b.op(op::FAdd, f, {r, b.op(op::FMul, f, {r, e})});
        const Id m = b.ext(glsl::FAbs, f, {s});
        const Id ok = b.op(op::LogicalAnd, boolean, {b.op(op::FOrdGreaterThan, boolean, {m, b.c_float(1e-30f)}),
                                                     b.op(op::FOrdLessThan, boolean, {m, b.c_float(1e30f)})});
        return b.op(op::Select, f, {ok, refined, r});
    }
    Id cmp(uint16_t compare, Id a, Id c) {  // 1.0 per component where it holds, else 0.0
        const Id bv = b.t_bvec(4);
        return b.op(op::Select, v4, {b.op(compare, bv, {a, c}), b.splat(b.c_float(1)), zero4});
    }

    Id mac(const vsh::Instruction& in, Id a, Id bb, Id c) {
        switch (in.mac) {
            case vsh::Mac::Mov: return a;
            case vsh::Mac::Mul: return b.op(op::FMul, v4, {a, bb});
            case vsh::Mac::Add: return b.op(op::FAdd, v4, {a, c});
            case vsh::Mac::Mad: return b.op(op::FAdd, v4, {b.op(op::FMul, v4, {a, bb}), c});
            case vsh::Mac::Dp3: return b.splat(dot3(a, bb));
            case vsh::Mac::Dph: return b.splat(b.op(op::FAdd, f, {dot3(a, bb), w(bb)}));
            case vsh::Mac::Dp4: return b.splat(dot(a, bb, 4));
            case vsh::Mac::Dst: return b.vec4(b.c_float(1), b.op(op::FMul, f, {y(a), y(bb)}), z(a), w(bb));
            case vsh::Mac::Min: return b.ext(glsl::FMin, v4, {a, bb});
            case vsh::Mac::Max: return b.ext(glsl::FMax, v4, {a, bb});
            case vsh::Mac::Slt: return cmp(op::FOrdLessThan, a, bb);
            case vsh::Mac::Sge: return cmp(op::FOrdGreaterThanEqual, a, bb);
            default: throw std::logic_error("vertex program: MAC operation");
        }
    }

    Id ilu(const vsh::Instruction& in, Id c) {
        const Id s = x(c);
        switch (in.ilu) {
            case vsh::Ilu::Mov: return c;
            case vsh::Ilu::Rcp: return b.splat(reciprocal(s));
            case vsh::Ilu::Rcc: {
                // The reciprocal, its magnitude clamped to [5.42101e-20, 1.884467e+19].
                const Id r = reciprocal(s);
                const Id pos = b.ext(glsl::FClamp, f, {r, b.c_float(5.42101e-20f), b.c_float(1.884467e+19f)});
                const Id neg = b.ext(glsl::FClamp, f, {r, b.c_float(-1.884467e+19f), b.c_float(-5.42101e-20f)});
                return b.splat(b.op(op::Select, f, {b.op(op::FOrdGreaterThan, boolean, {r, b.c_float(0)}), pos, neg}));
            }
            case vsh::Ilu::Rsq: return b.splat(b.ext(glsl::InverseSqrt, f, {b.ext(glsl::FAbs, f, {s})}));
            case vsh::Ilu::Exp: {
                const Id fl = b.ext(glsl::Floor, f, {s});
                return b.vec4(b.ext(glsl::Exp2, f, {fl}), b.op(op::FSub, f, {s, fl}), b.ext(glsl::Exp2, f, {s}), b.c_float(1));
            }
            case vsh::Ilu::Log: {
                const Id t = b.ext(glsl::FAbs, f, {s});
                const Id l = b.ext(glsl::Log2, f, {t});
                const Id e = b.ext(glsl::Floor, f, {l});
                const Id m = b.op(op::FDiv, f, {t, b.ext(glsl::Exp2, f, {e})});
                return b.vec4(e, m, l, b.c_float(1));
            }
            case vsh::Ilu::Lit: {
                const float eps = 1.0f / 256.0f;
                const Id diffuse = b.ext(glsl::FMax, f, {x(c), b.c_float(0)});
                const Id power = b.ext(glsl::FClamp, f, {w(c), b.c_float(-(128 - eps)), b.c_float(128 - eps)});
                const Id spec = b.ext(glsl::Pow, f, {b.ext(glsl::FMax, f, {y(c), b.c_float(0)}), power});
                const Id lit = b.op(op::FOrdGreaterThan, boolean, {diffuse, b.c_float(0)});
                return b.vec4(b.c_float(1), diffuse, b.op(op::Select, f, {lit, spec, b.c_float(0)}), b.c_float(1));
            }
            default: throw std::logic_error("vertex program: ILU operation");
        }
    }
};

}  // namespace

std::vector<uint32_t> vertex_program(const vsh::Program& program, uint32_t inputs) {
    Vs s;
    Builder& b = s.b;
    // The position exactly as the program computes it, in every program: a
    // game draws the same surface again with another program (a floor's
    // projected shadow, at a ZBias of a few units of depth, or an EQUAL
    // depth test) and relies on the two depths agreeing, as they do on the
    // NV2A. A GPU's compiler may otherwise fuse and reorder each program's
    // arithmetic its own way, and the second pass falls behind the first in
    // patches (shadows flickering and missing on real GPUs).
    b.exact_arithmetic(true);
    for (uint32_t n = 0; n < 16; ++n) {
        if (inputs & (1u << n)) {
            const Id v = b.input(s.v4, n);
            s.interface.push_back(v);
            s.inputs[n] = b.load(s.v4, v);
        } else {
            s.inputs[n] = s.w1;
        }
    }
    for (Id& t : s.temps) t = s.w1;
    s.temps[12] = s.zero4;
    for (Id& o : s.outputs) o = s.w1;

    for (const vsh::Instruction& in : program.code) {
        // Every operand is read before anything is written.
        const Id a = s.read(in.a, in), bb = s.read(in.b, in), c = s.read(in.c, in);
        Id mac = 0, ilu = 0;
        if (in.mac == vsh::Mac::Arl) {
            s.a0 = b.op(op::ConvertFToS, s.i32, {b.ext(glsl::Floor, s.f, {s.x(a)})});
        } else if (in.mac != vsh::Mac::Nop) {
            mac = s.mac(in, a, bb, c);
        }
        if (in.ilu != vsh::Ilu::Nop) ilu = s.ilu(in, c);
        if (mac && in.mac_temp_mask) s.temps[in.temp] = s.masked(s.temps[in.temp], mac, in.mac_temp_mask);
        if (ilu && in.ilu_temp_mask) s.temps[in.ilu_temp()] = s.masked(s.temps[in.ilu_temp()], ilu, in.ilu_temp_mask);
        if (in.out_mask) {
            const Id v = in.out_from_ilu ? ilu : mac;
            if (!v) throw std::runtime_error("vertex program: an output write with no operation");
            if (in.out_is_const) throw std::runtime_error("vertex program: writes to constants aren't supported yet");
            if (in.out_address == vsh::kPos) s.temps[12] = s.masked(s.temps[12], v, in.out_mask);
            else if (in.out_address < 13) s.outputs[in.out_address] = s.masked(s.outputs[in.out_address], v, in.out_mask);
        }
        if (in.final) break;
    }

    // Screen space (what the program computes, D3D's viewport transform
    // included) to clip space: x and y over the target, z over the depth
    // scale, all times w, so the rasteriser divides them back.
    const Id pos = s.temps[12];
    const Id screen = b.uniform_vec4(s.consts, b.c_int(kVsScreen));
    const Id pw = s.w(pos);
    auto axis = [&](Id v, uint32_t k, bool centred) {
        Id t = b.op(op::FMul, s.f, {v, b.extract(s.f, screen, k)});
        if (centred) t = b.op(op::FSub, s.f, {t, b.c_float(1)});
        return b.op(op::FMul, s.f, {t, pw});
    };
    // Squeezed across about a point first (the HUD at wide shapes): z + (x - z) * y,
    // exactly x where y is 1 and z 0.
    const Id pixel = b.uniform_vec4(s.consts, b.c_int(kVsPixels));
    const Id about = b.extract(s.f, pixel, 2);
    const Id x = b.op(op::FAdd, s.f, {about, b.op(op::FMul, s.f, {b.op(op::FSub, s.f, {s.x(pos), about}), b.extract(s.f, pixel, 1)})});
    const Id clip = b.vec4(axis(x, 0, true), axis(s.y(pos), 1, true), axis(s.z(pos), 2, false), pw);
    const Id position = b.builtin_var(spv::Storage::Output, s.v4, spv::builtin::Position);
    b.decorate(position, spv::deco::Invariant);
    b.store(position, clip);
    s.interface.push_back(position);

    // The point size, in pixels: the program's oPts.x where it writes it,
    // else D3DRS_POINTSIZE (the screen uniform's w); at least a pixel and
    // at most the hardware's 63.875 (the 6.3 size field D3D clamps to 0x1FF).
    bool writes_size = false;
    for (const auto& in : program.code) writes_size |= in.out_mask && !in.out_is_const && in.out_address == vsh::kPts;
    const Id size_in = writes_size ? s.x(s.outputs[vsh::kPts]) : b.extract(s.f, screen, 3);
    const Id point_size = b.builtin_var(spv::Storage::Output, s.f, spv::builtin::PointSize);
    const Id pixels = b.extract(s.f, b.uniform_vec4(s.consts, b.c_int(kVsPixels)), 0);  // the render resolution's scale
    b.store(point_size, b.op(spv::op::FMul, s.f, {b.ext(glsl::FClamp, s.f, {size_in, b.c_float(1), b.c_float(63.875f)}), pixels}));
    s.interface.push_back(point_size);

    // Colours are clamped to [0, 1], as the hardware does.
    const Id zero = b.c_vec({0, 0, 0, 0}), one = b.c_vec({1, 1, 1, 1});
    auto out = [&](uint32_t location, Id value) {
        const Id v = b.output(s.v4, location);
        b.store(v, value);
        s.interface.push_back(v);
    };
    auto sat = [&](Id v) { return b.ext(glsl::FClamp, s.v4, {v, zero, one}); };
    out(kVaryD0, sat(s.outputs[vsh::kD0]));
    out(kVaryD1, sat(s.outputs[vsh::kD1]));
    out(kVaryFog, s.outputs[vsh::kFog]);
    for (uint32_t t = 0; t < 4; ++t) out(kVaryT0 + t, s.outputs[vsh::kT0 + t]);
    out(kVaryB0, sat(s.outputs[vsh::kB0]));
    out(kVaryB1, sat(s.outputs[vsh::kB1]));
    return b.finish(s.interface);
}

}  // namespace qilin::gfx::nv2a
