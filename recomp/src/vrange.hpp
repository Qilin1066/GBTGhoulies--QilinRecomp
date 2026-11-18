// Value-range analysis over one function's instructions, used to prove the
// size of jump tables whose bound isn't a `cmp`/`and` directly above the jump.
//
// Each of the eight 32-bit registers holds an unsigned interval [lo, hi].
// The analysis runs forward over the instruction graph (fallthrough, direct
// branches, jump-table cases) and narrows intervals on conditional edges from
// the last `cmp`/`test`. Joins take the hull; a node revisited too often is
// widened to "anything". It is sound for what it tracks: everything it can't
// model (memory, calls, partial-register writes) makes the register unknown.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "decoder.hpp"
#include "xbe.hpp"

namespace qilin {

struct JumpTable;

struct Range {
    uint32_t lo = 0, hi = UINT32_MAX;
    static Range top() { return {}; }
    static Range point(uint32_t v) { return {v, v}; }
    bool is_top() const { return lo == 0 && hi == UINT32_MAX; }
    bool operator==(const Range&) const = default;
};

// The index register's range at a table jump, and the byte index table it
// was loaded from, if any.
struct IndexFact {
    Range index;
    uint32_t byte_table = 0;  // first byte of the index table read (0 if none)
    uint32_t byte_count = 0;  // bytes of it that can be read
    // The jump is `neg r; jmp [4*r + T]` (memmove's tail table): index is r's
    // range *before* the neg, and the table is indexed by its negation.
    bool negated = false;
};

// insns: the function's instructions; entries: where execution can start
// (the function entry, EH continuations); jtabs: resolved tables, whose cases
// become edges (bounded ones also narrow the index per case); want: the table
// jumps to report on.
std::map<uint32_t, IndexFact> index_ranges(const Xbe& xbe, const std::map<uint32_t, const Insn*>& insns,
                                           std::span<const uint32_t> entries, std::span<const JumpTable> jtabs,
                                           std::span<const uint32_t> want);

}  // namespace qilin
