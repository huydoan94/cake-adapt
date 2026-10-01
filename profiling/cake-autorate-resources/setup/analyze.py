#!/usr/bin/env python3
"""Summarize the cake-autorate / cake-adapt resource comparison.

setup/analyze.py raw

Each run directory (VARIANT-CONTROLLER-rROUND) holds:
  snaps   phase uptime cg_usage cg_user cg_system busy idle ctxt forks dl_bytes ul_bytes dl_rate ul_rate
  memory  uptime processes rss_kb private_kb   (every 5 s; private is 0 in round 1)
  logs    for the latency of DATA records (stats variant)
"""
import glob
import gzip
import os
import statistics
import sys

USER_HZ = 100
PHASES = ["startup", "idle", "download", "upload", "bidirectional", "sleep"]
LABELS = {
    "startup": "Startup (first 15 s)",
    "idle": "Idle, pinging (40 s)",
    "download": "Download (45 s)",
    "upload": "Upload (45 s)",
    "bidirectional": "Bidirectional (45 s)",
    "sleep": "Idle sleep (30 s)",
}
root = sys.argv[1]


def load_run(path):
    snaps = {}
    for line in open(os.path.join(path, "snaps")):
        f = line.split()
        snaps[f[0]] = {
            "t": float(f[1]),
            "cg": int(f[2]), "cg_user": int(f[3]), "cg_sys": int(f[4]),
            "busy": int(f[5]), "idle": int(f[6]), "ctxt": int(f[7]), "forks": int(f[8]),
            "dl_bytes": int(f[9]), "ul_bytes": int(f[10]),
            "dl_rate": f[11], "ul_rate": f[12],
        }
    order = ["start", "startup", "idle", "download", "upload", "bidirectional", "settle", "sleep"]
    phases = {}
    for previous, current in zip(order, order[1:]):
        a, b = snaps[previous], snaps[current]
        seconds = b["t"] - a["t"]
        phases[current] = {
            "seconds": seconds,
            "cg_pct": (b["cg"] - a["cg"]) / 1e4 / seconds,  # % of one CPU
            "cg_user_pct": (b["cg_user"] - a["cg_user"]) / 1e4 / seconds,
            "cg_sys_pct": (b["cg_sys"] - a["cg_sys"]) / 1e4 / seconds,
            "busy_pct": (b["busy"] - a["busy"]) * 100 / USER_HZ / seconds,
            "ctxt_s": (b["ctxt"] - a["ctxt"]) / seconds,
            "forks_s": (b["forks"] - a["forks"]) / seconds,
            "dl_mbit": (b["dl_bytes"] - a["dl_bytes"]) * 8 / 1e6 / seconds,
            "ul_mbit": (b["ul_bytes"] - a["ul_bytes"]) * 8 / 1e6 / seconds,
            "start": a["t"], "end": b["t"],
        }
    memory = []
    for line in open(os.path.join(path, "memory")):
        f = line.split()
        if len(f) == 4 and int(f[1]) > 0:
            memory.append((float(f[0]), int(f[1]), int(f[2]), int(f[3])))
    for name, phase in phases.items():
        inside = [m for m in memory if phase["start"] <= m[0] < phase["end"]]
        phase["procs"] = max((m[1] for m in inside), default=0)
        phase["rss_kb"] = statistics.mean(m[2] for m in inside) if inside else 0
        private = [m[3] for m in inside if m[3] > 0]
        phase["pss_kb"] = statistics.mean(private) if private else float("nan")
    phases["_peak_pss_kb"] = max((m[3] for m in memory), default=0)
    phases["_peak_rss_kb"] = max((m[2] for m in memory), default=0)
    phases["_peak_procs"] = max((m[1] for m in memory), default=0)
    return phases


def runs(variant, controller):
    return [load_run(p) for p in sorted(glob.glob(f"{root}/{variant}-{controller}-r*"))]


def mean(values):
    values = [v for v in values if v == v]  # drop NaN (no private memory in round 1)
    return statistics.mean(values) if values else float("nan")


def spread(values):
    return f"{min(values):.2f}–{max(values):.2f}" if len(values) > 1 else ""


def log_lines(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", errors="replace") as f:
        yield from f


def data_latency(run_dir):
    """Milliseconds from the pinger's reply timestamp to the DATA record."""
    latencies = []
    files = sorted(
        glob.glob(f"{run_dir}/*.log*"),
        key=lambda p: (not p.endswith(".old"), p)
    )
    for path in files:
        if path.endswith("stdout"):
            continue
        for line in log_lines(path):
            if not line.startswith("DATA;"):
                continue
            f = [x.strip() for x in line.split(";")]
            try:
                proc_us = float(f[3]) * (1e6 if "." in f[3] else 1)
                stamp = f[8].strip("[]")
                reply_us = float(stamp) * 1e6
            except (ValueError, IndexError):
                continue
            latencies.append((proc_us - reply_us) / 1000)
    return latencies


def percentile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]


