# Runtime: status (roadmap step 3)

Last updated 2026-11-18 (v0.16.0: guest RAM mapped at the kernel's windows too; D3D's resource code lifted; the game creates its textures and stops at its first vertex shader. v0.15.0: audio starts, `audio.md`; the renderer and Direct3D are in `graphics.md`).

The runtime is everything the lifted game code needs that isn't lifted: guest memory, the Xbox kernel, D3D, DirectSound/XACT and the other XDK libraries. All of it is replaced natively (HLE); none of it is emulated.

Four libraries are lifted with the game instead: the C runtime (since v0.11.0), so `sprintf`, `strtod` and the x87 maths give exactly the original's results; XAPI (since v0.12.0), so the heap, file and thread wrappers behave exactly as the original's; and XACT and XGraphics (since v0.15.0), the audio engine and the resource-header, swizzling and maths helpers, which sit on interfaces the runtime replaces (DirectSound, D3D). Below them is the kernel's documented interface (the `xboxkrnl.exe` exports), which the runtime implements. That boundary is narrower and better specified than XAPI's: lifted code imports 81 kernel functions and variables, against the 88 XAPI entry points the game calls. Four XAPI functions can't be lifted and are replaced natively (below).

## How it works

**`qilin_run`** (`recomp/runtime/host/run.cpp`) loads `default.xbe` into guest memory and calls the game's lifted `main`. It's built from `qilin lift` output:

```sh
qilin lift default.xbe --out gen
cmake -S . -B build -DQILIN_GEN_DIR=$PWD/gen && cmake --build build
build/qilin_run [--game-dir DIR] [--data-dir DIR] [--ram-mb N] [--dump-frames DIR] [--dump-audio FILE.wav] [--frames N] [--gpu N] path/to/default.xbe
```

