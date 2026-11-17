# Recompiler core: status (roadmap step 2)

Last updated 2026-11-17 (C++23 migration and code review, validation pass, lifter slice 1, differential test).

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

| | Python prototype | C++ port, v0.3.0 | C++ port, v0.4.0 |
|---|---|---|---|
| Functions | 9,402 | 9,197 | 8,843 |
| Reachable instructions | 530,764 | 529,718 | 530,444 |
| Lifted code accounted for (game, Rare libraries, EH funclets) | — | — | **100%**, 0 unknown bytes |
| Jump tables resolved | 556 | 553 | 558 (307 cmp, 202 cmp + byte table, 3 `and` mask, 46 heuristic) |
| Unresolved indirect jumps | 24 | 26 | 26 (7 in lifted code, all indirect tail calls) |
| Indirect calls | ~1,960 | 1,980 | 1,930 |

There are fewer functions in v0.4.0 because 2,834 fragments of real functions were merged back into their parents, while about 1,300 functions were newly found.

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
- 8,843 functions.
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
- **Fragment merging.** A function is cut short when exploration stops at a false candidate. If function G jumps past such candidates and none of them has strong evidence, they are labels inside G: they are dropped and G is re-explored. Weak evidence stands unless G's own code falls through into the candidate, or jumps to it over alignment filler. 2,834 fragments were merged this way.
- **Labels.** A "function" whose entry is an instruction of an earlier function, reached only by jumps (no call, no EH entry, no pointer), is a label that an earlier, shorter exploration mistook for a tail call. The same goes for a 16-aligned loop head that its function reaches with `jmp short` over alignment filler.
- **Takeover.** An aligned data or immediate pointer whose code runs through weaker functions' entries (orphan or gap seeds) takes them over: those entries are labels inside it.
- **Misaligned overlaps.** When one function's entry falls inside another's instruction, the one with weaker evidence goes. Before v0.4.0 the inner seed always lost, which threw away a real function at `0xD3310` in favour of a jump table decoded as an orphan.
- **Switch bounds.** The backward search for a jump table's bound follows the index register through `movzx` and `mov reg, reg` and stops when the register is overwritten. It accepts `and reg, 2^k - 1` as a bound. A table entry may not land on another function's start, and byte index tables are clipped against their dword table.
- **Fixpoint.** Immediate seeding, fragment merging, validation, label dropping and re-exploration repeat until nothing changes (at most 8 rounds).

**Lift check.** The lift report has no fragments left: every failure is an instruction class still to be implemented (calls for slice 2, x87 for slice 3, `std`, `pushfd`). In v0.3.0, 61 lift failures were fragments.

### Lifter, slice 1 (`recomp/src/lifter.*`, runtime `recomp/runtime/guest.hpp`)
Each guest function becomes `void fn_XXXXXXXX(rt::Context&)`. `Lifter::lift` returns `std::expected<std::string, LiftFailure>`, where the failure carries a kind (not a leaf, fragment, unsupported instruction, x87, undefined flag, implausible, bad control flow), the address and the instruction.

