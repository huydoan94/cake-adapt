#!/usr/bin/env python3
"""Compare timestamp-request cadence at two equivalent testbed ingress points."""
import argparse
import gzip
import json
from pathlib import Path
import re
import statistics
import struct


def read(path):
    if path.exists():
        return path.read_bytes()
    with gzip.open(str(path) + '.gz', 'rb') as stream:
        return stream.read()


def pcap(path):
    raw = read(path)
    formats = {b'\xd4\xc3\xb2\xa1': ('<', 1), b'\xa1\xb2\xc3\xd4': ('>', 1),
               b'\x4d\x3c\xb2\xa1': ('<', 1000), b'\xa1\xb2\x3c\x4d': ('>', 1000)}
    assert len(raw) >= 24 and raw[:4] in formats, 'invalid PCAP header'
    endian, fraction_scale = formats[raw[:4]]
    major, minor, _, _, snaplen, link = struct.unpack(endian + 'HHIIII', raw[4:24])
    assert (major, minor) == (2, 4) and link == 1 and snaplen >= 54, 'unsupported PCAP framing'
    offset = 24
    packets = []
    while offset < len(raw):
        assert offset + 16 <= len(raw), 'truncated record header'
        seconds, fraction, captured, original = struct.unpack(endian + 'IIII', raw[offset:offset+16])
        offset += 16
        assert 0 < captured == original <= snaplen and offset + captured <= len(raw), 'truncated packet'
        assert fraction < 1000000 * fraction_scale, 'invalid timestamp fraction'
        packet = raw[offset:offset+captured]
        offset += captured
        assert len(packet) >= 54 and packet[12:14] == b'\x08\x00', 'unexpected Ethernet protocol'
        ip = packet[14:]
        ihl = (ip[0] & 15) * 4
        assert ip[0] >> 4 == 4 and ihl >= 20 and ip[9] == 1, 'unexpected IP protocol'
        total = struct.unpack('!H', ip[2:4])[0]
        assert ihl + 20 <= total <= len(ip), 'incomplete ICMP timestamp'
        assert struct.unpack('!H', ip[6:8])[0] & 0x3fff == 0, 'fragmented request'
        icmp = ip[ihl:total]
        assert icmp[0:2] == b'\x0d\x00', 'not a timestamp request'
        source = tuple(ip[12:16])
        target = tuple(ip[16:20])
        assert target[:3] == (10, 99, 0) and 11 <= target[3] <= 16, 'unexpected reflector'
        identifier, sequence, originate = struct.unpack('!HHI', icmp[4:12])
        assert originate < 86400000, 'invalid UTC originate'
        timestamp_us = seconds * 1000000 + fraction // fraction_scale
        packets.append({'time_us': timestamp_us, 'target': target,
                        'key': (source, target, identifier, sequence, originate),
                        'originate_ms': originate})
    assert all(b['time_us'] >= a['time_us'] for a, b in zip(packets, packets[1:])), 'capture clock/order'
    assert len({p['key'] for p in packets}) == len(packets), 'duplicate request key'
    return packets


def distribution(values, allow_empty=False):
    values = sorted(values)
    if not values:
        assert allow_empty, 'empty metric'
        return {'samples': 0, 'p50_ms': None, 'p95_ms': None, 'p99_ms': None, 'max_ms': None}
    return {'samples': len(values), 'p50_ms': statistics.median(values),
            **{f'p{p}_ms': values[min(len(values)-1, int(p / 100 * len(values)))] for p in (95, 99)},
            'max_ms': values[-1]}


def dropped(path, packets):
    log = read(path).decode()
    match = re.search(r'^(\d+) packets dropped by kernel$', log, re.M)
    assert match and int(match[1]) == 0, 'missing/nonzero capture drop counter'
    count = re.search(r'^(\d+) packets captured$', log, re.M)
    assert count and int(count[1]) == len(packets), 'PCAP/capture count mismatch'
    return log


def cpu_before_after(before, after):
    def fields(path):
        pid, rest = read(path).decode().split(' (', 1)
        name, columns = rest.rsplit(') ', 1)
        values = columns.split()
        assert name == 'tcpdump', 'wrong capture process'
        return int(pid), int(values[19]), int(values[11]) + int(values[12])
    a, b = fields(before), fields(after)
    assert a[:2] == b[:2] and b[2] >= a[2], 'capture process changed'
    return b[2] - a[2]


