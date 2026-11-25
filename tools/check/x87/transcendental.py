"""Usage: transcendental.py [--count N] [--seed S] [--cxx g++-14] [--exec PREFIX] [--golden]

Cross-check rt::fsin, fcos, ftan, fpatan (atan2) and fyl2x (y * log2(x))
against mpmath: each result must be the double nearest the exact value of the
function at the exact (double) operands. The host libm's answers are counted
alongside, for comparison. Needs `pip install mpmath`.

--golden prints the unit-test vectors in recomp/tests/tests.cpp instead: fixed
operands, including hard ones, with their correctly rounded results. CI runs
them on every platform, so they check that each host gets the same bits."""
import argparse
import math
import os
import random
import shlex
import struct
import subprocess
import tempfile
from fractions import Fraction
from pathlib import Path

import mpmath
from mpmath import mp, mpf

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
mp.prec = 320


def d2b(d): return struct.unpack("<Q", struct.pack("<d", d))[0]
def b2d(b): return struct.unpack("<d", struct.pack("<Q", b))[0]


def nearest(v):
    """The double nearest an mpf (ties to even), subnormals and overflow included."""
    if mpmath.isinf(v):
        return math.copysign(math.inf, v)
    if v == 0:
        return 0.0
    sign, man, exp, _ = v._mpf_
    man = -int(man) if sign else int(man)
    q = Fraction(man) * (Fraction(2) ** int(exp))
    try:
        return float(q)  # Fraction -> float is correctly rounded
    except OverflowError:
        return math.copysign(math.inf, man)


def reference(op, a, b):
    x, y = mpf(a), mpf(b)
    if op == "S":
        return nearest(mpmath.sin(x))
    if op == "C":
        return nearest(mpmath.cos(x))
    if op == "T":
        return nearest(mpmath.tan(x))
    if op == "A":
        return nearest(mpmath.atan2(x, y))
    if op == "F":
        return nearest(mpmath.expm1(x * mpmath.log(2)))
    return nearest(x * mpmath.log(y, 2))


def logu(rng, lo, hi):  # log-uniform magnitude 2^lo .. 2^hi, random sign
    return rng.choice((-1, 1)) * 2.0 ** rng.uniform(lo, hi)


def near_pi2(rng):  # doubles next to multiples of pi/2, where reduction cancels most
    k = rng.randrange(1, 1 << rng.choice((4, 20, 40, 60)))
    d = float(mpf(k) * mp.pi / 2)
    return b2d(d2b(d) + rng.randint(-3, 3)) * rng.choice((-1, 1))


