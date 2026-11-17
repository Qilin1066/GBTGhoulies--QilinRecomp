#include "cfg.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>

#include "json.hpp"

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

uint32_t width_mask(uint16_t bits) { return bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1; }

// `xx byte ptr [eax], al` -- what a run of zero bytes decodes to. A function
// that contains it is almost certainly data being decoded as code.
bool is_zero_bytes_insn(const Insn& i) {
    return i.nops >= 2 && i.op[0].kind == Operand::Kind::Mem && i.op[0].size == 8 &&
           i.op[0].base == ZYDIS_REGISTER_EAX && i.op[0].index == ZYDIS_REGISTER_NONE && i.op[0].disp == 0 &&
           i.op[1].is_reg(ZYDIS_REGISTER_AL);
}

// MSVC alignment filler that is not a NOP mnemonic.
bool is_pad_filler(const Insn& i) {
    if (i.nops != 2) return false;
    if (i.mnem == ZYDIS_MNEMONIC_MOV) return i.op[0].is_reg(ZYDIS_REGISTER_EDI) && i.op[1].is_reg(ZYDIS_REGISTER_EDI);
    if (i.mnem == ZYDIS_MNEMONIC_LEA) {
        const auto r = i.op[0].reg;
        if (r != ZYDIS_REGISTER_ECX && r != ZYDIS_REGISTER_ESP && r != ZYDIS_REGISTER_EBX) return false;
        return i.op[1].kind == Operand::Kind::Mem && i.op[1].base == r && i.op[1].index == ZYDIS_REGISTER_NONE &&
               i.op[1].disp == 0;
    }
    return false;
}

