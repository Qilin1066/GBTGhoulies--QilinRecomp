#include "spirv.hpp"

#include <bit>
#include <stdexcept>

namespace qilin::gfx::spv {

namespace {
constexpr uint32_t kMagic = 0x07230203, kVersion = 0x00010000;  // SPIR-V 1.0
}

Builder::Builder(Stage stage) : stage_(stage) {
    glsl_ = id();
    main_ = id();
}

void Builder::emit(Words& w, uint16_t opcode, std::initializer_list<uint32_t> operands) {
    w.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
    w.insert(w.end(), operands.begin(), operands.end());
}

void Builder::emit(Words& w, uint16_t opcode, const std::vector<uint32_t>& operands) {
    w.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
    w.insert(w.end(), operands.begin(), operands.end());
}

// Types and constants: made once each. The key is the opcode and operands
// without the result id (which is operands[0] for a type, operands[1] for a
// constant: the caller passes a placeholder 0 there).
Id Builder::type_or_const(uint16_t opcode, const std::vector<uint32_t>& operands) {
    std::vector<uint32_t> key{opcode};
    key.insert(key.end(), operands.begin(), operands.end());
    if (auto it = made_.find(key); it != made_.end()) return it->second;
    const Id r = id();
    std::vector<uint32_t> ops = operands;
    const bool is_const = opcode == op::Constant || opcode == op::ConstantComposite || opcode == op::ConstantTrue ||
                          opcode == op::ConstantFalse;
    ops[is_const ? 1 : 0] = r;
    emit(globals_, opcode, ops);
    made_[key] = r;
    return r;
}

Id Builder::t_void() { return type_or_const(op::TypeVoid, {0}); }
Id Builder::t_bool() { return type_or_const(op::TypeBool, {0}); }
Id Builder::t_float() { return type_or_const(op::TypeFloat, {0, 32}); }
Id Builder::t_int() { return type_or_const(op::TypeInt, {0, 32, 1}); }
Id Builder::t_uint() { return type_or_const(op::TypeInt, {0, 32, 0}); }
Id Builder::t_vec(uint32_t n) { return type_or_const(op::TypeVector, {0, t_float(), n}); }
Id Builder::t_bvec(uint32_t n) { return type_or_const(op::TypeVector, {0, t_bool(), n}); }
Id Builder::t_array(Id element, uint32_t count) { return type_or_const(op::TypeArray, {0, element, c_uint(count)}); }
Id Builder::t_struct(std::initializer_list<Id> members) {
    std::vector<uint32_t> ops{0};
    ops.insert(ops.end(), members.begin(), members.end());
    // Structs aren't shared: each gets its own decorations.
    const Id r = id();
    ops[0] = r;
    emit(globals_, op::TypeStruct, ops);
    return r;
}
Id Builder::t_ptr(Storage s, Id type) { return type_or_const(op::TypePointer, {0, uint32_t(s), type}); }
Id Builder::t_func(Id ret) { return type_or_const(op::TypeFunction, {0, ret}); }
Id Builder::t_sampler2d() {
    const Id image = type_or_const(op::TypeImage, {0, t_float(), 1, 0, 0, 0, 1, 0});  // 2D, sampled, format unknown
    return type_or_const(op::TypeSampledImage, {0, image});
}
Id Builder::t_sampler3d() {
    const Id image = type_or_const(op::TypeImage, {0, t_float(), 2, 0, 0, 0, 1, 0});
    return type_or_const(op::TypeSampledImage, {0, image});
}
Id Builder::t_sampler_cube() {
    const Id image = type_or_const(op::TypeImage, {0, t_float(), 3, 0, 0, 0, 1, 0});
    return type_or_const(op::TypeSampledImage, {0, image});
}

Id Builder::c_float(float v) { return type_or_const(op::Constant, {t_float(), 0, std::bit_cast<uint32_t>(v)}); }
Id Builder::c_int(int32_t v) { return type_or_const(op::Constant, {t_int(), 0, uint32_t(v)}); }
Id Builder::c_uint(uint32_t v) { return type_or_const(op::Constant, {t_uint(), 0, v}); }
Id Builder::c_bool(bool v) { return type_or_const(v ? op::ConstantTrue : op::ConstantFalse, {t_bool(), 0}); }
Id Builder::c_vec(std::initializer_list<float> v) {
    std::vector<uint32_t> ops{t_vec(uint32_t(v.size())), 0};
    for (float f : v) ops.push_back(c_float(f));
    return type_or_const(op::ConstantComposite, ops);
}

Id Builder::var(Storage s, Id type) {
    const Id r = id();
    emit(globals_, op::Variable, {t_ptr(s, type), r, uint32_t(s)});
    return r;
}
void Builder::decorate(Id target, uint32_t decoration, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> ops{target, decoration};
    ops.insert(ops.end(), args.begin(), args.end());
    emit(annotations_, op::Decorate, ops);
}
Id Builder::input(Id type, uint32_t location) {
    const Id v = var(Storage::Input, type);
    decorate(v, deco::Location, {location});
    return v;
}
Id Builder::output(Id type, uint32_t location) {
    const Id v = var(Storage::Output, type);
    decorate(v, deco::Location, {location});
    return v;
}
Id Builder::builtin_var(Storage s, Id type, uint32_t which) {
    const Id v = var(s, type);
    decorate(v, deco::BuiltIn, {which});
    return v;
}
Id Builder::uniform_vec4_array(uint32_t count, uint32_t set, uint32_t binding) {
    const Id arr = t_array(t_vec(4), count);
    // Each array type is decorated once (its stride), whatever block uses it.
    static_assert(sizeof(float) * 4 == 16);
    std::vector<uint32_t> key{0xFFFF, arr};
    if (!made_.contains(key)) {
        decorate(arr, deco::ArrayStride, {16});
        made_[key] = arr;
    }
    const Id block = t_struct({arr});
    decorate(block, deco::Block);
    emit(annotations_, op::MemberDecorate, {block, 0, deco::Offset, 0});
    const Id v = var(Storage::Uniform, block);
    decorate(v, deco::DescriptorSet, {set});
    decorate(v, deco::Binding, {binding});
    return v;
}
Id Builder::sampler(Id sampler_type, uint32_t set, uint32_t binding) {
    const Id v = var(Storage::UniformConstant, sampler_type);
    decorate(v, deco::DescriptorSet, {set});
    decorate(v, deco::Binding, {binding});
    return v;
}

void Builder::mark_exact(uint16_t opcode, Id r) {
    if (!exact_) return;
    switch (opcode) {
        case op::FNegate: case op::FAdd: case op::FSub: case op::FMul: case op::FDiv: case op::VectorTimesScalar: case op::Dot:
            decorate(r, deco::NoContraction);
            break;
        default: break;
    }
}
Id Builder::op(uint16_t opcode, Id result_type, std::initializer_list<Id> operands) {
    const Id r = id();
    std::vector<uint32_t> ops{result_type, r};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit(code_, opcode, ops);
    mark_exact(opcode, r);
    return r;
}
Id Builder::op(uint16_t opcode, Id result_type, const std::vector<Id>& operands) {
    const Id r = id();
    std::vector<uint32_t> ops{result_type, r};
    ops.insert(ops.end(), operands.begin(), operands.end());
    emit(code_, opcode, ops);
    mark_exact(opcode, r);
    return r;
}
void Builder::op_void(uint16_t opcode, std::initializer_list<Id> operands) { emit(code_, opcode, operands); }
Id Builder::ext(uint32_t instruction, Id result_type, std::initializer_list<Id> args) {
    const Id r = id();
    std::vector<uint32_t> ops{result_type, r, glsl_, instruction};
    ops.insert(ops.end(), args.begin(), args.end());
    emit(code_, op::ExtInst, ops);
    return r;
}

Id Builder::vec4(Id x, Id y, Id z, Id w) { return op(op::CompositeConstruct, t_vec(4), {x, y, z, w}); }
Id Builder::splat(Id scalar, uint32_t n) {
    std::vector<Id> v(n, scalar);
    return op(op::CompositeConstruct, t_vec(n), v);
}
Id Builder::extract(Id result_type, Id composite, uint32_t index) {
    const Id r = id();
    emit(code_, op::CompositeExtract, {result_type, r, composite, index});
    return r;
}
Id Builder::shuffle(Id result_type, Id a, Id b, std::initializer_list<uint32_t> components) {
    const Id r = id();
    std::vector<uint32_t> ops{result_type, r, a, b};
    ops.insert(ops.end(), components.begin(), components.end());
    emit(code_, op::VectorShuffle, ops);
    return r;
}
Id Builder::uniform_vec4(Id block, Id index) {
    const Id p = op(op::AccessChain, t_ptr(Storage::Uniform, t_vec(4)), {block, c_int(0), index});
    return load(t_vec(4), p);
}

void Builder::begin_if(Id cond) {
    const Id then_label = id(), merge = id();
    emit(code_, op::SelectionMerge, {merge, 0});
    emit(code_, op::BranchConditional, {cond, then_label, merge});
    emit(code_, op::Label, {then_label});
    merges_.push_back(merge);
}
void Builder::end_if() {
    const Id merge = merges_.back();
    merges_.pop_back();
    emit(code_, op::Branch, {merge});
    emit(code_, op::Label, {merge});
}
void Builder::kill_if(Id cond) {
    const Id then_label = id(), merge = id();
    emit(code_, op::SelectionMerge, {merge, 0});
    emit(code_, op::BranchConditional, {cond, then_label, merge});
    emit(code_, op::Label, {then_label});
    emit(code_, op::Kill, {});
    emit(code_, op::Label, {merge});
}

std::vector<uint32_t> Builder::finish(const std::vector<Id>& interface) {
    if (!merges_.empty()) throw std::logic_error("spirv: an if left open");
    const Id fn_type = t_func(t_void());
    Words out{kMagic, kVersion, 0, 0, 0};
    emit(out, op::Capability, {1});  // Shader
    // GLSL.std.450, as a literal string.
    {
        const char name[] = "GLSL.std.450";
        std::vector<uint32_t> ops{glsl_};
        for (size_t i = 0; i < sizeof(name); i += 4) {
            uint32_t w = 0;
            for (size_t k = 0; k < 4 && i + k < sizeof(name); ++k) w |= uint32_t(uint8_t(name[i + k])) << (8 * k);
            ops.push_back(w);
        }
        emit(out, op::ExtInstImport, ops);
    }
    emit(out, op::MemoryModel, {0, 1});  // Logical, GLSL450
    {
        std::vector<uint32_t> ops{uint32_t(stage_), main_, 0x6E69616D, 0};  // "main"
        ops.insert(ops.end(), interface.begin(), interface.end());
        emit(out, op::EntryPoint, ops);
    }
    if (stage_ == Stage::Fragment) emit(out, op::ExecutionMode, {main_, 7});  // OriginUpperLeft
    if (depth_replacing_) emit(out, op::ExecutionMode, {main_, 12});  // DepthReplacing
    out.insert(out.end(), annotations_.begin(), annotations_.end());
    out.insert(out.end(), globals_.begin(), globals_.end());
    emit(out, op::Function, {t_void(), main_, 0, fn_type});
    emit(out, op::Label, {id()});
    out.insert(out.end(), code_.begin(), code_.end());
    emit(out, op::Return, {});
    emit(out, op::FunctionEnd, {});
    out[3] = next_;  // the bound
    return out;
}

}  // namespace qilin::gfx::spv
