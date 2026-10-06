#!/usr/bin/env python3
"""TCP queue estimate per phase: valid samples, p50/p95/max in ms, per direction.

usage: queues.py RUN_DIR... (each holds cake-adapt.log.gz and phases)
"""
import gzip
import sys
from pathlib import Path


def pct(values, q):
    return sorted(values)[int(q * (len(values) - 1))] if values else None


def fmt(values):
    if not values:
        return "-"
    return f"{len(values)}: {pct(values, .5):.1f} / {pct(values, .95):.1f} / {max(values):.1f}"


for run in map(Path, sys.argv[1:]):
    marks = [line.split() for line in (run / "phases").read_text().splitlines()]
    marks = [(float(t), name) for t, name in marks]
    rows = []
    for line in gzip.open(run / "cake-adapt.log.gz", "rt").read().splitlines():
        f = line.split("; ")
        if f[0] == "TCP_QUEUE":
            rows.append((float(f[2]), int(f[5]) / 1e3 if f[4] == "1" else None,
                         int(f[7]) / 1e3 if f[6] == "1" else None))
    print(run)
    for i, (start, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else float("inf")
        sel = [r for r in rows if start <= r[0] < end]
        dl = [r[1] for r in sel if r[1] is not None]
        ul = [r[2] for r in sel if r[2] is not None]
        print(f"  {name:9} down {fmt(dl):28} up {fmt(ul)}")
