#!/usr/bin/env python3
"""Function discovery + per-function stats over objdump listings from disasm.py.

Usage: analyze.py <disdir> <xboxkrnl_exports.txt> <out.json>
"""
import json
import os
import re
import struct
import sys
from bisect import bisect_right
from collections import Counter, defaultdict

LINE = re.compile(r"^\s*([0-9a-f]+):\t(.*)$")
HEX = re.compile(r"0x[0-9a-f]+")

PREFIXES = {"rep", "repz", "repnz", "repe", "repne", "lock", "data16", "addr16", "fs", "gs", "es", "cs", "ss", "ds", "bnd", "notrack"}


def load_listing(path):
    insns = []  # (addr, mnem, ops)
    for ln in open(path):
        m = LINE.match(ln)
        if not m:
            continue
        addr = int(m.group(1), 16)
        txt = m.group(2).strip()
        parts = txt.split(None, 1)
        mnem = parts[0] if parts else ""
        ops = parts[1] if len(parts) > 1 else ""
        # fold prefixes into mnemonic key
        while mnem in PREFIXES and ops:
            p2 = ops.split(None, 1)
            mnem = mnem + " " + p2[0]
            ops = p2[1] if len(p2) > 1 else ""
        insns.append((addr, mnem, ops))
    return insns


def main():
    disdir, kexp, outp = sys.argv[1:4]
    meta = json.load(open(os.path.join(disdir, "sections.json")))
    base, kthunk = meta["base"], meta["kthunk"]
    img = open(os.path.join(disdir, "image.bin"), "rb").read()
    knames = {int(l.split()[0]): l.split()[1] for l in open(kexp) if l[:1].isdigit()}

    # kernel thunk slots -> names
    slots = {}
    o = kthunk - base
    while True:
        v = struct.unpack_from("<I", img, o)[0]
        if v == 0:
            break
        slots[kthunk + (o - (kthunk - base))] = knames.get(v & 0x7FFFFFFF, f"ord{v & 0x7FFFFFFF}")
        o += 4

    code_secs = [s for s in meta["sections"] if s["code"]]
    insns, sec_of = [], {}
    for s in code_secs:
        lst = load_listing(os.path.join(disdir, s["tag"] + ".asm"))
        for a, m, op in lst:
            sec_of[a] = s["name"]
        insns.extend(lst)
    insns.sort()
    addrs = [a for a, _, _ in insns]
    insn_set = set(addrs)
    idx_of = {a: i for i, a in enumerate(addrs)}

    def in_code(a):
        return a in insn_set

    # ---- function starts ----
    starts = Counter()
    starts[meta["entry"]] += 1
    reasons = defaultdict(set)
    reasons[meta["entry"]].add("entry")
    call_edges = []
    kcalls = defaultdict(list)
    for a, m, op in insns:
        if m == "call":
            mm = re.fullmatch(r"0x([0-9a-f]+)", op)
            if mm:
                t = int(mm.group(1), 16)
                if in_code(t):
                    starts[t] += 1
                    reasons[t].add("call")
                    call_edges.append((a, t))
            mm = re.search(r"ds:(0x[0-9a-f]+)$", op)
            if mm and int(mm.group(1), 16) in slots:
                kcalls[slots[int(mm.group(1), 16)]].append(a)
        if m == "jmp":
            mm = re.search(r"ds:(0x[0-9a-f]+)$", op)
            if mm and int(mm.group(1), 16) in slots:
                kcalls[slots[int(mm.group(1), 16)]].append(a)

    # instructions following padding after ret/jmp
    PAD = {"int3", "nop"}
    for i in range(1, len(insns)):
        a, m, op = insns[i]
        if m in PAD or m == "(bad)":
            continue
        j = i - 1
        npad = 0
        while j >= 0 and insns[j][1] in PAD:
            j -= 1
            npad += 1
        if npad and j >= 0 and (insns[j][1].startswith("ret") or insns[j][1] == "jmp"):
            starts[a] += 1
            reasons[a].add("pad")

    # pointers from data sections into code
    data_secs = [s for s in meta["sections"] if s["name"] in (".rdata", ".data")]
    ptr_hits = Counter()
    for s in data_secs:
        off = s["va"] - base
        blob = img[off:off + s["rsize"]]
        for k in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, k)[0]
            if v in insn_set:
                ptr_hits[v] += 1
    for v, n in ptr_hits.items():
        i = idx_of[v]
        prev = insns[i - 1][1] if i else "int3"
        if v in reasons or prev in PAD or prev.startswith("ret"):
            starts[v] += n
            reasons[v].add("dataptr")

    fstarts = sorted(starts)

    # ---- per function stats ----
    funcs = []
    callers = Counter(t for _, t in call_edges)
    for n, st in enumerate(fstarts):
        en = fstarts[n + 1] if n + 1 < len(fstarts) else None
        i = idx_of[st]
        sec = sec_of[st]
        mn = Counter()
        size_end = st
        j = i
        while j < len(insns) and (en is None or insns[j][0] < en) and sec_of[insns[j][0]] == sec:
            mn[insns[j][1]] += 1
            size_end = insns[j][0]
            j += 1
        # trim trailing padding
        k = j - 1
        while k > i and insns[k][1] in PAD:
            k -= 1
        size = (insns[k + 1][0] if k + 1 < len(insns) else size_end + 1) - st
        funcs.append(dict(start=st, size=size, section=sec, ninsn=sum(mn.values()),
                          callers=callers.get(st, 0), reasons=sorted(reasons[st]),
                          mnem=dict(mn)))

    json.dump(dict(functions=funcs, kernel_calls={k: v for k, v in kcalls.items()},
                   call_edges=call_edges), open(outp, "w"))

    by = defaultdict(lambda: [0, 0])
    for f in funcs:
        by[f["section"]][0] += 1
        by[f["section"]][1] += f["size"]
    print(f"{len(funcs)} functions")
    for s, (c, b) in sorted(by.items(), key=lambda x: -x[1][1]):
        print(f"  {s:10} {c:6} funcs {b/1024:8.1f} KB")
    print("reasons:", Counter(",".join(f["reasons"]) for f in funcs).most_common(8))
    print("kernel imports called directly:", len(kcalls))


if __name__ == "__main__":
    main()
