#!/usr/bin/env python3
import json
import sys
from pathlib import Path
s=json.loads(Path(sys.argv[1]).read_text())
print('| Backend | Daemon ticks | Daemon + pinger ticks | CPU (% of one core) |')
print('| --- | ---: | ---: | ---: |')
for name,c in s['cases'].items():
    cpu=c['cpu']; print(f"| {name} | {cpu['daemon_own_ticks']} | {cpu['daemon_plus_pinger_ticks']} | {cpu['one_core_pct']:.3f} |")
print('\n| Backend | Phase | Added RTT p95 (ms) | Received probes >30 ms (%) | UL goodput (Mbit/s) | DL goodput (Mbit/s) |')
print('| --- | --- | ---: | ---: | ---: | ---: |')
for name,c in s['cases'].items():
    for phase,p in c['phases'].items():
        ul=f"{p['ul_received_mbit_s']:.2f}" if 'ul_received_mbit_s' in p else '—'
        dl=f"{p['dl_received_mbit_s']:.2f}" if 'dl_received_mbit_s' in p else '—'
        print(f"| {name} | {phase} | {p['added_rtt_p95_ms']:.2f} | {p['over30_pct']:.2f} | {ul} | {dl} |")
print('\n| Backend | One-target detection (s) | All-target stall check (s) | Global timeout (s) | Reply after recovery (s) |')
print('| --- | ---: | ---: | ---: | ---: |')
for name,c in s['cases'].items():
    values=[f'{v:.3f}' if v is not None else 'missing' for v in c['health'].values()]
    print('| '+name+' | '+' | '.join(values)+' |')
print('\n| Native method | CPU lower | Health matched | Strict gate passes |')
print('| --- | --- | --- | --- |')
for name,c in s['comparisons'].items():
    print(f"| {name} | {c['cpu_lower']} | {c['health_matched']} | {c['passed']} |")
