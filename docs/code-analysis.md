# default.xbe — code analysis

Tools: `tools/disasm.py` (section dump + GNU objdump), `tools/analyze.py` (function discovery, call graph, kernel call sites), `tools/profile.py` (instruction-set profile). Raw outputs in `analysis/`.

Method: linear-sweep disassembly; function starts = entry point + direct call targets + code after `ret`/`jmp` + int3/nop padding + pointers from `.rdata`/`.data` into code. Counts are a first pass; a recursive-descent pass (needed for the lifter anyway) will tighten them.

## Totals
- **8,497 functions** across all code sections
- **Rare's game code: ~5,210 functions, ~1.19 MB, ~361,000 instructions** (median function 131 bytes; largest 8.6 KB at `0x12920`)

## What's in `.text`
Boundaries found from function alignment (Rare's code is 16-byte aligned, Microsoft's prebuilt libraries aren't), kernel call sites, string references and the call graph.

| Range | Size | Contents | Plan |
|---|---|---|---|
| `0x011000–0x14A840` | ~1,206 KB | **Ghoulies game code** (C++) | Lift |
| `0x14A840–0x152780` | ~32 KB | **XAPI** (file I/O, threads, memory, save signing, title metadata), starting with `IsBadReadPtr` | Replace (native HLE) |
| `0x152780–0x165C00` | ~77 KB | **Rare engine libraries** (C): scene-graph renderer (`nodes\geometry\pushbuffer.c`, `blendshape.c`, `nd*` node types), `audiocore.c` wrapper over XACT, zlib inflate | Lift; swap zlib for real zlib |
| `0x165C00–0x170D80` | ~45 KB | **C/C++ runtime** (LIBCMT/LIBCPMT: printf family, float formatting, exceptions) | Replace with host CRT |
| `0x170D80–0x171020` | ~0.7 KB | Compiler-generated C++ exception-unwind funclets (belong to game) | Lift |

Two 5-byte stubs at `0xF7270` / `0xF7280` are jumps into XAPI's allocator, called by D3D/XGRPH/XACT/WMADEC: library memory allocation goes through one hook point.

## Cross-module calls (direct)
- game → D3D **1,095**, game → Rare libs 1,001, game → XAPI 727, game → CRT 393, game → D3DX 93, game → Bink 11, game → XGRPH 9
- Rare libs → D3D 111, → XACT 36, → D3DX 33
- XACT → DSOUND 67
- Game code makes **no direct kernel calls** except one critical-section helper; kernel access goes through XAPI. The HLE kernel surface is therefore driven by XAPI + the libraries we're replacing.

## Instruction-set profile (game code)
| Category | Count | Notes |
|---|---|---|
| Scalar SSE | **58,681** | `movss` 31.6k, `mulss` 6.8k, `addss` 4.7k, `comiss` 3.9k, `movaps` 3.7k, `subss` 3.6k... Compiled for SSE scalar float: maps 1:1 to IEEE `float` on ARM, deterministic |
| x87 | 5,457 | Mostly loads/stores/mul; `fsqrt`, `fcomip`. Must be translated (prefer native double) |
| MMX / 3DNow! | ~3 | Effectively none (likely data misread) |
| String ops (`rep movs/stos`) | 1,082 | memcpy/memset-style; trivial |
| Indirect calls | 846 | C++ virtual calls / function pointers; need a runtime lookup table |
| Switch jump tables | 496 | Tables inline after code; must be decoded |
| Other indirect jumps | 17 | |
| `fs:` (SEH / C++ EH) | 63 | Game uses C++ exception frames. Resolved in v0.7.0: lifted as guest-memory accesses (see `recompiler-core.md`) |
| Privileged/port I/O | 0 real | The few hits are jump-table data decoded as code |

Elsewhere: **BINK has 1,743 MMX instructions** (YUV→RGB blitters). The Rare libs are heavier on x87 (4.3k) plus 2.4k SSE.

## Assets checked
- `movie/*`: **Bink 1 (`BIKi`)**, e.g. 320×240 @ 30 fps. FFmpeg's Bink decoder handles these, so we can replace the MMX-heavy BINK sections instead of lifting them.
- `xwavebank/*`: **XACT wave banks `WBND` v3** (bank name `Gharddrive1` in `02110fd1`).
- `demand/*`: small per-asset records including XACT sound bank names.
- `bundles/*.bnl`: header + **zlib-compressed** payload (matches the zlib in the Rare libs).

## Pass 2: recursive-descent control flow (`tools/cfg.py`, LLVM 18 disassembler via `tools/x86dec.py`)
- **9,402 functions, 530,764 instructions** across all code sections.
- **`.text` coverage: 97.7% of non-padding bytes decoded as reachable code.** Of the game region's remaining gaps, most are switch-table data and MSVC multi-byte NOP padding; about 24 KB is unexplained (code reached only through pointers we don't yet resolve, plus inline data).
- **556 switch jump tables resolved** (317 bounded by `cmp`, 193 via `cmp` + byte index table, 46 heuristic), 3,364 case targets.
- **24 unresolved indirect jumps**, all accounted for: kernel import stubs (`jmp [thunk]`), a few function-pointer variables, and CRT internals.
- **~1,960 indirect calls** (C++ virtual calls and callbacks) → generated function table + trap-on-unknown at runtime.
- Function seeds: entry, call targets, tail calls, aligned code pointers in `.rdata`/`.data` (accepted only if they explore cleanly outside known code), padding-delimited starts, immediates.
- Instruction set of game + Rare libs: ~150 real mnemonics (scalar SSE, x87, integer, string ops).

