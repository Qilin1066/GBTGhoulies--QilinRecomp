# Recompiler core: status (roadmap step 2)

Last updated 2026-11-17.

## Done

### XBE loader (`recomp/src/xbe.*`)
It parses the image header, certificate, section table, library versions and kernel thunk table, and it builds the flat guest image (BSS zero-filled). It throws on anything malformed.

**Check:** `qilin info` output is identical, field for field, to the Python prototype's `xbeinfo.json`.

### Decoder (`recomp/src/decoder.*`)
This is Zydis v4.1.1 in 32-bit legacy mode, vendored. Instructions are cached in a compact form, about 100 bytes each, so all ~530k reachable instructions fit comfortably in memory.

**Cross-check against LLVM 18** (`tools/check/verify_decoder.py`):

| Set | Instructions | Result |
|---|---|---|
| Every reachable instruction | 532,837 | **100% agreement** on length and mnemonic |
| Linear sweep of every code section, data included | 660,357 | No unexplained disagreement; 238 known-benign cases, all in data bytes |

The 238 known-benign cases are:
- LLVM rejects undocumented aliases that Zydis accepts (`SAL /6`, `TEST /1`, `int1`, `bound`, ...).
- LLVM returns a lone `lock` prefix.
- Zydis rightly rejects encodings a Pentium III can't run (`mov cs`, EVEX).

The naming differences between the two libraries are mapped explicitly in the script. Examples: `fcomip`/`fcompi`, `popad`/`popal`, and Zydis's `ret far` versus LLVM's `retf`.

### Function discovery + CFG (`recomp/src/cfg.*`)
This is a C++ port of `tools/analysis/cfg.py` and `analyze.py`. It works on Zydis's structured operands instead of disassembly text. It takes about 1 second on x86-64.

| | Python prototype | C++ port |
|---|---|---|
| Functions | 9,402 | 9,286 |
| Reachable instructions | 530,764 | 532,837 |
| `.text` coverage of non-padding bytes | 97.7% | 98.1% |
| Jump tables resolved | 556 | 558 (313 cmp, 200 cmp + byte table, 45 heuristic) |
| Unresolved indirect jumps | 24 | 29 |
| Indirect calls | ~1,960 | 1,993 |

**Where the two disagree, the port is better in every case examined:**
- **Indexed table addresses are no longer code-pointer seeds.** The prototype's text regex treated the table address in `jmp [4*reg + table]` as a possible function pointer, so jump tables were decoded as code.
- **Misaligned seeds are pruned.** Some data values are 16-aligned and look like code pointers, but land inside another function's instruction. For example, `0x80000` is the `CC` byte inside `mov ecx, [0x4275CC]`. 47 such seeds are dropped, iterating until stable.
- **Fewer split functions.** 146 of the 154 prototype-only "functions" are instructions inside a larger function in the port. The one checked by hand, `0x26F60`, is an ordinary branch target that the prototype split off because of its seeding order. Three of the remaining eight were checked and are mid-instruction starts.

**Checks:**
- On ARM64 (GCC cross-build, run under qemu), the output is byte-identical to x86-64 (Clang).
- Both compilers build it with no warnings at `-Wall -Wextra -Wpedantic`.

## Known issues (to resolve before or in the lifter)

1. **22 far `call`/`jmp`/`ret` in "reachable" code.** Retail game code never uses far control flow, so each one marks a bogus function: data decoded as code, mostly `orphan`/`imm-ptr` seeds in DSOUND/WMADEC/XGRPH. The lifter will reject far control flow as an error, so these must be removed first. **Plan:** a validation pass that drops low-confidence seeds (anything other than call targets) containing privileged, far or undocumented instructions.
2. **15 functions with decode failures, 13 with self-overlapping instructions.** These are the same kind of thing (data decoded as code) and get the same plan.
3. **7 direct call targets land inside another function's instruction.** Either the caller is bogus (data) or there's genuine overlapping code. They need to be listed and checked by hand.
4. **Library sections (DSOUND 35%, WMADEC 37% coverage).** These get replaced, not lifted, so their low coverage doesn't block anything. Only the game region (`0x11000–0x14A895`) and the Rare libraries (`0x152780–0x165C00`) need complete coverage.
5. **Cross-check with the Ghoulies PC Recomp seed list.** Not done yet (see `prior-art.md`).

## Next

1. A **validation pass** for the known issues above.
2. **Lifter, first slice:** guest registers held as host locals, flags computed only where they're read (dataflow), scalar SSE mapped to `float`, integer ops, direct calls, jump tables mapped to `switch`. It emits C++ for leaf functions.
3. A **differential test harness:** run lifted leaf functions against the original x86 bytes executed under an x86 CPU emulator (Unicorn, or i386 on the CI host), comparing registers, flags and memory.
