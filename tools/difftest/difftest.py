#!/usr/bin/env python3
"""Differential test: lifted C++ vs the original x86 code under Unicorn.

For every lifted function, run many trials. Each trial builds one random
guest state (registers, xmm registers, stack arguments, a scratch heap full
of pointers / small ints / floats), then:

  1. runs the ORIGINAL x86 bytes of the function in Unicorn (an x86 CPU
     emulator) until it returns to a sentinel address;
  2. runs the LIFTED C++ function on an identical copy of the state;
  3. compares all general registers, esp, all 8 xmm registers (all lanes,
     bit-exact), and every byte either side wrote to memory.

If the original faults (unmapped access, divide error, int3), the lifted code
must trap as well. Trials that run past the instruction budget are skipped.

Calls: the original runs its whole call tree in Unicorn. The lifted function
calls lifted callees directly; a callee with no lifted body (library code,
or game code that doesn't lift yet) is run from its original bytes in a
second Unicorn instance that shares the lifted side's memory. So every
lifted function in a call tree is compared, whatever its callees are.

Usage:
  difftest.py --qilin build/qilin --xbe default.xbe [--work /tmp/qd]
              [--trials 24] [--only ADDR] [--cxx g++-14] [--sanitize]

Exit status 1 if any function mismatches. Needs: pip install unicorn

On Windows, --cxx names a compiler that takes GCC's options and makes a DLL,
such as Zig's ("path/to/zig.exe c++", from pip install ziglang); the
library is libdifftest.dll.
"""
import argparse
import concurrent.futures as cf
import ctypes as C
import fractions
import math
import mmap
import os
import random
import shlex
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter

try:
    import resource  # POSIX only: the workers' peak memory, in their logs
except ImportError:
    resource = None

WINDOWS = os.name == "nt"
LIB_NAME = "libdifftest.dll" if WINDOWS else "libdifftest.so"


def peak_rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >> 10 if resource else 0

import mpmath  # pip install mpmath: the reference for x87 transcendentals (EmuFixups)

from unicorn import (UC_ARCH_X86, UC_ERR_WRITE_PROT, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_32, UC_PROT_ALL,
                     UC_PROT_EXEC, UC_PROT_READ, Uc, UcError)
from unicorn.x86_const import (UC_X86_REG_CR0, UC_X86_REG_CR4, UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_EAX,
                               UC_X86_REG_EBP, UC_X86_REG_ES, UC_X86_REG_FS, UC_X86_REG_GDTR, UC_X86_REG_SS,
                               UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI, UC_X86_REG_EDX,
                               UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP,
                               UC_X86_REG_FP0, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG,
                               UC_X86_REG_MXCSR, UC_X86_REG_XMM0)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

MEM_SIZE = 64 << 20
SCRATCH = 0x01000000
SCRATCH_SIZE = 4 << 20
STACK_LO, STACK_HI = 0x03F00000, 0x03F10000
STACK_TOP = 0x03F08000
# Thread information block: fs:[0] heads the exception-registration chain that
# functions with C++ exception handling link into. The chain starts at a
# caller's record further up the stack.
TIB = 0x03F10000
TIB_INIT = struct.pack("<I", STACK_TOP + 0x100)
GDT = 0x03F11000
SENTINEL = 0x03FFFFF0
MAX_INSNS = 200_000
UC_TIMEOUT_US = 1_000_000  # rep-prefixed instructions count once; cap wall time too
CODE_LO, CODE_HI = 0x11000, 0x202000  # .text .. XPP (page-rounded)

GPRS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UC_GPR = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
          UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]


class Xmm(C.Structure):
    _fields_ = [("u", C.c_uint32 * 4)]


class Context(C.Structure):
    _fields_ = [(r, C.c_uint32) for r in GPRS] + [
        ("eflags", C.c_uint32), ("ret_to", C.c_uint32), ("fs_base", C.c_uint32), ("xmm", Xmm * 8),
        ("st", C.c_double * 8), ("ftop", C.c_uint32), ("ftag", C.c_uint32), ("fpcw", C.c_uint32), ("fpsw", C.c_uint32),
        ("mem", C.c_void_p), ("mem_size", C.c_uint64), ("mm", C.c_uint64 * 8)]


# ---- x87 values: the lifted side holds doubles, Unicorn 80-bit (mantissa, sign|exponent).
def d2x80(d):
    b = struct.unpack("<Q", struct.pack("<d", d))[0]
    sign, e, m = b >> 63, (b >> 52) & 0x7FF, b & ((1 << 52) - 1)
    if e == 0x7FF:  # inf / NaN: explicit integer bit, payload shifted up
        return ((1 << 63) | (m << 11), sign << 15 | 0x7FFF)
    if e == 0:
        if m == 0:
            return (0, sign << 15)
        sh = 53 - m.bit_length()  # double subnormal: normalise
        return (m << (11 + sh), sign << 15 | (1 - 1023 + 16383 - sh))
    return ((1 << 63) | (m << 11), sign << 15 | (e - 1023 + 16383))


def x802d(v):
    """80-bit -> nearest double, and whether that was exact."""
    mant, se = v
    sign, e = se >> 15, se & 0x7FFF
    if e == 0x7FFF:
        if mant & ((1 << 63) - 1) == 0:
            return math.copysign(math.inf, -1 if sign else 1), True
        b = sign << 63 | 0x7FF << 52 | ((mant >> 11) & ((1 << 52) - 1))
        return struct.unpack("<d", struct.pack("<Q", b))[0], mant & 0x7FF == 0
    if mant == 0:
        return (-0.0 if sign else 0.0), True
    q = fractions.Fraction(mant, 1 << 63) * fractions.Fraction(2) ** (e - 16383)
    try:
        d = float(q)  # correctly rounded
    except OverflowError:  # beyond a double's range (the x87 reaches 2^16384): an infinity, inexactly
        return math.copysign(math.inf, -1 if sign else 1), False
    return (-d if sign else d), fractions.Fraction(d) == q


def x80_exact(v):
    """80-bit -> exact Fraction, or None for an infinity or NaN."""
    mant, se = v
    sign, e = se >> 15, se & 0x7FFF
    if e == 0x7FFF:
        return None
    q = fractions.Fraction(mant, 1 << 63) * fractions.Fraction(2) ** (max(e, 1) - 16383)
    return -q if sign else q


