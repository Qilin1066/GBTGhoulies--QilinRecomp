# 0.48.0 validation

This update integrates the runtime and launcher fixes added to main after
[0.47.0](validation-0.47.md), then fixes two issues found in that new code.
No disc or complete-game playthrough is available here.

## Changes checked

DirectSound-owned buffers previously cleared only complete four-byte words.
The new regression creates buffers of 1, 2, 3, 5, 6, 7 and 4,099 bytes through
native XDK replacements, locks them, dirties every byte, releases them and
creates the same size again. It checks that the allocation is reused without
increasing reserved guest memory, every requested byte is zero, and a wrapping
lock returns the correct tail/head addresses and lengths. The mixer remains
running during these calls, but the buffers are not played; this test does not
establish in-game use or sound quality.

Guest RAM mapping previously reinstalled the Linux/macOS signal handler each
time, replacing its saved predecessor with itself. A non-guest fault after a
second mapping could therefore recurse rather than reach the original handler.
The handler is now installed once; Windows also retains one vectored handler.
Two isolated child processes install a pre-existing SA_SIGINFO handler, map RAM
twice and raise SIGSEGV or SIGBUS outside the reservation. Each must exit through
the predecessor with code 42. A five-second alarm bounds a broken chain. Existing
tests still check that an uncommitted guest write reports its guest address and
exits with code 3. Windows forwarding is not covered by these POSIX tests.

The integrated commits also introduce optional checked guest accesses
(`-DQILIN_CHECKED_MEMORY=ON`, unchecked by default), move the initial x87 control
word before the scheduler copies main-thread state, and add DirectSound-owned
buffer allocation/reuse. This update does not independently benchmark their
performance or certify all fault-handler behaviour. The launcher uses streamed
.NET hashing and its Windows fixture resolves Chocolatey's Ninja shim before
copying the real executable. Main's CI passed after those integrated fixes.

## Validation

Locally checked on Linux x64/GCC 14 with warnings as errors:

| Check | Result |
|---|---|
| Complete player/dispatch link with empty game table | Passed |
| Core/loader helpers and 200,000-case x87 consistency | Passed |
| Audio engine/capture suite | Passed |
| Host/kernel/timer/mixer suite, including new regressions | Passed |
| SDL platform/controller/settings/file-copy suite | Passed |
| Vulkan renderer and host Direct3D lifetime | Skipped locally: no working Vulkan device |
| AddressSanitizer + UndefinedBehaviorSanitizer, core/audio/host/platform | All four passed |

LeakSanitizer is disabled because this environment uses ptrace. CI builds with
warnings as errors on Linux GCC/Clang x64, Linux ARM64, macOS Intel/Apple Silicon
and Windows MSVC. Linux requires both Vulkan tests; Windows requires the
PowerShell/Ninja launcher regression. The Windows download is also built and
linked without game data by the separate packaging workflow.

Reproduction commands are in [the prior record](validation-0.47.md#reproduction-and-remaining-checks).
These checks do not replace a real-disc playthrough, physical controller/audio
checks or testing on the player's GPU. The [remaining completion checks](validation-0.44.md#what-is-still-needed-for-a-100-game-claim)
continue to apply.
