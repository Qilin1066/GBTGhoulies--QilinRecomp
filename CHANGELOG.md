# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

## [0.5.0] - 2026-11-18
Lifter slice 2 (calls), and every jump table in lifted code proven. 5,098 of the 5,747 functions in the game code and Rare libraries lift now (88.7%), up from 1,276.

### Added
- **Value-range analysis for jump tables** (`recomp/src/vrange.*`). It tracks each register's possible values along every path through a function and narrows them at conditional branches. Every jump table in the game code and the Rare libraries now has a proven size: 35 by the new analysis, and the rest by a `cmp` or an `and` next to the jump. The 7 still sized by scanning are all in XDK libraries that get replaced. On the 311 tables already bounded by `cmp` or `and`, the analysis agrees.
- **Jump tables with a negative first index,** for MSVC's `sub ecx, 4; jb` … `jmp [4*ecx + T]` form, used in `memcpy`.
- Unit tests for a register-held bound a block away from the jump, adjacent tables, and a negative-index table, lifted to C++.
- **Calls in lifted code.**
  - **Direct calls** push the guest return address, store modified registers back to the Context, call `fn_T(c)` as a plain C++ call, reload the registers, and trap if the callee returned anywhere but the call site.
  - **Tail calls:** a `jmp` or `jcc` out of the function.
  - **Indirect calls and jumps** go through `rt::call` (`recomp/runtime/dispatch.cpp`). It binary-searches the generated function table and falls back to `rt::call_unlifted`, which the host provides: HLE in the runtime, original code in the difftest, otherwise a trap with the address.
  - **Generated files:** `qilin lift` now writes `functions.hpp` (declarations) and `stubs.cpp`. That's one stub per call target without a lifted body, 699 in all: 448 game and Rare functions waiting on x87 or exception handling, and 251 library entry points. Those 251 are the first concrete list of what the runtime must replace (XAPI 71, D3D 87, CRT 36, XACT 25, D3DX 11, BINK 7, XPP 7, XGRPH 6, DSOUND 1).
- **String instructions:** `movsw`, `stosw` and `cmps` with `repe`/`repne`. `cmps` leaves the flags untouched when ecx is 0, as the CPU does.
- **Direction flag** tracked statically (`std` … `cld`), with backward `rep movs` for the Rare `memcpy`'s overlapping copies. DF must be clear at every call and exit.
- **Scalar SSE arithmetic follows x86 NaN rules on every host** (`rt::addss` and friends): the first NaN operand propagates, quietened, and invalid operations give `0xFFC00000`. Before, the result depended on the host CPU and on the compiler's operand order. Unit tests cover the rules.
- **Differential test with calls.**
  - **Coverage:** whole call trees are compared. A callee without a lifted body runs from its original bytes in a second Unicorn instance that shares the lifted side's memory.
  - **Results:** 5,098 functions, 40,784 trials, 0 mismatches, also under UBSan. 1,222 matching trials ran through original callee code.
- **Test hooks** (`QILIN_TEST_HOOKS`, compiled out otherwise): a frame per lifted function and a tick per backward branch. The harness uses them to stop runaway recursion and loops, and to place tail-called callees' returns.
- Difftest options `--test` (test chosen functions from the full lift) and `--reuse` (skip the rebuild).

### Changed
- **The difftest restores a saved CPU context in both emulators before every trial,** and runs each chunk of 120 functions in a fresh process.
- **Trials whose only differences are NaN against NaN are counted as `pass-nan`:** Unicorn propagates SSE NaNs by x87 rules.
- **Trials that call into non-code** (null or random pointers) are skipped.
- **A flag read after a call is reported as a lift error,** since callee flags aren't modelled. No function in the game does this.