def correct_transcendental(mnem, x, y):
    """The correctly rounded double of the x87 transcendental at exact operands
    x = ST(0), y = ST(1), or None where the operands are special (zeros,
    infinities, NaNs, out of range, x <= 0 or 1 for fyl2x): there the harness
    leaves Unicorn's answer."""
    if x is None or x == 0 or (mnem in ("fpatan", "fyl2x") and (y is None or y == 0)):
        return None
    if mnem in ("fsin", "fcos", "fptan") and abs(x) >= 2 ** 63:
        return None
    if mnem == "fyl2x" and (x <= 0 or x == 1):
        return None
    mpmath.mp.prec = 320
    mx = mpmath.mpf(x.numerator) / x.denominator  # exact: the denominator is a power of two
    if mnem == "fsin":
        v = mpmath.sin(mx)
    elif mnem == "fcos":
        v = mpmath.cos(mx)
    elif mnem == "fptan":
        v = mpmath.tan(mx)
    elif mnem == "f2xm1":
        v = mpmath.expm1(mx * mpmath.log(2))
    else:
        my = mpmath.mpf(y.numerator) / y.denominator
        v = mpmath.atan2(my, mx) if mnem == "fpatan" else my * mpmath.log(mx, 2)
    sign, man, exp, _ = v._mpf_
    q = fractions.Fraction(-int(man) if sign else int(man)) * fractions.Fraction(2) ** int(exp)
    try:
        return float(q)  # correctly rounded, subnormals included
    except OverflowError:
        return math.copysign(math.inf, q)


TAG_EMPTY = 3


def x87_read(uc):
    sw = uc.reg_read(UC_X86_REG_FPSW)
    tag = uc.reg_read(UC_X86_REG_FPTAG)
    valid = sum(1 << p for p in range(8) if (tag >> (2 * p)) & 3 != TAG_EMPTY)
    return {"top": (sw >> 11) & 7, "valid": valid, "sw": sw, "cw": uc.reg_read(UC_X86_REG_FPCW),
            "regs": [uc.reg_read(UC_X86_REG_FP0 + p) for p in range(8)]}


def x87_write(uc, cw, sw, top, valid, values):
    uc.reg_write(UC_X86_REG_FPCW, cw)
    uc.reg_write(UC_X86_REG_FPSW, (sw & ~0x3800) | (top << 11))
    uc.reg_write(UC_X86_REG_FPTAG, sum((0 if valid >> p & 1 else TAG_EMPTY) << (2 * p) for p in range(8)))
    for p in range(8):
        uc.reg_write(UC_X86_REG_FP0 + p, d2x80(values[p]))


FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.14159, -100.25, 1e-3, 1e10, -1e10, 1e-30, 65536.0,
          float("inf"), float("-inf"), 0.1, 255.0, 1e-40]