report = []
out = report.append
for variant in ("defaults", "stats"):
    data = {c: runs(variant, c) for c in ("none", "autorate", "adapt")}
    if not data["autorate"] or not data["adapt"]:
        continue
    out(f"\n=== {variant}: rounds autorate={len(data['autorate'])} adapt={len(data['adapt'])} none={len(data['none'])}")
    out("phase | cgroup CPU % (autorate / adapt) | ratio | system busy % above baseline (autorate / adapt) | forks/s (autorate / adapt / none) | ctxt/s (autorate / adapt / none) | CAKE Mbit/s dl+ul (autorate / adapt)")
    for phase in PHASES:
        a = [r[phase]["cg_pct"] for r in data["autorate"]]
        b = [r[phase]["cg_pct"] for r in data["adapt"]]
        base = mean([r[phase]["busy_pct"] for r in data["none"]]) if data["none"] else float("nan")
        sa = mean([r[phase]["busy_pct"] for r in data["autorate"]]) - base
        sb = mean([r[phase]["busy_pct"] for r in data["adapt"]]) - base
        fa = mean([r[phase]["forks_s"] for r in data["autorate"]])
        fb = mean([r[phase]["forks_s"] for r in data["adapt"]])
        fn = mean([r[phase]["forks_s"] for r in data["none"]]) if data["none"] else float("nan")
        ca = mean([r[phase]["ctxt_s"] for r in data["autorate"]])
        cb = mean([r[phase]["ctxt_s"] for r in data["adapt"]])
        cn = mean([r[phase]["ctxt_s"] for r in data["none"]]) if data["none"] else float("nan")
        ta = mean([r[phase]["dl_mbit"] + r[phase]["ul_mbit"] for r in data["autorate"]])
        tb = mean([r[phase]["dl_mbit"] + r[phase]["ul_mbit"] for r in data["adapt"]])
        out(
            f"{LABELS[phase]} | {mean(a):.2f} [{spread(a)}] / {mean(b):.2f} [{spread(b)}] | "
            f"{mean(a) / mean(b):.1f}x | {sa:.2f} / {sb:.2f} | {fa:.2f} / {fb:.2f} / {fn:.2f} | "
            f"{ca:.0f} / {cb:.0f} / {cn:.0f} | {ta:.1f} / {tb:.1f}"
        )
    out("user/system split of cgroup CPU (autorate user, sys / adapt user, sys):")
    for phase in PHASES:
        out(
            f"  {LABELS[phase]}: "
            f"{mean([r[phase]['cg_user_pct'] for r in data['autorate']]):.2f}, {mean([r[phase]['cg_sys_pct'] for r in data['autorate']]):.2f} / "
            f"{mean([r[phase]['cg_user_pct'] for r in data['adapt']]):.2f}, {mean([r[phase]['cg_sys_pct'] for r in data['adapt']]):.2f}"
        )
    out("memory (mean private kB per phase from statm, max processes) autorate / adapt:")
    for phase in PHASES:
        out(
            f"  {LABELS[phase]}: {mean([r[phase]['pss_kb'] for r in data['autorate']]):.0f} kB, {max(r[phase]['procs'] for r in data['autorate'])} procs / "
            f"{mean([r[phase]['pss_kb'] for r in data['adapt']]):.0f} kB, {max(r[phase]['procs'] for r in data['adapt'])} procs"
        )
    out(
        f"  peak private: {max(r['_peak_pss_kb'] for r in data['autorate'])} / {max(r['_peak_pss_kb'] for r in data['adapt'])} kB; "
        f"peak RSS: {max(r['_peak_rss_kb'] for r in data['autorate'])} / {max(r['_peak_rss_kb'] for r in data['adapt'])} kB; "
        f"peak processes: {max(r['_peak_procs'] for r in data['autorate'])} / {max(r['_peak_procs'] for r in data['adapt'])}"
    )
    awake = ["idle", "download", "upload", "bidirectional"]
    weights = [40, 45, 45, 45]
    ta = sum(mean([r[p]["cg_pct"] for r in data["autorate"]]) * w for p, w in zip(awake, weights)) / sum(weights)
    tb = sum(mean([r[p]["cg_pct"] for r in data["adapt"]]) * w for p, w in zip(awake, weights)) / sum(weights)
    out(f"time-weighted awake cgroup CPU: autorate {ta:.2f}% / adapt {tb:.2f}% ({ta / tb:.1f}x)")
    if variant == "stats":
        for controller in ("autorate", "adapt"):
            for run_dir in sorted(glob.glob(f"{root}/{variant}-{controller}-r*")):
                lat = data_latency(run_dir)
                if lat:
                    out(
                        f"reply->DATA latency {controller}: n={len(lat)} median={statistics.median(lat):.2f} ms "
                        f"p90={percentile(lat, 0.9):.2f} p99={percentile(lat, 0.99):.2f} max={max(lat):.2f}"
                    )
out("\n=== per-process CPU (ticks of 10 ms; children reaped by a process are included in it)")
for controller in ("autorate", "adapt"):
    path = f"{root}/breakdown-{controller}/procs"
    if not os.path.exists(path):
        continue
    ticks = {}
    for line in open(path):
        phase, pid, _ppid, comm, value = line.split()
        ticks[(phase, pid, comm)] = int(value)
    for previous, current, seconds in (("start", "idle", 40), ("idle", "bidirectional", 45)):
        fping = controller_ticks = 0
        for (phase, pid, comm), value in ticks.items():
            if phase != current:
                continue
            delta = value - ticks.get((previous, pid, comm), 0)
            if comm == "fping":
                fping += delta
            else:
                controller_ticks += delta
        out(
            f"{controller} {current} ({seconds} s): fping {fping} ticks ({fping / seconds:.2f}% CPU), "
            f"controller {controller_ticks} ticks ({controller_ticks / seconds:.2f}% CPU)"
        )
print("\n".join(report))
