// Xbox vertex programs: the NV2A's vertex-shader microcode, as D3D's
// CreateVertexShader receives it (XGAssembleShader's output), decoded.
// docs/graphics.md, "Vertex shaders".
//
// A program is a header word, then 128-bit instructions. Each instruction
// pairs a MAC operation (vector maths) with an ILU operation (scalar
// functions), reading up to three operands (A, B, C) from temporaries
// (r0-r12, r12 being oPos read back), inputs (v0-v15) or constants
// (c0-c191, the D3D registers c-96…c95), and writing temporaries, outputs
// (oPos, oD0 …) or constants.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qilin::gfx::vsh {

enum class Mac : uint8_t { Nop, Mov, Mul, Add, Mad, Dp3, Dph, Dp4, Dst, Min, Max, Slt, Sge, Arl };
enum class Ilu : uint8_t { Nop, Mov, Rcp, Rcc, Rsq, Exp, Log, Lit };
enum class Src : uint8_t { Temp = 1, Input = 2, Const = 3 };

struct Operand {
    Src kind = Src::Temp;
    uint8_t index = 0;           // the temporary or input; constants come from the instruction
    uint8_t swizzle[4] = {0, 1, 2, 3};
    bool negate = false;
};

// Output registers (the OUT_ADDRESS of an o-register write).
enum Output : uint8_t { kPos = 0, kD0 = 3, kD1 = 4, kFog = 5, kPts = 6, kB0 = 7, kB1 = 8, kT0 = 9, kT1 = 10, kT2 = 11, kT3 = 12 };

struct Instruction {
    Mac mac = Mac::Nop;
    Ilu ilu = Ilu::Nop;
    Operand a, b, c;
    uint8_t constant = 0;        // c-register index, 0-191 (D3D's c[n] is constant n + 96)
    bool relative = false;       // constant indexed by a0.x
    uint8_t temp = 0;            // temporary written (MAC; the ILU writes r1 when both do)
    uint8_t mac_temp_mask = 0;   // write masks, bit 3 = x … bit 0 = w, as in the microcode
    uint8_t ilu_temp_mask = 0;
    uint8_t out_mask = 0;        // the o-register or constant write
    bool out_is_const = false;   // writes c[out_address] (else o[out_address])
    uint8_t out_address = 0;
    bool out_from_ilu = false;   // the o/c write takes the ILU's result (else the MAC's)
    bool final = false;

    // The temporary the ILU writes: r1 whenever the MAC slot holds an
    // operation (whatever the MAC writes), else the instruction's temporary.
    // (The assembler keeps r1 free for this; programs read the ILU's result
    // from r1, e.g. the screen-space 1/w after `mul oPos … + rcc`.)
    uint8_t ilu_temp() const { return mac != Mac::Nop ? 1 : temp; }
};

struct Program {
    uint32_t header = 0;  // type and size word
    std::vector<Instruction> code;
};

// Decodes a function as CreateVertexShader gets it: `words` from the header on.
// Returns nothing (with the reason) if it isn't a vertex program.
std::optional<Program> decode(const uint32_t* words, size_t count, std::string* error);
// Decodes one instruction's four words.
Instruction decode_instruction(const uint32_t w[4]);
// Disassembly in the XDK's syntax, one instruction per line (paired ILU
// operations after a "+"), for reports and for checking against sources.
std::string disassemble(const Program& p);

}  // namespace qilin::gfx::vsh
