#include "cfg.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>

#include "json.hpp"
#include "vrange.hpp"

namespace qilin {

namespace {

bool is_gpr32(ZydisRegister r) { return r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI; }

// al/bl/cl/dl -> eax/ebx/ecx/edx; 32-bit registers map to themselves; others -> NONE.
ZydisRegister index_reg_of(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_AL: return ZYDIS_REGISTER_EAX;
        case ZYDIS_REGISTER_BL: return ZYDIS_REGISTER_EBX;
        case ZYDIS_REGISTER_CL: return ZYDIS_REGISTER_ECX;
        case ZYDIS_REGISTER_DL: return ZYDIS_REGISTER_EDX;
        default: return is_gpr32(r) ? r : ZYDIS_REGISTER_NONE;
    }
}

bool is_flat_mem(const Operand& o) {
    return o.kind == Operand::Kind::Mem && (o.seg == ZYDIS_REGISTER_DS || o.seg == ZYDIS_REGISTER_NONE);
}

}  // namespace

// Does this instruction (possibly) write 32-bit register r (or part of it)?
// Conservative: calls and the implicit-operand instructions count as writes.
bool writes_reg(const Insn& i, ZydisRegister r) {
    switch (i.mnem) {
        case ZYDIS_MNEMONIC_CALL:
            return r == ZYDIS_REGISTER_EAX || r == ZYDIS_REGISTER_ECX || r == ZYDIS_REGISTER_EDX;
        case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CWDE: case ZYDIS_MNEMONIC_MUL: case ZYDIS_MNEMONIC_DIV:
        case ZYDIS_MNEMONIC_IDIV: case ZYDIS_MNEMONIC_LAHF:
            return r == ZYDIS_REGISTER_EAX || r == ZYDIS_REGISTER_EDX;
        case ZYDIS_MNEMONIC_IMUL:
            if (i.nops == 1) return r == ZYDIS_REGISTER_EAX || r == ZYDIS_REGISTER_EDX;
            break;
        // String instructions: the pointers they step, ecx under a rep prefix,
        // eax for lods. (movsd/cmpsd here are the string forms: the SSE2 forms
        // don't exist on the Pentium III and are rejected as implausible.)
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
            return r == ZYDIS_REGISTER_ESI || r == ZYDIS_REGISTER_EDI || (i.has_rep() && r == ZYDIS_REGISTER_ECX);
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
            return r == ZYDIS_REGISTER_EDI || (i.has_rep() && r == ZYDIS_REGISTER_ECX);
        case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
            return r == ZYDIS_REGISTER_ESI || r == ZYDIS_REGISTER_EAX || (i.has_rep() && r == ZYDIS_REGISTER_ECX);
        case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_TEST: case ZYDIS_MNEMONIC_PUSH:
            return false;
        default:
            break;
    }
    for (uint8_t k = 0; k < i.nops && k < 2; ++k) {
        const Operand& o = i.op[k];
        if (o.kind != Operand::Kind::Reg) continue;
        const bool same = o.reg == r || index_reg_of(o.reg) == r ||
                          (o.reg >= ZYDIS_REGISTER_AX && o.reg <= ZYDIS_REGISTER_DI && o.reg - ZYDIS_REGISTER_AX == r - ZYDIS_REGISTER_EAX) ||
                          (o.reg >= ZYDIS_REGISTER_AH && o.reg <= ZYDIS_REGISTER_BH && o.reg - ZYDIS_REGISTER_AH == r - ZYDIS_REGISTER_EAX);
        // op[0] is the destination of almost everything; xchg also writes op[1].
        if (same && (k == 0 || i.mnem == ZYDIS_MNEMONIC_XCHG || i.mnem == ZYDIS_MNEMONIC_XADD)) return true;
    }
    return false;
}

namespace {

uint32_t width_mask(uint16_t bits) { return bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1; }

// `xx byte ptr [eax], al` -- what a run of zero bytes decodes to. A function
// that contains it is almost certainly data being decoded as code.
bool is_zero_bytes_insn(const Insn& i) {
    return i.nops >= 2 && i.op[0].kind == Operand::Kind::Mem && i.op[0].size == 8 &&
           i.op[0].base == ZYDIS_REGISTER_EAX && i.op[0].index == ZYDIS_REGISTER_NONE && i.op[0].disp == 0 &&
           i.op[1].is_reg(ZYDIS_REGISTER_AL);
}

}  // namespace

// MSVC alignment filler that is not a NOP mnemonic: `mov r, r` and
// `lea r, [r + 0]` (any 32-bit register, any displacement size), e.g.
// 8B FF, 8D 49 00, 8D 64 24 00, 8D 9B 00000000, 8D A4 24 00000000.
bool is_pad_filler(const Insn& i) {
    if (i.nops != 2 || i.op[0].kind != Operand::Kind::Reg || i.op[0].size != 32) return false;
    const auto r = i.op[0].reg;
    if (i.mnem == ZYDIS_MNEMONIC_MOV) return i.op[1].is_reg(r);
    if (i.mnem == ZYDIS_MNEMONIC_LEA)
        return i.op[1].kind == Operand::Kind::Mem && i.op[1].base == r && i.op[1].index == ZYDIS_REGISTER_NONE &&
               i.op[1].disp == 0;
    return false;
}

namespace {

// Address-like constants an instruction mentions: immediates >= 0x10000
// (`push offset fn`, `mov [x], offset fn`). Not relative branch offsets, and
// not memory displacements: [reg + T] / [4*reg + T] index tables and arrays,
// and [T] reads data, so neither is evidence of a function entry at T.
template <class F> void for_each_addr_constant(const Insn& i, F&& f) {
    for (uint8_t k = 0; k < i.nops; ++k) {
        const Operand& o = i.op[k];
        if (o.kind != Operand::Kind::Imm || o.imm_relative) continue;
        const uint64_t v = o.imm & 0xFFFFFFFFu;
        if (v >= 0x10000) f(uint32_t(v));
    }
}

}  // namespace

// ------------------------------------------------------------------ AddrSet

AddrSet::AddrSet(const Xbe& xbe) {
    lo_ = UINT32_MAX;
    for (auto& s : xbe.sections)
        if (s.code && s.rsize) {
            lo_ = std::min(lo_, s.va);
            hi_ = std::max(hi_, s.file_end());
        }
    if (lo_ >= hi_) lo_ = hi_ = 0;
    bits_.assign(hi_ - lo_, false);
}

void AddrSet::insert(uint32_t a) {
    if (a < lo_ || a >= hi_) throw XbeError(std::format("AddrSet: {:#x} is outside the code sections", a));
    if (!bits_[a - lo_]) {
        bits_[a - lo_] = true;
        ++count_;
    }
}

void AddrSet::clear() {
    std::ranges::fill(bits_, false);
    count_ = 0;
}

std::vector<uint32_t> AddrSet::to_vector() const {
    std::vector<uint32_t> v;
    v.reserve(count_);
    for (size_t i = 0; i < bits_.size(); ++i)
        if (bits_[i]) v.push_back(lo_ + uint32_t(i));
    return v;
}

// ------------------------------------------------------------------ Function

std::string Function::tag() const {
    switch (origin) {
        case Origin::Reachable: return "reachable";
        case Origin::DataPtr: return std::format("dataptr{}", round);
        case Origin::Orphan: return "orphan";
        case Origin::ImmPtr: return "imm-ptr";
        case Origin::Gap: return std::format("gap{}", round);
        case Origin::Eh: return "eh";
    }
    return "?";
}

// ------------------------------------------------------------------ Cfg

struct Cfg::Explored {
    std::map<uint32_t, const Insn*> insns;
    std::set<uint32_t> leaders, calls, tails;
    std::vector<IndirectSite> icalls, ijmps;
    std::vector<JumpTable> jtabs;
    std::vector<uint32_t> bad, overlap;
    std::set<uint32_t> imms;
};

Cfg::Cfg(const Xbe& xbe, Decoder& dec, CfgConfig cfg) : xbe_(xbe), dec_(dec), cfg_(std::move(cfg)), owned_(xbe) {}

uint32_t Cfg::bound_of(uint32_t f) const {
    const XbeSection* s = xbe_.code_section_at(f);
    const uint32_t send = s->file_end();
    auto it = cand_.upper_bound(f);
    return it == cand_.end() ? send : std::min(*it, send);
}