CASES = {
    "S": [("|x| <= pi/4", lambda r: r.uniform(-0.785, 0.785)), ("|x| <= 8", lambda r: r.uniform(-8, 8)),
          ("2^-30..2^62", lambda r: logu(r, -30, 62.9)), ("near k pi/2", near_pi2),
          ("tiny 2^-1074..2^-30", lambda r: logu(r, -1074, -30))],
    "A": [("ratio 2^-4..2^4", lambda r: (logu(r, -10, 10), logu(r, -10, 10))),
          ("ratio 2^-60..2^60", lambda r: (logu(r, -30, 30), logu(r, -30, 30) * 2.0 ** r.uniform(-30, 30))),
          ("magnitudes 2^-1000..2^1000", lambda r: (logu(r, -1000, 1000), logu(r, -1000, 1000))),
          ("subnormal operands", lambda r: (logu(r, -1074, -1023), logu(r, -1074, -1023)))],
    "L": [("x 2^-8..2^8, y 2^-8..2^8", lambda r: (logu(r, -8, 8), 2.0 ** r.uniform(-8, 8))),
          ("x near 1", lambda r: (logu(r, -20, 20), 1.0 + r.choice((-1, 1)) * 2.0 ** r.uniform(-52, -1))),
          ("x 2^-1074..2^1024", lambda r: (logu(r, -40, 40), 2.0 ** r.uniform(-1074, 1023.9))),
          ("y 2^-1000..2^1020", lambda r: (logu(r, -1000, 1020), 2.0 ** r.uniform(-30, 30))),
          ("subnormal results", lambda r: (logu(r, -1074, -1023), 2.0 ** r.uniform(-30, 30)))],
    "F": [("|x| <= 1", lambda r: r.uniform(-1, 1)), ("2^-60..1", lambda r: logu(r, -60, 0)),
          ("near k/32", lambda r: r.randint(-32, 32) / 32 + logu(r, -50, -6)),
          ("tiny 2^-1074..2^-60", lambda r: logu(r, -1074, -60)),
          ("beyond the x87's domain", lambda r: r.choice((r.uniform(1, 1024), r.uniform(-64, -1))))],
}
CASES["C"] = CASES["S"]
CASES["T"] = CASES["S"]
NAMES = {"S": "fsin", "C": "fcos", "T": "ftan", "A": "fpatan", "L": "fyl2x", "F": "f2xm1"}
H = float.fromhex
GOLDEN = [  # (op, a, b): small and large arguments, near multiples of pi/2, subnormals
    ("S", 0.5, 0), ("S", -3.0, 0), ("S", 1e6, 0), ("S", H("0x1.921fb54442d18p+1"), 0), ("S", H("0x1.fffffffffffffp+62"), 0),
    ("S", 2.0 ** -1000, 0), ("S", 355.0, 0), ("S", H("0x1.921fb54442d18p+20"), 0), ("S", 6381956970095103.0, 0),
    ("C", 0.5, 0), ("C", H("0x1.921fb54442d18p+0"), 0), ("C", 22.0, 0), ("C", 1e15, 0), ("C", -7.0, 0),
    ("C", float(mpf(2 ** 40 + 17) * mp.pi / 2), 0),
    ("T", 0.75, 0), ("T", H("0x1.921fb54442d18p+0"), 0), ("T", -100.0, 0), ("T", 1e10, 0),
    ("A", 1.0, 1.0), ("A", -2.5, 3.0), ("A", 1e-300, -1.0), ("A", 3.0, -1e-10), ("A", 2.0 ** -1060, 2.0 ** -1070),
    ("A", 1e-200, 1e100), ("A", -1.0, -1.0), ("A", 1e-300, 1e10),
    ("L", 1.0, 3.0), ("L", -2.5, 0.1), ("L", 3.0, 1.0000000000000002), ("L", 1e-310, 7.0), ("L", 5e300, 1e-300),
    ("L", 0.5, 2.0 ** -1074), ("L", 1.0, 1.0 - 2.0 ** -53),
    ("F", 0.5, 0), ("F", -0.5, 0), ("F", 1.0, 0), ("F", -1.0, 0), ("F", 2.0 ** -70, 0), ("F", -(2.0 ** -1070), 0),
    ("F", 0.03125, 0), ("F", -0.015625, 0), ("F", H("0x1.fffffffffffffp-1"), 0), ("F", 1023.5, 0), ("F", -55.0, 0),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=4000, help="cases per range")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cxx", default="g++-14")
    ap.add_argument("--exec", default="", help="run the driver through this, e.g. qemu-aarch64 for a cross build")
    ap.add_argument("--golden", action="store_true", help="print the unit-test vectors")
    args = ap.parse_args()
    if args.golden:
        call = {"S": "fsin({a})", "C": "fcos({a})", "T": "ftan({a})", "A": "fpatan({a}, {b})", "L": "fyl2x({a}, {b})",
                "F": "f2xm1({a})"}
        for op, a, b in GOLDEN:
            r = reference(op, a, b)
            expr = call[op].format(a=f"rt::dfrom(0x{d2b(a):016X}ull)", b=f"rt::dfrom(0x{d2b(b):016X}ull)")
            print(f"        {{rt::dbits(rt::{expr}), 0x{d2b(r):016X}ull}},  // {NAMES[op]}({a!r}{', ' + repr(b) if op in 'AL' else ''}) = {r!r}")
        return
    driver = str(Path(tempfile.mkdtemp()) / ("x87driver.exe" if os.name == "nt" else "x87driver"))
    subprocess.run([*shlex.split(args.cxx, posix=os.name != "nt"), "-std=c++23", "-O2", "-ffp-contract=off", "-I", str(ROOT / "recomp" / "runtime"),
                    str(HERE / "driver.cpp"), "-o", driver], check=True)
    rng = random.Random(args.seed)
    jobs = []
    for op, ranges in CASES.items():
        for name, gen in ranges:
            for _ in range(args.count):
                v = gen(rng)
                a, b = v if isinstance(v, tuple) else (v, 0.0)
                if op == "L":
                    b = abs(b)
                jobs.append((op, name, a, b))
    out = subprocess.run([*args.exec.split(), driver], input="".join(f"{op} {d2b(a):x} {d2b(b):x}\n" for op, _, a, b in jobs),
                         capture_output=True, text=True, check=True).stdout.split("\n")
    stats, bad = {}, 0
    for (op, name, a, b), line in zip(jobs, out):
        ours, libm = (b2d(int(t, 16)) for t in line.split())
        ref = reference(op, a, b)
        s = stats.setdefault((op, name), [0, 0, 0])
        s[0] += 1
        same = lambda v: d2b(v) == d2b(ref)
        if not same(ours):
            s[1] += 1
            if bad < 10:
                print(f"  MISMATCH {NAMES[op]}({a!r}, {b!r}): got {ours!r}, nearest double is {ref!r}")
            bad += 1
        if not same(libm):
            s[2] += 1
    print(f"{'function':8} {'operands':28} {'cases':>6} {'wrong':>6} {'host libm wrong':>16}")
    for (op, name), (n, w, h) in stats.items():
        print(f"{NAMES[op]:8} {name:28} {n:6} {w:6} {h:16}")
    print("mismatches:", bad)
    raise SystemExit(1 if bad else 0)


if __name__ == "__main__":
    main()