def f2u(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def rand_word(rng, kind_weights=(4, 3, 3, 1), floaty=False):
    """A random 32-bit word: a scratch pointer, a small int, a float or random
    bits, by kind_weights. floaty: the float-heavy heap's mix and floats."""
    k = rng.choices(range(4), (2, 1, 8, 1) if floaty else kind_weights)[0]
    if k == 0:  # pointer into scratch, 16-aligned plus a small offset
        return SCRATCH + rng.randrange(0, SCRATCH_SIZE - 0x10000, 16) + rng.choice([0, 0, 0, 4, 8, 12])
    if k == 1:
        return rng.randrange(0, 64)
    if k == 2:
        if not floaty:
            return f2u(rng.choice(FLOATS) if rng.random() < 0.5 else rng.uniform(-1000, 1000))
        # Full 24-bit significands, at world-coordinate and unit-vector scales.
        r = rng.random()
        return f2u(rng.choice(FLOATS) if r < 0.2 else rng.uniform(-1000, 1000) if r < 0.6 else rng.uniform(-1, 1))
    return rng.getrandbits(32)


def make_scratch(seed, floaty=False):
    """floaty: a heap of mostly floats with full significands, where rounding
    differences (x87 precision control, SSE) show up far more often."""
    rng = random.Random(seed)
    words = [rand_word(rng, floaty=floaty) for _ in range(SCRATCH_SIZE // 4)]
    return struct.pack(f"<{len(words)}I", *words)


def build(args, work):
    gen = os.path.join(work, "gen")
    img = os.path.join(work, "image.bin")
    subprocess.run([args.qilin, "image", args.xbe, img], check=True, stdout=subprocess.DEVNULL)
    # Reachable instructions, for the emulator corrections (see EmuFixups).
    subprocess.run([args.qilin, "insns", args.xbe, "-o", os.path.join(work, "insns.txt")], check=True,
                   stdout=subprocess.DEVNULL)
    cmd = [args.qilin, "lift", args.xbe, "--out", gen]
    if args.only:
        cmd += ["--only", args.only]
    print(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.splitlines()[0])
    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".cpp"))
    srcs.append(os.path.join(HERE, "harness.cpp"))
    srcs.append(os.path.join(ROOT, "recomp", "runtime", "dispatch.cpp"))
    # Production optimisation level and FP rules; warnings are errors, so a
    # generator regression that produces sloppy C++ fails here too.
    flags = ["-std=c++23", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
             "-DQILIN_CHECKED_MEMORY", "-DQILIN_TRACE_WRITES", "-DQILIN_TEST_HOOKS",
             "-I", os.path.join(ROOT, "recomp", "runtime")]
    if not WINDOWS:
        flags.append("-fPIC")  # (a DLL needs none, and Clang refuses it for Windows targets)
    if args.sanitize:
        # Undefined behaviour in generated code (shifts, overflow, bad casts) aborts the run.
        flags += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
    cxx = shlex.split(args.cxx, posix=not WINDOWS)  # a command and its options, such as "zig c++"
    objs = []

    def cc(src):
        obj = os.path.join(work, os.path.basename(src) + ".o")
        subprocess.run([*cxx, *flags, "-c", src, "-o", obj], check=True)
        return obj

    with cf.ThreadPoolExecutor(args.jobs) as ex:
        objs = list(ex.map(cc, srcs))
    lib = os.path.join(work, LIB_NAME)
    link = ["-fsanitize=undefined"] if args.sanitize else []
    subprocess.run([*cxx, "-shared", *link, "-o", lib, *objs], check=True)
    return img, lib


# C runtime assembly with irregular contracts (recomp/src/main.cpp,
# crt_conventions): shared epilogues that pop one word of their caller's frame
# before returning, and helpers that take their operands on the x87 stack.
GLUE_POPS = {0x165F80: 1, 0x16B7FE: 1, 0x16B80B: 1, 0x16B849: 1}
X87_ARGS = {0x1673D8: 1, 0x167CB8: 2, 0x167CDC: 2, 0x167EA1: 1, 0x16B64D: 1, 0x16B70C: 1, 0x16B723: 1, 0x16B760: 1,
            0x16B849: 1, 0x1706E0: 1, 0x1706FD: 1, 0x167500: 1, 0x16751D: 1}


class EmuFixups:
    """Where Unicorn (QEMU 5) differs from the hardware, make it follow the
    hardware, identically in both emulators:
      - rdtsc returns the host's time-stamp counter: replaced by a counter that
        restarts every trial and steps by a fixed amount;
      - fcomi/fcomip/fucomi/fucomip leave OF, SF and AF unchanged: Intel clears them;
      - legacy SSE instructions with a 16-byte memory operand (movaps,
        mulps, shufps …) don't check its alignment; the hardware raises #GP,
        and the lifted code traps. A misaligned one is sent to an unmapped
        address, so the original faults too;
      - SSE add, sub, mul and div (scalar and packed single) with a NaN in both
        operands return the second operand's: Intel returns the first's (the
        destination), quietened, as the lifted code does (rt::sse_nan);
      - after each x87 instruction of the C runtime's maths, a register value
        outside double's exponent range (pow's underflow and overflow paths
        rescale such values by 2^1536) is a known, documented limitation of
        the double registers: the trial is skipped. Game code never makes one;
      - fnsave stores the last instruction and operand pointers, which the
        lifted code (like nothing but frstor) doesn't track: zeroed here;
      - fst/fstp st(i) into an empty register leaves it tagged empty: Intel
        tags it valid (C runtime maths ends with `fstp st1` on a one-value stack);
      - fyl2x with an operand <= 0 doesn't pop: the trial is skipped;
      - fsin, fcos, fptan, fpatan, fyl2x and f2xm1 return the host libm's answer
        (fyl2x even rounds three times). The hardware's 64-bit results aren't
        available, and the lifted code is specified to return the correctly
        rounded double (recomp/runtime/x87_math.hpp), so for finite, ordinary
        operands the result is replaced with that, computed with mpmath from
        the exact 80-bit operands."""

    def __init__(self, insns_path):
        self.rdtsc, self.fcomi, self.fyl2x, self.trans, self.fst_reg, self.crt_x87, self.fnsave = [], [], [], [], [], [], []
        self.sse_aligned = []  # (address, operand text) of SSE instructions that need a 16-byte aligned m128
        self.sse_arith = []  # (address, next address, lanes, destination, source text) of add/sub/mul/div ss/ps
        for line in open(insns_path):
            parts = line.split(None, 3)
            a, n, m = int(parts[0], 16), int(parts[1]), parts[3].split("\t")[0]
            if m == "rdtsc":
                self.rdtsc.append(a)
            elif m in ("fcomi", "fcomip", "fucomi", "fucomip"):
                self.fcomi.append((a, a + n))
            if m == "fyl2x":
                self.fyl2x.append(a)
            if m.startswith("f") and 0x165C00 <= a < 0x1707B0 and m not in ("fwait", "fnstcw", "fldcw", "fnstsw"):
                self.crt_x87.append(a + n)
            text = parts[3].split("\t")[1] if "\t" in parts[3] else ""
            if m in ("addss", "subss", "mulss", "divss", "addps", "subps", "mulps", "divps"):
                ops = text.split(None, 1)[1].split(", ", 1)
                if ops[0].startswith("xmm"):
                    self.sse_arith.append((a, a + n, 1 if m.endswith("ss") else 4, int(ops[0][3:]), ops[1]))
            if "xmmword ptr [" in text and m not in ("movups", "movupd", "movdqu", "lddqu"):
                self.sse_aligned.append((a, text[text.index("xmmword ptr [") + 13:text.index("]", text.index("xmmword ptr ["))]))
            if m == "fnsave":
                self.fnsave.append((a, a + n))
            if m in ("fst", "fstp"):
                ops = parts[3].split("\t")[1].split(None, 1)[1] if " " in parts[3].split("\t")[1] else ""
                if ops.startswith("st") and ops[2:].isdigit():
                    self.fst_reg.append((a, a + n, int(ops[2:])))
            if m in ("fsin", "fcos", "fptan", "fpatan", "fyl2x", "f2xm1"):
                self.trans.append((a, a + n, m))
        self.quirk = ""

    def install(self, uc):
        state = {"tsc": 0, "fix_at": None}
        self_ = self

        def on_rdtsc(u, a, size, ud):
            state["tsc"] += 1000
            u.reg_write(UC_X86_REG_EAX, state["tsc"] & 0xFFFFFFFF)
            u.reg_write(UC_X86_REG_EDX, state["tsc"] >> 32)
            u.reg_write(UC_X86_REG_EIP, a + 2)
        for a in self.rdtsc:
            uc.hook_add(UC_HOOK_CODE, on_rdtsc, None, a, a)

        def on_fcomi(u, a, size, ud):
            state["fix_at"] = a + size

        def after_fcomi(u, a, size, ud):
            if state["fix_at"] == a:
                state["fix_at"] = None
                u.reg_write(UC_X86_REG_EFLAGS, u.reg_read(UC_X86_REG_EFLAGS) & ~0x890)  # OF, SF, AF
        for a, nxt in self.fcomi:
            uc.hook_add(UC_HOOK_CODE, on_fcomi, None, a, a)
            uc.hook_add(UC_HOOK_CODE, after_fcomi, None, nxt, nxt)

        def on_fst(u, a, size, i):
            top = (u.reg_read(UC_X86_REG_FPSW) >> 11) & 7
            state["fst"] = (a + size, (top + i) & 7)

        def after_fst(u, a, size, ud):
            f = state.get("fst")
            if f and f[0] == a:
                state["fst"] = None
                tag = u.reg_read(UC_X86_REG_FPTAG)
                if (tag >> (2 * f[1])) & 3 == TAG_EMPTY:
                    u.reg_write(UC_X86_REG_FPTAG, tag & ~(3 << (2 * f[1])))
        for a, nxt, i in self.fst_reg:
            if i:
                uc.hook_add(UC_HOOK_CODE, on_fst, i, a, a)
                uc.hook_add(UC_HOOK_CODE, after_fst, None, nxt, nxt)

        def on_fnsave(u, a, size, ud):
            # The operand is [reg + disp8] in the C runtime's pow; resolve it before the instruction runs.
            state["fnsave"] = (a + size, None)
            insn = bytes(u.mem_read(a, size))
            if insn[0] == 0xDD and (insn[1] >> 3) & 7 == 6:
                rm, mod = insn[1] & 7, insn[1] >> 6
                regs = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
                        UC_X86_REG_ESI, UC_X86_REG_EDI]
                if mod == 1 and rm != 4:
                    state["fnsave"] = (a + size, (u.reg_read(regs[rm]) + struct.unpack("<b", insn[2:3])[0]) & 0xFFFFFFFF)
            if state["fnsave"][1] is None:
                self_.quirk = f"fnsave operand form at {a:08X} not handled by the harness"

        def after_fnsave(u, a, size, ud):
            f = state.get("fnsave")
            if f and f[0] == a:
                state["fnsave"] = None
                if f[1] is not None:
                    u.mem_write(f[1] + 12, bytes(16))
        for a, nxt in self.fnsave:
            uc.hook_add(UC_HOOK_CODE, on_fnsave, None, a, a)
            uc.hook_add(UC_HOOK_CODE, after_fnsave, None, nxt, nxt)

        regs = {"eax": UC_X86_REG_EAX, "ecx": UC_X86_REG_ECX, "edx": UC_X86_REG_EDX, "ebx": UC_X86_REG_EBX,
                "esp": UC_X86_REG_ESP, "ebp": UC_X86_REG_EBP, "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI}

        def on_sse_aligned(u, a, size, expr):
            ea = 0
            for term in expr.replace("-", "+-").split("+"):
                term = term.strip()
                if not term:
                    continue
                neg = term.startswith("-")
                term = term.lstrip("-")
                if "*" in term:
                    r, k = term.split("*")
                    v = u.reg_read(regs[r]) * int(k, 0)
                elif term in regs:
                    v = u.reg_read(regs[term])
                else:
                    v = int(term, 16)
                ea += -v if neg else v
            if ea & 15:
                u.reg_write(UC_X86_REG_EIP, 0xFFFFF000)  # #GP stand-in: fetch from an unmapped page
        for a, expr in self.sse_aligned:
            uc.hook_add(UC_HOOK_CODE, on_sse_aligned, expr, a, a)

        # Both operands NaN: the destination's NaN, quietened (Intel); Unicorn gives the source's.
        def is_nan32(v):
            return (v & 0x7FFFFFFF) > 0x7F800000

        arith_at = {ins[0]: ins for ins in self.sse_arith}

        # One hook at every such instruction and every one after: first the
        # correction the instruction just run needs (consecutive SSE
        # instructions are common), then a look at this one's operands.
        def on_sse_arith(u, a, size, ud):
            pending = state.get("sse_nan")
            if pending and pending[0] == a:
                _, dst, fix = pending
                v = u.reg_read(UC_X86_REG_XMM0 + dst)
                for k, lane in fix.items():
                    v = v & ~(0xFFFFFFFF << (32 * k)) | lane << (32 * k)
                u.reg_write(UC_X86_REG_XMM0 + dst, v)
            state["sse_nan"] = None
            ins = arith_at.get(a)
            if ins is None:
                return
            _, nxt, lanes, dst, src = ins
            d = u.reg_read(UC_X86_REG_XMM0 + dst)
            if src.startswith("xmm"):
                s = u.reg_read(UC_X86_REG_XMM0 + int(src[3:]))
            else:
                expr = src[src.index("[") + 1:src.index("]")]
                ea = 0
                for term in expr.replace("-", "+-").split("+"):
                    term = term.strip()
                    if not term:
                        continue
                    neg = term.startswith("-")
                    term = term.lstrip("-")
                    if "*" in term:
                        r, k = term.split("*")
                        v = u.reg_read(regs[r]) * int(k, 0)
                    elif term in regs:
                        v = u.reg_read(regs[term])
                    else:
                        v = int(term, 16)
                    ea += -v if neg else v
                try:
                    s = int.from_bytes(bytes(u.mem_read(ea & 0xFFFFFFFF, 4 * lanes)), "little")
                except UcError:
                    return  # the instruction faults on its own
            fix = {}
            for k in range(lanes):
                x, y = d >> (32 * k) & 0xFFFFFFFF, s >> (32 * k) & 0xFFFFFFFF
                if is_nan32(x) and is_nan32(y):
                    fix[k] = x | 0x00400000
            if fix:
                state["sse_nan"] = (nxt, dst, fix)
        for a in sorted({ins[0] for ins in self.sse_arith} | {ins[1] for ins in self.sse_arith}):
            uc.hook_add(UC_HOOK_CODE, on_sse_arith, None, a, a)

        def after_crt_x87(u, a, size, ud):
            fx = x87_read(u)
            for p in range(8):
                if fx["valid"] >> p & 1:
                    mant, se = fx["regs"][p]
                    e = se & 0x7FFF
                    if mant and e != 0x7FFF and not (16383 - 1074 <= e <= 16383 + 1023):
                        self_.quirk = f"x87 value outside double's range after {a:08X}"
        for a in self.crt_x87:
            uc.hook_add(UC_HOOK_CODE, after_crt_x87, None, a, a)

        def on_fyl2x(u, a, size, ud):
            top = (u.reg_read(UC_X86_REG_FPSW) >> 11) & 7
            v, _ = x802d(u.reg_read(UC_X86_REG_FP0 + top))
            if not v > 0:
                self_.quirk = f"fyl2x of {v} at {a:08X} (Unicorn doesn't pop)"
        for a in self.fyl2x:
            uc.hook_add(UC_HOOK_CODE, on_fyl2x, None, a, a)

        def on_trans(u, a, size, mnem):
            top = (u.reg_read(UC_X86_REG_FPSW) >> 11) & 7
            st0 = x80_exact(u.reg_read(UC_X86_REG_FP0 + top))
            st1 = x80_exact(u.reg_read(UC_X86_REG_FP0 + ((top + 1) & 7)))
            state["trans"] = None
            r = correct_transcendental(mnem, st0, st1)
            if r is not None:  # the result lands in ST(0), or in ST(1) before the pop
                state["trans"] = (a + size, top if mnem in ("fsin", "fcos", "fptan", "f2xm1") else (top + 1) & 7, r)

        def after_trans(u, a, size, ud):
            t = state.get("trans")
            if t and t[0] == a:
                state["trans"] = None
                u.reg_write(UC_X86_REG_FP0 + t[1], d2x80(t[2]))
        for a, nxt, m in self.trans:
            uc.hook_add(UC_HOOK_CODE, on_trans, m, a, a)
            uc.hook_add(UC_HOOK_CODE, after_trans, None, nxt, nxt)
        return state


CB_UNLIFTED = C.CFUNCTYPE(C.c_int, C.POINTER(Context), C.c_uint32, C.c_uint32)


def reset_fp(uc):
    """FNINIT + default MXCSR, and zeroed x87 registers: the original and the
    callee emulator start every trial from the same floating-point state."""
    uc.reg_write(UC_X86_REG_FPCW, 0x037F)
    uc.reg_write(UC_X86_REG_FPSW, 0)
    uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)  # all empty
    for k in range(8):
        uc.reg_write(UC_X86_REG_FP0 + k, (0, 0))
    uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)


