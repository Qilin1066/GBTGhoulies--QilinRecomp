#include "cfg.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "json.hpp"

namespace qilin {

namespace {

bool is_gpr32(ZydisRegister r) { return r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI; }

// al/bl/cl/dl -> eax/ebx/ecx/edx; 32-bit registers map to themselves.
ZydisRegister widen_low8(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_AL: return ZYDIS_REGISTER_EAX;
        case ZYDIS_REGISTER_BL: return ZYDIS_REGISTER_EBX;
        case ZYDIS_REGISTER_CL: return ZYDIS_REGISTER_ECX;
        case ZYDIS_REGISTER_DL: return ZYDIS_REGISTER_EDX;
        default: return r;
    }
}

bool is_simple_mem(const Operand& o) {
    return o.kind == Operand::Kind::Mem && (o.seg == ZYDIS_REGISTER_DS || o.seg == ZYDIS_REGISTER_NONE);
}

// `xx byte ptr [eax], al` -- what a run of zero bytes decodes to. A function
// that contains it is almost certainly data being decoded as code.
bool is_zero_bytes_insn(const Insn& i) {
    return i.nops >= 2 && i.op[0].kind == Operand::Kind::Mem && i.op[0].size == 8 &&
           i.op[0].base == ZYDIS_REGISTER_EAX && i.op[0].index == ZYDIS_REGISTER_NONE && i.op[0].disp == 0 &&
           i.op[1].kind == Operand::Kind::Reg && i.op[1].reg == ZYDIS_REGISTER_AL;
}

// MSVC alignment filler that is not a NOP mnemonic.
bool is_pad_filler(const Insn& i) {
    if (i.nops != 2) return false;
    if (i.mnem == ZYDIS_MNEMONIC_MOV)
        return i.op[0].kind == Operand::Kind::Reg && i.op[1].kind == Operand::Kind::Reg &&
               i.op[0].reg == ZYDIS_REGISTER_EDI && i.op[1].reg == ZYDIS_REGISTER_EDI;
    if (i.mnem == ZYDIS_MNEMONIC_LEA) {
        auto r = i.op[0].reg;
        if (r != ZYDIS_REGISTER_ECX && r != ZYDIS_REGISTER_ESP && r != ZYDIS_REGISTER_EBX) return false;
        return i.op[1].kind == Operand::Kind::Mem && i.op[1].base == r && i.op[1].index == ZYDIS_REGISTER_NONE &&
               i.op[1].disp == 0;
    }
    return false;
}

// Address-like constants an instruction mentions (immediates and absolute
// non-indexed displacements >= 0x10000). Relative branch offsets are excluded.
template <class F> void for_each_addr_constant(const Insn& i, F&& f) {
    for (uint8_t k = 0; k < i.nops; ++k) {
        const auto& o = i.op[k];
        uint64_t v;
        if (o.kind == Operand::Kind::Imm && !o.imm_relative) {
            v = o.imm & 0xFFFFFFFFu;
        } else if (o.kind == Operand::Kind::Mem && o.has_disp && o.disp > 0 && o.index == ZYDIS_REGISTER_NONE) {
            // Indexed accesses ([4*reg + table]) address data arrays, never code entry points.
            v = uint64_t(o.disp) & 0xFFFFFFFFu;
        } else {
            continue;
        }
        if (v >= 0x10000) f(uint32_t(v));
    }
}

}  // namespace

struct Cfg::Explored {
    std::map<uint32_t, const Insn*> insns;
    std::set<uint32_t> leaders, calls, tails;
    std::vector<IndirectSite> icalls, ijmps;
    std::vector<JumpTable> jtabs;
    std::vector<uint32_t> bad, overlap;
    std::set<uint32_t> imms;
};

Cfg::Cfg(const Xbe& xbe, Decoder& dec, CfgConfig cfg) : xbe_(xbe), dec_(dec), cfg_(std::move(cfg)) {}

uint32_t Cfg::bound_of(uint32_t f) const {
    const XbeSection* s = xbe_.code_section_at(f);
    uint32_t send = s->va + s->rsize;
    auto it = cand_.upper_bound(f);
    if (it == cand_.end()) return send;
    return std::min(*it, send);
}

