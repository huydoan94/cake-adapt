#!/usr/bin/env python3
"""Per phase of an acks.sh run: upload bytes by kind (pure ACK, other TCP,
UDP and ICMP), ICMP and UDP added latency and loss, iperf3 throughput, and the
largest ISP backlog seen (which should stay near zero).

acks.py RESULT_DIR"""
import gzip
import json
import os
import re
import statistics
import struct
import sys

root = sys.argv[1]


def open_any(name, mode="r"):
    """Reads a result file, or its gzipped copy as stored under profiling/."""
    path = f"{root}/{name}"
    if os.path.exists(path + ".gz"):
        return gzip.open(path + ".gz", mode + ("t" if mode == "r" else ""))
    return open(path, mode)


marks = [(float(t), n) for t, n in (l.split() for l in open(f"{root}/phases"))]
phases = []
for i, (t, n) in enumerate(marks):
    if n not in ("idle", "end"):
        end = next(m[0] for m in marks[i + 1:] if m[1] in ("idle", "end"))
        phases.append((n, t + 2, end - 1))
base_idle = (marks[0][0], marks[1][0])


def pct(values, f):
    values = sorted(values)
    return values[min(len(values) - 1, int(f * len(values)))] if values else float("nan")


# Outgoing packets on cwan, after the upload CAKE: (time, wire bytes, kind).
packets = []
data = open_any("out.pcap", "rb").read()
offset = 24
while offset + 16 <= len(data):
    seconds, micros, captured, original = struct.unpack_from("<IIII", data, offset)
    frame = data[offset + 16: offset + 16 + captured]
    offset += 16 + captured
    t = seconds + micros / 1e6
    kind = "other"
    if len(frame) >= 34 and frame[12:14] == b"\x08\x00":
        ip = frame[14:]
        ihl = (ip[0] & 15) * 4
        total = struct.unpack_from(">H", ip, 2)[0]
        protocol = ip[9]
        if protocol == 6 and len(ip) >= ihl + 14:
            doff = (ip[ihl + 12] >> 4) * 4
            flags = ip[ihl + 13]
            payload = total - ihl - doff
            kind = "ack" if payload == 0 and flags & 0x17 == 0x10 else "tcp"
        elif protocol == 17:
            kind = "udp"
        elif protocol == 1:
            kind = "icmp"
    packets.append((t, original, kind))

# The daemon's download shaper, from LOAD records when it ran (kbit/s).
shaper = []
if os.path.exists(f"{root}/cake-adapt.log") or os.path.exists(f"{root}/cake-adapt.log.gz"):
    for line in open_any("cake-adapt.log"):
        f = [x.strip() for x in line.split(";")]
        if f[0] == "LOAD" and len(f) >= 8:
            shaper.append((float(f[2]), float(f[6])))

icmp = []
for line in open_any("icmp"):
    m = re.match(r"^\[(\d+\.\d+)\] \S+\s+: \[\d+\], \d+ bytes, ([\d.]+) ms", line)
    if m:
        icmp.append((float(m.group(1)), float(m.group(2))))
    elif "timed out" in line:
        icmp.append((float(line[1:line.index("]")]), None))


def udp(name, interval=0.02):
    # Sent at a fixed interval from 2 s before the first mark.
    start = marks[0][0] - 2.0
    received = {}
    sent = 0
    for line in open_any(name):
        if line.startswith("# sent"):
            sent = int(line.split()[2])
        else:
            seq, rtt = line.split()
            received[int(seq)] = int(rtt) / 1000
    return [(start + s * interval, received.get(s)) for s in range(sent)]


udp64, udp200 = udp("udp64"), udp("udp200")
backlog = []
for line in open_any("backlog"):
    f = line.split()
    if len(f) == 5:
        size = lambda x: float(x[:-1]) * 1024 if x.endswith("K") else float(x)
        backlog.append((float(f[0]), size(f[2]) * 8 / 1e6 * 1000, size(f[4]) * 8 / 30e6 * 1000))


def latency(samples, start, end):
    window = [r for t, r in samples if start <= t < end]
    got = [r for r in window if r is not None]
    lost = 100 * (len(window) - len(got)) / len(window) if window else float("nan")
    return pct(got, 0.5), pct(got, 0.99), lost


idle_icmp = statistics.median(r for t, r in icmp if base_idle[0] <= t < base_idle[1] and r)
idle_u64 = statistics.median(r for t, r in udp64 if base_idle[0] <= t < base_idle[1] and r)
print(f"idle RTT: ICMP {idle_icmp:.1f} ms, UDP {idle_u64:.1f} ms; latency below is added RTT (p50/p99 ms) and loss")
print(f"{'phase':8} {'dl Mbit':>7} {'ul Mbit':>7} | {'out kbit':>8} {'ack kbit':>8} {'ack %':>6} {'tcp %':>6} {'udp+icmp %':>10} | "
      f"{'ICMP':>15} | {'UDP 64k':>15} | {'UDP 200k':>15} | {'ISP q up/dn ms':>14} | {'dl shaper':>9}")
for name, start, end in phases:
    window = [p for p in packets if start <= p[0] < end]
    seconds = end - start
    total = sum(p[1] for p in window) * 8 / seconds / 1000
    by = lambda k: sum(p[1] for p in window if p[2] == k) * 8 / seconds / 1000
    ack, tcp, small = by("ack"), by("tcp"), by("udp") + by("icmp")

    def through(kind):
        path = f"{root}/{name}-{kind}.json"
        try:
            return json.load(open(path))["end"]["sum_received"]["bits_per_second"] / 1e6
        except (OSError, KeyError, ValueError):
            return float("nan")

    def fmt(samples, base):
        p50, p99, lost = latency(samples, start, end)
        return f"{p50 - base:5.1f}/{p99 - base:5.1f} {lost:3.0f}%"

    q = [b for b in backlog if start <= b[0] < end]
    qmax = f"{max(b[1] for b in q):5.1f}/{max(b[2] for b in q):5.1f}" if q else "-"
    print(f"{name:8} {through('download'):7.1f} {through('upload'):7.2f} | {total:8.0f} {ack:8.0f} "
          f"{100 * ack / total:6.1f} {100 * tcp / total:6.1f} {100 * small / total:10.1f} | "
          f"{fmt(icmp, idle_icmp):>15} | {fmt(udp64, idle_u64):>15} | {fmt(udp200, idle_u64):>15} | {qmax:>14} | "
          f"{(statistics.mean(r for t, r in shaper if start <= t < end) / 1000 if any(start <= t < end for t, _ in shaper) else float('nan')):9.1f}")
