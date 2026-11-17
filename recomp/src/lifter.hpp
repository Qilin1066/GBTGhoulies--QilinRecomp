// x86 -> C++ lifter.
//
// Each guest function becomes one C++ function `void fn_XXXXXXXX(Context&)`:
//   - guest registers are host locals (loaded at entry, stored at every exit),
//     so the host compiler can register-allocate them;
//   - EFLAGS are individual bools, and a flag is only computed where a later
//     instruction can read it (backward liveness over the function's CFG);
//   - basic blocks are labels, branches are gotos, jump tables are switches;
//   - scalar SSE maps to host float ops on lane 0 (bit-exact IEEE single).
//
// Anything not supported is an error naming the instruction. Nothing is
// skipped or approximated silently.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "xbe.hpp"

namespace qilin {

struct LiftResult {
    bool ok = false;
    std::string error;  // first problem found, with address
    std::string code;   // C++ definition when ok
};

class Lifter {
public:
    Lifter(const Xbe& xbe, Decoder& dec, const Cfg& cfg) : xbe_(xbe), dec_(dec), cfg_(cfg) {}

    // Lifts one function. Slice 1 handles leaf functions (no calls, no tail
    // jumps, no indirect branches other than resolved jump tables) without x87.
    LiftResult lift(const Function& f);

    static std::string name(uint32_t addr);

private:
    const Xbe& xbe_;
    Decoder& dec_;
    const Cfg& cfg_;
};

}  // namespace qilin
