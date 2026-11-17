// x86 (i386) instruction decoder: a thin, compact layer over Zydis.
//
// Decoded instructions are cached in a compact form (about 100 bytes each)
// rather than Zydis's full structures, so the whole binary's ~530k reachable
// instructions fit comfortably in memory.
#pragma once

#include <Zydis/Zydis.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "xbe.hpp"

namespace qilin {

struct Operand {
    enum class Kind : uint8_t { None, Reg, Mem, Imm, Ptr };
    Kind kind = Kind::None;
    uint16_t size = 0;           // bits
    ZydisRegister reg = ZYDIS_REGISTER_NONE;
    // Mem
    ZydisRegister seg = ZYDIS_REGISTER_NONE;
    ZydisRegister base = ZYDIS_REGISTER_NONE;
    ZydisRegister index = ZYDIS_REGISTER_NONE;
    uint8_t scale = 0;
    bool has_disp = false;
    int64_t disp = 0;
    // Imm
    bool imm_relative = false;
    bool imm_signed = false;
    uint64_t imm = 0;
};

struct Insn {
    uint32_t addr = 0;
    uint8_t len = 0;
    ZydisMnemonic mnem = ZYDIS_MNEMONIC_INVALID;
    ZydisInstructionAttributes attributes = 0;  // prefixes, privileged, ...
    uint8_t opcode = 0;          // primary opcode byte
    uint8_t opcode_map = 0;      // ZydisOpcodeMap (0 = default, 1 = 0F, ...)
    uint8_t modrm_reg = 0;       // ModRM.reg (opcode extension for group opcodes)
    uint8_t address_width = 32;
    ZydisISAExt isa_ext = ZYDIS_ISA_EXT_INVALID;
    uint8_t operand_width = 32;
    ZydisBranchType branch = ZYDIS_BRANCH_TYPE_NONE;
    uint8_t nops = 0;            // visible operands
    std::array<Operand, 4> op{};
    std::array<uint8_t, 15> bytes{};

    uint32_t end() const { return addr + len; }
    // Absolute target of a direct (relative) jmp/jcc/call/loop.
    std::optional<uint32_t> rel_target() const;
    bool is_ret() const { return mnem == ZYDIS_MNEMONIC_RET || mnem == ZYDIS_MNEMONIC_IRET; }
    bool is_jmp() const { return mnem == ZYDIS_MNEMONIC_JMP; }
    bool is_call() const { return mnem == ZYDIS_MNEMONIC_CALL; }
    bool is_cond_branch() const;  // jcc, jcxz/jecxz, loop*
    bool is_pad() const { return mnem == ZYDIS_MNEMONIC_INT3 || mnem == ZYDIS_MNEMONIC_NOP; }
    bool has_rep() const { return attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE); }
    bool has_repne() const { return attributes & ZYDIS_ATTRIB_HAS_REPNE; }
    bool has_lock() const { return attributes & ZYDIS_ATTRIB_HAS_LOCK; }
    // Something retail game code compiled by MSVC never contains: far control
    // flow, privileged/port I/O, undocumented aliases, BCD, segment loads,
    // 16-bit addressing. Seeing one means data is being decoded as code.
    const char* implausible_reason() const;
};

class Decoder {
public:
    explicit Decoder(const Xbe& xbe);

    // Decodes one instruction at va from the flat image. nullptr if the bytes
    // are not a valid instruction or va is unmapped. Results are cached;
    // returned pointers stay valid for the Decoder's lifetime.
    const Insn* decode(uint32_t va);

    // Intel-syntax text, for listings and cross-checks.
    std::string format(const Insn& ins) const;
    static const char* mnemonic_name(ZydisMnemonic m);

    size_t cache_size() const { return cache_.size(); }

private:
    const Xbe& xbe_;
    ZydisDecoder dec_;
    ZydisFormatter fmt_;
    std::unordered_map<uint32_t, std::optional<Insn>> cache_;
};

}  // namespace qilin