// hist ends with the `jmp [4*reg + table]`; out.at and out.table are set.
void Cfg::resolve_jt(const std::vector<const Insn*>& hist, const XbeSection& sec, JumpTable& out) {
    const ZydisRegister reg = hist.back()->op[0].index;
    std::optional<uint32_t> count;
    out.how = "heuristic";
    std::optional<uint32_t> btable;
    ZydisRegister idx_reg = reg;
    // Walk back (excluding the jmp itself) for: [movzx reg, byte [r2 + T2]] ...
    // then a bound on the index: `cmp idx, N` (N + 1 entries) or `and idx, 2^k - 1`.
    // Stop at anything else that writes the tracked register: an older bound
    // would describe a different value.
    std::optional<uint64_t> n;  // number of valid index values
    for (auto it = hist.rbegin() + 1; it != hist.rend(); ++it) {
        const Insn& i = **it;
        if (i.mnem == ZYDIS_MNEMONIC_MOVZX && !btable && i.nops == 2 && i.op[0].is_reg(reg) && is_flat_mem(i.op[1]) &&
            i.op[1].size == 8 && is_gpr32(i.op[1].base) && i.op[1].index == ZYDIS_REGISTER_NONE && i.op[1].disp > 0) {
            btable = uint32_t(i.op[1].disp);
            idx_reg = i.op[1].base;
            continue;
        }
        const bool on_idx = i.nops == 2 && i.op[0].kind == Operand::Kind::Reg && index_reg_of(i.op[0].reg) == idx_reg &&
                            i.op[1].kind == Operand::Kind::Imm;
        if (on_idx && i.mnem == ZYDIS_MNEMONIC_CMP) {
            // The immediate is sign-extended by the decoder; the bound is unsigned at the register's width.
            n = (i.op[1].imm & width_mask(i.op[0].size)) + 1;
            out.how = "cmp";
            break;
        }
        if (on_idx && i.mnem == ZYDIS_MNEMONIC_AND) {
            const uint64_t m = i.op[1].imm & width_mask(i.op[0].size);
            if ((m & (m + 1)) == 0) {  // 2^k - 1
                n = m + 1;
                out.how = "and";
            }
            break;
        }
        // Value-preserving moves into the index: `movzx eax, al` (same register)
        // keeps tracking it; `mov eax, ecx` / `movzx eax, cl` move tracking to the source.
        if ((i.mnem == ZYDIS_MNEMONIC_MOVZX || i.mnem == ZYDIS_MNEMONIC_MOV) && i.nops == 2 &&
            i.op[0].kind == Operand::Kind::Reg && index_reg_of(i.op[0].reg) == idx_reg && i.op[1].kind == Operand::Kind::Reg &&
            index_reg_of(i.op[1].reg) != ZYDIS_REGISTER_NONE) {
            idx_reg = index_reg_of(i.op[1].reg);
            continue;
        }
        if (writes_reg(i, idx_reg)) break;
    }
    if (n && *n > 4096) n.reset();  // not a plausible switch bound
    if (n) {
        if (btable) {
            // The byte table and the dword table never overlap: clip the byte table.
            if (*btable < out.table && *btable + *n > out.table) *n = out.table - *btable;
            uint32_t mx = 0;
            for (uint32_t k = 0; k < *n; ++k)
                if (xbe_.mapped(*btable + k)) mx = std::max<uint32_t>(mx, xbe_.u8(*btable + k));
            count = mx + 1;
            out.how += "+bytetable";
            out.index_table = *btable;
            out.index_count = uint32_t(*n);
        } else {
            count = uint32_t(*n);
        }
    } else {
        out.how = "heuristic";
    }
    const uint32_t lo = sec.va, hi = sec.file_end();
    uint32_t limit = count.value_or(512);
    if (btable && *btable > out.table) limit = std::min<uint32_t>(limit, (*btable - out.table) / 4);
    // An `and` mask bounds the index by the mask, not by how many cases exist:
    // read the whole range (slots may be unused, e.g. a biased base), then trim
    // trailing slots that aren't code.
    const bool masked = out.how.starts_with("and");
    const bool exact = count.has_value();
    out.bounded = count.has_value();
    std::set<uint32_t> tg;
    for (uint32_t i = 0; i < limit; ++i) {
        const uint32_t a = out.table + 4 * i;
        if (!xbe_.mapped(a, 4)) break;
        // No table runs into a known function.
        if (i > 0 && (funcs_.contains(a) || known_.contains(a))) break;
        const uint32_t v = xbe_.u32(a);
        if (!(v >= lo && v < hi)) {
            if (!exact) break;
            out.entries.push_back(0);
            continue;
        }
        out.entries.push_back(v);
        tg.insert(v);
    }
    if (masked)
        while (!out.entries.empty() && out.entries.back() == 0) out.entries.pop_back();
    out.targets.assign(tg.begin(), tg.end());
}

// Fill t (at, table set) from a proven index range. Every slot in range must
// hold an address in the table's code section; otherwise the proof doesn't
// describe this table and it stays heuristic.
bool Cfg::prove_table(const IndexFact& fact, const XbeSection& sec, JumpTable& t) const {
    if (fact.negated) {  // indices -hi .. -lo of a small non-negative range
        const Range p = fact.index;
        if (p.hi >= 4096 || fact.byte_table) return false;
        IndexFact n;
        n.index = Range{uint32_t(-int64_t(p.hi)), uint32_t(-int64_t(p.lo))};
        if (p.lo == 0 && p.hi > 0) {  // crosses zero: -hi .. 0 as int32
            t.entries.assign(p.hi + 1, 0);
            std::set<uint32_t> tg;
            for (uint32_t k = 0; k <= p.hi; ++k) {
                const uint32_t a = t.table + 4 * (uint32_t(-int64_t(p.hi)) + k);
                if (!xbe_.mapped(a, 4)) return false;
                const uint32_t v = xbe_.u32(a);
                if (v < sec.va || v >= sec.file_end()) return false;
                t.entries[k] = v;
                tg.insert(v);
            }
            t.first = -int32_t(p.hi);
            t.targets.assign(tg.begin(), tg.end());
            t.bounded = true;
            t.how = "range+neg";
            return true;
        }
        return prove_table(n, sec, t);
    }
    const Range r = fact.index;
    // Non-negative or entirely negative (as int32); at most 4096 slots.
    const bool neg = r.lo >= 0x80000000u;
    if (!neg && r.hi >= 0x80000000u) return false;
    if (r.hi - r.lo >= 4096) return false;
    t.entries.assign(r.hi - r.lo + 1, 0);
    std::set<uint32_t> tg;
    for (uint32_t k = 0; k < t.entries.size(); ++k) {
        const uint32_t a = t.table + 4 * (r.lo + k);  // mod 2^32, as the CPU computes it
        if (!xbe_.mapped(a, 4)) return false;
        const uint32_t v = xbe_.u32(a);
        if (v < sec.va || v >= sec.file_end()) return false;
        t.entries[k] = v;
        tg.insert(v);
    }
    t.first = int32_t(r.lo);
    t.targets.assign(tg.begin(), tg.end());
    t.bounded = true;
    t.how = fact.byte_table ? "range+bytetable" : "range";
    t.index_table = fact.byte_table;
    t.index_count = fact.byte_count;
    return true;
}

// Explore, then prove the size of any heuristically sized jump table with the
// value-range analysis and explore again with the proven tables, until stable.
Cfg::Explored Cfg::explore(uint32_t f, uint32_t bound) {
    std::map<uint32_t, JumpTable> proven;
    for (int round = 0;; ++round) {
        Explored r = explore_once(f, bound, proven);
        std::vector<uint32_t> want;
        for (auto& j : r.jtabs)
            if (!j.bounded && !proven.contains(j.at)) want.push_back(j.at);
        if (want.empty() || round == 4) return r;
        std::vector<uint32_t> entries{f};
        for (auto it = eh_roots_.upper_bound(f); it != eh_roots_.end() && (!bound || *it < bound); ++it)
            if (r.insns.contains(*it)) entries.push_back(*it);
        const auto facts = index_ranges(xbe_, r.insns, entries, r.jtabs, want);
        const XbeSection* sec = xbe_.code_section_at(f);
        bool any = false;
        for (uint32_t w : want) {
            auto jt = std::ranges::find(r.jtabs, w, &JumpTable::at);
            JumpTable p;
            p.at = w;
            p.table = jt->table;
            if (auto fit = facts.find(w); fit != facts.end() && prove_table(fit->second, *sec, p)) {
                proven.emplace(w, std::move(p));
                any = true;
            }
        }
        if (!any) return r;
    }
}

