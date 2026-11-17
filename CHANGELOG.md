# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

### Changed
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

[Unreleased]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Tecboy1066/GBTG-Qilincomp/releases/tag/v0.1.0
