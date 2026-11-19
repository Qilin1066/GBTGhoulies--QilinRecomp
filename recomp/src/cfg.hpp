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

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "decoder.hpp"
#include "regions.hpp"
#include "xbe.hpp"

namespace qilin {

struct IndexFact;  // vrange.hpp

struct JumpTable {
    uint32_t at = 0;                // the jmp [4*reg + table]
    uint32_t table = 0;             // table address
    std::string how;                // "cmp", "and", "range" (value-range analysis), "+bytetable", or "heuristic"
    std::vector<uint32_t> targets;  // unique, sorted
    std::vector<uint32_t> entries;  // table contents in index order (0 = not a code address)
    bool bounded = false;           // index range proven by a cmp
    uint32_t index_table = 0;       // byte index table (movzx reg, byte [idx + T]), 0 if none
    uint32_t index_count = 0;       // its length in bytes
    // entries[k] is the slot for index value first + k, at table + 4 * (first + k).
    // first is the lowest possible index: 0 for a cmp/and bound, possibly
    // negative for MSVC's `sub ecx, 4; jb; jmp [4*ecx + T]` trick.
    int32_t first = 0;
    uint32_t data_lo() const { return table + 4 * uint32_t(first); }
    uint32_t data_hi() const { return data_lo() + 4 * std::max<uint32_t>(1, uint32_t(entries.size())); }
    std::vector<uint32_t> dropped;  // targets beyond the function's bound (candidates for absorb_fragments)
};

struct IndirectSite {
    uint32_t at = 0;
    std::string text;
};

// How a function was found, strongest evidence first.
enum class Origin : uint8_t { Reachable, DataPtr, Orphan, ImmPtr, Gap, Eh };

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

    std::string tag() const;         // "reachable", "dataptr0", "orphan", "imm-ptr", "gap0", "eh", ...
};

struct CfgConfig {
    // Range in which 16-byte-aligned pointers in .rdata/.data are treated as
    // candidate code pointers (game code + Rare libs for Ghoulies).
    uint32_t dataptr_lo = 0x11000;
    uint32_t dataptr_hi = 0x165C00;
    // A library's own data section holding tables of its functions: Bink's
    // blitter tables in BINKDATA point into the BINK… sections.
    std::string lib_data_section = "BINKDATA";
    uint32_t lib_dataptr_lo = 0x1A59E0, lib_dataptr_hi = 0x1BABA0;
    std::string gap_section = ".text";
    // Regions where every function entry is aligned (MSVC /Gy + the game's
    // build settings): Ghoulies game code and Rare libs, 16 bytes. Verified:
    // all 4,041 directly-called functions there are 16-byte aligned. A
    // misaligned entry in these regions needs a direct call to count.
    std::vector<std::pair<uint32_t, uint32_t>> aligned_regions = {{0x11000, 0x14A840}, {0x152780, 0x165C00}};
    uint32_t alignment = 16;
    // Regions of .text for the coverage report (regions.hpp); other code
    // sections are reported per section, as replaced libraries.
    using Region = TextRegion;
    std::vector<Region> text_regions = kTextRegions;
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

// part / whole as a percentage with `decimals` places, in integer arithmetic,
// so reports are identical on every host.
std::string percent(uint64_t part, uint64_t whole, int decimals);

// MSVC alignment filler (mov r, r / lea r, [r + 0]); semantically a no-op.
bool is_pad_filler(const Insn& i);
// Does i (possibly) write 32-bit register r or part of it? Calls count as writing eax/ecx/edx.
bool writes_reg(const Insn& i, ZydisRegister r);

// Byte-level accounting of a code range: every byte is code, a known table,
// padding, or unknown. "Done" means unknown == 0 in every lifted region.
struct CoverageRegion {
    std::string name;
    uint32_t lo = 0, hi = 0;
    bool lifted = false;               // recompiled (vs replaced by native code)
    uint64_t code = 0, table = 0, padding = 0, unknown = 0;
    std::vector<Gap> unknown_spans{};
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
    // Per-region byte accounting (see CoverageRegion).
    std::vector<CoverageRegion> coverage();

    // All reachable instruction addresses, sorted.
    std::vector<uint32_t> all_instructions() const { return owned_.to_vector(); }

    // A secondary entry: code inside a known function that other code jumps
    // to (a shared exit such as `call __SEH_epilog; ret`), explored from
    // there to the containing function's end. Empty if `a` isn't an
    // instruction of some function.
    std::optional<Function> entry_inside(uint32_t a);

private:
    struct Explored;
    Explored explore(uint32_t f, uint32_t bound);
    Explored explore_once(uint32_t f, uint32_t bound, const std::map<uint32_t, JumpTable>& proven);
    bool prove_table(const IndexFact& fact, const XbeSection& sec, JumpTable& t) const;
    void resolve_jt(const std::vector<const Insn*>& hist, const XbeSection& sec, JumpTable& out);
    void linear_seeds();
    void run_queue(std::vector<uint32_t> q, Origin origin, uint8_t round = 0);
    bool clean_seed(uint32_t v);
    bool displaces_weak_seed(uint32_t v);
    bool take_over(uint32_t v, Origin origin);
    bool jumps_over_filler(const Function& g, uint32_t c) const;
    bool falls_into(const Function& g, uint32_t f) const;
    bool drop_labels();
    uint32_t bound_of(uint32_t f) const;
    void rebuild_owned();
    void validate();
    bool absorb_fragments();
    bool imm_round();
    bool reexplore_all();
    void scan_eh_tables();
    void scan_seh_tables();
    Function make_function(uint32_t f, Origin origin, uint8_t round, struct Explored& r);
    bool plausible(const Function& f) const;
    // Can `a` be a function entry on evidence other than a direct call?
    bool entry_ok(uint32_t a);
    bool in_aligned_region(uint32_t a) const;
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
    std::set<uint32_t> dptr_;
    std::set<uint32_t> unprovable_;    // table jumps the range analysis can't bound (not retried)          // 16-aligned code pointers found in .rdata/.data
    std::set<uint32_t> evidence_;      // addresses with independent evidence of being a function entry
    size_t absorbed_ = 0;              // fragments merged back into their parent
    // MSVC C++ exception metadata (FuncInfo in .rdata): unwind funclets are
    // functions of their own; catch handlers and the continuations they return
    // are extra entry points *inside* their parent function, reached only via
    // the C++ runtime. Explored as part of whichever function contains them.
    std::set<uint32_t> eh_unwind_;
    size_t seh_tables_ = 0, seh_excepts_ = 0;
    std::set<uint32_t> eh_roots_;
    size_t eh_funcinfos_ = 0;
    size_t eh_stubs_ = 0;
    std::vector<SectionGaps> gaps_;
    std::vector<uint32_t> pruned_;     // entry inside another function's instruction
    std::vector<uint32_t> conflicts_;  // call targets inside another function's instruction
    std::vector<uint32_t> rejected_;   // dropped by validate()
    std::vector<Suspect> suspects_;    // kept despite problems (call targets)
};

}  // namespace qilin
