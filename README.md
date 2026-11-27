# GBTGhoulies--QilinRecomp

A static recompilation of **Grabbed by the Ghoulies** (Rare / Microsoft Game Studios, original Xbox, 2003) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the game: the USA release (MS-083), extracted to a folder holding `default.xbe` and the disc files. You point the game at it the first time you play.

## Status

Plays from the attract sequence and save-slot menu through Chapter 1's story and first fight into the mansion, with sound, controllers and saves. Not yet tested to the end.

| Part | State |
|---|---|
| XBE loader | ✅ headers, certificate, sections, libraries, kernel thunks, flat image |
| x86 decoder (Zydis) | ✅ agrees with LLVM on every reachable instruction |
| Function discovery and CFG | ✅ 8,842 functions, 556 jump tables; `qilin coverage`: 0 unknown bytes in lifted code |
| Lifter → C++ | ✅ 6,214 functions (game, Rare libraries, C runtime, XAPI); differential test: 0 mismatches |
| Kernel and runtime | 🔨 native kernel, file system over the extracted disc and a data folder, guest threads; see `docs/runtime.md` |
| Graphics | 🔨 Direct3D 8 on Vulkan 1.3, shaders as generated SPIR-V, SMAA, FSR 1; see `docs/graphics.md` |
| Audio | 🔨 DirectSound over a software mixer, XACT lifted on top, out through SDL3; see `docs/audio.md` |
| Input | 🔨 controllers and keyboard through SDL3, settings menu (F1); see `docs/input.md` |

The latest checks are in [the 0.48.2 validation record](docs/validation-0.48.2.md).

## Playing

### Windows download

Each tagged release has `GBTGhoulies--QilinRecomp-<version>-windows-x64.zip` attached (`.github/workflows/player-kit.yml` also builds one on every push to `main`, as a run artifact). Unpack it and double-click **`Play Ghoulies.bat`**: the first time, it asks for your extracted disc folder and builds the game from it with the bundled compiler (LLVM-MinGW); after that it starts the game at once. `-ChooseDisc` picks another disc folder. Messages go to `run.log` and errors to `run-errors.log`. The game's code is built on your PC from your copy and never distributed. The download's `README.txt` lists the controls and settings.

### From the source on Windows

With Visual Studio 2022 (17.10 or newer, with "Desktop development with C++") and Git installed, in the project's folder:

- **`Play Ghoulies.bat`** starts the game. The first time it builds it (10 to 40 minutes), asking for your extracted disc folder.
- **`Update Ghoulies.bat`** pulls the latest code, rebuilds what changed and starts the game.

Both run `tools/build-ghoulies.ps1`. Messages go to `build-game\run.log` and `build-game\run-errors.log`.

### Linux and macOS

