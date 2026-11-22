# 0.44.0 reliability validation

This pass addresses host/runtime reliability. It does not certify complete
whole-game compatibility. Full coverage of lifted code means the recompiler
accounts for that code; it does not prove every scene, native replacement,
graphics path or audio path behaves correctly.

## Checked on 22 November 2026

Linux x64, GCC 14, release build with warnings treated as errors:

| Check | Result |
|---|---|
| Full player entry point and dispatch linked with empty function-table fixture | Passed; not a playable game |
| Recompiler/loader/runtime-helper unit suite | Passed, including synthetic XBE load from a Unicode path |
| x87 consistency, 200,000 cases | Passed |
| Audio engine unit suite | Passed, including zero-length mix/submix handling |
| Kernel/runtime host tests | Passed, including five save generations, backup failure before truncation and in-place write, append, invalid offsets, short information structures, growth seen by read-only handles, and real `/dev/full` write/flush/close failure |
| SDL platform tests | Passed: settings replacement, BOM, Unicode path, audio outage/recovery, virtual rumble stop and subsystem ownership |
| Vulkan renderer | Skipped locally: no working Vulkan device |
| AddressSanitizer + UndefinedBehaviorSanitizer | All four selected suites passed: core, host, audio and platform |

LeakSanitizer is disabled in this environment because it runs under ptrace.
The sanitizer run exposed an empty-buffer reference in zero-length audio mixes;
it was fixed without removing the tests' intentional gain priming.

Windows CI additionally runs the launcher fixture. It uses mock lifter/ninja
children and an empty disc layout: no game data, compiler certification or
physical controller/audio certification is implied. The test checks changed
kit contents at the same version, preserving a locked executable/stamp, then
launching a child with Unicode, spaces, `%PATH%` and `&` in the disc path.
Cross-platform CI compiles the complete player; Linux CI treats inability to
initialise Vulkan as a failure.

## What is still needed for a 100% game claim

The extracted supported USA disc was not available in this workspace. The
historical Windows `D:` path is not mounted here. No new ROM-based differential
run, visual comparison or gameplay completion is claimed by this pass.

| Evidence | Required checks |
|---|---|
| Recompiler with the real disc | Verify the supported XBE, regenerate all code without failures, rerun decoder/coverage and Unicorn differential checks |
| Complete original-spec playthrough | Every chapter and ending; transitions, cutscenes, fights, pickups and interactions; compare against the original game/xemu where behaviour differs |
| Saves | All save slots, create/load/overwrite/delete, restart and reload; restore related files together from history |
| Graphics | GTX 1080 Ti/NVIDIA shadow stripes and pickup rendering, plus other supported GPU/OS combinations; F9 full-resolution captures and validation logs |
| Timing | Original rate first, then 60/display/unlimited and aspect/resolution options; check game speed and timing-dependent sequences |
| Sound/input | Real device disconnect/reconnect, stereo/5.1, controller reconnect and rumble; capture logs across long sessions |
| Low-end hardware | Sustained play and scene-loading behaviour on the project's reference hardware |

Save history is per-file and per-open-handle, not a transaction spanning a group
of save files. Backups may add disk I/O during saves. They preserve the prior
file before destructive work and refuse the mutation if the backup fails;
they do not prove that the game's new save contents are logically valid.

## Reproduce the checks without game files

```sh
cmake -S . -B build -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DQILIN_SANITIZE=address,undefined
cmake --build build-sanitize --target qilin_tests qilin_host_tests \
  qilin_audio_tests qilin_platform_tests --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize \
  -R '^(qilin_tests|qilin_host_tests|qilin_audio_tests|qilin_platform_tests)$' \
  --output-on-failure
```
