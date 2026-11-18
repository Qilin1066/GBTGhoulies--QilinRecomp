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
"""
import argparse
import concurrent.futures as cf
import ctypes as C
import fractions
import math
import mmap
import os
import random
import resource
import struct
import subprocess
import sys
import threading
import time
from collections import Counter

from unicorn import (UC_ARCH_X86, UC_ERR_WRITE_PROT, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_32, UC_PROT_ALL,
                     UC_PROT_EXEC, UC_PROT_READ, Uc, UcError)
from unicorn.x86_const import (UC_X86_REG_CR0, UC_X86_REG_CR4, UC_X86_REG_EAX, UC_X86_REG_EBP,
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
        ("eflags", C.c_uint32), ("ret_to", C.c_uint32), ("xmm", Xmm * 8),
        ("st", C.c_double * 8), ("ftop", C.c_uint32), ("ftag", C.c_uint32), ("fpcw", C.c_uint32), ("fpsw", C.c_uint32),
        ("mem", C.c_void_p), ("mem_size", C.c_uint64)]


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
    d = float(q)  # correctly rounded
    return (-d if sign else d), fractions.Fraction(d) == q


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


def rand_word(rng, floaty=False):
    k = rng.choices(range(4), (2, 1, 8, 1) if floaty else (4, 3, 3, 1))[0]
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
    words = [rand_word(rng, floaty) for _ in range(SCRATCH_SIZE // 4)]
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
    flags = ["-std=c++23", "-O2", "-fPIC", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
             "-DQILIN_CHECKED_MEMORY", "-DQILIN_TRACE_WRITES", "-DQILIN_TEST_HOOKS",
             "-I", os.path.join(ROOT, "recomp", "runtime")]
    if args.sanitize:
        # Undefined behaviour in generated code (shifts, overflow, bad casts) aborts the run.
        flags += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
    objs = []

    def cc(src):
        obj = os.path.join(work, os.path.basename(src) + ".o")
        subprocess.run([args.cxx, *flags, "-c", src, "-o", obj], check=True)
        return obj

    with cf.ThreadPoolExecutor(os.cpu_count() or 2) as ex:
        objs = list(ex.map(cc, srcs))
    lib = os.path.join(work, "libdifftest.so")
    link = ["-fsanitize=undefined"] if args.sanitize else []
    subprocess.run([args.cxx, "-shared", *link, "-o", lib, *objs], check=True)
    return img, lib


class EmuFixups:
    """Where Unicorn (QEMU 5) differs from the hardware, make it follow the
    hardware, identically in both emulators:
      - rdtsc returns the host's time-stamp counter: replaced by a counter that
        restarts every trial and steps by a fixed amount;
      - fcomi/fcomip/fucomi/fucomip leave OF, SF and AF unchanged: Intel clears them;
      - fyl2x with an operand <= 0 doesn't pop: the trial is skipped."""

    def __init__(self, insns_path):
        self.rdtsc, self.fcomi, self.fyl2x = [], [], []
        for line in open(insns_path):
            parts = line.split(None, 3)
            a, n, m = int(parts[0], 16), int(parts[1]), parts[3].split("\t")[0]
            if m == "rdtsc":
                self.rdtsc.append(a)
            elif m in ("fcomi", "fcomip", "fucomi", "fucomip"):
                self.fcomi.append((a, a + n))
            elif m == "fyl2x":
                self.fyl2x.append(a)
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

        def on_fyl2x(u, a, size, ud):
            top = (u.reg_read(UC_X86_REG_FPSW) >> 11) & 7
            v, _ = x802d(u.reg_read(UC_X86_REG_FP0 + top))
            if not v > 0:
                self_.quirk = f"fyl2x of {v} at {a:08X} (Unicorn doesn't pop)"
        for a in self.fyl2x:
            uc.hook_add(UC_HOOK_CODE, on_fyl2x, None, a, a)
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
        for lo, data in ((self.base, self.image), (SCRATCH, self.scratch)):
            s, e = max(a, lo), min(a + n, lo + len(data))
            if s < e:
                out[s - a:e - a] = data[s - lo:e - lo]
        return bytes(out)

    def restore(self, ranges):
        # Merge and restore every written range in both memories.
        for a, n in merge(ranges):
            self.write_both(a, self.pristine_bytes(a, n))

    def trial(self, addr, rng):
        regs = [rand_word(rng) for _ in GPRS]
        regs[4] = STACK_TOP
        xmm = [[rand_word(rng, (1, 1, 6, 1)) for _ in range(4)] for _ in range(8)]
        stack = struct.pack("<I", SENTINEL) + struct.pack("<16I", *[rand_word(rng) for _ in range(16)])
        self.write_both(STACK_TOP, stack)
        # x87: the variant's control word, and half the time a value already in
        # ST(0) (an argument passed on the register stack).
        fst = [0.0] * 8
        ftop, fvalid = 0, 0
        if rng.random() < 0.5:
            ftop, fvalid = 7, 1 << 7
            fst[7] = struct.unpack("<f", struct.pack("<I", rand_word(rng, (1, 1, 6, 1))))[0]

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
            st["tsc"], st["fix_at"] = 0, None
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
        if status == "budget" or self.fixups.quirk:  # never run the lifted code on a state the original didn't finish
            self.restore(list(self.uc_writes) + [(STACK_TOP, len(stack))])
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
        elif rc == 0 and ctx.ret_to != SENTINEL:
            result, detail = "fail", f"returns to {ctx.ret_to:08X} instead of its caller (stack imbalance: not a whole function?)"
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
            # x87: TOP, which registers hold values, those values, the control word.
            if ctx.ftop != uc_fx["top"] or ctx.ftag != uc_fx["valid"]:
                diffs.append(f"x87 TOP/tags: original {uc_fx['top']}/{uc_fx['valid']:02X} lifted {ctx.ftop}/{ctx.ftag:02X}")
            else:
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
        self.restore(uc_w + h_w + [(STACK_TOP, len(stack))])
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
    threading.stack_size(512 << 20)
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
            log.write(f"{f:08X} v{v} {dict(per[f])} rss={resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >> 10}MB\n")
            log.flush()
    return per, first_fail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--work", default="/tmp/qilin-difftest")
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
    lib = os.path.join(args.work, "libdifftest.so")
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
