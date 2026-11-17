#include "vrange.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "cfg.hpp"

namespace qilin {

namespace {

constexpr int kNoReg = -1;

int gpr_index(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI ? int(r - ZYDIS_REGISTER_EAX) : kNoReg;
}

// al/cl/dl/bl as the low byte of a 32-bit register.
int low8_index(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_AL: return 0;
        case ZYDIS_REGISTER_CL: return 1;
        case ZYDIS_REGISTER_DL: return 2;
        case ZYDIS_REGISTER_BL: return 3;
        default: return kNoReg;
    }
}

std::optional<Range> make(int64_t lo, int64_t hi) {
    if (lo > hi || lo < 0 || hi > int64_t(UINT32_MAX)) return std::nullopt;
    return Range{uint32_t(lo), uint32_t(hi)};
}
Range or_top(std::optional<Range> r) { return r.value_or(Range::top()); }
// r + k modulo 2^32, when the result is still one interval.
std::optional<Range> shift(Range r, uint32_t k) {
    const uint32_t lo = r.lo + k, hi = r.hi + k;
    const bool wlo = lo < r.lo, whi = hi < r.hi;  // which endpoints wrapped
    if (wlo == whi) return Range{lo, hi};
    return std::nullopt;
}
Range hull(Range a, Range b) { return {std::min(a.lo, b.lo), std::max(a.hi, b.hi)}; }
std::optional<Range> meet(Range a, Range b) { return make(std::max(a.lo, b.lo), std::min(a.hi, b.hi)); }

// The last flag-setting comparison still describing live register values:
// flags reflect `reg (op) value` with value a known constant.
struct Flags {
    int reg = kNoReg;
    uint32_t value = 0;
    uint8_t width = 32;  // the comparison's operand width
    bool reversed = false;  // `cmp value, reg` rather than `cmp reg, value`
    uint32_t bias = 0;      // the compared value is reg + bias (`sub reg, imm` compares the old value)
    bool operator==(const Flags&) const = default;
};

struct ByteSrc {
    uint32_t table = 0;  // 0: none
    Range base;
    bool operator==(const ByteSrc&) const = default;
};

struct State {
    std::array<Range, 8> r{};
    std::array<ByteSrc, 8> src{};
    Flags flags;
    bool operator==(const State&) const = default;
};

State join(const State& a, const State& b) {
    State s;
    for (size_t k = 0; k < 8; ++k) {
        s.r[k] = hull(a.r[k], b.r[k]);
        s.src[k] = a.src[k] == b.src[k] ? a.src[k] : ByteSrc{};
    }
    s.flags = a.flags == b.flags ? a.flags : Flags{};
    return s;
}

// Mirror a condition for `cmp value, reg` (value OP reg <=> reg OP' value).
ZydisMnemonic mirror(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JNBE: return ZYDIS_MNEMONIC_JB;
        case ZYDIS_MNEMONIC_JB: return ZYDIS_MNEMONIC_JNBE;
        case ZYDIS_MNEMONIC_JNB: return ZYDIS_MNEMONIC_JBE;
        case ZYDIS_MNEMONIC_JBE: return ZYDIS_MNEMONIC_JNB;
        case ZYDIS_MNEMONIC_JNLE: return ZYDIS_MNEMONIC_JL;
        case ZYDIS_MNEMONIC_JL: return ZYDIS_MNEMONIC_JNLE;
        case ZYDIS_MNEMONIC_JNL: return ZYDIS_MNEMONIC_JLE;
        case ZYDIS_MNEMONIC_JLE: return ZYDIS_MNEMONIC_JNL;
        default: return m;  // jz/jnz are symmetric; js/jns etc. are not modelled for reversed
    }
}

ZydisMnemonic negate(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JNBE: return ZYDIS_MNEMONIC_JBE;
        case ZYDIS_MNEMONIC_JBE: return ZYDIS_MNEMONIC_JNBE;
        case ZYDIS_MNEMONIC_JB: return ZYDIS_MNEMONIC_JNB;
        case ZYDIS_MNEMONIC_JNB: return ZYDIS_MNEMONIC_JB;
        case ZYDIS_MNEMONIC_JZ: return ZYDIS_MNEMONIC_JNZ;
        case ZYDIS_MNEMONIC_JNZ: return ZYDIS_MNEMONIC_JZ;
        case ZYDIS_MNEMONIC_JNLE: return ZYDIS_MNEMONIC_JLE;
        case ZYDIS_MNEMONIC_JLE: return ZYDIS_MNEMONIC_JNLE;
        case ZYDIS_MNEMONIC_JL: return ZYDIS_MNEMONIC_JNL;
        case ZYDIS_MNEMONIC_JNL: return ZYDIS_MNEMONIC_JL;
        case ZYDIS_MNEMONIC_JS: return ZYDIS_MNEMONIC_JNS;
        case ZYDIS_MNEMONIC_JNS: return ZYDIS_MNEMONIC_JS;
        default: return ZYDIS_MNEMONIC_INVALID;
    }
}