def seg_desc(base, limit, access, flags):
    return struct.pack("<Q", (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 | access << 40 | (limit >> 16 & 0xF) << 48
                       | flags << 52 | (base >> 24) << 56)


# Flat 32-bit code and data segments, and fs on the TIB, as on the Xbox.
GDT_BYTES = (bytes(8) + seg_desc(0, 0xFFFFF, 0x9B, 0xC) + seg_desc(0, 0xFFFFF, 0x93, 0xC)
             + seg_desc(TIB, 0xFFF, 0x93, 0x4))


def setup_segments(uc):
    """Load the GDT's segments (the GDT must already be in uc's memory).
    Unicorn ignores FS_BASE in 32-bit mode, so fs needs a real descriptor."""
    uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(GDT_BYTES) - 1, 0))
    for r, sel in ((UC_X86_REG_CS, 0x08), (UC_X86_REG_SS, 0x10), (UC_X86_REG_DS, 0x10),
                   (UC_X86_REG_ES, 0x10), (UC_X86_REG_FS, 0x18)):
        uc.reg_write(r, sel)


def enable_sse(uc):
    # CR0.EM=0, CR0.MP=1; CR4.OSFXSR | OSXMMEXCPT.
    uc.reg_write(UC_X86_REG_CR0, (uc.reg_read(UC_X86_REG_CR0) & ~4) | 2)
    uc.reg_write(UC_X86_REG_CR4, uc.reg_read(UC_X86_REG_CR4) | 0x600)