Cfg::Explored Cfg::explore_once(uint32_t f, uint32_t bound, const std::map<uint32_t, JumpTable>& proven) {
    Explored r;
    r.leaders.insert(f);
    std::vector<uint32_t> work{f};
    // Exception-handler entry points inside this function's range.
    if (bound)
        for (auto it = eh_roots_.upper_bound(f); it != eh_roots_.end() && *it < bound; ++it) {
            r.leaders.insert(*it);
            work.push_back(*it);
        }
    const XbeSection* sec_f = xbe_.code_section_at(f);
    std::vector<const Insn*> hist;
    auto inside = [&](uint32_t t) { return !bound || (t >= f && t < bound); };
    while (!work.empty()) {
        uint32_t a = work.back();
        work.pop_back();
        hist.clear();
        for (;;) {
            if (r.insns.contains(a)) break;
            if (xbe_.code_section_at(a) != sec_f) { r.bad.push_back(a); break; }
            const Insn* ins = dec_.decode(a);
            if (!ins) { r.bad.push_back(a); break; }
            // Overlap: does a fall inside the nearest earlier decoded instruction?
            if (auto it = r.insns.lower_bound(a); it != r.insns.begin()) {
                --it;
                if (it->second->end() > a) r.overlap.push_back(a);
            }
            r.insns.emplace(a, ins);
            hist.push_back(ins);
            if (hist.size() > 12) hist.erase(hist.begin());
            for_each_addr_constant(*ins, [&](uint32_t v) { r.imms.insert(v); });

            if (ins->is_ret()) break;
            if (ins->mnem == ZYDIS_MNEMONIC_INT3 && a != f) break;
            if (ins->is_jmp()) {
                if (auto t = ins->rel_target()) {
                    if ((known_.contains(*t) && *t != f) || !inside(*t)) {
                        r.tails.insert(*t);
                    } else {
                        r.leaders.insert(*t);
                        work.push_back(*t);
                    }
                } else {
                    const Operand& m = ins->op[0];
                    // A switch table sits in the code section, after its function.
                    // [4*reg + T] with T in data indexes a table of records (e.g.
                    // handler pointers): an indirect jump, not a switch.
                    const bool jt = ins->nops >= 1 && is_flat_mem(m) && m.size == 32 && m.base == ZYDIS_REGISTER_NONE &&
                                    is_gpr32(m.index) && m.scale == 4 && m.has_disp && m.disp > 0 &&
                                    xbe_.code_section_at(uint32_t(m.disp)) != nullptr;
                    if (jt) {
                        JumpTable j;
                        if (auto p = proven.find(a); p != proven.end()) {
                            j = p->second;
                        } else {
                            j.at = a;
                            j.table = uint32_t(m.disp);
                            resolve_jt(hist, *sec_f, j);
                        }
                        for (uint32_t x : j.targets)
                            if (!inside(x)) j.dropped.push_back(x);
                        std::erase_if(j.targets, [&](uint32_t x) { return !inside(x); });
                        for (auto& e : j.entries)
                            if (e && !inside(e)) e = 0;
                        for (uint32_t x : j.targets) {
                            r.leaders.insert(x);
                            work.push_back(x);
                        }
                        r.jtabs.push_back(std::move(j));
                    } else {
                        r.ijmps.push_back({a, dec_.format(*ins)});
                    }
                }
                break;
            }
            if (ins->is_cond_branch()) {
                if (auto t = ins->rel_target()) {
                    if (!inside(*t)) {
                        r.tails.insert(*t);
                    } else {
                        r.leaders.insert(*t);
                        work.push_back(*t);
                    }
                }
                r.leaders.insert(ins->end());
                a = ins->end();
                continue;
            }
            if (ins->is_call()) {
                if (auto t = ins->rel_target()) r.calls.insert(*t);
                else r.icalls.push_back({a, dec_.format(*ins)});
                const Insn* nxt = dec_.decode(ins->end());
                if (!nxt || nxt->mnem == ZYDIS_MNEMONIC_INT3) break;  // probable noreturn call
            }
            a = ins->end();
        }
    }
    return r;
}

void Cfg::linear_seeds() {
    // Linear sweep of every code section (objdump-style: skip one byte on failure).
    // Uncached: most of what a linear sweep decodes inside data is never needed again.
    enum : uint8_t { Valid = 1, Pad = 2, Ret = 4, Jmp = 8, Call = 16 };
    struct L {
        uint32_t addr;
        uint32_t target;  // direct call target, if Call
        uint8_t kind;
    };
    std::vector<L> lin;
    lin.reserve(700'000);
    Insn tmp;
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        for (uint32_t a = s.va, end = s.file_end(); a < end;) {
            if (dec_.decode_uncached(a, tmp) && tmp.end() <= end) {
                uint8_t k = Valid;
                if (tmp.is_pad()) k |= Pad;
                if (tmp.is_ret()) k |= Ret;
                if (tmp.is_jmp()) k |= Jmp;
                uint32_t t = 0;
                if (tmp.is_call())
                    if (auto rt = tmp.rel_target()) { k |= Call; t = *rt; }
                lin.push_back({a, t, k});
                a = tmp.end();
            } else {
                lin.push_back({a, 0, 0});
                a += 1;
            }
        }
    }
    std::ranges::sort(lin, {}, &L::addr);
    // Index of the valid instruction starting at a, if any.
    auto find = [&](uint32_t a) -> std::optional<size_t> {
        auto it = std::ranges::lower_bound(lin, a, {}, &L::addr);
        if (it == lin.end() || it->addr != a || !(it->kind & Valid)) return std::nullopt;
        return size_t(it - lin.begin());
    };

    enum : uint8_t { RCall = 1, RPad = 2, RData = 4, REntry = 8 };
    std::map<uint32_t, uint8_t> reasons;
    reasons[xbe_.entry] |= REntry;
    for (auto& l : lin)
        if ((l.kind & Call) && find(l.target)) reasons[l.target] |= RCall;
    for (size_t k = 1; k < lin.size(); ++k) {
        if (!(lin[k].kind & Valid) || (lin[k].kind & Pad)) continue;
        size_t j = k - 1, npad = 0;
        while ((lin[j].kind & Pad) && j > 0) {
            ++npad;
            --j;
        }
        if (npad && (lin[j].kind & Valid) && !(lin[j].kind & Pad) && (lin[j].kind & (Ret | Jmp)))
            reasons[lin[k].addr] |= RPad;
    }
    for (auto& s : xbe_.sections) {
        if (s.name != ".rdata" && s.name != ".data") continue;
        for (uint32_t k = 0; k + 3 < s.rsize; k += 4) {
            const uint32_t v = xbe_.u32(s.va + k);
            auto i = find(v);
            if (!i) continue;
            const bool prev_ok = *i == 0 || (lin[*i - 1].kind & (Pad | Ret));
            if (reasons.contains(v) || prev_ok) reasons[v] |= RData;
        }
    }
    for (auto& [a, rs] : reasons) {
        if (rs & (RCall | REntry)) strong_.push_back(a);
        else if ((rs & RData) && entry_ok(a)) strong_.push_back(a);
        else if (rs == RPad && entry_ok(a)) weak_.push_back(a);
    }
}

void Cfg::run_queue(std::vector<uint32_t> q, Origin origin, uint8_t round) {
    while (!q.empty()) {
        const uint32_t f = q.back();
        q.pop_back();
        if (funcs_.contains(f) || !xbe_.code_section_at(f)) continue;
        cand_.insert(f);
        Explored r = explore(f, bound_of(f));
        Function fn = make_function(f, origin, round, r);
        for (uint32_t a : fn.insns) owned_.insert(a);
        funcs_.emplace(f, std::move(fn));
        for (const auto* targets : {&r.calls, &r.tails})
            for (uint32_t c : *targets)
                if (!funcs_.contains(c) && xbe_.code_section_at(c) && (targets == &r.calls || entry_ok(c))) {
                    known_.insert(c);
                    cand_.insert(c);
                    q.push_back(c);
                }
        for (uint32_t v : r.imms) {
            if (!entry_ok(v)) continue;
            evidence_.insert(v);
            if (known_.contains(v) && !funcs_.contains(v)) q.push_back(v);
            else if (xbe_.code_section_at(v) && !funcs_.contains(v) && !owned_.contains(v)) imm_seeds_.insert(v);
        }
    }
}

Function Cfg::make_function(uint32_t f, Origin origin, uint8_t round, Explored& r) {
    Function fn;
    fn.start = f;
    fn.origin = origin;
    fn.round = round;
    fn.insns.reserve(r.insns.size());
    fn.lo = r.insns.empty() ? f : r.insns.begin()->first;
    fn.hi = f;
    for (auto& [a, i] : r.insns) {
        fn.insns.push_back(a);
        fn.bytes += i->len;
        fn.hi = std::max(fn.hi, i->end());
    }
    fn.leaders = std::move(r.leaders);
    fn.calls = r.calls;
    fn.tails = r.tails;
    // A branch recorded as a tail call while a (since dropped) candidate stood
    // at its target, when the function's own code also reaches that target:
    // it's a label, not a tail call.
    std::erase_if(fn.tails, [&](uint32_t t) { return r.insns.contains(t); });
    fn.icalls = std::move(r.icalls);
    fn.ijmps = std::move(r.ijmps);
    fn.jtabs = std::move(r.jtabs);
    fn.bad = std::move(r.bad);
    fn.overlap = std::move(r.overlap);
    return fn;
}

