#!/usr/bin/env python3
"""Score the revised phase-3 gate: 3 alternating rounds per pair.

Usage: gate.py RESULTS [--pairs ts|echo|both] [--output FILE]

RESULTS holds r1-ts, r1-echo, ... r3-ts, r3-echo, each with the two cases of
its pair as written by epoch-batch.sh. Every case is scored by the earlier
comparison's analyze() (profiling/2026-10-13-native-icmp/analyze.py).

A metric passes when native's median over the rounds is better than fping's,
or worse by no more than fping's own range (max - min) over the rounds.
Combined daemon + pinger CPU must be lower (medians), and every round's
reflector-loss and recovery detection must meet analyze.py's allowances
against the fping case of the same round. Exit 1 when the gate fails.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / '2026-10-13-native-icmp'))
from analyze import HEALTH, PHASES, analyze  # noqa: E402

LOWER = ('added_rtt_p50_ms', 'added_rtt_p95_ms', 'over15_pct', 'over30_pct')
HIGHER = ('ul_received_mbit_s', 'dl_received_mbit_s')
INFO = ('added_rtt_p99_ms', 'lost')
PAIRS = (('ts', 'fping-ts', 'native-ts'), ('echo', 'fping', 'native'))


def score(root, kinds=('ts', 'echo')):
    report = {}
    for kind, ref, native in (p for p in PAIRS if p[0] in kinds):
        rounds = []
        for number in (1, 2, 3):
            directory = root / f'r{number}-{kind}'
            rounds.append({ref: analyze(directory / ref), native: analyze(directory / native)})
        metrics, failures = {}, []
        for phase in PHASES:
            for metric in LOWER + HIGHER + INFO:
                if metric not in rounds[0][ref]['phases'][phase]:
                    continue
                a = [r[ref]['phases'][phase][metric] for r in rounds]
                b = [r[native]['phases'][phase][metric] for r in rounds]
                spread = max(a) - min(a)
                delta = statistics.median(b) - statistics.median(a)
                if metric in LOWER:
                    passed = delta <= spread
                elif metric in HIGHER:
                    passed = -delta <= spread
                else:
                    passed = None
                metrics[f'{phase}: {metric}'] = {
                    ref: a, native: b, 'median_delta': round(delta, 4),
                    'reference_range': round(spread, 4), 'passed': passed}
                if passed is False:
                    failures.append(f'{phase}: {metric}')
        cpu_a = [r[ref]['cpu']['one_core_pct'] for r in rounds]
        cpu_b = [r[native]['cpu']['one_core_pct'] for r in rounds]
        cpu_lower = statistics.median(cpu_b) < statistics.median(cpu_a)
        slow = []
        for number, r in enumerate(rounds, 1):
            observed = r[ref]['health_observed'] and r[native]['health_observed']
            if not observed:
                slow.append(f'round {number}: health not observed')
                continue
            slow += [f'round {number}: {name}' for name, tolerance in HEALTH.items()
                     if r[native]['health'][name] > r[ref]['health'][name] + tolerance]
        report[native] = {
            'reference': ref, 'metrics': metrics, 'failed_metrics': failures,
            'cpu_one_core_pct': {ref: cpu_a, native: cpu_b}, 'cpu_lower': cpu_lower,
            'health': {n: {ref: [r[ref]['health'][n] for r in rounds],
                           native: [r[native]['health'][n] for r in rounds]} for n in HEALTH},
            'slower_health': slow,
            'passed': not failures and cpu_lower and not slow}
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--pairs', choices=('ts', 'echo', 'both'), default='both')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = score(args.root, ('ts', 'echo') if args.pairs == 'both' else (args.pairs,))
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    for native, entry in result.items():
        print(f"{native}: {'PASS' if entry['passed'] else 'FAIL'}; cpu lower {entry['cpu_lower']}; "
              f"failed metrics {entry['failed_metrics'] or 'none'}; slower health {entry['slower_health'] or 'none'}")
    sys.exit(0 if all(entry['passed'] for entry in result.values()) else 1)
