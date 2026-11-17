#include "decoder.hpp"

#include <algorithm>
#include <limits>

namespace qilin {

std::optional<uint32_t> Insn::rel_target() const {
    if (nops == 0 || op[0].kind != Operand::Kind::Imm || !op[0].imm_relative) return std::nullopt;
    return uint32_t(end() + uint32_t(op[0].imm));
}

bool Insn::is_cond_branch() const {
    switch (mnem) {
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JCXZ:
        case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JLE:
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_JNL:
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JNP:
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_JO:
        case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_JZ:
        case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
            return true;
        default:
            return false;
    }
}

const char* Insn::implausible_reason() const {
    if (too_many_operands) return "more than three operands (VEX/EVEX)";
    if (branch == ZYDIS_BRANCH_TYPE_FAR) return "far control transfer";
    if (attributes & ZYDIS_ATTRIB_IS_PRIVILEGED) return "privileged instruction";
    if (address_width == 16) return "16-bit addressing";
    switch (mnem) {
        case ZYDIS_MNEMONIC_INT1: case ZYDIS_MNEMONIC_INTO: case ZYDIS_MNEMONIC_INT:
        case ZYDIS_MNEMONIC_IRET: case ZYDIS_MNEMONIC_IRETD:
            return "interrupt instruction";
        case ZYDIS_MNEMONIC_BOUND: case ZYDIS_MNEMONIC_ARPL: case ZYDIS_MNEMONIC_SALC:
            return "obsolete instruction";
        case ZYDIS_MNEMONIC_IN: case ZYDIS_MNEMONIC_OUT: case ZYDIS_MNEMONIC_INSB: case ZYDIS_MNEMONIC_INSW:
        case ZYDIS_MNEMONIC_INSD: case ZYDIS_MNEMONIC_OUTSB: case ZYDIS_MNEMONIC_OUTSW: case ZYDIS_MNEMONIC_OUTSD:
            return "port I/O";
        case ZYDIS_MNEMONIC_AAA: case ZYDIS_MNEMONIC_AAD: case ZYDIS_MNEMONIC_AAM: case ZYDIS_MNEMONIC_AAS:
        case ZYDIS_MNEMONIC_DAA: case ZYDIS_MNEMONIC_DAS:
            return "BCD arithmetic";
        case ZYDIS_MNEMONIC_LDS: case ZYDIS_MNEMONIC_LES: case ZYDIS_MNEMONIC_LFS: case ZYDIS_MNEMONIC_LGS:
        case ZYDIS_MNEMONIC_LSS:
            return "far pointer load";
        case ZYDIS_MNEMONIC_STI: case ZYDIS_MNEMONIC_CLI:
            return "interrupt flag change";
        default:
            break;
    }
    // Writes to a segment register (mov sreg, pop sreg).
    if (nops >= 1 && op[0].kind == Operand::Kind::Reg && op[0].reg >= ZYDIS_REGISTER_ES &&
        op[0].reg <= ZYDIS_REGISTER_GS && (mnem == ZYDIS_MNEMONIC_MOV || mnem == ZYDIS_MNEMONIC_POP))
        return "segment register write";
    // Undocumented shift alias: C0/C1/D0-D3 with ModRM.reg = 6 (SAL /6).
    if (opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && (opcode == 0xC0 || opcode == 0xC1 || (opcode >= 0xD0 && opcode <= 0xD3)) &&
        modrm_reg == 6)
        return "undocumented SAL /6";
    // Undocumented TEST alias: F6/F7 /1.
    if (opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && (opcode == 0xF6 || opcode == 0xF7) && modrm_reg == 1)
        return "undocumented TEST /1";
    // The Xbox CPU is a Pentium III: base, x87, MMX and SSE only.
    switch (isa_ext) {
        case ZYDIS_ISA_EXT_BASE: case ZYDIS_ISA_EXT_X87: case ZYDIS_ISA_EXT_MMX: case ZYDIS_ISA_EXT_SSE:
        case ZYDIS_ISA_EXT_PAUSE:
            return nullptr;
        default:
            return "instruction set the Pentium III lacks";
    }
}

Decoder::Decoder(const Xbe& xbe) : xbe_(xbe) {
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatterInit(&fmt_, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&fmt_, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_TRUE);
    // One slot per byte of the address range spanned by the code sections.
    lo_ = std::numeric_limits<uint32_t>::max();
    for (auto& s : xbe.sections)
        if (s.code && s.rsize) {
            lo_ = std::min(lo_, s.va);
            hi_ = std::max(hi_, s.file_end());
        }
    if (lo_ < hi_) table_.assign(hi_ - lo_, kUnknown);
    else lo_ = hi_ = 0;
}

uint32_t* Decoder::slot(uint32_t va) {
    if (va >= lo_ && va < hi_) return &table_[va - lo_];
    return &outside_[va];  // value-initialised to kUnknown
}

bool Decoder::decode_uncached(uint32_t va, Insn& ins) const {
    if (!xbe_.mapped(va)) return false;
    auto img = xbe_.image();
    const size_t off = va - xbe_.base;
    const size_t avail = std::min<size_t>(ZYDIS_MAX_INSTRUCTION_LENGTH, img.size() - off);

    ZydisDecodedInstruction zi;
    ZydisDecodedOperand zo[ZYDIS_MAX_OPERAND_COUNT];
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec_, img.data() + off, avail, &zi, zo))) return false;

    ins = Insn{};
    ins.addr = va;
    ins.len = zi.length;
    ins.mnem = zi.mnemonic;
    ins.attributes = zi.attributes;
    ins.opcode = zi.opcode;
    ins.opcode_map = uint8_t(zi.opcode_map);
    ins.modrm_reg = zi.raw.modrm.reg;
    ins.address_width = zi.address_width;
    ins.operand_width = zi.operand_width;
    ins.isa_ext = zi.meta.isa_ext;
    ins.branch = zi.meta.branch_type;
    ins.too_many_operands = zi.operand_count_visible > Insn::kMaxOperands;
    ins.nops = uint8_t(std::min<int>(zi.operand_count_visible, Insn::kMaxOperands));
    for (uint8_t i = 0; i < ins.nops; ++i) {
        const ZydisDecodedOperand& s = zo[i];
        Operand& d = ins.op[i];
        d.size = s.size;
        switch (s.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                d.kind = Operand::Kind::Reg;
                d.reg = s.reg.value;
                break;
            case ZYDIS_OPERAND_TYPE_MEMORY:
                d.kind = Operand::Kind::Mem;
                d.seg = s.mem.segment;
                d.base = s.mem.base;
                d.index = s.mem.index;
                d.scale = s.mem.scale;
                d.has_disp = s.mem.disp.has_displacement;
                d.disp = s.mem.disp.value;
                break;
            case ZYDIS_OPERAND_TYPE_IMMEDIATE:
                d.kind = Operand::Kind::Imm;
                d.imm_relative = s.imm.is_relative;
                d.imm_signed = s.imm.is_signed;
                d.imm = s.imm.value.u;
                break;
            case ZYDIS_OPERAND_TYPE_POINTER:
                d.kind = Operand::Kind::Ptr;
                d.imm = (uint64_t(s.ptr.segment) << 32) | s.ptr.offset;
                break;
            default:
                break;
        }
    }
    return true;
}