bool Cfg::in_aligned_region(uint32_t a) const {
    return std::ranges::any_of(cfg_.aligned_regions, [&](auto r) { return a >= r.first && a < r.second; });
}

bool Cfg::entry_ok(uint32_t a) {
    for (auto [lo, hi] : cfg_.aligned_regions)
        if (a >= lo && a < hi && a % cfg_.alignment) return false;
    const Insn* i = dec_.decode(a);
    return i && !i->is_pad();  // a function never starts on alignment padding
}

bool Cfg::plausible(const Function& f) const {
    if (!f.bad.empty() || !f.overlap.empty()) return false;
    return std::ranges::none_of(f.insns, [&](uint32_t a) { return dec_.decode(a)->implausible_reason() != nullptr; });
}

// A function's exploration stops at the next candidate start. When a
// candidate is false (a heuristic seed in the middle of a real function), the
// real function is cut short: its tail ends up in a gap or as a separate
// "fragment" that the lifter rejects. Fix: if function G jumps (tail jump,
// branch, or jump-table case) to t beyond its bound, and none of the
// candidates between G and t has independent evidence of being an entry (a
// direct call, a data pointer, an immediate, the entry point), those
// candidates are not functions: drop them and re-explore G. Repeat until stable.
// G ends a block with `jmp c` where everything between the jmp and c is
// alignment filler: MSVC aligning a loop head inside G, not a tail call.
bool Cfg::jumps_over_filler(const Function& g, uint32_t c) const {
    for (uint32_t a : g.insns) {
        const Insn* i = dec_.decode(a);
        if (i->mnem != ZYDIS_MNEMONIC_JMP || i->rel_target() != c || i->end() >= c || c - i->end() > 15) continue;
        uint32_t x = i->end();
        while (x < c) {
            const Insn* p = dec_.decode(x);
            if (!p || !(p->is_pad() || is_pad_filler(*p))) break;
            x = p->end();
        }
        if (x == c) return true;
    }
    return false;
}

// G's instruction just before f ends at f and continues into it (not a
// ret, jmp or int3).
bool Cfg::falls_into(const Function& g, uint32_t f) const {
    auto it = std::ranges::lower_bound(g.insns, f);
    if (it == g.insns.begin() || it == g.insns.end() || *it != f) return false;
    const Insn* p = dec_.decode(*std::prev(it));
    return p->end() == f && !p->is_ret() && !p->is_jmp() && p->mnem != ZYDIS_MNEMONIC_INT3;
}

// A function whose entry is an instruction of an earlier function G, reached
// only by jumps (no call, no EH table, no pointer to it anywhere), is a label
// inside G that an earlier, shorter exploration of G mistook for a tail call.
bool Cfg::drop_labels() {
    std::set<uint32_t> called(eh_unwind_.begin(), eh_unwind_.end());
    called.insert(xbe_.entry);
    for (auto& [_, fn] : funcs_) called.insert(fn.calls.begin(), fn.calls.end());
    std::vector<uint32_t> drop;
    for (auto it = funcs_.begin(); it != funcs_.end(); ++it) {
        const auto& [gs, g] = *it;
        if (!plausible(g)) continue;
        for (auto jt = std::next(it); jt != funcs_.end() && !g.insns.empty() && jt->first <= g.insns.back(); ++jt) {
            const uint32_t f = jt->first;
            if (called.contains(f)) continue;
            // Weak evidence (a data value or immediate equal to f) doesn't make
            // f an entry when G's previous instruction falls into it, or G
            // jumps to it over alignment filler: no function starts that way.
            if (evidence_.contains(f) && !jumps_over_filler(g, f) && !falls_into(g, f)) continue;
            if (xbe_.u8(f - 1) == 0xCC) continue;  // after int3 padding: an entry
            if (std::ranges::binary_search(g.insns, f) || jumps_over_filler(g, f)) drop.push_back(f);
        }
    }
    if (drop.empty()) return false;
    for (uint32_t d : drop) {
        funcs_.erase(d);
        cand_.erase(d);
        known_.erase(d);
    }
    absorbed_ += drop.size();
    rebuild_owned();
    return true;
}

bool Cfg::absorb_fragments() {
    bool any = false;
    std::vector<uint32_t> follow_up;  // call/tail targets discovered by re-exploration
    for (bool changed = true; changed;) {
        changed = false;
        // Strong evidence: the entry point, direct calls, EH tables. Weak:
        // data values and immediates (a constant can coincide with an address).
        std::set<uint32_t> strong(eh_unwind_.begin(), eh_unwind_.end());
        strong.insert(xbe_.entry);
        for (auto& [_, fn] : funcs_) {
            strong.insert(fn.calls.begin(), fn.calls.end());
            // A tail jump to an address right after int3 padding: MSVC pads with
            // int3 only between functions, so that's an entry.
            for (uint32_t t : fn.tails)
                if (xbe_.mapped(t - 1) && xbe_.u8(t - 1) == 0xCC) strong.insert(t);
        }
        const std::set<uint32_t>& weak = evidence_;
        // One pass in address order. After a merge, G is re-examined in place
        // (its new tails may absorb more); the pass then carries on from G.
        // A pass with any merge is followed by another with fresh evidence.
        for (auto it = funcs_.begin(); it != funcs_.end();) {
            const uint32_t gs = it->first;
            bool merged = false;
            const Function& g = it->second;
            if (!plausible(g)) {
                ++it;
                continue;
            }
            std::set<uint32_t> into(g.tails.begin(), g.tails.end());
            for (auto& j : g.jtabs) into.insert(j.dropped.begin(), j.dropped.end());
            const XbeSection* sec = xbe_.code_section_at(gs);
            for (uint32_t t : into) {
                if (t <= gs || xbe_.code_section_at(t) != sec) continue;
                // Candidates in (gs, t]: every one must lack evidence.
                std::vector<uint32_t> blockers(cand_.upper_bound(gs), cand_.upper_bound(t));
                if (blockers.empty()) continue;
                if (std::ranges::any_of(blockers, [&](uint32_t c) { return strong.contains(c); })) continue;
                // Weak evidence stands unless G's own straight-line code runs into
                // that entry (falls through, or jumps over alignment filler to
                // it): no function starts where another one falls through.
                if (std::ranges::any_of(blockers, [&](uint32_t c) {
                        return weak.contains(c) && !std::ranges::binary_search(g.insns, c) && !jumps_over_filler(g, c);
                    }))
                    continue;
                for (uint32_t c : blockers) {
                    funcs_.erase(c);
                    cand_.erase(c);
                    known_.erase(c);
                }
                absorbed_ += blockers.size();
                Explored r = explore(gs, bound_of(gs));
                Function fn = make_function(gs, g.origin, g.round, r);
                for (const auto* s : {&fn.calls, &fn.tails}) follow_up.insert(follow_up.end(), s->begin(), s->end());
                for (uint32_t c : fn.calls) strong.insert(c);
                for (uint32_t tt : fn.tails)
                    if (xbe_.mapped(tt - 1) && xbe_.u8(tt - 1) == 0xCC) strong.insert(tt);
                funcs_.insert_or_assign(gs, std::move(fn));
                changed = any = merged = true;
                break;
            }
            it = merged ? funcs_.find(gs) : std::next(it);
        }
    }
    if (any) {
        rebuild_owned();
        std::erase_if(follow_up, [&](uint32_t t) { return funcs_.contains(t) || !xbe_.code_section_at(t); });
        for (uint32_t t : follow_up) known_.insert(t);
        run_queue(std::move(follow_up), Origin::Reachable);
    }
    return any;
}

