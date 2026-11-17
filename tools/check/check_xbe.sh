#!/usr/bin/env bash
# Full local check against your own copy of the game (not run in CI: the XBE
# can't be distributed).
#
#   tools/check/check_xbe.sh /path/to/default.xbe [build-dir]
#
# 1. unit tests
# 2. function discovery / CFG summary
# 3. Zydis vs LLVM cross-check on every reachable instruction (strict) and on a
#    linear sweep of every code section (known-benign classes reported)
set -euo pipefail
XBE=${1:?usage: check_xbe.sh /path/to/default.xbe [build-dir]}
BUILD=${2:-build}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

"$BUILD/qilin_tests"
"$BUILD/qilin" cfg "$XBE"
"$BUILD/qilin" insns "$XBE" -o "$OUT/reach.txt"
"$BUILD/qilin" insns "$XBE" --linear -o "$OUT/linear.txt"
python3 "$HERE/verify_decoder.py" --reachable "$OUT/reach.txt" --linear "$OUT/linear.txt"
