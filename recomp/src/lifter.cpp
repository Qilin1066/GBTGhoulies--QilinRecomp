#include "lifter.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace qilin {

namespace {

// ------------------------------------------------------------------ flags
enum : uint8_t { CF = 1, PF = 2, AF = 4, ZF = 8, SF = 16, OF = 32, ALLF = 63 };

struct LiftError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

[[noreturn]] void fail(const Insn& i, const Decoder& d, const std::string& why) {
    throw LiftError(fmt("%08X %s: %s", i.addr, d.format(i).c_str(), why.c_str()));
}

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
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return Cond{SF | OF, "(sf != of)"};
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return Cond{SF | OF, "(sf == of)"};
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return Cond{ZF | SF | OF, "(zf || sf != of)"};
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return Cond{ZF | SF | OF, "(!zf && sf == of)"};
        default: return std::nullopt;
    }
}

bool is_setcc(ZydisMnemonic m) { return m >= ZYDIS_MNEMONIC_SETB && m <= ZYDIS_MNEMONIC_SETZ && m != ZYDIS_MNEMONIC_SETSSBSY; }
bool is_cmovcc(ZydisMnemonic m) { return m >= ZYDIS_MNEMONIC_CMOVB && m <= ZYDIS_MNEMONIC_CMOVZ; }

// Flag effects: `use` = read, `kill` = definitely overwritten (for liveness),
// `defined` = overwritten with an architecturally defined value. Reading a
// flag that was killed but not defined (an "undefined" flag) is an error.
struct FlagFx {
    uint8_t use = 0, kill = 0, defined = 0;
};

bool shift_by_cl(const Insn& i) { return i.nops >= 2 && i.op[1].kind == Operand::Kind::Reg; }
uint32_t shift_imm(const Insn& i) { return i.nops >= 2 ? uint32_t(i.op[1].imm & 31) : 1; }

FlagFx flag_fx(const Insn& i) {
    using M = ZydisMnemonic;
    M m = i.mnem;
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
            if (shift_by_cl(i)) return {0, 0, 0};  // count may be 0: flags pass through
            uint32_t n = shift_imm(i);
            if (n == 0) return {0, 0, 0};
            uint8_t def = CF | ZF | SF | PF;
            if (n == 1) def |= OF;
            if (m == ZYDIS_MNEMONIC_SAR) def |= OF;  // SAR clears OF
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
        default:
            return {};
    }
}

// ------------------------------------------------------------------ registers
struct RegInfo {
    const char* base;  // 32-bit local holding it
    int width;         // 8, 16, 32
    bool high8;        // ah/ch/dh/bh
};

std::optional<RegInfo> gpr(ZydisRegister r) {
    static const char* n32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI) return RegInfo{n32[r - ZYDIS_REGISTER_EAX], 32, false};
    if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) return RegInfo{n32[r - ZYDIS_REGISTER_AX], 16, false};
    switch (r) {
        case ZYDIS_REGISTER_AL: return RegInfo{"eax", 8, false};
        case ZYDIS_REGISTER_CL: return RegInfo{"ecx", 8, false};
        case ZYDIS_REGISTER_DL: return RegInfo{"edx", 8, false};
        case ZYDIS_REGISTER_BL: return RegInfo{"ebx", 8, false};
        case ZYDIS_REGISTER_AH: return RegInfo{"eax", 8, true};
        case ZYDIS_REGISTER_CH: return RegInfo{"ecx", 8, true};
        case ZYDIS_REGISTER_DH: return RegInfo{"edx", 8, true};
        case ZYDIS_REGISTER_BH: return RegInfo{"ebx", 8, true};
        default: return std::nullopt;
    }
}

int xmm_index(ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM7) return r - ZYDIS_REGISTER_XMM0;
    return -1;
}

uint32_t mask_of(int w) { return w == 32 ? 0xFFFFFFFFu : (1u << w) - 1; }
uint32_t sign_of(int w) { return 1u << (w - 1); }

// ------------------------------------------------------------------ emitter
class FnEmitter {
public:
    FnEmitter(Decoder& d, const Function& f) : dec_(d), f_(f) {}

    std::string run();

private:
    // operand helpers
    std::string addr(const Insn& i, const Operand& o);
    std::string read(const Insn& i, const Operand& o, int w);
    std::string write(const Insn& i, const Operand& o, int w, const std::string& v);
    std::string xmm_lane0(const Insn& i, const Operand& o);  // 32-bit source: xmm lane 0 or m32
    std::string xmm128(const Insn& i, const Operand& o, bool aligned);
    int width(const Insn& i, const Operand& o);

    void line(const std::string& s) { out_ += "    " + s + "\n"; }
    void emit(const Insn& i, uint8_t live_out);
    void emit_flags_szp(uint8_t need, int w);
    void check_stack_balance(const std::vector<std::vector<size_t>>& succ);
    std::string label(uint32_t a) { labels_used_.insert(a); return fmt("L_%08X", a); }

    Decoder& dec_;
    const Function& f_;
    std::vector<const Insn*> ins_;
    std::unordered_map<uint32_t, size_t> idx_;
    std::set<uint32_t> labels_used_;
    std::string out_;
};

