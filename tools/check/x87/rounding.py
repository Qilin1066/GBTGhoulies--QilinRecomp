"""Usage: rounding.py [seed] [c++ compiler]

Cross-check rt::fadd/fsub/fmul/fdiv/fsqrt at 24-bit precision control against
exact rational arithmetic rounded to a 24-bit significand (ties to even)."""
import random
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction as F
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CXX = sys.argv[2] if len(sys.argv) > 2 else "g++-14"
DRIVER = str(Path(tempfile.mkdtemp()) / "x87driver")
subprocess.run([CXX, "-std=c++23", "-O2", "-ffp-contract=off", "-I", str(ROOT / "recomp" / "runtime"),
                str(HERE / "driver.cpp"), "-o", DRIVER], check=True)


def d2b(d): return struct.unpack('<Q', struct.pack('<d', d))[0]
def b2d(b): return struct.unpack('<d', struct.pack('<Q', b))[0]


def exponent(q):  # floor(log2(q)) for q > 0
    e = q.numerator.bit_length() - q.denominator.bit_length()
    while F(2) ** e > q:
        e -= 1
    while F(2) ** (e + 1) <= q:
        e += 1
    return e


def round24(q):
    if q == 0:
        return F(0)
    s = -1 if q < 0 else 1
    q = abs(q)
    ulp = F(2) ** (exponent(q) - 23)
    m = q / ulp
    fl = m.numerator // m.denominator
    rem = m - fl
    if rem > F(1, 2) or (rem == F(1, 2) and fl % 2 == 1):
        fl += 1
    return s * fl * ulp


def sqrt_ok(a, got):
    """got is the nearest 24-bit value to sqrt(a): got is 24-bit, and a lies between
    the squares of the midpoints to its neighbours (midpoints never tie exactly)."""
    if round24(got) != got:
        return False
    if got == 0:
        return a == 0
    ulp_hi = F(2) ** (exponent(got) - 23)
    # the neighbour below has a smaller ulp when got is a power of two
    ulp_lo = ulp_hi / 2 if got == F(2) ** exponent(got) else ulp_hi
    lo_mid, hi_mid = got - ulp_lo / 2, got + ulp_hi / 2
    return lo_mid * lo_mid <= a <= hi_mid * hi_mid


rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 7)


def narrow():  # a float value: 24-bit significand
    u = rnd.getrandbits(32) & 0x3FFFFFFF | rnd.choice([0, 0x80000000])
    return struct.unpack('<f', struct.pack('<I', u))[0]


def wide():
    k = rnd.choice(['dbl', 'int', 'big', 'near'])
    if k == 'dbl':
        return rnd.uniform(-1e6, 1e6)
    if k == 'int':
        return float(rnd.randint(-2**31, 2**31 - 1))
    if k == 'near':  # a float plus a tiny tail: stresses ties after the first rounding
        f = narrow()
        return f + f * 2.0**-25 * rnd.choice([1, -1]) + f * 2.0**-50 * rnd.choice([1, -1, 0])
    return rnd.uniform(-1, 1) * 2.0 ** rnd.randint(-60, 60)


cases = []
while len(cases) < 40000:
    a = narrow() if rnd.random() < .4 else wide()
    b = narrow() if rnd.random() < .4 else wide()
    if not (1e-30 < abs(a) < 1e30 and 1e-30 < abs(b) < 1e30):
        continue
    op = rnd.choice('+-*/s')
    if op == 's':
        a = abs(a)
    cases.append((op, a, b))
inp = "".join(f"{op} {d2b(a):x} {d2b(b):x}\n" for op, a, b in cases)
out = subprocess.run([DRIVER], input=inp, capture_output=True, text=True, check=True).stdout.split()
bad = 0
for (op, a, b), o in zip(cases, out, strict=True):
    got = F(b2d(int(o, 16)))
    A, B = F(a), F(b)
    if op == 's':
        ok = sqrt_ok(A, got)
    else:
        exact = {'+': A + B, '-': A - B, '*': A * B, '/': A / B}[op]
        ok = got == round24(exact)
    if not ok:
        bad += 1
        if bad <= 5:
            print('MISMATCH', op, a.hex(), b.hex(), float(got).hex())
print(len(cases), 'cases,', bad, 'mismatches')
sys.exit(1 if bad else 0)
