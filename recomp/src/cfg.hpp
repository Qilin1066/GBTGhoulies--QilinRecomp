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
//   4. Validation: drop seeds that are data decoded as code (entry inside
//      another instruction, decode failures, implausible instructions).
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
    uint32_t at = 0;                // the jmp [4*reg + table]
    uint32_t table = 0;             // table address
    std::string how;                // "cmp", "cmp+bytetable", "heuristic"
    std::vector<uint32_t> targets;  // unique, sorted
    std::vector<uint32_t> entries;  // table contents in index order (0 = not a code address)
    bool bounded = false;           // index range proven by a cmp
    uint32_t index_table = 0;       // byte index table (movzx reg, byte [idx + T]), 0 if none
    uint32_t index_count = 0;       // its length in bytes
};

struct IndirectSite {
    uint32_t at = 0;
    std::string text;
};

// How a function was found, strongest evidence first.
enum class Origin : uint8_t { Reachable, DataPtr, Orphan, ImmPtr, Gap };

struct Function {
    uint32_t start = 0;
    Origin origin = Origin::Reachable;
    uint8_t round = 0;               // seeding round within DataPtr / Gap
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

    std::string tag() const;         // "reachable", "dataptr0", "orphan", "imm-ptr", "gap0", ...
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

struct SectionGaps {
    const XbeSection* section = nullptr;
    std::vector<Gap> gaps;
};

struct Suspect {
    uint32_t start = 0;
    std::string why;
};

// Set of addresses inside the code sections, as a bitmap (O(1), 1 bit/byte).
class AddrSet {
public:
    explicit AddrSet(const Xbe& xbe);
    bool contains(uint32_t a) const { return a >= lo_ && a < hi_ && bits_[a - lo_]; }
    void insert(uint32_t a);
    void clear();
    size_t size() const { return count_; }
    std::vector<uint32_t> to_vector() const;

private:
    uint32_t lo_ = 0, hi_ = 0;
    std::vector<bool> bits_;
    size_t count_ = 0;
};

class Cfg {
public:
    Cfg(const Xbe& xbe, Decoder& dec, CfgConfig cfg = {});
    void run();

    const std::map<uint32_t, Function>& functions() const { return funcs_; }
    const std::vector<SectionGaps>& gaps() const { return gaps_; }
    const std::vector<Suspect>& suspects() const { return suspects_; }
    const std::vector<uint32_t>& conflicts() const { return conflicts_; }
    std::string summary() const;
    std::string to_json() const;

    // All reachable instruction addresses, sorted.
    std::vector<uint32_t> all_instructions() const { return owned_.to_vector(); }

private:
    struct Explored;
    Explored explore(uint32_t f, uint32_t bound);
    void resolve_jt(const std::vector<const Insn*>& hist, const XbeSection& sec, JumpTable& out);
    void linear_seeds();
    void run_queue(std::vector<uint32_t> q, Origin origin, uint8_t round = 0);
    bool clean_seed(uint32_t v);
    uint32_t bound_of(uint32_t f) const;
    void rebuild_owned();
    void validate();
    void prune_misaligned();
    void compute_gaps();

    const Xbe& xbe_;
    Decoder& dec_;
    CfgConfig cfg_;

    std::vector<uint32_t> strong_, weak_;
    std::set<uint32_t> known_;
    std::set<uint32_t> cand_;
    std::map<uint32_t, Function> funcs_;
    AddrSet owned_;
    std::set<uint32_t> imm_seeds_;
    std::vector<SectionGaps> gaps_;
    std::vector<uint32_t> pruned_;     // entry inside another function's instruction
    std::vector<uint32_t> conflicts_;  // call targets inside another function's instruction
    std::vector<uint32_t> rejected_;   // dropped by validate()
    std::vector<Suspect> suspects_;    // kept despite problems (call targets)
};

}  // namespace qilin
