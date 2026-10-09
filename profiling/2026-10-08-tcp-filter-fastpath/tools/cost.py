import json, os, re, sys
from collections import defaultdict
root = sys.argv[1]
def progs(path):
    out = {}
    text = open(path).read()
    for m in re.finditer(r'^(\d+): \S+\s+name (\S+).*?run_time_ns (\d+) run_cnt (\d+)', text, re.M):
        out[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))
    return out
groups = defaultdict(list)
for step in sorted(os.listdir(root)):
    d = os.path.join(root, step)
    if not os.path.isdir(d): continue
    b, a = progs(f'{d}/before'), progs(f'{d}/after')
    row = {}
    for key, (t, c) in a.items():
        if key[1] not in ('tcpdelay', 'inject_egress'): continue
        t0, c0 = b.get(key, (0, 0))
        if c - c0 > 0: row[key[1]] = ((t - t0), (c - c0))
    mbps = []
    for f in ('download.json', 'upload.json'):
        try: mbps.append(json.load(open(f'{d}/{f}'))['end']['sum_received']['bits_per_second'] / 1e6)
        except Exception: mbps.append(float('nan'))
    print(f"{step:22} " + "  ".join(f"{n}={row[n][0]/row[n][1]:7.0f}ns/{row[n][1]}" for n in sorted(row)) + f"  down={mbps[0]:.1f} up={mbps[1]:.1f} Mbit/s")
    case = step.rsplit('-', 2)
    groups[(case[0], case[1])].append(row)
print()
for (case, variant), rows in sorted(groups.items()):
    for n in ('tcpdelay', 'inject_egress'):
        t = sum(r[n][0] for r in rows if n in r); c = sum(r[n][1] for r in rows if n in r)
        if c: print(f"{case:8} {variant:5} {n:14} {t/c:7.0f} ns/run over {c} runs")
