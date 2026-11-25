#!/usr/bin/env python3
"""The C runtime's sprintf, lifted, against the original under Unicorn, on
real format strings.

The difftest's random states rarely make a valid format string, so this
builds them: every conversion the CRT knows (d i u x X o c s p e E f g G n
excluded), flags, widths and precisions (literal and *), size prefixes, and
for the floating-point conversions values across the whole double range,
rounding ties, subnormals, zeros, infinities and NaNs. Each case runs under
both of the game's x87 precisions, through the difftest's harness, which
compares registers, the x87 state and every byte written.

Usage (after a difftest run has built its library in --work):
  crt_printf.py --work /tmp/qilin-difftest [--count 3000] [--seed 1]
"""
import argparse
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "difftest"))
import difftest as D  # noqa: E402

SPRINTF = 0x1661F9
FMT = D.SCRATCH + 0x3F0000
STR = D.SCRATCH + 0x3F4000
BUF = D.SCRATCH + 0x3F8000
# The float-formatting hooks printf calls through, as the C runtime's start-up
# sets them (_cfltcvt_init at 0x167389); the raw image holds _fptrap there.
CFLTCVT_TAB = (0x3381A0, struct.pack("<6I", 0x16B4EC, 0x16B199, 0x16B1FE, 0x16B141, 0x16B1E4, 0x16B4EC))

H = float.fromhex
SPECIAL = [0.0, -0.0, float("inf"), float("-inf"), float("nan"), 1.0, -1.0, 0.5, 2.5, 0.125, 1e15, 1e16, 1e17,
           123456789.0, 0.1, 1 / 3, 9.999999, 9.9999995, 99.5, 0.05, 5e-324, 2.2250738585072014e-308,
           1.7976931348623157e308, H("0x1.fffffffffffffp-1"), 4503599627370496.5, 1e-5, 1e-4, 1e21, 1e22]


def rand_double(rng):
    r = rng.random()
    if r < 0.25:
        return rng.choice(SPECIAL) * rng.choice((1, -1))
    if r < 0.5:  # anything
        return struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
    if r < 0.75:  # ordinary magnitudes
        return rng.choice((1, -1)) * 10 ** rng.uniform(-12, 18)
    # a few decimal digits: exercises rounding at the precision asked for
    return round(rng.uniform(-1000, 1000), rng.randrange(0, 8))


def spec(rng):
    """One conversion: (text, stack words)."""
    words = []
    flags = "".join(sorted(set(rng.choice("-+ #0") for _ in range(rng.randrange(0, 3)))))
    width = rng.choice(["", "", str(rng.randrange(1, 30)), "*"])
    if width == "*":
        words.append(rng.choice((rng.randrange(0, 30), -rng.randrange(1, 30))) & 0xFFFFFFFF)
    prec = rng.choice(["", "", "." + str(rng.randrange(0, 20)), ".", ".*"])
    if prec == ".*":
        words.append(rng.choice((rng.randrange(0, 25), -1)) & 0xFFFFFFFF)
    conv = rng.choice("ddiuxXocspeEfgGfffgge%")
    size = ""
    if conv in "diuxXo":
        size = rng.choice(["", "", "", "h", "l", "I64"])
        if size == "I64":
            words += list(struct.unpack("<2I", struct.pack("<Q", rng.getrandbits(64) >> rng.randrange(0, 64))))
        else:
            words.append(rng.choice((rng.getrandbits(32), rng.randrange(0, 100), (-rng.randrange(1, 100)) & 0xFFFFFFFF)))
    elif conv in "eEfgG":
        size = rng.choice(["", "", "l"])
        words += list(struct.unpack("<2I", struct.pack("<d", rand_double(rng))))
    elif conv == "c":
        words.append(rng.randrange(32, 127))
    elif conv == "s":
        words.append(rng.choice((STR, STR + rng.randrange(0, 40), 0)))  # 0: the CRT prints "(null)"
    elif conv == "p":
        words.append(rng.getrandbits(32))
    if conv == "%":
        return "%%", []
    return f"%{flags}{width}{prec}{size}{conv}", words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="/tmp/qilin-difftest")
    ap.add_argument("--count", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    R = D.Runner(os.path.join(args.work, "image.bin"), os.path.join(args.work, D.LIB_NAME))
    rng = random.Random(args.seed)
    text = bytes(rng.choice(b"abcdefghijklmnopqrstuvwxyz ABC0123456789") for _ in range(80)) + b"\0"
    stats, fails = {}, 0
    for v in range(2):
        R.load_scratch(D.make_scratch(args.seed * 1000 + v))
        R.fpcw = 0x007F if v == 0 else 0x027F  # the game's 24-bit precision, the CRT's 53-bit
        for n in range(args.count):
            parts, words = [], []
            for _ in range(rng.randrange(1, 5)):
                if rng.random() < 0.5:
                    parts.append(rng.choice(["x=", " ", "[", "] ", ", ", "abc"]))
                t, w = spec(rng)
                parts.append(t)
                words += w
            fmt = "".join(parts).encode() + b"\0"
            setup = [CFLTCVT_TAB, (FMT, fmt), (STR, text), (BUF, bytes(512))]
            res, detail, _ = R.trial(SPRINTF, random.Random(f"{args.seed}:{v}:{n}"), args=[BUF, FMT] + words, setup=setup)
            stats[res] = stats.get(res, 0) + 1
            if res == "fail":
                fails += 1
                if fails <= 10:
                    print(f"MISMATCH {fmt[:-1].decode()!r} words {[hex(w) for w in words]}: {detail[:300]}")
    print("sprintf:", stats)
    raise SystemExit(1 if fails else 0)


if __name__ == "__main__":
    main()
