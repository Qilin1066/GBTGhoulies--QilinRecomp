#!/usr/bin/env python3
"""Dump XBE code sections and disassemble them with GNU objdump.

Usage: disasm.py default.xbe outdir
Writes outdir/<section>_<va>.bin and outdir/<section>_<va>.asm (intel syntax),
plus outdir/image.bin (flat memory image from base, BSS zero-filled) and outdir/sections.json.
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbeinfo import Xbe  # noqa: E402

# Sections that hold x86 code. DOLBY is APU DSP microcode, not x86.
NON_CODE = {".rdata", ".data", "BINKDATA", "DOLBY", "$$XTIMAGE", "$$XSIMAGE",
            "dirtydisk_bitmap", ".XTLID"}


def main():
    xbe_path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    data = open(xbe_path, "rb").read()
    x = Xbe(data)

    # flat image covering base .. end of last section
    end = max(s["va"] + s["vsize"] for s in x.sections)
    img = bytearray(end - x.base)
    img[0:x.size_headers] = data[0:x.size_headers]
    for s in x.sections:
        o = s["va"] - x.base
        img[o:o + s["rsize"]] = data[s["raw"]:s["raw"] + s["rsize"]]
    open(os.path.join(out, "image.bin"), "wb").write(img)

    meta = []
    for s in x.sections:
        code = s["name"] not in NON_CODE
        tag = f'{s["name"].strip(".$")}_{s["va"]:08X}'
        meta.append(dict(name=s["name"], tag=tag, va=s["va"], vsize=s["vsize"], rsize=s["rsize"], code=code))
        if not code:
            continue
        binp = os.path.join(out, tag + ".bin")
        open(binp, "wb").write(data[s["raw"]:s["raw"] + s["rsize"]])
        asm = subprocess.run(
            ["objdump", "-D", "-b", "binary", "-m", "i386", "-M", "intel",
             "--no-show-raw-insn", f"--adjust-vma={s['va']:#x}", binp],
            check=True, capture_output=True, text=True).stdout
        open(os.path.join(out, tag + ".asm"), "w").write(asm)
        print(f"{tag:24} {asm.count(chr(10)):8} lines")
    json.dump(dict(base=x.base, entry=x.entry, kthunk=x.kthunk, sections=meta),
              open(os.path.join(out, "sections.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
