#!/usr/bin/env python3
"""Analyze diagnostic capture boundaries, without scoring migration acceptance."""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from analyze import analyze, text

FIELDS = {'serial', 'target', 'deadline_us', 'callback_us', 'realtime_us',
          'sent_us', 'returned_us', 'originate_ms', 'failed'}


def marker(line):
    match = re.search(r'NATIVE_SEND (.*)$', line)
    if not match:
        return None
    pairs = [word.split('=') for word in match[1].split()]
    assert all(len(pair) == 2 for pair in pairs), 'malformed marker'
    row = {key: int(value) for key, value in pairs}
    assert set(row) == FIELDS and len(pairs) == len(FIELDS), 'marker schema'
    assert row['returned_us'] >= row['sent_us'] >= row['callback_us'] >= row['deadline_us'], 'capture ordering'
    assert row['originate_ms'] == row['realtime_us'] // 1000 % 86400000, 'originate conversion'
    assert row['target'] == row['serial'] % 6, 'target order'
    return row


def describe(values):
    values = sorted(values)
    assert values, 'empty metric'
    return {'samples': len(values), 'p50_ms': statistics.median(values),
            **{f'p{p}_ms': values[min(len(values)-1, int(p / 100 * len(values)))] for p in (95, 99)},
            'max_ms': values[-1],
            **{f'over_{p}_ms_pct': 100 * sum(v > p for v in values) / len(values)
               for p in (1, 2.4, 7.3)}}


def diagnose(case):
    complete = analyze(case)
    log = text(case / 'cake-adapt.log')
    assert 'native receive timestamp unavailable' not in log, 'kernel timestamps missing'
    marks = [(float(t), name) for t, name in
             (line.split() for line in (case / 'phases-epoch').read_text().splitlines())]
    main_end = next(t for t, name in marks if name == 'end')
    start = marks[0][0]
    all_rows = [row for line in log.splitlines() if (row := marker(line)) is not None]
    assert all(all(b[key] > a[key] for key in ('callback_us', 'sent_us', 'returned_us', 'realtime_us'))
               for a, b in zip(all_rows, all_rows[1:])), 'capture clocks stepped/reordered'
    rows = [row for row in all_rows if start <= row['realtime_us'] / 1e6 < main_end]
    assert rows and all(not row['failed'] for row in rows), 'missing/failed sends'
    assert all(b['serial'] == a['serial'] + 1 and b['sent_us'] > a['sent_us']
               for a, b in zip(rows, rows[1:])), 'missing or reordered sends'
    arrivals = []
    peer_receives = {}
    for line in log.splitlines():
        fields = [field.strip() for field in line.split(';')]
        if fields[0] == 'DATA':
            assert len(fields) == 33, 'DATA schema'
            processing, arrival = float(fields[2]), float(fields[8].strip('[]'))
            assert processing >= arrival, 'negative receive-to-DATA delay'
            arrivals.append((processing, arrival))
            if start <= processing < main_end:
                assert fields[9].startswith('10.99.0.'), 'unexpected testbed reflector'
                serial = int(fields[10]) * 6 + int(fields[9].rsplit('.', 1)[1]) - 11
                assert serial not in peer_receives, 'duplicate main-phase sample'
                peer_receives[serial] = int(fields[17])
    phases = {}
    for index, (begin, phase) in enumerate(marks):
        if phase not in ('start', 'upload-steady', 'upload-step', 'download-steady', 'bidirectional'):
            continue
        end = next(t for t, name in marks[index + 1:] if name in ('upload-steady', 'idle', 'end'))
        selected = [row for row in rows if begin <= row['realtime_us'] / 1e6 < end]
        assert len(selected) >= (250 if phase == 'start' else 1000), 'few markers'
        assert {row['target'] for row in selected} == set(range(6)), 'incomplete targets'
        previous = {}
        gaps = []
        for row in selected:
            if row['target'] in previous:
                gaps.append((row['sent_us'] - previous[row['target']]) / 1000)
            previous[row['target']] = row['sent_us']
        long_returns = [row for row in selected
                        if row['returned_us'] - row['sent_us'] > 2400
                        and row['serial'] in peer_receives]
        # Only valid for this testbed: all reflectors share the VM realtime clock.
        # Realtime capture precedes pre-send, so this return epoch is a lower bound.
        # Peer receive is floored to milliseconds; add 1 ms for its upper bound.
        received_before_return = sum(
            row['realtime_us'] + row['returned_us'] - row['sent_us'] >
            row['realtime_us'] // 1000 * 1000 + peer_receives[row['serial']] + 1000
            for row in long_returns)
        phases[phase] = {
            'matched_pre_send_to_return_over_2_4_ms': len(long_returns),
            'peer_received_before_return_for_those': received_before_return,
            'callback_to_pre_send_age': describe([(r['sent_us'] - r['callback_us']) / 1000 for r in selected]),
            'callback_to_completed_send_upper_bound_age': describe([(r['returned_us'] - r['callback_us']) / 1000 for r in selected]),
            'pre_send_to_return_interval': describe([(r['returned_us'] - r['sent_us']) / 1000 for r in selected]),
            'originate_age_through_return_upper_bound': describe([(r['returned_us'] - r['callback_us'] + r['realtime_us'] % 1000) / 1000 for r in selected]),
            'send_deadline_lateness': describe([(r['sent_us'] - r['deadline_us']) / 1000 for r in selected]),
            'receive_to_data_delay': describe([(processing - arrival) * 1000
                                               for processing, arrival in arrivals
                                               if begin <= processing < end]),
            'global_pre_send_gap': describe([(b['sent_us'] - a['sent_us']) / 1000
                                            for a, b in zip(selected, selected[1:])]),
            'per_target_pre_send_gap': describe(gaps),
        }
    return {'complete_case': complete, 'send_diagnostic': phases,
            'acceptance': 'not an acceptance run; migration remains blocked'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    assert (args.root / 'boot-before').read_text() == (args.root / 'boot-after').read_text()
    print(json.dumps(diagnose(args.root / 'native-ts'), indent=2))
