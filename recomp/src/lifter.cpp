#include "lifter.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <format>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qilin {

std::string LiftFailure::message() const {
    if (insn.empty()) return std::format("{:08X}: {}", addr, detail);
    return std::format("{:08X} {}: {}", addr, insn, detail);
}

std::string LiftFailure::category() const {
    switch (kind) {
        case Kind::Fragment: return "not a whole function: " + detail;
        case Kind::Unsupported: return std::format("instruction not supported yet [{}]", mnemonic);
        case Kind::X87: return std::format("x87 not supported yet (slice 3) [{}]", mnemonic);
        case Kind::UndefinedFlag: return "reads a flag left undefined";
        case Kind::Implausible: return "implausible instruction: " + detail;
        case Kind::BadControlFlow: return "bad control flow: " + detail;
    }
    std::unreachable();
}

namespace {

// ------------------------------------------------------------------ flags
enum : uint8_t { CF = 1, PF = 2, AF = 4, ZF = 8, SF = 16, OF = 32, ALLF = 63 };

// Thrown inside the emitter, turned into std::unexpected by Lifter::lift.
struct Abort {
    LiftFailure f;
};

// Condition codes shared by jcc / setcc / cmovcc.
struct Cond {
    uint8_t use;
    const char* expr;
};

std::optional<Cond> cond_of(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO: return Cond{OF, "of"};
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_CMOVNO: return Cond{OF, "!of"};
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: return Cond{CF, "cf"};
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: return Cond{CF, "!cf"};
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return Cond{ZF, "zf"};
        case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return Cond{ZF, "!zf"};
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return Cond{CF | ZF, "(cf || zf)"};
        case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return Cond{CF | ZF, "(!cf && !zf)"};
        case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return Cond{SF, "sf"};
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return Cond{SF, "!sf"};
        case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP: return Cond{PF, "pf"};
        case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_CMOVNP: return Cond{PF, "!pf"};
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return Cond{SF | OF, "sf != of"};
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return Cond{SF | OF, "sf == of"};
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return Cond{ZF | SF | OF, "(zf || sf != of)"};
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return Cond{ZF | SF | OF, "(!zf && sf == of)"};
        default: return std::nullopt;
    }
}

bool is_setcc(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETNB:
        case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE:
        case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_SETNP:
        case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_SETNLE:
            return true;
        default:
            return false;
    }
}

bool is_shift(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_SHL || m == ZYDIS_MNEMONIC_SHR || m == ZYDIS_MNEMONIC_SAR;
}

// Flag effects: `use` = read, `kill` = definitely overwritten (for liveness),
// `defined` = overwritten with an architecturally defined value. Reading a
// flag that was killed but not defined (an "undefined" flag) is an error.
struct FlagFx {
    uint8_t use = 0, kill = 0, defined = 0;
};

bool shift_by_cl(const Insn& i) { return i.nops >= 2 && i.op[1].kind == Operand::Kind::Reg; }
bool is_double_shift(ZydisMnemonic m) { return m == ZYDIS_MNEMONIC_SHLD || m == ZYDIS_MNEMONIC_SHRD; }
bool dshift_by_cl(const Insn& i) { return i.nops >= 3 && i.op[2].kind == Operand::Kind::Reg; }
uint32_t shift_imm(const Insn& i) { return i.nops >= 2 ? uint32_t(i.op[1].imm & 31) : 1; }

FlagFx flag_fx(const Insn& i) {
    const ZydisMnemonic m = i.mnem;
    if (auto c = cond_of(m)) return {c->use, 0, 0};
    switch (m) {
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_NEG:
            return {0, ALLF, ALLF};
        case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SBB:
            return {CF, ALLF, ALLF};
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC:
            return {0, ALLF & ~CF, ALLF & ~CF};
        case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST:
            return {0, ALLF, ALLF & ~AF};
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            if (shift_by_cl(i)) return {0, 0, 0};  // the count may be 0: flags then pass through
            const uint32_t n = shift_imm(i);
            if (n == 0) return {0, 0, 0};
            uint8_t def = CF | ZF | SF | PF;
            if (n == 1) def |= OF;  // OF is only defined for 1-bit shifts (SAR: cleared)
            return {0, ALLF, def};
        }
        case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_MUL:
            return {0, ALLF, CF | OF};
        case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV:
            return {0, ALLF, 0};
        case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS:
            return {0, ALLF, ALLF};
        case ZYDIS_MNEMONIC_LAHF:
            return {SF | ZF | AF | PF | CF, 0, 0};
        case ZYDIS_MNEMONIC_SAHF:
            return {0, SF | ZF | AF | PF | CF, SF | ZF | AF | PF | CF};
        case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CLC:
            return {0, CF, CF};
        case ZYDIS_MNEMONIC_CMC:
            return {CF, CF, CF};
        case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD: {
            if (dshift_by_cl(i)) return {0, 0, 0};  // the count may be 0: flags then pass through
            const uint32_t n = i.nops >= 3 ? uint32_t(i.op[2].imm & 31) : 0;
            if (n == 0) return {0, 0, 0};
            return {0, ALLF, uint8_t(CF | ZF | SF | PF | (n == 1 ? OF : 0))};
        }
        case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR:
            return {0, ALLF, ZF};
        case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR:
            if (!shift_by_cl(i) && shift_imm(i) == 1) return {CF, CF | OF, CF | OF};
            return {i.flags_read, 0, 0};  // other counts aren't lifted
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
            // rep: with ecx = 0 nothing runs and the flags pass through.
            return i.has_rep() ? FlagFx{0, 0, 0} : FlagFx{0, ALLF, ALLF};
        case ZYDIS_MNEMONIC_CALL:
            // The callee's flags aren't modelled: any flag read after a call
            // (before a write) is reported, never guessed.
            return {0, ALLF, 0};
        case ZYDIS_MNEMONIC_PUSHF: case ZYDIS_MNEMONIC_PUSHFD:
            // Copies every flag, defined or not, to the stack (CPUID probes):
            // rejected as unsupported, not as a read of an undefined flag.
            return {};
        default:
            // Everything else (fcomi/fucomi, bt*, bsf/bsr, shld/shrd, rotates,
            // cmpxchg, xadd, scas/cmps, popfd, ...): Zydis' flag table. Variable
            // counts that may be zero leave flags untouched, so for those only
            // the read side is trusted.
            if (m == ZYDIS_MNEMONIC_ROL || m == ZYDIS_MNEMONIC_ROR || m == ZYDIS_MNEMONIC_RCL || m == ZYDIS_MNEMONIC_RCR ||
                m == ZYDIS_MNEMONIC_SHLD || m == ZYDIS_MNEMONIC_SHRD)
                return {i.flags_read, 0, 0};
            if (i.has_rep()) return {i.flags_read, 0, 0};  // rep with ecx = 0 executes nothing
            return {i.flags_read, i.flags_written, i.flags_defined};
    }
}

// ------------------------------------------------------------------ registers
constexpr std::array<const char*, 8> kGpr = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
// x87 control state, held in locals like the general registers.
constexpr std::array<const char*, 4> kX87 = {"ftop", "ftag", "fpcw", "fpsw"};
enum { FTOP, FTAG, FPCW, FPSW };
enum Gpr : int { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

struct RegInfo {
    int gpr;     // index into kGpr
    int width;   // 8, 16, 32
    bool high8;  // ah/ch/dh/bh
    const char* name() const { return kGpr[size_t(gpr)]; }
};

std::optional<RegInfo> gpr(ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI) return RegInfo{r - ZYDIS_REGISTER_EAX, 32, false};
    if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) return RegInfo{r - ZYDIS_REGISTER_AX, 16, false};
    switch (r) {
        case ZYDIS_REGISTER_AL: return RegInfo{EAX, 8, false};
        case ZYDIS_REGISTER_CL: return RegInfo{ECX, 8, false};
        case ZYDIS_REGISTER_DL: return RegInfo{EDX, 8, false};
        case ZYDIS_REGISTER_BL: return RegInfo{EBX, 8, false};
        case ZYDIS_REGISTER_AH: return RegInfo{EAX, 8, true};
        case ZYDIS_REGISTER_CH: return RegInfo{ECX, 8, true};
        case ZYDIS_REGISTER_DH: return RegInfo{EDX, 8, true};
        case ZYDIS_REGISTER_BH: return RegInfo{EBX, 8, true};
        default: return std::nullopt;
    }
}

int mm_index(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7 ? r - ZYDIS_REGISTER_MM0 : -1;
}

int xmm_index(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM7 ? r - ZYDIS_REGISTER_XMM0 : -1;
}

constexpr uint32_t mask_of(int w) { return w == 32 ? 0xFFFFFFFFu : (1u << w) - 1; }
constexpr uint32_t sign_of(int w) { return 1u << (w - 1); }
// "(expr)" masked to width w; no mask needed for 32-bit uint32_t arithmetic.
std::string masked(std::string_view e, int w) {
    return w == 32 ? std::format("({})", e) : std::format("(({}) & 0x{:X}u)", e, mask_of(w));
}

// esp/ebp as offsets from esp at entry, for the stack-balance check.
constexpr int64_t kStackUnknown = INT64_MIN;
struct StackState {
    int64_t esp = kStackUnknown, ebp = kStackUnknown;
    bool seen = false;
};

// ------------------------------------------------------------------ emitter
class FnEmitter {
public:
    FnEmitter(Decoder& d, const Function& f, const Conventions* conv) : dec_(d), f_(f), conv_(conv) {}
    std::string run();

private:
    uint8_t flags_in(uint32_t a) const { return conv_ && conv_->flags_in.contains(a) ? conv_->flags_in.at(a) : 0; }
    uint8_t flags_out(uint32_t a) const { return conv_ && conv_->flags_out.contains(a) ? conv_->flags_out.at(a) : 0; }
    // flag_fx plus the conventions: a ret passes flags_out on, a call or tail
    // call passes the callee's flags_in and receives its flags_out.
    FlagFx fx_of(const Insn& i) const {
        FlagFx fx = flag_fx(i);
        if (i.is_ret()) fx.use |= flags_out(f_.start);
        const auto t = i.rel_target();
        if (t && (i.is_call() || ((i.is_jmp() || i.is_cond_branch()) && !idx_.contains(*t)))) fx.use |= flags_in(*t);
        if (t && i.is_call()) fx.defined |= flags_out(*t);
        return fx;
    }
    // c.eflags <-> flag locals, for a mask.
    static std::string pack_flags(uint8_t mask);
    static std::string unpack_flags(uint8_t mask);

    [[noreturn]] void fail(const Insn& i, LiftFailure::Kind k, std::string detail) const {
        throw Abort{{k, i.addr, dec_.format(i), Decoder::mnemonic_name(i.mnem), std::move(detail)}};
    }
    [[noreturn]] void unsupported(const Insn& i, std::string detail) const {
        fail(i, LiftFailure::Kind::Unsupported, std::move(detail));
    }

    // operand helpers
    std::string addr(const Insn& i, const Operand& o);
    std::optional<uint32_t> mmio_register(const Insn& i, const Operand& o, int w);
    std::string read(const Insn& i, const Operand& o, int w);
    std::string write(const Insn& i, const Operand& o, int w, std::string_view v);
    std::string xmm_lane0(const Insn& i, const Operand& o);  // 32-bit source: xmm lane 0 or m32
    std::string xmm128(const Insn& i, const Operand& o, bool aligned);
    int xmm_dst(const Insn& i, const Operand& o);             // destination xmm index (marks it written)
    int width(const Insn& i, const Operand& o) const;

    // Register bookkeeping: `use` = referenced (declared in the prologue),
    // `mark` = modified (also stored back at every exit). An unrecorded
    // reference is a compile error in the generated code, never a silent bug.
    void use(int g) { used_ |= 1u << g; }
    void use_xmm(int x) { used_ |= 1u << (8 + x); }
    void mark(int g) { use(g); written_ |= 1u << g; }
    void mark_xmm(int x) { use_xmm(x); written_ |= 1u << (8 + x); }
    // x87 control state held in locals: bit 16 + k of used_/written_ for kX87[k].
    void use_fx(int k) { used_ |= 1u << (16 + k); }
    void mark_fx(int k) { use_fx(k); written_ |= 1u << (16 + k); }

    void line(std::string_view s) {
        out_ += "    ";
        out_ += s;
        out_ += '\n';
    }
    void emit(const Insn& i, uint8_t live_out);
    bool emit_x87(const Insn& i, uint8_t need);
    int st_index(const Insn& i, const Operand& o);
    std::string fsrc(const Insn& i, const Operand& o);  // x87 source operand as a double
    void fpush(const std::string& v);
    void fpop();
    // Direction flag at instruction i (0 or 1; unknown is an error).
    int direction(const Insn& i) const {
        if (df_now_ > 1) unsupported(i, "direction flag not known statically");
        return df_now_;
    }
    uint8_t df_now_ = 0;
    // Backward branches (loops) tick in test builds, so a harness can stop a runaway loop.
    static std::string tick(const Insn& i, uint32_t t) { return t <= i.addr ? "QILIN_TICK(); " : ""; }
    void emit_szp(uint8_t need, int w);
    void check_stack_balance(const std::vector<std::vector<size_t>>& succ);

