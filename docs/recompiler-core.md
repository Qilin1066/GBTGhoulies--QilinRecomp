# Recompiler core: status (roadmap step 2)

Last updated 2026-11-18 (v0.21.0: Bink lifted, MMX arithmetic, `rdtsc`, more function discovery; v0.16.0: D3D's resource code lifted, immediate pointers after `ret n`; v0.15.0: XACT, XGraphics and three DirectSound helpers lifted; 16-bit `imul`, rotates, MMX, `movntps`, device registers; v0.12.0: XAPI lifted, packed SSE; v0.11.0: the C runtime lifted, SEH scope tables, `f2xm1`; v0.9.0: x87 transcendentals, the same bits on every host; v0.8.0: every function in the lifted set lifts; v0.7.0: C++ exception frames; v0.6.0: slice 3, x87; v0.5.0: slice 2, calls; every jump table in lifted code proven by value-range analysis; v0.4.0: every byte of the lifted code accounted for; earlier: C++23 migration and code review, validation pass, lifter slice 1, differential test).

## Language and toolchain
Everything is **C++23**: the recompiler, the runtime header and the generated code. The C++23 features in use are `std::expected` (XBE loader and lifter results), `std::print`/`std::format`, `std::ranges` algorithms, `std::string::contains`, `std::unreachable` and `std::bit_cast`.

Minimum compilers are GCC 14, Clang 19, Xcode 16 and Visual Studio 2022 17.10. CMake checks these at configure time. Clang 18 is excluded because it can't use libstdc++'s `std::expected`.

CI builds with warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow`, MSVC `/W4`) on six configurations:
- Linux x64 with GCC 14
- Linux x64 with Clang 19
- Linux ARM64 with GCC 14
- macOS ARM64 and macOS x64 (Intel): the runtime built for macOS 10.15, the tools for 13.3
- Windows x64 with MSVC

The generated code also has to be warning-free at `-Wall -Wextra`. The differential test compiles it with `-Werror`.

## Done

### XBE loader (`recomp/src/xbe.*`)
It parses the image header, certificate, section table, library versions and kernel thunk table, and it builds the flat guest image (BSS zero-filled). `Xbe::load`/`parse` return `std::expected<Xbe, std::string>`: a malformed file is an error message, never a crash.

The certificate title is decoded from UTF-16 (surrogate pairs included). `code_section_at()` does a binary search over the code sections, because it sits in the analysis passes' hottest loops.

**Check:** `qilin info` output is identical, field for field, to the Python prototype's `xbeinfo.json`.

### Decoder (`recomp/src/decoder.*`)
This is Zydis v4.1.1 in 32-bit legacy mode, vendored.

Decoded instructions are cached in a compact 120-byte form: three operands at most, enums packed into bit-fields, and no raw bytes. The bytes are read back from the image when needed. The cache is a flat, address-indexed slot table: one 32-bit slot per byte of the code range, which is 8 MB for Ghoulies. The linear sweep doesn't use the cache at all.

**Cross-check against LLVM 18** (`tools/check/verify_decoder.py`):

| Set | Instructions | Result |
|---|---|---|
| Every reachable instruction | 529,718 | **100% agreement** on length and mnemonic |
| Linear sweep of every code section, data included | 660,357 | No unexplained disagreement; 275 known-benign cases, all in data bytes |

The 275 known-benign cases fall into three groups:
- **238 encodings LLVM rejects or splits**: undocumented aliases Zydis accepts (`SAL /6`, `TEST /1`, `int1`, `bound`, ...), plus LLVM returning a lone `lock`/`xrelease` prefix.
- **37 encodings Zydis rightly rejects:** `mov cs` and one EVEX instruction, which no Pentium III can run.

An earlier version of this note said 238 in total. That count was taken before the third class was added.

The naming differences between the two libraries are mapped explicitly in the script. Examples: `fcomip`/`fcompi`, `popad`/`popal`, and Zydis's `ret far` versus LLVM's `retf`.

### Function discovery + CFG (`recomp/src/cfg.*`)
This is a C++ port of `tools/analysis/cfg.py` and `analyze.py`. It works on Zydis's structured operands instead of disassembly text. It runs in about **4 s**, including the fixpoint passes described under "Complete coverage" below. Reachable instructions are tracked in a bitmap.

| | Python prototype | C++ port, v0.3.0 | C++ port, now |
|---|---|---|---|
| Functions | 9,402 | 9,197 | 8,842 |
| Reachable instructions | 530,764 | 529,718 | 530,444 |
| Lifted code accounted for (game, Rare libraries, EH funclets) | — | — | **100%**, 0 unknown bytes |
| Jump tables resolved | 556 | 553 | 556 (307 cmp, 202 cmp + byte table, 5 `and` mask, 35 value-range, 7 heuristic, none in lifted code) |
| Unresolved indirect jumps | 24 | 26 | 28 (9 in lifted code, all indirect tail calls, 2 of them through tables of records in `.rdata`) |
| Indirect calls | ~1,960 | 1,980 | 1,930 |

There are fewer functions than in v0.3.0 because fragments of real functions were merged back into their parents (2,384 merges in the current pass order), while about 1,300 functions were newly found.

**Where the prototype and the port disagree, the port is better in every case examined:**
- **Indexed table addresses are no longer code-pointer seeds.** The prototype's text regex treated the table address in `jmp [4*reg + table]` as a possible function pointer, so jump tables were decoded as code.
- **Misaligned seeds are pruned.** Some data values are 16-aligned and look like code pointers, but land inside another function's instruction. For example, `0x80000` is the `CC` byte inside `mov ecx, [0x4275CC]`.
- **Fewer split functions.** 146 of the prototype's 154 extra "functions" are instructions inside a larger function in the port. The one checked by hand, `0x26F60`, is an ordinary branch target that the prototype split off because of its seeding order. Three of the remaining eight were checked and are mid-instruction starts.

**Checks:** on ARM64 (a GCC 14 cross-build run under qemu), both the CFG output and the lifted C++ are byte-identical to x86-64.

### Validation pass (`Cfg::validate`)
This pass removes functions that are really data decoded as code, and repeats until nothing changes. A function is dropped when any of these holds:
- **Its entry lies inside another function's instruction.** The side with weaker evidence is dropped: a guessed seed loses to a direct call target.
- **It fails to decode or has overlapping instructions.**
- **It contains an instruction MSVC-compiled retail code never contains:**
  - far control flow;
  - privileged instructions, port I/O, `cli`/`sti`;
  - `int`/`int1`, BCD, `bound`/`arpl`;
  - segment-register writes, 16-bit addressing;
  - undocumented `SAL /6`/`TEST /1`;
  - anything outside the Pentium III's instruction sets (base, x87, MMX, SSE).

Functions that are the entry point, or direct call/jump targets of surviving functions, are kept but listed by `qilin suspects`.

**Result:**
- 8,842 functions.
- 0 decode failures and 0 overlaps.
- 119 data-as-code functions dropped: 46 misaligned seeds and 73 that failed the checks above.
- 7 suspects remain. All are hardware code in XAPI (`int 0x2D`, far jump, `cli`/`sti`), D3D (`wbinvd`, port I/O) and XPP (`cli`/`sti`). Those libraries get replaced, so none of this is lifted.

### Complete coverage (v0.4.0)
`qilin coverage <xbe> [--list]` sorts every byte of each text region into code, tables (jump and byte index tables), padding (`int3`, `nop` and MSVC's alignment filler), or unknown. The exit code is 3 while any byte of a lifted region is unknown, so CI can hold the line.

| Region | Range | Lifted | Code | Tables | Padding | Unknown |
|---|---|---|---|---|---|---|
| Game | `0x11000–0x14A840` | yes | 1,215,616 | 24,907 | 43,637 | **0** |
| Rare engine libraries | `0x152780–0x165C00` | yes | 75,308 | 444 | 3,224 | **0** |
| C++ EH funclets | `0x1707B0–0x171020` | yes | 2,043 | 0 | 117 | **0** |

XAPI and the XDK library sections (D3D, DSOUND, BINK and so on) aren't lifted: they get replaced. Their coverage is reported but doesn't gate anything. The CRT is lifted function by function, only what lifted code reaches (v0.11.0), so its region isn't a coverage gate either; the SEH scope tables took its unknown bytes from 2,561 to 1,529.

**The game code ends at `0x14A840`, not `0x14A895`** (corrected in v0.7.0). `0x14A840` is XAPI's `IsBadReadPtr`, which probes the pages it's given inside `__try`/`__except` and returns TRUE if one faults. The old boundary fell inside it, so it was lifted as a game function without its last part, which holds the exception handler and the NULL-pointer return. A bad pointer would have crashed the recomp, and a NULL one would have reached a stub for `0x14A8A0` and trapped; the original returns TRUE for both. It's now a stub like the rest of XAPI, so the runtime replaces it. The game calls it from 59 sites.

The game code went from 15,198 unknown bytes to 0. These are the rules that got it there, each grounded in how MSVC lays out code:

- **Function alignment.** Every function in the game and Rare library regions starts on a 16-byte boundary. A candidate entry that isn't aligned there is rejected.
- **C++ exception handling tables.** `FuncInfo` records (magic `0x19930520`–`22`) in `.rdata` and `.data` lead to unwind funclets, catch handlers, and `__ehhandler` stubs (`mov eax, &FuncInfo; jmp __CxxFrameHandler`). A catch handler returns its continuation address as `mov eax, imm32; ret`, so those continuations become entry points of their parent's code. This found 70 tables, 68 stubs, 147 funclets and 20 continuations.
- **Evidence strength.** A direct call, the entry point and the EH tables are strong evidence that an address is a function. A 16-aligned value in a data section, or an immediate operand, is weak evidence: a constant can look like an address. A tail jump to an address that follows `int3` padding is strong, because MSVC pads with `int3` only between functions.
- **Fragment merging.** A function is cut short when exploration stops at a false candidate. If function G jumps past such candidates and none of them has strong evidence, they are labels inside G: they are dropped and G is re-explored. Weak evidence stands unless G's own code falls through into the candidate, or jumps to it over alignment filler. 2,384 merges happen this way in the current pass order.
- **Labels.** A "function" whose entry is an instruction of an earlier function, reached only by jumps (no call, no EH entry, no pointer), is a label that an earlier, shorter exploration mistook for a tail call. The same goes for a 16-aligned loop head that its function reaches with `jmp short` over alignment filler.
- **Takeover.** An aligned data or immediate pointer whose code runs through weaker functions' entries (orphan or gap seeds) takes them over: those entries are labels inside it.
- **Misaligned overlaps.** When one function's entry falls inside another's instruction, the one with weaker evidence goes. Before v0.4.0 the inner seed always lost, which threw away a real function at `0xD3310` in favour of a jump table decoded as an orphan.
- **Switch bounds.** The backward search for a jump table's bound follows the index register through `movzx` and `mov reg, reg` and stops when the register is overwritten. It accepts `and reg, 2^k - 1` as a bound. A table entry may not land on another function's start, and byte index tables are clipped against their dword table.
- **Fixpoint.** Immediate seeding, fragment merging, validation, label dropping and re-exploration repeat until nothing changes (at most 8 rounds).

### Jump-table sizes: value-range analysis (`recomp/src/vrange.*`)
A `cmp` or `and` directly above a table jump bounds most switches. For the rest, the CFG runs a value-range analysis over the function and uses the proven range of the index register at the jump. Before this, those tables were sized by scanning until an entry stopped looking like a code address.

**How it works:**
- Each of the eight 32-bit registers holds an unsigned interval. Instructions update it: constant moves, register copies, `movzx` (including loads from a byte index table, whose bytes give the range), `lea`, `add`/`sub`/`inc`/`dec` with x86 wraparound, `and` masks, shifts and `imul` by a constant.
- On each conditional branch, the last `cmp`/`test` narrows the compared register: unsigned, signed and equality conditions, a register holding a constant on either side, and the flags of `sub reg, imm`.
- Ranges flow along every path, including from one jump table's cases into another, and merge where paths join. Anything it can't model (memory, calls, partial-register writes) makes the register unknown, so a result is never narrower than the truth.
- A proof is accepted only if every slot in the range holds an address in the code section. The function is then re-explored with the proven table.

**Results in lifted code:** all 32 heuristically sized tables there are now proven, or turned out not to be switch tables.
- **15 tables in the `0x108xxx` group** (`dec ecx; cmp ecx, 4; ja`) were over-read by the scan. Each was taken as 10 slots when it has 5, running into the next function's table, so every one of those functions had 5 phantom cases.
- **`0x16322D`** was read as 14 slots; it has 10. The last 4 are a separate table, used by the jump at `0x16328D`.
- **`0x165B44`** in the Rare `memcpy` uses MSVC's negative-index form: `sub ecx, 4; jb` then `jmp [4*ecx + 0x165C4C]` with ecx from -4 to -1, which reads the 4 slots at `0x165C3C`. Tables now carry a signed first index for this.
- **`0xD410B` and `0xD4179`** index tables of 20-byte records in `.rdata` (`lea ecx, [eax+eax*4]` then `jmp [4*ecx + 0x304EB8]`). They aren't switches, and are now indirect jumps. A switch table always sits in the code section.
- **The others** had their bound in a register (`mov edx, 3; cmp ecx, edx`), in an earlier block, or behind a `rep movsd` that the old bound search wrongly treated as overwriting every register.

**Cross-check:** run on the 311 tables already bounded by `cmp` or `and`, the analysis gives the same size for all of them except two `and` tables in replaced library code, where it reports the full mask range and the old path drops unused trailing slots. Both answers are correct. The 7 tables still sized by scanning are all in XDK libraries that aren't lifted.

**Lift check.** The lift report has no failures (in v0.3.0, 61 were fragments).

**Replaced functions inside the lifted ranges** (v0.8.0; empty since v0.21.0). While Bink was to be replaced, three functions in the Rare-library range that only Bink calls were listed in `kReplaced` and skipped, and `qilin lift` fails (exit 3) if anything it lifts could reach a listed one (a call or tail call, the address as an immediate or displacement, or stored in a data section). Bink is lifted now (below), so two of them (`0x162570`, `0x1626B0`) are lifted with it and the list is empty. The third, Bink's MMX probe at `0x162840` (`pushfd`/`popfd` to test the CPUID flag, then `cpuid`), is in `kNative` and answered by the runtime as the Xbox's Pentium III would (it has MMX), rather than lifting `pushfd`, `popfd` and `cpuid` for one probe whose answer is known. Native functions inside lifted ranges are skipped by the range lift too.

### Lifter, slices 1–3 and exception frames (`recomp/src/lifter.*`, runtime `recomp/runtime/guest.hpp` and `dispatch.cpp`)
Each guest function becomes `void fn_XXXXXXXX(rt::Context&)`. `Lifter::lift` returns `std::expected<std::string, LiftFailure>`, where the failure carries a kind (fragment, unsupported instruction, x87, undefined flag, implausible, bad control flow), the address and the instruction.

Inside a lifted function:
- **Registers:** guest registers are host locals. The prologue declares only the registers the function references. Each exit stores back only the registers it modifies: the compiler can't drop those stores itself, because guest memory writes may alias the context.
- **Flags:** EFLAGS are separate bools. A backward liveness pass means a flag is computed only where something can read it. Reading a flag the previous instruction left undefined is a lift error, including OF/AF after a shift by `cl`. Instructions the lifter's own table doesn't list take their flag effects from Zydis's tables. Before, those were taken to touch no flags, so `fcomi`, `bt`, `bsf` and the rotates looked transparent. A check over every decodable address in the image found that the hand table agrees with Zydis everywhere, or is deliberately stricter: shift counts that may be zero, `rep` with ecx = 0, and AF after `sbb`.
- **Control flow:** blocks become labels and gotos. Jump tables become `switch` with a trap for an unknown index.
- **Calls (slice 2):**
  - **Direct `call T`:** push the guest return address onto the guest stack, store the modified registers back to the Context (`writeback()`), call `fn_T(c)` as a plain C++ call, reload the registers the function uses (`reload()`), and check that the callee returned to the call site. A callee that returns anywhere else traps. The stack layout is exactly the original's, so arguments at `[esp+4]` and callees that move their return address (`__SEH_prolog`, `_chkstk`) work unchanged.
  - **Tail calls:** a `jmp` or `jcc` out of the function writes back, calls `fn_T(c)` and returns.
  - **Indirect calls and jumps** (`call [eax+0x14]`, `jmp eax`, the two record tables in `.rdata`) go through `rt::call(c, target)`. It binary-searches the generated function table, then falls back to `rt::call_unlifted`. That fallback belongs to whatever links the code: high-level replacements in the runtime, the original bytes in the differential test, otherwise a trap with the address.
  - **Stubs:** every call or tail-call target without a lifted body gets a generated `fn_T` that forwards to `rt::call_unlifted`, so all direct calls link. Since v0.11.0 there are 252, all library entry points that lifted code calls directly, so they are the concrete list of what the runtime must replace. (v0.5.0 had 699 stubs, 251 of them library entry points; v0.6.0 had 287, 12 of them game functions with exception frames; v0.10.0 had 276, 41 of them in the CRT, which is now lifted.)
  - **Flags** aren't carried across calls. A flag read after a call before any write is a lift error, never a guess. No function in the game does this. The C runtime's hand-written x87 maths does, in four reviewed places, listed as conventions (below).
  - **Direction flag:** tracked statically per instruction. It's clear on entry and after every call (the MSVC ABI), set by `std` and cleared by `cld`. String instructions run in that direction, and DF must be clear at every call and exit. The Rare `memcpy` uses `std; rep movsd; cld` for overlapping copies.
  - **The stack-balance check** treats esp as unknown after a call, since a callee ending in `ret n` pops its arguments too.
- **Integer ops:**
  - mov/movzx/movsx/lea/xchg;
  - add/sub/adc/sbb/cmp/and/or/xor/test/inc/dec/neg/not;
  - shl/shr/sar; shld/shrd by an immediate or `cl` (v0.11.0);
  - rcl/rcr by 1 (v0.11.0; other counts aren't used);
  - jecxz (v0.11.0);
  - bsf/bsr (v0.12.0; a zero source sets ZF and leaves the destination unchanged, as the Pentium III does);
  - imul/mul/div/idiv (traps on #DE);
  - cdq/cwde, setcc/cmovcc, lahf/sahf;
  - push/pop/leave/ret.
- **String ops:** `rep stos`/`rep movs` (8, 16 and 32-bit, with an F2 or F3 prefix) run as whole runs. They use `memset`/`memmove` where that's equivalent, and an element-wise copy only when the destination overlaps the source run in the direction of copying, which is exactly the CPU's pattern-replicating behaviour. Backward copies (DF = 1) have their own helper. `cmps` and `scas` (v0.11.0) with `repe`/`repne` update the flags only if at least one element was compared, as the CPU does when ecx is 0.
- **Scalar SSE**, bit-exact on lane 0:
  - movss/movaps/movups;
  - add/sub/mul/div/min/max/sqrtss;
  - the and/or/xor/andn packed logic ops;
  - comiss/ucomiss;
  - cvtsi2ss/cvt(t)ss2si, with x86 overflow semantics;
  - packed (v0.12.0, for XGraphics' maths): add/sub/mul/div/min/max ps lane by lane with the scalar rules, shufps, unpcklps/unpckhps, movlps/movhps, movhlps/movlhps. 16-byte memory operands must be aligned, as on the hardware (#GP otherwise).

  **NaN results follow x86 rules on every host.** A NaN operand propagates, quietened; if both are NaN, the first operand's. An invalid operation on non-NaN operands (inf − inf, 0 × inf, 0 / 0, the square root of a negative) gives the default NaN `0xFFC00000`. `rt::addss` and its siblings check the result with one compare, since NaN results are rare. Before, the result depended on the host: ARM's default NaN is `0x7FC00000`, and a C++ compiler may swap the operands of `a + b`, which changes which NaN propagates. Unit tests cover the rules.

  `guest.hpp` turns off multiply-add contraction for Clang and MSVC and refuses `-ffast-math`. The `qilin_runtime` CMake target adds `-ffp-contract=off` for GCC.
- **x87 (slice 3):** 9,646 instructions in 628 lifted functions, mostly float loads and stores, arithmetic, `fcomip` and float return values in `ST(0)`. There are 30 float-returning functions, called from 665 sites.
  - **State:** the register stack lives in the Context in physical order, as eight doubles plus TOP, the tag bits, the control word and the status word. TOP, tags and the two words are held in locals like the general registers, written back at calls and exits and reloaded after calls. So values cross calls exactly as on the hardware: float returns, C-runtime helpers that take arguments on the stack, virtual calls. Nothing is inferred about what a callee pushes.
  - **Precision control is honoured at runtime.** The game calls `_controlfp(_PC_24, _MCW_PC)` at startup (`0x33462`), so add, subtract, multiply, divide and square root round to a 24-bit significand while the exponent keeps the register's range. That is unlike a C++ `float`, which would overflow at 3.4e38 and denormalise below 1.2e-38. `rt::fadd` & co compute in double and round once. That's exact when both operands have at most 24 significant bits, because 53 ≥ 2·24 + 2. When an operand is wider (a loaded double or integer, a constant, a transcendental result), the double result is first made round-to-odd from its exact error (TwoSum, or `fma` for products, quotients and roots), which makes the second rounding exact as well. At 53-bit precision (the C runtime's setting until the game changes it, so static constructors run with it) double arithmetic is the x87 arithmetic.
  - **NaN results follow x87 rules on every host:** a QNaN beats an SNaN; between two of the same kind the larger significand wins; invalid operations give the indefinite `0xFFF8…`. Loads quieten SNaNs, as `fld` does. Arithmetic memory operands keep them signalling, because that decides which NaN propagates.
  - **Also covered:** loads and stores of 32 and 64-bit floats and 16/32/64-bit integers, `fxch`, `fchs`, `fabs`, the constants, `fcom`/`fucom`/`ftst` with `fnstsw ax`, `fcomi`/`fucomi` setting EFLAGS directly, `fist` by the control word's rounding mode, `fsin`/`fcos`/`fptan` (C2 set and the operand unchanged at 2^63 and above), `fpatan` and `fyl2x`. Since v0.11.0, for the C runtime: `fistp` to 64 bits, `frndint` (by the rounding mode), `fscale`, `fxam`, `fnclex`, `f2xm1`, `fsincos` (v0.12.0, XGraphics), `fld` of an 80-bit real, and `fnsave`/`frstor` (the 108-byte image with 80-bit registers; the instruction and operand pointers are stored as 0, and nothing but `frstor` reads them).
  - **Not exact, by design:** values wider than 53 bits. That covers `fild` of a 64-bit integer above 2^53, the x87's 64-bit constants (`fldpi`, `fldlg2` …; these use their nearest doubles), 80-bit constants loaded with `fld tbyte` (the C runtime's; also the nearest double), magnitudes outside double's exponent range (the register's is wider; float data stays far inside it), and precision control at 64 bits, which the game doesn't use. Transcendentals are below.
  - **Transcendentals (v0.9.0):** `fsin`, `fcos`, `fptan`, `fpatan`, `fyl2x` and, since v0.11.0, `f2xm1` (2^x − 1, for the C runtime's `pow`) return the correctly rounded double of the true function at their operands. They come from `recomp/runtime/x87_math.hpp`, not the host's maths library.
    - **The same bits on every host.** The code uses only IEEE addition, subtraction, multiplication, division and square root, which every supported compiler and CPU computes identically with contraction off. Exact products use the hardware `fma` where the target has one (ARM64, x86-64 built with FMA) and Dekker's method otherwise; both are exact, so the bits don't depend on which ran. No library `fma`, whose software fallbacks have had bugs.
    - **Fast path, then a rounding check.** Each function first evaluates with a relative error below 2^-64: Cody–Waite reduction for \|x\| ≤ 2^20, a 16-entry table for `atan`, and double-double only for the leading terms. If that bound can't change the rounding, the result is the correctly rounded double. That holds for about 99.7% of calls. Otherwise the slow path runs: exact Payne–Hanek reduction and double-double throughout, about 100 bits.
    - **Correctly rounded except** where the true value lies within about 2^-100 of a rounding boundary. Subnormal results are rounded correctly too.
    - **Speed:** about 50–100 ns per call on the x86-64 test machine, against 4–23 ns for glibc. The first all-double-double version took 760–1,970 ns; the fast paths made it 10 to 20 times faster.
    - **Not the hardware's own results.** The x87 computes 64-bit results with its own argument reduction, which for large arguments isn't the true value; those aren't reproduced. The game stores most of these results as `float`.
    - **Checks:**
      - `tools/check/x87/transcendental.py` compares against mpmath on 24 operand ranges, including near multiples of π/2, arguments up to 2^63, x near 1 for `fyl2x`, and subnormal operands and results. 480,000 cases per seed: 0 wrong, with GCC 14 and Clang 19, with and without FMA, and cross-built for ARM64 under qemu. The host glibc was wrong in up to 26% of `fyl2x` cases (it rounds twice) and 1.4% of `tan` cases near multiples of π/2.
      - `tools/check/x87/consistency.cpp` (the `qilin_x87_consistency` test, so every CI platform runs it) checks the fast paths against the slow ones: 16 million cases per compiler, 0 differences.
      - 34 golden vectors in the unit tests pin the exact bits on every CI platform (45 with `f2xm1`).
      - **`f2xm1` (v0.11.0):** 2^x − 1 = 2^n (1 + T_j)(1 + E) − 1 with x = n + j/32 + r, a 32-entry table of 2^(j/32) − 1 in double-double, and E = expm1(r ln 2) by Taylor series (to r^9 in the fast path, r^15 in the slow one). Defined for every finite x, so also beyond the x87's \|x\| ≤ 1. Checked like the others: 0 wrong against mpmath on 5 more ranges (100,000 cases per seed, three seeds, GCC, Clang and ARM64 under qemu); fast against slow on 6 million cases, 0 differences. The host's `exp2(x) - 1` was wrong in 66% of cases with \|x\| ≤ 1, from the cancellation.
  - **`fst st(i)` marks its destination valid.** An earlier draft only copied the value, so a store into an empty register left it tagged empty.
  - **Checked against exact arithmetic:** `tools/check/x87/rounding.py` runs 40,000 random operations per seed, narrow and deliberately wide and near ties, through `rt::fadd`/`fsub`/`fmul`/`fdiv`/`fsqrt`, and compares each with the exact rational result rounded to 24 bits. 0 mismatches, built with GCC 14 and Clang 19. The naive "double, then round" version fails 24 of 40,000.
- **C++ exception frames (v0.7.0):** 20 functions link a registration record into the thread's exception chain at `fs:[0]` and unlink it on the way out (`push -1; push __ehhandler; mov eax, fs:[0]; push eax; mov fs:[0], esp` … `mov fs:[0], ecx`).
  - **The chain lives in guest memory,** exactly as in the original. `fs:` addresses are relative to `c.fs_base`, the guest address of the thread's information block (on the Xbox, the KPCR's `NT_TIB`). The runtime gives each guest thread its own block. Library code that walks the chain (the CRT's `__CxxFrameHandler`, and `__SEH_prolog`, which XAPI uses) therefore sees the same records the game wrote.
  - **A throw stops the game with a diagnostic; nothing unwinds.** The runtime replaces `_CxxThrowException` (`0x1679C7`), which traps with the exception's type and message. That loses nothing, because no exception the game can throw is ever handled:
    - **Throws in lifted code** are all `std::logic_error`s from the STL: `length_error` "vector<T> too long" (`0x184E0`), and through the CRT, `length_error` "string too long" (`0x16FC3F`) and `out_of_range` "invalid string position" (`0x16FBE7`).
    - **13 lifted functions have try blocks, 17 in all,** each with one `catch (...)`. 16 of those free memory or destroy elements and rethrow.
    - **The one that resumes** is in `std::string`'s reallocation (`0x18550`): if allocating the larger buffer throws, it retries with the exact size. Only `operator new` (`0x165F33`) is inside that try block. On the Xbox CRT it returns NULL instead of throwing. `_nh_malloc` only ever calls an installed new handler, which could throw, and no reachable code installs one (`[0x4DD2C8]` is only read).

    So in the original, every throw reaches the top of the thread unhandled and the game ends. The handlers that would run first only free memory. The catch handlers and their continuations are lifted as unreachable code inside their parent functions. `tools/check/eh_report.py` lists the frames, try blocks, catch handlers (following their branches to a rethrow or a continuation) and every call to `_CxxThrowException`.
- **Stack-balance check:** a forward esp/ebp analysis rejects "functions" that are fragments of other functions: they return with esp off balance or pop their own return address.
- **XAPI (v0.12.0)** is lifted the same way as the C runtime below: the XAPI functions lifted code can reach, plus XAPI's process start-up (`0x151011`), which the runtime calls. Together with the CRT that's 471 library functions (6,214 lifted in all), and the runtime implements the kernel beneath them. XAPI's packed-SSE maths (XGraphics' matrix and vector functions, `XGMatrixMultiply` among them, with 46 call sites) is pure computation, so lifting gives the exact original results. Four XAPI functions can't be lifted and are listed in `kNative` with the reason: `QueryPerformanceCounter` (`rdtsc`), `OutputDebugStringA` (`int 0x2D`), a start-up patch of kernel code and a shutdown notification that reloads the GDT (`cli`, `sgdt`, a far jump). `qilin lift` doesn't follow them, and the runtime replaces the first two.
- **XACT and XGraphics (v0.15.0)** are lifted the same way: the functions lifted code can reach in the `XACTENG` and `XGRPH` sections. XACT is the audio engine (cues, variations, sound banks, notifications); its only hardware access is one read of the APU's sample counter (below), and it sits on DirectSound, which the runtime replaces. XGraphics builds D3D resource headers, swizzles textures and does matrix maths; it touches no hardware. Three DirectSound functions XACT calls are pure computation and lifted too (`kLiftedDsound`): `XAudioCalculatePitch` (an x87 `fyl2x`, with its two lock helpers) and the PCM and Xbox ADPCM `WAVEFORMATEX` builders. 7,221 functions lift in all, 1,478 of them library functions; the game's generated code is unchanged.
- **D3DX (v0.18.0)** is lifted whole (the `D3DX` section, 24 functions: matrix and vector maths): 2,376 trials, 1,148 matching, every function exercised, 0 mismatches.
- **D3D's resource code (v0.16.0)** is lifted too (`kLiftedD3d`, 18 functions): creating textures, surfaces and vertex buffers, surface levels, descriptions and locks. It's computation over the game's allocator, so headers, pitches and swizzled layouts are the original's; what it calls that waits on the GPU or writes the command buffer stays native (`graphics.md`).
- **Function discovery: immediate pointers after `ret n` (v0.16.0).** A code address pushed or stored as an immediate is taken as a function entry when it follows padding or a `ret`; library code packs functions without padding, so an entry right after a `ret n` that is an instruction of a function already found counts too. That found six callbacks the CFG had missed: XAPI's thread start routine (`0x14BB4D`), XACT's DPC routine (`0x1BD90B`), an XGraphics constructor and destructor passed to the C runtime's array constructor, and two more in XAPI. The game's own code is unchanged (5,743 functions); 7,244 lift in all. Their differential test: 594 trials, 0 mismatches.
- **Bink (v0.21.0)** is lifted: RAD's video codec, the `BINK` section and its pixel-format blitters (`BINK32` … `BINK32M`, `0x1A59E0`–`0x1BABA0`), the functions lifted code can reach. It's CPU code (decoding, colour conversion, file and thread handling through XAPI, sound through DirectSound). Its blitters are reached only through tables in its own data section, so function discovery reads aligned pointers into Bink's sections from `BINKDATA` too (`CfgConfig::lib_data_section`): 32 entries. 7,572 functions lift in all. Differential test of the blitters: 0 mismatches.
- **More function discovery (v0.21.0).** An immediate code pointer also counts as an entry right after a found function's final unconditional `jmp`, and the immediate-pointer round now repeats until it finds nothing: a table of callbacks filled in code (DirectSound's full-HRTF table, ten functions each starting where the one before ends) is found one entry per round. Ten new entries in all, the game's own code unchanged.
- **Device registers (v0.15.0).** A load or store at a constant address in the Xbox's register space (`0xFD000000`–`0xFEFFFFFF`: the NV2A, the APU …) is lifted to `rt::mmio_read32` / `rt::mmio_write32`, which the runtime implements register by register; an unregistered one stops it, named. Any other access there (another width, or a computed address) fails the lift. In all the lifted code there is exactly one: XACT reads the APU's global sample counter (`0xFE80200C`) as its clock.
- **New instructions (v0.15.0):** 16-bit `imul` (two and three operands; XACT), `rol`/`ror` by an immediate or `cl`, with the count masked and a zero count changing nothing, flags included (XGraphics), `movntps` (a non-temporal aligned store, lifted as `movaps`), and MMX: `movq` to and from MMX registers and memory, `punpcklwd`/`punpckhwd`, `emms` (XGraphics' swizzler). MMX registers are their own `Context::mm[8]`, since the x87 registers they alias are doubles here. Each MMX instruction sets the x87 state as the hardware does (TOP 0, every register in use) and `emms` frees them all; the swizzler enters with the x87 stack empty and ends with `emms`, so nothing is lost by keeping the two apart. **v0.21.0, for Bink:** `movd`, `paddw`, `paddsw`, `psubusw`, `pmullw`, `pmulhw`, `por`, `pxor`, `pand`, `pandn`, the word, dword and quadword shifts (`psllw`/`pslld`/`psllq`, `psrlw`/`psrld`/`psrlq`, by an immediate, a register or memory, a count past the lane's width giving 0), and the byte and dword unpacks (`punpcklbw`, `punpckhbw`, `punpckldq`, `punpckhdq`; a low unpack from memory reads 32 bits), each in `rt::mmx`, lane by lane as the CPU computes. Bink's blitters end without `emms`, leaving MMX values in the x87 registers' mantissas; the difftest compares only TOP and the tags then. And `rdtsc`, which reads the runtime's time-stamp counter: 733 MHz on the host's clock, the count `QueryPerformanceCounter` gives.
- **The C runtime (v0.11.0).** LIBCMT (`0x165C00–0x1707B0`) is lifted rather than replaced, so `sprintf`, `strtod`, the heap wrappers and the x87 maths behave exactly as in the original. Native versions of hot, simple functions (`memcpy`, `strlen`) may replace lifted ones later, each checked against the original.
  - **Only what lifted code can reach.** `qilin lift` adds the CRT functions that lifted code calls, tail-calls or names as an immediate or displacement, transitively, plus every CRT function whose address is stored in a data section (the CRT's own pointer tables: the float-formatting hooks `printf` calls through, the initializer tables). That's 265 of the CRT's 434 functions. If any of them doesn't lift, `qilin lift` fails.
  - **Conventions** (`crt_conventions()` in `main.cpp`): the CRT's hand-written assembly breaks the usual function contract in a few places, each listed by address and reviewed, never inferred.
    - *Flags across calls:* two helpers classify an operand (inf or NaN) and return the answer in ZF (`0x16B7A5`, `0x16B7E8`); the next routine branches on it (`0x16751D`, `0x1706FD`, the cores of `asin` and `acos`). For those, the flags travel in `c.eflags` at their x86 bit positions: packed at the `ret` (or before the call, for flags passed in) and unpacked after the call (or at the entry).
    - *Stack glue:* `__SEH_prolog`/`__SEH_epilog`, `__EH_prolog`, `strchr`'s shared exit (`lea eax, [edx-1]; pop ebx; ret`, jumped to from inside `strchr`), a `__finally` caller (`pop eax; pop ecx; xchg [esp], eax; jmp eax`) and the x87 maths' shared epilogues (`pop edx; ret`, restoring the caller's saved control word). They pop past their own return address or return with esp moved, so the stack-balance check is skipped for them. The generated code follows esp at run time either way, and every caller still checks where it was returned to.
  - **SEH scope tables** (in the CFG). `__try`/`__except` in the CRT and XAPI pushes a scope table, through `__SEH_prolog` or inline. For each `__except`, the filter expression and the handler body are laid out inside the parent function, and the handler continues into the parent's code. The CFG now reads the 46 tables (24 `__except` entries) and explores those filters and handlers as entry points inside their parent, like C++ catch handlers. Before, a handler that a scope table pointed to became a "function", which cut `_input` (`scanf`'s engine) into about 60 fragments joined by tail calls. A `__finally` handler is a real function the parent also calls directly, so it's left as one. The runtime never dispatches SEH (a fault traps), so these entry points are unreachable code, as the catch handlers are.
  - **Negated jump-table indices** (in the value-range analysis): `memmove`'s backward tail copy is `neg ecx; jmp [4*ecx + T]` with ecx in 0..7. The analysis now takes the range before the `neg`, giving a proven table of indices −7..0. Before, the table was sized by scanning at one entry, and the lifted code trapped on every other index; the difftest found it.
  - **Start-up.** The CRT's initializers run as XAPI ran them (see `runtime.md`).

**Coverage today:** `qilin lift` covers the game code (`0x11000–0x14A840`) and the Rare engine libraries (`0x152780–0x165C00`): 5,746 functions, less the 3 that belong to Bink, so 5,743. Before v0.7.0 the game range ended at `0x14A895` and held 5,747, one of them XAPI's `IsBadReadPtr`. Since v0.11.0 it adds the 265 CRT functions that code can reach (6,008 of 6,008), and since v0.12.0 the reachable XAPI functions too: **6,214 of 6,214**, 471 of them in the two libraries.

| | v0.4.0 (slice 1) | v0.5.0 (slice 2) | v0.6.0 (slice 3) | v0.7.0 (exception frames) | v0.8.0 | v0.11.0 (with the CRT) |
|---|---|---|---|---|---|---|
| **Lifted** | 1,276 | 5,098 (88.7%) | 5,726 (99.6%) | 5,745 of 5,746 | 5,743 of 5,743 (100%) | **6,008 of 6,008 (100%)** |
| Not leaves (needed calls) | 4,422 | 0 | 0 | 0 | 0 | 0 |
| Use x87 | 48 | 628 | 0 | 0 | 0 | 0 |
| C++ exception frames (`fs:[0]`) | — | 20 | 20 | 0 | 0 | 0 |
| One-off (`pushfd` in a CPU-ID probe) | 2 | 1 | 1 | 1 | 0 (Bink's; replaced) | 0 |
| Fragments of other functions | 0 | 0 | 0 | 0 | 0 | 0 |

Most x87 users used to be counted as "not a leaf", because they also make calls.

The generated code compiles with no warnings at `-Wall -Wextra -Werror` under GCC 14 and Clang 19.

### Differential test (`tools/difftest/`)
`difftest.py` lifts everything and builds a shared library from it. The build uses production settings (`-O2`, `-ffp-contract=off`, warnings as errors) plus bounds-checked, write-traced memory. It then runs each function in many trials.

**Each trial:**
1. Build one random guest state: registers, xmm registers, stack arguments, and a 4 MB scratch heap full of pointers, small ints and floats.
2. Run the original x86 bytes in Unicorn, with the code pages read-only. The original runs its whole call tree.
3. Run the lifted C++ on an identical copy of that state.
4. Compare the results bit for bit: every general register, esp, all 8 xmm registers (all lanes), every written byte, and the return address. If the original faults, the lifted code must trap too.
5. **x87 state** is compared too: TOP, which registers are valid, every valid register's value and the control word. Unicorn holds 80-bit registers and the lifted code doubles, so a value matches only if the original's register holds exactly that double. A register holding more than 53 bits in the original (a 64-bit constant or a transcendental result) that matches once rounded would be counted apart, as `pass-x87wide`. No trial in the v0.6.0 runs needed that.

Half the trials start with a value already in `ST(0)`, as when a caller passes one in. Each function runs under both precision-control settings: the game's 24-bit (the first heap image) and the C runtime's 53-bit (the second). With `--variants 4`, a second pair of heap images is float-heavy, mostly floats with full significands at world and unit-vector scales, where rounding differences show up more often.

**Segments.** Both emulators get a GDT with flat 32-bit code and data segments, and `fs` on a thread information block at `0x3F10000`, which the lifted side gets as `c.fs_base`. Unicorn ignores a plain FS base in 32-bit mode, so `fs` needs a real descriptor. The block's first word, the exception-chain head, starts each trial pointing further up the stack, and writes to it are compared like any other memory.

**Calls.** Lifted callees are called directly. A callee with no lifted body (library code) runs from its original bytes in a second Unicorn instance that maps the lifted side's memory, so whole call trees are compared whatever their callees are. A called callee stops at the return address at `[esp]`. A tail-called one (marked in test builds, since it may be the middle of an epilogue with saved registers at `[esp]`) stops at the tail-calling function's own return address.

**Isolation between trials:** both emulators restore a saved CPU context before every trial, covering segments, control registers, x87 and MXCSR. x87 and MXCSR state carry over between callees within a trial, as in the original. Each chunk of 120 functions runs in a fresh process.

**Runaway guards** (`QILIN_TEST_HOOKS`, compiled out of normal builds): every lifted function opens a frame and every backward branch ticks. The harness stops a trial at a call depth of 4,096, at 2 million calls, 20 million loop iterations or 50,000 unlifted calls, or after 10 seconds, and reports it as a mismatch. The original's run is capped at 200,000 instructions, so a correct lifted run stays far below all of these.

**Set aside, not failed:**
- **NaN against NaN.** Unicorn propagates SSE NaNs by x87 rules (larger payload wins), real SSE by operand order. Trials whose only differences are NaN against NaN, in an xmm lane or an aligned memory word, are counted as `pass-nan`.
- **Calls into non-code.** A call through a null or random pointer makes the original execute data, which depends on CPU state the lifted side doesn't carry (flags, segments). Those trials are skipped.

**Emulator corrections.** Unicorn 2.1 (QEMU 5) differs from the hardware in four places that the game's lifted code reaches (three more for the C runtime are under v0.11.0 below). The harness corrects them in both emulators, at the addresses `qilin insns` lists:
- **`rdtsc`** reads the host's time-stamp counter, so an XAPI timing function (`0x14C29C`) gave different results in the two runs. It now returns a counter that restarts every trial.
- **`fcomi`, `fcomip`, `fucomi` and `fucomip`** leave OF, SF and AF as they were; Intel documents them as cleared. `0x140D80` reads them back with `lahf`. They're cleared after each one.
- **`fyl2x` with an operand ≤ 0** doesn't pop. The lifted code pops, as the hardware does, and those trials are skipped.
- **`fsin`, `fcos`, `fptan`, `fpatan` and `fyl2x`** return the host libm's answer; `fyl2x` computes `log(x) / log(2) * y`, rounding three times. The hardware's 64-bit results aren't available, so for ordinary operands both emulators get the correctly rounded double, computed with mpmath from the exact 80-bit operands. That makes the difftest check the lifted code against its specification. In the x87 run below the correction fired about 48,000 times in the 9 functions that use these instructions.

**Mutation checks:**
- **Slice 2:** a lifter with the post-call `reload()` removed was caught as mismatches in 17 of 100 sampled functions. Without the runaway guards, that build had crashed or hung the harness instead.
- **Slice 3:** four x87 bugs, each planted in the generated code of every function it applies to, then tested on the functions among them that had completed a trial before:

  | Planted bug | Caught in |
  |---|---|
  | One pop dropped per function | 270 of 449 functions |
  | `fcomi` setting CF with its operands swapped | 92 of 155 |
  | Precision control ignored (always 53-bit) | 35 of 310; 56 with the float-heavy heaps added |
  | `fst st(i)` not marking its destination valid (the earlier draft's bug) | 0 of 30 |

  Precision control is the hardest to see. One operation on float inputs, stored straight to a float, gives the same result at either precision, so only longer chains in registers can show it. The `fst st(i)` bug can't show on the game's code: a probe that traps when the destination is empty never fired in 161 completed trials of those 30 functions.

**Results (v0.7.0, exception frames):**

| | `-O2` run (8 trials per function, 2 heap images) | UBSan run (same trials) |
|---|---|---|
| Functions tested | 5,745 | 5,745 |
| Trials | 45,959 | 45,960 |
| Completed and matching | 20,702 | 20,702 |
| … of which ran through an unlifted callee's original code | 858 (in 204 functions) | 858 |
| Matching except NaN against NaN | 1 | 1 |
| Faulted in both versions | 22,795 | 22,796 |
| Skipped (budget, a write into code, a call into non-code, the `fyl2x` case) | 2,461 | 2,461 |
| **Mismatches** | **0** | **0**, and no undefined behaviour |
| Functions with at least one completed, matching trial | 4,440 | 4,440 |
| … of the 20 exception-frame functions | 7 | 7 |

These results stand for v0.8.0: its generated code is byte-identical, less the two Bink helpers, which nothing lifted calls. The trials are the same as v0.6.0's for every function that was already lifted. The exercised functions are v0.6.0's, less `IsBadReadPtr` (now XAPI), plus 7 of the 20 exception-frame functions. In the `-O2` run one trial is missing, because a function hit the harness's time cap; in the UBSan run that trial faulted in both.

**Planted bugs in the exception frames:** an epilogue that restores the wrong register into `fs:[0]` was caught in all 6 of the 19 functions it applies to that completed a trial. A prologue that never links its record was caught in 1 of 20. Without a throw, a missing link is visible only to a callee that reads the chain.

**Results (v0.6.0, slice 3):**

| | `-O2` run (8 trials per function, 2 heap images) | UBSan run (same trials) |
|---|---|---|
| Functions tested | 5,726 | 5,726 |
| Trials | 45,808 | 45,808 |
| Completed and matching | 20,695 | 20,695 |
| … of which ran through an unlifted callee's original code | 861 (in 204 functions) | 861 |
| Matching except NaN against NaN | 1 | 1 |
| Faulted in both versions | 22,659 | 22,659 |
| Skipped (budget, a write into code, a call into non-code, the `fyl2x` case) | 2,453 | 2,453 |
| **Mismatches** | **0** | **0**, and no undefined behaviour |
| Functions with at least one completed, matching trial | 4,434 | 4,434 |
| … of the 628 x87 functions | 511 | 511 |

Fewer trials ran through original callee code than in v0.5.0, because the 628 functions that were stubs then are lifted now.

**x87 run with float-heavy heaps** (the 628 x87 functions, 16 trials each over 4 heap images): 10,047 trials, 3,919 completed and matching, **0 mismatches**. 540 of the 628 functions completed at least one matching trial.

**x87 run with the portable transcendentals (v0.9.0)** (the 626 x87 functions, 16 trials each over 4 heap images): 10,015 trials, 3,908 completed and matching, **0 mismatches**. 540 functions completed at least one matching trial. One trial is missing because a function hit the time cap.

The first x87 run (616 functions) had 19 failing trials in 4 functions. None was a lifter bug: they were the three emulator deviations above.

**For the C runtime (v0.11.0):**
- **Irregular contracts get matching trials.** The shared epilogues that pop a word of their caller's frame get that word below the return address. The helpers that take their operands on the x87 stack (`_ftol`, `_CIpow`, `_CIacos` …) always start with them (2 for `pow`), half the time from a list of ordinary values. Both lists (`GLUE_POPS`, `X87_ARGS`) sit next to `EmuFixups`.
- **Three more emulator deviations.** `fst`/`fstp st(i)` into an empty register leaves it tagged empty in Unicorn, where Intel tags it valid (the CRT's maths ends with `fstp st1` on a one-value stack): corrected. `fnsave` stores the last instruction and operand pointers, which the lifted code stores as 0: zeroed in Unicorn. After each x87 instruction of the CRT's maths, a register value outside double's exponent range is a known limitation of double registers (`pow`'s underflow and overflow paths rescale such values by 2^1536): the trial is skipped. Game code never makes one.
- **A targeted `sprintf` check** (`tools/check/crt_printf.py`). Random states rarely form a valid format string, so this builds them: every conversion, flags, widths and precisions (literal and `*`), size prefixes, and doubles across the whole range, with rounding ties, subnormals, zeros, infinities and NaNs. Each case runs under both precisions through the same harness. It puts in place the float-formatting hooks that start-up sets (the raw image holds `_fptrap` there). **3,000 format strings, 3,000 matching**, for example `3.142|-1.500000e-007|1e+021|-10|ghoulies|00002.00` on both sides.

**Results (v0.11.0, CRT lifted):**

| | CRT functions (48 trials each) | Whole lifted set (8 trials each) |
|---|---|---|
| Functions tested | 265 | 6,008 |
| Trials | 12,631 | 36,048 |
| Completed and matching | 4,968 (192 through an unlifted callee) | 15,933 (355 through an unlifted callee) |
| Faulted in both versions | 5,832 | 18,105 |
| Skipped | 1,831 | 2,010 |
| **Mismatches** | **0** | **0** |
| Functions with at least one completed, matching trial | 171 | 4,434 |

**XAPI (v0.12.0):** 192 functions, 9,205 trials, 1,749 completed and matching, **0 mismatches**; the 14 CRT functions newly reached through XAPI: 671 trials, 0 mismatches. One more emulator deviation: Unicorn doesn't check the alignment of SSE's 16-byte memory operands, where the hardware raises #GP and the lifted code traps; the harness sends a misaligned one to an unmapped page, so the original faults too. With three pointer arguments, XGraphics' maths almost never gets aligned random ones, so `tools/check/xg_math.py` gives every argument an aligned block of floats (game-scale values with zeros, subnormals, infinities and NaNs): **7 functions, 4,200 trials, all matching** (one NaN against NaN set aside).

**XACT and XGraphics (v0.15.0):** XACT, 332 functions: 7,950 trials, 1,029 completed and matching, **0 mismatches**. XGraphics, 647 functions: 15,516 trials, 5,356 completed and matching, **0 mismatches**, with 533 functions exercised; the swizzler's three fast paths (one MMX) and `XGSwizzleRect` itself all completed matching trials (another 1,353 trials, 0 mismatches). The three DirectSound helpers: 1,584 trials, 1,230 matching, 0 mismatches. Most of XACT's functions are methods on its objects, so random arguments rarely make a valid object and only a third were exercised; the game running them is the rest of the check. Unicorn doesn't expose MMX registers, so they aren't compared; the swizzler's MMX values only leave it through memory, which is.

Before the fixes above, the CRT run found two real bugs, both fixed: `memmove`'s jump table (indices −7..0, see the negated-index analysis) and the `_input` fragmentation (see SEH scope tables). The other failures were the harness gaps and emulator deviations listed above. The 94 CRT functions never completed are mostly the heap, locks and streams, which call XAPI and the kernel.

**Results (v0.5.0, slice 2):**

| | `-O2` run (8 trials per function, 2 heap images) | UBSan run (same trials) |
|---|---|---|
| Functions tested | 5,098 | 5,098 |
| Trials | 40,784 | 40,784 |
| Completed and matching | 18,656 | 18,656 |
| … of which ran through an unlifted callee's original code | 1,222 (in 373 functions) | 1,222 |
| Matching except NaN against NaN | 1 | 1 |
| Faulted in both versions | 19,920 | 19,920 |
| Skipped (budget, a write into code, or a call into non-code) | 2,207 | 2,207 |
| **Mismatches** | **0** | **0**, and no undefined behaviour |
| Functions with at least one completed, matching trial | 3,923 | 3,923 |

1,175 functions faulted or hit the budget on every random state. Most of them follow chains of pointers, or call deep into the engine. They need structured inputs to be exercised; they aren't known to be wrong.

(v0.4.0, slice 1: 1,276 functions, 30,624 trials, 0 mismatches.)

**Bugs found by the harness, now fixed:**
- **Epilogue fragments** mistaken for functions. The fix is the stack-balance check.
- **A test-setup problem:** random pointers let the original overwrite its own running code. The fix is read-only code pages in Unicorn.
- **A `memcpy` from a null pointer** (0 bytes) in the harness itself, found by UBSan.
- **Slice 2, in the lifter and the CFG:**
  - a branch to the very next instruction (`jnz $+2`) left its label undefined;
  - stale tail-call entries for branch targets that are the function's own instructions;
  - two weak "entries" that the previous instruction falls into: `0x83540`, mid-function and matched by a constant in `.rdata`, and `0xC4000`, the return address of a call.
- **Slice 2, in the harness:**
  - CPU state leaked between trials (231 false mismatches). A trial whose random pointers send the original into data can change segment registers;
  - the two emulators kept separate x87/MXCSR state;
  - Unicorn's stop address was ignored for blocks it had already translated;
  - tail calls into shared epilogues stopped at the wrong place;
  - the worker processes ran out of memory as the emulators' translation caches grew.

## Code review (C++23 migration), fixes
- **Jump-table extents used the number of unique targets instead of the table's length,** and byte index tables weren't recorded at all. Gap seeding could therefore start "functions" inside switch data. Two such functions (`0x6B850`, `0x12FC54`, both table bytes) are gone now.
- **`cmp al, imm8` switch bounds weren't masked to the register width.** The decoder sign-extends the immediate, so byte-indexed switches with more than 127 cases fell back to the heuristic. There's now a unit test.
- **Gap statistics were keyed by section name,** so the four `BINKYUY2` sections reported one combined number four times.
- **Lifter accuracy:**
  - OF/AF after a shift by `cl` could be read stale without an error;
  - `sar` claimed OF was defined for counts > 1;
  - `stos`/`movs` with an F2 prefix weren't treated as `rep`.
- **Lifter efficiency:**
  - every exit stored all 8 GPRs and all 8 xmm registers;
  - `rep movs`/`rep stos` ran element by element even when a bulk copy or fill is equivalent.
- **Analysis efficiency:**
  - the decoder cache was a hash map of 220-byte entries;
  - the set of owned instructions was a `std::set`;
  - the misaligned-seed pass built a 530k-node `std::map`;
  - the summary had a dead coverage loop.
- **Robustness:**
  - the XBE header area is bounds-checked as a whole (section table, library table, certificate);
  - the loader no longer keeps a second copy of the file;
  - the harness looks functions up by binary search and runs at `-O2` rather than `-O1`.

## How we'll know everything is recompiled
The function and jump-table counts are **what we have found so far, not totals read from the game**. A retail XBE has no symbol table: nothing in the file says how many functions or switch tables there are. The real totals are fixed by the binary, but we can only approach them by accounting for every byte. So "done" is defined by these checks rather than by reaching a number:

1. **Every byte of the code we lift is accounted for.** It is either an instruction of a function, or a known kind of data: a jump table, a byte index table, alignment padding, or a constant. ✅ **Done in v0.4.0:** 0 unknown bytes in the game code, the Rare libraries and the EH funclets (`qilin coverage`). The 97.8% quoted before was for all of `.text`, including XAPI and the CRT, which aren't lifted.
2. **Every switch has a proven size.** ✅ **Done for lifted code:** every jump table in the game code and Rare libraries is bounded by a `cmp`, an `and` mask or the value-range analysis. 7 tables are still sized by scanning, all in XDK libraries that get replaced. The lifter also traps on any index outside a table.
3. **Every indirect jump is resolved.** ✅ **Done for lifted code:** the 9 that aren't switches are indirect tail calls through function pointers (`jmp [eax+0x14]`, `jmp eax`, `jmp [0x304EC4]`, two record tables in `.rdata`). They go through the indirect-call dispatcher, which traps on an unknown target.
4. **Every function lifts.** ✅ **Done in v0.8.0:** all 5,743 functions in the game code and Rare libraries lift, with zero failures. Three more in those ranges belong to Bink and are replaced with it; `qilin lift` fails if anything lifted can reach them. Since v0.11.0 the 265 CRT functions that lifted code can reach lift too, and `qilin lift` fails if one doesn't.
5. **Nothing turns up at runtime.** An indirect call to an address we don't have a function for traps with the address logged. A full playthrough (and the attract mode, menus and every level) with no such trap closes the list. The Ghoulies PC Recomp's seed list of functions found during play is an extra cross-check.

Checks 1–4 are static and will be reported by `qilin` itself. Check 5 needs the runtime (roadmap step 3) and a playable build.

## Known issues
1. **Static analysis can't prove that no function is missing from a region that is fully accounted for.** A function nobody calls directly, sitting where the CFG put another function's code, would only show up at runtime. Check 5 above (the indirect-call trap during a playthrough) is the backstop.
2. **The ARM64 host has not been differentially tested.** The generated code and the analysis are byte-identical across hosts, but the difftest runs on x86-64. Scalar SSE arithmetic now follows x86 NaN rules explicitly (`rt::addss` and friends), which was the known ARM difference. The difftest still needs to run on an ARM host.
3. **Unicorn propagates SSE NaNs by x87 rules,** so two-NaN cases can't be checked against it. Those trials are counted separately (`pass-nan`), and the rules are covered by unit tests instead.
4. **MXCSR is assumed to be the default** (round-to-nearest, no flush-to-zero). This must be confirmed against what the game or XDK sets at boot.
5. **Library sections have low coverage** (DSOUND 35%, WMADEC 36%). They get replaced, so this doesn't block anything.
6. **The Ghoulies PC Recomp seed list hasn't been used yet** as a cross-check (see `prior-art.md`).
7. **x87 transcendentals are correctly rounded doubles, not the x87's own results** (by design, v0.9.0). Before v0.9.0 they came from the host's maths library, so they could differ between hosts.
8. **Unicorn differs from the hardware** in seven known places (`rdtsc`, the `fcomi` family's OF/SF/AF, `fyl2x` with an operand ≤ 0, transcendentals from the host libm, `fst st(i)` into an empty register, `fnsave`'s pointer fields, which the lifted code doesn't track, and the alignment of SSE memory operands). The harness corrects or skips them; other deviations may exist that the trials haven't reached yet.
9. **The difftest can't exercise most of the exception-frame functions.** They're `std::string` and `std::vector` members that allocate, and allocation goes through XAPI to the kernel, which the harness doesn't have, so 13 of the 20 fault in both versions on every random state. The 7 that complete match, and a planted bug in the unlink was caught in every one that completed. A harness stand-in for the allocator would reach the rest.

## Next
1. **The runtime** (roadmap step 3). The recompiler core is complete: every function in the lifted set lifts, and the generated code is the same on every host.
2. **Native versions of hot CRT functions** (`memcpy`, `memset`, `strlen` …), once the runtime shows which are hot, each checked against the lifted original.
3. **Indirect-call dispatch** is a binary search over 6,214 entries, about 13 steps. A direct-mapped table indexed by address (16-byte granularity in the aligned regions) would make it one lookup. Do this when the runtime measures it.
