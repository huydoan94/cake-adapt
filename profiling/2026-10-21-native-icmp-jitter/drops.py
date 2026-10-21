#!/usr/bin/env python3
"""Per-drop view of the jittered upload-step phase.

Usage: drops.py RESULTS

RESULTS holds r1-ts, r2-ts, r3-ts with fping-ts and native-ts cases. For
every capacity drop it prints the upload shaper rate at the drop (the last
DATA record before it), the independent probe's RTT p95 in the 3 s after it,
and the seconds to the first bufferbloat sample and to the shaper reaching
4000 kbit/s or less. It ends with each method's median over all drops and,
over all drops of both methods, the correlation between the rate at the drop
and the 3-s p95.
"""
import gzip
import re
import statistics
import sys
from pathlib import Path

PROBE = re.compile(r'^\[(\d+\.\d+)\] \S+\s+: \[\d+\], (?:\d+ bytes, ([\d.]+) ms|timed out)')


def text(path):
    if path.exists():
        return path.read_text()
    with gzip.open(str(path) + '.gz', 'rt') as stream:
        return stream.read()


def p95(values):
    values = sorted(values)
    return values[min(len(values) - 1, int(0.95 * len(values)))]


def drops(run):
    marks = [(float(t), n) for t, n in (l.split() for l in text(run / 'phases-epoch').splitlines())]
    lows = [t for t, n in marks if n == 'capacity-low']
    data = []
    for line in text(run / 'cake-adapt.log').splitlines():
        f = [v.strip() for v in line.split(';')]
        if f[0] == 'DATA' and len(f) >= 33:
            data.append((float(f[2]), f[30], int(f[32])))
    probe = []
    for line in text(run / 'probe').splitlines():
        m = PROBE.match(line)
        if m and m[2]:
            probe.append((float(m[1]), float(m[2])))
    rows = []
    for t0 in lows:
        rate = next(r for t, c, r in reversed(data) if t < t0)
        bb = next((t - t0 for t, c, r in data if t >= t0 and c.endswith('_bb')), None)
        low = next((t - t0 for t, c, r in data if t >= t0 and r <= 4000), None)
        rows.append(dict(rate=rate, p95=p95([r for t, r in probe if t0 <= t < t0 + 3]), bb=bb, low=low))
    return rows


root = Path(sys.argv[1])
every = {'fping-ts': [], 'native-ts': []}
print(f"{'case':20}{'drop':>5}{'rate kbit/s':>13}{'3-s p95 ms':>12}{'first bb s':>12}{'<=4000 s':>10}")
for r in ('r1-ts', 'r2-ts', 'r3-ts'):
    for method in every:
        for number, row in enumerate(drops(root / r / method), 1):
            every[method].append(row)
            print(f"{r + ' ' + method:20}{number:5}{row['rate']:13}{row['p95']:12.1f}"
                  f"{row['bb'] if row['bb'] is not None else float('nan'):12.2f}"
                  f"{row['low'] if row['low'] is not None else float('nan'):10.2f}")
for method, rows in every.items():
    print(f"{method}: {len(rows)} drops, median rate {statistics.median(r['rate'] for r in rows):.0f}, "
          f"median 3-s p95 {statistics.median(r['p95'] for r in rows):.1f} ms, "
          f"median first bb {statistics.median(r['bb'] for r in rows if r['bb'] is not None):.2f} s, "
          f"median to <=4000 {statistics.median(r['low'] for r in rows if r['low'] is not None):.2f} s")
both = every['fping-ts'] + every['native-ts']
print(f"correlation rate at drop vs 3-s p95 over {len(both)} drops: "
      f"{statistics.correlation([r['rate'] for r in both], [r['p95'] for r in both]):.2f}")