### Fixed
- **Jump tables that were sized by scanning took in the next table.** 15 tables at `0x108xxx` were read as 10 slots instead of 5, and `0x16322D` as 14 instead of 10. That gave those functions phantom switch cases.
- **Two jumps through tables of records in `.rdata`** (`0xD410B`, `0xD4179`) were treated as switches. They're indirect jumps now.
- **String instructions were taken to overwrite every register** when searching backwards for a switch bound. They write only esi, edi, ecx (with `rep`) and, for `lods`, eax.
- **A branch to the next instruction (`jnz $+2`)** left its label undefined in the generated code.
- **`if ((sf == of))` in generated code** tripped Clang's `-Wparentheses-equality`. All generated code now compiles warning-free under GCC 14 and Clang 19.
- **CFG:** tail-call entries for branch targets that are the function's own instructions are dropped.
- **CFG:** a weakly evidenced "entry" that the previous instruction falls into is a label, not a function: `0x83540`, matched by a constant in `.rdata`, and `0xC4000`, a call's return address.
- **Difftest:** CPU state leaked between trials after the original executed data (231 false mismatches).
- **Difftest:** the two emulators kept separate x87/MXCSR state.
- **Difftest:** Unicorn ignored a new stop address for blocks it had already translated.
- **Difftest:** tail calls into shared SEH epilogues stopped at the wrong place.
- **Difftest:** workers ran out of memory.
- **MSVC:** `rt::divss` tripped warning C4723 ("potential divide by 0") wherever a constant zero divisor was inlined. IEEE division by zero is intended there.


### Documentation
- **The renderer targets Vulkan 1.3.** It's widely supported: the Raspberry Pi 4 and 5 (Mesa v3dv, conformant from Mesa 24.3), macOS through MoltenVK 1.3, and current Windows and Linux drivers. 1.3 core is the floor; optional features and limits follow what v3dv on a Pi 4 provides. See `roadmap-and-design-targets.md`.

## [0.4.0] - 2026-11-17
Every byte of the lifted code is accounted for.

### Added
- **`qilin coverage <xbe> [--list]`** sorts every byte of each text region into code, tables, padding or unknown. It exits with code 3 while any byte of a lifted region is unknown. Today there are none: the game code, the Rare libraries and the C++ EH funclets are at 100%, down from 15,198 unknown bytes in the game code.
- **C++ exception handling tables are parsed** (`FuncInfo`, unwind maps, try blocks, catch handlers and `__ehhandler` stubs): 70 tables, 68 stubs, 147 unwind funclets and 20 catch continuations. The funclets have their own lifted region, `0x1707B0–0x171020`.
- **Function discovery rules grounded in MSVC's layout:**
  - 16-byte function alignment in the game and Rare library regions;
  - strong evidence (calls, entry point, EH tables, tail jumps that land after `int3` padding) versus weak evidence (data values, immediates);
  - merging of fragments back into their parent functions (2,834 merged);
  - dropping labels that were mistaken for tail-call targets, including loop heads behind `jmp short` over alignment filler;
  - data and immediate pointers that take over the weaker seeds inside them;
  - a fixpoint that repeats all of this until nothing changes.
- **Switch tables:** `and reg, 2^k - 1` bounds, index-register tracking through `movzx` and `mov`, byte index tables clipped against their dword table, and no table entry landing on another function's start. 558 tables in all.
- `qilin --version`.
- Unit tests for the alignment-filler label case, EH stub and funclet discovery, Zydis flag effects and zero-unknown coverage.

### Changed
- **The CFG pass takes about 4 s.** Fragment merging carries on in place after each merge rather than rescanning every function.
- **The lifter takes the flag effects of instructions it doesn't list from Zydis's tables.** Before, those instructions (`fcomi`, `bt`, `bsf`, rotates and so on) were treated as leaving flags alone. The hand table was checked against Zydis at every decodable address: it agrees, or is deliberately stricter.
- `pushfd` is reported as an unsupported instruction rather than as a read of undefined flags.

### Fixed
- **Every "function" that was really a fragment of another is gone:** 61 in v0.3.0, 0 now. The lift report lists only instruction classes still to be implemented.
- **Misaligned overlaps drop the weaker side.** Before, the inner seed always lost. A real function at `0xD3310` was thrown away because a jump table in front of it had been decoded as an orphan function.
- **Memory displacements (`[ecx + 0x1AAB0]`) are no longer taken as function addresses,** which had created a false function over a byte table.
- **Jump-table bound search** could pick up a `cmp` on a register that had been overwritten since.

### Documentation
- **Docs:** hardware scope decided. Supported hosts are 64-bit machines with Vulkan, with macOS on Apple Silicon covered through MoltenVK. Hardware without Vulkan, and 32-bit hosts, are out of scope, so there is one renderer and one flat memory model. The Raspberry Pi 4 is described as a reference point for typical low-end hardware, not a minimum spec.
- **Docs:** naming convention for `qilin` (see `docs/naming.md`).
- **Docs:** `recompiler-core.md` explains that function and jump-table counts are discoveries, not known totals, and defines the checks that mean everything is recompiled.

