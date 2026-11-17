# Recompiler core: status (roadmap step 2)

Last updated 2026-11-17 (validation pass, lifter slice 1, differential test).

## Done

### XBE loader (`recomp/src/xbe.*`)
It parses the image header, certificate, section table, library versions and kernel thunk table, and it builds the flat guest image (BSS zero-filled). It throws on anything malformed.

**Check:** `qilin info` output is identical, field for field, to the Python prototype's `xbeinfo.json`.

### Decoder (`recomp/src/decoder.*`)
This is Zydis v4.1.1 in 32-bit legacy mode, vendored. Instructions are cached in a compact form, about 100 bytes each, so all ~530k reachable instructions fit comfortably in memory.

**Cross-check against LLVM 18** (`tools/check/verify_decoder.py`):

| Set | Instructions | Result |
|---|---|---|
| Every reachable instruction | 532,837 | **100% agreement** on length and mnemonic |
| Linear sweep of every code section, data included | 660,357 | No unexplained disagreement; 238 known-benign cases, all in data bytes |

The 238 known-benign cases are:
- LLVM rejects undocumented aliases that Zydis accepts (`SAL /6`, `TEST /1`, `int1`, `bound`, ...).
- LLVM returns a lone `lock` prefix.
- Zydis rightly rejects encodings a Pentium III can't run (`mov cs`, EVEX).

The naming differences between the two libraries are mapped explicitly in the script. Examples: `fcomip`/`fcompi`, `popad`/`popal`, and Zydis's `ret far` versus LLVM's `retf`.

### Function discovery + CFG (`recomp/src/cfg.*`)
This is a C++ port of `tools/analysis/cfg.py` and `analyze.py`. It works on Zydis's structured operands instead of disassembly text. It takes about 1 second on x86-64.

| | Python prototype | C++ port |
|---|---|---|
| Functions | 9,402 | 9,286 |
| Reachable instructions | 530,764 | 532,837 |
| `.text` coverage of non-padding bytes | 97.7% | 98.1% |
| Jump tables resolved | 556 | 558 (313 cmp, 200 cmp + byte table, 45 heuristic) |
| Unresolved indirect jumps | 24 | 29 |
| Indirect calls | ~1,960 | 1,993 |

**Where the two disagree, the port is better in every case examined:**
- **Indexed table addresses are no longer code-pointer seeds.** The prototype's text regex treated the table address in `jmp [4*reg + table]` as a possible function pointer, so jump tables were decoded as code.
- **Misaligned seeds are pruned.** Some data values are 16-aligned and look like code pointers, but land inside another function's instruction. For example, `0x80000` is the `CC` byte inside `mov ecx, [0x4275CC]`. 47 such seeds are dropped, iterating until stable.
- **Fewer split functions.** 146 of the 154 prototype-only "functions" are instructions inside a larger function in the port. The one checked by hand, `0x26F60`, is an ordinary branch target that the prototype split off because of its seeding order. Three of the remaining eight were checked and are mid-instruction starts.

**Checks:**
- On ARM64 (GCC cross-build, run under qemu), the output is byte-identical to x86-64 (Clang).
- Both compilers build it with no warnings at `-Wall -Wextra -Wpedantic`.

### Validation pass (`Cfg::validate`)
This pass removes functions that are really data decoded as code, and repeats until nothing changes. A function is dropped when any of these holds:
- **Its entry lies inside another function's instruction.** The side with weaker evidence is dropped: a guessed seed loses to a direct call target.
- **It fails to decode or has overlapping instructions.**
- **It contains an instruction MSVC-compiled retail code never contains:** far control flow, privileged instructions or port I/O, `int`/`int1`, BCD, `bound`/`arpl`, segment-register writes, 16-bit addressing, undocumented `SAL /6`/`TEST /1`, or anything outside the Pentium III's instruction sets (base, x87, MMX, SSE).

Functions that are the entry point, or direct call/jump targets of surviving functions, are kept but listed by `qilin suspects`.

**Result:**
- 9,199 functions.
- 0 decode failures and 0 overlaps.
- 134 data-as-code functions dropped.
- 5 suspects remain, all hardware code in XAPI (`int 0x2D`, far jump) and D3D (`wbinvd`, port I/O). Those libraries get replaced, so none of this is lifted.