const Insn* Decoder::decode(uint32_t va) {
    uint32_t* s = slot(va);
    if (*s == kInvalid) return nullptr;
    if (*s != kUnknown) return &insns_[*s - 2];
    Insn ins;
    if (!decode_uncached(va, ins)) {
        *s = kInvalid;
        return nullptr;
    }
    insns_.push_back(ins);
    *s = uint32_t(insns_.size() + 1);  // index + 2
    return &insns_.back();
}

std::span<const uint8_t> Decoder::bytes(const Insn& ins) const {
    return xbe_.image().subspan(ins.addr - xbe_.base, ins.len);
}

std::string Decoder::format(const Insn& ins) const {
    // Re-decode from the image: the compact form is not enough for the formatter.
    ZydisDecodedInstruction zi;
    ZydisDecodedOperand zo[ZYDIS_MAX_OPERAND_COUNT];
    auto b = bytes(ins);
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec_, b.data(), b.size(), &zi, zo))) return "(bad)";
    char buf[256];
    if (ZYAN_FAILED(ZydisFormatterFormatInstruction(&fmt_, &zi, zo, zi.operand_count_visible, buf, sizeof buf,
                                                    ins.addr, nullptr)))
        return "(format error)";
    return buf;
}

const char* Decoder::mnemonic_name(ZydisMnemonic m) {
    const char* s = ZydisMnemonicGetString(m);
    return s ? s : "?";
}

}  // namespace qilin

namespace qilin {
static_assert(sizeof(Operand) == 32, "Operand grew: check the bit-field packing");
}  // namespace qilin
