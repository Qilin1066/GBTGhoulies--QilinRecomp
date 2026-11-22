# 0.46.0 validation

This pass fixes memory-range handling, controller promotion and save-history
copy memory use. It follows [0.45.1](validation-0.45.md) and changes no rendering,
game timing or lifted instructions.

Locally checked on Linux x64/GCC 14 with warnings as errors:

| Check | Result |
|---|---|
| Complete player/dispatch link with empty game table | Passed |
| Core/loader helpers | Passed |
| 200,000-case x87 consistency | Passed |
| Audio engine/capture tests | Passed |
| Host/kernel/timer/mixer tests | Passed |
| SDL platform/controller/settings/file-copy tests | Passed |
| Vulkan renderer | Skipped locally: no working Vulkan device |
| AddressSanitizer + UndefinedBehaviorSanitizer, core/audio/host/platform suites | All four passed |

LeakSanitizer remains disabled because the execution environment uses ptrace.
CI exercises the same changes across Linux GCC/Clang x64, Linux ARM64, macOS
Intel/Apple Silicon and MSVC, plus the Windows download toolchain. Linux CI
requires a working Vulkan test.

## New regressions covered

Memory tests reserve an unaligned fixed range spanning page boundaries, access
its final page, recommit without losing contents, reject zero/overflowing or
out-of-range requests without altering state, and verify zero-length whole
reservation decommit followed by zero-filled recommit. Explicit 4 KiB alignment
is tested with adjacent allocations. Native kernel calls verify allocation
and decommit output parameters, overflow refusal and cleanup after simulated
physical-memory exhaustion. These use synthetic guest RAM, not a game disc.

Snapshot tests compare patterned binary data over several copy chunks, replace
an existing destination, copy an empty file, copy a file to itself and check
that missing/invalid sources or blocked destinations preserve existing data
and leave no temporary file. Existing tests still verify five generations
and refusing a save mutation when its backup cannot be made. Backup work is
included in file-I/O timing before the new save is written.

Five virtual gamepads exercise the four-port limit: after port 0 disconnects,
the waiting pad's button is read on port 0, while the other pads retain their
ports. Removing all pads leaves the keyboard on port 0.

## References and remaining checks

The NT-style memory behaviour is checked against Microsoft's descriptions of
[page coverage during allocation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
and [decommit ranges and zero-sized whole-region decommit](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwfreevirtualmemory).
The project's existing 64 KiB reservation policy is retained; these references
are not a certification of every Xbox kernel behaviour.

Save history still operates per file and per open handle. Streaming copies
bound host memory use, but do not turn related save files into a transaction
or guarantee a logically valid game save. Physical controller reconnects,
real-disc gameplay and end-to-end completion remain unverified here; see the
[remaining completion checks](validation-0.44.md#what-is-still-needed-for-a-100-game-claim).
Reproduction commands are in the prior validation records.
