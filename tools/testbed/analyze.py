#!/usr/bin/env python3
"""Summarize namespace-testbed runs: added RTT per phase and iperf3 throughput.

analyze.py RESULTS_DIR
"""
import glob
import json
import os
import re
import statistics
import sys

PROBE = re.compile(r"^\[(\d+\.\d+)\] \S+\s+: \[\d+\], (?:\d+ bytes, ([\d.]+) ms|timed out)")
CAPACITY = {"upload-steady": (8.0, None), "upload-step": (20 * 8 + 20 * 4 + 20 * 8) / 60 * 1.0, "bidirectional": (8.0, 40.0)}


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int(fraction * len(values)))] if values else float("nan")


def throughput(path):
    try:
        data = json.load(open(path))
        return data["end"]["sum_received"]["bits_per_second"] / 1e6
    except (OSError, KeyError, ValueError):
        return float("nan")


def analyze(run):
    marks = [(float(t), name) for t, name in (line.split() for line in open(f"{run}/phases"))]
    start_uptime = marks[0][0]
    samples = []
    for line in open(f"{run}/probe"):
        match = PROBE.match(line)
        if match:
            samples.append((float(match.group(1)), float(match.group(2)) if match.group(2) else None))
    if not samples:
        return None
    epoch0 = samples[0][0]
    timeline = [(start_uptime + t - epoch0, rtt) for t, rtt in samples]

    def window(begin, end):
        return [rtt for t, rtt in timeline if begin <= t < end]

    idle = [r for r in window(marks[0][0], marks[1][0]) if r is not None]
    base = statistics.median(idle)
    phases = {}
    for index, (t, name) in enumerate(marks):
        if name not in ("upload-steady", "upload-step", "bidirectional"):
            continue
        end = next(m[0] for m in marks[index + 1:] if m[1] in ("idle", "end"))
        values = window(t, end)
        lost = sum(1 for r in values if r is None)
        added = [r - base for r in values if r is not None]
        phases[name] = {
            "p50": percentile(added, 0.5), "p95": percentile(added, 0.95), "p99": percentile(added, 0.99),
            "over15": 100 * sum(a > 15 for a in added) / len(added),
            "over30": 100 * sum(a > 30 for a in added) / len(added),
            "lost": lost,
        }
    phases["upload-steady"]["used"] = 100 * throughput(f"{run}/upload-steady.json") / 8.0
    phases["upload-step"]["used"] = 100 * throughput(f"{run}/upload-step.json") / CAPACITY["upload-step"]
    phases["bidirectional"]["used"] = 100 * throughput(f"{run}/bidir-upload.json") / 8.0
    phases["bidirectional"]["used_dl"] = 100 * throughput(f"{run}/bidir-download.json") / 40.0
    return base, phases


root = sys.argv[1]
print(f"{'variant':22} {'phase':14} {'p50':>6} {'p95':>6} {'p99':>6} {'>15%':>6} {'>30%':>6} {'lost':>5} {'ul used%':>9} {'dl used%':>9}")
for run in sorted(glob.glob(f"{root}/V*")):
    result = analyze(run)
    if result is None:
        continue
    base, phases = result
    for name, m in phases.items():
        print(f"{os.path.basename(run):22} {name:14} {m['p50']:6.1f} {m['p95']:6.1f} {m['p99']:6.1f} "
              f"{m['over15']:6.1f} {m['over30']:6.1f} {m['lost']:5d} {m['used']:9.1f} "
              f"{m.get('used_dl', float('nan')):9.1f}")
    print(f"{'':22} (idle base RTT {base:.1f} ms; delays are added RTT in ms)")