int FnEmitter::width(const Insn& i, const Operand& o) {
    int w = o.size;
    if (w != 8 && w != 16 && w != 32) fail(i, dec_, fmt("unsupported operand width %d", w));
    return w;
}

std::string FnEmitter::addr(const Insn& i, const Operand& o) {
    if (o.seg != ZYDIS_REGISTER_DS && o.seg != ZYDIS_REGISTER_SS && o.seg != ZYDIS_REGISTER_ES &&
        o.seg != ZYDIS_REGISTER_NONE)
        fail(i, dec_, "segment override (fs:/gs:) not supported yet");
    std::string e;
    if (o.base != ZYDIS_REGISTER_NONE) {
        auto r = gpr(o.base);
        if (!r || r->width != 32) fail(i, dec_, "unsupported base register");
        e = r->base;
    }
    if (o.index != ZYDIS_REGISTER_NONE) {
        auto r = gpr(o.index);
        if (!r || r->width != 32) fail(i, dec_, "unsupported index register");
        if (!e.empty()) e += " + ";
        e += r->base;
        if (o.scale > 1) e += fmt(" * %uu", o.scale);
    }
    uint32_t d = uint32_t(o.disp);
    if (d || e.empty()) {
        if (!e.empty()) e += " + ";
        e += fmt("0x%Xu", d);
    }
    return "uint32_t(" + e + ")";
}

std::string FnEmitter::read(const Insn& i, const Operand& o, int w) {
    switch (o.kind) {
        case Operand::Kind::Reg: {
            auto r = gpr(o.reg);
            if (!r) fail(i, dec_, "unsupported register operand");
            if (r->width == 32) return r->base;
            if (r->width == 16) return fmt("(%s & 0xFFFFu)", r->base);
            if (r->high8) return fmt("((%s >> 8) & 0xFFu)", r->base);
            return fmt("(%s & 0xFFu)", r->base);
        }
        case Operand::Kind::Mem: {
            std::string a = addr(i, o);
            if (w == 8) return fmt("uint32_t(rt::ld8(m, %s, 0x%Xu))", a.c_str(), i.addr);
            if (w == 16) return fmt("uint32_t(rt::ld16(m, %s, 0x%Xu))", a.c_str(), i.addr);
            if (w == 32) return fmt("rt::ld32(m, %s, 0x%Xu)", a.c_str(), i.addr);
            fail(i, dec_, "unsupported memory width");
        }
        case Operand::Kind::Imm:
            return fmt("0x%Xu", uint32_t(o.imm) & mask_of(w));
        default:
            fail(i, dec_, "unsupported operand kind");
    }
}

std::string FnEmitter::write(const Insn& i, const Operand& o, int w, const std::string& v) {
    switch (o.kind) {
        case Operand::Kind::Reg: {
            auto r = gpr(o.reg);
            if (!r) fail(i, dec_, "unsupported destination register");
            if (r->width == 32) return fmt("%s = %s;", r->base, v.c_str());
            if (r->width == 16) return fmt("rt::set_lo16(%s, %s);", r->base, v.c_str());
            if (r->high8) return fmt("rt::set_hi8(%s, %s);", r->base, v.c_str());
            return fmt("rt::set_lo8(%s, %s);", r->base, v.c_str());
        }
        case Operand::Kind::Mem: {
            std::string a = addr(i, o);
            if (w == 8) return fmt("rt::st8(m, %s, uint8_t(%s), 0x%Xu);", a.c_str(), v.c_str(), i.addr);
            if (w == 16) return fmt("rt::st16(m, %s, uint16_t(%s), 0x%Xu);", a.c_str(), v.c_str(), i.addr);
            if (w == 32) return fmt("rt::st32(m, %s, %s, 0x%Xu);", a.c_str(), v.c_str(), i.addr);
            fail(i, dec_, "unsupported memory width");
        }
        default:
            fail(i, dec_, "unsupported destination");
    }
}

std::string FnEmitter::xmm_lane0(const Insn& i, const Operand& o) {
    if (o.kind == Operand::Kind::Reg) {
        int x = xmm_index(o.reg);
        if (x < 0) fail(i, dec_, "expected xmm register");
        return fmt("xmm%d.u[0]", x);
    }
    if (o.kind == Operand::Kind::Mem) return fmt("rt::ld32(m, %s, 0x%Xu)", addr(i, o).c_str(), i.addr);
    fail(i, dec_, "unsupported SSE operand");
}

std::string FnEmitter::xmm128(const Insn& i, const Operand& o, bool aligned) {
    if (o.kind == Operand::Kind::Reg) {
        int x = xmm_index(o.reg);
        if (x < 0) fail(i, dec_, "expected xmm register");
        return fmt("xmm%d", x);
    }
    if (o.kind == Operand::Kind::Mem)
        return fmt("rt::ld128(m, %s, %s, 0x%Xu)", addr(i, o).c_str(), aligned ? "true" : "false", i.addr);
    fail(i, dec_, "unsupported SSE operand");
}

void FnEmitter::emit_flags_szp(uint8_t need, int w) {
    if (need & ZF) line("zf = r == 0;");
    if (need & SF) line(fmt("sf = (r & 0x%Xu) != 0;", sign_of(w)));
    if (need & PF) line("pf = rt::parity8(r);");
}

