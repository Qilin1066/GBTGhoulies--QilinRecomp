# default.xbe — first analysis

Source: `Grabbed by the Ghoulies (USA) (En,Fr,De,Es,It)` dump, `default.xbe` (4,554,752 bytes).
Generated with `tools/xbeinfo.py`; raw output in `analysis/xbeinfo.json`.

## Identity
- **Type:** retail XBE (entry point and kernel thunk decode with the retail XOR keys)
- **Title:** Grabbed by the Ghoulies, title ID `4D530053` (MS-083), region `1` (North America), version 1
- **Build timestamp:** 2003-09-24 17:54:09 UTC
- **Debug path left in the header:** `c:\source_24.09.03.0028\Ghoulies\game\XBox_Release\GrabbedByTheGhoulies.exe`
  (the build was made from a source snapshot dated 24.09.03, matching the timestamp)

## Memory layout
- Base `0x00010000`, image size 5,644,160 bytes (~5.4 MB)
- Entry point `0x0014FAE9` (in `.text`)
- Kernel thunk table `0x00202EA0` (start of `.rdata`), TLS directory `0x00203468`
- Stack commit 64 KB; heap reserve 1 MB / commit 4 KB; init flags `0x80000005`

## Sections
| Section | Size | What it is | Plan |
|---|---|---|---|
| `.text` | 1,408 KB | Game code + statically linked XAPI, LIBCMT, LIBCPMT | Lift (game); replace XAPI/CRT by signature |
| `.rdata` | 1,190 KB | Constants, kernel thunks | Data |
| `.data` | 2,188 KB virtual (1,055 KB on disk) | Globals; rest is BSS | Data |
| `D3D` | 77.5 KB | Direct3D 8 (Xbox) | **Replace** → Vulkan layer |
| `D3DX` | 3.6 KB | D3DX helpers | Replace or lift |
| `XGRPH` | 129.4 KB | XGraphics (texture swizzle, etc.) | Replace (swizzle) or lift |
| `DSOUND` | 121.3 KB | DirectSound | **Replace** → SDL/miniaudio |
| `XACTENG` | 43.0 KB | XACT audio engine (wave/sound banks) | Replace, or lift on top of our DSOUND |
| `WMADEC` | 100.1 KB | WMA decoder | Lift, or replace with FFmpeg |
| `XPP` | 24.2 KB | Peripherals (controllers, memory units) | **Replace** → SDL input |
| `DOLBY` | 28.4 KB | Dolby Digital encoder (DSP code for the audio APU) | Drop: run stereo/surround on the host |
| `BINK*` (16 sections) + `BINKDATA` | ~91 KB + 61.5 KB | RAD Bink video decoder + blitters | Lift (likely MMX-heavy) or replace with FFmpeg's Bink decoder |
| `$$XTIMAGE`, `$$XSIMAGE`, `dirtydisk_bitmap`, `.XTLID` | ~48 KB | Dashboard icons, dirty-disc screen, title IDs | Not code |

**Key point:** most of the Microsoft libraries sit in their own named sections, so their boundaries are known for free. Only XAPI and the C/C++ runtime are mixed into `.text` and need signature matching.

## XDK libraries
All nine libraries are the same build, **XDK 1.0.5659 QFE 1** (approved, non-debug):
XAPILIB, D3D8, D3DX8, XGRAPHC, XBOXKRNL, LIBCMT, LIBCPMT, XACTENG, DSOUND.
One XDK version throughout makes signature matching much simpler.

## Kernel imports — 116 functions
See `analysis/kernel_imports.md` for the full list by subsystem. Summary:
- NT syscalls (files, sync, memory, threads): 28
- Kernel scheduler / sync / time (Ke/Kf): 23
- Memory manager (Mm): 12
- Rtl runtime: 10
- HAL: 7; Ex: 6; data exports/keys: 6; Xc crypto: 5; Av video: 4; Io: 4; Xe section loading: 4; Ps threads: 3; Ob: 2; Fsc: 1; Dbg: 1

Many of these exist only for the libraries we're replacing (interrupt hookup, `MmClaimGpuInstanceMemory`, PCI space, Av display modes, contiguous memory for GPU/APU). Once D3D/DSOUND are replaced at API level, the kernel surface the game and XAPI actually need is noticeably smaller. Crypto (SHA/HMAC/signature) is most likely save-game signing.

## Other notes
- `movie/` assets (hash-named) are presumably Bink videos, given the BINK sections; `xwavebank/` holds XACT wave banks (one is 298 MB, probably music), and the WMADEC section suggests WMA-compressed audio. To be confirmed by inspecting file headers.
- `cdx.xbe` + `cdxmedia/`, `Videos/` and `Other PNGs/` are the bonus demo/trailer launcher and its content, not part of the game.
- Asset and folder names are 32-bit hashes; the game's file-loading code will reveal the hash function.