## [0.3.0] - 2026-11-17
C++23 migration and full code review.

### Changed
- **The whole project is C++23**: recompiler, runtime header and generated code.
  - CMake enforces GCC 14, Clang 19, Xcode 16 or Visual Studio 2022 17.10.
  - The code now uses `std::expected`, `std::print`/`std::format`, `std::ranges`, `std::unreachable` and `std::bit_cast`.
- **CI treats warnings as errors** on Linux x64 (GCC 14 and Clang 19), Linux ARM64, macOS ARM64 and Windows (MSVC).
- **Lifting covers the Rare engine libraries too**, not just the game code: 1,373 functions, up from 1,243.
- **Generated functions touch fewer registers.** They declare only the registers they use and store back only the ones they modify.
- **New `qilin_runtime` CMake target.** It carries the floating-point rules lifted code needs (`-ffp-contract=off`). `guest.hpp` refuses `-ffast-math`.

### Fixed
- **Function discovery:**
  - Jump-table extents used the unique-target count instead of the table length, and byte index tables weren't recorded. Gap seeding could start "functions" inside switch data; two such were removed.
  - `cmp al, imm8` switch bounds weren't masked to the register width.
- **Statistics:** gap statistics were merged across sections that share a name (`BINKYUY2` ×4).
- **Lifter:**
  - OF/AF read after a shift by `cl` is now an error rather than a stale value.
  - `sar` defines OF only for a count of 1.
  - `F2`-prefixed `stos`/`movs` are treated as `rep`.
- **XBE loader:**
  - Header tables are bounds-checked as a whole.
  - UTF-16 titles with surrogate pairs decode correctly.
- **Validation:** `cli`/`sti` count as implausible in game code.
- **Test harness:** a `memcpy` from a null pointer, found by UBSan.
- **MSVC build:** a non-conforming preprocessor and `fopen` deprecation errors.

### Performance
- **CFG pass:** 1.9 s / 288 MB → 1.1 s / 127 MB.
  - The decoder cache is a flat address-indexed table of 120-byte entries.
  - The linear sweep no longer uses the cache.
  - Reachable instructions are tracked in a bitmap.
- **String operations:** `rep stos`/`rep movs` use `memset`/`memmove` wherever that gives the same result.

### Verified
- **Differential test at `-O2`:** 10,984 trials, 0 mismatches.
- **UBSan run:** 5,492 trials, 0 mismatches, no undefined behaviour.
- **Decoder:** 100% agreement with LLVM on 529,718 reachable instructions.
- **ARM64:** CFG output and lifted C++ are byte-identical to x86-64.

## [0.2.0] - 2026-11-17
Validation pass, lifter slice 1, differential test.

### Added
- **CFG validation pass:** drops data decoded as code.
  - Functions whose entry is inside another function's instruction.
  - Decode failures and overlapping instructions.
  - Instructions retail MSVC code never contains.
  - Call targets are kept and listed by `qilin suspects`.
- **Lifter slice 1 (`qilin lift`):** x86 → C++ for leaf functions.
  - Registers become host locals, and flags are computed only where they're live.
  - Labels/gotos for control flow, `switch` for jump tables.
  - Integer operations and bit-exact scalar SSE.
  - A stack-balance check rejects function fragments.
- **Runtime header `recomp/runtime/guest.hpp`.**
- **Differential test (`tools/difftest`):** each lifted function is checked against the original x86 under Unicorn. 9,944 trials, 0 mismatches.

### Fixed
- **MSVC build:** a local struct with default member initialisers, and shadowed names.

## [0.1.0] - 2026-11-17
Recompiler core foundations.

### Added
- **C++20 CMake project** with Zydis v4.1.1 and Zycore vendored (MIT).
- **XBE loader:** headers, certificate, sections, libraries, kernel thunks and flat image. Its output is identical to the Python prototype.
- **Function discovery and recursive-descent CFG,** ported from `tools/analysis/cfg.py`.
- **`tools/check/verify_decoder.py`:** cross-checks Zydis against LLVM on every reachable instruction and on a linear sweep.
- **Unit tests** on a synthetic XBE.
- **CI** on Linux x64/ARM64, macOS ARM64 and Windows x64.
- **Design and analysis notes** in `docs/`.

### Fixed
- **CI:** use `macos-latest` (the `macos-14` image was retired).

[Unreleased]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/releases/tag/v0.1.0
