#!/usr/bin/env python3
"""Cross-check the recompiler's x86 decoder (Zydis) against LLVM's disassembler.

Usage:
  qilin insns default.xbe -o reach.txt            # every reachable instruction
  qilin insns default.xbe --linear -o linear.txt  # linear sweep of every code section
  verify_decoder.py --reachable reach.txt --linear linear.txt

Reachable instructions (what the lifter consumes): LLVM must decode the same
bytes to the same length and mnemonic (after mapping the two libraries' naming
conventions). Any disagreement fails.

Linear sweep (every byte of every code section, including data in them): the
same rule, except three known-benign classes are reported rather than failed:
encodings LLVM's decoder rejects (undocumented aliases such as int1, SAL /6,
TEST /1), LLVM returning a lone prefix byte, and encodings Zydis rightly
rejects that no Pentium III could run (mov to CS, EVEX). Anything else fails.
Exit status 1 on failure, so this can gate CI.

Needs libLLVM (any version >= 15) with the X86 target; found automatically
under /usr/lib/llvm-*/lib, or set LIBLLVM=/path/to/libLLVM.so.
"""
import ctypes as C
import glob
import os
import sys
from collections import Counter, defaultdict

# Zydis mnemonic -> LLVM mnemonic, where the naming differs (Intel syntax).
ZY2LLVM = {
    "jnb": "jae", "jnbe": "ja", "jz": "je", "jnz": "jne", "jnl": "jge", "jnle": "jg",
    "setnb": "setae", "setnbe": "seta", "setz": "sete", "setnz": "setne", "setnl": "setge", "setnle": "setg",
    "cmovnb": "cmovae", "cmovnbe": "cmova", "cmovz": "cmove", "cmovnz": "cmovne", "cmovnl": "cmovge",
    "cmovnle": "cmovg",
    "fcomip": "fcompi", "fucomip": "fucompi",
    "pushad": "pushal", "popad": "popal", "pusha": "pushaw", "popa": "popaw",
    "fwait": "wait", "xlat": "xlatb", "sgdt": "sgdtd", "sidt": "sidtd",
}
# Far forms: Zydis keeps the mnemonic and marks the operand "far"; LLVM renames.
FAR = {"call": "lcall", "jmp": "ljmp", "ret": "retf"}
PREFIXES = {"rep", "repe", "repne", "lock", "data16", "data32", "addr16", "addr32", "cs", "ds", "es", "fs",
            "gs", "ss", "notrack", "bnd", "xacquire", "xrelease"}


def llvm():
    path = os.environ.get("LIBLLVM") or (sorted(glob.glob("/usr/lib/llvm-*/lib/libLLVM*.so*")) or [None])[-1]
    if not path:
        sys.exit("libLLVM not found; set LIBLLVM")
    L = C.CDLL(path)
    for f in ("LLVMInitializeX86TargetInfo", "LLVMInitializeX86TargetMC", "LLVMInitializeX86Disassembler"):
        getattr(L, f)()
    L.LLVMCreateDisasm.restype = C.c_void_p
    L.LLVMCreateDisasm.argtypes = [C.c_char_p, C.c_void_p, C.c_int, C.c_void_p, C.c_void_p]
    L.LLVMSetDisasmOptions.argtypes = [C.c_void_p, C.c_uint64]
    L.LLVMDisasmInstruction.restype = C.c_size_t
    L.LLVMDisasmInstruction.argtypes = [C.c_void_p, C.c_char_p, C.c_uint64, C.c_uint64, C.c_char_p, C.c_size_t]
    dc = L.LLVMCreateDisasm(b"i386-pc-windows", None, 0, None, None)
    L.LLVMSetDisasmOptions(dc, 4 | 2)  # Intel syntax, hex immediates
    buf = C.create_string_buffer(256)

    def dis(code, addr):
        n = L.LLVMDisasmInstruction(dc, code, len(code), addr, buf, 256)
        return n, buf.value.decode("latin-1").strip()
    return dis


def llvm_mnemonic(text):
    toks = text.replace("\t", " ").split()
    while toks and toks[0] in PREFIXES:
        toks.pop(0)
    return toks[0] if toks else ""


def zydis_mnemonic(zm, ztext):
    """Zydis mnemonic mapped to LLVM's name for the same instruction."""
    # Zydis folds rep/lock into attributes; its mnemonic is the base instruction.
    if zm in FAR and " far" in ztext:
        # LLVM prints a far call/jmp through memory (FF /3, FF /5) as plain call/jmp.
        if zm != "ret" and "ptr [" in ztext:
            return zm
        return FAR[zm]
    return ZY2LLVM.get(zm, zm)


