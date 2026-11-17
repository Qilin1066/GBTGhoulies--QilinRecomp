#!/usr/bin/env python3
"""Recursive-descent control-flow recovery for the XBE.

Usage: cfg.py <disdir> <functions.json (linear pass, for seeds)> <out.json>

Produces per-function: instruction-address count, basic-block leaders, direct callees,
resolved jump tables, unresolved indirect jumps, indirect call sites.
Also reports code coverage of each section and gaps.
"""
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86dec import BRANCH_JCC, Decoder  # noqa: E402

NUM = r"(0x[0-9a-f]+|\d+)"
JT_RE = re.compile(r"^dword ptr \[4\*(e[a-ds][xip]|e[sd]i|ebp)\s*\+\s*" + NUM + r"\]$")
CMP_RE = re.compile(r"^(e[a-ds][xip]|e[sd]i|ebp|[a-d]l),\s*(0x[0-9a-f]+|\d+)$")
MOVZX_RE = re.compile(r"^(e[a-ds][xip]|e[sd]i|ebp),\s*byte ptr \[(e[a-ds][xip]|e[sd]i|ebp)\s*\+\s*" + NUM + r"\]$")
IMM_RE = re.compile(r"0x[0-9a-f]{5,8}")
SUB8 = {"al": "eax", "bl": "ebx", "cl": "ecx", "dl": "edx"}


class Prog:
    def __init__(self, disdir):
        self.meta = json.load(open(os.path.join(disdir, "sections.json")))
        self.base = self.meta["base"]
        self.img = open(os.path.join(disdir, "image.bin"), "rb").read()
        self.dec = Decoder(self.img, self.base)
        self.code = [(s["va"], s["va"] + s["rsize"], s["name"]) for s in self.meta["sections"] if s["code"]]

    def section(self, a):
        for lo, hi, n in self.code:
            if lo <= a < hi:
                return (lo, hi, n)
        return None

    def u32(self, a):
        return struct.unpack_from("<I", self.img, a - self.base)[0]

    def u8(self, a):
        return self.img[a - self.base]


def explore(P, f, known_starts, bound=None):
    """Recursive descent within one function. Returns dict."""
    insns = {}
    leaders = {f}
    work = [f]
    calls, icalls, ijmps, tails = set(), [], [], set()
    jtabs = []
    bad, overlap = [], []
    imms = set()
    sec_f = P.section(f)
    while work:
        a = work.pop()
        hist = []
        while True:
            if a in insns:
                break
            if not P.section(a) or P.section(a) != sec_f:
                bad.append(a)
                break
            ins = P.dec.decode(a)
            if ins is None:
                bad.append(a)
                break
            # overlap check: does a fall inside a previously decoded instruction?
            for back in range(1, 15):
                pi = insns.get(a - back)
                if pi is not None:
                    if pi.end > a:
                        overlap.append(a)
                    break
            insns[a] = ins
            hist.append(ins)
            if len(hist) > 12:
                hist.pop(0)
            m = ins.mnem
            for x in IMM_RE.findall(ins.ops):
                imms.add(int(x, 16))
            if m.startswith("ret"):
                break
            if m == "int3" and a != f:
                break
            if m == "jmp":
                t = ins.rel_target()
                if t is not None:
                    if (t in known_starts and t != f) or (bound and not (f <= t < bound)):
                        tails.add(t)
                    else:
                        leaders.add(t)
                        work.append(t)
                else:
                    jm = JT_RE.match(ins.ops)
                    if jm:
                        tg, how = resolve_jt(P, jm.group(1), int(jm.group(2), 0), hist, sec_f)
                        if bound:
                            tg = [t for t in tg if f <= t < bound]
                        jtabs.append(dict(at=a, table=int(jm.group(2), 0), n=len(tg), how=how))
                        for t in tg:
                            leaders.add(t)
                            work.append(t)
                    else:
                        ijmps.append(dict(at=a, ops=ins.ops))
                break
            if m in BRANCH_JCC:
                t = ins.rel_target()
                if t is not None:
                    if bound and not (f <= t < bound):
                        tails.add(t)
                    else:
                        leaders.add(t)
                        work.append(t)
                leaders.add(ins.end)
                a = ins.end
                continue
            if m == "call":
                t = ins.rel_target()
                if t is not None:
                    calls.add(t)
                else:
                    icalls.append(dict(at=a, ops=ins.ops))
                nxt = P.dec.decode(ins.end)
                if nxt is None or nxt.mnem == "int3":
                    break  # probable noreturn call
            a = ins.end
    return dict(insns=insns, leaders=leaders, calls=calls, icalls=icalls, ijmps=ijmps,
                tails=tails, jtabs=jtabs, bad=bad, overlap=overlap, imms=imms)


