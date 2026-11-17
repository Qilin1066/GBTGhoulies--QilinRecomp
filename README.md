# GBTG-Qilincomp

A static recompilation of **Grabbed by the Ghoulies** (Rare, original Xbox, 2003) into portable C++23 with a Vulkan renderer, aimed at 64-bit Windows, Linux and macOS (Apple Silicon), on x86-64 or ARM64. Anything with a Vulkan driver is in scope; Apple Silicon is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor.

> **No game data is included, and none ever will be.** You need your own copy of the game. The tools read `default.xbe` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 2, **recompiler core**, is in progress.

| Piece | State |
|---|---|
| XBE loader (headers, certificate, sections, libraries, kernel thunks, flat image) | ✅ output identical to the Python prototype |
| x86 decoder (Zydis, vendored) | ✅ agrees with LLVM on every reachable instruction |
| Function discovery + CFG (recursive descent, switch tables, C++ EH tables, data-as-code validation) | ✅ 8,843 functions, 556 jump tables (every one in lifted code has a proven size); **100% of the lifted code accounted for** (`qilin coverage`: 0 unknown bytes in the game code, Rare libraries and EH funclets) |
| Lifter → C++, slice 1 (leaf functions: integer, scalar SSE, jump tables) | ✅ 1,276 functions (game code + Rare libraries), differentially tested against the original x86 under Unicorn, also under UBSan: 0 mismatches |
| Lifter slice 2 (calls) / slice 3 (x87) | next |
| Runtime, HLE, Vulkan | later (see roadmap) |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## Building

The whole project (recompiler, runtime and generated code) is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older. There are no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds and tests with warnings as errors on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and Windows x64 (MSVC).

## Using it

```sh
build/qilin info     path/to/default.xbe                 # header, sections, XDK libraries, kernel imports (JSON)
build/qilin cfg      path/to/default.xbe --json cfg.json # function discovery + control flow
build/qilin insns    path/to/default.xbe [--linear] -o listing.txt
build/qilin suspects path/to/default.xbe                 # kept functions with problems, for review
build/qilin coverage path/to/default.xbe [--list]        # every byte: code, table, padding or unknown (exit 3 if lifted code has unknown bytes)
build/qilin lift     path/to/default.xbe --out gen/      # lift to C++ (gen/ is gitignored: never commit it)

Code that compiles lifted C++ must link the `qilin_runtime` CMake target (or pass `-ffp-contract=off`): SSE results are only bit-exact without fused multiply-add.
```

### Full check against your copy of the game

```sh
tools/check/check_xbe.sh path/to/default.xbe
```

This runs the unit tests and the CFG pass, then cross-checks Zydis against LLVM's disassembler. Finally it runs the differential test, which compares each lifted function with the original x86 code run in the Unicorn emulator. It needs Python 3, `libLLVM` (Debian/Ubuntu: `apt install libllvm18`) and `pip install unicorn`. Set `CXX` to a C++23 compiler for the differential test, which builds the lifted code at `-O2` with warnings as errors. Pass `--sanitize` to `tools/difftest/difftest.py` for an UndefinedBehaviorSanitizer build. The decoder check is:

- **Strict on every reachable instruction:** same length and same mnemonic.
- **Strict on a linear sweep of every code section too**, except for three classes it reports instead of failing, because they only occur in data bytes:
  - encodings LLVM's decoder doesn't accept, such as `int1`, `SAL /6` or `bound`;
  - LLVM returning a lone prefix byte;
  - encodings no Pentium III can run, such as `mov cs` or EVEX.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `xbe` loader, `decoder` (Zydis wrapper), `cfg` (function discovery + control flow), `lifter` (x86 → C++), `main` (CLI) |
| `recomp/tests/` | Unit tests on a synthetic in-memory XBE, so they need no game files |
| `tools/analysis/` | The original Python analysis prototype, using LLVM via ctypes. Kept as the reference the C++ port is checked against |
| `recomp/runtime/` | Runtime header the generated C++ includes (guest memory, flag and float helpers) |
| `tools/check/` | The LLVM decoder cross-check and the full local check script |
| `tools/difftest/` | Differential test: lifted C++ against the original x86 under Unicorn |
| `third_party/` | Vendored dependencies: Zydis and Zycore, MIT licensed. See `third_party/README.md` |
| `docs/` | Design notes |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, x86 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **No DirectX.** D3D8 is translated at API level to Vulkan, using a deliberately small baseline feature set so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **XDK libraries are replaced natively**, not recompiled: XAPI, CRT, D3D8, DSOUND, XACT, XPP, WMADEC and Bink.
- **Ground truth is xemu** running the original game.

See `docs/` for the full roadmap and the analysis notes.
