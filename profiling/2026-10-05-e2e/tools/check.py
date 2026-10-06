#!/usr/bin/env python3
"""check.py RESULT: per-phase table and expected-event checks for one e2e.sh run.

The daemon log is the SIGUSR1 export (start to export) followed by the active
log (after the SIGUSR2 reset). Rates come from the 1 s samples, latency from
the independent 100 ms probe as RTT above its idle 5th percentile.
"""
import gzip
import json
import re
import statistics
import sys
from pathlib import Path

root = Path(sys.argv[1])


def text(name):
    """A result file, or its gzipped copy as committed."""
    path = root / name
    if path.exists():
        return path.read_text()
    return gzip.open(f"{path}.gz", "rt").read()


phases = []
for line in (root / "phases").read_text().splitlines():
    stamp, _, name = line.partition(" ")
    if stamp.isdigit():
        phases.append((int(stamp), name.split()[0]))
log = ""
for export in sorted((root / "logs").glob("cake-adapt_*.log.gz")):
    log += gzip.open(export, "rt").read()
log += text("cake-adapt.log")

samples = []
for line in (root / "samples").read_text().splitlines():
    fields = dict(f.split("=", 1) for f in line.split()[1:] if "=" in f)
    fields["t"] = int(line.split()[0])
    samples.append(fields)

probe = []
for line in text("probe").splitlines():
    m = re.match(r"\[(\d+)\.\d+\] .* ([\d.]+) ms", line)
    if m:
        probe.append((int(m.group(1)), float(m.group(2))))
base = sorted(r for t, r in probe if phases[0][0] <= t < phases[1][0])
floor = base[len(base) // 20] if base else 0.0


def kbit(text):
    m = re.match(r"([\d.]+)([KMG]?)bit", text or "")
    if not m:
        return None
    return float(m.group(1)) * {"": 1e-3, "K": 1, "M": 1e3, "G": 1e6}[m.group(2)]


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))] if values else float("nan")


print("| Phase | s | UL shaper kbit/s | DL shaper kbit/s | probe added p50/p95 ms | CPU ticks | RSS kB | fds |")
print("|---|---:|---|---|---|---:|---:|---:|")
for (start, name), (end, _) in zip(phases, phases[1:]):
    window = [s for s in samples if start <= s["t"] < end]
    if not window or name in ("end",):
        continue
    ul = [kbit(s.get("ul")) for s in window if kbit(s.get("ul"))]
    dl = [kbit(s.get("dl")) for s in window if kbit(s.get("dl"))]
    rtt = [r - floor for t, r in probe if start <= t < end]
    ticks = [int(s["ticks"]) for s in window if s.get("ticks", "").isdigit()]
    rss = [int(s["rss"]) for s in window if s.get("rss", "").isdigit()]
    fds = [int(s["fds"]) for s in window if s.get("fds", "").isdigit()]
    span = lambda v: f"{min(v):.0f}-{max(v):.0f}" if v else "-"
    print(f"| {name} | {end - start} | {span(ul)} | {span(dl)} | "
          f"{percentile(rtt, .5):.1f} / {percentile(rtt, .95):.1f} | "
          f"{(ticks[-1] - ticks[0]) if len(ticks) > 1 else '-'} | {span(rss)} | {span(fds)} |")

for path in sorted(root.glob("*.json")):
    try:
        end = json.loads(path.read_text())["end"]
        rate = end.get("sum_received", end.get("sum", {})).get("bits_per_second", 0) / 1e6
        print(f"{path.stem}: {rate:.2f} Mbit/s")
    except (ValueError, KeyError):
        print(f"{path.stem}: unreadable")

reset = re.search(r"log-reset size=(\d+)\n.*log-reset-done size=(\d+) inode=(\d+)", (root / "phases").read_text())
inode = (root / "inode-before").read_text().split()[0]
reset_ok = bool(reset) and int(reset.group(2)) < int(reset.group(1)) and reset.group(3) == inode

external = next(t for t, n in phases if n == "upload-bandwidth-external")
reconciled = any(
    external <= float(l.split("; ")[2]) <= external + 5 and "dev cwan" in l
    for l in log.splitlines() if l.startswith("SHAPER;")
)

checks = {
    "CAKE discovered on both": len(re.findall(r"CAKE discovered: interface=(cwan|ifb4cwan)", log)) >= 2,
    "TCP measurement started": "TCP measurement started: interface=cwan" in log,
    "TCP_QUEUE records": log.count("\nTCP_QUEUE;") > 100,
    "upload shaper raised under load": re.search(r"SHAPER;[^\n]*dev cwan cake bandwidth 3000Kbit", log) is not None,
    "upload cut on congestion": re.search(r"congestion changed: direction=upload state=detected", log) is not None,
    "ACK counters stayed valid": "ACK ceiling disabled" not in log,
    "log export": "received log file export signal" in log,
    # The reset removes its own announcement, so check the sizes the script recorded.
    "log reset in place": reset_ok,
    "upload CAKE removed and recovered": "CAKE removed: direction=upload" in log and "CAKE observation recovered: interface=cwan" in log,
    "download CAKE removed and recovered": "CAKE removed: direction=download" in log and "CAKE observation recovered: interface=ifb4cwan" in log,
    "external bandwidth reconciled within 5 s": reconciled,
    "IFB recreated and rediscovered": "no CAKE qdisc found on interface=ifb4cwan" in log,
    "dead reflector replaced": "replacing reflector: 10.99.0.11" in log,
    "stall entered": "to: STALL" in log,
    "global timeout": "global ping response timeout" in log,
    "pingers restarted": "Restarting pingers." in log,
    "idle entered": "to: IDLE" in log,
    "minimum enforced on idle": "Enforcing minimum shaper rates." in log,
    "woken by load": "Connection load exceeded active threshold" in log,
    "clean shutdown": "received signal 15; shutting down" in log,
    "no errors": "ERROR;" not in log,
}
for name, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'} {name}")
warnings = sorted(set(re.sub(r"[\d.:-]+; ", "", l) for l in log.splitlines() if l.startswith("WARNING;")))
print("WARNING kinds:", *warnings, sep="\n  ")
