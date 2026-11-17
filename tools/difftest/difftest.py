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

Usage:
  difftest.py --qilin build/qilin --xbe default.xbe [--work /tmp/qd]
              [--trials 24] [--only ADDR] [--cxx g++-14] [--sanitize]

Exit status 1 if any function mismatches. Needs: pip install unicorn
"""
import argparse
import concurrent.futures as cf
import ctypes as C
import os
import random
import struct
import subprocess
import sys
import time
from collections import Counter

from unicorn import (UC_ARCH_X86, UC_ERR_WRITE_PROT, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_32, UC_PROT_EXEC,
                     UC_PROT_READ, Uc, UcError)
from unicorn.x86_const import (UC_X86_REG_CR0, UC_X86_REG_CR4, UC_X86_REG_EAX, UC_X86_REG_EBP,
                               UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI, UC_X86_REG_EDX,
                               UC_X86_REG_EFLAGS, UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_MXCSR,
                               UC_X86_REG_XMM0)

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
        ("mem", C.c_void_p), ("mem_size", C.c_uint64)]


FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.14159, -100.25, 1e-3, 1e10, -1e10, 1e-30, 65536.0,
          float("inf"), float("-inf"), 0.1, 255.0, 1e-40]


def f2u(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def rand_word(rng, kind_weights=(4, 3, 3, 1)):
    k = rng.choices(range(4), kind_weights)[0]
    if k == 0:  # pointer into scratch, 16-aligned plus a small offset
        return SCRATCH + rng.randrange(0, SCRATCH_SIZE - 0x10000, 16) + rng.choice([0, 0, 0, 4, 8, 12])
    if k == 1:
        return rng.randrange(0, 64)
    if k == 2:
        return f2u(rng.choice(FLOATS) if rng.random() < 0.5 else rng.uniform(-1000, 1000))
    return rng.getrandbits(32)


def make_scratch(seed):
    rng = random.Random(seed)
    words = [rand_word(rng) for _ in range(SCRATCH_SIZE // 4)]
    return struct.pack(f"<{len(words)}I", *words)


def build(args, work):
    gen = os.path.join(work, "gen")
    img = os.path.join(work, "image.bin")
    subprocess.run([args.qilin, "image", args.xbe, img], check=True, stdout=subprocess.DEVNULL)
    cmd = [args.qilin, "lift", args.xbe, "--out", gen]
    if args.only:
        cmd += ["--only", args.only]
    print(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.splitlines()[0])
    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".cpp"))
    srcs.append(os.path.join(HERE, "harness.cpp"))
    # Production optimisation level and FP rules; warnings are errors, so a
    # generator regression that produces sloppy C++ fails here too.
    flags = ["-std=c++23", "-O2", "-fPIC", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
             "-DQILIN_CHECKED_MEMORY", "-DQILIN_TRACE_WRITES", "-I", os.path.join(ROOT, "recomp", "runtime")]
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


class Runner:
    def __init__(self, image_path, lib_path, base=0x10000):
        self.image = open(image_path, "rb").read()
        self.base = base
        self.lib = C.CDLL(lib_path)
        self.lib.qd_call.argtypes = [C.c_uint32, C.POINTER(Context), C.c_char_p, C.c_int]
        self.lib.qd_writes.argtypes = [C.POINTER(C.c_uint32), C.c_int]
        self.lib.qd_function_addr.restype = C.c_uint32
        assert self.lib.qd_context_size() == C.sizeof(Context), "Context layout mismatch"
        self.hmem = (C.c_uint8 * MEM_SIZE)()
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(0, MEM_SIZE)
        # Enable SSE: CR0.EM=0, CR0.MP=1; CR4.OSFXSR | OSXMMEXCPT.
        self.uc.reg_write(UC_X86_REG_CR0, (self.uc.reg_read(UC_X86_REG_CR0) & ~4) | 2)
        self.uc.reg_write(UC_X86_REG_CR4, self.uc.reg_read(UC_X86_REG_CR4) | 0x600)
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

        # ---- original under Unicorn ----
        uc = self.uc
        for r, v in zip(UC_GPR, regs):
            uc.reg_write(r, v)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)
        for k in range(8):
            v = sum(xmm[k][j] << (32 * j) for j in range(4))
            uc.reg_write(UC_X86_REG_XMM0 + k, v)
        self.uc_writes = []
        self.intr = None
        status = "ok"
        try:
            uc.emu_start(addr, SENTINEL, timeout=UC_TIMEOUT_US, count=MAX_INSNS)
            if self.intr is not None:
                status = "fault"
            elif uc_eip(uc) != SENTINEL:
                status = "budget"
        except UcError as e:
            status = "budget" if e.errno == UC_ERR_WRITE_PROT else "fault"
        if status == "budget":  # never run the lifted code on a state the original didn't finish
            self.restore(list(self.uc_writes) + [(STACK_TOP, len(stack))])
            return "skip", "", regs
        uc_regs = [uc.reg_read(r) for r in UC_GPR]
        uc_xmm = [uc.reg_read(UC_X86_REG_XMM0 + k) for k in range(8)]
        uc_w = list(self.uc_writes)

        # ---- lifted ----
        ctx = Context()
        for name, v in zip(GPRS, regs):
            setattr(ctx, name, v)
        for k in range(8):
            for j in range(4):
                ctx.xmm[k].u[j] = xmm[k][j]
        ctx.mem = C.addressof(self.hmem)
        ctx.mem_size = MEM_SIZE
        err = C.create_string_buffer(256)
        rc = self.lib.qd_call(addr, C.byref(ctx), err, 256)
        n = self.lib.qd_writes((C.c_uint32 * 2)(), 0)
        buf = (C.c_uint32 * (2 * max(1, n)))()
        self.lib.qd_writes(buf, n)
        h_w = [(buf[2 * k], buf[2 * k + 1]) for k in range(n)]

        result, detail = "pass", ""
        if rc == 0 and ctx.ret_to != SENTINEL:
            result, detail = "fail", f"returns to {ctx.ret_to:08X} instead of its caller (stack imbalance: not a whole function?)"
        elif status == "budget":
            result = "skip"
        elif status == "fault":
            if rc != 2:
                result, detail = "fail", "original faulted but lifted code returned normally"
            else:
                result = "fault-both"
        elif rc != 0:
            result, detail = "fail", f"lifted code trapped but original returned: {err.value.decode()}"
        else:
            diffs = []
            for k, name in enumerate(GPRS):
                hv = getattr(ctx, name)
                if hv != uc_regs[k]:
                    diffs.append(f"{name}: original {uc_regs[k]:08X} lifted {hv:08X}")
            for k in range(8):
                hv = sum(ctx.xmm[k].u[j] << (32 * j) for j in range(4))
                if hv != uc_xmm[k]:
                    diffs.append(f"xmm{k}: original {uc_xmm[k]:032X} lifted {hv:032X}")
            for a, sz in merge(uc_w + h_w):
                ub = bytes(uc.mem_read(a, sz))
                hb = bytes(self.hmem[a:a + sz])
                if ub != hb:
                    diffs.append(f"memory {a:08X}+{sz}: original {ub.hex()} lifted {hb.hex()}")
                    break
            if diffs:
                result, detail = "fail", "; ".join(diffs[:4])
        self.restore(uc_w + h_w + [(STACK_TOP, len(stack))])
        return result, detail, regs


def uc_eip(uc):
    from unicorn.x86_const import UC_X86_REG_EIP
    return uc.reg_read(UC_X86_REG_EIP)


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
    """Run all trials for a slice of functions in this process."""
    img, lib, funcs, args, wid = job
    R = Runner(img, lib)
    per = {f: Counter() for f in funcs}
    first_fail = {}
    per_variant = max(1, args.trials // args.variants)
    log = open(os.path.join(args.work, f"results.{wid}.txt"), "w")
    for v in range(args.variants):
        R.load_scratch(make_scratch(args.seed * 1000 + v))
        for n, f in enumerate(funcs):
            t0 = time.time()
            for k in range(per_variant):
                rng = random.Random(f"{args.seed}:{f}:{v}:{k}")
                res, detail, regs = R.trial(f, rng)
                per[f][res] += 1
                if res == "fail" and f not in first_fail:
                    first_fail[f] = (detail, regs)
                if time.time() - t0 > args.fn_seconds:
                    per[f]["timeout"] += 1
                    break
            log.write(f"{f:08X} v{v} {dict(per[f])}\n")
            log.flush()
    return per, first_fail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--work", default="/tmp/qilin-difftest")
    ap.add_argument("--trials", type=int, default=24)
    ap.add_argument("--variants", type=int, default=3, help="distinct scratch-heap images")
    ap.add_argument("--only")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0, help="test only the first N functions")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--fn-seconds", type=float, default=3.0, help="time cap per function per variant")
    ap.add_argument("--sanitize", action="store_true", help="build the lifted code with UBSan (use GCC)")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    img, lib = build(args, args.work)
    probe = C.CDLL(lib)
    probe.qd_function_addr.restype = C.c_uint32
    funcs = [probe.qd_function_addr(i) for i in range(probe.qd_function_count())]
    if args.limit:
        funcs = funcs[:args.limit]
    jobs = [(img, lib, funcs[w::args.jobs], args, w) for w in range(args.jobs)]
    per, first_fail = {}, {}
    with cf.ProcessPoolExecutor(args.jobs) as ex:
        for p, ff in ex.map(worker, jobs):
            per.update(p)
            first_fail.update(ff)
    tot = Counter()
    for c in per.values():
        tot.update(c)
    failing = sorted(f for f, c in per.items() if c["fail"])
    exercised = sum(1 for c in per.values() if c["pass"])
    only_fault = sum(1 for c in per.values() if not c["pass"] and not c["fail"])
    print(f"functions: {len(funcs)}  trials: {sum(v for k, v in tot.items() if k != 'timeout')}  {dict(tot)}")
    print(f"functions with >=1 completed matching trial: {exercised}")
    print(f"functions where every trial faulted or hit the budget (not exercised): {only_fault}")
    print(f"functions with a mismatch: {len(failing)}")
    for f in failing[:60]:
        d, regs = first_fail[f]
        print(f"  {f:08X}: {d}")
        print("            initial regs: " + " ".join(f"{n}={v:08X}" for n, v in zip(GPRS, regs)))
    sys.exit(1 if failing else 0)


if __name__ == "__main__":
    main()
