# GBTG-Qilincomp

A static recompilation of **Grabbed by the Ghoulies** (Rare, original Xbox, 2003) into portable C++20 with a Vulkan renderer, aimed at Windows, Linux, macOS (Apple Silicon) and ARM64 Linux, with a Raspberry Pi 4 as the minimum spec.

> **No game data is included, and none ever will be.** You need your own copy of the game. The tools read `default.xbe` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 2, **recompiler core**, is in progress.

| Piece | State |
|---|---|
| XBE loader (headers, certificate, sections, libraries, kernel thunks, flat image) | ✅ output identical to the Python prototype |
| x86 decoder (Zydis, vendored) | ✅ agrees with LLVM on all 532,837 reachable instructions |
| Function discovery + CFG (recursive descent, switch tables) | ✅ ported from the prototype: 9,286 functions, 558 jump tables |
| Lifter → C++ | next |
| Runtime, HLE, Vulkan | later (see roadmap) |

## Building

You need CMake ≥ 3.20 and a C++20 compiler (Clang, GCC or MSVC). There are no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds and tests on Linux x64, Linux ARM64, macOS ARM64 and Windows x64.

## Using it

```sh
build/qilin info  path/to/default.xbe            # header, sections, XDK libraries, kernel imports (JSON)
build/qilin cfg   path/to/default.xbe --json cfg.json   # function discovery + control flow
build/qilin insns path/to/default.xbe [--linear] -o listing.txt
```

### Full check against your copy of the game

```sh
tools/check/check_xbe.sh path/to/default.xbe
```

This runs the unit tests and the CFG pass, then cross-checks Zydis against LLVM's disassembler. It needs Python 3 and `libLLVM` (Debian/Ubuntu: `apt install libllvm18`). The check is:

- **Strict on every reachable instruction:** same length and same mnemonic.
- **Strict on a linear sweep of every code section too**, except for three classes it reports instead of failing, because they only occur in data bytes:
  - encodings LLVM's decoder doesn't accept, such as `int1`, `SAL /6` or `bound`;
  - LLVM returning a lone prefix byte;
  - encodings no Pentium III can run, such as `mov cs` or EVEX.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `xbe` loader, `decoder` (Zydis wrapper), `cfg` (function discovery + control flow), `main` (CLI) |
| `recomp/tests/` | Unit tests on a synthetic in-memory XBE, so they need no game files |
| `tools/analysis/` | The original Python analysis prototype, using LLVM via ctypes. Kept as the reference the C++ port is checked against |
| `tools/check/` | The LLVM decoder cross-check and the full local check script |
| `third_party/` | Vendored dependencies: Zydis and Zycore, MIT licensed. See `third_party/README.md` |
| `docs/` | Design notes |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it's fast on a Pi 4, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, x86 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **No DirectX.** D3D8 is translated at API level to Vulkan, using only the feature set the Pi 4's v3dv driver supports. macOS runs through MoltenVK.
- **XDK libraries are replaced natively**, not recompiled: XAPI, CRT, D3D8, DSOUND, XACT, XPP, WMADEC and Bink.
- **Ground truth is xemu** running the original game.

See `docs/` for the full roadmap and the analysis notes.