After [building](#building), lift the game and build `qilin_run` from it:

```sh
build/qilin lift path/to/default.xbe --out gen
cmake -S . -B build -DQILIN_GEN_DIR=$PWD/gen
cmake --build build --parallel --target qilin_run
build/qilin_run path/to/default.xbe
```

## Building

The whole project (recompiler, runtime and generated code) is C++23. You need CMake 3.25 or later and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

The one other dependency is **SDL3**: an installed SDL3 3.2 or later, or else CMake downloads SDL 3.4.20 (SHA-256 checked) once and builds it static. On Linux, SDL needs these development headers for a window and sound (Debian, Ubuntu, Raspberry Pi OS):

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

Options: `-DQILIN_GEN_DIR=<dir>` builds `qilin_run` from lifted code; `-DQILIN_SANITIZE=address,undefined` (or `thread`) instruments the runtime's own code, not the generated code (`LSAN_OPTIONS=suppressions=tools/lsan.supp` hides graphics-driver leaks); `-DQILIN_CHECKED_MEMORY=ON` bounds-checks every guest memory access.

CI builds and tests with warnings as errors on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64 (the runtime built for macOS 10.15) and Windows x64 (MSVC).

## Using it

```sh
build/qilin info     path/to/default.xbe [--json out.json]  # header, sections, XDK libraries, kernel imports
build/qilin cfg      path/to/default.xbe [--json cfg.json]  # function discovery and control flow
build/qilin insns    path/to/default.xbe [--linear] [-o listing.txt]
build/qilin suspects path/to/default.xbe                    # kept functions with problems, for review
build/qilin coverage path/to/default.xbe [--list]           # every byte: code, table, padding or unknown (exit 3 if lifted code has unknown bytes)
build/qilin lift     path/to/default.xbe --out gen/ [--range lo-hi] [--only addr]  # lift to C++ (gen/ is gitignored: never commit it)
build/qilin image    path/to/default.xbe out.bin            # the flat memory image
build/qilin --version
build/qilin_run      path/to/default.xbe [--game-dir DIR] [--data-dir DIR] [--ram-mb N] [--dump-frames DIR] [--dump-audio FILE.wav]
                     [--frames N] [--gpu N] [--audio-channels 2|6] [--headless] [--fullscreen] [--mute]
```

`--game-dir` is the extracted disc (default: `default.xbe`'s folder), `--data-dir` the saves and caches; `docs/runtime.md` covers every option. Code that compiles lifted C++ must link the `qilin_runtime` CMake target (or pass `-ffp-contract=off`): SSE results are only bit-exact without fused multiply-add.

### Full check against your copy of the game

```sh
tools/check/check_xbe.sh path/to/default.xbe
```

Runs the unit tests, the CFG pass, the decoder check and the differential test. Needs Python 3, `libLLVM` (Debian/Ubuntu: `apt install libllvm18`) and `pip install unicorn mpmath`; `CXX` names the C++23 compiler.

### Decoder check

- **Strict on every reachable instruction:** same length and same mnemonic.
- **Strict on a linear sweep of every code section too**, except for three classes it reports instead of failing, because they only occur in data bytes:
  - encodings LLVM's decoder doesn't accept, such as `int1`, `SAL /6` or `bound`;
  - LLVM returning a lone prefix byte;
  - encodings no Pentium III can run, such as `mov cs` or EVEX.

### Differential test

`tools/difftest/difftest.py` runs each lifted function, call trees included, against the original x86 in Unicorn, built at `-O2` with warnings as errors. `--sanitize` adds UndefinedBehaviorSanitizer; `--variants 4` adds two float-heavy heap images to expose rounding differences.

### x87 checks

`tools/check/x87/rounding.py` checks precision-controlled arithmetic against exact rationals; `transcendental.py` checks `fsin`, `fcos`, `fptan`, `fpatan` and `fyl2x` against mpmath.

### On Windows

The differential test and the x87 checks run with Zig's C++ compiler (`pip install ziglang unicorn mpmath`):

```sh
python tools/difftest/difftest.py --qilin C:/full/path/qilin.exe --xbe default.xbe --cxx "C:/full/path/zig.exe c++" --jobs 4
python tools/check/x87/rounding.py 7 "C:/full/path/zig.exe c++"
python tools/check/x87/transcendental.py --cxx "C:/full/path/zig.exe c++"
```

Each difftest worker takes a few hundred MB, so keep `--jobs` low.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `xbe` loader, `decoder` (Zydis wrapper), `cfg` (function discovery and control flow), `lifter` (x86 → C++), `main` (CLI) |
| `recomp/tests/` | Unit tests on a synthetic in-memory XBE, so they need no game files |
| `recomp/runtime/` | Runtime headers the generated C++ includes (guest memory, flag and float helpers, x87 maths) |
| `recomp/runtime/host/` | `qilin_run`, the replacement registry, the guest heap and the replacements (kernel, file system, Direct3D, DirectSound, XInput) |
| `recomp/runtime/gfx/` | The renderer: Vulkan 1.3, loaded at run time |
| `recomp/runtime/platform/` | The window, controllers and sound output, through SDL3 |
| `recomp/runtime/audio/` | The audio engine: voices (PCM, Xbox ADPCM) and the 48 kHz mixer |
| `recomp/runtime/ui/` | The settings menu (Dear ImGui) |
| `tools/analysis/` | The Python analysis prototype (LLVM via ctypes), the reference the C++ port is checked against |
| `tools/check/` | The LLVM decoder cross-check, the x87 checks, the C++ exception-handling report, the launcher check and the full local check script |
| `tools/difftest/` | Differential test: lifted C++ against the original x86 under Unicorn |
| `tools/gen/` | Generators for committed sources: SPIR-V for FSR, SMAA and depth packing, the kernel export list, the XDK symbol names |
| `tools/player-kit/` | The Windows download: packaging, launcher and its README |
| `tools/build-ghoulies.ps1` | The source build and launcher behind `Play Ghoulies.bat` and `Update Ghoulies.bat` |
| `third_party/` | Vendored dependencies; see `third_party/README.md` |
| `docs/` | Design notes, analysis and validation records |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. Versions before 2026-11-21 were released under MIT, and those copies stay under MIT. The vendored code in `third_party/` keeps its own licences, listed in `third_party/README.md`: Zydis and Zycore, Dear ImGui, FSR 1, SMAA, volk and XbSymbolDatabase (MIT), the Vulkan headers (Apache-2.0 or MIT) and nxdk's kernel export list (CC0-1.0). SDL3, fetched by CMake at build time, keeps its zlib licence.

The game itself is Microsoft's and Rare's and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly. A Raspberry Pi 4-class board is the reference for typical low-end hardware.
- **Static recompilation, x86 → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** D3D8 is translated at API level to a small baseline (1.3 core plus what the Pi 4's driver offers), so one render path runs everywhere. macOS runs through MoltenVK.
- **The kernel and XDK libraries are replaced natively**, not recompiled: D3D8, DSOUND, XPP, WMADEC and Bink. The C runtime, XAPI, XACT and XGraphics are lifted with the game, for exact behaviour, over a native kernel.
- **Ground truth is xemu** running the original game.

See [`docs/`](docs/) for the roadmap and the analysis notes.