// Immediates that point at code (callbacks registered with `mov [x], imm`,
// `push imm`). Runs again in the final fixpoint, because code found late can
// hold new ones. Returns whether any function was added.
bool Cfg::imm_round() {
    bool added = false;
    // Until none is left: a function found here can end where the next one,
    // known only by an immediate, starts (a table of callbacks filled in
    // code, each after the one before it).
    for (std::vector<uint32_t> ok;; ok.clear()) {
    for (uint32_t v : imm_seeds_) {
        if (owned_.contains(v) || funcs_.contains(v) || !xbe_.mapped(v - 1)) continue;
        const uint8_t prev = xbe_.u8(v - 1);
        // After padding/ret anywhere; in the aligned regions an aligned,
        // clean entry is evidence enough (the byte before may be a table).
        const bool after_pad = prev == 0xCC || prev == 0x90 || prev == 0xC3;
        // Or right after a `ret n` (an instruction of a function already
        // found, owned_ holding instruction starts) that ends there:
        // the previous function's end (unaligned library code packs functions so).
        const bool after_ret_n = v >= 3 && xbe_.mapped(v - 3) && xbe_.u8(v - 3) == 0xC2 && owned_.contains(v - 3) && [&] {
                                     const Insn* r = dec_.decode(v - 3);
                                     return r && r->len == 3 && r->mnem == ZYDIS_MNEMONIC_RET;
                                 }();
        // Or right after an unconditional jmp of a function already found
        // (its last instruction, a loop's back edge or a tail jump), which
        // nothing in that function jumped past (v isn't owned).
        const bool after_jmp = [&] {
            for (const uint32_t len : {2u, 5u}) {
                if (v < len || !xbe_.mapped(v - len) || !owned_.contains(v - len)) continue;
                const Insn* j = dec_.decode(v - len);
                if (j && j->len == len && j->mnem == ZYDIS_MNEMONIC_JMP && (xbe_.u8(v - len) == 0xEB || xbe_.u8(v - len) == 0xE9))
                    return true;
            }
            return false;
        }();
        if (!entry_ok(v)) continue;
        if (after_pad || after_ret_n || after_jmp || (in_aligned_region(v) && (clean_seed(v) || displaces_weak_seed(v)))) ok.push_back(v);
    }
    if (ok.empty()) break;
    run_queue({ok.rbegin(), ok.rend()}, Origin::ImmPtr);
    added = true;
    }
    // Data and immediate pointers still blocked only by weak seeds inside them.
    for (const auto* seeds : {&dptr_, &imm_seeds_})
        for (uint32_t v : *seeds)
            if (in_aligned_region(v) && !funcs_.contains(v) && !owned_.contains(v))
                added |= take_over(v, seeds == &dptr_ ? Origin::DataPtr : Origin::ImmPtr);
    return added;
}

// v's exploration is clean except that it runs through code owned by a
// weakly-evidenced function (data-pointer / orphan / gap / imm seed) whose
// entry lies strictly inside one of v's instructions: that function is the
// misaligned one. Accepting v lets prune_misaligned() remove it.
bool Cfg::displaces_weak_seed(uint32_t v) {
    if (funcs_.contains(v) || owned_.contains(v)) return false;
    Explored r = explore(v, bound_of(v));
    if (!r.bad.empty() || !r.overlap.empty() || r.insns.size() < 3) return false;
    bool displaces = false;
    for (auto& [a, i] : r.insns) {
        if (is_zero_bytes_insn(*i) || i->implausible_reason()) return false;
        for (auto it = funcs_.upper_bound(a); it != funcs_.end() && it->first < i->end(); ++it)
            if (it->second.origin != Origin::Reachable && it->second.origin != Origin::Eh) displaces = true;
    }
    return displaces;
}

// v explores cleanly, and every already-owned instruction it reaches belongs
// to a weakly-evidenced function (orphan / gap / data-pointer / imm seed)
// whose *entry* v's own code reaches: those entries are labels inside v.
// Drop them and add v. Returns whether v was added.
bool Cfg::take_over(uint32_t v, Origin origin) {
    if (funcs_.contains(v) || owned_.contains(v) || !entry_ok(v)) return false;
    Explored r = explore(v, bound_of(v));
    if (!r.bad.empty() || !r.overlap.empty() || r.insns.size() < 3) return false;
    std::vector<uint32_t> victims;
    for (auto& [a, i] : r.insns) {
        if (is_zero_bytes_insn(*i) || i->implausible_reason()) return false;
        if (auto it = funcs_.find(a); it != funcs_.end()) {
            const Origin o = it->second.origin;
            if (o == Origin::Reachable || o == Origin::Eh) return false;
            victims.push_back(a);
        }
    }
    if (victims.empty()) return false;
    std::set<uint32_t> covered;
    for (uint32_t w : victims) covered.insert(funcs_.at(w).insns.begin(), funcs_.at(w).insns.end());
    for (auto& [a, i] : r.insns)
        if (owned_.contains(a) && !covered.contains(a)) return false;  // owned by someone stronger
    for (uint32_t w : victims) {
        funcs_.erase(w);
        cand_.erase(w);
        known_.erase(w);
    }
    absorbed_ += victims.size();
    rebuild_owned();
    run_queue({v}, origin);
    return true;
}

bool Cfg::clean_seed(uint32_t v) {
    if (funcs_.contains(v) || owned_.contains(v)) return false;
    Explored r = explore(v, bound_of(v));
    if (!r.bad.empty() || !r.overlap.empty() || r.insns.size() < 3) return false;
    return std::ranges::none_of(r.insns, [&](auto& p) { return owned_.contains(p.first) || is_zero_bytes_insn(*p.second); });
}

void Cfg::rebuild_owned() {
    owned_.clear();
    for (auto& [_, fn] : funcs_)
        for (uint32_t a : fn.insns) owned_.insert(a);
}

// MSVC C++ EH: FuncInfo { magic 0x1993052x; maxState; UnwindMapEntry* (8 bytes:
// toState, action); nTryBlocks; TryBlockMapEntry* (20 bytes: tryLow, tryHigh,
// catchHigh, nCatches, HandlerType*); ... }, HandlerType (16 bytes: adjectives,
// type, dispCatchObj, addressOfHandler). A catch handler is called by the
// runtime with the parent's frame and returns the continuation address in eax
// (`mov eax, imm32; ret`).
// MSVC structured exception handling (__try/__except/__finally) in the CRT and
// XAPI: the function's prologue pushes a scope table, either via
// __SEH_prolog (`push scopetable; call X`, X starting `push imm32; mov eax,
// fs:[0]`) or inline (`push -1; push scopetable; push handler; mov eax,
// fs:[0]`). Each table entry is {enclosing level, filter, handler}. For an
// __except (filter != 0), the filter expression and the handler body are
// laid out inside the parent function, and the handler continues into the
// parent's code: both are entry points inside the parent (eh_roots_), never
// functions of their own. A __finally (filter 0) handler is a real function
// the parent also calls directly: nothing to do.
void Cfg::scan_seh_tables() {
    auto code = [&](uint32_t a) { return xbe_.code_section_at(a) != nullptr; };
    auto data = [&](uint32_t a) {
        const XbeSection* s = xbe_.section_at(a);
        return s && !s->code && xbe_.mapped(a, 12);
    };
    auto fs0_load = [&](uint32_t a) {  // mov eax, dword ptr fs:[0]
        return xbe_.mapped(a, 6) && xbe_.u8(a) == 0x64 && xbe_.u8(a + 1) == 0xA1 && xbe_.u32(a + 2) == 0;
    };
    std::set<uint32_t> tables;
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        for (uint32_t a = s.va; a + 12 <= s.file_end(); ++a) {
            if (xbe_.u8(a) == 0x68 && xbe_.u8(a + 5) == 0xE8) {
                const uint32_t t = xbe_.u32(a + 1), x = a + 10 + xbe_.u32(a + 6);
                if (data(t) && code(x) && xbe_.mapped(x, 11) && xbe_.u8(x) == 0x68 && fs0_load(x + 5)) tables.insert(t);
            }
            if (xbe_.u8(a) == 0x6A && xbe_.u8(a + 1) == 0xFF && xbe_.u8(a + 2) == 0x68 && xbe_.u8(a + 7) == 0x68 &&
                fs0_load(a + 12)) {
                const uint32_t t = xbe_.u32(a + 3);
                if (data(t)) tables.insert(t);
            }
        }
    }
    std::set<uint32_t> roots;
    for (auto it = tables.begin(); it != tables.end(); ++it) {
        const uint32_t t = *it, next = std::next(it) == tables.end() ? UINT32_MAX : *std::next(it);
        ++seh_tables_;
        for (uint32_t k = 0; t + 12 * (k + 1) <= next && data(t + 12 * k); ++k) {
            const uint32_t enclosing = xbe_.u32(t + 12 * k), filter = xbe_.u32(t + 12 * k + 4), handler = xbe_.u32(t + 12 * k + 8);
            if (!(enclosing == 0xFFFFFFFFu || enclosing < k) || !(filter == 0 || code(filter)) || !code(handler)) break;
            if (filter == 0) continue;
            roots.insert(filter);
            roots.insert(handler);
            ++seh_excepts_;
        }
    }
    eh_roots_.insert(roots.begin(), roots.end());
    std::erase_if(strong_, [&](uint32_t a) { return roots.contains(a); });
    std::erase_if(weak_, [&](uint32_t a) { return roots.contains(a); });
}