`--game-dir` is the extracted disc (by default, `default.xbe`'s folder). `--data-dir` holds the hard disk: saves and caches, by default `%LOCALAPPDATA%\qilin\ghoulies` on Windows, `~/Library/Application Support/qilin/ghoulies` on macOS and `$XDG_DATA_HOME/qilin/ghoulies` (`~/.local/share/…`) elsewhere. `--dump-frames DIR` writes every frame shown as a PNG, `--dump-audio FILE.wav` everything the mixer plays, `--frames N` stops after N frames, and `--gpu N` picks the Vulkan device (see `graphics.md`). Set `QILIN_TRACE_HLE=1` to log every replacement call (caller, arguments, result) and every object name the file system resolves, with where it went.

**Start-up.** The XBE entry point (`0x14FAE9`) is XAPI start-up code. It sizes the static TLS block, then creates the main thread, whose procedure (`0x14FA75`) initialises XAPI, runs the initializer tables, then calls the game's `main(0, 0, 0)` at `0x32390`. The runtime sets up memory, the main thread's stack, KPCR and KTHREAD, then follows the same steps:

1. The entry point's prologue, natively: the certificate size fix-up and the TLS size (`0x549528`) and index. The kernel puts a thread's TLS block at the top of its stack (`KTHREAD.TlsData`, `StackBase` − size), which makes `[StackBase + 4 × index]` the block, as XAPI's `SetLastError` reads it; the stack starts below it. XAPI's thread start routine (`0x14BB4D`) then points the block's first word at its data and copies in the TLS template; the runtime does the same for the main thread.
2. XAPI's process initialisation, lifted (`0x151011`): critical sections, the auto power-off timer, the process heap (`RtlCreateHeap` over `NtAllocateVirtualMemory`), console settings, drive letters. The step before it in the thread procedure, `0x1517B9`, patches kernel code for known kernel builds (`cli`, writes into the kernel, `sti`); it's left out.
3. The C runtime's initializers, in XAPI's order (`0x15172C`, then `0x1516D4`). They hold only library code, no game constructors:

| Step | Entries | What |
|---|---|---|
| Table `0x32C800` | `0x16AFDD`, `0x16D401` | `_mtinit` (per-thread data), `_ioinit` (stdio handles) |
| `_FPinit` (`[0x337A88]` = `0x1673C1`) | | `_fpmath`: the float-formatting hooks `printf` calls through, x87 set-up. Without it `%f` reaches `_fptrap` and aborts |
| Table `0x32C838` | `0x167A80`, `0x1688B3`, `0x16EE9C`, `0x16C272` | C runtime |
| Table `0x32C810` | `0x16D805`; `0x197AAF`, `0x197ABB`, `0x197AC7` (XGRPH); `0x1C8D48`, `0x1C8D53`, `0x1CBAD6`, `0x1CBAE1` (DSOUND) | The C runtime's runs; the library ones are skipped, their libraries being replaced natively |

An entry runs if it's lifted (`rt::is_lifted`); otherwise `qilin_run` names it and skips it.

**Guest memory.** RAM flat from address 0: 64 MB by default, the retail Xbox's, or more with `qilin_run --ram-mb N` (a multiple of 4, up to 1024). The game reads the size from `GlobalMemoryStatus` and records 128 MB, the development kit's size, when there's more than 64 MB; testing found no change in behaviour. Guest RAM is a shared-memory object mapped into a 4 GB host reservation three times, as the Xbox's kernel maps physical memory: at 0, at `0x80000000` (contiguous memory, which D3D hands out as `Data | 0x80000000`) and at `0xF0000000` (the write-combined window D3D's surface locks return). The game writes vertices and pixels through those windows, so they're the same pages, and lifted code still addresses memory as base + address with nothing extra per access (`recomp/runtime/host/guest_ram.cpp`: `memfd` on Linux, `shm_open` on macOS, placeholder views with `MapViewOfFile3` on Windows 10 1803 and later). Pages cost nothing until touched. These two windows are the only aliases the game uses so far; the checked build stops on any other address outside RAM. Rendering at higher resolutions, SMAA and similar features don't need guest RAM: render targets and textures live in the Vulkan renderer's host memory.

| Range (for R bytes of RAM) | What |
|---|---|
| `0x00010000` … end of image | The XBE image, BSS zeroed |
| `0x80000000` + address, `0xF0000000` + address | The same RAM again: the kernel's contiguous and write-combined windows |
| End of image (64 KB aligned) … stack | Virtual memory (`host::VirtualMemory`): `NtAllocateVirtualMemory` and contiguous allocations |
| R − 0x120000 … R − 0x20000 | The main thread's stack (1 MB), with its TLS block at the top |
| R − 0x20000 | The main thread's KPCR: `fs:[0]` (the exception chain, starts as `0xFFFFFFFF`), stack limits, `Prcb`, `CurrentThread` at `+0x28` |
| R − 0x1F000 | The main thread's KTHREAD |
| R − 0x10000 | Objects behind the kernel's data exports, 256 bytes each |

**Virtual memory** works as on the Xbox: 4 KB pages, reservations on 64 KB boundaries (lowest first, or highest with `MEM_TOP_DOWN`), pages committed inside a reservation and zeroed when committed, `NtQueryVirtualMemory` reporting reserved, committed and free runs. Contiguous allocations (`MmAllocateContiguousMemory[Ex]`, and the runtime's own D3D resource headers) come from the top, within the caller's address limits and below 256 MB whatever the RAM size: physical addresses are 28 bits on the Xbox, and D3D masks resource addresses to them. On the Xbox they're mapped at `0x80000000` + physical; here the guest gets the RAM address, since the address space is flat.

`qilin_run` is built with checked memory for bring-up, so an access outside RAM stops with the guest address of the instruction.

**Kernel imports.** A function's thunk slot keeps `0x80000000 | ordinal`, which `rt::call_unlifted` recognises. A data export's slot (`KeTickCount`, `LaunchDataPage`, `XboxHardwareInfo` and 12 more) gets the address of its object: `XeImageFileName` names `\Device\CdRom0\default.xbe`, `HalDiskCachePartitionCount` is 3, and the rest start as zero. Names, ordinals, stdcall argument sizes and data/fastcall flags come from nxdk's `xboxkrnl.exe.def` (CC0, in `third_party/nxdk/`), turned into `recomp/runtime/host/kernel_exports.inc` by `tools/gen/kernel_exports.py`.

**XBE sections.** Sections without the preload flag (`$$XTIMAGE`, `$$XSIMAGE`, `.XTLID`) start as zero, as the kernel's loader leaves them, and `XeLoadSection` reads them in. Preloaded sections start with a reference count of 1, as the kernel's loader counts them, so a title's `XeLoadSection` on one (the game loads `dirtydisk_bitmap` to show its disc-error screen) only counts a reference. The counts and the shared-page counts live in the section headers, as the kernel keeps them.

**Device registers.** The one register lifted code touches is the APU's sample counter, which XACT reads; `rt::mmio_read32` answers it from the mixer's clock (`audio.md`). Any other register stops the runtime, named.

**Replacements** (`recomp/runtime/host/host.hpp`). Every call from lifted code to code that isn't lifted reaches `rt::call_unlifted`: a direct call to a stub, an indirect call the function table doesn't know, or a kernel import. It runs the replacement registered for that guest address. There's no fallback: anything unregistered stops the game and names the target and its caller (XDK library functions by name, from `xdk_symbols.inc`), for example:

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
| `RtlInitAnsiString`, `RtlEqualString`, `RtlUnicodeStringToAnsiString`, `RtlCompareMemoryUlong` | As the kernel: `RtlEqualString` folds ASCII only, `RtlUnicodeStringToAnsiString` maps Latin-1 and turns the rest into `?`. Allocating the destination string stops the runtime |
| `KeQuerySystemTime` | The host's UTC time, in 100 ns since 1601 |
| `XeLoadSection`, `XeUnloadSection` | XBE sections above |
| `RtlRaiseException` | Stops the runtime with the exception code |
| `KfRaiseIrql`, `KfLowerIrql` (fastcall), `KeRaiseIrqlToDpcLevel` | The KPCR's IRQL byte, which XAPI reads directly. With one processor and no interrupts yet, raising it only records the level; lowering it with a raise, or raising it with a lower, stops the runtime (a bug check on the Xbox) |

**The file system** (`recomp/runtime/host/kernel_io.cpp`) is the kernel's object namespace and I/O manager over host directories:

| Object | Host |
|---|---|
| `\Device\CdRom0` | The extracted disc (`--game-dir`), read-only |
| `\Device\Harddisk0\Partition1` … `6` | Folders `partition1` … `partition6` in the data directory, made when first used. 1 is E: (saves, `TDATA`/`UDATA`), 3–5 the title caches X:, Y:, Z: |
| `\Device\Harddisk0\Partition0` | The whole disk, raw. Its first 512 KB, the configuration area, is the file `partition0.bin` (sparse: unwritten sectors read as zero). It holds the cache partition database that XAPI reads and writes at start-up |
| `\??\D:` and the other drive letters | Symbolic links made by `IoCreateSymbolicLink`, followed when names resolve |

Names are matched case-insensitively, as on FATX and the disc's XDVDFS (an exact match first), relative to a handle or to `ObDosDevicesDirectory()` (`D:\…` names), and can't climb out of a device with `..`.

| Kernel function | Implementation |
|---|---|
| `NtCreateFile`, `NtOpenFile`, `NtClose` | Files, directories and raw devices, with every create disposition. Writing to the disc fails with `STATUS_MEDIA_WRITE_PROTECTED` |
| `NtReadFile`, `NtWriteFile`, `NtFlushBuffersFile` | Synchronous, at an offset or the file position. An event or APC routine stops the runtime |
| `NtQueryInformationFile`, `NtSetInformationFile`, `NtQueryFullAttributesFile` | Basic, standard, position, network-open; set position, end of file, allocation, delete-on-close |
| `NtQueryVolumeInformationFile` | Size and full-size: the partition's size from the retail partition table, less what its folder holds, within the host's free space; the disc in 2 KB sectors |
| `NtQueryDirectoryFile` | One entry per call, sorted, filtered by the mask (`*`, `?`) |
| `NtDeviceIoControlFile` | Disk geometry and partition information from the retail 8 GB disk's table. On the disc, SCSI pass-through fails as on an empty drive: XAPI's DVD authentication check at start-up ignores the result |
| `NtFsControlFile` | Dismount, which XAPI's formatter sends |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink`, `NtOpenSymbolicLinkObject`, `NtQuerySymbolicLinkObject` | The link table |
| `FscSetCacheSize` | Accepted: the host caches files, so the kernel's file cache takes no guest RAM |

**Cache partitions.** XAPI gives each title one of the three cache partitions, recorded in the configuration area, and formats it the first time: it writes a FATX volume header, FAT and root directory as raw sectors. The runtime takes the header as the format: it empties the partition's folder and keeps the header (`partitionN.fatx`), whose cluster size the volume reports from then on. This title asks for 64 KB clusters on its Z: drive (XBE init flags), and XAPI refuses to mount a volume whose cluster size differs. Raw writes to a partition that aren't part of a format, and raw reads of one, stop the runtime.

**XInput** (`recomp/runtime/host/xinput.cpp`): the controller ports with nothing plugged in, until there's an input backend. `XInitDevices`, `XGetDevices` (no devices) and `XGetDeviceChanges` (none arrive); opening a port stops the runtime.

**Direct3D** (`recomp/runtime/host/d3d.cpp`) over the Vulkan renderer: the device, its frame buffers, clears and swaps at the display's vblank. See `graphics.md`.

**DirectSound** (`recomp/runtime/host/dsound.cpp`) over the software mixer, beneath the lifted XACT. See `audio.md`.

**Where it stops now:** the game's `main` creates its graphics device on Vulkan, starts XACT (the engine, DirectSound, the effects image, the mix-bin headroom), checks its wave banks and the controller ports, loads `bundles/common.bnl` and the font, registers their resources, creates its textures, and stops at its first vertex shader: `D3DDevice_CreateVertexShader` from `0x65F37`. In v0.15.0 it stopped at the first texture. Earlier, start-up writes the title's save metadata and images to `partition1/UDATA/4d530053/`, gets cache partition 5 formatted with 64 KB clusters and mounted as Z:, and checks that `D:\loctext\english` exists. With a disc file missing, the game shows its disc-error screen, as the console does: it loads `dirtydisk_bitmap` and draws the message into the front buffer (`LockRect`). In v0.14.0 it stopped at the audio engine; in v0.13.0 at the first Direct3D call; in v0.12.0 at the first `NtOpenFile`.

## Tests

`qilin_host_tests` (CI, no game files) covers the kernel export table, a replacement called through `rt::call_unlifted` with stdcall argument popping, `describe()`, virtual memory (granularity, top-down reservations, commit zeroing, queries, release), and the file system on a scratch disc and hard disk: links, case-insensitive names, reads at offsets and to the end, the read-only disc, `..`, directory masks, creating and writing a save, attributes, formatting a cache partition, and the configuration area.

## Next

1. **Shaders and drawing** (`graphics.md`): the game creates its first vertex shader next.
2. **A window, input and audio output** on SDL3 (the controller to test against: an Xbox One pad over Bluetooth).
3. **Audio still to come** (`audio.md`): streams (streamed wave banks, music), WMA, the I3DL2 reverb, envelopes, LFOs, filters, 3D.
4. **The scheduler:** guest threads (`PsCreateSystemThreadEx`), waits, events, timers and DPCs. Until then a second thread stops the runtime.
5. **Keep running `qilin_run`** and implement each function it stops at.
