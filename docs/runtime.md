# Runtime: status (roadmap step 3)

Last updated 2026-11-18 (v0.10.0: first slice: `qilin_run`, the replacement registry, the first replacements).

The runtime is everything the lifted game code needs that isn't lifted: guest memory, the kernel, XAPI, the C runtime, D3D, DirectSound/XACT and the other XDK libraries. All of it is replaced natively (HLE); none of it is emulated.

## How it works

**`qilin_run`** (`recomp/runtime/host/run.cpp`) loads `default.xbe` into guest memory and calls the game's lifted `main`. It's built from `qilin lift` output:

```sh
qilin lift default.xbe --out gen
cmake -S . -B build -DQILIN_GEN_DIR=$PWD/gen && cmake --build build
build/qilin_run default.xbe
```

**Start-up.** The XBE entry point (`0x14FAE9`) is XAPI start-up code. It creates the main thread, whose procedure (`0x14FA75`) initialises XAPI and the C runtime, then calls the game's `main(0, 0, 0)` at `0x32390`. The C runtime's initializer tables (`0x32C800`, `0x32C810`, `0x32C838`) hold only library code, with no game constructors. So the runtime replaces all of start-up: it sets up memory, the stack and the thread information block, then calls `main`.

**Guest memory.** The Xbox's 64 MB of RAM, flat from address 0:

| Range | What |
|---|---|
| `0x00010000` … end of image | The XBE image, BSS zeroed |
| End of image … `0x03EE0000` | Guest heap: whole 4 KB pages, first fit, zeroed (`host::GuestHeap`) |
| `0x03EE0000–0x03FE0000` | The main thread's stack (1 MB) |
| `0x03FE0000` | The main thread's information block: `fs:[0]`, the exception chain, starts as `0xFFFFFFFF` |
| `0x03FF0000` | Objects behind the kernel's data exports, 256 bytes each |

`qilin_run` is built with checked memory for bring-up, so an access outside RAM stops with the guest address of the instruction.

**Kernel imports.** A function's thunk slot keeps `0x80000000 | ordinal`, which `rt::call_unlifted` recognises. A data export's slot (`KeTickCount`, `LaunchDataPage`, `XboxHardwareInfo` and 12 more) gets the address of its object. Names, ordinals, stdcall argument sizes and data/fastcall flags come from nxdk's `xboxkrnl.exe.def` (CC0, in `third_party/nxdk/`), turned into `recomp/runtime/host/kernel_exports.inc` by `tools/gen/kernel_exports.py`.

**Replacements** (`recomp/runtime/host/host.hpp`). Every call from lifted code to code that isn't lifted reaches `rt::call_unlifted`: a direct call to a stub, an indirect call the function table doesn't know, or a kernel import. It runs the replacement registered for that guest address. There's no fallback: anything unregistered stops the game and names the target and its caller, for example:

```
qilin: stopped at 001661F9: no replacement for crt code at 001661F9, called from 000FF33A
```

Replacements read stdcall arguments with `host::arg(c, i)` and return with `host::ret(c, eax, arg_bytes)`, which pops the return address and the arguments as `ret n` does. Lifted code then checks the return address, as after any call.

## Replacements so far

| Address | Function | Replacement |
|---|---|---|
| `0x14C29C` | `QueryPerformanceCounter` | Host steady clock, scaled to the Xbox's 733,333,333 Hz (XAPI reads `rdtsc`) |
| `0x14C2AD` | `QueryPerformanceFrequency` | 733,333,333 |
| `0x14AD17` | `GlobalMemoryStatus` | 64 MB total, available from the guest heap, XAPI's fixed virtual figures |
| `0x14AE89` | `XGetLaunchInfo` | `ERROR_NOT_FOUND`: always a cold boot |
| `0x14FA3B` | `XGetLanguage` | English (1); there are no console settings yet |

Each was identified from its disassembly, and the source comment says what the original does.

**Where it stops now:** `main` calls `sprintf` (`0x1661F9`, the C runtime) from `0xFF335`.

## Tests

`qilin_host_tests` (CI, no game files) covers the kernel export table, a replacement called through `rt::call_unlifted` with stdcall argument popping, `describe()`, and the guest heap.

## Next

1. **The C runtime:** the printf family, with MSVC's formatting (floats included), then string and memory functions. They're called with guest pointers, so each reads and writes guest memory.
2. **Keep running `qilin_run`** and replace each function it stops at, in order. The 276 library entry points the game calls directly are the list (D3D 90, XAPI 79, CRT 41, XACT 28 and others), plus kernel imports used through XAPI.
3. **Names:** XbSymbolDatabase can name the XDK library functions by signature, which would speed up identifying them.
4. **Threads, the file system over the extracted disc, then D3D on Vulkan** (roadmap steps 3–4).
