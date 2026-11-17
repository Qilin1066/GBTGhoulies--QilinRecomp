// x86 -> C++ lifter.
//
// Each guest function becomes one C++ function `void fn_XXXXXXXX(Context&)`:
//   - guest registers are host locals (loaded at entry; only the ones the
//     function modifies are stored back at its exits), so the host compiler
//     can register-allocate them;
//   - EFLAGS are individual bools, and a flag is only computed where a later
//     instruction can read it (backward liveness over the function's CFG);
//   - basic blocks are labels, branches are gotos, jump tables are switches;
//   - scalar SSE maps to host float ops on lane 0 (bit-exact IEEE single).
//
// Anything not supported is an error naming the instruction. Nothing is
// skipped or approximated silently.
#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "cfg.hpp"
#include "decoder.hpp"

namespace qilin {

struct LiftFailure {
    enum class Kind : uint8_t {
        NotLeaf,           // calls / tail jumps / unresolved indirect jumps (slice 2)
        Fragment,          // not a whole function (unbalanced stack, flags live on entry)
        Unsupported,       // an instruction the lifter doesn't handle yet
        X87,               // x87 (slice 3)
        UndefinedFlag,     // reads a flag the previous writer leaves undefined
        Implausible,       // an instruction retail MSVC code never contains
        BadControlFlow,    // branch outside the function, missing target...
    };
    Kind kind;
    uint32_t addr = 0;      // offending instruction (or the function start)
    std::string insn;       // its text, when there is one
    std::string mnemonic;   // its mnemonic, when there is one
    std::string detail;

    std::string message() const;    // "0001234A mov ...: detail"
    std::string category() const;   // stable bucket for reports, e.g. "unsupported [pushfd]"
};

class Lifter {
public:
    explicit Lifter(Decoder& dec) : dec_(dec) {}

    // Lifts one function to a C++ definition. Slice 1 handles leaf functions
    // (no calls, no tail jumps, no indirect branches other than resolved jump
    // tables) without x87.
    std::expected<std::string, LiftFailure> lift(const Function& f);

    static std::string name(uint32_t addr);

private:
    Decoder& dec_;
};

}  // namespace qilin
