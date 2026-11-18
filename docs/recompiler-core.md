# Recompiler core: status (roadmap step 2)

Last updated 2026-11-18 (v0.5.0: slice 2, calls; every jump table in lifted code proven by value-range analysis; v0.4.0: every byte of the lifted code accounted for; earlier: C++23 migration and code review, validation pass, lifter slice 1, differential test).

## Language and toolchain
Everything is **C++23**: the recompiler, the runtime header and the generated code. The C++23 features in use are `std::expected` (XBE loader and lifter results), `std::print`/`std::format`, `std::ranges` algorithms, `std::string::contains`, `std::unreachable` and `std::bit_cast`.

Minimum compilers are GCC 14, Clang 19, Xcode 16 and Visual Studio 2022 17.10. CMake checks these at configure time. Clang 18 is excluded because it can't use libstdc++'s `std::expected`.

CI builds with warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow`, MSVC `/W4`) on five configurations:
- Linux x64 with GCC 14
- Linux x64 with Clang 19
- Linux ARM64 with GCC 14
- macOS ARM64
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
| Game | `0x11000–0x14A895` | yes | 1,215,701 | 24,907 | 43,637 | **0** |
| Rare engine libraries | `0x152780–0x165C00` | yes | 75,308 | 444 | 3,224 | **0** |
| C++ EH funclets | `0x1707B0–0x171020` | yes | 2,043 | 0 | 117 | **0** |

XAPI, the CRT and the XDK library sections (D3D, DSOUND, BINK and so on) aren't lifted: they get replaced. Their coverage is reported but doesn't gate anything.

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

**Lift check.** The lift report has no fragments left: every failure is something still to be implemented (x87 for slice 3, C++ exception frames, and one `pushfd`). In v0.3.0, 61 lift failures were fragments.

### Lifter, slices 1 and 2 (`recomp/src/lifter.*`, runtime `recomp/runtime/guest.hpp` and `dispatch.cpp`)
Each guest function becomes `void fn_XXXXXXXX(rt::Context&)`. `Lifter::lift` returns `std::expected<std::string, LiftFailure>`, where the failure carries a kind (fragment, unsupported instruction, x87, undefined flag, implausible, bad control flow), the address and the instruction.

Inside a lifted function:
- **Registers:** guest registers are host locals. The prologue declares only the registers the function references. Each exit stores back only the registers it modifies: the compiler can't drop those stores itself, because guest memory writes may alias the context.
- **Flags:** EFLAGS are separate bools. A backward liveness pass means a flag is computed only where something can read it. Reading a flag the previous instruction left undefined is a lift error, including OF/AF after a shift by `cl`. Instructions the lifter's own table doesn't list take their flag effects from Zydis's tables. Before, those were taken to touch no flags, so `fcomi`, `bt`, `bsf` and the rotates looked transparent. A check over every decodable address in the image found that the hand table agrees with Zydis everywhere, or is deliberately stricter: shift counts that may be zero, `rep` with ecx = 0, and AF after `sbb`.
- **Control flow:** blocks become labels and gotos. Jump tables become `switch` with a trap for an unknown index.
- **Calls (slice 2):**
  - **Direct `call T`:** push the guest return address onto the guest stack, store the modified registers back to the Context (`writeback()`), call `fn_T(c)` as a plain C++ call, reload the registers the function uses (`reload()`), and check that the callee returned to the call site. A callee that returns anywhere else traps. The stack layout is exactly the original's, so arguments at `[esp+4]` and callees that move their return address (`__SEH_prolog`, `_chkstk`) work unchanged.
  - **Tail calls:** a `jmp` or `jcc` out of the function writes back, calls `fn_T(c)` and returns.
  - **Indirect calls and jumps** (`call [eax+0x14]`, `jmp eax`, the two record tables in `.rdata`) go through `rt::call(c, target)`. It binary-searches the generated function table, then falls back to `rt::call_unlifted`. That fallback belongs to whatever links the code: high-level replacements in the runtime, the original bytes in the differential test, otherwise a trap with the address.
  - **Stubs:** every call or tail-call target without a lifted body gets a generated `fn_T` that forwards to `rt::call_unlifted`, so all direct calls link. There are 699: 448 game and Rare-library functions waiting on x87 or exception handling, and 251 library entry points the game calls directly. Those 251 are the first concrete list of what the runtime must replace: XAPI 71, D3D 87, CRT 36, XACT 25, D3DX 11, BINK 7, XPP 7, XGRPH 6, DSOUND 1.
  - **Flags** aren't carried across calls. A flag read after a call before any write is a lift error, never a guess. No function in the game does this.
  - **Direction flag:** tracked statically per instruction. It's clear on entry and after every call (the MSVC ABI), set by `std` and cleared by `cld`. String instructions run in that direction, and DF must be clear at every call and exit. The Rare `memcpy` uses `std; rep movsd; cld` for overlapping copies.
  - **The stack-balance check** treats esp as unknown after a call, since a callee ending in `ret n` pops its arguments too.
- **Integer ops:**
  - mov/movzx/movsx/lea/xchg;
  - add/sub/adc/sbb/cmp/and/or/xor/test/inc/dec/neg/not;
  - shl/shr/sar;
  - imul/mul/div/idiv (traps on #DE);
  - cdq/cwde, setcc/cmovcc, lahf/sahf;
  - push/pop/leave/ret.
- **String ops:** `rep stos`/`rep movs` (8, 16 and 32-bit, with an F2 or F3 prefix) run as whole runs. They use `memset`/`memmove` where that's equivalent, and an element-wise copy only when the destination overlaps the source run in the direction of copying, which is exactly the CPU's pattern-replicating behaviour. Backward copies (DF = 1) have their own helper. `cmps` with `repe`/`repne` updates the flags only if at least one element was compared, as the CPU does when ecx is 0.
- **Scalar SSE**, bit-exact on lane 0:
  - movss/movaps/movups;
  - add/sub/mul/div/min/max/sqrtss;
  - the and/or/xor/andn packed logic ops;
  - comiss/ucomiss;
  - cvtsi2ss/cvt(t)ss2si, with x86 overflow semantics.

  **NaN results follow x86 rules on every host.** A NaN operand propagates, quietened; if both are NaN, the first operand's. An invalid operation on non-NaN operands (inf − inf, 0 × inf, 0 / 0, the square root of a negative) gives the default NaN `0xFFC00000`. `rt::addss` and its siblings check the result with one compare, since NaN results are rare. Before, the result depended on the host: ARM's default NaN is `0x7FC00000`, and a C++ compiler may swap the operands of `a + b`, which changes which NaN propagates. Unit tests cover the rules.

  `guest.hpp` turns off multiply-add contraction for Clang and MSVC and refuses `-ffast-math`. The `qilin_runtime` CMake target adds `-ffp-contract=off` for GCC.
- **Stack-balance check:** a forward esp/ebp analysis rejects "functions" that are fragments of other functions: they return with esp off balance or pop their own return address.

**Coverage today:** `qilin lift` covers the game code (`0x11000–0x14A895`) and the Rare engine libraries (`0x152780–0x165C00`), 5,747 functions in all.

| | v0.4.0 (slice 1) | v0.5.0 (slice 2) |
|---|---|---|
| **Lifted** | 1,276 | **5,098 (88.7%)** |
| Not leaves (needed calls) | 4,422 | 0 |
| Use x87 (slice 3) | 48 | 628 |
| C++ exception frames (`fs:[0]`) | — | 20 |
| One-off (`pushfd` in a CPU-ID probe) | 2 | 1 |
| Fragments of other functions | 0 | 0 |

Most x87 users used to be counted as "not a leaf", because they also make calls.

The generated code compiles with no warnings at `-Wall -Wextra -Werror` under GCC 14 and Clang 19.

### Differential test (`tools/difftest/`)
`difftest.py` lifts everything and builds a shared library from it. The build uses production settings (`-O2`, `-ffp-contract=off`, warnings as errors) plus bounds-checked, write-traced memory. It then runs each function in many trials.

**Each trial:**
1. Build one random guest state: registers, xmm registers, stack arguments, and a 4 MB scratch heap full of pointers, small ints and floats.
2. Run the original x86 bytes in Unicorn, with the code pages read-only. The original runs its whole call tree.
3. Run the lifted C++ on an identical copy of that state.
4. Compare the results bit for bit: every general register, esp, all 8 xmm registers (all lanes), every written byte, and the return address. If the original faults, the lifted code must trap too.

**Calls.** Lifted callees are called directly. A callee with no lifted body (library code, or game code waiting on x87) runs from its original bytes in a second Unicorn instance that maps the lifted side's memory, so whole call trees are compared whatever their callees are. A called callee stops at the return address at `[esp]`. A tail-called one (marked in test builds, since it may be the middle of an epilogue with saved registers at `[esp]`) stops at the tail-calling function's own return address.

**Isolation between trials:** both emulators restore a saved CPU context before every trial, covering segments, control registers, x87 and MXCSR. x87 and MXCSR state carry over between callees within a trial, as in the original. Each chunk of 120 functions runs in a fresh process.

**Runaway guards** (`QILIN_TEST_HOOKS`, compiled out of normal builds): every lifted function opens a frame and every backward branch ticks. The harness stops a trial at a call depth of 4,096, at 2 million calls, 20 million loop iterations or 50,000 unlifted calls, or after 10 seconds, and reports it as a mismatch. The original's run is capped at 200,000 instructions, so a correct lifted run stays far below all of these.

**Set aside, not failed:**
- **NaN against NaN.** Unicorn propagates SSE NaNs by x87 rules (larger payload wins), real SSE by operand order. Trials whose only differences are NaN against NaN, in an xmm lane or an aligned memory word, are counted as `pass-nan`.
- **Calls into non-code.** A call through a null or random pointer makes the original execute data, which depends on CPU state the lifted side doesn't carry (flags, segments). Those trials are skipped.

**Mutation check:** a lifter with the post-call `reload()` removed was caught as mismatches in 17 of 100 sampled functions. Without the runaway guards, that build had crashed or hung the harness instead.

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
4. **Every function lifts.** The lift report must show zero failures. Today 5,098 of 5,747 functions in the game code and Rare libraries lift (88.7%). The rest are 628 x87 users (slice 3), 20 functions with C++ exception frames, and one CPU-ID probe (`pushfd`) that the runtime will replace. None is a fragment.
5. **Nothing turns up at runtime.** An indirect call to an address we don't have a function for traps with the address logged. A full playthrough (and the attract mode, menus and every level) with no such trap closes the list. The Ghoulies PC Recomp's seed list of functions found during play is an extra cross-check.

Checks 1–4 are static and will be reported by `qilin` itself. Check 5 needs the runtime (roadmap step 3) and a playable build.

## Known issues
1. **Static analysis can't prove that no function is missing from a region that is fully accounted for.** A function nobody calls directly, sitting where the CFG put another function's code, would only show up at runtime. Check 5 above (the indirect-call trap during a playthrough) is the backstop.
2. **The ARM64 host has not been differentially tested.** The generated code and the analysis are byte-identical across hosts, but the difftest runs on x86-64. Scalar SSE arithmetic now follows x86 NaN rules explicitly (`rt::addss` and friends), which was the known ARM difference. The difftest still needs to run on an ARM host.
3. **Unicorn propagates SSE NaNs by x87 rules,** so two-NaN cases can't be checked against it. Those trials are counted separately (`pass-nan`), and the rules are covered by unit tests instead.
4. **MXCSR is assumed to be the default** (round-to-nearest, no flush-to-zero). This must be confirmed against what the game or XDK sets at boot.
5. **Library sections have low coverage** (DSOUND 35%, WMADEC 36%). They get replaced, so this doesn't block anything.
6. **The Ghoulies PC Recomp seed list hasn't been used yet** as a cross-check (see `prior-art.md`).

## Next
1. **Slice 3: x87.** 628 functions. Use host `double`, honour precision control where the game sets it, and keep x87 state in the Context across calls.
2. **C++ exception frames:** 20 functions write `fs:[0]`. The strategy is still to be chosen: map to host C++ exceptions, or keep an explicit frame chain.
3. **Indirect-call dispatch** is a binary search over 5,098 entries, about 13 steps. A direct-mapped table indexed by address (16-byte granularity in the aligned regions) would make it one lookup. Do this when the runtime measures it.