void Cfg::scan_eh_tables() {
    auto code = [&](uint32_t a) { return xbe_.code_section_at(a) != nullptr; };
    auto rd = [&](uint32_t a, uint32_t& v) {
        if (!xbe_.mapped(a, 4)) return false;
        v = xbe_.u32(a);
        return true;
    };
    std::set<uint32_t> handlers;
    std::vector<uint32_t> funcinfos;
    for (auto& s : xbe_.sections) {
        if (s.code) continue;  // the tables live in data sections (.rdata)
        // Up to the virtual end: a table can run into the section's zero-filled tail.
        for (uint32_t a = s.va; a + 20 <= s.end(); a += 4) {
            const uint32_t magic = xbe_.u32(a);
            if (magic < 0x19930520 || magic > 0x19930522) continue;
            uint32_t max_state = 0, unwind = 0, ntry = 0, ptry = 0;
            if (!rd(a + 4, max_state) || !rd(a + 8, unwind) || !rd(a + 12, ntry) || !rd(a + 16, ptry)) continue;
            if (max_state > 4096 || ntry > 1024) continue;
            std::vector<uint32_t> acts, hs;
            bool ok = true;
            for (uint32_t k = 0; k < max_state && ok; ++k) {
                uint32_t act;
                ok = rd(unwind + 8 * k + 4, act) && (act == 0 || code(act));
                if (ok && act) acts.push_back(act);
            }
            for (uint32_t t = 0; t < ntry && ok; ++t) {
                uint32_t ncatch = 0, ph = 0;
                ok = rd(ptry + 20 * t + 12, ncatch) && rd(ptry + 20 * t + 16, ph) && ncatch <= 64;
                for (uint32_t c = 0; c < ncatch && ok; ++c) {
                    uint32_t h;
                    ok = rd(ph + 16 * c + 12, h) && code(h);
                    if (ok) hs.push_back(h);
                }
            }
            if (!ok) continue;  // not a FuncInfo after all
            ++eh_funcinfos_;
            funcinfos.push_back(a);
            eh_unwind_.insert(acts.begin(), acts.end());
            handlers.insert(hs.begin(), hs.end());
        }
    }
    // __ehhandler stubs: `mov eax, &FuncInfo; jmp __CxxFrameHandler` (B8 imm32 E9 rel32),
    // one per function with C++ EH; the function's prologue pushes the stub's address.
    std::ranges::sort(funcinfos);
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        for (uint32_t a = s.va; a + 10 <= s.file_end(); ++a) {
            if (xbe_.u8(a) != 0xB8 || xbe_.u8(a + 5) != 0xE9) continue;
            if (!std::ranges::binary_search(funcinfos, xbe_.u32(a + 1))) continue;
            eh_unwind_.insert(a);  // same treatment as unwind funclets: separate EH-only functions
            ++eh_stubs_;
        }
    }

    // Continuations: every `mov eax, imm32` directly followed by `ret` in handler code.
    for (uint32_t h : handlers) {
        eh_roots_.insert(h);
        Explored r = explore(h, 0);
        for (auto& [a, i] : r.insns) {
            if (i->mnem != ZYDIS_MNEMONIC_MOV || i->nops != 2 || !i->op[0].is_reg(ZYDIS_REGISTER_EAX) ||
                i->op[1].kind != Operand::Kind::Imm)
                continue;
            const Insn* n = dec_.decode(i->end());
            const uint32_t cont = uint32_t(i->op[1].imm);
            if (n && n->is_ret() && code(cont)) eh_roots_.insert(cont);
        }
    }
}

void Cfg::run() {
    linear_seeds();
    scan_eh_tables();
    scan_seh_tables();
    known_.insert(strong_.begin(), strong_.end());
    cand_.insert(strong_.begin(), strong_.end());
    cand_.insert(weak_.begin(), weak_.end());

    // Aligned code pointers stored in data sections (vtables, callback tables).
    std::set<uint32_t>& dptr = dptr_;
    for (auto& s : xbe_.sections) {
        const bool lib = s.name == cfg_.lib_data_section;
        if (s.name != ".rdata" && s.name != ".data" && !lib) continue;
        const uint32_t lo = lib ? cfg_.lib_dataptr_lo : cfg_.dataptr_lo, hi = lib ? cfg_.lib_dataptr_hi : cfg_.dataptr_hi;
        for (uint32_t k = 0; k + 3 < s.rsize; k += 4) {
            const uint32_t v = xbe_.u32(s.va + k);
            if (v >= lo && v < hi && v % 16 == 0 && dec_.decode(v) && entry_ok(v) && !eh_roots_.contains(v))
                dptr.insert(v);
        }
    }

    {
        std::vector<uint32_t> q(strong_.rbegin(), strong_.rend());
        q.push_back(xbe_.entry);
        run_queue(std::move(q), Origin::Reachable);
    }
    {
        // Unwind funclets: separate functions, entered only via the EH tables.
        std::vector<uint32_t> q;
        for (uint32_t u : eh_unwind_ | std::views::reverse)
            if (!funcs_.contains(u)) q.push_back(u);
        known_.insert(eh_unwind_.begin(), eh_unwind_.end());
        evidence_.insert(eh_unwind_.begin(), eh_unwind_.end());
        run_queue(std::move(q), Origin::Eh);
    }
    for (uint8_t round = 0; round < 4; ++round) {
        std::vector<uint32_t> acc;
        for (uint32_t v : dptr)
            if (clean_seed(v)) acc.push_back(v);
        if (acc.empty()) break;
        known_.insert(acc.begin(), acc.end());
        cand_.insert(acc.begin(), acc.end());
        run_queue({acc.rbegin(), acc.rend()}, Origin::DataPtr, round);
    }
    {
        std::vector<uint32_t> q;
        for (uint32_t w : weak_ | std::views::reverse)
            if (!owned_.contains(w)) q.push_back(w);
        run_queue(std::move(q), Origin::Orphan);
    }
    imm_round();

    for (uint32_t s : strong_) evidence_.insert(s);
    evidence_.insert(dptr.begin(), dptr.end());
    absorb_fragments();

    // Gap seeding: a 16-aligned decodable start after padding, in remaining gaps.
    // Known tables (jump tables at their full length, byte index tables) are data.
    auto data_ranges = [&] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (auto& [_, fn] : funcs_)
            for (auto& j : fn.jtabs) {
                v.emplace_back(j.data_lo(), j.data_hi());
                if (j.index_table) v.emplace_back(j.index_table, j.index_table + j.index_count);
            }
        std::ranges::sort(v);
        return v;
    };
    auto ranges = data_ranges();
    for (uint8_t round = 0; round < 6; ++round) {
        std::vector<uint32_t> fresh;
        for (auto& s : xbe_.sections) {
            if (!s.code) continue;
            for (uint32_t a = s.va, hi = s.file_end(); a < hi;) {
                if (owned_.contains(a)) { a = dec_.decode(a)->end(); continue; }
                const Insn* ins = dec_.decode(a);
                if (!ins) { a += 1; continue; }
                if (ins->is_pad() || is_pad_filler(*ins)) { a = ins->end(); continue; }
                // Inside a known table? (ranges sorted by start; take the first that covers a)
                std::optional<uint32_t> skip_to;
                for (auto& [s0, e] : ranges) {
                    if (s0 > a) break;
                    if (a < e) { skip_to = e; break; }
                }
                if (skip_to) { a = *skip_to; continue; }
                if (s.name == cfg_.gap_section && a % 16 == 0 && xbe_.mapped(a - 1)) {
                    const uint8_t prevb = xbe_.u8(a - 1);
                    // ...or right after a known table, which can end mid-alignment.
                    const bool after_table = in_aligned_region(a) &&
                                             std::ranges::any_of(ranges, [&](auto& r) { return r.first < a && a - 1 < r.second; });
                    if (prevb == 0xCC || prevb == 0x90 || prevb == 0xC3 || after_table) {
                        Explored r = explore(a, bound_of(a));
                        bool clean = r.bad.empty() && r.overlap.empty() && r.insns.size() >= 3;
                        bool has_exit = false;
                        for (auto& [ia, i] : r.insns) {
                            if (owned_.contains(ia) || is_zero_bytes_insn(*i)) clean = false;
                            if (i->is_ret() || i->is_jmp()) has_exit = true;
                        }
                        if (clean && has_exit) fresh.push_back(a);
                    }
                }
                a = (a + 4) & ~3u;
            }
        }
        std::erase_if(fresh, [&](uint32_t n) { return funcs_.contains(n); });
        if (fresh.empty()) break;
        cand_.insert(fresh.begin(), fresh.end());
        run_queue({fresh.rbegin(), fresh.rend()}, Origin::Gap, round);
        ranges = data_ranges();
    }
    // Fixpoint: candidates left behind by dropped seeds still cut real
    // functions short. Rebuild the boundaries from the surviving functions,
    // re-explore, absorb, validate; repeat until nothing changes.
    for (int round = 0; round < 8; ++round) {
        const bool i = imm_round();
        const bool a = absorb_fragments();
        validate();
        const bool l = drop_labels();
        const bool r = reexplore_all();
        if (!i && !a && !l && !r) break;
    }
    compute_gaps();
}

