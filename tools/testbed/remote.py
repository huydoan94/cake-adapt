#!/usr/bin/env python3
"""Score remote.sh runs: does congestion beyond the ISP mislead the daemon?

remote.py RESULTS_DIR...

Per run and phase (skipping each phase's first 5 s): the local probe's added
RTT (p50/p95/p99 above the run's idle minimum), the true queue at the ISP and
at the remote bottleneck (p50, ms), the mean shaper rates and the number of
shaper cuts per direction, the share of DATA samples flagged as
bufferbloat per direction (DATA load conditions), and each iperf3 stream's
throughput. Shaper rates come from the same DATA records.
"""
import gzip
import json
import os
import re
import sys

PROBE = re.compile(r"^\[(\d+\.\d+)\] \S+\s+: \[\d+\], (?:\d+ bytes, ([\d.]+) ms|timed out)")
UNITS = {"bit": 1, "Kbit": 1e3, "Mbit": 1e6, "Gbit": 1e9}
PHASES = ("remote-upload", "remote-download", "local-dl-remote-ul", "local-ul-remote-dl",
          "local-bidirectional")
SKIP_S = 5


def rate_bits(text):
    for unit in sorted(UNITS, key=len, reverse=True):
        if text.endswith(unit):
            return float(text[: -len(unit)]) * UNITS[unit]
    raise ValueError(text)


def size_bytes(text):
    for suffix, scale in (("M", 1024 * 1024), ("K", 1024)):
        if text.endswith(suffix):
            return float(text[:-1]) * scale
    return float(text)


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int(fraction * len(values)))] if values else float("nan")


def open_text(path):
    return gzip.open(path + ".gz", "rt") if os.path.exists(path + ".gz") else open(path)


def mbit(path):
    try:
        return json.load(open(path))["end"]["sum_received"]["bits_per_second"] / 1e6
    except (OSError, KeyError, ValueError):
        return float("nan")


def run_summary(run):
    marks = [(float(t), n) for t, n in (line.split() for line in open(f"{run}/phases-epoch"))]
    windows = {}
    for index, (start, name) in enumerate(marks[:-1]):
        if name in PHASES:
            windows[name] = (start + SKIP_S, marks[index + 1][0])
    # The probe prints its own epoch timestamps.
    probe = []
    for line in open(f"{run}/probe"):
        match = PROBE.match(line)
        if match and match.group(2):
            probe.append((float(match.group(1)), float(match.group(2))))
    base = min(rtt for _, rtt in probe)
    queues = []
    for line in open_text(f"{run}/backlog"):
        fields = line.split()
        if len(fields) != 9:
            continue
        t = float(fields[0])
        pairs = [(fields[i], fields[i + 1]) for i in (1, 3, 5, 7)]
        queues.append((t, [size_bytes(b) * 8 / rate_bits(r) * 1000 for r, b in pairs]))
    shapers, summaries = [], []
    for line in open_text(f"{run}/cake-adapt.log"):
        fields = [field.strip() for field in line.split(";")]
        if fields[0] == "SHAPER" and len(fields) >= 4:
            words = fields[3].split()
            shapers.append((float(fields[2]), words[5], int(words[-1].rstrip("Kbit"))))
        elif fields[0] == "DATA" and len(fields) >= 33:
            # DL_LOAD_CONDITION, UL_LOAD_CONDITION, CAKE_DL_RATE_KBPS, CAKE_UL_RATE_KBPS.
            summaries.append((float(fields[2]), fields[29], fields[30], int(fields[31]), int(fields[32])))
    rows = {}
    for name, (start, end) in windows.items():
        inside = lambda items: [item for item in items if start <= item[0] < end]
        added = [rtt - base for _, rtt in inside(probe)]
        q = [values for _, values in inside(queues)]
        s = inside(summaries)
        cuts = {"cwan": 0, "ifb4cwan": 0}
        last = {}
        for t, device, rate in shapers:
            if device in last and start <= t < end and rate < last[device]:
                cuts[device] += 1
            last[device] = rate
        rows[name] = {
            "rtt": [percentile(added, f) for f in (0.5, 0.95, 0.99)],
            "queues": [percentile([v[i] for v in q], 0.5) for i in range(4)],
            "dl_rate": sum(x[3] for x in s) / len(s) / 1000 if s else float("nan"),
            "ul_rate": sum(x[4] for x in s) / len(s) / 1000 if s else float("nan"),
            "dl_bb": 100 * sum("bb" in x[1] for x in s) / len(s) if s else float("nan"),
            "ul_bb": 100 * sum("bb" in x[2] for x in s) / len(s) if s else float("nan"),
            "dl_cuts": cuts["ifb4cwan"],
            "ul_cuts": cuts["cwan"],
        }
    streams = {
        "remote-upload": ("remote-upload.json", None),
        "remote-download": (None, "remote-download.json"),
    }
    for name in PHASES[2:]:
        streams[name] = (f"{name}-upload.json", f"{name}-download.json")
    for name, (up, down) in streams.items():
        if name in rows:
            rows[name]["up_mbit"] = mbit(f"{run}/{up}") if up else float("nan")
            rows[name]["down_mbit"] = mbit(f"{run}/{down}") if down else float("nan")
    return rows


print("added RTT in ms above the idle minimum; true queue p50 in ms at the ISP (up/down) and the")
print("remote bottleneck (up/down); mean shaper Mbit/s and cuts; % of samples flagged bufferbloat;")
print("throughput Mbit/s per stream")
print()
print(f"{'run':14} {'phase':20} {'rtt p50/p95/p99':>17} | {'isp q up/dn':>11} {'remote up/dn':>12} | "
      f"{'shaper ul/dl':>12} {'cuts ul/dl':>10} {'bb% ul/dl':>10} | {'Mbit up/dn':>11}")
for run in sys.argv[1:]:
    for name, row in run_summary(run).items():
        rtt = "/".join(f"{v:.0f}" for v in row["rtt"])
        q = row["queues"]
        print(f"{os.path.basename(run.rstrip('/')):14} {name:20} {rtt:>17} | "
              f"{q[0]:5.0f}/{q[1]:<5.0f} {q[2]:6.0f}/{q[3]:<5.0f} | "
              f"{row['ul_rate']:5.1f}/{row['dl_rate']:<6.1f} {row['ul_cuts']:4d}/{row['dl_cuts']:<5d} "
              f"{row['ul_bb']:4.0f}/{row['dl_bb']:<5.0f} | {row['up_mbit']:5.1f}/{row['down_mbit']:<5.1f}")
    print()
