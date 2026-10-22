#!/usr/bin/env python3
"""Compare two references without altering the native acceptance gate."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from analyze import analyze


def difference(a, b):
    return {'a': a, 'b': b, 'b_minus_a': b - a, 'absolute_difference': abs(b - a)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('saved_pair', type=Path)
    args = parser.parse_args()
    assert (args.root / 'boot-before').read_text() == (args.root / 'boot-after').read_text()
    cases = {name: analyze(args.root / name) for name in ('reference-a', 'reference-b')}
    saved = json.loads(args.saved_pair.read_text())['cases']
    phase_results = {}
    for phase, metrics in cases['reference-a']['phases'].items():
        phase_results[phase] = {}
        for metric, a in metrics.items():
            b = cases['reference-b']['phases'][phase][metric]
            result = difference(a, b)
            if metric != 'samples':
                native_gap = (saved['native-ts']['phases'][phase][metric] -
                              saved['fping-ts']['phases'][phase][metric])
                result.update(saved_native_minus_reference=native_gap,
                              reference_gap_at_least_saved_absolute_gap=abs(b - a) >= abs(native_gap))
            phase_results[phase][metric] = result
    cpu = {metric: difference(cases['reference-a']['cpu'][metric], cases['reference-b']['cpu'][metric])
           for metric in ('daemon_own_ticks', 'daemon_plus_pinger_ticks', 'seconds', 'one_core_pct')}
    assert all(case['health_observed'] for case in cases.values()), 'missing health observations'
    health = {metric: difference(cases['reference-a']['health'][metric], cases['reference-b']['health'][metric])
              for metric in cases['reference-a']['health']}
    print(json.dumps({'cases': cases, 'reference_differences': {'phases': phase_results,
                      'cpu': cpu, 'health': health},
                      'acceptance': 'unchanged; this diagnostic cannot pass migration'}, indent=2))