def resolve_jt(P, reg, table, hist, sec):
    """Return (targets, method)."""
    count = None
    how = "heuristic"
    # walk history backwards for: [movzx reg, byte ptr [r2 + T2]] ... cmp rX, N ; ja
    btable = None
    idx_reg = reg
    for ins in reversed(hist[:-1]):
        if ins.mnem == "movzx" and btable is None:
            mm = MOVZX_RE.match(ins.ops)
            if mm and mm.group(1) == reg:
                btable = int(mm.group(3), 0)
                idx_reg = mm.group(2)
                continue
        if ins.mnem == "cmp":
            cm = CMP_RE.match(ins.ops)
            if cm and SUB8.get(cm.group(1), cm.group(1)) == idx_reg:
                n = int(cm.group(2), 0) + 1
                if btable is not None:
                    count = max(P.u8(btable + i) for i in range(n)) + 1
                    how = "cmp+bytetable"
                else:
                    count = n
                    how = "cmp"
                break
    lo, hi, _ = sec
    tg = []
    limit = count if count is not None else 512
    for i in range(limit):
        v = P.u32(table + 4 * i)
        if not (lo <= v < hi):
            if count is None:
                break
            continue
        tg.append(v)
    return sorted(set(tg)), how


def main():
    disdir, linf, outp = sys.argv[1:4]
    P = Prog(disdir)
    lin = json.load(open(linf))["functions"]
    strong = [f["start"] for f in lin if set(f["reasons"]) & {"call", "dataptr", "entry"}]
    weak = [f["start"] for f in lin if set(f["reasons"]) == {"pad"}]
    import bisect
    known = set(strong)
    cand = sorted(set(strong) | set(weak))  # all candidate starts, for bounds
    funcs = {}
    owned = set()
    imm_seeds = set()

    def add_cand(v):
        i = bisect.bisect_left(cand, v)
        if i == len(cand) or cand[i] != v:
            cand.insert(i, v)

    def bound_of(f):
        i = bisect.bisect_right(cand, f)
        nxt = cand[i] if i < len(cand) else None
        sec = P.section(f)
        return min(nxt, sec[1]) if nxt else sec[1]

    def run(q, tag):
        while q:
            f = q.pop()
            if f in funcs or not P.section(f):
                continue
            add_cand(f)
            r = explore(P, f, known, bound_of(f))
            r["tag"] = tag
            funcs[f] = r
            owned.update(r["insns"])
            for c in list(r["calls"]) + list(r["tails"]):
                if c not in funcs and P.section(c):
                    known.add(c)
                    add_cand(c)
                    q.append(c)
            for v in r["imms"]:
                if v in known and v not in funcs:
                    q.append(v)
                elif P.section(v) and v not in funcs and v not in owned:
                    imm_seeds.add(v)

    # aligned code pointers stored in data sections (vtables, callback tables)
    dptr = set()
    for sec in P.meta["sections"]:
        if sec["name"] in (".rdata", ".data"):
            o = sec["va"] - P.base
            for k in range(0, sec["rsize"] - 3, 4):
                v = struct.unpack_from("<I", P.img, o + k)[0]
                if 0x11000 <= v < 0x165C00 and v % 16 == 0 and P.dec.decode(v):
                    dptr.add(v)
    run(sorted(strong, reverse=True) + [P.meta["entry"]], "reachable")

    def clean_seed(v):
        if v in funcs:
            return False
        allown = owned
        if v in allown:
            return False
        r = explore(P, v, known, bound_of(v))
        return (not r["bad"] and not r["overlap"] and len(r["insns"]) >= 3
                and not any(x in allown for x in r["insns"])
                and not any("byte ptr [eax], al" in i.ops for i in r["insns"].values()))

    for it in range(4):
        acc = [v for v in sorted(dptr) if clean_seed(v)]
        if not acc:
            break
        for v in acc:
            known.add(v)
            add_cand(v)
        run(sorted(acc, reverse=True), f"dataptr{it}")
    run(sorted([w for w in weak if w not in owned], reverse=True), "orphan")
    imm_ok = []
    for v in sorted(imm_seeds):
        if v in owned or v in funcs:
            continue
        prev = P.img[v - P.base - 1]
        if prev in (0xCC, 0x90, 0xC3) and P.dec.decode(v):
            imm_ok.append(v)
    run(sorted(imm_ok, reverse=True), "imm-ptr")

    # gap seeding: after padding / multi-byte nops / known tables, a 16-aligned decodable start
    PADT = {"int3", "nop"}
    PADTXT = {"mov\tedi, edi", "lea\tecx, [ecx]", "lea\tesp, [esp]", "lea\tebx, [ebx]"}
    jt_ranges = sorted((j["table"], j["table"] + 4 * max(1, j["n"])) for r in funcs.values() for j in r["jtabs"])
    for it in range(6):
        new = []
        allins = {}
        for r in funcs.values():
            allins.update(r["insns"])
        for lo, hi, name in P.code:
            a = lo
            while a < hi:
                if a in allins:
                    a = allins[a].end
                    continue
                ins = P.dec.decode(a)
                if ins is None:
                    a += 1
                    continue
                if ins.mnem in PADT or ins.text in PADTXT:
                    a = ins.end
                    continue
                jr = next((e for s0, e in jt_ranges if s0 <= a < e), None)
                if jr:
                    a = jr
                    continue
                prevb = P.img[a - P.base - 1]
                if name == ".text" and a % 16 == 0 and prevb in (0xCC, 0x90, 0xC3):
                    r = explore(P, a, known, bound_of(a))
                    clean = (not r["bad"] and not r["overlap"]
                             and any(i.mnem.startswith("ret") or i.mnem == "jmp" for i in r["insns"].values())
                             and not any(x in allins for x in r["insns"])
                             and not any("byte ptr [eax], al" in i.ops for i in r["insns"].values())
                             and len(r["insns"]) >= 3)
                    if clean:
                        new.append(a)
                # skip to next 4-aligned address
                a = (a + 4) & ~3
        new = [n for n in new if n not in funcs]
        if not new:
            break
        for n in new:
            add_cand(n)
        run(sorted(new, reverse=True), f"gap{it}")
        jt_ranges = sorted((j["table"], j["table"] + 4 * max(1, j["n"])) for r in funcs.values() for j in r["jtabs"])

    # ---- reports ----
    sec_cov = defaultdict(int)
    for f, r in funcs.items():
        pass
    allins = {}
    for r in funcs.values():
        allins.update(r["insns"])
    for a, ins in allins.items():
        sec_cov[P.section(a)[2]] += ins.size
    gaps = defaultdict(list)
    for lo, hi, name in P.code:
        a = lo
        while a < hi:
            if a in allins:
                a = allins[a].end
                continue
            g = a
            while a < hi and a not in allins:
                a += 1
            blob = P.img[g - P.base:a - P.base]
            if any(b not in (0xCC, 0x90, 0x00) for b in blob):
                gaps[name].append((g, a - g))

    out = []
    for f in sorted(funcs):
        r = funcs[f]
        out.append(dict(start=f, tag=r["tag"], section=P.section(f)[2], ninsn=len(r["insns"]),
                        bytes=sum(i.size for i in r["insns"].values()),
                        lo=min(r["insns"]) if r["insns"] else f,
                        hi=max(i.end for i in r["insns"].values()) if r["insns"] else f,
                        nblocks=len(r["leaders"]), calls=sorted(r["calls"]), tails=sorted(r["tails"]),
                        icalls=r["icalls"], ijmps=r["ijmps"], jtabs=r["jtabs"],
                        bad=r["bad"], overlap=r["overlap"]))
    json.dump(dict(functions=out, gaps={k: v for k, v in gaps.items()}), open(outp, "w"))

    tagc = Counter(r["tag"] for r in funcs.values())
    print("functions:", len(funcs), dict(tagc))
    print("instructions:", len(allins))
    print("coverage by section (non-padding bytes decoded / raw size):")
    for lo, hi, name in P.code:
        nonpad = sum(1 for b in P.img[lo - P.base:hi - P.base] if b not in (0xCC, 0x90))
        gb = sum(n for _, n in gaps[name])
        print(f"  {name:10} {sec_cov[name]:8} / {hi - lo:8}  ({100 * sec_cov[name] / max(1, nonpad):5.1f}% of non-pad)  gaps:{len(gaps[name]):5} gapbytes:{gb}")
    jt = [j for r in funcs.values() for j in r["jtabs"]]
    print("jump tables:", len(jt), Counter(j["how"] for j in jt), "targets:", sum(j["n"] for j in jt))
    print("unresolved indirect jmps:", sum(len(r["ijmps"]) for r in funcs.values()))
    print("indirect calls:", sum(len(r["icalls"]) for r in funcs.values()))
    print("functions with bad decode:", sum(1 for r in funcs.values() if r["bad"]),
          " with overlapping insns:", sum(1 for r in funcs.values() if r["overlap"]))


if __name__ == "__main__":
    main()