## Raw NV2A usage — resolved
Rare's code writes raw GPU commands through one D3D routine at `0x177250` (D3D's "begin push"), **called from only 17 functions** (16 game, 1 Rare-libs). Every command they write is immediate-mode drawing:

- `SET_BEGIN_END(QUADS | TRIANGLE_STRIP | TRIANGLES | LINE_LOOP)` → `INLINE_ARRAY[n]` (non-incrementing; 8–256 dwords of vertex data) → `SET_BEGIN_END(END)`
- Callers are 2D quads, line loops and strips (HUD, overlays, effects) at `0x5AE40`–`0x62750`, `0x118FD0`–`0x11F690`, `0x156AE0`.

Asset check: a 6.7 MB decompressed character bundle contains **no** push-buffer command streams, so models ship as vertex/index data.

**Consequence:** no NV2A emulation needed. The Vulkan layer implements `BeginPush` as a guest-visible scratch buffer and, at `EndPush`, interprets just `SET_BEGIN_END` / `INLINE_ARRAY` (anything else asserts) and issues the equivalent of `DrawPrimitiveUP` with the current vertex format.

## Risks remaining
1. ~~**C++ exceptions / SEH**~~ Resolved in v0.7.0: the frame chain stays in guest memory, and since nothing in the game handles an exception it can throw, a throw stops with a diagnostic (see `recompiler-core.md`).
2. **Indirect calls**: requires a function table covering every possible target (vtables in `.rdata`, function pointers in `.data`).
3. **`pushbuffer.c` engine node**: may use D3D push-buffer *recording* (`BeginPushBuffer`/`RunPushBuffer`) at runtime. If so, our D3D layer records its own command lists, so no NV2A parsing is needed. To confirm once D3D functions are named.

## Next steps
1. C++ recompiler core: XBE loader, Zydis decoder (vendored; verified against LLVM on every instruction), CFG port, lifter emitting C++.
2. Name XDK library functions with XbSymbolDatabase (MIT); cross-check our function discovery against the existing Ghoulies PC Recomp's seed addresses (see `prior-art.md`).
3. Name the D3D8 / XAPI / CRT entry points the game calls (1,095 game→D3D call sites) to define the runtime's API surface.
4. Differential test harness: run lifted leaf functions on host vs the original x86 under an emulator/CPU, compare state.
