#!/usr/bin/env python3
"""Estimate per-direction ISP queueing delay from TCP timestamps seen at the router.

estimate.py DIR   (DIR has cwan.pcap, backlog, phases from capture.sh)

Downstream: for packets arriving from the remote, arrival - TSval_remote * tick
changes only with the downstream one-way delay. Upstream: when an arriving packet
echoes (TSecr) a TSval we sent, TSval_remote * tick - our departure time of that
TSval changes only with the upstream one-way delay (plus delayed-ACK waits, which
only ever add). Each estimate is relative to the flow's minimum.
"""
import statistics
import struct
import sys
from collections import defaultdict

LOCAL = "10.98.0.2"
UP_RATE_BPS = 8e6
DOWN_RATE_BPS = 40e6
WINDOW = 0.1


def packets(path):
    with open(path, "rb") as f:
        magic = f.read(24)[:4]
        nano = magic in (b"\x4d\x3c\xb2\xa1",)
        while True:
            header = f.read(16)
            if len(header) < 16:
                return
            seconds, fraction, caplen, _ = struct.unpack("<IIII", header)
            data = f.read(caplen)
            t = seconds + fraction / (1e9 if nano else 1e6)
            if len(data) < 14 + 20 or data[12:14] != b"\x08\x00":
                continue
            ip = data[14:]
            ihl = (ip[0] & 15) * 4
            if ip[9] != 6:
                continue
            src = ".".join(str(b) for b in ip[12:16])
            dst = ".".join(str(b) for b in ip[16:20])
            tcp = ip[ihl:]
            if len(tcp) < 20:
                continue
            sport, dport = struct.unpack("!HH", tcp[:4])
            offset = (tcp[12] >> 4) * 4
            options = tcp[20:offset]
            index = 0
            tsval = tsecr = None
            while index < len(options):
                kind = options[index]
                if kind == 0:
                    break
                if kind == 1:
                    index += 1
                    continue
                if index + 1 >= len(options):
                    break
                length = options[index + 1]
                if kind == 8 and length == 10 and index + 10 <= len(options):
                    tsval, tsecr = struct.unpack("!II", options[index + 2:index + 10])
                if length < 2:
                    break
                index += length
            if tsval is None:
                continue
            outgoing = src == LOCAL
            flow = (sport, dport) if outgoing else (dport, sport)
            yield t, outgoing, flow, tsval, tsecr


def main(root):
    flows = defaultdict(lambda: {"departures": {}, "remote": []})
    for t, outgoing, flow, tsval, tsecr in packets(f"{root}/cwan.pcap"):
        state = flows[flow]
        if outgoing:
            state["departures"].setdefault(tsval, t)
        else:
            state["remote"].append((t, tsval, tsecr))

    down_samples = []
    up_samples = []
    for flow, state in flows.items():
        remote = state["remote"]
        if len(remote) < 100:
            continue
        # Remote timestamp clock: least-squares seconds per tick over the flow.
        ts = [r[1] for r in remote]
        times = [r[0] for r in remote]
        mean_ts = statistics.fmean(ts)
        mean_t = statistics.fmean(times)
        tick = sum((a - mean_ts) * (b - mean_t) for a, b in zip(ts, times)) / sum((a - mean_ts) ** 2 for a in ts)
        # A growing queue stretches arrival times and biases the slope, so snap
        # to the standard timestamp clock rates when close (1 ms on Linux/BSD).
        for standard in (0.001, 0.004, 0.01, 0.1):
            if abs(tick - standard) / standard < 0.05:
                tick = standard
                break
        down = [(t, t - v * tick) for t, v, _ in remote]
        floor = min(x for _, x in down)
        down_samples += [(t, (x - floor) * 1000) for t, x in down]
        up = [(t, v * tick - state["departures"][e]) for t, v, e in remote if e in state["departures"]]
        if up:
            floor = min(x for _, x in up)
            up_samples += [(t, (x - floor) * 1000) for t, x in up]
        print(f"flow {flow}: {len(remote)} remote packets, tick {tick * 1000:.3f} ms, {len(up)} upstream echoes")

    truth = []
    for line in open(f"{root}/backlog"):
        t, up_bytes, down_bytes = line.split()
        truth.append((float(t), int(up_bytes) * 8 / UP_RATE_BPS * 1000, int(down_bytes) * 8 / DOWN_RATE_BPS * 1000))
    marks = [(float(t), name) for t, name in (line.split() for line in open(f"{root}/phases"))]

    def windows(samples):
        # Delayed ACKs and processing only add delay, so each window keeps its minimum.
        buckets = defaultdict(list)
        for t, value in samples:
            buckets[int(t / WINDOW)].append(value)
        return {key: min(values) for key, values in buckets.items()}

    up_w = windows(up_samples)
    down_w = windows(down_samples)
    print(f"\n{'phase':14} {'n':>4} | {'true up':>8} {'est up':>8} {'err up':>8} | {'true dn':>8} {'est dn':>8} {'err dn':>8} | attribution correct")
    for index, (start, name) in enumerate(marks[:-1]):
        if name in ("idle", "end"):
            continue
        end = marks[index + 1][0]
        rows = []
        for t, true_up, true_down in truth:
            if start + 2 <= t < end:
                key = int(t / WINDOW)
                if key in up_w and key in down_w:
                    rows.append((true_up, up_w[key], true_down, down_w[key]))
        if not rows:
            print(f"{name:14} no overlapping samples")
            continue
        correct = sum((tu > 15) == (eu > 15) and (td > 15) == (ed > 15) for tu, eu, td, ed in rows)
        med = lambda i: statistics.median(r[i] for r in rows)
        err = lambda i, j: statistics.median(abs(r[i] - r[j]) for r in rows)
        print(f"{name:14} {len(rows):4d} | {med(0):8.1f} {med(1):8.1f} {err(0, 1):8.1f} | "
              f"{med(2):8.1f} {med(3):8.1f} {err(2, 3):8.1f} | {100 * correct / len(rows):5.1f}%")


main(sys.argv[1])
