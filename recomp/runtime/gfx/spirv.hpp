// A small SPIR-V builder: the renderer generates its shaders (the NV2A's
// vertex programs, fixed-function pipeline and register combiners) as
// SPIR-V directly, so no shader compiler is needed at build or run time.
//
// Only what those shaders need: scalar and vector float, int and bool types,
// inputs, outputs, uniform blocks, combined image samplers, arithmetic, the
// GLSL.std.450 instructions, selects, structured if, discard.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace qilin::gfx::spv {

using Id = uint32_t;

namespace op {
enum : uint16_t {
    Undef = 1, ExtInstImport = 11, ExtInst = 12, MemoryModel = 14, EntryPoint = 15, ExecutionMode = 16, Capability = 17,
    TypeVoid = 19, TypeBool = 20, TypeInt = 21, TypeFloat = 22, TypeVector = 23, TypeImage = 25, TypeSampledImage = 27,
    TypeArray = 28, TypeStruct = 30, TypePointer = 32, TypeFunction = 33, ConstantTrue = 41, ConstantFalse = 42,
    Constant = 43, ConstantComposite = 44, Function = 54, FunctionEnd = 56, Variable = 59, Load = 61, Store = 62,
    AccessChain = 65, Decorate = 71, MemberDecorate = 72, VectorExtractDynamic = 77, VectorShuffle = 79,
    CompositeConstruct = 80, CompositeExtract = 81, CompositeInsert = 82, ImageSampleImplicitLod = 87,
    ImageSampleProjImplicitLod = 91, ConvertFToU = 109, ConvertFToS = 110, ConvertSToF = 111, ConvertUToF = 112,
    Bitcast = 124, SNegate = 126, FNegate = 127, IAdd = 128, FAdd = 129, ISub = 130, FSub = 131, IMul = 132, FMul = 133,
    FDiv = 136, VectorTimesScalar = 142, Dot = 148, Any = 154, All = 155, LogicalOr = 166, LogicalAnd = 167,
    LogicalNot = 168, Select = 169, IEqual = 170, INotEqual = 171, ULessThan = 176, SLessThan = 177, FOrdEqual = 180,
    FOrdNotEqual = 182, FOrdLessThan = 184, FOrdGreaterThan = 186, FOrdLessThanEqual = 188, FOrdGreaterThanEqual = 190,
    ShiftRightLogical = 194, ShiftLeftLogical = 196, BitwiseOr = 197, BitwiseAnd = 199, SelectionMerge = 247,
    Label = 248, Branch = 249, BranchConditional = 250, Kill = 252, Return = 253,
};
}  // namespace op

namespace glsl {
enum : uint32_t {
    Floor = 8, Fract = 10, Exp2 = 29, Log2 = 30, Sqrt = 31, InverseSqrt = 32, FAbs = 4, FMin = 37, FMax = 40,
    FClamp = 43, SClamp = 45, FMix = 46, RoundEven = 2, Fma = 50, Pow = 26, Normalize = 69, Length = 66, Reflect = 71,
};
}  // namespace glsl

enum class Storage : uint32_t { UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Function = 7, PushConstant = 9 };
enum class Stage : uint32_t { Vertex = 0, Fragment = 4 };

namespace deco {
enum : uint32_t { Block = 2, ArrayStride = 6, BuiltIn = 11, NoPerspective = 13, Flat = 14, Location = 30, Binding = 33, DescriptorSet = 34, Offset = 35 };
}
namespace builtin {
enum : uint32_t { Position = 0, PointSize = 1, FragCoord = 15, PointCoord = 16, FragDepth = 22 };
}

class Builder {
public:
    explicit Builder(Stage stage);

    Id id() { return next_++; }

    // Types (each made once).
    Id t_void();
    Id t_bool();
    Id t_float();
    Id t_int();
    Id t_uint();
    Id t_vec(uint32_t n);   // float vector, n 2..4
    Id t_bvec(uint32_t n);  // bool vector
    Id t_array(Id element, uint32_t count);
    Id t_struct(std::initializer_list<Id> members);
    Id t_ptr(Storage s, Id type);
    Id t_func(Id ret);
    Id t_sampler2d();  // a combined image sampler, 2D float
    Id t_sampler3d();
    Id t_sampler_cube();

    // Constants (each made once).
    Id c_float(float v);
    Id c_int(int32_t v);
    Id c_uint(uint32_t v);
    Id c_bool(bool v);
    Id c_vec(std::initializer_list<float> v);  // 2..4 components

    // Globals and interface.
    Id var(Storage s, Id type);  // a global variable of pointer-to-type
    Id input(Id type, uint32_t location);
    Id output(Id type, uint32_t location);
    Id builtin_var(Storage s, Id type, uint32_t which);
    void depth_replacing() { depth_replacing_ = true; }  // a fragment shader that writes FragDepth
    // A uniform block: one member, an array of `count` vec4, at set/binding.
    Id uniform_vec4_array(uint32_t count, uint32_t set, uint32_t binding);
    Id sampler(Id sampler_type, uint32_t set, uint32_t binding);
    void decorate(Id target, uint32_t decoration, std::initializer_list<uint32_t> args = {});

    // The body: instructions go into main() in order.
    Id op(uint16_t opcode, Id result_type, std::initializer_list<Id> operands);
    Id op(uint16_t opcode, Id result_type, const std::vector<Id>& operands);
    void op_void(uint16_t opcode, std::initializer_list<Id> operands);
    Id ext(uint32_t instruction, Id result_type, std::initializer_list<Id> args);
    Id load(Id type, Id pointer) { return op(op::Load, type, {pointer}); }
    void store(Id pointer, Id value) { op_void(op::Store, {pointer, value}); }

    // Helpers on vec4 floats.
    Id vec4(Id x, Id y, Id z, Id w);
    Id splat(Id scalar, uint32_t n = 4);
    Id extract(Id result_type, Id composite, uint32_t index);
    Id shuffle(Id result_type, Id a, Id b, std::initializer_list<uint32_t> components);
    Id uniform_vec4(Id block, Id index);  // block[0][index], index an int Id

    // Structured if: begins a block run only when cond holds.
    void begin_if(Id cond);
    void end_if();
    void kill_if(Id cond);  // fragment: discard when cond holds

    // Finishes main() and returns the module; `interface` are the entry
    // point's input and output variables.
    std::vector<uint32_t> finish(const std::vector<Id>& interface);

private:
    bool depth_replacing_ = false;
    using Words = std::vector<uint32_t>;
    static void emit(Words& w, uint16_t opcode, std::initializer_list<uint32_t> operands);
    static void emit(Words& w, uint16_t opcode, const std::vector<uint32_t>& operands);
    Id type_or_const(uint16_t opcode, const std::vector<uint32_t>& operands);  // dedup in globals_

    Stage stage_;
    Id next_ = 1;
    Id glsl_ = 0, main_ = 0;
    Words annotations_, globals_, code_;
    std::map<std::vector<uint32_t>, Id> made_;  // opcode + operands -> id, for types and constants
    std::vector<Id> merges_;                     // open ifs' merge labels
};

}  // namespace qilin::gfx::spv