    Decoder& dec_;
    const Function& f_;
    const Conventions* conv_;
    std::vector<const Insn*> ins_;
    std::unordered_map<uint32_t, size_t> idx_;
    uint32_t used_ = 0;     // bit g: GPR g referenced; bit 8+x: xmm x; bit 16+k: x87 kX87[k]
    uint32_t written_ = 0;  // same bits: modified
    bool x87_ = false;      // uses the x87 register stack (needs ST())
    bool calls_ = false;    // has a call that returns here (needs reload())
    std::string out_;
};

int FnEmitter::width(const Insn& i, const Operand& o) const {
    const int w = o.size;
    if (w != 8 && w != 16 && w != 32) unsupported(i, std::format("operand width {}", w));
    return w;
}

// fs: is the thread information block (on the Xbox, the KPCR's NT_TIB):
// fs:[0] is the head of the thread's exception-registration chain, which
// functions with C++ exception handling link and unlink. It lives in guest
// memory at c.fs_base, so the chain is kept exactly as the original keeps it.
// A device register: an absolute 32-bit access in the register space.
// Anything else there (a different width, or an address computed from
// registers) isn't modelled and fails the lift.
std::optional<uint32_t> FnEmitter::mmio_register(const Insn& i, const Operand& o, int w) {
    const uint32_t d = uint32_t(o.disp);
    const bool absolute = o.base == ZYDIS_REGISTER_NONE && o.index == ZYDIS_REGISTER_NONE && o.seg != ZYDIS_REGISTER_FS;
    constexpr uint32_t kMmioLo = 0xFD000000u, kMmioHi = 0xFF000000u;  // as rt::kMmioLo/kMmioHi in guest.hpp
    if (!absolute || d < kMmioLo || d >= kMmioHi) return std::nullopt;
    if (w != 32 || (d & 3)) unsupported(i, "a device register access that isn't an aligned 32-bit load or store");
    return d;
}

std::string FnEmitter::addr(const Insn& i, const Operand& o) {
    const bool fs = o.seg == ZYDIS_REGISTER_FS;
    if (!fs && o.seg != ZYDIS_REGISTER_DS && o.seg != ZYDIS_REGISTER_SS && o.seg != ZYDIS_REGISTER_ES &&
        o.seg != ZYDIS_REGISTER_NONE)
        unsupported(i, "segment override (gs:) not supported");
    std::string e = fs ? "c.fs_base" : "";
    if (o.base != ZYDIS_REGISTER_NONE) {
        auto r = gpr(o.base);
        if (!r || r->width != 32) unsupported(i, "base register");
        use(r->gpr);
        if (!e.empty()) e += " + ";
        e += r->name();
    }
    if (o.index != ZYDIS_REGISTER_NONE) {
        auto r = gpr(o.index);
        if (!r || r->width != 32) unsupported(i, "index register");
        use(r->gpr);
        if (!e.empty()) e += " + ";
        e += r->name();
        if (o.scale > 1) e += std::format(" * {}u", o.scale);
    }
    const uint32_t d = uint32_t(o.disp);
    if (d || e.empty()) {
        if (!e.empty()) e += " + ";
        e += std::format("0x{:X}u", d);
    }
    return std::format("uint32_t({})", e);
}

std::string FnEmitter::read(const Insn& i, const Operand& o, int w) {
    switch (o.kind) {
        case Operand::Kind::Reg: {
            auto r = gpr(o.reg);
            if (!r) unsupported(i, "register operand");
            use(r->gpr);
            if (r->width == 32) return r->name();
            if (r->width == 16) return std::format("({} & 0xFFFFu)", r->name());
            if (r->high8) return std::format("(({} >> 8) & 0xFFu)", r->name());
            return std::format("({} & 0xFFu)", r->name());
        }
        case Operand::Kind::Mem: {
            if (const auto r = mmio_register(i, o, w)) return std::format("rt::mmio_read32(0x{:X}u, 0x{:X}u)", *r, i.addr);
            const std::string a = addr(i, o);
            if (w == 8) return std::format("uint32_t(rt::ld8(m, {}, 0x{:X}u))", a, i.addr);
            if (w == 16) return std::format("uint32_t(rt::ld16(m, {}, 0x{:X}u))", a, i.addr);
            if (w == 32) return std::format("rt::ld32(m, {}, 0x{:X}u)", a, i.addr);
            unsupported(i, "memory operand width");
        }
        case Operand::Kind::Imm:
            return std::format("0x{:X}u", uint32_t(o.imm) & mask_of(w));
        default:
            unsupported(i, "operand kind");
    }
}

std::string FnEmitter::write(const Insn& i, const Operand& o, int w, std::string_view v) {
    switch (o.kind) {
        case Operand::Kind::Reg: {
            auto r = gpr(o.reg);
            if (!r) unsupported(i, "destination register");
            mark(r->gpr);
            if (r->width == 32) return std::format("{} = {};", r->name(), v);
            if (r->width == 16) return std::format("rt::set_lo16({}, {});", r->name(), v);
            if (r->high8) return std::format("rt::set_hi8({}, {});", r->name(), v);
            return std::format("rt::set_lo8({}, {});", r->name(), v);
        }
        case Operand::Kind::Mem: {
            if (const auto r = mmio_register(i, o, w)) return std::format("rt::mmio_write32(0x{:X}u, {}, 0x{:X}u);", *r, v, i.addr);
            const std::string a = addr(i, o);
            if (w == 8) return std::format("rt::st8(m, {}, uint8_t({}), 0x{:X}u);", a, v, i.addr);
            if (w == 16) return std::format("rt::st16(m, {}, uint16_t({}), 0x{:X}u);", a, v, i.addr);
            if (w == 32) return std::format("rt::st32(m, {}, {}, 0x{:X}u);", a, v, i.addr);
            unsupported(i, "memory operand width");
        }
        default:
            unsupported(i, "destination");
    }
}

std::string FnEmitter::xmm_lane0(const Insn& i, const Operand& o) {
    if (o.kind == Operand::Kind::Reg) {
        const int x = xmm_index(o.reg);
        if (x < 0) unsupported(i, "expected an xmm register");
        use_xmm(x);
        return std::format("xmm{}.u[0]", x);
    }
    if (o.kind == Operand::Kind::Mem) return std::format("rt::ld32(m, {}, 0x{:X}u)", addr(i, o), i.addr);
    unsupported(i, "SSE operand");
}

std::string FnEmitter::xmm128(const Insn& i, const Operand& o, bool aligned) {
    if (o.kind == Operand::Kind::Reg) {
        const int x = xmm_index(o.reg);
        if (x < 0) unsupported(i, "expected an xmm register");
        use_xmm(x);
        return std::format("xmm{}", x);
    }
    if (o.kind == Operand::Kind::Mem)
        return std::format("rt::ld128(m, {}, {}, 0x{:X}u)", addr(i, o), aligned, i.addr);
    unsupported(i, "SSE operand");
}

int FnEmitter::xmm_dst(const Insn& i, const Operand& o) {
    const int x = o.kind == Operand::Kind::Reg ? xmm_index(o.reg) : -1;
    if (x < 0) unsupported(i, "expected an xmm destination");
    mark_xmm(x);
    return x;
}

void FnEmitter::emit_szp(uint8_t need, int w) {
    if (need & ZF) line("zf = r == 0;");
    if (need & SF) line(std::format("sf = (r & 0x{:X}u) != 0;", sign_of(w)));
    if (need & PF) line("pf = rt::parity8(r);");
}

