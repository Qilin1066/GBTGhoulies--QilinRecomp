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
#include <map>
#include <set>
#include <string>

#include "cfg.hpp"
#include "decoder.hpp"

namespace qilin {

struct LiftFailure {
    enum class Kind : uint8_t {
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

// Hand-written assembly helpers (the CRT's x87 maths and SEH frames) that
// break the usual function contract, listed by address. Each entry is a
// reviewed exception, never inferred.
struct Conventions {
    // Flags passed in to / out of a function, as a mask of the lifter's flag
    // bits (CF 1, PF 2, AF 4, ZF 8, SF 16, OF 32). They travel in c.eflags,
    // at their x86 bit positions.
    std::map<uint32_t, uint8_t> flags_in, flags_out;
    // Frame helpers and shared epilogues that pop past their own return
    // address or return with esp moved (__SEH_prolog, `pop edx; ret` tails):
    // the stack-balance check is skipped. The generated code follows esp at
    // run time either way; callers still check where they're returned to.
    std::set<uint32_t> stack_glue;
};

class Lifter {
public:
    explicit Lifter(Decoder& dec, const Conventions* conv = nullptr) : dec_(dec), conv_(conv) {}

    // Lifts one function to a C++ definition (no x87 yet: slice 3).
    //
    // Calls (slice 2): a direct `call T` pushes the guest return address,
    // stores the modified registers to the Context, calls fn_T, reloads the
    // registers and checks the callee returned to the call site. A jump out
    // of the function is a tail call. Indirect calls and jumps go through
    // rt::call (the generated function table, then rt::call_unlifted).
    // Every fn_T a function calls must be defined: lifted, or a stub that
    // forwards to rt::call_unlifted (qilin lift generates those).
    std::expected<std::string, LiftFailure> lift(const Function& f);

    static std::string name(uint32_t addr);

private:
    Decoder& dec_;
    const Conventions* conv_;
};

}  // namespace qilin