class Runner:
    def __init__(self, image_path, lib_path, base=0x10000, insns_path=None):
        self.image = open(image_path, "rb").read()
        self.base = base
        self.lib = C.CDLL(lib_path)
        self.lib.qd_call.argtypes = [C.c_uint32, C.POINTER(Context), C.c_char_p, C.c_int]
        self.lib.qd_writes.argtypes = [C.POINTER(C.c_uint32), C.c_int]
        self.lib.qd_function_addr.restype = C.c_uint32
        self.lib.qd_set_unlifted.argtypes = [CB_UNLIFTED]
        assert self.lib.qd_context_size() == C.sizeof(Context), "Context layout mismatch"
        # Lifted side's guest memory: page-aligned, so the callee emulator can map it directly.
        self.mm = mmap.mmap(-1, MEM_SIZE)
        self.hmem = (C.c_uint8 * MEM_SIZE).from_buffer(self.mm)
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(0, MEM_SIZE)
        enable_sse(self.uc)
        self.uc_writes = []
        self.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, acc, a, sz, v, ud: self.uc_writes.append((a, sz)))
        self.intr = None

        def on_intr(uc, intno, ud):
            self.intr = intno
            uc.emu_stop()
        self.uc.hook_add(UC_HOOK_INTR, on_intr)
        self.write_both(self.base, self.image)
        # Code is read-only: a random pointer that would overwrite the running
        # code makes the trial meaningless (the lifted code can't see it).
        self.uc.mem_protect(CODE_LO, CODE_HI - CODE_LO, UC_PROT_READ | UC_PROT_EXEC)

        # Callee emulator: runs the original bytes of callees that have no
        # lifted body, on the lifted side's memory.
        self.uc2 = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc2.mem_map_ptr(0, MEM_SIZE, UC_PROT_ALL, C.addressof(self.hmem))
        self.uc2.mem_protect(CODE_LO, CODE_HI - CODE_LO, UC_PROT_READ | UC_PROT_EXEC)
        enable_sse(self.uc2)
        self.uc2_writes = []
        self.uc2.hook_add(UC_HOOK_MEM_WRITE, lambda uc, acc, a, sz, v, ud: self.uc2_writes.append((a, sz)))
        self.intr2 = None

        def on_intr2(uc, intno, ud):
            self.intr2 = intno
            uc.emu_stop()
        self.uc2.hook_add(UC_HOOK_INTR, on_intr2)
        self.write_both(GDT, GDT_BYTES)
        self.write_both(TIB, TIB_INIT)
        setup_segments(self.uc)
        setup_segments(self.uc2)
        self.fixups = EmuFixups(insns_path or os.path.join(os.path.dirname(lib_path), "insns.txt"))
        self.fix_uc = self.fixups.install(self.uc)
        self.fix_uc2 = self.fixups.install(self.uc2)
        self.unlifted_calls = 0
        self.fpcw = 0x007F
        self.wide_reg = self.wide_from_unlifted = False
        self.cb = CB_UNLIFTED(self.run_unlifted)  # keep a reference: ctypes doesn't
        self.lib.qd_set_unlifted(self.cb)
        # Clean CPU state (segments, control registers, x87, MXCSR) for both
        # emulators, restored before every trial: a trial whose random
        # pointers send the original into data can execute anything, and
        # that must not leak into later trials.
        reset_fp(self.uc)
        reset_fp(self.uc2)
        self.uc_clean = self.uc.context_save()
        self.uc2_clean = self.uc2.context_save()

    def run_unlifted(self, cp, target, tail_esp):
        """rt::call_unlifted: run the callee's original bytes until it returns to its caller.

        Called: [esp] is the return address. Tail-called (jmp): the target may
        be the middle of an epilogue, with saved registers at [esp]; it returns
        where the tail-calling function would have, to the return address at
        that function's entry esp."""
        try:
            ctx = cp.contents
            if not CODE_LO <= target < CODE_HI:
                # A call through a null or random pointer: the original goes on to
                # execute data, depending on CPU state (flags, segments) the lifted
                # side doesn't carry. The trial says nothing about lifted code.
                self.unlifted_fault = f"call into non-code at {target:08X}"
                return 3
            slot = tail_esp if tail_esp else ctx.esp
            if slot + 4 > MEM_SIZE:
                return 2
            ret = struct.unpack_from("<I", self.mm, slot)[0]
            uc = self.uc2
            for name, r in zip(GPRS, UC_GPR):
                uc.reg_write(r, getattr(ctx, name))
            # EFLAGS aren't modelled across calls (DF clear); MXCSR carries over
            # from earlier callees in this trial, as in the original. The x87
            # state is the lifted side's.
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            x87_write(uc, ctx.fpcw, ctx.fpsw, ctx.ftop, ctx.ftag, list(ctx.st))
            for k in range(8):
                uc.reg_write(UC_X86_REG_XMM0 + k, sum(ctx.xmm[k].u[j] << (32 * j) for j in range(4)))
            self.intr2 = None
            self.unlifted_calls += 1
            # Unicorn applies the stop address when it translates a block: a block
            # cached by an earlier run with another stop address would run
            # straight past this one. Drop any cached block at the stop address.
            uc.ctl_remove_cache(ret, ret + 1)
            try:
                uc.emu_start(target, ret, timeout=UC_TIMEOUT_US, count=MAX_INSNS)
            except UcError as e:
                self.unlifted_fault = f"{target:08X}: {e} at {uc.reg_read(UC_X86_REG_EIP):08X}"
                return 3 if e.errno == UC_ERR_WRITE_PROT else 2
            if self.intr2 is not None:
                self.unlifted_fault = f"{target:08X}: interrupt {self.intr2} at {uc.reg_read(UC_X86_REG_EIP):08X}"
                return 2
            if uc.reg_read(UC_X86_REG_EIP) != ret:
                return 3
            for name, r in zip(GPRS, UC_GPR):
                setattr(ctx, name, uc.reg_read(r))
            for k in range(8):
                v = uc.reg_read(UC_X86_REG_XMM0 + k)
                for j in range(4):
                    ctx.xmm[k].u[j] = (v >> (32 * j)) & 0xFFFFFFFF
            fx = x87_read(uc)
            ctx.ftop, ctx.ftag, ctx.fpcw = fx["top"], fx["valid"], fx["cw"]
            ctx.fpsw = fx["sw"] & ~0x3800
            for p in range(8):
                v, exact = x802d(fx["regs"][p])
                ctx.st[p] = v
                if not exact and fx["valid"] >> p & 1:
                    self.wide_from_unlifted = True  # a 64-bit-precision value the lifted side can't hold
            ctx.ret_to = ret
            return 0
        except Exception as e:  # never let an exception unwind through C++ frames
            print(f"run_unlifted({target:08X}): {e!r}", file=sys.stderr)
            return 2

    def write_both(self, addr, data):
        self.uc.mem_write(addr, data)  # mem_write ignores page protection
        C.memmove(C.addressof(self.hmem) + addr, data, len(data))

    def load_scratch(self, data):
        self.scratch = data
        self.write_both(SCRATCH, data)
        self.write_both(STACK_LO, bytes(STACK_HI - STACK_LO))

    def pristine_bytes(self, a, n):
        out = bytearray(n)
        for lo, data in ((self.base, self.image), (SCRATCH, self.scratch), (TIB, TIB_INIT), (GDT, GDT_BYTES)):
            s, e = max(a, lo), min(a + n, lo + len(data))
            if s < e:
                out[s - a:e - a] = data[s - lo:e - lo]
        return bytes(out)

    def restore(self, ranges):
        # Merge and restore every written range in both memories.
        for a, n in merge(ranges):
            self.write_both(a, self.pristine_bytes(a, n))

    def trial(self, addr, rng, args=None, setup=()):
        """One trial. A targeted check (tools/check/crt_printf.py) passes the
        stack arguments and (address, bytes) blocks to place in memory first."""
        regs = [rand_word(rng) for _ in GPRS]
        regs[4] = STACK_TOP
        xmm = [[rand_word(rng, (1, 1, 6, 1)) for _ in range(4)] for _ in range(8)]
        # A shared epilogue pops its caller's saved word before returning.
        glue = [rand_word(rng) for _ in range(GLUE_POPS.get(addr, 0))]
        words = list(args or [])
        words += [rand_word(rng) for _ in range(max(0, 16 - len(words)))]
        stack = struct.pack(f"<{len(glue)}I", *glue) + struct.pack("<I", SENTINEL) + struct.pack(f"<{len(words)}I", *words)
        self.write_both(STACK_TOP, stack)
        for a, data in setup:
            self.write_both(a, data)
        setup_ranges = [(a, len(data)) for a, data in setup]
        # x87: the variant's control word, and half the time a value already in
        # ST(0) (an argument passed on the register stack); the C runtime's
        # register-stack helpers always get their arguments.
        fst = [0.0] * 8
        ftop, fvalid = 0, 0
        nargs = X87_ARGS.get(addr, 1 if rng.random() < 0.5 else 0)
        for k in range(nargs):
            ftop = (ftop - 1) & 7
            fvalid |= 1 << ftop
            if addr in X87_ARGS and rng.random() < 0.5:
                fst[ftop] = rng.choice(FLOATS) * rng.choice((1.0, 1.0, 0.37, 13.0))
            else:
                fst[ftop] = struct.unpack("<f", struct.pack("<I", rand_word(rng, (1, 1, 6, 1))))[0]

        # ---- original under Unicorn ----
        uc = self.uc
        uc.context_restore(self.uc_clean)
        self.uc2.context_restore(self.uc2_clean)
        for r, v in zip(UC_GPR, regs):
            uc.reg_write(r, v)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        for k in range(8):
            v = sum(xmm[k][j] << (32 * j) for j in range(4))
            uc.reg_write(UC_X86_REG_XMM0 + k, v)
        x87_write(uc, self.fpcw, 0, ftop, fvalid, fst)
        self.wide_from_unlifted = self.wide_reg = False
        for st in (self.fix_uc, self.fix_uc2):
            st["tsc"], st["fix_at"], st["trans"], st["sse_nan"] = 0, None, None, None
        self.fixups.quirk = ""
        self.uc_writes = []
        self.uc2_writes = []
        self.unlifted_fault = ""
        self.intr = None
        status, why = "ok", ""
        try:
            uc.emu_start(addr, SENTINEL, timeout=UC_TIMEOUT_US, count=MAX_INSNS)
            if self.intr is not None:
                status, why = "fault", f"interrupt {self.intr} at {uc_eip(uc):08X}"
            elif uc_eip(uc) != SENTINEL:
                status = "budget"
        except UcError as e:
            status = "budget" if e.errno == UC_ERR_WRITE_PROT else "fault"
            why = f"{e} at {uc_eip(uc):08X}"
        self.last_why = why  # for debugging a single trial
        if status == "budget" or self.fixups.quirk:  # never run the lifted code on a state the original didn't finish
            self.restore(list(self.uc_writes) + [(STACK_TOP, len(stack))] + setup_ranges)
            return "skip", "", regs
        uc_regs = [uc.reg_read(r) for r in UC_GPR]
        uc_xmm = [uc.reg_read(UC_X86_REG_XMM0 + k) for k in range(8)]
        uc_fx = x87_read(uc)
        uc_w = list(self.uc_writes)

        # ---- lifted ----
        ctx = Context()
        for name, v in zip(GPRS, regs):
            setattr(ctx, name, v)
        for k in range(8):
            for j in range(4):
                ctx.xmm[k].u[j] = xmm[k][j]
        for p in range(8):
            ctx.st[p] = fst[p]
        ctx.ftop, ctx.ftag, ctx.fpcw, ctx.fpsw = ftop, fvalid, self.fpcw, 0
        ctx.fs_base = TIB
        ctx.mem = C.addressof(self.hmem)
        ctx.mem_size = MEM_SIZE
        err = C.create_string_buffer(256)
        rc = self.lib.qd_call(addr, C.byref(ctx), err, 256)
        n = self.lib.qd_writes((C.c_uint32 * 2)(), 0)
        buf = (C.c_uint32 * (2 * max(1, n)))()
        self.lib.qd_writes(buf, n)
        h_w = [(buf[2 * k], buf[2 * k + 1]) for k in range(n)] + self.uc2_writes

        result, detail = "pass", ""
        if self.fixups.quirk:
            result = "skip"  # the callee emulator hit a Unicorn x87 deviation
        elif rc == 3:
            result = "skip"  # an unlifted callee ran past the budget
        elif rc == 4:
            result, detail = "fail", err.value.decode()
        elif rc == 0 and status == "fault" and ctx.ret_to != SENTINEL and uc_eip(uc) == ctx.ret_to:
            # Both overwrote their own return address (random arguments that
            # overrun a local buffer) and went to the same place: the
            # original faulted fetching there, the lifted code's caller
            # would trap at the return check.
            result = "fault-both"
        elif rc == 0 and ctx.ret_to != SENTINEL:
            result, detail = "fail", (f"returns to {ctx.ret_to:08X} instead of its caller (stack imbalance: not a whole function?)"
                                      f" [original esp={uc_regs[4]:08X} ebp={uc_regs[5]:08X}; lifted esp={ctx.esp:08X} ebp={ctx.ebp:08X}]")
        elif status == "budget":
            result = "skip"
        elif status == "fault":
            if rc != 2:
                result, detail = "fail", f"original faulted ({why}) but lifted code returned normally"
            else:
                result = "fault-both"
        elif rc != 0:
            extra = f" ({self.unlifted_fault})" if "unlifted" in err.value.decode() else ""
            result, detail = "fail", f"lifted code trapped but original returned: {err.value.decode()}{extra}"
        else:
            # A difference where both sides hold a NaN (an xmm lane or an aligned
            # memory word) is set aside: Unicorn propagates SSE NaNs by x87 rules
            # (larger payload wins), real SSE and the lifted code by operand order.
            diffs, nan_only = [], False
            for k, name in enumerate(GPRS):
                hv = getattr(ctx, name)
                if hv != uc_regs[k]:
                    diffs.append(f"{name}: original {uc_regs[k]:08X} lifted {hv:08X}")
            for k in range(8):
                hv = sum(ctx.xmm[k].u[j] << (32 * j) for j in range(4))
                if hv != uc_xmm[k]:
                    lanes = [((uc_xmm[k] >> (32 * j)) & 0xFFFFFFFF, (hv >> (32 * j)) & 0xFFFFFFFF) for j in range(4)]
                    if all(u == h or (nan32(u) and nan32(h)) for u, h in lanes):
                        nan_only = True
                    else:
                        diffs.append(f"xmm{k}: original {uc_xmm[k]:032X} lifted {hv:032X}")
            # MMX registers aren't compared: Unicorn doesn't expose them, and
            # the code that uses them (XGraphics' swizzler) only lets their
            # values out through memory, which is compared, before its emms.
            # x87: TOP, which registers hold values, those values, the control word.
            # A function that ends in MMX state (no emms: TOP 0, every
            # register tagged in use, as Bink's blitters leave it) has the
            # MMX values in the x87 registers' mantissas; the lifted code keeps
            # MMX registers apart from its x87 doubles, so only TOP and the
            # tags are compared then.
            mmx_state = ctx.ftop == 0 and ctx.ftag == 0xFF and uc_fx["top"] == 0 and uc_fx["valid"] == 0xFF
            if ctx.ftop != uc_fx["top"] or ctx.ftag != uc_fx["valid"]:
                diffs.append(f"x87 TOP/tags: original {uc_fx['top']}/{uc_fx['valid']:02X} lifted {ctx.ftop}/{ctx.ftag:02X}")
            elif not mmx_state:
                for p in range(8):
                    if uc_fx["valid"] >> p & 1 and d2x80(ctx.st[p]) != tuple(uc_fx["regs"][p]):
                        o, exact = x802d(uc_fx["regs"][p])
                        if not exact and d2x80(o) == d2x80(ctx.st[p]):
                            self.wide_reg = True  # the original holds more than 53 bits
                            continue
                        diffs.append(f"st[{p}]: original {uc_fx['regs'][p]} ({o!r}) lifted {ctx.st[p]!r}")
            if ctx.fpcw != uc_fx["cw"]:
                diffs.append(f"x87 control word: original {uc_fx['cw']:04X} lifted {ctx.fpcw:04X}")
            for a, sz in merge(uc_w + h_w):
                ub = bytes(uc.mem_read(a, sz))
                hb = bytes(self.hmem[a:a + sz])
                if ub == hb:
                    continue
                real = False
                for p in (p for p in range(sz) if ub[p] != hb[p]):
                    w = (a + p) & ~3
                    if w >= a and w + 4 <= a + sz:
                        o = w - a
                        u, h = struct.unpack_from("<I", ub, o)[0], struct.unpack_from("<I", hb, o)[0]
                        if nan32(u) and nan32(h):
                            nan_only = True
                            continue
                    real = True
                    break
                if real:
                    diffs.append(f"memory {a:08X}+{sz}: original {ub.hex()} lifted {hb.hex()}")
                    break
            if diffs:
                wide = " [a 64-bit x87 value was involved]" if (self.wide_reg or self.wide_from_unlifted) else ""
                result, detail = "fail", "; ".join(diffs[:4]) + wide
            elif nan_only:
                result = "pass-nan"
            elif self.wide_reg:
                result = "pass-x87wide"  # equal once the original's 64-bit register value is rounded to double
        self.restore(uc_w + h_w + [(STACK_TOP, len(stack))] + setup_ranges)
        return result, detail, regs