// Values x with `x (cond) c` true, as up to two unsigned intervals.
// Returns false for conditions that aren't modelled (no narrowing).
bool satisfying(ZydisMnemonic m, uint32_t c, std::vector<Range>& out) {
    out.clear();
    auto add = [&](int64_t lo, int64_t hi) {
        if (auto r = make(lo, hi)) out.push_back(*r);
    };
    // Signed [slo, shi] (as int32 values) to unsigned intervals.
    auto add_signed = [&](int64_t slo, int64_t shi) {
        if (slo > shi) return;
        if (slo >= 0) add(slo, shi);
        else if (shi < 0) add(slo + (int64_t(1) << 32), shi + (int64_t(1) << 32));
        else {
            add(0, shi);
            add(slo + (int64_t(1) << 32), int64_t(UINT32_MAX));
        }
    };
    const int64_t u = c, s = int32_t(c);
    constexpr int64_t smin = INT32_MIN, smax = INT32_MAX;
    switch (m) {
        case ZYDIS_MNEMONIC_JNBE: add(u + 1, UINT32_MAX); return true;
        case ZYDIS_MNEMONIC_JNB: add(u, UINT32_MAX); return true;
        case ZYDIS_MNEMONIC_JB: add(0, u - 1); return true;
        case ZYDIS_MNEMONIC_JBE: add(0, u); return true;
        case ZYDIS_MNEMONIC_JZ: add(u, u); return true;
        case ZYDIS_MNEMONIC_JNZ: add(0, u - 1); add(u + 1, UINT32_MAX); return true;
        case ZYDIS_MNEMONIC_JNLE: add_signed(s + 1, smax); return true;
        case ZYDIS_MNEMONIC_JNL: add_signed(s, smax); return true;
        case ZYDIS_MNEMONIC_JL: add_signed(smin, s - 1); return true;
        case ZYDIS_MNEMONIC_JLE: add_signed(smin, s); return true;
        default: return false;
    }
}

// Narrow r to the values satisfying the condition; nullopt if none can.
std::optional<Range> narrow(Range r, ZydisMnemonic m, uint32_t c) {
    std::vector<Range> parts;
    if (!satisfying(m, c, parts)) return r;
    std::optional<Range> acc;
    for (Range p : parts)
        if (auto x = meet(r, p)) acc = acc ? hull(*acc, *x) : *x;
    return acc;
}

class Analysis {
public:
    Analysis(const Xbe& xbe, const std::map<uint32_t, const Insn*>& insns, std::span<const JumpTable> jtabs)
        : xbe_(xbe), insns_(insns) {
        for (auto& j : jtabs) jt_[j.at] = &j;
    }

    void run(std::span<const uint32_t> entries) {
        for (uint32_t e : entries)
            if (insns_.contains(e)) push(e, State{});
        while (!work_.empty()) {
            const uint32_t a = work_.front();
            work_.pop_front();
            queued_.erase(a);
            step(a, in_.at(a));
        }
    }

    const State* at(uint32_t a) const {
        auto it = in_.find(a);
        return it == in_.end() ? nullptr : &it->second;
    }

private:
    static constexpr int kWiden = 12;

    void push(uint32_t a, const State& s) {
        if (!insns_.contains(a)) return;  // tail jump out of the function
        auto [it, fresh] = in_.try_emplace(a, s);
        if (!fresh) {
            State j = join(it->second, s);
            if (j == it->second) return;
            if (++visits_[a] > kWiden)
                for (size_t k = 0; k < 8; ++k)
                    if (j.r[k] != it->second.r[k]) j.r[k] = Range::top();
            it->second = j;
        }
        if (queued_.insert(a).second) work_.push_back(a);
    }

    Range reg(const State& s, ZydisRegister r) const {
        const int k = gpr_index(r);
        return k == kNoReg ? Range::top() : s.r[size_t(k)];
    }

    // Range of a register or immediate operand, if it's known to be one value.
    std::optional<uint32_t> constant(const State& s, const Operand& o) const {
        if (o.kind == Operand::Kind::Imm) return uint32_t(o.imm);
        if (o.kind == Operand::Kind::Reg) {
            const Range r = reg(s, o.reg);
            if (r.lo == r.hi) return r.lo;
        }
        return std::nullopt;
    }