void FnEmitter::emit(const Insn& i, uint8_t live_out) {
    FlagFx fx = flag_fx(i);
    uint8_t need = live_out & fx.kill;
    if (need & ~fx.defined) fail(i, dec_, "a later instruction reads a flag this one leaves undefined");
    // For CL-count shifts flags are conditionally written; compute whatever is live.
    if ((i.mnem == ZYDIS_MNEMONIC_SHL || i.mnem == ZYDIS_MNEMONIC_SHR || i.mnem == ZYDIS_MNEMONIC_SAR) && shift_by_cl(i))
        need = live_out & (CF | ZF | SF | PF);

    const Operand& d = i.op[0];
    const Operand& s = i.op[1];
    auto M = i.mnem;

    if (auto c = cond_of(M)) {
        if (i.is_cond_branch()) {
            auto t = i.rel_target();
            line(fmt("if (%s) goto %s;", c->expr, label(*t).c_str()));
            return;
        }
        if (is_setcc(M)) { line(write(i, d, 8, fmt("uint32_t(%s)", c->expr))); return; }
        if (is_cmovcc(M)) {
            int w = width(i, d);
            line(fmt("{ uint32_t v = %s; if (%s) { %s } }", read(i, s, w).c_str(), c->expr, write(i, d, w, "v").c_str()));
            return;
        }
    }

    switch (M) {
        case ZYDIS_MNEMONIC_NOP:
        case ZYDIS_MNEMONIC_CLD:  // DF is assumed clear everywhere (MSVC ABI)
        case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0: case ZYDIS_MNEMONIC_PREFETCHT1:
        case ZYDIS_MNEMONIC_PREFETCHT2:
            return;
        case ZYDIS_MNEMONIC_INT3:
            line(fmt("rt::trap(0x%Xu, \"int3\");", i.addr));
            return;
        case ZYDIS_MNEMONIC_MOV: {
            int w = width(i, d);
            if (d.kind == Operand::Kind::Reg && s.kind == Operand::Kind::Reg && d.reg == s.reg) return;  // mov edi, edi
            line(write(i, d, w, read(i, s, w)));
            return;
        }
        case ZYDIS_MNEMONIC_MOVZX: case ZYDIS_MNEMONIC_MOVSX: {
            int wd = width(i, d), ws = width(i, s);
            std::string v = read(i, s, ws);
            if (M == ZYDIS_MNEMONIC_MOVSX)
                v = ws == 8 ? fmt("uint32_t(int32_t(int8_t(%s)))", v.c_str()) : fmt("uint32_t(int32_t(int16_t(%s)))", v.c_str());
            line(write(i, d, wd, fmt("(%s & 0x%Xu)", v.c_str(), mask_of(wd))));
            return;
        }
        case ZYDIS_MNEMONIC_LEA: {
            if (width(i, d) != 32) fail(i, dec_, "16-bit lea");
            line(write(i, d, 32, addr(i, s)));
            return;
        }
        case ZYDIS_MNEMONIC_XCHG: {
            int w = width(i, d);
            line(fmt("{ uint32_t a = %s, b = %s; %s %s }", read(i, d, w).c_str(), read(i, s, w).c_str(),
                     write(i, d, w, "b").c_str(), write(i, s, w, "a").c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_ADC:
        case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR:
        case ZYDIS_MNEMONIC_TEST: {
            int w = width(i, d);
            uint32_t mk = mask_of(w), sg = sign_of(w);
            line("{");
            // xor r, r / sub r, r: result and flags are constant (common idiom).
            bool same = d.kind == Operand::Kind::Reg && s.kind == Operand::Kind::Reg && d.reg == s.reg &&
                        (M == ZYDIS_MNEMONIC_XOR || M == ZYDIS_MNEMONIC_SUB);
            line(fmt("uint32_t a = %s, b = %s;", same ? "0u" : read(i, d, w).c_str(), same ? "0u" : read(i, s, w).c_str()));
            const char* expr = nullptr;
            switch (M) {
                case ZYDIS_MNEMONIC_ADD: expr = "a + b"; break;
                case ZYDIS_MNEMONIC_SUB: case ZYDIS_MNEMONIC_CMP: expr = "a - b"; break;
                case ZYDIS_MNEMONIC_ADC: expr = "a + b + uint32_t(cf)"; break;
                case ZYDIS_MNEMONIC_SBB: expr = "a - b - uint32_t(cf)"; break;
                case ZYDIS_MNEMONIC_AND: case ZYDIS_MNEMONIC_TEST: expr = "a & b"; break;
                default: expr = "a | b"; break;  // OR; XOR handled below
            }
            if (M == ZYDIS_MNEMONIC_XOR) expr = "a ^ b";
            line(fmt("[[maybe_unused]] uint32_t r = (%s) & 0x%Xu;", expr, mk));
            bool arith_add = M == ZYDIS_MNEMONIC_ADD || M == ZYDIS_MNEMONIC_ADC;
            bool arith_sub = M == ZYDIS_MNEMONIC_SUB || M == ZYDIS_MNEMONIC_CMP || M == ZYDIS_MNEMONIC_SBB;
            if (arith_add || arith_sub) {
                if (need & CF) {
                    if (M == ZYDIS_MNEMONIC_ADD) line("cf = r < a;");
                    else if (M == ZYDIS_MNEMONIC_ADC) line("cf = cf ? (r <= a) : (r < a);");
                    else if (M == ZYDIS_MNEMONIC_SBB) line("cf = cf ? (a <= b) : (a < b);");
                    else line("cf = a < b;");
                }
                if (need & OF) {
                    if (arith_add) line(fmt("of = ((a ^ r) & (b ^ r) & 0x%Xu) != 0;", sg));
                    else line(fmt("of = ((a ^ b) & (a ^ r) & 0x%Xu) != 0;", sg));
                }
                if (need & AF) line("af = ((a ^ b ^ r) & 0x10u) != 0;");
            } else {
                if (need & CF) line("cf = false;");
                if (need & OF) line("of = false;");
            }
            emit_flags_szp(need, w);
            if (M != ZYDIS_MNEMONIC_CMP && M != ZYDIS_MNEMONIC_TEST) line(write(i, d, w, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG: case ZYDIS_MNEMONIC_NOT: {
            int w = width(i, d);
            uint32_t mk = mask_of(w), sg = sign_of(w);
            line("{");
            line(fmt("uint32_t a = %s;", read(i, d, w).c_str()));
            if (M == ZYDIS_MNEMONIC_INC) {
                line(fmt("uint32_t r = (a + 1u) & 0x%Xu;", mk));
                if (need & OF) line(fmt("of = r == 0x%Xu;", sg));
                if (need & AF) line("af = (r & 0xFu) == 0;");
            } else if (M == ZYDIS_MNEMONIC_DEC) {
                line(fmt("uint32_t r = (a - 1u) & 0x%Xu;", mk));
                if (need & OF) line(fmt("of = a == 0x%Xu;", sg));
                if (need & AF) line("af = (a & 0xFu) == 0;");
            } else if (M == ZYDIS_MNEMONIC_NEG) {
                line(fmt("uint32_t r = (0u - a) & 0x%Xu;", mk));
                if (need & CF) line("cf = a != 0;");
                if (need & OF) line(fmt("of = a == 0x%Xu;", sg));
                if (need & AF) line("af = ((a ^ r) & 0x10u) != 0;");
            } else {
                line(fmt("uint32_t r = (~a) & 0x%Xu;", mk));
            }
            if (M != ZYDIS_MNEMONIC_NOT) emit_flags_szp(need, w);
            line(write(i, d, w, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: {
            int w = width(i, d);
            uint32_t mk = mask_of(w), sg = sign_of(w);
            std::string cnt = i.nops >= 2 ? (shift_by_cl(i) ? "(ecx & 31u)" : fmt("%uu", shift_imm(i))) : "1u";
            if (i.nops >= 2 && !shift_by_cl(i) && shift_imm(i) == 0) return;  // no-op, flags unchanged
            line("{");
            line(fmt("uint32_t n = %s;", cnt.c_str()));
            line("if (n) {");
            line(fmt("uint32_t a = %s;", read(i, d, w).c_str()));
            if (M == ZYDIS_MNEMONIC_SHL) {
                line(fmt("uint64_t wide = uint64_t(a) << n; uint32_t r = uint32_t(wide) & 0x%Xu;", mk));
                if (need & CF) line(fmt("cf = ((wide >> %d) & 1u) != 0;", w));
                if (need & OF) line(fmt("of = ((r & 0x%Xu) != 0) != (((wide >> %d) & 1u) != 0);", sg, w));
            } else if (M == ZYDIS_MNEMONIC_SHR) {
                line(fmt("uint32_t r = (n < 32u ? a >> n : 0u) & 0x%Xu;", mk));
                if (need & CF) line("cf = ((a >> (n - 1u)) & 1u) != 0;");
                if (need & OF) line(fmt("of = (a & 0x%Xu) != 0;", sg));
            } else {
                std::string sx = w == 32 ? "int32_t(a)" : w == 16 ? "int32_t(int16_t(a))" : "int32_t(int8_t(a))";
                line(fmt("int32_t sa = %s; uint32_t r = uint32_t(sa >> (n > 31u ? 31u : n)) & 0x%Xu;", sx.c_str(), mk));
                if (need & CF) line("cf = ((sa >> (n - 1u > 31u ? 31u : n - 1u)) & 1) != 0;");
                if (need & OF) line("of = false;");
            }
            emit_flags_szp(need, w);
            line(write(i, d, w, "r"));
            line("}");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_MUL: {
            int w = width(i, d);
            if (i.nops == 1) {  // edx:eax = eax * r/m32
                if (w != 32) fail(i, dec_, "8/16-bit one-operand multiply");
                line("{");
                if (M == ZYDIS_MNEMONIC_IMUL) {
                    line(fmt("int64_t p = int64_t(int32_t(eax)) * int64_t(int32_t(%s));", read(i, d, 32).c_str()));
                    line("eax = uint32_t(uint64_t(p)); edx = uint32_t(uint64_t(p) >> 32);");
                    if (need & (CF | OF)) line("cf = of = p != int64_t(int32_t(eax));");
                } else {
                    line(fmt("uint64_t p = uint64_t(eax) * uint64_t(%s);", read(i, d, 32).c_str()));
                    line("eax = uint32_t(p); edx = uint32_t(p >> 32);");
                    if (need & (CF | OF)) line("cf = of = edx != 0;");
                }
                line("}");
                return;
            }
            if (w != 32) fail(i, dec_, "16-bit imul");
            const Operand& a = i.nops == 3 ? i.op[1] : i.op[0];
            const Operand& b = i.nops == 3 ? i.op[2] : i.op[1];
            line("{");
            line(fmt("int64_t p = int64_t(int32_t(%s)) * int64_t(int32_t(%s));", read(i, a, 32).c_str(),
                     read(i, b, 32).c_str()));
            line("uint32_t r = uint32_t(uint64_t(p));");
            if (need & (CF | OF)) line("cf = of = p != int64_t(int32_t(r));");
            line(write(i, d, 32, "r"));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_DIV: case ZYDIS_MNEMONIC_IDIV: {
            if (width(i, d) != 32) fail(i, dec_, "8/16-bit divide");
            line("{");
            line(fmt("uint32_t dv = %s;", read(i, d, 32).c_str()));
            line(fmt("if (dv == 0) rt::trap(0x%Xu, \"divide by zero (#DE)\");", i.addr));
            if (M == ZYDIS_MNEMONIC_DIV) {
                line("uint64_t n = (uint64_t(edx) << 32) | eax; uint64_t q = n / dv;");
                line(fmt("if (q > 0xFFFFFFFFull) rt::trap(0x%Xu, \"divide overflow (#DE)\");", i.addr));
                line("eax = uint32_t(q); edx = uint32_t(n % dv);");
            } else {
                line("int64_t n = int64_t((uint64_t(edx) << 32) | eax); int64_t sd = int32_t(dv);");
                line(fmt("if (n == INT64_MIN && sd == -1) rt::trap(0x%Xu, \"divide overflow (#DE)\");", i.addr));
                line("int64_t q = n / sd;");
                line(fmt("if (q > INT32_MAX || q < INT32_MIN) rt::trap(0x%Xu, \"divide overflow (#DE)\");", i.addr));
                line("eax = uint32_t(int32_t(q)); edx = uint32_t(int32_t(n % sd));");
            }
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_CDQ: line("edx = uint32_t(int32_t(eax) >> 31);"); return;
        case ZYDIS_MNEMONIC_CWDE: line("eax = uint32_t(int32_t(int16_t(eax)));"); return;
        case ZYDIS_MNEMONIC_LAHF:
            line("rt::set_hi8(eax, (uint32_t(sf) << 7) | (uint32_t(zf) << 6) | (uint32_t(af) << 4) | (uint32_t(pf) << 2) | 2u | uint32_t(cf));");
            return;
        case ZYDIS_MNEMONIC_SAHF:
            line("{ uint32_t h = (eax >> 8) & 0xFFu; sf = h & 0x80u; zf = h & 0x40u; af = h & 0x10u; pf = h & 0x04u; cf = h & 0x01u; }");
            return;
        case ZYDIS_MNEMONIC_STC: line("cf = true;"); return;
        case ZYDIS_MNEMONIC_CLC: line("cf = false;"); return;
        case ZYDIS_MNEMONIC_CMC: line("cf = !cf;"); return;
        case ZYDIS_MNEMONIC_PUSH: {
            int w = d.kind == Operand::Kind::Imm ? 32 : width(i, d);
            if (w != 32) fail(i, dec_, "16-bit push");
            line(fmt("{ uint32_t v = %s; esp -= 4u; rt::st32(m, esp, v, 0x%Xu); }", read(i, d, 32).c_str(), i.addr));
            return;
        }
        case ZYDIS_MNEMONIC_POP: {
            if (width(i, d) != 32) fail(i, dec_, "16-bit pop");
            line(fmt("{ uint32_t v = rt::ld32(m, esp, 0x%Xu); esp += 4u; %s }", i.addr, write(i, d, 32, "v").c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_LEAVE:
            line(fmt("esp = ebp; ebp = rt::ld32(m, esp, 0x%Xu); esp += 4u;", i.addr));
            return;
        case ZYDIS_MNEMONIC_RET: {
            uint32_t n = i.nops >= 1 && d.kind == Operand::Kind::Imm ? uint32_t(d.imm & 0xFFFF) : 0;
            line(fmt("c.ret_to = rt::ld32(m, esp, 0x%Xu); esp += %uu; writeback(); return;", i.addr, 4 + n));
            return;
        }
        case ZYDIS_MNEMONIC_JMP: {
            if (auto t = i.rel_target()) {
                line(fmt("goto %s;", label(*t).c_str()));
                return;
            }
            const JumpTable* jt = nullptr;
            for (auto& j : f_.jtabs)
                if (j.at == i.addr) jt = &j;
            if (!jt) fail(i, dec_, "indirect jump that is not a resolved jump table");
            auto r = gpr(d.index);
            line(fmt("switch (%s) {", r->base));
            for (size_t k = 0; k < jt->entries.size(); ++k)
                if (jt->entries[k]) line(fmt("    case %zu: goto %s;", k, label(jt->entries[k]).c_str()));
            line(fmt("    default: rt::trap(0x%Xu, \"jump table index outside the recovered table\");", i.addr));
            line("}");
            return;
        }
        // ---- string ops (DF = 0) ----
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSD: {
            if (M == ZYDIS_MNEMONIC_MOVSD && i.nops > 0 && i.op[0].kind == Operand::Kind::Reg) break;  // SSE2 movsd
            bool stos = M == ZYDIS_MNEMONIC_STOSB || M == ZYDIS_MNEMONIC_STOSD;
            int sz = (M == ZYDIS_MNEMONIC_STOSB || M == ZYDIS_MNEMONIC_MOVSB) ? 1 : 4;
            std::string body;
            if (stos) body = sz == 1 ? fmt("rt::st8(m, edi, uint8_t(eax), 0x%Xu); edi += 1u;", i.addr)
                                     : fmt("rt::st32(m, edi, eax, 0x%Xu); edi += 4u;", i.addr);
            else body = sz == 1 ? fmt("rt::st8(m, edi, rt::ld8(m, esi, 0x%Xu), 0x%Xu); esi += 1u; edi += 1u;", i.addr, i.addr)
                                : fmt("rt::st32(m, edi, rt::ld32(m, esi, 0x%Xu), 0x%Xu); esi += 4u; edi += 4u;", i.addr, i.addr);
            if (i.has_rep()) line("while (ecx) { " + body + " --ecx; }");
            else line("{ " + body + " }");
            return;
        }
        // ---- scalar SSE ----
        case ZYDIS_MNEMONIC_MOVSS: {
            if (d.kind == Operand::Kind::Mem) {
                line(fmt("rt::st32(m, %s, %s, 0x%Xu);", addr(i, d).c_str(), xmm_lane0(i, s).c_str(), i.addr));
            } else if (s.kind == Operand::Kind::Mem) {
                line(fmt("xmm%d = rt::Xmm{{%s, 0u, 0u, 0u}};", xmm_index(d.reg), xmm_lane0(i, s).c_str()));
            } else {
                line(fmt("xmm%d.u[0] = %s;", xmm_index(d.reg), xmm_lane0(i, s).c_str()));
            }
            return;
        }
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: {
            bool al = M == ZYDIS_MNEMONIC_MOVAPS;
            if (d.kind == Operand::Kind::Mem)
                line(fmt("rt::st128(m, %s, %s, %s, 0x%Xu);", addr(i, d).c_str(), xmm128(i, s, al).c_str(), al ? "true" : "false", i.addr));
            else
                line(fmt("xmm%d = %s;", xmm_index(d.reg), xmm128(i, s, al).c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_ADDSS: case ZYDIS_MNEMONIC_SUBSS: case ZYDIS_MNEMONIC_MULSS: case ZYDIS_MNEMONIC_DIVSS: {
            const char* op = M == ZYDIS_MNEMONIC_ADDSS ? "+" : M == ZYDIS_MNEMONIC_SUBSS ? "-" : M == ZYDIS_MNEMONIC_MULSS ? "*" : "/";
            int x = xmm_index(d.reg);
            line(fmt("xmm%d.u[0] = rt::u32(rt::f32(xmm%d.u[0]) %s rt::f32(%s));", x, x, op, xmm_lane0(i, s).c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_MAXSS: {
            int x = xmm_index(d.reg);
            line(fmt("xmm%d.u[0] = rt::%s(xmm%d.u[0], %s);", x, M == ZYDIS_MNEMONIC_MINSS ? "minss" : "maxss", x,
                     xmm_lane0(i, s).c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_SQRTSS: {
            line(fmt("xmm%d.u[0] = rt::u32(std::sqrt(rt::f32(%s)));", xmm_index(d.reg), xmm_lane0(i, s).c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_XORPS: case ZYDIS_MNEMONIC_ANDPS: case ZYDIS_MNEMONIC_ORPS: case ZYDIS_MNEMONIC_ANDNPS: {
            int x = xmm_index(d.reg);
            if (M == ZYDIS_MNEMONIC_XORPS && s.kind == Operand::Kind::Reg && s.reg == d.reg) {
                line(fmt("xmm%d = rt::Xmm{{0u, 0u, 0u, 0u}};", x));
                return;
            }
            const char* e = M == ZYDIS_MNEMONIC_XORPS ? "a ^ b" : M == ZYDIS_MNEMONIC_ANDPS ? "a & b" : M == ZYDIS_MNEMONIC_ORPS ? "a | b" : "~a & b";
            line(fmt("{ rt::Xmm s = %s; for (int k = 0; k < 4; ++k) { uint32_t a = xmm%d.u[k], b = s.u[k]; xmm%d.u[k] = %s; } }",
                     xmm128(i, s, true).c_str(), x, x, e));
            return;
        }
        case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS: {
            line(fmt("{ float a = rt::f32(xmm%d.u[0]), b = rt::f32(%s);", xmm_index(d.reg), xmm_lane0(i, s).c_str()));
            line("  bool un = a != a || b != b;");
            if (need & ZF) line("  zf = un || a == b;");
            if (need & PF) line("  pf = un;");
            if (need & CF) line("  cf = un || a < b;");
            if (need & OF) line("  of = false;");
            if (need & SF) line("  sf = false;");
            if (need & AF) line("  af = false;");
            line("  (void)un; }");
            return;
        }
        case ZYDIS_MNEMONIC_CVTSI2SS: {
            line(fmt("xmm%d.u[0] = rt::u32(float(int32_t(%s)));", xmm_index(d.reg), read(i, s, 32).c_str()));
            return;
        }
        case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI: {
            if (width(i, d) != 32) fail(i, dec_, "unexpected destination");
            line(write(i, d, 32, fmt("rt::%s(rt::f32(%s))", M == ZYDIS_MNEMONIC_CVTTSS2SI ? "cvtt_f32_i32" : "cvt_f32_i32",
                                     xmm_lane0(i, s).c_str())));
            return;
        }
        default:
            break;
    }
    if (i.isa_ext == ZYDIS_ISA_EXT_X87) fail(i, dec_, "x87 not supported yet (lifter slice 2)");
    fail(i, dec_, "instruction not supported yet");
}

// Forward analysis of esp (and ebp) as constant offsets from esp at entry.
// Every `ret` must find esp back at offset 0, otherwise this "function" is a
// fragment of another one (or not code), and lifting it would be wrong.
void FnEmitter::check_stack_balance(const std::vector<std::vector<size_t>>& succ) {
    constexpr int64_t UNK = INT64_MIN;
    struct St { int64_t esp = UNK, ebp = UNK; bool seen = false; };
    const size_t n = ins_.size();
    std::vector<St> in(n);
    size_t entry = idx_.at(f_.start);
    in[entry] = {0, UNK, true};
    std::vector<size_t> work{entry};
    auto is_reg = [](const Operand& o, ZydisRegister r) { return o.kind == Operand::Kind::Reg && o.reg == r; };
    while (!work.empty()) {
        size_t k = work.back();
        work.pop_back();
        St st = in[k];
        const Insn& i = *ins_[k];
        const Operand& d = i.op[0];
        const Operand& s = i.op[1];
        auto add = [&](int64_t v) { if (st.esp != UNK) st.esp += v; };
        switch (i.mnem) {
            case ZYDIS_MNEMONIC_PUSH: add(-4); break;
            case ZYDIS_MNEMONIC_POP:
                if (st.esp != UNK && st.esp >= 0)
                    fail(i, dec_, "pops its own return address: a fragment of another function, not a whole function");
                add(4);
                if (is_reg(d, ZYDIS_REGISTER_ESP)) st.esp = UNK;
                if (is_reg(d, ZYDIS_REGISTER_EBP)) st.ebp = UNK;
                break;
            case ZYDIS_MNEMONIC_LEAVE: st.esp = st.ebp == UNK ? UNK : st.ebp + 4; st.ebp = UNK; break;
            case ZYDIS_MNEMONIC_RET:
                if (st.esp != UNK && st.esp != 0)
                    fail(i, dec_, fmt("stack not balanced at ret (esp is %+lld from entry): not a whole function",
                                      (long long)st.esp));
                break;
            default:
                if (i.nops >= 1 && is_reg(d, ZYDIS_REGISTER_ESP)) {
                    if (i.mnem == ZYDIS_MNEMONIC_SUB && s.kind == Operand::Kind::Imm) add(-int64_t(int32_t(s.imm)));
                    else if (i.mnem == ZYDIS_MNEMONIC_ADD && s.kind == Operand::Kind::Imm) add(int64_t(int32_t(s.imm)));
                    else if (i.mnem == ZYDIS_MNEMONIC_MOV && is_reg(s, ZYDIS_REGISTER_EBP)) st.esp = st.ebp;
                    else if (i.mnem == ZYDIS_MNEMONIC_LEA && s.index == ZYDIS_REGISTER_NONE &&
                             (s.base == ZYDIS_REGISTER_ESP || s.base == ZYDIS_REGISTER_EBP)) {
                        int64_t b = s.base == ZYDIS_REGISTER_ESP ? st.esp : st.ebp;
                        st.esp = b == UNK ? UNK : b + s.disp;
                    } else if (i.mnem != ZYDIS_MNEMONIC_CMP && i.mnem != ZYDIS_MNEMONIC_TEST) st.esp = UNK;
                } else if (i.nops >= 1 && is_reg(d, ZYDIS_REGISTER_EBP) && i.mnem != ZYDIS_MNEMONIC_CMP &&
                           i.mnem != ZYDIS_MNEMONIC_TEST && i.mnem != ZYDIS_MNEMONIC_PUSH) {
                    st.ebp = (i.mnem == ZYDIS_MNEMONIC_MOV && is_reg(s, ZYDIS_REGISTER_ESP)) ? st.esp : UNK;
                }
                break;
        }
        for (size_t t : succ[k]) {
            St& o = in[t];
            if (!o.seen) { o = st; o.seen = true; work.push_back(t); continue; }
            St m = o;
            if (m.esp != st.esp) m.esp = UNK;
            if (m.ebp != st.ebp) m.ebp = UNK;
            if (m.esp != o.esp || m.ebp != o.ebp) { o = m; work.push_back(t); }
        }
    }
}

std::string FnEmitter::run() {
    for (uint32_t a : f_.insns) {
        idx_[a] = ins_.size();
        ins_.push_back(dec_.decode(a));
    }
    const size_t n = ins_.size();
    if (!f_.calls.empty() || !f_.tails.empty() || !f_.icalls.empty() || !f_.ijmps.empty())
        throw LiftError("not a leaf function (calls, tail jumps or unresolved indirect jumps): slice 1 lifts leaves only");

    // Successors.
    std::vector<std::vector<size_t>> succ(n);
    std::vector<bool> falls(n, false);
    auto need_idx = [&](const Insn& i, uint32_t t) -> size_t {
        auto it = idx_.find(t);
        if (it == idx_.end()) fail(i, dec_, fmt("branch target %08X is not an instruction of this function", t));
        return it->second;
    };
    for (size_t k = 0; k < n; ++k) {
        const Insn& i = *ins_[k];
        if (const char* why = i.implausible_reason()) fail(i, dec_, why);
        if (i.is_ret() || i.mnem == ZYDIS_MNEMONIC_INT3) continue;
        if (i.is_jmp()) {
            if (auto t = i.rel_target()) {
                succ[k].push_back(need_idx(i, *t));
            } else {
                for (auto& j : f_.jtabs)
                    if (j.at == i.addr)
                        for (uint32_t t : j.targets) succ[k].push_back(need_idx(i, t));
            }
            continue;
        }
        if (i.is_cond_branch()) {
            auto t = i.rel_target();
            if (!t) fail(i, dec_, "conditional branch without target");
            succ[k].push_back(need_idx(i, *t));
        }
        falls[k] = true;
        succ[k].push_back(need_idx(i, i.end()));
    }

    check_stack_balance(succ);

    // Backward flag liveness to a fixpoint.
    std::vector<uint8_t> live_in(n, 0), live_out(n, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = n; k-- > 0;) {
            uint8_t out = 0;
            for (size_t s : succ[k]) out |= live_in[s];
            FlagFx fx = flag_fx(*ins_[k]);
            uint8_t in = fx.use | (out & ~fx.kill);
            if (out != live_out[k] || in != live_in[k]) {
                live_out[k] = out;
                live_in[k] = in;
                changed = true;
            }
        }
    }
    if (live_in[idx_.at(f_.start)])
        throw LiftError(fmt("%08X: flags are read before being written (live on entry)", f_.start));

    // Labels: every branch target and every non-adjacent fallthrough.
    std::set<uint32_t> labels;
    for (size_t k = 0; k < n; ++k)
        for (size_t s : succ[k])
            if (!(falls[k] && s == k + 1 && ins_[k]->end() == ins_[s]->addr)) labels.insert(ins_[s]->addr);

    std::string body;
    for (size_t k = 0; k < n; ++k) {
        const Insn& i = *ins_[k];
        if (k == 0 && i.addr != f_.start) {  // entry is not the lowest address
            out_ += fmt("    goto L_%08X;\n", f_.start);
            labels.insert(f_.start);
        }
        if (labels.count(i.addr)) out_ += fmt("L_%08X:;\n", i.addr);
        out_ += fmt("    // %08X  %s\n", i.addr, dec_.format(i).c_str());
        emit(i, live_out[k]);
        if (falls[k] && !(k + 1 < n && ins_[k + 1]->addr == i.end())) {
            out_ += fmt("    goto L_%08X;\n", i.end());
            labels.insert(i.end());
        }
    }
    // Labels referenced after their instruction was emitted (fallthrough gotos) were
    // added to `labels` late; they were also branch-target labels by construction
    // (succ lists include fallthrough), so they are already defined above.

    std::string fn = fmt("void %s(rt::Context& c) {\n", Lifter::name(f_.start).c_str());
    fn += "    [[maybe_unused]] const rt::Mem m{c.mem, c.mem_size};\n";
    fn += "    uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, ebx = c.ebx, esp = c.esp, ebp = c.ebp, esi = c.esi, edi = c.edi;\n";
    fn += "    rt::Xmm xmm0 = c.xmm[0], xmm1 = c.xmm[1], xmm2 = c.xmm[2], xmm3 = c.xmm[3], xmm4 = c.xmm[4], xmm5 = c.xmm[5], xmm6 = c.xmm[6], xmm7 = c.xmm[7];\n";
    fn += "    [[maybe_unused]] bool cf = false, pf = false, af = false, zf = false, sf = false, of = false;\n";
    fn += "    [[maybe_unused]] auto writeback = [&] {\n"
          "        c.eax = eax; c.ecx = ecx; c.edx = edx; c.ebx = ebx; c.esp = esp; c.ebp = ebp; c.esi = esi; c.edi = edi;\n"
          "        c.xmm[0] = xmm0; c.xmm[1] = xmm1; c.xmm[2] = xmm2; c.xmm[3] = xmm3; c.xmm[4] = xmm4; c.xmm[5] = xmm5; c.xmm[6] = xmm6; c.xmm[7] = xmm7;\n"
          "    };\n";
    fn += out_;
    fn += "}\n";
    return fn;
}

}  // namespace

std::string Lifter::name(uint32_t addr) { return fmt("fn_%08X", addr); }

LiftResult Lifter::lift(const Function& f) {
    (void)xbe_;
    (void)cfg_;
    LiftResult r;
    try {
        FnEmitter e(dec_, f);
        r.code = e.run();
        r.ok = true;
    } catch (const LiftError& ex) {
        r.error = ex.what();
    }
    return r;
}

}  // namespace qilin
