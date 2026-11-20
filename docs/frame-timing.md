# Frame timing in Grabbed by the Ghoulies (default.xbe, US)

Found 2026-11-17 by static analysis (`tools/cfg.py` + LLVM disassembly). Addresses are virtual addresses in the retail XBE.

## Clock sources
- The game never reads the hardware directly. It uses XAPI's `QueryPerformanceCounter` (`0x14C29C`, `rdtsc`) and the frequency captured at startup.
- `main` (`0x32390`) stores the start count at `0x50E0D8` and the frequency at `0x50E0E0`, runs init, then loops forever calling the per-frame function **`0x33B50`**.
- `0x1077D0` = seconds since start (x87 result: `(QPC - start) / freq`).

## Delta time: variable timestep, driven by vertical blanks
- **`0xFC3B0`** is a D3D vertical-blank/swap callback (its address is stored as an immediate at `0xFC3A1`, i.e. registered with D3D). Unless paused (`0x4E5CD8`/`0x4E5CDC` flags), it increments a vblank counter (`0x5096D8`). When the callback data says a frame was presented (flag bit 0), it stores **vblanks since the previous present** in `0x5096D0` and remembers the current count in `0x5096D4`.
- **`0xFC030`** is the first call of every frame:
  - `rate` = 50 (PAL mode, flags `0x5096E0`/`0x5096E4`) or 60.
  - `dt = vblanks / rate`, clamped to `5.0 / rate` (≈83 ms at 60 Hz) → **`0x5096C0`** (float seconds). Raw QPC delta since start → `0x5096BC`.
  - `x = dt * 300 + carry` → integer ticks this frame **`0x5096C4`**, fractional carry **`0x5096C8`** (a 300 Hz tick accumulator).
- `dt` (`0x5096C0`) is consumed across the frame, e.g. countdown timers `t -= dt` in `0x33B50`, and passed into the Rare audio layer (`0x15B780`).
- On the Xbox the game presents every 2nd vblank → dt = 2/60 (30 fps).

## Busy-wait loops (replace for efficiency)
- **`0x32680`** spins on `QueryPerformanceCounter` until a target time. It burns a full CPU core.
- **`0x32700`** (called from 8 game functions, likely loading/transition screens) loops at 50/60 Hz using that spin-wait.
- In the port these become real sleeps (`std::this_thread::sleep_until` with a short final spin), important on low-end boards and laptops.

## The frame rate unlocked (v0.40.0)

`0xFC030` is replaced natively (`frame_clock` in `recomp/runtime/host/d3d.cpp`): with the settings' `frame_rate = original` it computes exactly what the original does (vblanks since the last present over the rate, the 5-vblank clamp, the 300 Hz ticks, the counter's low word at `0x5096BC`); with `60`, `display` or `unlimited` the host clock's time since its last call stands in for the vblanks (the first frame two vblanks, as the console's), with the same clamp and ticks. Nothing in the game reads the tick count, and the step is passed on as a float, so the game moves by real time at any rate.

**Pacing** (`frame_pacing`, at the end of `Swap`): original, the console's vblank after the game's interval (2: 30 fps); 60, sixty a second by the clock; display, the screen's refresh rate (`Window::refresh_rate`), with vsync by taking the window's next image early (`Renderer::acquire_early`: it waits for the refresh, so the game reads its input just after one); unlimited, no wait without vsync. Every frame gives up the guest processor for 2 ms at least, and the wait for the display is a sleep until just before the refresh: the game's other threads run only while the main one waits, as on the console in Swap's vblank wait. The vblank callback still runs for each real vblank that passed.

**Latent bugs a shorter step finds.** The console never gave the game a step under 1/30 s. The screen transitions' updates (`0x12BC80`, `0x12BDD0`) count a timer down only while it's above zero and move on only once it's below: a timer landing on exactly zero stalls them for good. The transition opened at `0x12BD2C` starts at 1/60, so a step of exactly 1/60 left the game on its loading screen before the first fight. Both are replaced natively (`game_fixes.cpp`), as the originals but for a timer landing on zero counting as just past it. Checked at 30, 60 and unlocked (lavapipe, about 40 frames a second in the menus, 18 in the fight): the story, the loading and the first fight play through. Other code that assumes a long step may still turn up; `frame_rate = original` is the console's timing exactly. `QILIN_FIXED_STEP=seconds[,after]` gives the game that step every frame (from `after` seconds in) whatever the time, to try a 144 Hz step (0.00694) on a machine that can't draw that fast.

## Consequences for the port
1. **High refresh rate (144 Hz etc.)** (done in v0.40.0, above): override `0xFC030` with host C++ that sets `dt` from real elapsed time (high-resolution clock), keeps the 5-frame clamp and runs the same 300 Hz tick accumulator. No render interpolation needed for the common case. The existing Windows Ghoulies recomp also runs at 60 fps/uncapped at correct speed after replacing its frame timer, which supports this.
2. **Risk:** the Xbox never produced dt < 1/60 s. Any code that applies a per-frame (not per-dt) factor, such as damping, would run faster at 144 Hz. Mitigation: playtest at 30/60/144/240 Hz against xemu; keep an optional fixed-step mode (logic at 60 Hz plus render interpolation) as a fallback if specific systems misbehave.
3. **Vsync/presentation:** our Vulkan presenter calls the game's vblank callback semantics directly (or the override bypasses them); frame pacing comes from the swapchain (FIFO / mailbox / VRR), not from spin loops.