    void kill(State& s, int k) {
        s.r[size_t(k)] = Range::top();
        s.src[size_t(k)] = {};
        if (s.flags.reg == k) s.flags = {};
    }

    void set(State& s, int k, Range v, ByteSrc src = {}) {
        if (s.flags.reg == k) s.flags = {};
        s.r[size_t(k)] = v;
        s.src[size_t(k)] = src;
    }

    // [base + index*scale + disp] over the registers' ranges.
    std::optional<Range> address(const State& s, const Operand& m) const {
        int64_t lo = m.disp, hi = m.disp;
        if (m.base != ZYDIS_REGISTER_NONE) {
            if (gpr_index(m.base) == kNoReg) return std::nullopt;
            const Range b = reg(s, m.base);
            lo += b.lo;
            hi += b.hi;
        }
        if (m.index != ZYDIS_REGISTER_NONE) {
            if (gpr_index(m.index) == kNoReg) return std::nullopt;
            const Range i = reg(s, m.index);
            const int64_t sc = m.scale ? m.scale : 1;
            lo += int64_t(i.lo) * sc;
            hi += int64_t(i.hi) * sc;
        }
        return make(lo, hi);
    }

    // `movzx r32, byte [b + T]` with b's range small: the loaded byte's range.
    std::optional<std::pair<Range, ByteSrc>> byte_load(const State& s, const Operand& m) const {
        if (m.kind != Operand::Kind::Mem || m.size != 8 || m.disp <= 0) return std::nullopt;
        if (m.seg != ZYDIS_REGISTER_DS && m.seg != ZYDIS_REGISTER_NONE) return std::nullopt;
        ZydisRegister b = m.base;
        if (b == ZYDIS_REGISTER_NONE && m.scale <= 1) b = m.index;
        else if (m.index != ZYDIS_REGISTER_NONE) return std::nullopt;
        if (gpr_index(b) == kNoReg) return std::nullopt;
        const Range br = reg(s, b);
        if (br.hi - br.lo > 4095) return std::nullopt;
        const uint32_t t = uint32_t(m.disp);
        if (!xbe_.mapped(t + br.lo, br.hi - br.lo + 1)) return std::nullopt;
        uint8_t mn = 0xFF, mx = 0;
        for (uint32_t k = br.lo;; ++k) {
            const uint8_t v = xbe_.u8(t + k);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            if (k == br.hi) break;
        }
        return std::pair{Range{mn, mx}, ByteSrc{t, br}};
    }

