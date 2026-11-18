# Runtime: status (roadmap step 3)

Last updated 2026-11-18 (v0.12.0: XAPI lifted too; the runtime implements the kernel beneath it; guest RAM size is a setting).

The runtime is everything the lifted game code needs that isn't lifted: guest memory, the Xbox kernel, D3D, DirectSound/XACT and the other XDK libraries. All of it is replaced natively (HLE); none of it is emulated.

Two libraries are lifted with the game instead: the C runtime (since v0.11.0), so `sprintf`, `strtod` and the x87 maths give exactly the original's results, and XAPI (since v0.12.0), so the heap, file and thread wrappers and XGraphics' maths behave exactly as the original's. Below them is the kernel's documented interface (the `xboxkrnl.exe` exports), which the runtime implements. That boundary is narrower and better specified than XAPI's: lifted code imports 81 kernel functions and variables, against the 88 XAPI entry points the game calls. Four XAPI functions can't be lifted and are replaced natively (below).

## How it works

**`qilin_run`** (`recomp/runtime/host/run.cpp`) loads `default.xbe` into guest memory and calls the game's lifted `main`. It's built from `qilin lift` output:

```sh
qilin lift default.xbe --out gen
cmake -S . -B build -DQILIN_GEN_DIR=$PWD/gen && cmake --build build
build/qilin_run default.xbe
```

**Start-up.** The XBE entry point (`0x14FAE9`) is XAPI start-up code. It sizes the static TLS block, then creates the main thread, whose procedure (`0x14FA75`) initialises XAPI, runs the initializer tables, then calls the game's `main(0, 0, 0)` at `0x32390`. The runtime sets up memory, the main thread's stack, KPCR and KTHREAD, then follows the same steps:

1. The entry point's prologue, natively: the certificate size fix-up and the TLS size (`0x549528`) and index.
2. XAPI's process initialisation, lifted (`0x151011`): critical sections, the auto power-off timer, the process heap (`RtlCreateHeap` over `NtAllocateVirtualMemory`), console settings, drive letters. The step before it in the thread procedure, `0x1517B9`, patches kernel code for known kernel builds (`cli`, writes into the kernel, `sti`); it's left out.
3. The C runtime's initializers, in XAPI's order (`0x15172C`, then `0x1516D4`). They hold only library code, no game constructors:

| Step | Entries | What |
|---|---|---|
| Table `0x32C800` | `0x16AFDD`, `0x16D401` | `_mtinit` (per-thread data), `_ioinit` (stdio handles) |
| `_FPinit` (`[0x337A88]` = `0x1673C1`) | | `_fpmath`: the float-formatting hooks `printf` calls through, x87 set-up. Without it `%f` reaches `_fptrap` and aborts |
| Table `0x32C838` | `0x167A80`, `0x1688B3`, `0x16EE9C`, `0x16C272` | C runtime |
| Table `0x32C810` | `0x16D805`; `0x197AAF`, `0x197ABB`, `0x197AC7` (XGRPH); `0x1C8D48`, `0x1C8D53`, `0x1CBAD6`, `0x1CBAE1` (DSOUND) | The C runtime's runs; the library ones are skipped, their libraries being replaced natively |

An entry runs if it's lifted (`rt::is_lifted`); otherwise `qilin_run` names it and skips it.

**Guest memory.** RAM flat from address 0: 64 MB by default, the retail Xbox's, or more with `qilin_run --ram-mb N` (a multiple of 4, up to 1024). The game reads the size from `GlobalMemoryStatus` and records 128 MB, the development kit's size, when there's more than 64 MB; testing found no change in behaviour. The host allocates RAM with `calloc`, so pages the guest never touches cost nothing. Rendering at higher resolutions, SMAA and similar features don't need guest RAM: render targets and textures live in the Vulkan renderer's host memory.

| Range (for R bytes of RAM) | What |
|---|---|
| `0x00010000` … end of image | The XBE image, BSS zeroed |
| End of image (64 KB aligned) … stack | Virtual memory (`host::VirtualMemory`): `NtAllocateVirtualMemory` and contiguous allocations |
| R − 0x120000 … R − 0x20000 | The main thread's stack (1 MB) |
| R − 0x20000 | The main thread's KPCR: `fs:[0]` (the exception chain, starts as `0xFFFFFFFF`), stack limits, `Prcb`, `CurrentThread` at `+0x28` |
| R − 0x1F000 | The main thread's KTHREAD |
| R − 0x10000 | Objects behind the kernel's data exports, 256 bytes each |

**Virtual memory** works as on the Xbox: 4 KB pages, reservations on 64 KB boundaries (lowest first, or highest with `MEM_TOP_DOWN`), pages committed inside a reservation and zeroed when committed, `NtQueryVirtualMemory` reporting reserved, committed and free runs. Contiguous allocations (`MmAllocateContiguousMemory[Ex]`) come from the top, within the caller's address limits. On the Xbox they're mapped at `0x80000000` + physical; here the guest gets the RAM address, since the address space is flat.

