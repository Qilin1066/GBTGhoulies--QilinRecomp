# 0.47.0 validation

This pass fixes stale player-kit rebuilds and Direct3D device shutdown. It
follows [0.46.0](validation-0.46.md), without changing lifted instructions,
rendering algorithms or game timing.

Locally checked on Linux x64/GCC 14 with warnings as errors:

| Check | Result |
|---|---|
| Complete player/dispatch link with empty game table | Passed |
| Core/loader helpers | Passed |
| 200,000-case x87 consistency | Passed |
| Audio engine/capture tests | Passed |
| Host/kernel/timer/mixer tests | Passed |
| SDL platform/controller/settings/file-copy tests | Passed |
| Vulkan renderer and host Direct3D lifetime | Skipped locally: no working Vulkan device |
| AddressSanitizer + UndefinedBehaviorSanitizer, core/audio/host/platform suites | All four passed |

LeakSanitizer remains disabled because the execution environment uses ptrace.
CI builds with warnings as errors on Linux GCC/Clang x64, Linux ARM64, macOS
Intel/Apple Silicon and MSVC, plus the Windows download toolchain. Linux CI
requires both Vulkan tests to run. Windows CI requires PowerShell and Ninja
so the launcher check cannot disappear because a tool is absent.

## Launcher regression

The test runs the real launcher with real Ninja, a synthetic compiler and a
synthetic disc. The compiler fixture records compile/link calls and puts
input identity into its outputs. It does not compile game code. The test
checks:

- A changed archive with an old timestamp changes the installed executable,
  without recompiling unchanged guest objects.
- Changed runtime headers or compiler bytes with old timestamps recompile.
- Changed disc bytes with unchanged certificate/timestamp metadata recompile.
- An unchanged invocation performs neither compilation nor linking.
- `-Rebuild` recompiles, despite unchanged inputs.
- A locked installation preserves the working executable and stamp; recovery
  succeeds, and the launched child receives literal Unicode, spaces, `%PATH%`
  and `&` in its disc path.

Hashes include relative header names, so adding/removing headers also changes
compilation identity. The compiler executable is hashed; this is not a hash
of every toolchain DLL or a test of a complete real-disc build.

## Direct3D lifetime regression

A separate process creates a real headless Vulkan-backed host Direct3D device
through its native replacement, using synthetic guest RAM and no-op replacements
for three lifted startup helpers. It shuts down twice, checks that the
pipeline-library file exists and can be renamed/removed, then creates another
device and repeats. This catches a stale global device and exercises renderer
teardown. File closure also matters on Windows, where an open stream prevents
renaming. No window, menu interaction or game drawing is simulated by this test.

The runtime now explicitly destroys the menu before the renderer and the
renderer before automatic window/controller objects. Its existing destructor
joins the library worker, waits for GPU work and writes pending pipeline-cache
data. This does not certify whole-game behaviour, every fatal-exit path or
physical-device teardown on a player's machine.

## Reproduction and remaining checks

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --build build-sanitize --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure \
  -R '^qilin_(tests|audio_tests|host_tests|platform_tests)$'
```

The sanitizer build is configured with `-DQILIN_SANITIZE=address,undefined`.
See the [earlier validation record](validation-0.44.md#what-is-still-needed-for-a-100-game-claim)
for real-disc playthrough and remaining completion checks. No disc is available
in this environment, so this version makes no 100% whole-game compatibility claim.
