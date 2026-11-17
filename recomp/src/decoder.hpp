// x86 (i386) instruction decoder: a thin, compact layer over Zydis.
//
// Decoded instructions are cached in a compact form (~128 bytes: no raw
// bytes, at most three visible operands) in a flat address-indexed table, so
// all ~530k reachable instructions fit in well under 100 MB and every lookup
// is a single array index.
#pragma once

#include <Zydis/Zydis.h>

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xbe.hpp"

namespace qilin {

// Register and enum fields are 16/8-bit bit-fields: Zydis's enums fit easily,
// and it keeps an operand at 32 bytes (the cache holds ~600k instructions).
struct Operand {
    enum class Kind : uint8_t { None, Reg, Mem, Imm, Ptr };
    Kind kind = Kind::None;
    uint8_t scale = 0;            // Mem
    bool has_disp = false;        // Mem
    bool imm_relative = false;    // Imm
    bool imm_signed = false;      // Imm
    uint16_t size = 0;            // bits
    ZydisRegister reg : 16 = ZYDIS_REGISTER_NONE;    // Reg
    ZydisRegister seg : 16 = ZYDIS_REGISTER_NONE;    // Mem
    ZydisRegister base : 16 = ZYDIS_REGISTER_NONE;   // Mem
    ZydisRegister index : 16 = ZYDIS_REGISTER_NONE;  // Mem
    int64_t disp = 0;             // Mem
    uint64_t imm = 0;             // Imm (sign-extended to 64 bits when signed), Ptr (seg:off)

    bool is_reg(ZydisRegister r) const { return kind == Kind::Reg && reg == r; }
};
static_assert(ZYDIS_REGISTER_MAX_VALUE < (1 << 15) && ZYDIS_MNEMONIC_MAX_VALUE < (1 << 15) &&
              ZYDIS_ISA_EXT_MAX_VALUE < (1 << 7) && ZYDIS_BRANCH_TYPE_MAX_VALUE < (1 << 7),
              "Zydis enums no longer fit the compact bit-fields");

struct Insn {
    static constexpr int kMaxOperands = 3;  // the most any non-VEX i386 instruction has

    uint32_t addr = 0;
    uint8_t len = 0;
    uint8_t nops = 0;             // visible operands stored in op[]
    bool too_many_operands = false;
    uint8_t opcode = 0;           // primary opcode byte
    uint8_t opcode_map = 0;       // ZydisOpcodeMap (0 = default, 1 = 0F, ...)
    uint8_t modrm_reg = 0;        // ModRM.reg (opcode extension for group opcodes)
    uint8_t address_width = 32;
    uint8_t operand_width = 32;
    ZydisMnemonic mnem : 16 = ZYDIS_MNEMONIC_INVALID;
    ZydisBranchType branch : 8 = ZYDIS_BRANCH_TYPE_NONE;
    ZydisISAExt isa_ext : 8 = ZYDIS_ISA_EXT_INVALID;
    ZydisInstructionAttributes attributes = 0;  // prefixes, privileged, ...
    // Status flags per Zydis, as CF=1 PF=2 AF=4 ZF=8 SF=16 OF=32: read,
    // overwritten (any value), overwritten with a defined value.
    uint8_t flags_read = 0, flags_written = 0, flags_defined = 0;
    std::array<Operand, kMaxOperands> op{};

    uint32_t end() const { return addr + len; }
    // Absolute target of a direct (relative) jmp/jcc/call/loop.
    std::optional<uint32_t> rel_target() const;
    bool is_ret() const { return mnem == ZYDIS_MNEMONIC_RET || mnem == ZYDIS_MNEMONIC_IRET; }
    bool is_jmp() const { return mnem == ZYDIS_MNEMONIC_JMP; }
    bool is_call() const { return mnem == ZYDIS_MNEMONIC_CALL; }
    bool is_cond_branch() const;  // jcc, jcxz/jecxz, loop*
    bool is_pad() const { return mnem == ZYDIS_MNEMONIC_INT3 || mnem == ZYDIS_MNEMONIC_NOP; }
    // F3 (rep/repe) and, for stos/movs/lods/ins/outs, F2 too: there it also means rep.
    bool has_rep() const {
        return attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
    }
    bool has_repne() const { return attributes & ZYDIS_ATTRIB_HAS_REPNE; }
    bool has_lock() const { return attributes & ZYDIS_ATTRIB_HAS_LOCK; }
    // Something retail game code compiled by MSVC never contains: far control
    // flow, privileged/port I/O, undocumented aliases, BCD, segment loads,
    // 16-bit addressing, instruction sets the Pentium III lacks. Seeing one
    // means data is being decoded as code. nullptr if plausible.
    const char* implausible_reason() const;
};

class Decoder {
public:
    explicit Decoder(const Xbe& xbe);
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Decodes one instruction at va from the flat image. nullptr if the bytes
    // are not a valid instruction or va is unmapped. Results are cached;
    // returned pointers stay valid for the Decoder's lifetime.
    const Insn* decode(uint32_t va);
    // Same, without touching the cache (for one-off sweeps over data/junk).
    bool decode_uncached(uint32_t va, Insn& out) const;

    // The instruction's raw bytes, from the image.
    std::span<const uint8_t> bytes(const Insn& ins) const;
    // Intel-syntax text, for listings and diagnostics.
    std::string format(const Insn& ins) const;
    static const char* mnemonic_name(ZydisMnemonic m);

    size_t cache_size() const { return insns_.size(); }

private:
    static constexpr uint32_t kUnknown = 0, kInvalid = 1;  // slot values; n >= 2 -> insns_[n - 2]
    uint32_t* slot(uint32_t va);

    const Xbe& xbe_;
    ZydisDecoder dec_;
    ZydisFormatter fmt_;
    uint32_t lo_ = 0, hi_ = 0;                        // address range covered by table_
    std::vector<uint32_t> table_;                     // one slot per byte of the code range
    std::unordered_map<uint32_t, uint32_t> outside_;  // slots for addresses outside it
    std::deque<Insn> insns_;                          // stable addresses
};

}  // namespace qilin
