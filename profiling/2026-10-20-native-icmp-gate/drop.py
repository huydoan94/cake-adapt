#!/usr/bin/env python3
"""Upload capacity drop: how fast each case detects it and cuts the shaper.

Usage: drop.py RESULTS (the unpacked raw.tar.xz). Prints, per timestamp case,
seconds after the drop to the first delayed sample, first bufferbloat, first
upload shaper cut and first shaper <= 4000 kbit/s; samples in the first
second; sample age (log time - ICMP timestamp, ms) in the step phase; and
probe RTT p95 in the first 3 s, the rest of the low phase, the step before
the drop and 20 s after recovery.
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
    return values[min(len(values) - 1, int(0.95 * len(values)))] if values else float('nan')


def case(run):
    marks = {n: float(t) for t, n in (l.split() for l in text(run / 'phases-epoch').splitlines())}
    t0, back, step = marks['capacity-low'], marks['capacity-back'], marks['upload-step']
    data, cuts = [], []
    for line in text(run / 'cake-adapt.log').splitlines():
        f = [v.strip() for v in line.split(';')]
        if f[0] == 'DATA' and len(f) >= 33:
            stamp = float(f[8].strip('[]'))
            data.append(dict(t=float(f[2]), sent=stamp, reflector=f[9], ul_owd=int(f[17]),
                             ul_delta=int(f[19]), ul_thr=int(f[20]), ul_sum=int(f[25]),
                             cond=f[30], ul_rate=int(f[32])))
        elif f[0] == 'SHAPER' and ' dev cwan ' in line:
            cuts.append((float(f[2]), int(re.search(r'bandwidth (\d+)Kbit', line)[1])))
    after = [d for d in data if t0 <= d['t'] < back]
    first_bb = next((d['t'] - t0 for d in after if d['cond'].endswith('_bb')), None)
    first_cut = next((t - t0 for t, r in cuts if t >= t0), None)
    first_low = next((d['t'] - t0 for d in after if d['ul_rate'] <= 4000), None)
    first_delayed = next((d['t'] - t0 for d in after if d['ul_delta'] > d['ul_thr']), None)
    window = [d for d in data if t0 <= d['t'] < t0 + 1.0]
    probe = []
    for line in text(run / 'probe').splitlines():
        m = PROBE.match(line)
        if m and m[2]:
            probe.append((float(m[1]), float(m[2])))
    early = [r for t, r in probe if t0 <= t < t0 + 3]
    late = [r for t, r in probe if t0 + 3 <= t < back]
    step_part = [r for t, r in probe if step <= t < t0]
    backp = [r for t, r in probe if back <= t < back + 20]
    steady = [d for d in data if step <= d['t'] < t0]
    age = [1000 * (d['t'] - d['sent']) for d in steady]
    return dict(first_delayed=first_delayed, first_bb=first_bb, first_cut=first_cut, first_low=first_low,
                samples_1s=len(window), age_med=statistics.median(age), age_p95=p95(age),
                rtt_p95_drop3s=p95(early), rtt_p95_low_rest=p95(late), rtt_p95_step_before=p95(step_part),
                rtt_p95_back=p95(backp))


root = Path(sys.argv[1])
keys = None
for r in ('r1-ts', 'r2-ts', 'r3-ts'):
    for m in ('fping-ts', 'native-ts'):
        c = case(root / r / m)
        if keys is None:
            keys = list(c)
            print('case'.ljust(20) + ''.join(k[:14].rjust(15) for k in keys))
        print(f'{r} {m}'.ljust(20) + ''.join(
            (f'{c[k]:15.3f}' if isinstance(c[k], float) else str(c[k]).rjust(15)) for k in keys))