// Address-like constants an instruction mentions (immediates and absolute
// non-indexed displacements >= 0x10000). Relative branch offsets are excluded;
// indexed accesses ([4*reg + table]) address data arrays, never code entries.
template <class F> void for_each_addr_constant(const Insn& i, F&& f) {
    for (uint8_t k = 0; k < i.nops; ++k) {
        const Operand& o = i.op[k];
        uint64_t v;
        if (o.kind == Operand::Kind::Imm && !o.imm_relative) v = o.imm & 0xFFFFFFFFu;
        else if (o.kind == Operand::Kind::Mem && o.has_disp && o.disp > 0 && o.index == ZYDIS_REGISTER_NONE)
            v = uint64_t(o.disp) & 0xFFFFFFFFu;
        else continue;
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
    // Walk back (excluding the jmp itself) for: [movzx reg, byte [r2 + T2]] ... cmp idx, N
    for (auto it = hist.rbegin() + 1; it != hist.rend(); ++it) {
        const Insn& i = **it;
        if (i.mnem == ZYDIS_MNEMONIC_MOVZX && !btable && i.nops == 2 && i.op[0].is_reg(reg) && is_flat_mem(i.op[1]) &&
            i.op[1].size == 8 && is_gpr32(i.op[1].base) && i.op[1].index == ZYDIS_REGISTER_NONE && i.op[1].disp > 0) {
            btable = uint32_t(i.op[1].disp);
            idx_reg = i.op[1].base;
            continue;
        }
        if (i.mnem == ZYDIS_MNEMONIC_CMP && i.nops == 2 && i.op[0].kind == Operand::Kind::Reg &&
            i.op[1].kind == Operand::Kind::Imm && index_reg_of(i.op[0].reg) == idx_reg) {
            // The immediate is sign-extended by the decoder; the bound is unsigned at the register's width.
            const uint64_t n = (i.op[1].imm & width_mask(i.op[0].size)) + 1;
            if (n > 4096) break;  // not a plausible switch bound
            if (btable) {
                uint32_t mx = 0;
                for (uint32_t k = 0; k < n; ++k)
                    if (xbe_.mapped(*btable + k)) mx = std::max<uint32_t>(mx, xbe_.u8(*btable + k));
                count = mx + 1;
                out.how = "cmp+bytetable";
                out.index_table = *btable;
                out.index_count = uint32_t(n);
            } else {
                count = uint32_t(n);
                out.how = "cmp";
            }
            break;
        }
    }
    const uint32_t lo = sec.va, hi = sec.file_end();
    const uint32_t limit = count.value_or(512);
    out.bounded = count.has_value();
    std::set<uint32_t> tg;
    for (uint32_t i = 0; i < limit; ++i) {
        const uint32_t a = out.table + 4 * i;
        if (!xbe_.mapped(a, 4)) break;
        const uint32_t v = xbe_.u32(a);
        if (!(v >= lo && v < hi)) {
            if (!count) break;
            out.entries.push_back(0);
            continue;
        }
        out.entries.push_back(v);
        tg.insert(v);
    }
    out.targets.assign(tg.begin(), tg.end());
}

Cfg::Explored Cfg::explore(uint32_t f, uint32_t bound) {
    Explored r;
    r.leaders.insert(f);
    std::vector<uint32_t> work{f};
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
                    const bool jt = ins->nops >= 1 && is_flat_mem(m) && m.size == 32 && m.base == ZYDIS_REGISTER_NONE &&
                                    is_gpr32(m.index) && m.scale == 4 && m.has_disp && m.disp > 0;
                    if (jt) {
                        JumpTable j;
                        j.at = a;
                        j.table = uint32_t(m.disp);
                        resolve_jt(hist, *sec_f, j);
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
        if (rs & (RCall | RData | REntry)) strong_.push_back(a);
        else if (rs == RPad) weak_.push_back(a);
    }
}

void Cfg::run_queue(std::vector<uint32_t> q, Origin origin, uint8_t round) {
    while (!q.empty()) {
        const uint32_t f = q.back();
        q.pop_back();
        if (funcs_.contains(f) || !xbe_.code_section_at(f)) continue;
        cand_.insert(f);
        Explored r = explore(f, bound_of(f));
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
            owned_.insert(a);
        }
        fn.leaders = std::move(r.leaders);
        fn.calls = r.calls;
        fn.tails = r.tails;
        fn.icalls = std::move(r.icalls);
        fn.ijmps = std::move(r.ijmps);
        fn.jtabs = std::move(r.jtabs);
        fn.bad = std::move(r.bad);
        fn.overlap = std::move(r.overlap);
        funcs_.emplace(f, std::move(fn));
        for (const auto* targets : {&r.calls, &r.tails})
            for (uint32_t c : *targets)
                if (!funcs_.contains(c) && xbe_.code_section_at(c)) {
                    known_.insert(c);
                    cand_.insert(c);
                    q.push_back(c);
                }
        for (uint32_t v : r.imms) {
            if (known_.contains(v) && !funcs_.contains(v)) q.push_back(v);
            else if (xbe_.code_section_at(v) && !funcs_.contains(v) && !owned_.contains(v)) imm_seeds_.insert(v);
        }
    }
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

void Cfg::run() {
    linear_seeds();
    known_.insert(strong_.begin(), strong_.end());
    cand_.insert(strong_.begin(), strong_.end());
    cand_.insert(weak_.begin(), weak_.end());

    // Aligned code pointers stored in data sections (vtables, callback tables).
    std::set<uint32_t> dptr;
    for (auto& s : xbe_.sections) {
        if (s.name != ".rdata" && s.name != ".data") continue;
        for (uint32_t k = 0; k + 3 < s.rsize; k += 4) {
            const uint32_t v = xbe_.u32(s.va + k);
            if (v >= cfg_.dataptr_lo && v < cfg_.dataptr_hi && v % 16 == 0 && dec_.decode(v)) dptr.insert(v);
        }
    }

    {
        std::vector<uint32_t> q(strong_.rbegin(), strong_.rend());
        q.push_back(xbe_.entry);
        run_queue(std::move(q), Origin::Reachable);
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
    {
        std::vector<uint32_t> ok;
        for (uint32_t v : imm_seeds_) {
            if (owned_.contains(v) || funcs_.contains(v) || !xbe_.mapped(v - 1)) continue;
            const uint8_t prev = xbe_.u8(v - 1);
            if ((prev == 0xCC || prev == 0x90 || prev == 0xC3) && dec_.decode(v)) ok.push_back(v);
        }
        run_queue({ok.rbegin(), ok.rend()}, Origin::ImmPtr);
    }

    // Gap seeding: a 16-aligned decodable start after padding, in remaining gaps.
    // Known tables (jump tables at their full length, byte index tables) are data.
    auto data_ranges = [&] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (auto& [_, fn] : funcs_)
            for (auto& j : fn.jtabs) {
                v.emplace_back(j.table, j.table + 4 * std::max<uint32_t>(1, uint32_t(j.entries.size())));
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
                    if (prevb == 0xCC || prevb == 0x90 || prevb == 0xC3) {
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
    validate();
    compute_gaps();
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
        for (auto& [start, fn] : funcs_) {
            std::string why;
            if (!fn.bad.empty()) why = "decode failure";
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
            if (fn.origin != Origin::Reachable) drop.insert(start);              // the seed is misaligned
            else if (host.origin != Origin::Reachable) drop.insert(host.start);  // the host is data as code
            else conflicts_.push_back(start);
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
        std::format_to(w, "  {:<10} {:>8} / {:>8}  ({:5.1f}% of non-pad)  gaps:{:>5} gapbytes:{}\n", s.name, covered,
                       s.rsize, 100.0 * double(covered) / double(std::max<uint64_t>(1, nonpad)), sg.gaps.size(),
                       gap_bytes);
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
    std::format_to(w, "jump tables: {} (cmp {}, cmp+bytetable {}, heuristic {}) targets: {}\n", njt, how["cmp"],
                   how["cmp+bytetable"], how["heuristic"], ntg);
    std::format_to(w, "unresolved indirect jmps: {}\nindirect calls: {}\n", nij, nic);
    std::format_to(w, "functions with bad decode: {}  with overlapping insns: {}\n", nbad, nov);
    std::format_to(w, "pruned misaligned seeds: {}  call targets inside another function's instruction: {}\n",
                   pruned_.size(), conflicts_.size());
    std::format_to(w, "rejected as data decoded as code: {}\n", rejected_.size());
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
            j.kv("entries", t.entries.size());
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