Inside a lifted function:
- **Registers:** guest registers are host locals. The prologue declares only the registers the function references. Each exit stores back only the registers it modifies: the compiler can't drop those stores itself, because guest memory writes may alias the context.
- **Flags:** EFLAGS are separate bools. A backward liveness pass means a flag is computed only where something can read it. Reading a flag the previous instruction left undefined is a lift error, including OF/AF after a shift by `cl`. Instructions the lifter's own table doesn't list take their flag effects from Zydis's tables. Before, those were taken to touch no flags, so `fcomi`, `bt`, `bsf` and the rotates looked transparent. A check over every decodable address in the image found that the hand table agrees with Zydis everywhere, or is deliberately stricter: shift counts that may be zero, `rep` with ecx = 0, and AF after `sbb`.
- **Control flow:** blocks become labels and gotos. Jump tables become `switch` with a trap for an unknown index.
- **Integer ops:**
  - mov/movzx/movsx/lea/xchg;
  - add/sub/adc/sbb/cmp/and/or/xor/test/inc/dec/neg/not;
  - shl/shr/sar;
  - imul/mul/div/idiv (traps on #DE);
  - cdq/cwde, setcc/cmovcc, lahf/sahf;
  - push/pop/leave/ret.
- **String ops:** `rep stos`/`rep movs` (with an F2 or F3 prefix) run as whole runs. They use `memset`/`memmove` where that's equivalent, and an element-wise forward copy only when the destination overlaps ahead of the source, which is exactly the CPU's pattern-replicating behaviour.
- **Scalar SSE**, bit-exact on lane 0:
  - movss/movaps/movups;
  - add/sub/mul/div/min/max/sqrtss;
  - the and/or/xor/andn packed logic ops;
  - comiss/ucomiss;
  - cvtsi2ss/cvt(t)ss2si, with x86 overflow semantics.

  `guest.hpp` turns off multiply-add contraction for Clang and MSVC and refuses `-ffast-math`. The `qilin_runtime` CMake target adds `-ffp-contract=off` for GCC.
- **Stack-balance check:** a forward esp/ebp analysis rejects "functions" that are fragments of other functions: they return with esp off balance or pop their own return address.

**Coverage today:** `qilin lift` covers the game code (`0x11000–0x14A895`) and the Rare engine libraries (`0x152780–0x165C00`), 5,748 functions in all.

| | Functions |
|---|---|
| **Lifted** | **1,276** |
| Not leaves (need calls: slice 2) | 4,422 |
| Use x87 (slice 3) | 48 |
| One-offs (`std`, `pushfd`) | 2 |
| Fragments of other functions | **0** (61 in v0.3.0) |

Fewer functions lift than in v0.3.0 (1,373) because merged fragments now belong to their parents, and most of those parents make calls.

The generated code compiles with no warnings at `-Wall -Wextra` under GCC 14 for both x86-64 and ARM64.

### Differential test (`tools/difftest/`)
`difftest.py` lifts everything and builds a shared library from it. The build uses production settings (`-O2`, `-ffp-contract=off`, warnings as errors) plus bounds-checked, write-traced memory. It then runs each function in many trials.

**Each trial:**
1. Build one random guest state: registers, xmm registers, stack arguments, and a 4 MB scratch heap full of pointers, small ints and floats.
2. Run the original x86 bytes in Unicorn, with the code pages read-only.
3. Run the lifted C++ on an identical copy of that state.
4. Compare the results bit for bit: every general register, esp, all 8 xmm registers (all lanes), every written byte, and the return address. If the original faults, the lifted code must trap too.

**Results:**

| | `-O2` run (24 trials per function, 3 heap images) | UBSan run (24 trials per function) |
|---|---|---|
| Functions tested | 1,276 | 1,276 |
| Trials | 30,624 | 30,624 |
| Completed | 19,601 | 19,601 |
| Faulted in both versions | 10,480 | 10,480 |
| Skipped (hit the budget, or wrote into code) | 543 | 543 |
| **Mismatches** | **0** | **0**, and no undefined behaviour |
| Functions with at least one completed, matching trial | 1,258 | 1,258 |

18 functions faulted on every random state in the `-O2` run, typically because they follow a chain of pointers. They need structured inputs to be exercised; they aren't known to be wrong.

**Bugs found by the harness, now fixed:**
- **Epilogue fragments** mistaken for functions. The fix is the stack-balance check.
- **A test-setup problem:** random pointers let the original overwrite its own running code. The fix is read-only code pages in Unicorn.
- **A `memcpy` from a null pointer** (0 bytes) in the harness itself, found by UBSan.

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
2. **Every switch has a proven size.** Of the 558 jump tables, 46 are sized heuristically. They need a bounding `cmp`, or the lifter must trap on any index outside the table, which it already does.
3. **Every indirect jump is resolved.** 7 remain in lifted code, and all are indirect tail calls through function pointers (`jmp [eax+0x14]`, `jmp eax`, `jmp [0x304EC4]`), not switches. Slice 2's indirect-call dispatcher handles them.
4. **Every function lifts.** The lift report must show zero failures. Today 1,276 of 5,748 functions in the game code and Rare libraries lift. The rest are waiting on calls (slice 2), x87 (slice 3) or two one-off instructions; none is a fragment.
5. **Nothing turns up at runtime.** An indirect call to an address we don't have a function for traps with the address logged. A full playthrough (and the attract mode, menus and every level) with no such trap closes the list. The Ghoulies PC Recomp's seed list of functions found during play is an extra cross-check.

Checks 1–4 are static and will be reported by `qilin` itself. Check 5 needs the runtime (roadmap step 3) and a playable build.

## Known issues
1. **Static analysis can't prove that no function is missing from a region that is fully accounted for.** A function nobody calls directly, sitting where the CFG put another function's code, would only show up at runtime. Check 5 above (the indirect-call trap during a playthrough) is the backstop.
2. **The ARM64 host has not been differentially tested.** The generated code and the analysis are byte-identical across hosts, but the difftest runs on x86-64. NaN payloads differ on ARM: the default NaN is `0x7FC00000` there, not `0xFFC00000`. The fix is to canonicalise NaN results in `rt::` helpers where the game could observe them, and to run the difftest on an ARM host.
3. **MXCSR is assumed to be the default** (round-to-nearest, no flush-to-zero). This must be confirmed against what the game or XDK sets at boot.
4. **Library sections have low coverage** (DSOUND 35%, WMADEC 36%). They get replaced, so this doesn't block anything.
5. **The Ghoulies PC Recomp seed list hasn't been used yet** as a cross-check (see `prior-art.md`).

## Next
1. **Slice 2: calls.** Direct calls become C++ calls, with full register state passed through the context. That needs the generated function table and an indirect-call trap on unknown targets, plus tail jumps.
2. **Slice 3: x87.** Use host `double`, and honour precision control where the game sets it.
3. **SEH/C++ EH strategy** for the 63 `fs:` sites.