`qilin_run` is built with checked memory for bring-up, so an access outside RAM stops with the guest address of the instruction.

**Kernel imports.** A function's thunk slot keeps `0x80000000 | ordinal`, which `rt::call_unlifted` recognises. A data export's slot (`KeTickCount`, `LaunchDataPage`, `XboxHardwareInfo` and 12 more) gets the address of its object. Names, ordinals, stdcall argument sizes and data/fastcall flags come from nxdk's `xboxkrnl.exe.def` (CC0, in `third_party/nxdk/`), turned into `recomp/runtime/host/kernel_exports.inc` by `tools/gen/kernel_exports.py`.

**Replacements** (`recomp/runtime/host/host.hpp`). Every call from lifted code to code that isn't lifted reaches `rt::call_unlifted`: a direct call to a stub, an indirect call the function table doesn't know, or a kernel import. It runs the replacement registered for that guest address. There's no fallback: anything unregistered stops the game and names the target and its caller, for example:

```
qilin: stopped at 80000123: no replacement for kernel RtlInitializeCriticalSection (ordinal 291), called from 0016D9F2
```

Replacements read stdcall arguments with `host::arg(c, i)` and return with `host::ret(c, eax, arg_bytes)`, which pops the return address and the arguments as `ret n` does. Lifted code then checks the return address, as after any call.

## Replacements so far

**XAPI functions that can't be lifted** (`kNative` in `recomp/src/main.cpp`, replacements in `recomp/runtime/host/xapi.cpp`):

| Address | Function | Why | Replacement |
|---|---|---|---|
| `0x14C29C` | `QueryPerformanceCounter` | `rdtsc` | Host steady clock, scaled to the Xbox's 733,333,333 Hz |
| `0x14B924` | `OutputDebugStringA` | `int 0x2D`, the kernel debugger's print service | Prints the string to stderr as `[guest] …` |
| `0x1517B9` | Kernel patch at start-up | `cli`, writes into kernel code | Not run |
| `0x151B45` | Shutdown notification | reloads the GDT | Never called: the runtime doesn't shut down |

**Kernel functions** (`recomp/runtime/host/kernel.cpp`). Each one is registered with its stdcall argument size, checked against the export table, and works on the guest's own structures:

| Kernel function | Implementation |
|---|---|
| `RtlInitializeCriticalSection`, `RtlEnterCriticalSection`, `RtlLeaveCriticalSection`, `RtlTryEnterCriticalSection` | The kernel's algorithm on the guest's `RTL_CRITICAL_SECTION` (event, `LockCount`, `RecursionCount`, `OwningThread`). A wait for another thread stops the runtime, which has one thread so far |
| `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtQueryVirtualMemory` | Virtual memory above |
| `MmAllocateContiguousMemory[Ex]`, `MmFreeContiguousMemory`, `MmPersistContiguousMemory` | Contiguous allocations above; persisting is a no-op |
| `MmQueryStatistics` | Pages of RAM, available pages after the image, stack, kernel area and committed memory |
| `KeInitializeDpc`, `KeInitializeTimerEx`, `KeSetTimer[Ex]`, `KeCancelTimer` | The guest objects are initialised as the kernel does. Timers are recorded with their due time and DPC; firing them waits for the scheduler. XAPI's first timer is its six-hour auto power-off |
| `ExQueryNonVolatileSetting` | A North American retail console with factory settings: English, 60 Hz, NTSC-M, region 1. An unmodelled setting stops the runtime |
| `RtlNtStatusToDosError` | The NT table for the statuses the runtime and the file system return; any other status stops the runtime |
| `HalRegisterShutdownNotification` | Accepted; the routines never run |

**Where it stops now:** XAPI's process initialisation opens a file with `NtOpenFile` (from `0x150EAE`): the file system is next. In v0.11.0 it stopped at the CRT's first critical section; before that, at `sprintf`.

## Tests

`qilin_host_tests` (CI, no game files) covers the kernel export table, a replacement called through `rt::call_unlifted` with stdcall argument popping, `describe()`, and virtual memory (granularity, top-down reservations, commit zeroing, queries, release).

## Next

1. **The file system:** the object namespace (`\Device\CdRom0`, the hard disk partitions, `\??\D:` and the other symbolic links XAPI creates), handles, and `Nt*File` over host directories: the extracted disc read-only, save data and cache partitions writable.
2. **The scheduler:** guest threads (`PsCreateSystemThreadEx`), waits, events, timers and DPCs. Until then a second thread stops the runtime.
3. **Keep running `qilin_run`** and implement each kernel function it stops at. Lifted code imports 81; the rest of the list is the 167 library entry points (D3D, XACT, DSOUND …) it calls directly.
4. **Hot C runtime functions** (`memcpy`, `strlen` …) may get native versions once the runtime shows which are hot, each checked against the lifted original.
5. **Names:** XbSymbolDatabase can name the XDK library functions by signature, which would speed up identifying them.
6. **D3D on Vulkan** (roadmap step 4).