def uc_eip(uc):
    return uc.reg_read(UC_X86_REG_EIP)


def nan32(w):
    return (w & 0x7FFFFFFF) > 0x7F800000


def merge(ranges):
    iv = sorted((a, a + n) for a, n in ranges if n > 0)
    out = []
    for a, b in iv:
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return [(a, b - a) for a, b in out if b <= MEM_SIZE]


def worker(job):
    """Run all trials for a slice of functions in this process, on a thread
    with a large stack: lifted code recurses on the host stack as deeply as
    the guest does, and the harness stops runaway recursion at a fixed depth."""
    threading.stack_size((255 if WINDOWS else 512) << 20)  # Python on Windows takes less than 256 MiB
    out = []
    t = threading.Thread(target=lambda: out.append(worker_body(job)))
    t.start()
    t.join()
    return out[0]


def worker_body(job):
    img, lib, funcs, args, wid = job
    R = Runner(img, lib)
    per = {f: Counter() for f in funcs}
    first_fail = {}
    per_variant = max(1, args.trials // args.variants)
    log = open(os.path.join(args.work, f"results.{wid}.txt"), "w")
    for v in range(args.variants):
        # Variants alternate the x87 precision; the second pair uses a float-heavy heap.
        R.load_scratch(make_scratch(args.seed * 1000 + v, floaty=(v // 2) % 2 == 1))
        # The game's x87 control word (24-bit precision) and the C runtime's (53-bit).
        R.fpcw = 0x007F if v % 2 == 0 else 0x027F
        for n, f in enumerate(funcs):
            t0 = time.time()
            for k in range(per_variant):
                rng = random.Random(f"{args.seed}:{f}:{v}:{k}")
                before = R.unlifted_calls
                res, detail, regs = R.trial(f, rng)
                per[f][res] += 1
                if res == "pass" and R.unlifted_calls != before:
                    per[f]["pass-via-unlifted"] += 1  # passed through original code of an unlifted callee
                if res == "fail" and f not in first_fail:
                    first_fail[f] = (detail, regs)
                if time.time() - t0 > args.fn_seconds:
                    per[f]["timeout"] += 1
                    break
            log.write(f"{f:08X} v{v} {dict(per[f])} rss={peak_rss_mb()}MB\n")
            log.flush()
    return per, first_fail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "qilin-difftest"))
    ap.add_argument("--trials", type=int, default=24)
    ap.add_argument("--variants", type=int, default=3, help="distinct scratch-heap images")
    ap.add_argument("--only", help="lift only this function (its callees then run as original code)")
    ap.add_argument("--test", help="comma-separated function addresses to test, from the full lift")
    ap.add_argument("--reuse", action="store_true", help="reuse the library already built in --work")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0, help="test only the first N functions")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--fn-seconds", type=float, default=3.0, help="time cap per function per variant")
    ap.add_argument("--sanitize", action="store_true", help="build the lifted code with UBSan (use GCC)")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    lib = os.path.join(args.work, LIB_NAME)
    img = os.path.join(args.work, "image.bin")
    if not (args.reuse and os.path.exists(lib) and os.path.exists(img)):
        img, lib = build(args, args.work)
    probe = C.CDLL(lib)
    probe.qd_function_addr.restype = C.c_uint32
    funcs = [probe.qd_function_addr(i) for i in range(probe.qd_function_count())]
    if args.test:
        want = {int(x, 16) for x in args.test.split(",")}
        funcs = [f for f in funcs if f in want]
    if args.limit:
        funcs = funcs[:args.limit]
    # Chunks of functions, each in a fresh process: the emulators' translation
    # caches grow with every function run, so long-lived workers run out of memory.
    chunk = 120
    jobs = [(img, lib, funcs[k:k + chunk], args, k // chunk) for k in range(0, len(funcs), chunk)]
    per, first_fail = {}, {}
    with cf.ProcessPoolExecutor(args.jobs, max_tasks_per_child=1) as ex:
        for p, ff in ex.map(worker, jobs):
            per.update(p)
            first_fail.update(ff)
    tot = Counter()
    for c in per.values():
        tot.update(c)
    failing = sorted(f for f, c in per.items() if c["fail"])
    exercised = sum(1 for c in per.values() if c["pass"])
    only_fault = sum(1 for c in per.values() if not c["pass"] and not c["fail"])
    counted = ("pass", "pass-nan", "pass-x87wide", "fail", "fault-both", "skip")
    print(f"functions: {len(funcs)}  trials: {sum(tot[k] for k in counted)}  {dict(tot)}")
    print(f"functions with >=1 completed matching trial: {exercised}")
    if tot["pass-x87wide"]:
        print(f"trials matching once a 64-bit x87 register value is rounded to double: {tot['pass-x87wide']}")
    if tot["pass-nan"]:
        print(f"trials matching except NaN against NaN (Unicorn's x87-style SSE NaN propagation): {tot['pass-nan']}")
    print(f"functions where every trial faulted or hit the budget (not exercised): {only_fault}")
    print(f"functions with a mismatch: {len(failing)}")
    for f in failing[:60]:
        d, regs = first_fail[f]
        print(f"  {f:08X}: {d}")
        print("            initial regs: " + " ".join(f"{n}={v:08X}" for n, v in zip(GPRS, regs)))
    sys.exit(1 if failing else 0)


if __name__ == "__main__":
    main()
