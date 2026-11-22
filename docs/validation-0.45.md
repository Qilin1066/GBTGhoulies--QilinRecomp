# 0.45.0 validation

This pass follows [0.44.0's reliability work](validation-0.44.md). It adds
checked capture output, timer corrections and explicit mixer shutdown. It also
fixes the Windows compile regression observed in 0.44.0's MSVC and LLVM-MinGW
build logs: a path helper unintentionally pulled the Windows `far` macro into
the HRTF code. Path conversion is now independent of Windows API headers.

Local Linux x64/GCC 14, warnings as errors, 22 November 2026:

| Check | Result |
|---|---|
| Full player entry point and dispatch link (empty game table) | Passed |
| Core/loader/helper tests | Passed |
| 200,000-case x87 consistency check | Passed |
| Audio engine and WAV capture tests | Passed |
| Host/kernel/timer/mixer integration tests | Passed |
| SDL platform regression tests | Passed |
| Vulkan rendering | Skipped: no working Vulkan device locally |
| AddressSanitizer + UndefinedBehaviorSanitizer, core/audio/host/platform suites | All four passed |

LeakSanitizer is disabled in the ptrace-based execution environment.

The new timer checks exercise arithmetic and guest timer structures without
starting a scheduler clock thread. They check epoch conversion, cancellation,
`INT64_MIN`, saturated deadlines and phase-preserving periodic catch-up.

WAV tests verify headers while open and after close, Unicode filenames,
NaN/infinity encoding, invalid destinations, refusal above RIFF's limit before
reading/allocating samples, and an actual `/dev/full` failure on Linux. The
mixer integration test invokes the native DirectSoundCreate replacement,
starts the real worker, forces a catch-up, joins it, finalises its capture and
repeats shutdown safely. It links only empty guest-dispatch fixtures.

Windows CI and the download workflow must confirm the Windows build fix and
run the Windows launcher tests. No new real-disc differential check,
end-to-end playthrough, physical controller/audio test or GPU comparison is
claimed. The remaining completion evidence is listed in the
[previous validation record](validation-0.44.md#what-is-still-needed-for-a-100-game-claim).

Reproduction commands are unchanged from that record. The release build uses
`QILIN_WERROR=ON`; instrumented checks use `QILIN_SANITIZE=address,undefined`
and `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`.
