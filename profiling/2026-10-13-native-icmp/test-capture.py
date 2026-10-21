#!/usr/bin/env python3
"""Exercise PCAP formats and reject corrupted or ambiguous timestamp captures."""
import importlib.util
from pathlib import Path
import struct
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('capture', Path(__file__).with_name('capture.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def frame():
    ethernet = b'\x00' * 12 + b'\x08\x00'
    ip = bytearray(20)
    ip[0], ip[9] = 0x45, 1
    ip[2:4] = struct.pack('!H', 40)
    ip[12:16], ip[16:20] = bytes((10, 77, 1, 2)), bytes((10, 99, 0, 11))
    icmp = bytearray(20)
    icmp[0] = 13
    icmp[4:12] = struct.pack('!HHI', 55, 7, 12345)
    return ethernet + ip + icmp


def fixture(endian='<', nano=False, packet=None):
    packet = frame() if packet is None else packet
    magic = {('<', False): b'\xd4\xc3\xb2\xa1', ('>', False): b'\xa1\xb2\xc3\xd4',
             ('<', True): b'\x4d\x3c\xb2\xa1', ('>', True): b'\xa1\xb2\x3c\x4d'}[endian, nano]
    return (magic + struct.pack(endian+'HHIIII', 2, 4, 0, 0, 96, 1) +
            struct.pack(endian+'IIII', 1791930000, 123000 if not nano else 123000000,
                        len(packet), len(packet)) + packet)


with tempfile.TemporaryDirectory(prefix='timestamp-pcap-test-') as directory:
    path = Path(directory) / 'capture.pcap'
    for endian in ('<', '>'):
        for nano in (False, True):
            path.write_bytes(fixture(endian, nano))
            rows = module.pcap(path)
            assert rows[0]['time_us'] == 1791930000123000
            assert rows[0]['key'][2:] == (55, 7, 12345)
    original = fixture()
    bad_protocol = bytearray(frame()); bad_protocol[34] = 8
    fragmented = bytearray(frame()); fragmented[20:22] = struct.pack('!H', 0x2000)
    duplicate = original + original[24:]
    for bad in (original[:15], original[:-1], original + b'x',
                fixture(packet=bad_protocol), fixture(packet=fragmented), duplicate):
        path.write_bytes(bad)
        try:
            module.pcap(path)
        except AssertionError:
            pass
        else:
            raise AssertionError('corrupt or ambiguous capture accepted')
    path.write_text('1 packets captured\n1 packets dropped by kernel\n')
    try:
        module.dropped(path, [1])
    except AssertionError:
        pass
    else:
        raise AssertionError('capture loss accepted')

print('PCAP tests pass: both byte orders/precisions, tuples, truncation, protocol, fragmentation, duplicates, drops')

if len(sys.argv) > 1:
    with tempfile.TemporaryDirectory(prefix='timestamp-case-test-') as directory:
        root = Path(directory)
        case = root / 'native-ts'
        shutil.copytree(Path(sys.argv[1]), case)
        marks = [(float(t), label) for t, label in
                 (line.split() for line in (case / 'phases-epoch').read_text().splitlines())]
        marks = marks[:7]
        marks[-1] = (marks[-1][0], 'end')
        (case / 'phases-epoch').write_text(''.join(f'{t:.6f} {label}\n' for t, label in marks))
        capture = root / 'capture-native-ts'
        capture.mkdir()
        frames = []
        for serial, timestamp in enumerate(range(round(marks[0][0]*1e6), round(marks[-1][0]*1e6), 50000)):
            packet = bytearray(frame())
            packet[33] = 11 + serial % 6
            packet[40:46] = struct.pack('!HI', serial, (timestamp-1000)//1000 % 86400000)
            frames.append((timestamp, bytes(packet)))
        def write_packets(name, delay):
            raw = fixture()[:24]
            for timestamp, packet in frames:
                seconds, fraction = divmod(timestamp+delay, 1000000)
                raw += struct.pack('<IIII', seconds, fraction, len(packet), len(packet)) + packet
            (capture / name).write_bytes(raw)
        write_packets('isp-ingress.pcap', 0)
        write_packets('reflector-ingress.pcap', 20000)
        for name in ('isp', 'reflector'):
            (capture / (name+'-stderr')).write_text(f'{len(frames)} packets captured\n0 packets dropped by kernel\n')
            for boundary, ticks in (('before', 0), ('after', 2)):
                fields = ['0'] * 22
                fields[11], fields[19] = str(ticks), '100'
                (capture / (name+'-cpu-'+boundary)).write_text('1 (tcpdump) '+' '.join(fields)+'\n')
        (capture / 'cpu-before.time').write_text('0\n')
        (capture / 'cpu-after.time').write_text('150\n')
        result = module.analyze(root, 'native-ts')
        for phase in result['phases'].values():
            assert phase['unmatched_packets'] == 0
            assert phase['isp_to_reflector_ingress']['p95_ms'] == 20
            assert phase['per_target_isp_ingress_gap']['p50_ms'] == 300
        assert result['capture_cpu']['ticks'] == 4
        (capture / 'reflector-ingress.pcap').write_bytes(fixture()[:24])
        (capture / 'reflector-stderr').write_text('0 packets captured\n0 packets dropped by kernel\n')
        loss = module.analyze(root, 'native-ts')
        for phase in loss['phases'].values():
            assert phase['unmatched_packets'] == phase['first_packets']
            assert phase['matched_packets'] == 0
            assert phase['isp_to_reflector_ingress']['p95_ms'] is None
        print('100% unmatched peer coverage is retained explicitly with null delay metrics')
        print('complete synthetic case passes: matching, phases, 20-ms delay, 300-ms cadence, CPU units')
