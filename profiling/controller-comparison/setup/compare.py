#!/usr/bin/env python3
"""Compare cake-autorate and cake-adapt logs from the same two-minute workload."""
import re, statistics, sys

DIR = sys.argv[1]
RUNS = ("autorate", "adapt")

def phases(run):
    marks = []
    for line in open(f"{DIR}/{run}.phases"):
        ts, _, name = line.split()
        marks.append((float(ts), name))
    return marks

def phase_of(marks, t):
    name = "pre"
    for ts, n in marks:
        if t >= ts:
            name = n
    return name

def records(run, kind):
    header = None
    rows = []
    for line in open(f"{DIR}/{run}.log"):
        fields = [f.strip() for f in line.rstrip("\n").split(";")]
        if fields[0] == f"{kind}_HEADER":
            header = fields
        elif fields[0] == kind and header:
            rows.append(dict(zip(header, fields)))
    return rows

def shaper_steps(run, marks):
    rows = []
    for line in open(f"{DIR}/{run}.log"):
        if line.startswith("SHAPER;"):
            fields = [f.strip() for f in line.split(";")]
            m = re.search(r"dev (\S+) cake bandwidth (\d+)Kbit", fields[3])
            rows.append((float(fields[2]), m.group(1), int(m.group(2))))
    return rows

out = {}
for run in RUNS:
    marks = phases(run)
    start = marks[0][0]
    data = records(run, "DATA")
    load = records(run, "LOAD")
    steps = shaper_steps(run, marks)
    r = {"marks": marks}
    by_phase = {}
    for row in data:
        t = float(row["LOG_TIMESTAMP"])
        p = phase_of(marks, t)
        d = by_phase.setdefault(p, {"n": 0, "dl_bb": 0, "ul_bb": 0, "dl_cond": {}, "ul_cond": {}, "dl_rate": [], "ul_rate": [], "rtt_delta": []})
        d["n"] += 1
        for k, c in (("dl_cond", row["DL_LOAD_CONDITION"]), ("ul_cond", row["UL_LOAD_CONDITION"])):
            d[k][c] = d[k].get(c, 0) + 1
        d["dl_bb"] += row["DL_LOAD_CONDITION"].endswith("_bb")
        d["ul_bb"] += row["UL_LOAD_CONDITION"].endswith("_bb")
        d["dl_rate"].append(int(row["CAKE_DL_RATE_KBPS"]))
        d["ul_rate"].append(int(row["CAKE_UL_RATE_KBPS"]))
        d["rtt_delta"].append(int(row["DL_OWD_DELTA_US"]))
    r["phases"] = by_phase
    thresholds = {(row["DL_ADJ_DELAY_THR"], row["DL_ADJ_MAX_ADJUST_UP_THR_US"], row["DL_ADJ_MAX_ADJUST_DOWN_THR_US"],
                   row["UL_ADJ_DELAY_THR"], row["UL_ADJ_MAX_ADJUST_UP_THR_US"], row["UL_ADJ_MAX_ADJUST_DOWN_THR_US"]) for row in data}
    r["thresholds"] = thresholds
    # Recompute each record's load percent the way cake-autorate does.
    bad_load = sum(
        1 for row in data
        if int(row["CAKE_DL_RATE_KBPS"]) and int(row["DL_LOAD_PERCENT"]) != 100 * int(row["DL_ACHIEVED_RATE_KBPS"]) // int(row["CAKE_DL_RATE_KBPS"])
    )
    r["load_formula_mismatch"] = bad_load
    ratios = {"ifb4eth1": [], "eth1": []}
    last = {"ifb4eth1": 20000, "eth1": 20000}
    for t, dev, rate in steps:
        ratios[dev].append((phase_of(marks, t), rate / last[dev] if last[dev] else 0, rate))
        last[dev] = rate
    r["steps"] = ratios
    r["samples_per_second"] = len(data) / (marks[-1][0] - marks[0][0] + 2)
    out[run] = r

for run in RUNS:
    r = out[run]
    print(f"===== {run}: {r['samples_per_second']:.1f} DATA/s, load-formula mismatches={r['load_formula_mismatch']}")
    print("  thresholds:", sorted(r["thresholds"]))
    for p in ("pre", "idle", "download", "upload", "end"):
        pass
    order = []
    for ts, n in r["marks"]:
        order.append(n)
    seen = set()
    for p in ["pre"] + order:
        if p in seen or p not in r["phases"]:
            continue
        seen.add(p)
    for p, d in r["phases"].items():
        print(f"  [{p:8}] n={d['n']:4} dl_rate min/max/last={min(d['dl_rate'])}/{max(d['dl_rate'])}/{d['dl_rate'][-1]}"
              f" ul_rate min/max/last={min(d['ul_rate'])}/{max(d['ul_rate'])}/{d['ul_rate'][-1]}"
              f" bb dl/ul={d['dl_bb']}/{d['ul_bb']} median_delta={statistics.median(d['rtt_delta'])}")
        print(f"             dl_cond={dict(sorted(d['dl_cond'].items()))} ul_cond={dict(sorted(d['ul_cond'].items()))}")
    for dev in ("ifb4eth1", "eth1"):
        kinds = {}
        for p, ratio, rate in r["steps"][dev]:
            key = "up" if ratio > 1 else "down"
            kinds.setdefault((p, key), []).append(round(ratio, 4))
        print(f"  {dev} rate changes:")
        for (p, key), vals in sorted(kinds.items()):
            common = {}
            for v in vals:
                common[v] = common.get(v, 0) + 1
            top = sorted(common.items(), key=lambda kv: -kv[1])[:4]
            print(f"    {p:8} {key:4} n={len(vals):3} most common ratios={top}")
