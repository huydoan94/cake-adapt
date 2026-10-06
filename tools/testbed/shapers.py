#!/usr/bin/env python3
"""Per-phase CAKE shaper and achieved rates (kbit/s) from cake-adapt's LOAD and
SHAPER records, for runs made with output_load_stats and output_cake_changes.

shapers.py RESULTS_DIR

Log times map to phase marks through the first log record, which comes within
about a second of the start mark; averages skip each phase's first 5 s.
ul>cap% is the share of LOAD samples with the upload shaper above the upload
bottleneck; cuts count shaper decreases within the phase.
"""
import glob
import gzip
import os
import statistics
import sys
root = sys.argv[1]
PH = ("upload-steady", "upload-step", "download-steady", "bidirectional")
print(f"{'variant':14} {'phase':16} {'ul cake':>8} {'ul ach':>7} {'ul>cap%':>8} {'dl cake':>8} {'dl ach':>7} {'dl cuts':>7} {'ul cuts':>7}")
for run in sorted(os.path.dirname(path) for path in glob.glob(f"{root}/*/phases")):
    marks = [(float(t), n) for t, n in (l.split() for l in open(f"{run}/phases"))]
    loads, shapers, first = [], [], None
    log = f"{run}/cake-adapt.log"
    for line in (gzip.open(log + ".gz", "rt") if os.path.exists(log + ".gz") else open(log)):
        f = [x.strip() for x in line.split(";")]
        if len(f) < 3:
            continue
        try:
            ts = float(f[2])
        except ValueError:
            continue
        if first is None:
            first = ts
        if f[0] == "LOAD":
            loads.append((ts, float(f[4]), float(f[5]), float(f[6]), float(f[7])))
        if f[0] == "SHAPER":
            shapers.append((ts, f[3]))
    off = first - marks[0][0]
    cap_ul = float(open(f"{run}/capacity").read().split()[0]) * 1000
    for i, (t, name) in enumerate(marks):
        if name not in PH:
            continue
        end = next(m[0] for m in marks[i + 1:] if m[1] in ("idle", "end"))
        rows = [r for r in loads if t + off + 5 <= r[0] < end + off]
        cuts = {"ifb4cwan": 0, "cwan": 0}
        last = {}
        for ts, cmd in shapers:
            dev = cmd.split()[5]; rate = int(cmd.split()[-1].replace("Kbit", ""))
            if t + off <= ts < end + off and dev in last and rate < last[dev]:
                cuts[dev] += 1
            last[dev] = rate
        if not rows:
            continue
        m = lambda k: statistics.mean(r[k] for r in rows)
        over = 100 * sum(r[4] > cap_ul for r in rows) / len(rows)
        print(f"{os.path.basename(run):14} {name:16} {m(4):8.0f} {m(2):7.0f} {over:8.1f} {m(3):8.0f} {m(1):7.0f} {cuts['ifb4cwan']:7d} {cuts['cwan']:7d}")
