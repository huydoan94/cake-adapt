#!/usr/bin/env python3
"""Describe saved reply cadence and rates; this is not an acceptance scorer."""
import argparse
import gzip
import json
import statistics


def describe(values):
    values = sorted(values)
    assert values, 'empty metric'
    return {'median': statistics.median(values),
            'p95': values[min(len(values) - 1, int(.95 * len(values)))]}


def diagnose(case):
    marks = [(float(t), name) for t, name in
             (line.split() for line in (case / 'phases-epoch').read_text().splitlines())]
    rows = []
    with gzip.open(case / 'cake-adapt.log.gz', 'rt') as stream:
        for line in stream:
            fields = [value.strip() for value in line.split(';')]
            if fields[0] == 'DATA':
                assert len(fields) == 33, 'unexpected DATA schema'
                rows.append((float(fields[2]), float(fields[8].strip('[]')),
                             fields[9], int(fields[31]), int(fields[32])))
    result = {}
    for index, (begin, phase) in enumerate(marks):
        if phase not in ('start', 'upload-steady', 'upload-step',
                         'download-steady', 'bidirectional'):
            continue
        end = next(t for t, name in marks[index + 1:]
                   if name in ('upload-steady', 'idle', 'end'))
        selected = [row for row in rows if begin <= row[0] < end]
        previous = {}
        gaps = []
        for _, arrival, target, _, _ in selected:
            if target in previous:
                gaps.append((arrival - previous[target]) * 1000)
            previous[target] = arrival
        assert len(previous) == 6, 'incomplete reflector set'
        result[phase] = {
            'samples': len(selected),
            'per_target_reply_gap_ms': describe(gaps),
            'arrival_to_data_ms': describe([(row[0] - row[1]) * 1000 for row in selected]),
            'sample_weighted_median_dl_kbit_s': statistics.median(row[3] for row in selected),
            'sample_weighted_median_ul_kbit_s': statistics.median(row[4] for row in selected),
        }
    return result


if __name__ == '__main__':
    from pathlib import Path

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    print(json.dumps({name: diagnose(args.root / name)
                      for name in ('fping-ts', 'native-ts')}, indent=2))