// Re-explore every function with boundaries taken only from the functions
// that survived (plus nothing else). Returns whether anything changed.
bool Cfg::reexplore_all() {
    cand_.clear();
    for (auto& [s, _] : funcs_) cand_.insert(s);
    bool changed = false;
    std::vector<uint32_t> follow_up;
    for (auto& [s, fn] : funcs_) {
        Explored r = explore(s, bound_of(s));
        Function nf = make_function(s, fn.origin, fn.round, r);
        if (nf.insns != fn.insns) {
            changed = true;
            for (const auto* t : {&nf.calls, &nf.tails}) follow_up.insert(follow_up.end(), t->begin(), t->end());
            fn = std::move(nf);
        }
    }
    rebuild_owned();
    std::erase_if(follow_up, [&](uint32_t t) { return funcs_.contains(t) || !xbe_.code_section_at(t); });
    if (!follow_up.empty()) {
        // New call targets are functions; new tail targets only if they can be entries.
        known_.insert(follow_up.begin(), follow_up.end());
        run_queue(std::move(follow_up), Origin::Reachable);
        changed = true;
    }
    return changed;
}

// Remove functions that are data decoded as code, repeating until stable:
//   - entry inside another function's instruction (prune_misaligned), and
//   - decode failures, self-overlapping instructions, or instructions no
//     MSVC-compiled retail code contains (Insn::implausible_reason),
// unless the function is the entry point or a direct call/jump target of a
// surviving function. Problems in such kept functions are recorded in
// suspects_ (expected only in the XDK library code that gets replaced).
void Cfg::validate() {
    for (;;) {
        prune_misaligned();
        std::set<uint32_t> targets{xbe_.entry};
        for (auto& [_, fn] : funcs_) {
            targets.insert(fn.calls.begin(), fn.calls.end());
            targets.insert(fn.tails.begin(), fn.tails.end());
        }
        std::vector<uint32_t> drop;
        suspects_.clear();
        // Known tables (jump tables, byte index tables) are data: code may not overlap them.
        // (A function's own unused leading slots may sit on its code: MSVC biases
        // the table base when the lowest index can't occur. Other functions' tables, never.)
        struct T {
            uint32_t lo, hi, owner;
        };
        std::vector<T> tables;
        for (auto& [s0, fn] : funcs_)
            for (auto& j : fn.jtabs) {
                tables.push_back({j.data_lo(), j.table + 4 * uint32_t(j.entries.size()), s0});
                if (j.index_table) tables.push_back({j.index_table, j.index_table + j.index_count, s0});
            }
        std::ranges::sort(tables, {}, &T::lo);
        auto overlaps_table = [&](uint32_t a, uint32_t end, uint32_t self) {
            auto it = std::ranges::upper_bound(tables, a, {}, &T::lo);
            for (auto k = it; k != tables.begin();) {  // tables starting at or before a
                --k;
                if (k->hi > a && k->owner != self) return true;
                if (a - k->lo > 0x10000) break;
            }
            for (; it != tables.end() && it->lo < end; ++it)
                if (it->owner != self) return true;
            return false;
        };
        for (auto& [start, fn] : funcs_) {
            std::string why;
            if (!fn.bad.empty()) why = "decode failure";
            else if (auto it = std::ranges::find_if(fn.insns, [&](uint32_t a) { return overlaps_table(a, dec_.decode(a)->end(), start); });
                     it != fn.insns.end())
                why = std::format("instruction at {:08X} overlaps a jump/index table", *it);
            else if (!fn.overlap.empty()) why = "overlapping instructions";
            else
                for (uint32_t a : fn.insns)
                    if (const char* r = dec_.decode(a)->implausible_reason()) {
                        why = std::format("{} at {:08X}", r, a);
                        break;
                    }
            if (why.empty()) continue;
            if (targets.contains(start)) suspects_.push_back({start, std::move(why)});
            else drop.push_back(start);
        }
        if (drop.empty()) break;
        for (uint32_t d : drop) funcs_.erase(d);
        rejected_.insert(rejected_.end(), drop.begin(), drop.end());
        rebuild_owned();
    }
    std::ranges::sort(rejected_);
}

// A function whose entry lies strictly inside an instruction of another
// function means one of the two is data decoded as code. Drop the one with
// the weaker evidence: a seed that only looked like a code pointer, or else a
// host function that was itself only an orphan/heuristic seed. If both are
// reachable, record a conflict for review. Repeat until stable.
// Evidence strength of a function's origin: reached from code or EH tables (3),
// a pointer stored in data or an immediate (2), a heuristic orphan/gap seed (1).
static int strength(Origin o) {
    switch (o) {
    case Origin::Reachable:
    case Origin::Eh: return 3;
    case Origin::DataPtr:
    case Origin::ImmPtr: return 2;
    default: return 1;
    }
}

void Cfg::prune_misaligned() {
    struct Span {
        uint32_t start, end, func;  // an instruction and the function owning it
    };
    std::vector<Span> spans;
    for (;;) {
        spans.clear();
        for (auto& [start, fn] : funcs_)
            for (uint32_t a : fn.insns) spans.push_back({a, dec_.decode(a)->end(), start});
        // By address; where two functions share an instruction, the lower-addressed
        // owner sorts last so the lookup below sees it (deterministic tie-break).
        std::ranges::sort(spans, [](const Span& a, const Span& b) {
            return a.start != b.start ? a.start < b.start : a.func > b.func;
        });
        std::set<uint32_t> drop;
        conflicts_.clear();
        for (auto& [start, fn] : funcs_) {
            // The instruction starting closest below `start`: does it cover it?
            auto it = std::ranges::lower_bound(spans, start, {}, &Span::start);
            if (it == spans.begin()) continue;
            const Span& prev = *std::prev(it);
            if (prev.end <= start) continue;
            const Function& host = funcs_.at(prev.func);
            // Drop the weaker of the two; on a tie the inner seed is the misaligned one.
            const int s_seed = strength(fn.origin), s_host = strength(host.origin);
            if (s_seed == 3 && s_host == 3) conflicts_.push_back(start);
            else if (s_host < s_seed) drop.insert(host.start);  // the host is data as code
            else drop.insert(start);                             // the seed is misaligned
        }
        if (drop.empty()) break;
        for (uint32_t d : drop) funcs_.erase(d);
        pruned_.insert(pruned_.end(), drop.begin(), drop.end());
        rebuild_owned();
    }
    std::ranges::sort(pruned_);
}

void Cfg::compute_gaps() {
    gaps_.clear();
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        SectionGaps sg{&s, {}};
        for (uint32_t a = s.va, hi = s.file_end(); a < hi;) {
            if (owned_.contains(a)) { a = dec_.decode(a)->end(); continue; }
            const uint32_t g = a;
            bool nonpad = false;
            for (; a < hi && !owned_.contains(a); ++a) {
                const uint8_t b = xbe_.u8(a);
                if (b != 0xCC && b != 0x90 && b != 0x00) nonpad = true;
            }
            if (nonpad) sg.gaps.push_back({g, a - g});
        }
        gaps_.push_back(std::move(sg));
    }
}