void Cfg::resolve_jt(const Insn& jmp, ZydisRegister reg, uint32_t table, const std::vector<const Insn*>& hist,
                     const XbeSection& sec, JumpTable& out) {
    (void)jmp;
    std::optional<uint32_t> count;
    out.how = "heuristic";
    std::optional<uint32_t> btable;
    ZydisRegister idx_reg = reg;
    // Walk back (excluding the jmp itself) for: [movzx reg, byte [r2 + T2]] ... cmp idx, N
    for (auto it = hist.rbegin() + 1; it != hist.rend(); ++it) {
        const Insn& i = **it;
        if (i.mnem == ZYDIS_MNEMONIC_MOVZX && !btable && i.nops == 2 && i.op[0].kind == Operand::Kind::Reg &&
            i.op[0].reg == reg && is_simple_mem(i.op[1]) && i.op[1].size == 8 && is_gpr32(i.op[1].base) &&
            i.op[1].index == ZYDIS_REGISTER_NONE && i.op[1].disp > 0) {
            btable = uint32_t(i.op[1].disp);
            idx_reg = i.op[1].base;
            continue;
        }
        if (i.mnem == ZYDIS_MNEMONIC_CMP && i.nops == 2 && i.op[0].kind == Operand::Kind::Reg &&
            i.op[1].kind == Operand::Kind::Imm) {
            ZydisRegister r = widen_low8(i.op[0].reg);
            if ((is_gpr32(i.op[0].reg) || r != i.op[0].reg) && r == idx_reg) {
                uint64_t n = (i.op[1].imm & 0xFFFFFFFFu) + 1;
                if (n > 4096) break;  // not a plausible switch bound
                if (btable) {
                    uint32_t mx = 0;
                    for (uint32_t k = 0; k < n; ++k)
                        if (xbe_.mapped(*btable + k)) mx = std::max<uint32_t>(mx, xbe_.u8(*btable + k));
                    count = mx + 1;
                    out.how = "cmp+bytetable";
                } else {
                    count = uint32_t(n);
                    out.how = "cmp";
                }
                break;
            }
        }
    }
    uint32_t lo = sec.va, hi = sec.va + sec.rsize;
    uint32_t limit = count ? *count : 512;
    out.bounded = count.has_value();
    std::set<uint32_t> tg;
    for (uint32_t i = 0; i < limit; ++i) {
        uint32_t a = table + 4 * i;
        if (!xbe_.mapped(a, 4)) break;
        uint32_t v = xbe_.u32(a);
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
    while (!work.empty()) {
        uint32_t a = work.back();
        work.pop_back();
        hist.clear();
        for (;;) {
            if (r.insns.count(a)) break;
            if (xbe_.code_section_at(a) != sec_f) { r.bad.push_back(a); break; }
            const Insn* ins = dec_.decode(a);
            if (!ins) { r.bad.push_back(a); break; }
            // Overlap: does a fall inside the nearest earlier decoded instruction?
            if (auto it = r.insns.lower_bound(a); it != r.insns.begin()) {
                --it;
                if (a - it->first < 15 && it->second->end() > a) r.overlap.push_back(a);
            }
            r.insns.emplace(a, ins);
            hist.push_back(ins);
            if (hist.size() > 12) hist.erase(hist.begin());
            for_each_addr_constant(*ins, [&](uint32_t v) { r.imms.insert(v); });

            if (ins->is_ret()) break;
            if (ins->mnem == ZYDIS_MNEMONIC_INT3 && a != f) break;
            if (ins->is_jmp()) {
                if (auto t = ins->rel_target()) {
                    bool other = (known_.count(*t) && *t != f) || (bound && !(*t >= f && *t < bound));
                    if (other) {
                        r.tails.insert(*t);
                    } else {
                        r.leaders.insert(*t);
                        work.push_back(*t);
                    }
                } else {
                    const Operand& m = ins->op[0];
                    bool jt = ins->nops >= 1 && is_simple_mem(m) && m.size == 32 && m.base == ZYDIS_REGISTER_NONE &&
                              is_gpr32(m.index) && m.scale == 4 && m.has_disp && m.disp > 0;
                    if (jt) {
                        JumpTable j;
                        j.at = a;
                        j.table = uint32_t(m.disp);
                        resolve_jt(*ins, m.index, j.table, hist, *sec_f, j);
                        if (bound) {
                            auto outside = [&](uint32_t x) { return !(x >= f && x < bound); };
                            std::erase_if(j.targets, outside);
                            for (auto& e : j.entries)
                                if (e && outside(e)) e = 0;
                        }
                        for (uint32_t jt : j.targets) {
                            r.leaders.insert(jt);
                            work.push_back(jt);
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
                    if (bound && !(*t >= f && *t < bound)) {
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
                if (auto t = ins->rel_target()) {
                    r.calls.insert(*t);
                } else {
                    r.icalls.push_back({a, dec_.format(*ins)});
                }
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
    struct L { uint32_t addr; const Insn* ins; };
    std::vector<L> lin;
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        uint32_t a = s.va, end = s.va + s.rsize;
        while (a < end) {
            const Insn* i = dec_.decode(a);
            if (i && i->end() <= end) {
                lin.push_back({a, i});
                a = i->end();
            } else {
                lin.push_back({a, nullptr});
                a += 1;
            }
        }
    }
    std::unordered_set<uint32_t> in_code;
    std::unordered_map<uint32_t, size_t> idx;
    in_code.reserve(lin.size());
    for (size_t k = 0; k < lin.size(); ++k)
        if (lin[k].ins) { in_code.insert(lin[k].addr); idx[lin[k].addr] = k; }

    std::set<uint32_t> strong, weak_only;
    std::map<uint32_t, std::set<std::string>> reasons;
    reasons[xbe_.entry].insert("entry");
    for (auto& l : lin) {
        if (l.ins && l.ins->is_call())
            if (auto t = l.ins->rel_target(); t && in_code.count(*t)) reasons[*t].insert("call");
    }
    auto is_pad = [](const L& l) { return l.ins && l.ins->is_pad(); };
    for (size_t k = 1; k < lin.size(); ++k) {
        if (!lin[k].ins || lin[k].ins->is_pad()) continue;
        size_t j = k - 1, npad = 0;
        while (is_pad(lin[j])) {
            ++npad;
            if (j == 0) break;
            --j;
        }
        if (npad && lin[j].ins && !lin[j].ins->is_pad() && (lin[j].ins->is_ret() || lin[j].ins->is_jmp()))
            reasons[lin[k].addr].insert("pad");
    }
    for (auto& s : xbe_.sections) {
        if (s.name != ".rdata" && s.name != ".data") continue;
        for (uint32_t k = 0; k + 3 < s.rsize; k += 4) {
            uint32_t v = xbe_.u32(s.va + k);
            auto it = idx.find(v);
            if (it == idx.end()) continue;
            size_t i = it->second;
            bool prev_ok = i == 0 || is_pad(lin[i - 1]) || (lin[i - 1].ins && lin[i - 1].ins->is_ret());
            if (reasons.count(v) || prev_ok) reasons[v].insert("dataptr");
        }
    }
    for (auto& [a, rs] : reasons) {
        if (rs.count("call") || rs.count("dataptr") || rs.count("entry")) strong_.push_back(a);
        else if (rs.size() == 1 && rs.count("pad")) weak_.push_back(a);
    }
}

void Cfg::run_queue(std::vector<uint32_t> q, const std::string& tag) {
    while (!q.empty()) {
        uint32_t f = q.back();
        q.pop_back();
        if (funcs_.count(f) || !xbe_.code_section_at(f)) continue;
        add_cand(f);
        Explored r = explore(f, bound_of(f));
        Function fn;
        fn.start = f;
        fn.tag = tag;
        fn.insns.reserve(r.insns.size());
        for (auto& [a, i] : r.insns) {
            fn.insns.push_back(a);
            fn.bytes += i->len;
            owned_.insert(a);
        }
        fn.lo = r.insns.empty() ? f : r.insns.begin()->first;
        fn.hi = f;
        for (auto& [a, i] : r.insns) fn.hi = std::max(fn.hi, i->end());
        fn.leaders = std::move(r.leaders);
        fn.calls = r.calls;
        fn.tails = r.tails;
        fn.icalls = std::move(r.icalls);
        fn.ijmps = std::move(r.ijmps);
        fn.jtabs = std::move(r.jtabs);
        fn.bad = std::move(r.bad);
        fn.overlap = std::move(r.overlap);
        funcs_.emplace(f, std::move(fn));
        for (const auto* set : {&r.calls, &r.tails})
            for (uint32_t c : *set)
                if (!funcs_.count(c) && xbe_.code_section_at(c)) {
                    known_.insert(c);
                    add_cand(c);
                    q.push_back(c);
                }
        for (uint32_t v : r.imms) {
            if (known_.count(v) && !funcs_.count(v)) q.push_back(v);
            else if (xbe_.code_section_at(v) && !funcs_.count(v) && !owned_.count(v)) imm_seeds_.insert(v);
        }
    }
}

bool Cfg::clean_seed(uint32_t v) {
    if (funcs_.count(v) || owned_.count(v)) return false;
    Explored r = explore(v, bound_of(v));
    if (!r.bad.empty() || !r.overlap.empty() || r.insns.size() < 3) return false;
    for (auto& [a, i] : r.insns)
        if (owned_.count(a) || is_zero_bytes_insn(*i)) return false;
    return true;
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
            uint32_t v = xbe_.u32(s.va + k);
            if (v >= cfg_.dataptr_lo && v < cfg_.dataptr_hi && v % 16 == 0 && dec_.decode(v)) dptr.insert(v);
        }
    }

    {
        std::vector<uint32_t> q(strong_.rbegin(), strong_.rend());
        q.push_back(xbe_.entry);
        run_queue(std::move(q), "reachable");
    }
    for (int it = 0; it < 4; ++it) {
        std::vector<uint32_t> acc;
        for (uint32_t v : dptr)
            if (clean_seed(v)) acc.push_back(v);
        if (acc.empty()) break;
        for (uint32_t v : acc) { known_.insert(v); add_cand(v); }
        run_queue(std::vector<uint32_t>(acc.rbegin(), acc.rend()), "dataptr" + std::to_string(it));
    }
    {
        std::vector<uint32_t> q;
        for (auto it = weak_.rbegin(); it != weak_.rend(); ++it)
            if (!owned_.count(*it)) q.push_back(*it);
        run_queue(std::move(q), "orphan");
    }
    {
        std::vector<uint32_t> ok;
        for (uint32_t v : imm_seeds_) {
            if (owned_.count(v) || funcs_.count(v)) continue;
            if (!xbe_.mapped(v - 1)) continue;
            uint8_t prev = xbe_.u8(v - 1);
            if ((prev == 0xCC || prev == 0x90 || prev == 0xC3) && dec_.decode(v)) ok.push_back(v);
        }
        run_queue(std::vector<uint32_t>(ok.rbegin(), ok.rend()), "imm-ptr");
    }

    // Gap seeding: a 16-aligned decodable start after padding, in remaining gaps.
    auto jt_ranges = [&] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (auto& [_, fn] : funcs_)
            for (auto& j : fn.jtabs)
                v.emplace_back(j.table, j.table + 4 * std::max<uint32_t>(1, uint32_t(j.targets.size())));
        std::sort(v.begin(), v.end());
        return v;
    };
    auto ranges = jt_ranges();
    for (int it = 0; it < 6; ++it) {
        std::vector<uint32_t> fresh;
        for (auto& s : xbe_.sections) {
            if (!s.code) continue;
            uint32_t a = s.va, hi = s.va + s.rsize;
            while (a < hi) {
                if (owned_.count(a)) { a = dec_.decode(a)->end(); continue; }
                const Insn* ins = dec_.decode(a);
                if (!ins) { a += 1; continue; }
                if (ins->is_pad() || is_pad_filler(*ins)) { a = ins->end(); continue; }
                std::optional<uint32_t> jr;
                for (auto& [s0, e] : ranges) {
                    if (s0 > a) break;
                    if (a < e) { jr = e; break; }
                }
                if (jr) { a = *jr; continue; }
                if (s.name == cfg_.gap_section && a % 16 == 0 && xbe_.mapped(a - 1)) {
                    uint8_t prevb = xbe_.u8(a - 1);
                    if (prevb == 0xCC || prevb == 0x90 || prevb == 0xC3) {
                        Explored r = explore(a, bound_of(a));
                        bool clean = r.bad.empty() && r.overlap.empty() && r.insns.size() >= 3;
                        bool has_exit = false;
                        for (auto& [ia, i] : r.insns) {
                            if (owned_.count(ia) || is_zero_bytes_insn(*i)) clean = false;
                            if (i->is_ret() || i->is_jmp()) has_exit = true;
                        }
                        if (clean && has_exit) fresh.push_back(a);
                    }
                }
                a = (a + 4) & ~3u;
            }
        }
        std::erase_if(fresh, [&](uint32_t n) { return funcs_.count(n) != 0; });
        if (fresh.empty()) break;
        for (uint32_t n : fresh) add_cand(n);
        run_queue(std::vector<uint32_t>(fresh.rbegin(), fresh.rend()), "gap" + std::to_string(it));
        ranges = jt_ranges();
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
                        char b[96];
                        std::snprintf(b, sizeof b, "%s at %08X", r, a);
                        why = b;
                        break;
                    }
            if (why.empty()) continue;
            if (targets.count(start)) suspects_.push_back({start, why});
            else drop.push_back(start);
        }
        if (drop.empty()) break;
        for (uint32_t d : drop) {
            funcs_.erase(d);
            rejected_.push_back(d);
        }
        owned_.clear();
        for (auto& [_, fn] : funcs_) owned_.insert(fn.insns.begin(), fn.insns.end());
    }
    std::sort(rejected_.begin(), rejected_.end());
}

// A function whose entry lies strictly inside an instruction of another
// function means one of the two is data decoded as code. Drop the one with
// the weaker evidence: a seed that only looked like a code pointer, or else a
// host function that was itself only an orphan/heuristic seed. If both are
// reachable, record a conflict for review. Repeat until stable.
void Cfg::prune_misaligned() {
    for (;;) {
        struct Owner { uint32_t end, func; };
        std::map<uint32_t, Owner> ends;  // insn start -> end + owning function
        for (auto& [start, fn] : funcs_)
            for (uint32_t a : fn.insns) ends.emplace(a, Owner{dec_.decode(a)->end(), start});
        std::set<uint32_t> drop;
        conflicts_.clear();
        for (auto& [start, fn] : funcs_) {
            auto it = ends.lower_bound(start);
            if (it == ends.begin()) continue;
            --it;
            if (!(it->first < start && it->second.end > start)) continue;
            const Function& host = funcs_.at(it->second.func);
            if (fn.tag != "reachable") drop.insert(start);           // the seed is the misaligned one
            else if (host.tag != "reachable") drop.insert(host.start); // the host is data decoded as code
            else conflicts_.push_back(start);
        }
        if (drop.empty()) break;
        for (uint32_t d : drop) {
            funcs_.erase(d);
            pruned_.push_back(d);
        }
        owned_.clear();
        for (auto& [_, fn] : funcs_) owned_.insert(fn.insns.begin(), fn.insns.end());
    }
    std::sort(pruned_.begin(), pruned_.end());
}

void Cfg::compute_gaps() {
    gaps_.clear();
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        uint32_t a = s.va, hi = s.va + s.rsize;
        while (a < hi) {
            if (owned_.count(a)) { a = dec_.decode(a)->end(); continue; }
            uint32_t g = a;
            bool nonpad = false;
            while (a < hi && !owned_.count(a)) {
                uint8_t b = xbe_.u8(a);
                if (b != 0xCC && b != 0x90 && b != 0x00) nonpad = true;
                ++a;
            }
            if (nonpad) gaps_[s.name].push_back({g, a - g});
        }
    }
}

std::vector<uint32_t> Cfg::all_instructions() const { return {owned_.begin(), owned_.end()}; }

std::string Cfg::summary() const {
    std::string out;
    char b[512];
    std::map<std::string, int> tags;
    for (auto& [_, f] : funcs_) tags[f.tag]++;
    std::snprintf(b, sizeof b, "functions: %zu {", funcs_.size());
    out += b;
    bool first = true;
    for (auto& [t, n] : tags) {
        std::snprintf(b, sizeof b, "%s%s: %d", first ? "" : ", ", t.c_str(), n);
        out += b;
        first = false;
    }
    out += "}\n";
    std::snprintf(b, sizeof b, "instructions: %zu\n", owned_.size());
    out += b;
    out += "coverage by section (non-padding bytes decoded / raw size):\n";
    std::map<std::string, uint64_t> cov;
    for (uint32_t a : owned_) cov[xbe_.code_section_at(a)->name] += dec_.decode(a)->len;
    for (auto& s : xbe_.sections) {
        if (!s.code) continue;
        uint64_t nonpad = 0;
        for (uint32_t a = s.va; a < s.va + s.rsize; ++a) {
            uint8_t v = xbe_.u8(a);
            if (v != 0xCC && v != 0x90) ++nonpad;
        }
        uint64_t gb = 0;
        size_t ng = 0;
        if (auto it = gaps_.find(s.name); it != gaps_.end()) {
            ng = it->second.size();
            for (auto& g : it->second) gb += g.size;
        }
        // Sections can share a name (BINKYUY2 x4); report per section instance.
        uint64_t c = 0;
        for (uint32_t a = s.va; a < s.va + s.rsize;) {
            if (owned_.count(a)) { auto* i = dec_.decode(a); c += i->len; a = i->end(); } else ++a;
        }
        std::snprintf(b, sizeof b, "  %-10s %8llu / %8u  (%5.1f%% of non-pad)  gaps:%5zu gapbytes:%llu\n",
                      s.name.c_str(), (unsigned long long)c, s.rsize, 100.0 * double(c) / double(std::max<uint64_t>(1, nonpad)),
                      ng, (unsigned long long)gb);
        out += b;
    }
    std::map<std::string, int> how;
    size_t njt = 0, ntg = 0, nij = 0, nic = 0, nbad = 0, nov = 0;
    for (auto& [_, f] : funcs_) {
        for (auto& j : f.jtabs) { how[j.how]++; ++njt; ntg += j.targets.size(); }
        nij += f.ijmps.size();
        nic += f.icalls.size();
        nbad += !f.bad.empty();
        nov += !f.overlap.empty();
    }
    std::snprintf(b, sizeof b, "jump tables: %zu (cmp %d, cmp+bytetable %d, heuristic %d) targets: %zu\n", njt,
                  how["cmp"], how["cmp+bytetable"], how["heuristic"], ntg);
    out += b;
    std::snprintf(b, sizeof b, "unresolved indirect jmps: %zu\nindirect calls: %zu\n", nij, nic);
    out += b;
    std::snprintf(b, sizeof b, "functions with bad decode: %zu  with overlapping insns: %zu\n", nbad, nov);
    out += b;
    std::snprintf(b, sizeof b, "pruned misaligned seeds: %zu  call targets inside another function's instruction: %zu\n",
                  pruned_.size(), conflicts_.size());
    out += b;
    std::snprintf(b, sizeof b, "rejected as data decoded as code: %zu\n", rejected_.size());
    out += b;
    std::snprintf(b, sizeof b, "suspect call targets kept (see `qilin suspects`): %zu\n", suspects_.size());
    out += b;
    return out;
}

std::string Cfg::to_json() const {
    JsonWriter j;
    j.begin_object();
    j.key("functions").begin_array();
    for (auto& [a, f] : funcs_) {
        j.begin_object();
        j.kv("start", f.start);
        j.kv("tag", f.tag);
        j.kv("section", xbe_.code_section_at(f.start)->name);
        j.kv("ninsn", uint64_t(f.insns.size()));
        j.kv("bytes", f.bytes);
        j.kv("lo", f.lo);
        j.kv("hi", f.hi);
        j.kv("nblocks", uint64_t(f.leaders.size()));
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
            j.begin_object().kv("at", t.at).kv("table", t.table).kv("n", uint64_t(t.targets.size())).kv("how", t.how);
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
    j.key("gaps").begin_object();
    for (auto& [name, gs] : gaps_) {
        j.key(name).begin_array();
        for (auto& g : gs) j.begin_array().value(g.addr).value(g.size).end_array();
        j.end_array();
    }
    j.end_object();
    j.end_object();
    return j.str();
}

}  // namespace qilin
