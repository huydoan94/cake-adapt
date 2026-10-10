#!/usr/bin/env python3
"""profsum.py PROF_DIR: per build and workload, the runs' CPU, filter, injector and memory figures.

Each run directory holds `variant` (build mode hash kernel) and `cpu.txt` from
prof.sh. CPU ticks are user + system clock ticks (USER_HZ 100, so 1 tick =
10 ms) over the 40 s workload. One core for 40 s is 4,000 ticks, so the
percentage of one core is ticks / 40.
"""
import collections
import statistics
import sys
from pathlib import Path

ORDER = ["idle", "download", "upload", "bidirectional", "congested", "download6", "bidirectional6"]
rows = collections.defaultdict(list)
for run in sorted(Path(sys.argv[1]).iterdir()):
    if not (run / "cpu.txt").exists():
        continue
    build, mode = (run / "variant").read_text().split()[:2]
    if mode != "cpu":
        continue
    for line in (run / "cpu.txt").read_text().splitlines():
        name, *fields = line.split()
        values = dict(f.split("=") for f in fields)
        rows[(build, name)].append({k: int(v) for k, v in values.items()})

print("| Workload | Build | Runs | CPU ticks / 40 s (median) | % of one core | filter runs | filter ns/run (median) | injector runs | injector ns/run (median) | RSS kB |")
print("|---|---|---:|---|---:|---:|---:|---:|---:|---:|")
for name in ORDER:
    for build in sorted({b for b, n in rows if n == name}):
        runs = rows[(build, name)]
        ticks = [r["ticks"] for r in runs]
        per_run = [r["bpf_ns"] / r["bpf_runs"] for r in runs if r["bpf_runs"]]
        inject_per_run = [r["inject_ns"] / r["inject_runs"] for r in runs if r["inject_runs"]]
        inject = f"{statistics.median(inject_per_run):.0f}" if inject_per_run else "-"
        median = statistics.median(ticks)
        print(f"| {name} | {build} | {len(runs)} | {' / '.join(map(str, ticks))} ({median:g}) | {median / 40:.2f} "
              f"| {sum(r['bpf_runs'] for r in runs) // len(runs)} "
              f"| {statistics.median(per_run):.0f} | {sum(r['inject_runs'] for r in runs) // len(runs)} "
              f"| {inject} | {max(r['rss_kb'] for r in runs)} |")