void FnEmitter::emit(const Insn& i, uint8_t live_out) {
    const FlagFx fx = fx_of(i);
    uint8_t need = live_out & fx.kill;
    if (need & ~fx.defined)
        fail(i, LiftFailure::Kind::UndefinedFlag,
             i.is_call() ? "a flag is read after this call before being written (the callee's flags aren't modelled)"
                         : "a later instruction reads a flag this one leaves undefined");
    if ((is_shift(i.mnem) && shift_by_cl(i)) || (is_double_shift(i.mnem) && dshift_by_cl(i))) {
        // Count in cl may be 0 (flags unchanged) or > 1 (OF undefined): only CF/ZF/SF/PF are safe to model.
        if (live_out & (OF | AF)) fail(i, LiftFailure::Kind::UndefinedFlag, "OF/AF read after a shift by cl");
        need = live_out & (CF | ZF | SF | PF);
    }

    const Operand& d = i.op[0];
    const Operand& s = i.op[1];
    const ZydisMnemonic M = i.mnem;

    if (auto c = cond_of(M)) {
        if (i.is_cond_branch()) {
            const uint32_t t = *i.rel_target();
            if (idx_.contains(t)) line(std::format("if ({}) {{ {}goto L_{:08X}; }}", c->expr, tick(i, t), t));
            else line(std::format("if ({}) {{ QILIN_TAIL(); writeback(); {}{}(c); return; }}  // conditional tail call", c->expr,
                                  flags_in(t) ? pack_flags(flags_in(t)) + " " : std::string(), Lifter::name(t)));
            return;
        }
        if (is_setcc(M)) {
            line(write(i, d, 8, std::format("uint32_t({})", c->expr)));
            return;
        }
        // cmovcc: the source is read (and can fault) even when the condition is false.
        const int w = width(i, d);
        if (w != 32) unsupported(i, "16-bit cmov");
        line(std::format("{{ uint32_t v = {}; if ({}) {{ {} }} }}", read(i, s, w), c->expr, write(i, d, w, "v")));
        return;
    }

    switch (M) {
        case ZYDIS_MNEMONIC_NOP:
        case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0: case ZYDIS_MNEMONIC_PREFETCHT1:
        case ZYDIS_MNEMONIC_PREFETCHT2:
            return;
        case ZYDIS_MNEMONIC_INT3:
            line(std::format("rt::trap(0x{:X}u, \"int3\");", i.addr));
            return;
        case ZYDIS_MNEMONIC_MOV: {
            if (d.kind == Operand::Kind::Reg && s.kind == Operand::Kind::Reg && d.reg == s.reg) return;  // mov edi, edi
            const int w = width(i, d);
            line(write(i, d, w, read(i, s, w)));
            return;
        }
        case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: {
            const int wd = width(i, d), ws = width(i, s);
            std::string v = read(i, s, ws);
            if (M == ZYDIS_MNEMONIC_MOVSX)
                v = std::format("uint32_t(int32_t(int{}_t({})))", ws, v);
            line(write(i, d, wd, masked(v, wd)));
            return;
        }
        case ZYDIS_MNEMONIC_LEA: {
            if (width(i, d) != 32) unsupported(i, "16-bit lea");
            if (s.seg == ZYDIS_REGISTER_FS) unsupported(i, "lea with fs:");  // lea ignores the segment base
            line(write(i, d, 32, addr(i, s)));
            return;
        }
        case ZYDIS_MNEMONIC_XCHG: {
            const int w = width(i, d);
            line(std::format("{{ uint32_t a = {}, b = {}; {} {} }}", read(i, d, w), read(i, s, w), write(i, d, w, "b"),
                             write(i, s, w, "a")));
            return;
        }
        case ZYDIS_MNEMONIC_XADD: {
            // xadd d, s: d = d + s and s = the old d, flags as add. (One
            // guest processor runs at a time, so lock adds nothing.)
            const int w = width(i, d);
            const uint32_t sg = sign_of(w);
            line("{");
            line(std::format("uint32_t a = {}, b = {};", read(i, d, w), read(i, s, w)));
            line(std::format("[[maybe_unused]] uint32_t r = {};", masked("a + b", w)));
            if (need & CF) line("cf = r < a;");
            if (need & OF) line(std::format("of = ((a ^ r) & (b ^ r) & 0x{:X}u) != 0;", sg));
            if (need & AF) line("af = ((a ^ b ^ r) & 0x10u) != 0;");
            emit_szp(need, w);
            line(write(i, s, w, "a"));
            line(write(i, d, w, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_ADC:
        case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
        case ZYDIS_MNEMONIC_TEST: {
            const int w = width(i, d);
            const uint32_t sg = sign_of(w);
            // xor r, r / sub r, r: the result and flags are constant (common idiom).
            const bool same = d.kind == Operand::Kind::Reg && s.kind == Operand::Kind::Reg && d.reg == s.reg &&
                              (M == ZYDIS_MNEMONIC_XOR || M == ZYDIS_MNEMONIC_SUB);
            const char* expr = nullptr;
            switch (M) {
                case ZYDIS_MNEMONIC_ADD: expr = "a + b"; break;
                case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: expr = "a - b"; break;
                case ZYDIS_MNEMONIC_ADC: expr = "a + b + uint32_t(cf)"; break;
                case ZYDIS_MNEMONIC_SBB: expr = "a - b - uint32_t(cf)"; break;
                case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_TEST: expr = "a & b"; break;
                case ZYDIS_MNEMONIC_OR: expr = "a | b"; break;
                default: expr = "a ^ b"; break;
            }
            line("{");
            line(std::format("uint32_t a = {}, b = {};", same ? "0u" : read(i, d, w), same ? "0u" : read(i, s, w)));
            line(std::format("[[maybe_unused]] uint32_t r = {};", masked(expr, w)));
            const bool add = M == ZYDIS_MNEMONIC_ADD || M == ZYDIS_MNEMONIC_ADC;
            const bool sub = M == ZYDIS_MNEMONIC_SUB || M == ZYDIS_MNEMONIC_CMP || M == ZYDIS_MNEMONIC_SBB;
            if (add || sub) {
                if (need & CF) {
                    if (M == ZYDIS_MNEMONIC_ADD) line("cf = r < a;");
                    else if (M == ZYDIS_MNEMONIC_ADC) line("cf = cf ? (r <= a) : (r < a);");
                    else if (M == ZYDIS_MNEMONIC_SBB) line("cf = cf ? (a <= b) : (a < b);");
                    else line("cf = a < b;");
                }
                if (need & OF) {
                    if (add) line(std::format("of = ((a ^ r) & (b ^ r) & 0x{:X}u) != 0;", sg));
                    else line(std::format("of = ((a ^ b) & (a ^ r) & 0x{:X}u) != 0;", sg));
                }
                if (need & AF) line("af = ((a ^ b ^ r) & 0x10u) != 0;");
            } else {
                if (need & CF) line("cf = false;");
                if (need & OF) line("of = false;");
            }
            emit_szp(need, w);
            if (M != ZYDIS_MNEMONIC_CMP && M != ZYDIS_MNEMONIC_TEST) line(write(i, d, w, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG: case ZYDIS_MNEMONIC_NOT: {
            const int w = width(i, d);
            const uint32_t sg = sign_of(w);
            line("{");
            line(std::format("uint32_t a = {};", read(i, d, w)));
            if (M == ZYDIS_MNEMONIC_INC) {
                line(std::format("uint32_t r = {};", masked("a + 1u", w)));
                if (need & OF) line(std::format("of = r == 0x{:X}u;", sg));
                if (need & AF) line("af = (r & 0xFu) == 0;");
            } else if (M == ZYDIS_MNEMONIC_DEC) {
                line(std::format("uint32_t r = {};", masked("a - 1u", w)));
                if (need & OF) line(std::format("of = a == 0x{:X}u;", sg));
                if (need & AF) line("af = (a & 0xFu) == 0;");
            } else if (M == ZYDIS_MNEMONIC_NEG) {
                line(std::format("uint32_t r = {};", masked("0u - a", w)));
                if (need & CF) line("cf = a != 0;");
                if (need & OF) line(std::format("of = a == 0x{:X}u;", sg));
                if (need & AF) line("af = ((a ^ r) & 0x10u) != 0;");
            } else {
                line(std::format("uint32_t r = {};", masked("~a", w)));
            }
            if (M != ZYDIS_MNEMONIC_NOT) emit_szp(need, w);
            line(write(i, d, w, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            const int w = width(i, d);
            const uint32_t sg = sign_of(w);
            if (!shift_by_cl(i) && shift_imm(i) == 0) return;  // no-op; flags unchanged
            if (shift_by_cl(i)) use(ECX);
            const std::string cnt = shift_by_cl(i) ? "(ecx & 31u)" : std::format("{}u", shift_imm(i));
            line("{");
            line(std::format("const uint32_t n = {};", cnt));
            line("if (n) {");
            line(std::format("uint32_t a = {};", read(i, d, w)));
            if (M == ZYDIS_MNEMONIC_SHL) {
                line(std::format("uint64_t wide = uint64_t(a) << n; uint32_t r = {};", masked("uint32_t(wide)", w)));
                if (need & CF) line(std::format("cf = ((wide >> {}) & 1u) != 0;", w));
                if (need & OF) line(std::format("of = ((r & 0x{:X}u) != 0) != (((wide >> {}) & 1u) != 0);", sg, w));
            } else if (M == ZYDIS_MNEMONIC_SHR) {
                line(std::format("uint32_t r = {};", masked("a >> n", w)));
                if (need & CF) line("cf = ((a >> (n - 1u)) & 1u) != 0;");
                if (need & OF) line(std::format("of = (a & 0x{:X}u) != 0;", sg));
            } else {
                line(std::format("int32_t sa = int32_t(int{}_t(a)); uint32_t r = {};", w, masked("uint32_t(sa >> n)", w)));
                if (need & CF) line("cf = ((sa >> (n - 1u)) & 1) != 0;");
                if (need & OF) line("of = false;");
            }
            emit_szp(need, w);
            line(write(i, d, w, "r"));
            line("}");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_MUL: {
            const int w = width(i, d);
            if (i.nops == 1) {  // edx:eax = eax * r/m32
                if (w != 32) unsupported(i, "8/16-bit one-operand multiply");
                mark(EAX);
                mark(EDX);
                line("{");
                if (M == ZYDIS_MNEMONIC_IMUL) {
                    line(std::format("const int64_t p = int64_t(int32_t(eax)) * int64_t(int32_t({}));", read(i, d, 32)));
                    line("eax = uint32_t(uint64_t(p)); edx = uint32_t(uint64_t(p) >> 32);");
                    if (need & (CF | OF)) line("cf = of = p != int64_t(int32_t(eax));");
                } else {
                    line(std::format("const uint64_t p = uint64_t(eax) * uint64_t({});", read(i, d, 32)));
                    line("eax = uint32_t(p); edx = uint32_t(p >> 32);");
                    if (need & (CF | OF)) line("cf = of = edx != 0;");
                }
                line("}");
                return;
            }
            const Operand& a = i.nops == 3 ? i.op[1] : i.op[0];
            const Operand& b = i.nops == 3 ? i.op[2] : i.op[1];
            if (w == 16) {  // r16 = low 16 bits of the signed product; CF = OF = it doesn't fit in 16 bits
                line("{");
                line(std::format("const int32_t p = int32_t(int16_t({})) * int32_t(int16_t({}));", read(i, a, 16), read(i, b, 16)));
                line("const uint32_t r = uint32_t(p) & 0xFFFFu;");
                if (need & (CF | OF)) line("cf = of = p != int32_t(int16_t(r));");
                line(write(i, d, 16, "r"));
                line("}");
                return;
            }
            if (w != 32) unsupported(i, "8-bit imul");
            line("{");
            line(std::format("const int64_t p = int64_t(int32_t({})) * int64_t(int32_t({}));", read(i, a, 32), read(i, b, 32)));
            line("const uint32_t r = uint32_t(uint64_t(p));");
            if (need & (CF | OF)) line("cf = of = p != int64_t(int32_t(r));");
            line(write(i, d, 32, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: {
            if (width(i, d) != 32) unsupported(i, "8/16-bit divide");
            mark(EAX);
            mark(EDX);
            line("{");
            line(std::format("const uint32_t dv = {};", read(i, d, 32)));
            line(std::format("if (dv == 0) rt::trap(0x{:X}u, \"divide by zero (#DE)\");", i.addr));
            if (M == ZYDIS_MNEMONIC_DIV) {
                line("const uint64_t n = (uint64_t(edx) << 32) | eax; const uint64_t q = n / dv;");
                line(std::format("if (q > 0xFFFFFFFFull) rt::trap(0x{:X}u, \"divide overflow (#DE)\");", i.addr));
                line("eax = uint32_t(q); edx = uint32_t(n % dv);");
            } else {
                line("const int64_t n = int64_t((uint64_t(edx) << 32) | eax); const int64_t sd = int32_t(dv);");
                line(std::format("if (n == INT64_MIN && sd == -1) rt::trap(0x{:X}u, \"divide overflow (#DE)\");", i.addr));
                line("const int64_t q = n / sd;");
                line(std::format("if (q > INT32_MAX || q < INT32_MIN) rt::trap(0x{:X}u, \"divide overflow (#DE)\");", i.addr));
                line("eax = uint32_t(int32_t(q)); edx = uint32_t(int32_t(n % sd));");
            }
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_CDQ: use(EAX); mark(EDX); line("edx = uint32_t(int32_t(eax) >> 31);"); return;
        case ZYDIS_MNEMONIC_CWDE: mark(EAX); line("eax = uint32_t(int32_t(int16_t(eax)));"); return;
        case ZYDIS_MNEMONIC_LAHF:
            mark(EAX);
            line("rt::set_hi8(eax, (uint32_t(sf) << 7) | (uint32_t(zf) << 6) | (uint32_t(af) << 4) | (uint32_t(pf) << 2) | 2u | uint32_t(cf));");
            return;
        case ZYDIS_MNEMONIC_SAHF:
            use(EAX);
            line("{ const uint32_t h = (eax >> 8) & 0xFFu; sf = h & 0x80u; zf = h & 0x40u; af = h & 0x10u; pf = h & 0x04u; cf = h & 0x01u; }");
            return;
        case ZYDIS_MNEMONIC_STC: line("cf = true;"); return;
        case ZYDIS_MNEMONIC_CLC: line("cf = false;"); return;
        case ZYDIS_MNEMONIC_CMC: line("cf = !cf;"); return;
        case ZYDIS_MNEMONIC_PUSH: {
            const int w = d.kind == Operand::Kind::Imm ? 32 : width(i, d);
            if (w != 32) unsupported(i, "16-bit push");
            mark(ESP);
            // The value is read before esp moves (push esp pushes the old value).
            line(std::format("{{ const uint32_t v = {}; esp -= 4u; rt::st32(m, esp, v, 0x{:X}u); }}", read(i, d, 32), i.addr));
            return;
        }
        case ZYDIS_MNEMONIC_POP: {
            if (width(i, d) != 32) unsupported(i, "16-bit pop");
            mark(ESP);
            // pop m32 computes its address after esp has been incremented.
            line(std::format("{{ const uint32_t v = rt::ld32(m, esp, 0x{:X}u); esp += 4u; {} }}", i.addr, write(i, d, 32, "v")));
            return;
        }
        case ZYDIS_MNEMONIC_LEAVE:
            mark(ESP);
            mark(EBP);
            line(std::format("esp = ebp; ebp = rt::ld32(m, esp, 0x{:X}u); esp += 4u;", i.addr));
            return;
        case ZYDIS_MNEMONIC_CALL: {
            // The target operand is read before the push (`call [esp+4]` sees the old esp).
            std::string target;
            const auto t = i.rel_target();
            if (!t) {
                if (width(i, d) != 32) unsupported(i, "16-bit indirect call");
                target = read(i, d, 32);
            }
            mark(ESP);
            const uint32_t ret = i.end();
            std::string code = "{ ";
            if (!t) code += std::format("const uint32_t t = {}; ", target);
            code += std::format("esp -= 4u; rt::st32(m, esp, 0x{:X}u, 0x{:X}u); writeback(); ", ret, i.addr);
            if (t && flags_in(*t)) code += pack_flags(flags_in(*t)) + " ";
            code += t ? std::format("{}(c); ", Lifter::name(*t)) : std::string("rt::call(c, t); ");
            if (t && flags_out(*t)) code += unpack_flags(flags_out(*t)) + " ";
            if (idx_.contains(ret)) code += std::format("reload(); rt::check_return(c, 0x{:X}u, 0x{:X}u); }}", ret, i.addr);
            else code += std::format("rt::trap(0x{:X}u, \"returned from a call the CFG took as no-return\"); }}", i.addr);
            line(code);
            calls_ = true;
            return;
        }
        case ZYDIS_MNEMONIC_RET: {
            const uint32_t n = i.nops >= 1 && d.kind == Operand::Kind::Imm ? uint32_t(d.imm & 0xFFFF) : 0;
            mark(ESP);
            if (const uint8_t o = flags_out(f_.start)) line(pack_flags(o));
            line(std::format("c.ret_to = rt::ld32(m, esp, 0x{:X}u); esp += {}u; writeback(); return;", i.addr, 4 + n));
            return;
        }
        case ZYDIS_MNEMONIC_JMP: {
            if (auto t = i.rel_target()) {
                if (idx_.contains(*t)) line(std::format("{}goto L_{:08X};", tick(i, *t), *t));
                else line(std::format("QILIN_TAIL(); writeback(); {}{}(c); return;  // tail call",
                                      flags_in(*t) ? pack_flags(flags_in(*t)) + " " : std::string(), Lifter::name(*t)));
                return;
            }
            const auto jt = std::ranges::find(f_.jtabs, i.addr, &JumpTable::at);
            if (jt == f_.jtabs.end()) {
                // Indirect tail call (jmp eax, jmp [eax+0x14], jmp [0x304EC4]).
                if (width(i, d) != 32) unsupported(i, "16-bit indirect jump");
                line(std::format("{{ const uint32_t t = {}; QILIN_TAIL(); writeback(); rt::call(c, t); return; }}  // indirect tail call",
                                 read(i, d, 32)));
                return;
            }
            use(gpr(d.index)->gpr);
            if (std::ranges::any_of(jt->targets, [&](uint32_t x) { return x <= i.addr; })) line("QILIN_TICK();");
            line(std::format("switch ({}) {{", gpr(d.index)->name()));
            for (size_t k = 0; k < jt->entries.size(); ++k)
                if (jt->entries[k])
                    line(std::format("    case {}u: goto L_{:08X};", uint32_t(jt->first) + uint32_t(k), jt->entries[k]));
            line(std::format("    default: rt::trap(0x{:X}u, \"jump table index outside the recovered table\");", i.addr));
            line("}");
            return;
        }
        // ---- string ops. DF is tracked statically (df_now_); F2 and F3 both
        // mean rep for stos/movs, F3 is repe and F2 repne for cmps ----
        case ZYDIS_MNEMONIC_STD: case ZYDIS_MNEMONIC_CLD:
            return;  // the direction is resolved per string instruction at lift time
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: {
            const int sz = M == ZYDIS_MNEMONIC_STOSB ? 1 : M == ZYDIS_MNEMONIC_STOSW ? 2 : 4;
            if (direction(i) != 0) unsupported(i, "stos with the direction flag set");
            use(EAX);
            mark(EDI);
            if (i.has_rep()) {
                mark(ECX);
                line(std::format("rt::rep_stos{}(m, edi, ecx, eax, 0x{:X}u);", sz * 8, i.addr));
            } else {
                line(std::format("rt::st{0}(m, edi, uint{0}_t(eax), 0x{1:X}u); edi += {2}u;", sz * 8, i.addr, sz));
            }
            return;
        }
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD: {
            if (M == ZYDIS_MNEMONIC_MOVSD && i.nops > 0) unsupported(i, "SSE2 movsd");  // (implausible on a P3 anyway)
            const int sz = M == ZYDIS_MNEMONIC_MOVSB ? 1 : M == ZYDIS_MNEMONIC_MOVSW ? 2 : 4;
            const bool back = direction(i) == 1;
            mark(ESI);
            mark(EDI);
            if (i.has_rep()) {
                mark(ECX);
                line(std::format("rt::rep_movs{}(m, esi, edi, ecx, {}u, 0x{:X}u);", back ? "_back" : "", sz, i.addr));
            } else {
                line(std::format("rt::st{0}(m, edi, rt::ld{0}(m, esi, 0x{1:X}u), 0x{1:X}u); esi {2}= {3}u; edi {2}= {3}u;", sz * 8,
                                 i.addr, back ? '-' : '+', sz));
            }
            return;
        }
        case ZYDIS_MNEMONIC_BSF: case ZYDIS_MNEMONIC_BSR: {
            // Zero source: ZF set, destination unchanged (as the Pentium III and the emulator leave it).
            if (width(i, d) != 32) unsupported(i, "16-bit bit scan");
            line(std::format("{{ const uint32_t v_ = {}; if (v_ == 0u) {{ zf = true; }} else {{ zf = false; {} }} }}", read(i, s, 32),
                             write(i, d, 32, M == ZYDIS_MNEMONIC_BSF ? "uint32_t(std::countr_zero(v_))" : "uint32_t(31 - std::countl_zero(v_))")));
            return;
        }
        case ZYDIS_MNEMONIC_JECXZ: {  // flags untouched
            use(ECX);
            const uint32_t t = *i.rel_target();
            if (idx_.contains(t)) line(std::format("if (ecx == 0u) {{ {}goto L_{:08X}; }}", tick(i, t), t));
            else line(std::format("if (ecx == 0u) {{ QILIN_TAIL(); writeback(); {}(c); return; }}  // conditional tail call", Lifter::name(t)));
            return;
        }
        case ZYDIS_MNEMONIC_SHLD: case ZYDIS_MNEMONIC_SHRD: {
            if (width(i, d) != 32) unsupported(i, "16-bit double shift");
            const bool by_cl = dshift_by_cl(i);
            const uint32_t imm = by_cl ? 0 : uint32_t(i.op[2].imm & 31);
            if (!by_cl && imm == 0) return;  // no-op; flags unchanged
            if (by_cl) use(ECX);
            line("{");
            line(std::format("const uint32_t n = {};", by_cl ? std::string("(ecx & 31u)") : std::format("{}u", imm)));
            line("if (n) {");
            line(std::format("const uint32_t a = {}, b = {};", read(i, d, 32), read(i, s, 32)));
            if (M == ZYDIS_MNEMONIC_SHLD) {
                line("const uint32_t r = (a << n) | (b >> (32u - n));");
                if (need & CF) line("cf = ((a >> (32u - n)) & 1u) != 0;");
            } else {
                line("const uint32_t r = (a >> n) | (b << (32u - n));");
                if (need & CF) line("cf = ((a >> (n - 1u)) & 1u) != 0;");
            }
            if (need & OF) line("of = ((r ^ a) & 0x80000000u) != 0;");  // count 1 only (flag_fx)
            emit_szp(need, 32);
            line(write(i, d, 32, "r"));
            line("}");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: {
            // The count is masked to 5 bits; a count of 0 changes nothing,
            // flags included. CF is the bit rotated last into the end it
            // lands at; OF is defined for counts of 1 (as for RCL/RCR).
            const int w = width(i, d);
            const bool left = M == ZYDIS_MNEMONIC_ROL;
            if (shift_by_cl(i)) use(ECX);
            const std::string n = shift_by_cl(i) ? "(ecx & 31u)" : std::format("{}u", shift_imm(i) & 31);
            line("{");
            line(std::format("const uint32_t n0 = {}; const uint32_t a = {};", n, read(i, d, w)));
            line("if (n0) {");
            line(std::format("const uint32_t n = n0 % {}u;", w));
            const std::string r = left ? std::format("n ? ((a << n) | (a >> ({} - n))) : a", w) : std::format("n ? ((a >> n) | (a << ({} - n))) : a", w);
            line(std::format("const uint32_t r = {};", masked(r, w)));
            if (left) {
                if (need & CF) line("cf = (r & 1u) != 0;");
                if (need & OF) line(std::format("of = (((r >> {}) & 1u) != 0) != ((r & 1u) != 0);", w - 1));
            } else {
                if (need & CF) line(std::format("cf = ((r >> {}) & 1u) != 0;", w - 1));
                if (need & OF) line(std::format("of = (((r >> {}) ^ (r >> {})) & 1u) != 0;", w - 1, w - 2));
            }
            line(write(i, d, w, "r"));
            line("}");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR: {
            if (shift_by_cl(i) || shift_imm(i) != 1) unsupported(i, "rotate through carry by more than 1");
            const int w = width(i, d);
            line("{");
            line(std::format("const uint32_t a = {}; const uint32_t c0 = cf ? 1u : 0u;", read(i, d, w)));
            if (M == ZYDIS_MNEMONIC_RCR) {
                line(std::format("const uint32_t r = {};", masked(std::format("(a >> 1) | (c0 << {})", w - 1), w)));
                line("cf = (a & 1u) != 0;");
                if (need & OF) line(std::format("of = (((r >> {}) ^ (r >> {})) & 1u) != 0;", w - 1, w - 2));
            } else {
                line(std::format("const uint32_t r = {};", masked("(a << 1) | c0", w)));
                line(std::format("cf = ((a >> {}) & 1u) != 0;", w - 1));
                if (need & OF) line(std::format("of = (((r >> {}) & 1u) != 0) != cf;", w - 1));
            }
            line(write(i, d, w, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD: {
            const int sz = M == ZYDIS_MNEMONIC_SCASB ? 1 : M == ZYDIS_MNEMONIC_SCASW ? 2 : 4;
            const int w = sz * 8;
            const char step = direction(i) == 1 ? '-' : '+';
            use(EAX);
            mark(EDI);
            auto flags = [&](uint8_t want) {  // al/ax/eax - [edi]
                line(std::format("[[maybe_unused]] uint32_t r = {};", masked("a - b", w)));
                if (want & CF) line("cf = a < b;");
                if (want & OF) line(std::format("of = ((a ^ b) & (a ^ r) & 0x{:X}u) != 0;", sign_of(w)));
                if (want & AF) line("af = ((a ^ b ^ r) & 0x10u) != 0;");
                emit_szp(want, w);
            };
            const std::string acc = masked("eax", w), load = std::format("uint32_t(rt::ld{}(m, edi, 0x{:X}u))", w, i.addr);
            if (i.has_rep()) {
                mark(ECX);
                line("{");
                line(std::format("const uint32_t a = {}; uint32_t b = 0u; bool ran = false;", acc));
                line(std::format("while (ecx) {{ b = {}; edi {}= {}u; ecx -= 1u; ran = true; if (a {} b) break; }}", load, step, sz,
                                 i.has_repne() ? "==" : "!="));
                if (live_out & ALLF) {
                    line("if (ran) {");
                    flags(live_out);
                    line("}");
                } else {
                    line("(void)ran;");
                }
                line("}");
            } else {
                line("{");
                line(std::format("const uint32_t a = {}, b = {}; edi {}= {}u;", acc, load, step, sz));
                flags(need);
                line("}");
            }
            return;
        }
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD: {
            if (M == ZYDIS_MNEMONIC_CMPSD && i.nops > 0) unsupported(i, "SSE2 cmpsd");
            const int sz = M == ZYDIS_MNEMONIC_CMPSB ? 1 : M == ZYDIS_MNEMONIC_CMPSW ? 2 : 4;
            const int w = sz * 8;
            const char step = direction(i) == 1 ? '-' : '+';
            mark(ESI);
            mark(EDI);
            // Flags of the last comparison: [esi] - [edi].
            auto flags = [&](uint8_t want) {
                line(std::format("[[maybe_unused]] uint32_t r = {};", masked("a - b", w)));
                if (want & CF) line("cf = a < b;");
                if (want & OF) line(std::format("of = ((a ^ b) & (a ^ r) & 0x{:X}u) != 0;", sign_of(w)));
                if (want & AF) line("af = ((a ^ b ^ r) & 0x10u) != 0;");
                emit_szp(want, w);
            };
            auto load = [&](const char* r) { return std::format("uint32_t(rt::ld{}(m, {}, 0x{:X}u))", w, r, i.addr); };
            if (i.has_rep()) {
                // repe: continue while equal; repne: while not equal. With ecx = 0
                // nothing runs and the flags keep their values.
                mark(ECX);
                line("{");
                line("uint32_t a = 0u, b = 0u; bool ran = false;");
                line(std::format("while (ecx) {{ a = {}; b = {}; esi {}= {}u; edi {}= {}u; ecx -= 1u; ran = true; if (a {} b) break; }}",
                                 load("esi"), load("edi"), step, sz, step, sz, i.has_repne() ? "==" : "!="));
                if (live_out & ALLF) {
                    line("if (ran) {");
                    flags(live_out);
                    line("}");
                } else {
                    line("(void)ran;");
                }
                line("}");
            } else {
                line("{");
                line(std::format("const uint32_t a = {}, b = {}; esi {}= {}u; edi {}= {}u;", load("esi"), load("edi"), step, sz,
                                 step, sz));
                flags(need);
                line("}");
            }
            return;
        }
        // ---- scalar SSE ----
        case ZYDIS_MNEMONIC_MOVSS: {
            if (d.kind == Operand::Kind::Mem) {
                line(std::format("rt::st32(m, {}, {}, 0x{:X}u);", addr(i, d), xmm_lane0(i, s), i.addr));
            } else if (s.kind == Operand::Kind::Mem) {  // load: upper lanes are zeroed
                line(std::format("xmm{} = rt::Xmm{{{{{}, 0u, 0u, 0u}}}};", xmm_dst(i, d), xmm_lane0(i, s)));
            } else {  // register move: lane 0 only
                line(std::format("xmm{}.u[0] = {};", xmm_dst(i, d), xmm_lane0(i, s)));
            }
            return;
        }
        case ZYDIS_MNEMONIC_EMMS:
            mark_fx(FTAG);
            line("ftag = 0u;");
            return;
        case ZYDIS_MNEMONIC_RDTSC:
            mark(0);
            mark(2);
            line(std::format("{{ const uint64_t t_ = rt::rdtsc(0x{:X}u); eax = uint32_t(t_); edx = uint32_t(t_ >> 32); }}", i.addr));
            return;
        case ZYDIS_MNEMONIC_MOVD: case ZYDIS_MNEMONIC_PADDW: case ZYDIS_MNEMONIC_PADDSW: case ZYDIS_MNEMONIC_PSUBUSW:
        case ZYDIS_MNEMONIC_PMULLW: case ZYDIS_MNEMONIC_PMULHW: case ZYDIS_MNEMONIC_POR: case ZYDIS_MNEMONIC_PXOR:
        case ZYDIS_MNEMONIC_PAND: case ZYDIS_MNEMONIC_PANDN:
        case ZYDIS_MNEMONIC_PSLLW: case ZYDIS_MNEMONIC_PSLLD: case ZYDIS_MNEMONIC_PSLLQ:
        case ZYDIS_MNEMONIC_PSRLW: case ZYDIS_MNEMONIC_PSRLD: case ZYDIS_MNEMONIC_PSRLQ:
        case ZYDIS_MNEMONIC_PSRAW: case ZYDIS_MNEMONIC_PSRAD:
        case ZYDIS_MNEMONIC_PADDD: case ZYDIS_MNEMONIC_PSUBD: case ZYDIS_MNEMONIC_PSUBW: case ZYDIS_MNEMONIC_PSUBSW:
        case ZYDIS_MNEMONIC_PACKSSDW: case ZYDIS_MNEMONIC_PACKSSWB: case ZYDIS_MNEMONIC_PACKUSWB: case ZYDIS_MNEMONIC_PMADDWD:
        case ZYDIS_MNEMONIC_PADDB: case ZYDIS_MNEMONIC_PSUBB:
        case ZYDIS_MNEMONIC_PUNPCKLBW: case ZYDIS_MNEMONIC_PUNPCKHBW: case ZYDIS_MNEMONIC_PUNPCKLDQ: case ZYDIS_MNEMONIC_PUNPCKHDQ: {
            const bool dm = d.kind == Operand::Kind::Reg && mm_index(d.reg) >= 0;
            const bool sm = s.kind == Operand::Kind::Reg && mm_index(s.reg) >= 0;
            if (!dm && !sm) unsupported(i, "this form (not MMX)");
            mark_fx(FTOP);
            mark_fx(FTAG);
            line("ftop = 0u; ftag = 0xFFu;");
            if (M == ZYDIS_MNEMONIC_MOVD) {  // 32 bits to or from the low half; loads zero the high half
                if (dm) line(std::format("c.mm[{}] = uint64_t({});", mm_index(d.reg), read(i, s, 32)));
                else line(write(i, d, 32, std::format("uint32_t(c.mm[{}])", mm_index(s.reg))));
                return;
            }
            if (!dm) unsupported(i, "MMX destination");
            const int r = mm_index(d.reg);
            std::string src;
            if (sm) src = std::format("c.mm[{}]", mm_index(s.reg));
            else if (s.kind == Operand::Kind::Mem && s.size == 32) src = std::format("uint64_t(rt::ld32(m, {}, 0x{:X}u))", addr(i, s), i.addr);
            else if (s.kind == Operand::Kind::Mem) src = std::format("rt::ld64(m, {}, 0x{:X}u)", addr(i, s), i.addr);
            else if (s.kind == Operand::Kind::Imm) src = std::format("uint64_t({}u)", uint32_t(s.imm & 0xFF));
            else unsupported(i, "MMX source operand");
            const std::string a = std::format("c.mm[{}]", r);
            std::string e;
            switch (M) {
                case ZYDIS_MNEMONIC_PADDW: e = std::format("rt::mmx::paddw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PADDSW: e = std::format("rt::mmx::paddsw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSUBUSW: e = std::format("rt::mmx::psubusw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PMULLW: e = std::format("rt::mmx::pmullw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PMULHW: e = std::format("rt::mmx::pmulhw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_POR: e = std::format("({} | {})", a, src); break;
                case ZYDIS_MNEMONIC_PXOR: e = sm && s.reg == d.reg ? "uint64_t(0)" : std::format("({} ^ {})", a, src); break;
                case ZYDIS_MNEMONIC_PAND: e = std::format("({} & {})", a, src); break;
                case ZYDIS_MNEMONIC_PANDN: e = std::format("(~{} & {})", a, src); break;
                case ZYDIS_MNEMONIC_PSLLW: e = std::format("rt::mmx::shl<uint16_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSLLD: e = std::format("rt::mmx::shl<uint32_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSLLQ: e = std::format("rt::mmx::shl<uint64_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSRLW: e = std::format("rt::mmx::shr<uint16_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSRLD: e = std::format("rt::mmx::shr<uint32_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSRLQ: e = std::format("rt::mmx::shr<uint64_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSRAW: e = std::format("rt::mmx::sar<int16_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PADDD: e = std::format("rt::mmx::add<uint32_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PADDB: e = std::format("rt::mmx::add<uint8_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSUBD: e = std::format("rt::mmx::sub<uint32_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSUBW: e = std::format("rt::mmx::sub<uint16_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSUBB: e = std::format("rt::mmx::sub<uint8_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSUBSW: e = std::format("rt::mmx::psubsw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PACKSSDW: e = std::format("rt::mmx::packssdw({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PACKSSWB: e = std::format("rt::mmx::packsswb({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PACKUSWB: e = std::format("rt::mmx::packuswb({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PMADDWD: e = std::format("rt::mmx::pmaddwd({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PSRAD: e = std::format("rt::mmx::sar<int32_t>({}, {})", a, src); break;
                case ZYDIS_MNEMONIC_PUNPCKLBW: e = std::format("rt::mmx::unpack<uint8_t>({}, {}, false)", a, src); break;
                case ZYDIS_MNEMONIC_PUNPCKHBW: e = std::format("rt::mmx::unpack<uint8_t>({}, {}, true)", a, src); break;
                case ZYDIS_MNEMONIC_PUNPCKLDQ: e = std::format("rt::mmx::unpack<uint32_t>({}, {}, false)", a, src); break;
                case ZYDIS_MNEMONIC_PUNPCKHDQ: e = std::format("rt::mmx::unpack<uint32_t>({}, {}, true)", a, src); break;
                default: unsupported(i, "MMX operation");
            }
            line(std::format("c.mm[{}] = {};", r, e));
            return;
        }
        case ZYDIS_MNEMONIC_MOVQ: case ZYDIS_MNEMONIC_PUNPCKLWD: case ZYDIS_MNEMONIC_PUNPCKHWD: {
            const bool dm = d.kind == Operand::Kind::Reg && mm_index(d.reg) >= 0;
            const bool sm = s.kind == Operand::Kind::Reg && mm_index(s.reg) >= 0;
            if (!dm && !sm) unsupported(i, "this form (not MMX)");
            // Any MMX instruction: TOP 0, every x87 register in use.
            mark_fx(FTOP);
            mark_fx(FTAG);
            line("ftop = 0u; ftag = 0xFFu;");
            auto src64 = [&]() -> std::string {
                if (sm) return std::format("c.mm[{}]", mm_index(s.reg));
                if (s.kind == Operand::Kind::Mem && s.size == 32) return std::format("uint64_t(rt::ld32(m, {}, 0x{:X}u))", addr(i, s), i.addr);
                if (s.kind == Operand::Kind::Mem) return std::format("rt::ld64(m, {}, 0x{:X}u)", addr(i, s), i.addr);
                unsupported(i, "MMX source operand");
            };
            if (M == ZYDIS_MNEMONIC_MOVQ) {
                if (dm) line(std::format("c.mm[{}] = {};", mm_index(d.reg), src64()));
                else if (d.kind == Operand::Kind::Mem) line(std::format("rt::st64(m, {}, c.mm[{}], 0x{:X}u);", addr(i, d), mm_index(s.reg), i.addr));
                else unsupported(i, "movq destination");
                return;
            }
            if (!dm) unsupported(i, "MMX destination");
            // Interleave the 16-bit words of the low (or high) halves: dst, src, dst, src.
            const bool hi = M == ZYDIS_MNEMONIC_PUNPCKHWD;
            line(std::format("{{ const uint64_t a = c.mm[{0}] >> {1}, b = {2} >> {1}; c.mm[{0}] = (a & 0xFFFFu) | ((b & 0xFFFFu) << 16) | "
                             "(((a >> 16) & 0xFFFFu) << 32) | (((b >> 16) & 0xFFFFu) << 48); }}",
                             mm_index(d.reg), hi ? 32 : 0, src64()));
            return;
        }
        case ZYDIS_MNEMONIC_MOVNTPS:  // a non-temporal store: as movaps to memory, the cache hint aside
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: {
            const bool al = M != ZYDIS_MNEMONIC_MOVUPS;
            if (d.kind == Operand::Kind::Mem)
                line(std::format("rt::st128(m, {}, {}, {}, 0x{:X}u);", addr(i, d), xmm128(i, s, al), al, i.addr));
            else
                line(std::format("xmm{} = {};", xmm_dst(i, d), xmm128(i, s, al)));
            return;
        }
        case ZYDIS_MNEMONIC_ADDSS: case ZYDIS_MNEMONIC_SUBSS: case ZYDIS_MNEMONIC_MULSS: case ZYDIS_MNEMONIC_DIVSS: {
            const char* op = M == ZYDIS_MNEMONIC_ADDSS ? "addss" : M == ZYDIS_MNEMONIC_SUBSS ? "subss" : M == ZYDIS_MNEMONIC_MULSS ? "mulss" : "divss";
            const int x = xmm_dst(i, d);
            line(std::format("xmm{0}.u[0] = rt::{1}(xmm{0}.u[0], {2});", x, op, xmm_lane0(i, s)));
            return;
        }
        case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_MAXSS: {
            const int x = xmm_dst(i, d);
            line(std::format("xmm{0}.u[0] = rt::{1}(xmm{0}.u[0], {2});", x, M == ZYDIS_MNEMONIC_MINSS ? "minss" : "maxss",
                             xmm_lane0(i, s)));
            return;
        }
        case ZYDIS_MNEMONIC_SQRTSS:
            line(std::format("xmm{}.u[0] = rt::sqrtss({});", xmm_dst(i, d), xmm_lane0(i, s)));
            return;
        case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ANDNPS: {
            const int x = xmm_dst(i, d);
            if (M == ZYDIS_MNEMONIC_XORPS && s.is_reg(d.reg)) {
                line(std::format("xmm{} = rt::Xmm{{}};", x));
                return;
            }
            const char* e = M == ZYDIS_MNEMONIC_XORPS ? "a ^ b" : M == ZYDIS_MNEMONIC_ANDPS ? "a & b" : M == ZYDIS_MNEMONIC_ORPS ? "a | b" : "~a & b";
            line(std::format("{{ const rt::Xmm s = {1}; for (int k = 0; k < 4; ++k) {{ const uint32_t a = xmm{0}.u[k], b = s.u[k]; xmm{0}.u[k] = {2}; }} }}",
                             x, xmm128(i, s, true), e));
            return;
        }
        // ---- packed SSE (XGraphics maths): each lane exactly as the scalar op
        case ZYDIS_MNEMONIC_ADDPS: case ZYDIS_MNEMONIC_SUBPS: case ZYDIS_MNEMONIC_MULPS: case ZYDIS_MNEMONIC_DIVPS:
        case ZYDIS_MNEMONIC_MINPS: case ZYDIS_MNEMONIC_MAXPS: {
            const char* op = M == ZYDIS_MNEMONIC_ADDPS ? "addss" : M == ZYDIS_MNEMONIC_SUBPS ? "subss" : M == ZYDIS_MNEMONIC_MULPS ? "mulss"
                           : M == ZYDIS_MNEMONIC_DIVPS ? "divss" : M == ZYDIS_MNEMONIC_MINPS ? "minss" : "maxss";
            const int x = xmm_dst(i, d);
            line(std::format("{{ const rt::Xmm s_ = {1}; for (int k = 0; k < 4; ++k) xmm{0}.u[k] = rt::{2}(xmm{0}.u[k], s_.u[k]); }}", x,
                             xmm128(i, s, true), op));
            return;
        }
        case ZYDIS_MNEMONIC_SHUFPS: {
            const int x = xmm_dst(i, d);
            const uint32_t imm = uint32_t(i.op[2].imm & 0xFF);
            line(std::format("{{ const rt::Xmm d_ = xmm{0}, s_ = {1}; xmm{0} = rt::Xmm{{{{d_.u[{2}], d_.u[{3}], s_.u[{4}], s_.u[{5}]}}}}; }}", x,
                             xmm128(i, s, true), imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3));
            return;
        }
        case ZYDIS_MNEMONIC_UNPCKLPS: case ZYDIS_MNEMONIC_UNPCKHPS: {
            const int x = xmm_dst(i, d);
            const int b = M == ZYDIS_MNEMONIC_UNPCKLPS ? 0 : 2;
            line(std::format("{{ const rt::Xmm d_ = xmm{0}, s_ = {1}; xmm{0} = rt::Xmm{{{{d_.u[{2}], s_.u[{2}], d_.u[{3}], s_.u[{3}]}}}}; }}", x,
                             xmm128(i, s, true), b, b + 1));
            return;
        }
        case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVHPS: {  // lanes 0-1 / 2-3 to or from 64 bits of memory
            const int lo = M == ZYDIS_MNEMONIC_MOVLPS ? 0 : 2;
            if (d.kind == Operand::Kind::Mem) {
                const int x = xmm_index(s.reg);
                if (x < 0) unsupported(i, "expected an xmm register");
                use_xmm(x);
                line(std::format("rt::st64(m, {}, uint64_t(xmm{}.u[{}]) | uint64_t(xmm{}.u[{}]) << 32, 0x{:X}u);", addr(i, d), x, lo, x, lo + 1, i.addr));
            } else {
                if (s.kind != Operand::Kind::Mem) unsupported(i, "register form");
                const int x = xmm_dst(i, d);
                line(std::format("{{ const uint64_t v_ = rt::ld64(m, {}, 0x{:X}u); xmm{}.u[{}] = uint32_t(v_); xmm{}.u[{}] = uint32_t(v_ >> 32); }}",
                                 addr(i, s), i.addr, x, lo, x, lo + 1));
            }
            return;
        }
        case ZYDIS_MNEMONIC_MOVHLPS: case ZYDIS_MNEMONIC_MOVLHPS: {
            const int x = xmm_dst(i, d), y = xmm_index(s.reg);
            if (y < 0) unsupported(i, "expected an xmm register");
            use_xmm(y);
            if (M == ZYDIS_MNEMONIC_MOVHLPS) line(std::format("{{ const rt::Xmm s_ = xmm{1}; xmm{0}.u[0] = s_.u[2]; xmm{0}.u[1] = s_.u[3]; }}", x, y));
            else line(std::format("{{ const rt::Xmm s_ = xmm{1}; xmm{0}.u[2] = s_.u[0]; xmm{0}.u[3] = s_.u[1]; }}", x, y));
            return;
        }
        case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS: {
            const int x = xmm_index(d.reg);
            if (x < 0) unsupported(i, "expected an xmm register");
            use_xmm(x);
            line(std::format("{{ const float a = rt::f32(xmm{}.u[0]), b = rt::f32({});", x, xmm_lane0(i, s)));
            line("  [[maybe_unused]] const bool un = a != a || b != b;");
            if (need & ZF) line("  zf = un || a == b;");
            if (need & PF) line("  pf = un;");
            if (need & CF) line("  cf = un || a < b;");
            if (need & OF) line("  of = false;");
            if (need & SF) line("  sf = false;");
            if (need & AF) line("  af = false;");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_CVTSI2SS:
            line(std::format("xmm{}.u[0] = rt::u32(float(int32_t({})));", xmm_dst(i, d), read(i, s, 32)));
            return;
        case ZYDIS_MNEMONIC_CVTPS2PI: case ZYDIS_MNEMONIC_CVTTPS2PI: {
            // Two floats (xmm lanes 0-1, or 64 bits of memory) to two int32 in an MMX register.
            const int r = d.kind == Operand::Kind::Reg ? mm_index(d.reg) : -1;
            if (r < 0) unsupported(i, "expected an MMX destination");
            mark_fx(FTOP);
            mark_fx(FTAG);
            line("ftop = 0u; ftag = 0xFFu;");
            std::string lo, hi;
            if (s.kind == Operand::Kind::Reg) {
                const int x = xmm_index(s.reg);
                if (x < 0) unsupported(i, "expected an xmm source");
                use_xmm(x);
                lo = std::format("xmm{}.u[0]", x), hi = std::format("xmm{}.u[1]", x);
            } else if (s.kind == Operand::Kind::Mem) {
                lo = std::format("rt::ld32(m, {}, 0x{:X}u)", addr(i, s), i.addr);
                hi = std::format("rt::ld32(m, {} + 4u, 0x{:X}u)", addr(i, s), i.addr);
            } else {
                unsupported(i, "SSE operand");
            }
            const char* f = M == ZYDIS_MNEMONIC_CVTTPS2PI ? "cvtt_f32_i32" : "cvt_f32_i32";
            line(std::format("c.mm[{}] = uint64_t(rt::{}(rt::f32({}))) | (uint64_t(rt::{}(rt::f32({}))) << 32);", r, f, lo, f, hi));
            return;
        }
        case ZYDIS_MNEMONIC_PSHUFW: {
            // Each 16-bit lane of the destination from the source lane the immediate's 2-bit field names.
            const int r = d.kind == Operand::Kind::Reg ? mm_index(d.reg) : -1;
            if (r < 0) unsupported(i, "expected an MMX destination");
            const Operand& im = i.op[2];
            if (im.kind != Operand::Kind::Imm) unsupported(i, "pshufw without an immediate");
            std::string src;
            if (s.kind == Operand::Kind::Reg && mm_index(s.reg) >= 0) src = std::format("c.mm[{}]", mm_index(s.reg));
            else if (s.kind == Operand::Kind::Mem) src = std::format("rt::ld64(m, {}, 0x{:X}u)", addr(i, s), i.addr);
            else unsupported(i, "MMX source operand");
            mark_fx(FTOP);
            mark_fx(FTAG);
            line("ftop = 0u; ftag = 0xFFu;");
            const uint32_t sel = uint32_t(im.imm) & 0xFF;
            line(std::format("{{ const uint64_t v_ = {}; c.mm[{}] = ((v_ >> {}) & 0xFFFFu) | (((v_ >> {}) & 0xFFFFu) << 16) | (((v_ >> {}) & 0xFFFFu) << 32) | (((v_ >> {}) & 0xFFFFu) << 48); }}",
                             src, r, 16 * (sel & 3), 16 * (sel >> 2 & 3), 16 * (sel >> 4 & 3), 16 * (sel >> 6 & 3)));
            return;
        }
        case ZYDIS_MNEMONIC_CVTPI2PS: {
            // Two int32 (an MMX register or 64 bits of memory) to floats in xmm lanes 0-1.
            const int x = xmm_dst(i, d);
            std::string src;
            if (s.kind == Operand::Kind::Reg && mm_index(s.reg) >= 0) src = std::format("c.mm[{}]", mm_index(s.reg));
            else if (s.kind == Operand::Kind::Mem) src = std::format("rt::ld64(m, {}, 0x{:X}u)", addr(i, s), i.addr);
            else unsupported(i, "MMX source operand");
            line(std::format("{{ const uint64_t v_ = {}; xmm{}.u[0] = rt::u32(float(int32_t(uint32_t(v_)))); xmm{}.u[1] = rt::u32(float(int32_t(uint32_t(v_ >> 32)))); }}",
                             src, x, x));
            mark_fx(FTOP);
            mark_fx(FTAG);
            line("ftop = 0u; ftag = 0xFFu;");
            return;
        }
        case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI:
            if (width(i, d) != 32) unsupported(i, "unexpected destination");
            line(write(i, d, 32, std::format("rt::{}(rt::f32({}))", M == ZYDIS_MNEMONIC_CVTTSS2SI ? "cvtt_f32_i32" : "cvt_f32_i32",
                                             xmm_lane0(i, s))));
            return;
        default:
            break;
    }
    if (i.isa_ext == ZYDIS_ISA_EXT_X87 && emit_x87(i, need)) return;
    if (i.isa_ext == ZYDIS_ISA_EXT_X87) fail(i, LiftFailure::Kind::X87, "x87 instruction not supported yet");
    unsupported(i, "instruction not supported yet");
}

// ---------------------------------------------------------------- x87 (slice 3)
// The register stack lives in the Context (c.st, physical order); TOP, the
// tag bits, the control and status words are locals (see kX87). ST(i) is
// c.st[(ftop + i) & 7]. Arithmetic goes through rt::fadd & co, which apply
// precision control exactly (see guest.hpp).
int FnEmitter::st_index(const Insn& i, const Operand& o) {
    if (o.kind != Operand::Kind::Reg || o.reg < ZYDIS_REGISTER_ST0 || o.reg > ZYDIS_REGISTER_ST7)
        unsupported(i, "x87 register operand");
    return int(o.reg - ZYDIS_REGISTER_ST0);
}

std::string FnEmitter::fsrc(const Insn& i, const Operand& o) {
    if (o.kind == Operand::Kind::Reg) return std::format("ST({}u)", st_index(i, o));
    if (o.kind != Operand::Kind::Mem) unsupported(i, "x87 operand");
    const std::string a = addr(i, o);
    if (o.size == 32) return std::format("rt::f32_raw(rt::ld32(m, {}, 0x{:X}u))", a, i.addr);
    if (o.size == 64) return std::format("rt::f64_raw(rt::ld64(m, {}, 0x{:X}u))", a, i.addr);
    unsupported(i, std::format("{}-bit x87 memory operand", o.size));
}

void FnEmitter::fpush(const std::string& v) {
    mark_fx(FTOP);
    mark_fx(FTAG);
    line(std::format("{{ const double v_ = {}; ftop = (ftop - 1u) & 7u; ftag |= 1u << ftop; ST(0u) = v_; }}", v));
}

void FnEmitter::fpop() {
    mark_fx(FTOP);
    mark_fx(FTAG);
    line("ftag &= ~(1u << ftop); ftop = (ftop + 1u) & 7u;");
}

bool FnEmitter::emit_x87(const Insn& i, uint8_t need) {
    const ZydisMnemonic M = i.mnem;
    const Operand& d = i.op[0];
    x87_ = true;
    use_fx(FTOP);
    auto int_src = [&](const Operand& o) {
        if (o.kind != Operand::Kind::Mem) unsupported(i, "x87 integer operand");
        const std::string a = addr(i, o);
        if (o.size == 16) return std::format("double(int16_t(rt::ld16(m, {}, 0x{:X}u)))", a, i.addr);
        if (o.size == 32) return std::format("double(int32_t(rt::ld32(m, {}, 0x{:X}u)))", a, i.addr);
        if (o.size == 64) return std::format("double(int64_t(rt::ld64(m, {}, 0x{:X}u)))", a, i.addr);
        unsupported(i, "x87 integer operand width");
    };
    // Arithmetic: dst = dst op src, or src op dst for the reversed forms.
    auto arith = [&](const char* fn, bool reversed, bool integer, bool pop) {
        use_fx(FPCW);
        std::string dst = "ST(0u)", src;
        if (i.nops == 0) {
            dst = "ST(1u)";  // faddp with no operands: st1 op= st0, pop
            src = "ST(0u)";
        } else if (i.nops == 1) {
            src = integer ? int_src(d) : fsrc(i, d);
        } else {
            dst = std::format("ST({}u)", st_index(i, d));
            src = fsrc(i, i.op[1]);
        }
        line(reversed ? std::format("{0} = rt::{1}(fpcw, {2}, {0});", dst, fn, src)
                      : std::format("{0} = rt::{1}(fpcw, {0}, {2});", dst, fn, src));
        if (pop) fpop();
    };
    switch (M) {
        case ZYDIS_MNEMONIC_FADD: arith("fadd", false, false, false); return true;
        case ZYDIS_MNEMONIC_FADDP: arith("fadd", false, false, true); return true;
        case ZYDIS_MNEMONIC_FIADD: arith("fadd", false, true, false); return true;
        case ZYDIS_MNEMONIC_FSUB: arith("fsub", false, false, false); return true;
        case ZYDIS_MNEMONIC_FSUBP: arith("fsub", false, false, true); return true;
        case ZYDIS_MNEMONIC_FISUB: arith("fsub", false, true, false); return true;
        case ZYDIS_MNEMONIC_FSUBR: arith("fsub", true, false, false); return true;
        case ZYDIS_MNEMONIC_FSUBRP: arith("fsub", true, false, true); return true;
        case ZYDIS_MNEMONIC_FISUBR: arith("fsub", true, true, false); return true;
        case ZYDIS_MNEMONIC_FMUL: arith("fmul", false, false, false); return true;
        case ZYDIS_MNEMONIC_FMULP: arith("fmul", false, false, true); return true;
        case ZYDIS_MNEMONIC_FIMUL: arith("fmul", false, true, false); return true;
        case ZYDIS_MNEMONIC_FDIV: arith("fdiv", false, false, false); return true;
        case ZYDIS_MNEMONIC_FDIVP: arith("fdiv", false, false, true); return true;
        case ZYDIS_MNEMONIC_FIDIV: arith("fdiv", false, true, false); return true;
        case ZYDIS_MNEMONIC_FDIVR: arith("fdiv", true, false, false); return true;
        case ZYDIS_MNEMONIC_FDIVRP: arith("fdiv", true, false, true); return true;
        case ZYDIS_MNEMONIC_FIDIVR: arith("fdiv", true, true, false); return true;

        case ZYDIS_MNEMONIC_FLD:
            if (d.kind == Operand::Kind::Reg) fpush(fsrc(i, d));
            else if (d.size == 32) fpush(std::format("rt::fld32(rt::ld32(m, {}, 0x{:X}u))", addr(i, d), i.addr));
            else if (d.size == 64) fpush(std::format("rt::fld64(rt::ld64(m, {}, 0x{:X}u))", addr(i, d), i.addr));
            else if (d.size == 80) {
                const std::string a = addr(i, d);
                fpush(std::format("rt::fld80(rt::ld64(m, {0}, 0x{1:X}u), rt::ld16(m, {0} + 8u, 0x{1:X}u))", a, i.addr));
            } else unsupported(i, "x87 load size");
            return true;
        case ZYDIS_MNEMONIC_FILD: fpush(int_src(d)); return true;
        case ZYDIS_MNEMONIC_FLDZ: fpush("0.0"); return true;
        case ZYDIS_MNEMONIC_FLD1: fpush("1.0"); return true;
        // The x87's constants are 64-bit; these are their nearest doubles.
        case ZYDIS_MNEMONIC_FLDPI: fpush("rt::dfrom(0x400921FB54442D18ull)"); return true;
        case ZYDIS_MNEMONIC_FLDLG2: fpush("rt::dfrom(0x3FD34413509F79FFull)"); return true;
        case ZYDIS_MNEMONIC_FLDLN2: fpush("rt::dfrom(0x3FE62E42FEFA39EFull)"); return true;
        case ZYDIS_MNEMONIC_FLDL2E: fpush("rt::dfrom(0x3FF71547652B82FEull)"); return true;
        case ZYDIS_MNEMONIC_FLDL2T: fpush("rt::dfrom(0x400A934F0979A371ull)"); return true;

        case ZYDIS_MNEMONIC_FST: case ZYDIS_MNEMONIC_FSTP: {
            const bool pop = M == ZYDIS_MNEMONIC_FSTP;
            if (d.kind == Operand::Kind::Reg) {
                const int k = st_index(i, d);
                if (k != 0) {
                    mark_fx(FTAG);  // the destination holds a value now, even if it was empty
                    line(std::format("ST({0}u) = ST(0u); ftag |= 1u << ((ftop + {0}u) & 7u);", k));
                }
            } else if (d.size == 32) {
                line(std::format("rt::st32(m, {}, rt::fst32(ST(0u)), 0x{:X}u);", addr(i, d), i.addr));
            } else if (d.size == 64) {
                line(std::format("rt::st64(m, {}, rt::dbits(ST(0u)), 0x{:X}u);", addr(i, d), i.addr));
            } else {
                unsupported(i, "80-bit x87 store");
            }
            if (pop) fpop();
            return true;
        }
        case ZYDIS_MNEMONIC_FIST: case ZYDIS_MNEMONIC_FISTP:
            if (d.kind != Operand::Kind::Mem || (d.size != 32 && !(d.size == 64 && M == ZYDIS_MNEMONIC_FISTP)))
                unsupported(i, "fist other than 32-bit, or 64-bit fistp");
            use_fx(FPCW);
            if (d.size == 32) line(std::format("rt::st32(m, {}, rt::fist32(fpcw, ST(0u)), 0x{:X}u);", addr(i, d), i.addr));
            else line(std::format("rt::st64(m, {}, rt::fist64(fpcw, ST(0u)), 0x{:X}u);", addr(i, d), i.addr));
            if (M == ZYDIS_MNEMONIC_FISTP) fpop();
            return true;

        case ZYDIS_MNEMONIC_FXCH: {
            const int k = i.nops == 0 ? 1 : st_index(i, i.nops == 2 ? i.op[1] : d);
            line(std::format("{{ const double t_ = ST(0u); ST(0u) = ST({0}u); ST({0}u) = t_; }}", k));
            return true;
        }
        case ZYDIS_MNEMONIC_FCHS: line("ST(0u) = rt::dfrom(rt::dbits(ST(0u)) ^ 0x8000000000000000ull);"); return true;
        case ZYDIS_MNEMONIC_FABS: line("ST(0u) = rt::dfrom(rt::dbits(ST(0u)) & 0x7FFFFFFFFFFFFFFFull);"); return true;
        case ZYDIS_MNEMONIC_FSQRT:
            use_fx(FPCW);
            line("ST(0u) = rt::fsqrt(fpcw, ST(0u));");
            return true;
        case ZYDIS_MNEMONIC_FSIN: case ZYDIS_MNEMONIC_FCOS:
            mark_fx(FPSW);
            line(std::format("if (rt::x87_trig_in_range(ST(0u))) {{ ST(0u) = rt::{}(ST(0u)); fpsw &= ~0x600u; }} else {{ fpsw = (fpsw & ~0x200u) | 0x400u; }}",
                             M == ZYDIS_MNEMONIC_FSIN ? "fsin" : "fcos"));
            return true;
        case ZYDIS_MNEMONIC_FSINCOS:  // ST0 = sin, then push cos
            mark_fx(FPSW);
            line("if (rt::x87_trig_in_range(ST(0u))) {");
            line("const double x_ = ST(0u); ST(0u) = rt::fsin(x_); fpsw &= ~0x600u;");
            fpush("rt::fcos(x_)");
            line("} else { fpsw = (fpsw & ~0x200u) | 0x400u; }");
            return true;
        case ZYDIS_MNEMONIC_FPTAN:
            mark_fx(FPSW);
            line("if (rt::x87_trig_in_range(ST(0u))) {");
            line("ST(0u) = rt::ftan(ST(0u)); fpsw &= ~0x600u;");
            fpush("1.0");
            line("} else { fpsw = (fpsw & ~0x200u) | 0x400u; }");
            return true;
        case ZYDIS_MNEMONIC_FPATAN:
            line("ST(1u) = rt::fpatan(ST(1u), ST(0u));");
            fpop();
            return true;
        case ZYDIS_MNEMONIC_FYL2X:
            line("ST(1u) = rt::fyl2x(ST(1u), ST(0u));");
            fpop();
            return true;

        case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
            const int k = st_index(i, i.nops == 2 ? i.op[1] : d);
            line(std::format("{{ const double a_ = ST(0u), b_ = ST({}u); [[maybe_unused]] const bool un_ = a_ != a_ || b_ != b_;", k));
            if (need & ZF) line("zf = un_ || a_ == b_;");
            if (need & PF) line("pf = un_;");
            if (need & CF) line("cf = un_ || a_ < b_;");
            if (need & OF) line("of = false;");
            if (need & SF) line("sf = false;");
            if (need & AF) line("af = false;");
            line("}");
            if (M == ZYDIS_MNEMONIC_FCOMIP || M == ZYDIS_MNEMONIC_FUCOMIP) fpop();
            return true;
        }
        case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FCOMPP:
        case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP: case ZYDIS_MNEMONIC_FUCOMPP: case ZYDIS_MNEMONIC_FTST: {
            mark_fx(FPSW);
            std::string src = "ST(1u)";
            if (M == ZYDIS_MNEMONIC_FTST) src = "0.0";
            else if (i.nops == 1) src = fsrc(i, d);
            else if (i.nops == 2) src = fsrc(i, i.op[1]);
            line(std::format("fpsw = rt::fcom_codes(fpsw, ST(0u), {});", src));
            const int pops = (M == ZYDIS_MNEMONIC_FCOMP || M == ZYDIS_MNEMONIC_FUCOMP) ? 1
                             : (M == ZYDIS_MNEMONIC_FCOMPP || M == ZYDIS_MNEMONIC_FUCOMPP) ? 2 : 0;
            for (int k = 0; k < pops; ++k) fpop();
            return true;
        }
        case ZYDIS_MNEMONIC_FNSTSW: {
            use_fx(FPSW);
            const std::string sw = "((fpsw & ~0x3800u) | (ftop << 11)) & 0xFFFFu";
            if (d.kind == Operand::Kind::Reg) {
                if (d.reg != ZYDIS_REGISTER_AX) unsupported(i, "fnstsw to a register other than ax");
                mark(EAX);
                line(std::format("eax = (eax & 0xFFFF0000u) | ({});", sw));
            } else {
                line(std::format("rt::st16(m, {}, uint16_t({}), 0x{:X}u);", addr(i, d), sw, i.addr));
            }
            return true;
        }
        case ZYDIS_MNEMONIC_FNSAVE: case ZYDIS_MNEMONIC_FRSTOR:
            for (int k : {FTOP, FTAG, FPCW, FPSW}) mark_fx(k);
            x87_ = true;
            line(std::format("rt::{}(m, {}, c.st, ftop, ftag, fpcw, fpsw, 0x{:X}u);", M == ZYDIS_MNEMONIC_FNSAVE ? "fnsave" : "frstor",
                             addr(i, d), i.addr));
            return true;
        case ZYDIS_MNEMONIC_FNSTCW:
            use_fx(FPCW);
            line(std::format("rt::st16(m, {}, uint16_t(fpcw), 0x{:X}u);", addr(i, d), i.addr));
            return true;
        case ZYDIS_MNEMONIC_FLDCW:
            mark_fx(FPCW);
            line(std::format("fpcw = uint32_t(rt::ld16(m, {}, 0x{:X}u));", addr(i, d), i.addr));
            return true;
        case ZYDIS_MNEMONIC_FWAIT: case ZYDIS_MNEMONIC_FNOP:
            return true;
        case ZYDIS_MNEMONIC_FFREE:
            mark_fx(FTAG);
            line(std::format("ftag &= ~(1u << ((ftop + {}u) & 7u));", st_index(i, d)));
            return true;
        case ZYDIS_MNEMONIC_FRNDINT:
            use_fx(FPCW);
            line("ST(0u) = rt::frndint(fpcw, ST(0u));");
            return true;
        case ZYDIS_MNEMONIC_F2XM1:
            line("ST(0u) = rt::f2xm1(ST(0u));");
            return true;
        case ZYDIS_MNEMONIC_FSCALE:
            line("ST(0u) = rt::fscale(ST(0u), ST(1u));");
            return true;
        case ZYDIS_MNEMONIC_FXAM:
            mark_fx(FPSW);
            use_fx(FTAG);
            line("fpsw = rt::fxam_codes(fpsw, ((ftag >> (ftop & 7u)) & 1u) == 0, ST(0u));");
            return true;
        case ZYDIS_MNEMONIC_FNCLEX:  // clears the exception flags, keeps the condition codes
            mark_fx(FPSW);
            line("fpsw &= 0x4700u;");
            return true;
        case ZYDIS_MNEMONIC_FINCSTP: mark_fx(FTOP); line("ftop = (ftop + 1u) & 7u;"); return true;
        case ZYDIS_MNEMONIC_FDECSTP: mark_fx(FTOP); line("ftop = (ftop - 1u) & 7u;"); return true;
        default:
            return false;
    }
}

// Forward analysis of esp (and ebp) as constant offsets from esp at entry.
// Every `ret` must find esp back at offset 0, and nothing may pop the return
// address; otherwise this "function" is a fragment of another one (or not
// code), and lifting it would be wrong.
void FnEmitter::check_stack_balance(const std::vector<std::vector<size_t>>& succ) {
    constexpr int64_t UNK = kStackUnknown;
    std::vector<StackState> in(ins_.size());
    const size_t entry = idx_.at(f_.start);
    in[entry] = {0, UNK, true};
    std::vector<size_t> work{entry};
    while (!work.empty()) {
        const size_t k = work.back();
        work.pop_back();
        StackState st = in[k];
        const Insn& i = *ins_[k];
        const Operand& d = i.op[0];
        const Operand& s = i.op[1];
        auto add = [&](int64_t v) {
            if (st.esp != UNK) st.esp += v;
        };
        switch (i.mnem) {
            case ZYDIS_MNEMONIC_PUSH: add(-4); break;
            case ZYDIS_MNEMONIC_POP:
                if (st.esp != UNK && st.esp >= 0)
                    fail(i, LiftFailure::Kind::Fragment, "pops its own return address");
                add(4);
                if (d.is_reg(ZYDIS_REGISTER_ESP)) st.esp = UNK;
                if (d.is_reg(ZYDIS_REGISTER_EBP)) st.ebp = UNK;
                break;
            case ZYDIS_MNEMONIC_LEAVE:
                st.esp = st.ebp == UNK ? UNK : st.ebp + 4;
                st.ebp = UNK;
                break;
            case ZYDIS_MNEMONIC_CALL:
                // The callee pops its return address, and its arguments too
                // if it ends in `ret n`: n isn't known here.
                st.esp = UNK;
                break;
            case ZYDIS_MNEMONIC_RET:
                if (st.esp != UNK && st.esp != 0)
                    fail(i, LiftFailure::Kind::Fragment, std::format("stack not balanced at ret (esp {:+})", st.esp));
                break;
            default:
                if (i.nops >= 1 && d.is_reg(ZYDIS_REGISTER_ESP)) {
                    if (i.mnem == ZYDIS_MNEMONIC_SUB && s.kind == Operand::Kind::Imm) add(-int64_t(int32_t(s.imm)));
                    else if (i.mnem == ZYDIS_MNEMONIC_ADD && s.kind == Operand::Kind::Imm) add(int64_t(int32_t(s.imm)));
                    else if (i.mnem == ZYDIS_MNEMONIC_MOV && s.is_reg(ZYDIS_REGISTER_EBP)) st.esp = st.ebp;
                    else if (i.mnem == ZYDIS_MNEMONIC_LEA && s.index == ZYDIS_REGISTER_NONE &&
                             (s.base == ZYDIS_REGISTER_ESP || s.base == ZYDIS_REGISTER_EBP)) {
                        const int64_t b = s.base == ZYDIS_REGISTER_ESP ? st.esp : st.ebp;
                        st.esp = b == UNK ? UNK : b + s.disp;
                    } else if (i.mnem != ZYDIS_MNEMONIC_CMP && i.mnem != ZYDIS_MNEMONIC_TEST) {
                        st.esp = UNK;
                    }
                } else if (i.nops >= 1 && d.is_reg(ZYDIS_REGISTER_EBP) && i.mnem != ZYDIS_MNEMONIC_CMP &&
                           i.mnem != ZYDIS_MNEMONIC_TEST && i.mnem != ZYDIS_MNEMONIC_PUSH) {
                    st.ebp = (i.mnem == ZYDIS_MNEMONIC_MOV && s.is_reg(ZYDIS_REGISTER_ESP)) ? st.esp : UNK;
                }
                break;
        }
        for (const size_t t : succ[k]) {
            StackState& o = in[t];
            if (!o.seen) {
                o = st;
                o.seen = true;
                work.push_back(t);
                continue;
            }
            StackState merged = o;
            if (merged.esp != st.esp) merged.esp = UNK;
            if (merged.ebp != st.ebp) merged.ebp = UNK;
            if (merged.esp != o.esp || merged.ebp != o.ebp) {
                o = merged;
                work.push_back(t);
            }
        }
    }
}

std::string FnEmitter::run() {
    for (const uint32_t a : f_.insns) {
        idx_[a] = ins_.size();
        ins_.push_back(dec_.decode(a));
    }
    const size_t n = ins_.size();

    // Successors.
    std::vector<std::vector<size_t>> succ(n);
    std::vector<bool> falls(n, false);
    auto need_idx = [&](const Insn& i, uint32_t t) -> size_t {
        auto it = idx_.find(t);
        if (it == idx_.end())
            fail(i, LiftFailure::Kind::BadControlFlow, std::format("branch target {:08X} is not an instruction of this function", t));
        return it->second;
    };
    for (size_t k = 0; k < n; ++k) {
        const Insn& i = *ins_[k];
        if (const char* why = i.implausible_reason()) fail(i, LiftFailure::Kind::Implausible, why);
        if (i.is_ret() || i.mnem == ZYDIS_MNEMONIC_INT3) continue;
        if (i.is_jmp()) {
            if (auto t = i.rel_target()) {
                if (idx_.contains(*t)) succ[k].push_back(idx_.at(*t));  // else a tail call: an exit
            } else if (auto jt = std::ranges::find(f_.jtabs, i.addr, &JumpTable::at); jt != f_.jtabs.end()) {
                for (const uint32_t x : jt->targets) succ[k].push_back(need_idx(i, x));
            }
            continue;
        }
        if (i.is_cond_branch()) {
            auto t = i.rel_target();
            if (!t) fail(i, LiftFailure::Kind::BadControlFlow, "conditional branch without a target");
            if (idx_.contains(*t)) succ[k].push_back(idx_.at(*t));  // else a conditional tail call
        }
        // A call the CFG took as no-return (int3 or the end of the function
        // follows) has no fallthrough; the generated code traps if it returns.
        if (i.is_call() && !idx_.contains(i.end())) continue;
        falls[k] = true;
        succ[k].push_back(need_idx(i, i.end()));
    }

    if (!(conv_ && conv_->stack_glue.contains(f_.start))) check_stack_balance(succ);

    // Backward flag liveness to a fixpoint.
    std::vector<uint8_t> live_in(n, 0), live_out(n, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = n; k-- > 0;) {
            uint8_t out = 0;
            for (const size_t s : succ[k]) out |= live_in[s];
            const FlagFx fx = fx_of(*ins_[k]);
            const uint8_t in = fx.use | (out & ~fx.kill);
            if (out != live_out[k] || in != live_in[k]) {
                live_out[k] = out;
                live_in[k] = in;
                changed = true;
            }
        }
    }
    if (const uint8_t in = flags_in(f_.start); in) {
        line(unpack_flags(in));  // first line of the body
    }
    if (live_in[idx_.at(f_.start)] & ~flags_in(f_.start))
        throw Abort{{LiftFailure::Kind::Fragment, f_.start, {}, {}, "flags are read before being written"}};

    // Direction flag, forward: clear on entry and after a call (MSVC ABI), set
    // by std, cleared by cld; 2 = differs between paths. It must be clear at
    // every call and exit.
    std::vector<uint8_t> df(n, 0xFF);
    {
        std::vector<bool> has_pred(n, false);
        for (size_t k = 0; k < n; ++k)
            for (const size_t t : succ[k]) has_pred[t] = true;
        std::vector<size_t> work;
        for (size_t k = 0; k < n; ++k)
            if (!has_pred[k] || ins_[k]->addr == f_.start) {
                df[k] = 0;
                work.push_back(k);
            }
        while (!work.empty()) {
            const size_t k = work.back();
            work.pop_back();
            const ZydisMnemonic mn = ins_[k]->mnem;
            const uint8_t out = mn == ZYDIS_MNEMONIC_STD ? 1 : (mn == ZYDIS_MNEMONIC_CLD || ins_[k]->is_call()) ? 0 : df[k];
            for (const size_t t : succ[k]) {
                const uint8_t v = df[t] == 0xFF ? out : (df[t] == out ? out : 2);
                if (v != df[t]) {
                    df[t] = v;
                    work.push_back(t);
                }
            }
        }
        for (size_t k = 0; k < n; ++k) {
            const Insn& i = *ins_[k];
            const bool exits = i.is_call() || i.is_ret() || (i.is_jmp() && !(i.rel_target() && idx_.contains(*i.rel_target()))) ||
                               (i.is_cond_branch() && i.rel_target() && !idx_.contains(*i.rel_target()));
            if (exits && df[k] != 0 && df[k] != 0xFF) unsupported(i, "direction flag set at a call or exit");
        }
    }

    // Labels: every branch target and every non-adjacent fallthrough.
    std::set<uint32_t> labels;
    for (size_t k = 0; k < n; ++k) {
        for (const size_t s : succ[k])
            if (!(falls[k] && s == k + 1 && ins_[k]->end() == ins_[s]->addr)) labels.insert(ins_[s]->addr);
        // A branch to the next instruction (jnz $+2) still names its target.
        if (ins_[k]->is_cond_branch())
            if (auto t = ins_[k]->rel_target(); t && idx_.contains(*t)) labels.insert(*t);
    }
    if (ins_.front()->addr != f_.start) labels.insert(f_.start);

    if (ins_.front()->addr != f_.start) out_ += std::format("    goto L_{:08X};\n", f_.start);
    for (size_t k = 0; k < n; ++k) {
        const Insn& i = *ins_[k];
        if (labels.contains(i.addr)) out_ += std::format("L_{:08X}:;\n", i.addr);
        out_ += std::format("    // {:08X}  {}\n", i.addr, dec_.format(i));
        df_now_ = df[k] == 0xFF ? 0 : df[k];
        emit(i, live_out[k]);
        if (falls[k] && !(k + 1 < n && ins_[k + 1]->addr == i.end())) out_ += std::format("    goto L_{:08X};\n", i.end());
    }

    // Prologue: load the registers this function references. Epilogue (at
    // every ret): store back only the ones it can modify.
    std::string gprs, xmms, wb;
    for (int g = 0; g < 8; ++g) {
        if (used_ & (1u << g)) gprs += std::format("{}{} = c.{}", gprs.empty() ? "" : ", ", kGpr[size_t(g)], kGpr[size_t(g)]);
        if (written_ & (1u << g)) wb += std::format(" c.{0} = {0};", kGpr[size_t(g)]);
    }
    for (int x = 0; x < 8; ++x) {
        if (used_ & (1u << (8 + x))) xmms += std::format("{}xmm{} = c.xmm[{}]", xmms.empty() ? "" : ", ", x, x);
        if (written_ & (1u << (8 + x))) wb += std::format(" c.xmm[{0}] = xmm{0};", x);
    }
    std::string fx;
    for (int k = 0; k < 4; ++k) {
        if (used_ & (1u << (16 + k))) fx += std::format("{}{} = c.{}", fx.empty() ? "" : ", ", kX87[size_t(k)], kX87[size_t(k)]);
        if (written_ & (1u << (16 + k))) wb += std::format(" c.{0} = {0};", kX87[size_t(k)]);
    }

    // The MMX registers the function names: locals too, loaded as it's
    // entered, stored at every exit and call, and read again after a call
    // (all of them: which it changes isn't tracked). In the context each
    // operation loaded and stored its register, as the host compiler can't
    // tell that guest memory isn't the context.
    std::string mms, mm_reload;
    for (int k = 0; k < 8; ++k) {
        const std::string in_context = std::format("c.mm[{}]", k), local = std::format("mm{}", k);
        if (out_.find(in_context) == std::string::npos) continue;
        for (size_t at = 0; (at = out_.find(in_context, at)) != std::string::npos; at += local.size()) out_.replace(at, in_context.size(), local);
        mms += std::format("{}{} = {}", mms.empty() ? "" : ", ", local, in_context);
        wb += std::format(" {} = {};", in_context, local);
        mm_reload += std::format(" {} = {};", local, in_context);
    }

    std::string fn = std::format("void {}(rt::Context& c) {{\n", Lifter::name(f_.start));
    fn += std::format("    QILIN_FRAME(0x{:X}u, c.esp);\n", f_.start);
    fn += "    [[maybe_unused]] const rt::Mem m{c.mem, c.mem_size};\n";
    if (!gprs.empty()) fn += std::format("    uint32_t {};\n", gprs);
    if (!xmms.empty()) fn += std::format("    rt::Xmm {};\n", xmms);
    if (!mms.empty()) fn += std::format("    uint64_t {};\n", mms);
    if (!fx.empty()) fn += std::format("    uint32_t {};\n", fx);
    if (x87_) fn += "    [[maybe_unused]] auto ST = [&](uint32_t k) -> double& { return c.st[(ftop + k) & 7u]; };\n";
    fn += "    [[maybe_unused]] bool cf = false, pf = false, af = false, zf = false, sf = false, of = false;\n";
    fn += std::format("    [[maybe_unused]] auto writeback = [&] {{{} }};\n", wb);
    if (calls_) {
        // After a call: the callee may have changed any register.
        std::string rl;
        for (int g = 0; g < 8; ++g)
            if (used_ & (1u << g)) rl += std::format(" {0} = c.{0};", kGpr[size_t(g)]);
        for (int x = 0; x < 8; ++x)
            if (used_ & (1u << (8 + x))) rl += std::format(" xmm{0} = c.xmm[{0}];", x);
        for (int k = 0; k < 4; ++k)
            if (used_ & (1u << (16 + k))) rl += std::format(" {0} = c.{0};", kX87[size_t(k)]);
        rl += mm_reload;
        fn += std::format("    [[maybe_unused]] auto reload = [&] {{{} }};\n", rl);
    }
    fn += out_;
    fn += "}\n";
    return fn;
}

}  // namespace

std::string Lifter::name(uint32_t addr) { return std::format("fn_{:08X}", addr); }

namespace {
constexpr struct { uint8_t bit; const char* name; int x86; } kFlagBits[] = {
    {CF, "cf", 0}, {PF, "pf", 2}, {AF, "af", 4}, {ZF, "zf", 6}, {SF, "sf", 7}, {OF, "of", 11}};
}  // namespace

std::string FnEmitter::pack_flags(uint8_t mask) {
    std::string e = "c.eflags = 0u";
    for (auto& f : kFlagBits)
        if (mask & f.bit) e += std::format(" | ({} ? 0x{:X}u : 0u)", f.name, 1u << f.x86);
    return e + ";";
}

std::string FnEmitter::unpack_flags(uint8_t mask) {
    std::string e;
    for (auto& f : kFlagBits)
        if (mask & f.bit) e += std::format("{}{} = (c.eflags & 0x{:X}u) != 0;", e.empty() ? "" : " ", f.name, 1u << f.x86);
    return e;
}

std::expected<std::string, LiftFailure> Lifter::lift(const Function& f) {
    try {
        FnEmitter e(dec_, f, conv_);
        return e.run();
    } catch (const Abort& a) {
        return std::unexpected(a.f);
    }
}

}  // namespace qilin