    // Transfer function: the state after instruction i.
    State transfer(const Insn& i, State s) {
        const ZydisMnemonic m = i.mnem;
        const int d = i.nops >= 1 && i.op[0].kind == Operand::Kind::Reg ? gpr_index(i.op[0].reg) : kNoReg;
        const Operand* src = i.nops >= 2 ? &i.op[1] : nullptr;
        bool handled = false;
        Flags nf{};  // flags after this instruction, if it sets them and they're modelled
        bool sets_cmp = false;

        switch (m) {
            case ZYDIS_MNEMONIC_MOV:
                if (d != kNoReg && src) {
                    if (src->kind == Operand::Kind::Imm) set(s, d, Range::point(uint32_t(src->imm)));
                    else if (src->kind == Operand::Kind::Reg && gpr_index(src->reg) != kNoReg) {
                        const int k = gpr_index(src->reg);
                        set(s, d, s.r[size_t(k)], s.src[size_t(k)]);
                    } else kill(s, d);
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_MOVZX:
                if (d != kNoReg && src) {
                    if (auto bl = byte_load(s, *src)) set(s, d, bl->first, bl->second);
                    else if (src->kind == Operand::Kind::Reg && low8_index(src->reg) != kNoReg &&
                             s.r[size_t(low8_index(src->reg))].hi <= 0xFF) {
                        const int k = low8_index(src->reg);
                        set(s, d, s.r[size_t(k)], s.src[size_t(k)]);
                    } else set(s, d, Range{0, src->size == 8 ? 0xFFu : 0xFFFFu});
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_LEA:
                if (d != kNoReg && src) {
                    set(s, d, or_top(address(s, *src)));
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_SUB:
                if (d != kNoReg && src && src->is_reg(i.op[0].reg)) {
                    set(s, d, Range::point(0));
                    handled = true;
                    break;
                }
                if (m == ZYDIS_MNEMONIC_XOR) break;
                [[fallthrough]];
            case ZYDIS_MNEMONIC_ADD:
                if (d != kNoReg && src) {
                    const Range a = s.r[size_t(d)];
                    std::optional<Range> v;
                    if (src->kind == Operand::Kind::Imm) {
                        const int64_t k = m == ZYDIS_MNEMONIC_ADD ? int64_t(int32_t(src->imm)) : -int64_t(int32_t(src->imm));
                        v = shift(a, uint32_t(k));
                        // `sub reg, imm` sets flags as `cmp old, imm`; old = new + imm.
                        if (m == ZYDIS_MNEMONIC_SUB && i.op[0].size == 32) {
                            nf = {d, uint32_t(src->imm), 32, false, uint32_t(src->imm)};
                            sets_cmp = true;
                        }
                    } else if (src->kind == Operand::Kind::Reg && gpr_index(src->reg) != kNoReg && m == ZYDIS_MNEMONIC_ADD) {
                        const Range b = s.r[size_t(gpr_index(src->reg))];
                        v = make(int64_t(a.lo) + b.lo, int64_t(a.hi) + b.hi);
                    }
                    set(s, d, or_top(v));
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC:
                if (d != kNoReg) {
                    const Range a = s.r[size_t(d)];
                    const int64_t k = m == ZYDIS_MNEMONIC_INC ? 1 : -1;
                    set(s, d, or_top(shift(a, uint32_t(k))));
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_AND:
                if (d != kNoReg && src) {
                    if (auto c = constant(s, *src)) set(s, d, Range{0, std::min(s.r[size_t(d)].hi, *c)});
                    else if (src->kind == Operand::Kind::Reg && gpr_index(src->reg) != kNoReg)
                        set(s, d, Range{0, std::min(s.r[size_t(d)].hi, s.r[size_t(gpr_index(src->reg))].hi)});
                    else set(s, d, Range{0, s.r[size_t(d)].hi});
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
                if (d != kNoReg && src && src->kind == Operand::Kind::Imm) {
                    const uint32_t k = uint32_t(src->imm & 31);
                    const Range a = s.r[size_t(d)];
                    if (m == ZYDIS_MNEMONIC_SHR || a.hi <= 0x7FFFFFFF) set(s, d, Range{a.lo >> k, a.hi >> k});
                    else kill(s, d);
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_SHL:
                if (d != kNoReg && src && src->kind == Operand::Kind::Imm) {
                    const uint32_t k = uint32_t(src->imm & 31);
                    const Range a = s.r[size_t(d)];
                    set(s, d, or_top(make(int64_t(a.lo) << k, int64_t(a.hi) << k)));
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_IMUL:
                if (d != kNoReg && i.nops == 3 && i.op[1].kind == Operand::Kind::Reg && i.op[2].kind == Operand::Kind::Imm &&
                    int32_t(i.op[2].imm) >= 0) {
                    const Range a = reg(s, i.op[1].reg);
                    const int64_t k = int32_t(i.op[2].imm);
                    set(s, d, or_top(make(int64_t(a.lo) * k, int64_t(a.hi) * k)));
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_CMP:
                if (i.nops == 2) {
                    const int a = i.op[0].kind == Operand::Kind::Reg ? gpr_index(i.op[0].reg) : kNoReg;
                    const int a8 = i.op[0].kind == Operand::Kind::Reg ? low8_index(i.op[0].reg) : kNoReg;
                    const int b = i.op[1].kind == Operand::Kind::Reg ? gpr_index(i.op[1].reg) : kNoReg;
                    const uint8_t w = uint8_t(i.op[0].size);
                    if ((a != kNoReg || a8 != kNoReg) && i.op[1].kind == Operand::Kind::Imm) {
                        nf = {a != kNoReg ? a : a8, uint32_t(i.op[1].imm) & (w >= 32 ? UINT32_MAX : (1u << w) - 1), w, false};
                    } else if (a != kNoReg && b != kNoReg) {
                        if (auto c = constant(s, i.op[1])) nf = {a, *c, 32, false};
                        else if (auto c2 = constant(s, i.op[0])) nf = {b, *c2, 32, true};
                    }
                    sets_cmp = nf.reg != kNoReg;
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_TEST:
                if (i.nops == 2 && i.op[0].kind == Operand::Kind::Reg && i.op[1].is_reg(i.op[0].reg) &&
                    gpr_index(i.op[0].reg) != kNoReg) {
                    nf = {gpr_index(i.op[0].reg), 0, 32, false};
                    sets_cmp = true;
                    handled = true;
                }
                break;
            case ZYDIS_MNEMONIC_CALL:
                for (int k : {0, 1, 2}) kill(s, k);  // eax, ecx, edx: caller-saved
                s.flags = {};
                return s;
            default:
                break;
        }
        if (!handled)
            for (int k = 0; k < 8; ++k)
                if (writes_reg(i, ZydisRegister(ZYDIS_REGISTER_EAX + k))) kill(s, k);
        if (sets_cmp) s.flags = nf;
        else if (i.flags_written) s.flags = {};
        return s;
    }

    // Narrow the state for the edge where condition m holds.
    std::optional<State> on_edge(State s, ZydisMnemonic m) const {
        const Flags f = s.flags;
        if (f.reg == kNoReg) return s;
        const ZydisMnemonic c = f.reversed ? mirror(m) : m;
        Range r = s.r[size_t(f.reg)];
        if (f.width < 32) {
            // Only when the whole register already fits in the compared width.
            const uint32_t mask = (1u << f.width) - 1;
            if (r.hi > mask) return s;
            // Signed conditions at a narrower width aren't modelled.
            if (c == ZYDIS_MNEMONIC_JL || c == ZYDIS_MNEMONIC_JLE || c == ZYDIS_MNEMONIC_JNL || c == ZYDIS_MNEMONIC_JNLE)
                return s;
        }
        // Narrow the compared value (reg + bias), then map back to reg.
        const Range cmpv = or_top(shift(r, f.bias));
        auto n = narrow(cmpv, c, f.value);
        if (!n) return std::nullopt;  // this edge can't be taken
        if (auto back = shift(*n, 0u - f.bias)) {
            auto x = meet(r, *back);
            if (!x) return std::nullopt;
            s.r[size_t(f.reg)] = *x;
        }
        return s;
    }

    void step(uint32_t a, State s) {
        const Insn& i = *insns_.at(a);
        if (i.is_ret() || i.mnem == ZYDIS_MNEMONIC_INT3) return;
        if (i.is_jmp()) {
            if (auto t = i.rel_target()) {
                push(*t, transfer(i, s));
                return;
            }
            auto jt = jt_.find(a);
            if (jt == jt_.end()) return;
            const JumpTable& j = *jt->second;
            const int x = gpr_index(i.op[0].index);
            for (uint32_t t : j.targets) {
                State c = s;
                if (j.bounded && x != kNoReg && j.index_table == 0) {
                    // The cases of a proven table that lead to t.
                    std::optional<Range> cases;
                    for (uint32_t k = 0; k < j.entries.size(); ++k)
                        if (j.entries[k] == t) {
                            const Range v = Range::point(uint32_t(j.first) + k);
                            cases = cases ? hull(*cases, v) : v;
                        }
                    if (cases)
                        if (auto n = meet(c.r[size_t(x)], *cases)) c.r[size_t(x)] = *n;
                }
                push(t, c);
            }
            return;
        }
        if (i.is_cond_branch()) {
            const State base = transfer(i, s);
            const auto t = i.rel_target();
            const ZydisMnemonic neg = negate(i.mnem);
            if (t)
                if (auto e = on_edge(base, i.mnem)) push(*t, *e);
            if (neg == ZYDIS_MNEMONIC_INVALID) push(i.end(), base);
            else if (auto e = on_edge(base, neg)) push(i.end(), *e);
            return;
        }
        State n = transfer(i, s);
        if (i.is_call()) {
            const Insn* nx = insns_.contains(i.end()) ? insns_.at(i.end()) : nullptr;
            if (!nx) return;
        }
        push(i.end(), n);
    }

    const Xbe& xbe_;
    const std::map<uint32_t, const Insn*>& insns_;
    std::unordered_map<uint32_t, const JumpTable*> jt_;
    std::unordered_map<uint32_t, State> in_;
    std::unordered_map<uint32_t, int> visits_;
    std::deque<uint32_t> work_;
    std::unordered_set<uint32_t> queued_;
};

}  // namespace

std::map<uint32_t, IndexFact> index_ranges(const Xbe& xbe, const std::map<uint32_t, const Insn*>& insns,
                                           std::span<const uint32_t> entries, std::span<const JumpTable> jtabs,
                                           std::span<const uint32_t> want) {
    Analysis an(xbe, insns, jtabs);
    an.run(entries);
    std::map<uint32_t, IndexFact> out;
    for (uint32_t w : want) {
        const State* s = an.at(w);
        auto it = insns.find(w);
        if (!s || it == insns.end()) continue;
        const int x = gpr_index(it->second->op[0].index);
        if (x == kNoReg) continue;
        IndexFact f;
        f.index = s->r[size_t(x)];
        if (const ByteSrc& b = s->src[size_t(x)]; b.table) {
            f.byte_table = b.table + b.base.lo;
            f.byte_count = b.base.hi - b.base.lo + 1;
        }
        out.emplace(w, f);
    }
    return out;
}

}  // namespace qilin
