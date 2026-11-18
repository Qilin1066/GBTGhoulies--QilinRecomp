#!/usr/bin/env python3
"""C++ exception handling in the lifted code: which functions register an
exception frame, their try blocks and catch handlers, and where they throw.

This backs the exception policy in docs/recompiler-core.md: a catch handler
either returns a continuation (`mov eax, imm32; ret`), which resumes the
function, or rethrows. The report lists both kinds, and the throw sites with
the type each one throws.

Usage: eh_report.py <qilin> <default.xbe>
"""
import bisect
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

LIFTED = [(0x11000, 0x14A840), (0x152780, 0x165C00)]  # game code, Rare libraries


def lifted(a):
    return any(lo <= a < hi for lo, hi in LIFTED)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    qilin, xbe = sys.argv[1:]
    with tempfile.TemporaryDirectory() as t:
        img_path, ins_path, info_path = (os.path.join(t, n) for n in ("image.bin", "insns.txt", "info.json"))
        for cmd in (["image", xbe, img_path], ["insns", xbe, "-o", ins_path], ["info", xbe, "--json", info_path]):
            subprocess.run([qilin, *cmd], check=True, stdout=subprocess.DEVNULL)
        img = open(img_path, "rb").read()
        info = json.load(open(info_path))
        ins = {}
        for line in open(ins_path):
            p = line.rstrip("\n").split(None, 3)
            ins[int(p[0], 16)] = (int(p[1]), p[3].split("\t", 1)[-1])
    order = sorted(ins)
    base = info["base"] if isinstance(info["base"], int) else int(info["base"], 16)

    def u32(a):
        return struct.unpack_from("<I", img, a - base)[0]

    def cstr(a):
        return img[a - base:img.index(b"\0", a - base)].decode("latin1")

    def num(v):
        return v if isinstance(v, int) else int(v, 16)

    # FuncInfo tables (magic 0x19930520..22) in the data sections.
    funcinfos = []
    for s in info["sections"]:
        if s["name"] not in (".rdata", ".data"):
            continue
        va, size = num(s["va"]), num(s["vsize"])
        for a in range(va, va + size - 20, 4):
            if a + 20 - base > len(img) or not 0x19930520 <= u32(a) <= 0x19930522:
                continue
            if u32(a + 4) <= 4096 and u32(a + 12) <= 1024:
                funcinfos.append(a)
    fiset = set(funcinfos)
    # __ehhandler stubs (mov eax, &FuncInfo; jmp __CxxFrameHandler), and the functions that push them.
    stub_of = {}
    for a in order:
        m = re.fullmatch(r"mov eax, 0x([0-9A-F]+)", ins[a][1])
        if m and int(m.group(1), 16) in fiset:
            stub_of[int(m.group(1), 16)] = a
    pushers = {}
    for a in order:
        m = re.fullmatch(r"push 0x([0-9A-F]+)", ins[a][1])
        if m:
            pushers.setdefault(int(m.group(1), 16), []).append(a)

    def throw_info_types(ti):
        """Type names in a ThrowInfo's catchable-type array, or None."""
        try:
            cta = u32(ti + 12)
            n = u32(cta)
            if not 1 <= n <= 16:
                return None
            names = [cstr(u32(u32(cta + 4 + 4 * k) + 4) + 8) for k in range(n)]
        except (struct.error, ValueError):
            return None
        return names if all(x.startswith(".?A") for x in names) else None

    def pushes_before(a, n=6):
        i = bisect.bisect_left(order, a)
        return [ins[p][1] for p in order[max(0, i - n):i] if ins[p][1].startswith("push")]

    # Throw sites: `push &ThrowInfo; push &object; call _CxxThrowException`.
    throws = {}  # call address -> (target, first type name)
    for a in order:
        m = re.fullmatch(r"call 0x([0-9A-F]+)", ins[a][1])
        if not m:
            continue
        for p in pushes_before(a, 4):
            mm = re.fullmatch(r"push 0x([0-9A-F]+)", p)
            names = mm and throw_info_types(int(mm.group(1), 16))
            if names:
                throws[a] = (int(m.group(1), 16), names[0])
                break
    throw_fn = {t for t, _ in throws.values()}
    rethrow_sites = set()

    def follow(h):
        """A catch handler's outcome, following its branches: the continuation
        it returns (it resumes the function), or a rethrow."""
        seen, todo, out = set(), [h], set()
        while todo and len(seen) < 2000:
            a = todo.pop()
            if a in seen or a not in ins:
                continue
            seen.add(a)
            n, text = ins[a]
            nxt = a + n
            m = re.fullmatch(r"mov eax, 0x([0-9A-F]+)", text)
            if m and nxt in ins and ins[nxt][1].startswith("ret"):
                out.add(f"resumes at {int(m.group(1), 16):08X}")
                continue
            m = re.fullmatch(r"call 0x([0-9A-F]+)", text)
            if m and int(m.group(1), 16) in throw_fn:
                # A throw pushes its ThrowInfo; `throw;` passes two nulls.
                if a not in throws:
                    rethrow_sites.add(a)
                out.add(f"throws {throws[a][1]}" if a in throws else "rethrows (throw;)")
                continue
            if text.startswith("ret"):
                out.add("returns without a continuation (?)")
                continue
            m = re.fullmatch(r"j\w+ 0x([0-9A-F]+)", text)
            if m:
                todo.append(int(m.group(1), 16))
                if text.startswith("jmp"):
                    continue
            todo.append(nxt)
        return ", ".join(sorted(out)) or "?"

    frames = tries = resuming = 0
    print("Exception frames registered by lifted functions:")
    for fi in funcinfos:
        stub = stub_of.get(fi)
        sites = [p for p in pushers.get(stub, []) if lifted(p)] if stub else []
        if not sites:
            continue
        frames += len(sites)
        ntry, ptry = u32(fi + 12), u32(fi + 16)
        print(f"  FuncInfo {fi:08X}: pushed at {', '.join(f'{p:08X}' for p in sites)}, {u32(fi + 4)} states, {ntry} try blocks")
        for t in range(ntry):
            lo, hi, catch_high, ncatch, ph = (u32(ptry + 20 * t + 4 * k) for k in range(5))
            for c in range(ncatch):
                td, handler = u32(ph + 16 * c + 4), u32(ph + 16 * c + 12)
                what = follow(handler)
                tries += 1
                resuming += what.startswith("resumes")
                print(f"    try states {lo}-{hi}: catch ({cstr(td + 8) if td else '...'}) at {handler:08X}: {what}")
    print(f"{frames} frames, {tries} catch handlers, {resuming} of them resume")

    print("Calls to _CxxThrowException outside the catch handlers above:")
    for a in order:
        m = re.fullmatch(r"call 0x([0-9A-F]+)", ins[a][1])
        if not m or int(m.group(1), 16) not in throw_fn or a in rethrow_sites:
            continue
        where = "lifted" if lifted(a) else "replaced library code"
        what = f"throws {throws[a][1]}" if a in throws else "throws (ThrowInfo not a constant) or rethrows"
        print(f"  {a:08X} ({where}): {what}")


if __name__ == "__main__":
    main()
