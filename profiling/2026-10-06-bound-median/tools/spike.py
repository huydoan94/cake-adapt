#!/usr/bin/env python3
"""TCP queue estimates and fping delays from spike.sh logs.

usage: spike.py LOG... (plain or .gz); the slow reflector is 10.99.0.13.
"""
import gzip
import sys

SLOW = "10.99.0.13"


def pct(values, q):
    values = sorted(values)
    return values[int(q * (len(values) - 1))] if values else float("nan")


for path in sys.argv[1:]:
    opener = gzip.open if path.endswith(".gz") else open
    rows = [line.rstrip("\n").split("; ") for line in opener(path, "rt")]
    up = [int(f[7]) / 1e3 for f in rows if f[0] == "TCP_QUEUE" and f[6] == "1"]
    down = [int(f[5]) / 1e3 for f in rows if f[0] == "TCP_QUEUE" and f[4] == "1"]
    data = [f for f in rows if f[0] == "DATA" and len(f) > 30]
    slow = [(int(f[14]) + int(f[19])) / 1e3 for f in data if f[9] == SLOW]
    other = [(int(f[14]) + int(f[19])) / 1e3 for f in data if f[9] != SLOW]
    flips = sum(1 for f in rows if len(f) > 3 and f[3].startswith("bufferbloat attribution changed"))
    print(path)
    print(f"  fping RTT increase (ms): {SLOW} p95 {pct(slow, .95):.1f} max {max(slow):.1f}; "
          f"others p95 {pct(other, .95):.1f} max {max(other):.1f}")
    print(f"  TCP upload (ms):   n={len(up)} p50={pct(up, .5):.1f} p95={pct(up, .95):.1f} "
          f"p99={pct(up, .99):.1f} max={max(up):.1f} above 20 ms: {sum(x > 20 for x in up)}")
    print(f"  TCP download (ms): n={len(down)} p50={pct(down, .5):.1f} p95={pct(down, .95):.1f} "
          f"p99={pct(down, .99):.1f} max={max(down):.1f}")
    print(f"  attribution changes: {flips}")
