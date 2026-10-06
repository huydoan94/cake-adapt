#!/usr/bin/env python3
"""Median [min-max] over repetitions, per variant and phase, from analyze.py JSON files."""
import json
import statistics
import sys
from pathlib import Path

ORDER = ["off", "before", "after", "head", "noguard"]
PHASES = ["upload", "download", "mixed", "recovery"]
METRICS = [
    ("UL Mb/s", lambda r, p: r["throughput"].get(p, {}).get("upload", {}).get("mbps")),
    ("DL Mb/s", lambda r, p: r["throughput"].get(p, {}).get("download", {}).get("mbps")),
    ("p50 ms", lambda r, p: r["phases"][p]["added_p50_ms"]),
    ("p95 ms", lambda r, p: r["phases"][p]["added_p95_ms"]),
    (">30ms %", lambda r, p: r["phases"][p]["above30ms_percent"]),
    ("UDP p95", lambda r, p: r["phases"][p]["udp_added_p95_ms"]),
    ("BPF ns", lambda r, p: r["phases"][p]["bpf_ns_per_run"]),
    ("daemon %", lambda r, p: r["phases"][p]["daemon_cpu_percent_one_core"]),
]


def cell(values):
    values = [v for v in values if v is not None]
    if not values:
        return "-"
    median = statistics.median(values)
    return f"{median:.1f} [{min(values):.1f}-{max(values):.1f}]" if len(values) > 1 else f"{median:.1f}"


def main():
    runs = {}
    for path in sys.argv[1:]:
        for result in json.loads(Path(path).read_text()):
            runs.setdefault(result["variant"], []).append(result)
    reps = max(len(v) for v in runs.values())
    print(f"repetitions per variant: {', '.join(f'{k}={len(runs[k])}' for k in ORDER if k in runs)}")
    for phase in PHASES:
        print(f"\n## {phase}")
        print("| variant | " + " | ".join(name for name, _ in METRICS) + " |")
        print("|---" * (len(METRICS) + 1) + "|")
        for variant in ORDER:
            if variant not in runs:
                continue
            row = [cell([metric(r, phase) for r in runs[variant]]) for _, metric in METRICS]
            print(f"| {variant} | " + " | ".join(row) + " |")
    _ = reps


if __name__ == "__main__":
    main()