std::vector<CoverageRegion> Cfg::coverage() {
    enum : uint8_t { Unknown, Code, Table, Padding };
    std::vector<CoverageRegion> out;
    std::vector<std::pair<uint32_t, uint32_t>> tables;
    for (auto& [_, fn] : funcs_)
        for (auto& j : fn.jtabs) {
            tables.emplace_back(j.data_lo(), j.table + 4 * uint32_t(j.entries.size()));
            if (j.index_table) tables.emplace_back(j.index_table, j.index_table + j.index_count);
        }
    auto account = [&](CoverageRegion reg) {
        const uint32_t lo = reg.lo, hi = reg.hi;
        std::vector<uint8_t> cls(hi - lo, Unknown);
        for (uint32_t a = lo; a < hi;) {
            if (owned_.contains(a)) {
                const Insn* i = dec_.decode(a);
                for (uint32_t k = 0; k < i->len && a + k < hi; ++k) cls[a + k - lo] = Code;
                a = i->end();
            } else {
                ++a;
            }
        }
        for (auto [t0, t1] : tables)
            for (uint32_t a = std::max(t0, lo); a < std::min(t1, hi); ++a)
                if (cls[a - lo] == Unknown) cls[a - lo] = Table;
        // Padding: int3/nop/zero bytes, alignment filler, and a short jmp over filler.
        auto is_free = [&](uint32_t a, uint32_t n) {
            for (uint32_t k = 0; k < n; ++k)
                if (a + k >= hi || cls[a + k - lo] != Unknown) return false;
            return true;
        };
        auto filler_at = [&](uint32_t a) -> uint32_t {  // length of a filler insn at a, or 0
            const uint8_t b = xbe_.u8(a);
            if (b == 0xCC || b == 0x90 || b == 0x00) return 1;
            Insn ins;
            if (dec_.decode_uncached(a, ins) && (ins.is_pad() || is_pad_filler(ins)) && is_free(a, ins.len)) return ins.len;
            return 0;
        };
        for (uint32_t a = lo; a < hi;) {
            if (cls[a - lo] != Unknown) { ++a; continue; }
            if (uint32_t n = filler_at(a)) {
                for (uint32_t k = 0; k < n; ++k) cls[a + k - lo] = Padding;
                a += n;
                continue;
            }
            // jmp short over filler (EB xx) that lands on non-filler.
            if (xbe_.u8(a) == 0xEB && a + 2 <= hi) {
                const uint32_t t = a + 2 + uint32_t(int8_t(xbe_.u8(a + 1)));
                bool ok = t > a + 2 && t <= hi;
                for (uint32_t x = a + 2; ok && x < t;) {
                    const uint32_t n = filler_at(x);
                    if (!n) ok = false;
                    x += n ? n : 1;
                }
                if (ok) {
                    for (uint32_t x = a; x < t; ++x) cls[x - lo] = Padding;
                    a = t;
                    continue;
                }
            }
            ++a;
        }
        for (uint32_t a = lo; a < hi; ++a) {
            switch (cls[a - lo]) {
                case Code: ++reg.code; break;
                case Table: ++reg.table; break;
                case Padding: ++reg.padding; break;
                default:
                    ++reg.unknown;
                    if (!reg.unknown_spans.empty() && reg.unknown_spans.back().addr + reg.unknown_spans.back().size == a)
                        ++reg.unknown_spans.back().size;
                    else
                        reg.unknown_spans.push_back({a, 1});
            }
        }
        out.push_back(std::move(reg));
    };
    for (auto& s : xbe_.sections) {
        if (!s.code || !s.rsize) continue;
        bool split = false;
        for (auto& r : cfg_.text_regions)
            if (r.lo >= s.va && r.hi <= s.file_end()) {
                split = true;
                account(CoverageRegion{.name = r.name, .lo = r.lo, .hi = r.hi, .lifted = r.lifted});
            }
        if (!split) account(CoverageRegion{.name = s.name, .lo = s.va, .hi = s.file_end(), .lifted = false});
    }
    return out;
}

std::string percent(uint64_t part, uint64_t whole, int decimals) {
    uint64_t scale = 1;
    for (int k = 0; k < decimals; ++k) scale *= 10;
    const uint64_t w = std::max<uint64_t>(1, whole);
    const uint64_t v = (part * 100 * scale * 2 + w) / (2 * w);  // rounded half up
    if (decimals == 0) return std::format("{}", v);
    return std::format("{}.{:0{}}", v / scale, v % scale, decimals);
}

std::string Cfg::summary() const {
    std::string out;
    auto w = std::back_inserter(out);
    std::map<std::string, int> tags;
    for (auto& [_, f] : funcs_) tags[f.tag()]++;
    std::format_to(w, "functions: {} {{", funcs_.size());
    bool first = true;
    for (auto& [t, n] : tags) {
        std::format_to(w, "{}{}: {}", first ? "" : ", ", t, n);
        first = false;
    }
    std::format_to(w, "}}\ninstructions: {}\n", owned_.size());
    out += "coverage by section (bytes decoded / raw size, % of non-padding bytes):\n";
    for (auto& sg : gaps_) {
        const XbeSection& s = *sg.section;
        uint64_t nonpad = 0, covered = 0, gap_bytes = 0;
        for (uint32_t a = s.va; a < s.file_end(); ++a) {
            const uint8_t v = xbe_.u8(a);
            if (v != 0xCC && v != 0x90) ++nonpad;
        }
        for (uint32_t a = s.va; a < s.file_end();) {
            if (owned_.contains(a)) {
                const Insn* i = dec_.decode(a);
                covered += i->len;
                a = i->end();
            } else {
                ++a;
            }
        }
        for (auto& g : sg.gaps) gap_bytes += g.size;
        std::format_to(w, "  {:<10} {:>8} / {:>8}  ({:>5}% of non-pad)  gaps:{:>5} gapbytes:{}\n", s.name, covered,
                       s.rsize, percent(covered, nonpad, 1), sg.gaps.size(), gap_bytes);
    }
    std::map<std::string, int> how;
    size_t njt = 0, ntg = 0, nij = 0, nic = 0, nbad = 0, nov = 0;
    for (auto& [_, f] : funcs_) {
        for (auto& j : f.jtabs) {
            how[j.how]++;
            ++njt;
            ntg += j.targets.size();
        }
        nij += f.ijmps.size();
        nic += f.icalls.size();
        nbad += !f.bad.empty();
        nov += !f.overlap.empty();
    }
    std::format_to(w, "jump tables: {} (", njt);
    bool first_how = true;
    for (auto& [h, k] : how) {
        std::format_to(w, "{}{} {}", first_how ? "" : ", ", h, k);
        first_how = false;
    }
    std::format_to(w, ") targets: {}\n", ntg);
    std::format_to(w, "unresolved indirect jmps: {}\nindirect calls: {}\n", nij, nic);
    std::format_to(w, "functions with bad decode: {}  with overlapping insns: {}\n", nbad, nov);
    std::format_to(w, "pruned misaligned seeds: {}  call targets inside another function's instruction: {}\n",
                   pruned_.size(), conflicts_.size());
    std::format_to(w, "rejected as data decoded as code: {}\n", rejected_.size());
    std::format_to(w, "fragments merged back into their parent function: {}\n", absorbed_);
    std::format_to(w, "C++ EH: {} FuncInfo tables, {} handler stubs, {} unwind funclets, {} catch/continuation entry points\n",
                   eh_funcinfos_, eh_stubs_, eh_unwind_.size() - eh_stubs_, eh_roots_.size() - 2 * seh_excepts_);
    std::format_to(w, "SEH: {} scope tables, {} __except filter/handler pairs (entry points inside their function)\n", seh_tables_,
                   seh_excepts_);
    std::format_to(w, "suspect call targets kept (see `qilin suspects`): {}\n", suspects_.size());
    return out;
}

std::string Cfg::to_json() const {
    JsonWriter j;
    j.begin_object();
    j.key("functions").begin_array();
    for (auto& [a, f] : funcs_) {
        j.begin_object();
        j.kv("start", f.start);
        j.kv("tag", f.tag());
        j.kv("section", xbe_.code_section_at(f.start)->name);
        j.kv("ninsn", f.insns.size());
        j.kv("bytes", f.bytes);
        j.kv("lo", f.lo);
        j.kv("hi", f.hi);
        j.kv("nblocks", f.leaders.size());
        j.key("calls").begin_array();
        for (auto c : f.calls) j.value(c);
        j.end_array();
        j.key("tails").begin_array();
        for (auto c : f.tails) j.value(c);
        j.end_array();
        j.key("icalls").begin_array();
        for (auto& s : f.icalls) j.begin_object().kv("at", s.at).kv("ops", s.text).end_object();
        j.end_array();
        j.key("ijmps").begin_array();
        for (auto& s : f.ijmps) j.begin_object().kv("at", s.at).kv("ops", s.text).end_object();
        j.end_array();
        j.key("jtabs").begin_array();
        for (auto& t : f.jtabs) {
            j.begin_object().kv("at", t.at).kv("table", t.table).kv("n", t.targets.size()).kv("how", t.how);
            j.kv("entries", t.entries.size()).kv("first", t.first).kv("index_table", t.index_table).kv("index_count", t.index_count);
            j.key("dropped").begin_array();
            for (auto x : t.dropped) j.value(x);
            j.end_array();
            j.key("targets").begin_array();
            for (auto x : t.targets) j.value(x);
            j.end_array().end_object();
        }
        j.end_array();
        j.key("bad").begin_array();
        for (auto x : f.bad) j.value(x);
        j.end_array();
        j.key("overlap").begin_array();
        for (auto x : f.overlap) j.value(x);
        j.end_array();
        j.end_object();
    }
    j.end_array();
    j.key("gaps").begin_array();
    for (auto& sg : gaps_) {
        j.begin_object().kv("section", sg.section->name).kv("va", sg.section->va);
        j.key("gaps").begin_array();
        for (auto& g : sg.gaps) j.begin_array().value(g.addr).value(g.size).end_array();
        j.end_array().end_object();
    }
    j.end_array();
    j.end_object();
    return j.str();
}

}  // namespace qilin
