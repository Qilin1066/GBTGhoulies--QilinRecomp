#!/usr/bin/env python3
"""Instruction-set profile per code region (what the lifter must support).

Usage: profile.py <disdir> <functions.json> [out.json]
"""
import json
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import load_listing  # noqa: E402

# .text sub-regions found by alignment / string / call-graph analysis
TEXT_REGIONS = [
    ("game", 0x11000, 0x14A840),  # XAPI starts with IsBadReadPtr at 0x14A840
    ("xapi", 0x14A840, 0x152780),
    ("rare-libs", 0x152780, 0x165C00),
    ("crt", 0x165C00, 0x170D80),
    ("game-eh-funclets", 0x170D80, 0x171020),
]

MMX = re.compile(r"^(p(add|sub|mul|madd|and|andn|or|xor|cmp|sll|srl|sra|unpck|ack|avg|max|min|sad|shuf|insr|extr|movmsk)\w*|movq|movd|emms|packss\w*|packus\w*|punpck\w*)$")
SSE = re.compile(r"^(\w+ps|\w+ss|movhlps|movlhps|ldmxcsr|stmxcsr|prefetch\w*|sfence|cvt\w+|movntq|movntps|maskmovq|unpck[lh]ps|ucomiss|comiss)$")
X87 = re.compile(r"^f\w+$")
AMD3D = re.compile(r"^(pf\w+|pi2f\w|pf2i\w|femms|pavgusb|pmulhrw|prefetchw)$")


def classify(m, ops):
    base = m.split()[-1]
    if base in ("femms",) or AMD3D.match(base):
        return "3dnow"
    if X87.match(base) and base not in ("fs", "fwait"):
        return "x87"
    if base in ("fwait", "wait"):
        return "x87"
    if SSE.match(base) and base not in ("movss",) or base == "movss":
        if base.startswith("cvt") and "xmm" not in ops and "mm" not in ops:
            return None
        return "sse"
    if MMX.match(base):
        # movd/movq with xmm count as sse
        if "xmm" in ops:
            return "sse"
        return "mmx"
    if m.startswith("rep") or base in ("movs", "stos", "lods", "scas", "cmps") or base[:4] in ("movs", "stos", "lods", "scas", "cmps") and base not in ("movsx", "movss", "movsd", "movsb") and len(base) <= 5:
        return "string"
    if base in ("in", "out", "cli", "sti", "hlt", "wbinvd", "invd", "rdmsr", "wrmsr", "lidt", "lgdt", "invlpg") or base.startswith("mov") and re.search(r"\bcr\d|\bdr\d", ops):
        return "privileged"
    if base in ("cpuid", "rdtsc", "rdpmc"):
        return "cpuid/rdtsc"
    if base == "int3" or base == "int":
        return None
    if base in ("call", "jmp") and not re.fullmatch(r"0x[0-9a-f]+", ops):
        if "*4+0x" in ops:
            return "jumptable" if base == "jmp" else "indirect-call"
        return "indirect-" + base
    if "fs:" in ops:
        return "fs-segment"
    return None


def main():
    disdir = sys.argv[1]
    funcs = json.load(open(sys.argv[2]))["functions"]
    meta = json.load(open(os.path.join(disdir, "sections.json")))

    def region(a, sec):
        if sec != ".text":
            return sec if not sec.startswith("BINK") else "BINK*"
        for n, lo, hi in TEXT_REGIONS:
            if lo <= a < hi:
                return n
        return ".text?"

    total = Counter()
    cats = defaultdict(Counter)
    mnem_by_cat = defaultdict(Counter)
    for s in meta["sections"]:
        if not s["code"]:
            continue
        for a, m, ops in load_listing(os.path.join(disdir, s["tag"] + ".asm")):
            if m in ("int3", "nop", "(bad)"):
                continue
            r = region(a, s["name"])
            total[r] += 1
            c = classify(m, ops)
            if c:
                cats[r][c] += 1
                if r == "game":
                    mnem_by_cat[c][m.split()[-1]] += 1

    order = ["x87", "sse", "mmx", "3dnow", "string", "indirect-call", "indirect-jmp", "jumptable", "fs-segment", "cpuid/rdtsc", "privileged"]
    print(f"{'region':18}{'insns':>9} " + " ".join(f"{c:>13}" for c in order))
    for r in sorted(total, key=lambda r: -total[r]):
        print(f"{r:18}{total[r]:9} " + " ".join(f"{cats[r][c]:13}" for c in order))
    print()
    for c in order:
        if mnem_by_cat[c]:
            print(f"game {c:14}", ", ".join(f"{k}:{v}" for k, v in mnem_by_cat[c].most_common(14)))
    if len(sys.argv) > 3:
        json.dump(dict(totals=total, categories=cats, game_mnemonics=mnem_by_cat), open(sys.argv[3], "w"), indent=1)


if __name__ == "__main__":
    main()