### Lifter, slice 1 (`recomp/src/lifter.*`, runtime `recomp/runtime/guest.hpp`)
Each guest function becomes `void fn_XXXXXXXX(rt::Context&)`. Inside it:
- **Registers:** guest registers are host locals, loaded at entry and stored at every exit.
- **Flags:** EFLAGS are separate bools. A backward liveness pass means a flag is computed only where something can read it. Reading a flag the previous instruction left undefined is a lift error.
- **Control flow:** blocks become labels and gotos. Jump tables become `switch` with a trap for an unknown index.
- **Integer ops:** mov/movzx/movsx/lea/xchg; add/sub/adc/sbb/cmp/and/or/xor/test/inc/dec/neg/not; shl/shr/sar; imul/mul/div/idiv (traps on #DE); cdq/cwde; setcc/cmovcc; lahf/sahf; push/pop/leave/ret; and rep stos/movs.
- **Scalar SSE**, bit-exact on lane 0: movss/movaps/movups, add/sub/mul/div/min/max/sqrtss, the and/or/xor/andn packed logic ops, comiss/ucomiss, and cvtsi2ss/cvt(t)ss2si with x86 overflow semantics.
- **Stack-balance check:** a forward esp/ebp analysis rejects "functions" that are fragments of other functions. These are "functions" that return with esp off balance or pop their own return address.

Anything unsupported is an error naming the instruction.

**Coverage today:**
- **1,243 functions lifted.** That is every leaf function in the game code (`0x11000–0x14A895`) except:
  - 49 fragments;
  - 22 that use x87 (slice 2);
  - 7 that read flags on entry (also fragments);
  - a handful of one-offs (`pushfd`/`popfd`, `scasd`, `sti`, an `fs:` access).
- **4,380 non-leaf functions are next.** They need calls and the function table.
- **Portability:** the generated code compiles with no warnings under GCC for ARM64 at `-Wall -Wextra`, and under Clang for x86-64.

### Differential test (`tools/difftest/`)
`difftest.py` lifts everything, builds a shared library with bounds-checked and write-traced memory, and runs each function in many trials.

**Each trial:**
1. Build one random guest state: registers, xmm registers, stack arguments, and a 4 MB scratch heap full of pointers, small ints and floats.
2. Run the original x86 bytes in Unicorn, with the code pages read-only.
3. Run the lifted C++ on an identical copy of that state.
4. Compare the results bit for bit: every general register, esp, all 8 xmm registers (all lanes), every written byte, and the return address. If the original faults, the lifted code must trap too.

**Result** (8 trials per function, 2 heap images):

| | |
|---|---|
| Functions tested | 1,243 |
| Trials | 9,944: 5,969 completed, 3,831 faulted in both versions, 144 skipped (hit the instruction or time budget, or wrote into code) |
| **Mismatches** | **0** |
| Functions with at least one completed, matching trial | 1,115 |
| Functions never exercised | 128 |

The 128 never-exercised functions faulted on every random state, typically because they follow a chain of pointers. They need structured inputs to be exercised; they aren't known to be wrong.

**Bugs found by the harness during development, now fixed:**
- **Epilogue fragments** mistaken for functions. The fix is the stack-balance check.
- **A test-setup problem:** random pointers let the original overwrite its own running code. The fix is read-only code pages in Unicorn.

## Known issues

1. **About 56 "functions" are epilogue fragments of other functions.** These are the 49 stack-imbalanced ones plus the 7 that read flags on entry. The lifter now rejects them, but the CFG should fold them back into their parent functions. They are seeded as call targets or data pointers by the linear pass, so the seeding should be checked against direct-call evidence.
2. **The ARM64 host has not been differentially tested.** The generated code compiles for ARM64, but the tests run on an x86-64 host. NaN payloads differ on ARM: the default NaN is `0x7FC00000` there, not `0xFFC00000`. The fix is to canonicalise NaN results in `rt::` helpers where the game could observe them, and to run the difftest under an ARM host.
3. **MXCSR is assumed to be the default** (round-to-nearest, no flush-to-zero). This must be confirmed against what the game or XDK sets at boot.
4. **Library sections have low coverage** (DSOUND 35%, WMADEC 37%). They get replaced, so this doesn't block anything.
5. **The Ghoulies PC Recomp seed list hasn't been used yet** as a cross-check (see `prior-art.md`).

## Next

1. **Slice 2: calls.** Direct calls become C++ calls, with full register state passed through the context. That needs the generated function table and an indirect-call trap on unknown targets, plus tail jumps.
2. **Slice 3: x87.** Use host `double`, and honour precision control where the game sets it.
3. **Fold epilogue fragments** back into their parent functions in the CFG.
4. **SEH/C++ EH strategy** for the 63 `fs:` sites.
