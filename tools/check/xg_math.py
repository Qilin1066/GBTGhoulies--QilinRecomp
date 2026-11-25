#!/usr/bin/env python3
"""XGraphics' SSE maths (XGMatrixMultiply, XGVec3Transform …), lifted, against
the original under Unicorn, with 16-byte aligned operands.

The difftest's random pointers are misaligned three times in four, and these
functions' movaps/mulps fault on a misaligned operand, so with three pointer
arguments they almost never complete there. This gives every argument an
aligned block of floats: ordinary values at game scales, with zeros,
negative zeros, subnormals, infinities and NaNs mixed in.

Usage (after a difftest run has built its library in --work):
  xg_math.py --work /tmp/qilin-difftest [--count 2000] [--seed 1]
"""
import argparse
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "difftest"))
import difftest as D  # noqa: E402

FUNCTIONS = [0x151C08, 0x151C86, 0x151D5B, 0x151DD9, 0x151E1E, 0x151E64, 0x151F62]
BLOCKS = [D.SCRATCH + 0x3E0000 + 0x400 * k for k in range(3)]


def rand_float(rng):
    r = rng.random()
    if r < 0.05:
        return rng.choice((0.0, -0.0, float("inf"), float("-inf"), float("nan"), 1e-40, -3e-39))
    if r < 0.5:
        return rng.uniform(-2, 2)
    return rng.choice((1, -1)) * 10 ** rng.uniform(-6, 6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="/tmp/qilin-difftest")
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    R = D.Runner(os.path.join(args.work, "image.bin"), os.path.join(args.work, D.LIB_NAME))
    rng = random.Random(args.seed)
    fails = 0
    for v in range(2):
        R.load_scratch(D.make_scratch(args.seed * 1000 + v, floaty=True))
        R.fpcw = 0x007F if v == 0 else 0x027F
        for f in FUNCTIONS:
            stats = {}
            for n in range(args.count // 2):
                setup = [(b, struct.pack("<64f", *[rand_float(rng) for _ in range(64)])) for b in BLOCKS]
                res, detail, _ = R.trial(f, random.Random(f"{args.seed}:{f}:{v}:{n}"), args=list(BLOCKS) + [rng.randrange(0, 5)],
                                         setup=setup)
                stats[res] = stats.get(res, 0) + 1
                if res == "fail":
                    fails += 1
                    if fails <= 10:
                        print(f"MISMATCH {f:08X}: {detail[:300]}")
            print(f"{f:08X} v{v}: {stats}")
    print("mismatches:", fails)
    raise SystemExit(1 if fails else 0)


if __name__ == "__main__":
    main()