def analyze(root, method):
    case, capture = root / method, root / ('capture-' + method)
    marks = [(round(float(t) * 1e6), label) for t, label in
             (line.split() for line in read(case / 'phases-epoch').decode().splitlines())]
    assert [label for _, label in marks] == ['start', 'upload-steady', 'idle', 'upload-step',
                                           'capacity-low', 'capacity-back', 'end'], 'incomplete upload case'
    assert all(b[0] > a[0] for a, b in zip(marks, marks[1:])), 'phase clock stepped'
    assert read(case / 'log-inode-before') == read(case / 'log-inode-after'), 'log replaced'
    first, peer = pcap(capture / 'isp-ingress.pcap'), pcap(capture / 'reflector-ingress.pcap')
    dropped(capture / 'isp-stderr', first); dropped(capture / 'reflector-stderr', peer)
    peers = {p['key']: p for p in peer}
    samples = []
    header = None
    for line in read(case / 'cake-adapt.log').decode().splitlines():
        fields = [value.strip() for value in line.split(';')]
        if fields[0] == 'DATA_HEADER':
            header = {name: index for index, name in enumerate(fields)}
        elif fields[0] == 'DATA':
            assert header is not None, 'missing DATA header'
            samples.append((float(fields[header['LOG_TIMESTAMP']]) * 1e6,
                            float(fields[header['ICMP_TIMESTAMP']].strip('[]')) * 1e6))
    phases = {}
    for index, (begin, phase) in enumerate(marks):
        if phase not in ('start', 'upload-steady', 'upload-step'):
            continue
        end = next(t for t, label in marks[index+1:] if label in ('upload-steady', 'idle', 'end'))
        selected = [p for p in first if begin <= p['time_us'] < end]
        assert len(selected) >= (250 if phase == 'start' else 1000), 'few capture packets'
        assert {p['target'][3] for p in selected} == set(range(11, 17)), 'missing reflector'
        matched = [(p, peers[p['key']]) for p in selected if p['key'] in peers]
        assert all(q['time_us'] >= p['time_us'] for p, q in matched), 'cross-capture clock/order'
        target_gaps = []
        previous = {}
        for p in selected:
            if p['target'] in previous:
                target_gaps.append((p['time_us'] - previous[p['target']]) / 1000)
            previous[p['target']] = p['time_us']
        # These testbed capture times and originate values share the VM UTC clock.
        def since_originate(packet):
            delta = packet['time_us'] % 86400000000 - packet['originate_ms'] * 1000
            if delta < -43200000000:
                delta += 86400000000
            assert 0 <= delta < 10000000, 'inconsistent originate/capture clocks'
            return delta / 1000
        entry_gaps = [(b['time_us'] - a['time_us']) / 1000 for a, b in zip(selected, selected[1:])]
        data = [(processing - arrival) / 1000 for processing, arrival in samples if begin <= processing < end]
        phases[phase] = {
            'controller_samples': len(data),
            'printed_or_kernel_arrival_to_data_delay': distribution(data, allow_empty=True),
            'first_packets': len(selected), 'matched_packets': len(matched),
            'unmatched_packets': len(selected) - len(matched),
            'global_isp_ingress_gap': distribution(entry_gaps),
            'per_target_isp_ingress_gap': distribution(target_gaps),
            'global_gap_over_55_ms_pct': 100 * sum(v > 55 for v in entry_gaps) / len(entry_gaps),
            'global_gap_over_60_ms_pct': 100 * sum(v > 60 for v in entry_gaps) / len(entry_gaps),
            'originate_to_isp_ingress': distribution([since_originate(p) for p in selected]),
            'originate_to_reflector_ingress': distribution([since_originate(q) for _, q in matched], allow_empty=True),
            'isp_to_reflector_ingress': distribution([(q['time_us'] - p['time_us']) / 1000 for p, q in matched], allow_empty=True),
        }
    seconds = float(read(capture / 'cpu-after.time')) - float(read(capture / 'cpu-before.time'))
    ticks = sum(cpu_before_after(capture / (name + '-cpu-before'), capture / (name + '-cpu-after'))
                for name in ('isp', 'reflector'))
    assert seconds > 0
    probe = []
    for line in read(case / 'probe').decode().splitlines():
        match = re.match(r'^\[(\d+\.\d+)\].*(timed out|\d+ bytes, [\d.]+ ms)', line)
        if match:
            probe.append((float(match[1]) * 1e6, 'timed out' in match[2]))
    for phase, filename in (('upload-steady', 'upload-steady.json'), ('upload-step', 'upload-step.json')):
        result = json.loads(read(case / filename))
        assert 'error' not in result, 'iperf failure'
        rate = result['end']['sum_received']['bits_per_second'] / 1e6
        assert rate > 0
        phases[phase]['ul_goodput_mbit_s'] = rate
        index = next(i for i, (_, label) in enumerate(marks) if label == phase)
        begin, end = marks[index][0], next(t for t, label in marks[index+1:] if label in ('idle', 'end'))
        samples = [lost for t, lost in probe if begin <= t < end]
        assert len(samples) >= 300, 'few independent probes'
        phases[phase]['probe_samples'], phases[phase]['probe_lost'] = len(samples), sum(samples)
    return {'phases': phases, 'capture_cpu': {'ticks': ticks, 'seconds': seconds,
                                             'one_core_pct': ticks / seconds}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    assert read(args.root / 'boot-before') == read(args.root / 'boot-after'), 'boot changed'
    print(json.dumps({'cases': {name: analyze(args.root, name) for name in ('fping-ts', 'native-ts')},
                      'acceptance': 'diagnostic only; migration gate unchanged'}, indent=2))
