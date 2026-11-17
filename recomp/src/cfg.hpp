// Function discovery and recursive-descent control-flow recovery.
//
// C++ port of the analysis prototype (tools/analysis/cfg.py + analyze.py),
// working on Zydis's structured operands instead of disassembly text.
//
// Pipeline:
//   1. Linear sweep of every code section -> seed function starts
//      (entry, direct call targets, code after ret/jmp + padding, aligned code
//      pointers stored in .rdata/.data).
//   2. Recursive descent per function, bounded by the next candidate start,
//      resolving switch jump tables (cmp-bounded, cmp + byte index table, or
//      heuristic).
//   3. Extra seeding rounds: clean data pointers, orphans, immediates that
//      point at code, and 16-aligned starts in remaining gaps.
//   4. Prune seeds whose entry falls inside another function's instruction.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "decoder.hpp"
#include "xbe.hpp"

namespace qilin {

struct JumpTable {
    uint32_t at = 0;     // the jmp [4*reg + table]
    uint32_t table = 0;  // table address
    std::string how;     // "cmp", "cmp+bytetable", "heuristic"
    std::vector<uint32_t> targets;
};

struct IndirectSite {
    uint32_t at = 0;
    std::string text;
};

struct Function {
    uint32_t start = 0;
    std::string tag;                 // which discovery round found it
    std::vector<uint32_t> insns;     // sorted instruction addresses
    std::set<uint32_t> leaders;      // basic-block starts
    std::set<uint32_t> calls;        // direct call targets
    std::set<uint32_t> tails;        // tail-jump targets (other functions)
    std::vector<IndirectSite> icalls;
    std::vector<IndirectSite> ijmps; // unresolved indirect jumps
    std::vector<JumpTable> jtabs;
    std::vector<uint32_t> bad;       // addresses that failed to decode / left the section
    std::vector<uint32_t> overlap;   // instructions overlapping an earlier one
    uint32_t bytes = 0;
    uint32_t lo = 0, hi = 0;
};

struct CfgConfig {
    // Range in which 16-byte-aligned pointers in .rdata/.data are treated as
    // candidate code pointers (game code + Rare libs for Ghoulies).
    uint32_t dataptr_lo = 0x11000;
    uint32_t dataptr_hi = 0x165C00;
    std::string gap_section = ".text";
};

struct Gap {
    uint32_t addr = 0;
    uint32_t size = 0;
};

class Cfg {
public:
    Cfg(const Xbe& xbe, Decoder& dec, CfgConfig cfg = {});
    void run();

    const std::map<uint32_t, Function>& functions() const { return funcs_; }
    const std::map<std::string, std::vector<Gap>>& gaps() const { return gaps_; }
    std::string summary() const;
    std::string to_json() const;

    // All reachable instruction addresses, sorted.
    std::vector<uint32_t> all_instructions() const;

private:
    struct Explored;
    Explored explore(uint32_t f, uint32_t bound);
    void resolve_jt(const Insn& jmp, ZydisRegister reg, uint32_t table, const std::vector<const Insn*>& hist,
                    const XbeSection& sec, JumpTable& out);
    void linear_seeds();
    void run_queue(std::vector<uint32_t> q, const std::string& tag);
    bool clean_seed(uint32_t v);
    uint32_t bound_of(uint32_t f) const;
    void add_cand(uint32_t v) { cand_.insert(v); }
    void compute_gaps();
    void prune_misaligned();

    const Xbe& xbe_;
    Decoder& dec_;
    CfgConfig cfg_;

    std::vector<uint32_t> strong_, weak_;
    std::set<uint32_t> known_;
    std::set<uint32_t> cand_;
    std::map<uint32_t, Function> funcs_;
    std::set<uint32_t> owned_;
    std::set<uint32_t> imm_seeds_;
    std::map<std::string, std::vector<Gap>> gaps_;
    std::vector<uint32_t> pruned_;     // dropped: entry inside another function's instruction
    std::vector<uint32_t> conflicts_;  // call targets inside another function's instruction
};

}  // namespace qilin
