# GBTG-Qilincomp

A static recompilation of **Grabbed by the Ghoulies** (Rare, original Xbox, 2003) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor.

> **No game data is included, and none ever will be.** You need your own copy of the game. The tools read `default.xbe` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 2, **recompiler core**, is in progress.

| Piece | State |
|---|---|
| XBE loader (headers, certificate, sections, libraries, kernel thunks, flat image) | ✅ output identical to the Python prototype |
| x86 decoder (Zydis, vendored) | ✅ agrees with LLVM on every reachable instruction |
| Function discovery + CFG (recursive descent, switch tables, C++ EH tables, data-as-code validation) | ✅ 8,842 functions, 556 jump tables (every one in lifted code has a proven size); **100% of the lifted code accounted for** (`qilin coverage`: 0 unknown bytes in the game code, Rare libraries and EH funclets) |
| Lifter → C++ (integer, scalar SSE, x87 with the game's 24-bit precision control, jump tables, string ops; direct, indirect and tail calls; C++ exception frames) | ✅ all 6,214 functions (game code, Rare libraries, and the 471 C runtime and XAPI functions they reach; 3 more belong to Bink and are replaced with it), differentially tested against the original x86 under Unicorn, whole call trees included, also under UBSan: 0 mismatches |
| Runtime: `qilin_run` loads the game and runs the lifted `main`, with a registry of native replacements for everything that isn't lifted | 🔨 started: XAPI and the C runtime lifted, the kernel beneath them native (memory, critical sections, timers, settings, the file system over your extracted disc and a data folder); XACT and XGraphics lifted too; the game runs its attract sequence, takes START, shows its save-slot menu and saves (see `docs/runtime.md`) |
| HLE of the XDK libraries, Vulkan | 🔨 started: the Vulkan 1.3 renderer (clears and frame flips, tested on Mesa's software driver in CI) and the first Direct3D replacements: the device, its frame buffers, clears, swaps at the display's vblank (see `docs/graphics.md`). Since v0.22.0 the game draws: vertex programs and pixel combiners as generated SPIR-V, textures, in a window (SDL3) since v0.23.0, with controllers and the keyboard since v0.24.0 (`docs/input.md`). DirectSound replaced over a software mixer (PCM and Xbox ADPCM voices, mix bins, streams, `--dump-audio`), playing through SDL3 since v0.25.0 (see `docs/audio.md`). Since v0.28.0 Chapter 1 is playable from the story to its first fight, and since v0.29.0 past it, into the mansion |

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

CMake stops with a clear message if the compiler is older.

The one other dependency is **SDL3** (window, input, audio output). CMake uses an installed SDL3 (3.2 or later) if it finds one; otherwise it downloads SDL 3.4.20 (checked against its SHA-256) and builds it static, which needs network access once. On Linux, SDL needs the development headers of what it supports, or the build has no window or sound there. On Debian, Ubuntu and Raspberry Pi OS:

```sh
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev libegl-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev
```

Windows and macOS need nothing more.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CI builds and tests with warnings as errors on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64 (the runtime built for macOS 10.15) and Windows x64 (MSVC).

## Using it

```sh
build/qilin info     path/to/default.xbe                 # header, sections, XDK libraries, kernel imports (JSON)
build/qilin cfg      path/to/default.xbe --json cfg.json # function discovery + control flow
build/qilin insns    path/to/default.xbe [--linear] -o listing.txt
build/qilin suspects path/to/default.xbe                 # kept functions with problems, for review
build/qilin coverage path/to/default.xbe [--list]        # every byte: code, table, padding or unknown (exit 3 if lifted code has unknown bytes)
build/qilin lift     path/to/default.xbe --out gen/      # lift to C++ (gen/ is gitignored: never commit it)
build/qilin_run      path/to/default.xbe [--game-dir DIR] [--data-dir DIR] [--ram-mb N] [--dump-frames DIR] [--dump-audio FILE.wav] [--frames N] [--gpu N] [--headless] [--fullscreen] [--mute]  # run it (built with -DQILIN_GEN_DIR=gen)

Code that compiles lifted C++ must link the `qilin_runtime` CMake target (or pass `-ffp-contract=off`): SSE results are only bit-exact without fused multiply-add.
```

### Full check against your copy of the game

```sh
tools/check/check_xbe.sh path/to/default.xbe
```

This runs the unit tests and the CFG pass, then cross-checks Zydis against LLVM's disassembler. Finally it runs the differential test, which compares each lifted function with the original x86 code run in the Unicorn emulator. It needs Python 3, `libLLVM` (Debian/Ubuntu: `apt install libllvm18`) and `pip install unicorn`. Set `CXX` to a C++23 compiler for the differential test, which builds the lifted code at `-O2` with warnings as errors. Pass `--sanitize` to `tools/difftest/difftest.py` for an UndefinedBehaviorSanitizer build, and `--variants 4` to add two float-heavy heap images, which make rounding differences more likely to show. `tools/check/x87/rounding.py` checks the x87 precision-control arithmetic against exact rational results, and `tools/check/x87/transcendental.py` checks `fsin`, `fcos`, `fptan`, `fpatan` and `fyl2x` against mpmath (`pip install mpmath`, which the differential test also needs). The decoder check is:

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
| `recomp/runtime/` | Runtime headers the generated C++ includes (guest memory, flag and float helpers, x87 maths) |
| `recomp/runtime/host/` | The runtime's host side: `qilin_run`, the replacement registry, the guest heap and the replacements themselves (kernel, file system, Direct3D, XInput) |
| `recomp/runtime/gfx/` | The renderer: Vulkan 1.3, loaded at run time |
| `recomp/runtime/platform/` | The window, the controllers and sound output, through SDL3 |
| `recomp/runtime/audio/` | The audio engine: voices (PCM, Xbox ADPCM) and the 48 kHz mixer |
| `tools/check/` | The LLVM decoder cross-check, the x87 rounding and transcendental cross-checks, the C++ exception-handling report and the full local check script |
| `tools/difftest/` | Differential test: lifted C++ against the original x86 under Unicorn |
| `third_party/` | Vendored dependencies: Zydis and Zycore (MIT), the Vulkan headers (Apache-2.0 or MIT), volk (MIT), nxdk's kernel export list (CC0). See `third_party/README.md` |
| `docs/` | Design notes |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, x86 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **No DirectX.** D3D8 is translated at API level to Vulkan 1.3, using a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The kernel and XDK libraries are replaced natively**, not recompiled: D3D8, DSOUND, XACT, XPP, WMADEC and Bink. The C runtime and XAPI are lifted with the game, for exact behaviour, over a native kernel.
- **Ground truth is xemu** running the original game.

See `docs/` for the full roadmap and the analysis notes.