def equivalent(zm, lm):
    if zm == lm:
        return True
    # Size-suffixed string ops: LLVM prints movsd/stosd/..., Zydis movsd/stosd too, but
    # LLVM's movsd (SSE) vs string movsd share a name; compare loosely on the stem.
    stems = ("movs", "stos", "lods", "scas", "cmps", "ins", "outs")
    if any(zm.startswith(s) and lm.startswith(s) for s in stems):
        return True
    return False


far = []  # far control transfers inside reachable code: data decoded as code


def classify(zm, zlen, n, ltext):
    """Name a known-benign disagreement class, or None."""
    lm = llvm_mnemonic(ltext) if n else ""
    if n == 0:
        # LLVM rejects an encoding Zydis accepts: undocumented aliases and
        # instructions LLVM's i386 decoder omits (int1, bound, SAL /6, TEST /1...).
        return "llvm-rejects:" + zm
    if zm == "bad" and (ltext.startswith("mov\tcs,") or llvm_mnemonic(ltext).startswith("v")):
        # Zydis rejects; LLVM accepts something no Pentium III can execute
        # (mov to CS is #UD; EVEX/AVX-512 is disabled in our Zydis build).
        return "zydis-rejects-invalid-on-p3:" + lm
    if n == 1 and ltext.split()[0] in PREFIXES | {"xrelease", "xacquire"}:
        # LLVM returns a lone prefix where Zydis decodes (or rejects) the whole instruction.
        return "llvm-lone-prefix:" + ltext.split()[0]
    return None


def check(path, reachable, dis, hard, benign, stats):
    kind = "reachable" if reachable else "linear"
    for line in open(path):
        head, _, ztext = line.rstrip("\n").partition("\t")
        addr_s, len_s, hexb, zm = head.split(" ")
        addr, zlen, code = int(addr_s, 16), int(len_s), bytes.fromhex(hexb)
        n, ltext = dis(code, addr)
        if n == 0:
            ltext = ""
        stats[kind] += 1
        if zm == "bad":
            if n != 0:
                msg = f"{kind} {addr:08x}: Zydis can't decode {hexb}, LLVM gives {n} bytes '{ltext}'"
                cat = None if reachable else classify(zm, 0, n, ltext)
                (benign[cat].append(msg) if cat else hard.append(msg))
            continue
        msg = None
        if n != zlen:
            msg = f"{kind} {addr:08x}: length Zydis {zlen} '{ztext}' vs LLVM {n} '{ltext}'"
        else:
            lm = llvm_mnemonic(ltext)
            zmn = zydis_mnemonic(zm, ztext)
            if not equivalent(zmn, lm):
                msg = f"{kind} {addr:08x}: mnemonic Zydis {zmn!r} '{ztext}' vs LLVM {lm!r} '{ltext}'"
            if reachable and zmn in ("lcall", "ljmp", "retf"):
                far.append(f"{addr:08x}: {ztext}")
        if msg:
            cat = None if reachable else classify(zm, zlen, n, ltext)
            (benign[cat].append(msg) if cat else hard.append(msg))


def main():
    args = sys.argv[1:]
    files = []
    while args:
        if args[0] in ("--reachable", "--linear") and len(args) >= 2:
            files.append((args[1], args[0] == "--reachable"))
            args = args[2:]
        else:
            sys.exit(__doc__)
    if not files:
        sys.exit(__doc__)
    dis = llvm()
    hard, stats = [], Counter()
    benign = defaultdict(list)
    for p, reach in files:
        check(p, reach, dis, hard, benign, stats)
    for k, v in stats.items():
        print(f"{k}: {v} instructions checked")
    if benign:
        print(f"linear sweep: {sum(len(v) for v in benign.values())} known-benign disagreements "
              "(bytes that are data, not reachable code):")
        for cat, ms in sorted(benign.items(), key=lambda kv: -len(kv[1])):
            print(f"  {cat}: {len(ms)}   e.g. {ms[0]}")
    if far:
        print(f"warning: {len(far)} far call/jmp/ret in reachable code (data decoded as code?), e.g. {far[:5]}")
    if hard:
        print(f"FAIL: {len(hard)} disagreements")
        for m in hard[:100]:
            print("  " + m)
        sys.exit(1)
    print("decoder cross-check: OK")


if __name__ == "__main__":
    main()
