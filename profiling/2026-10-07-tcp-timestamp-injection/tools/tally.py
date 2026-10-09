#!/usr/bin/env python3
"""Tally one watcher log of the timestamp-injection experiment.

usage: tally.py ROUND.log [fping-download-delta.txt]

ROUND.log holds `tcpdump -v` lines for SYNs and SYN-ACKs (and in round 3
RSTs) prefixed WAN (eth1) or LAN (br-lan), and cake-adapt TCP_QUEUE records
prefixed EST. Prints the handshake counts, Windows RSTs by whether the
SYN-ACK Windows received carried a timestamp, the estimator's validity, and
both directions' estimates per 10 s beside fping's RTT/2 delta when its DATA
extract is given (fping cannot split directions: its RTT/2 is the same both
ways).
"""
import collections
import re
import sys
import time

MARKER = "3394153729"  # TSINJECT_MARKER, 0xca4ead01
WINDOWS = "192.168.56.1."

PACKET = re.compile(
    r"^(WAN|LAN)\s+(\S+) > (\S+): Flags \[(\S+)\], cksum \S+ \((\w+)\)(?:.*?options \[([^\]]*)\])?"
)


def utc(seconds):
    return time.strftime("%H:%M:%S", time.gmtime(seconds))


def handshakes(lines):
    # An RST carries no options.
    packets = [m.groups()[:5] + (m.group(6) or "",) for m in map(PACKET.match, lines) if m]
    print(f"packets {len(packets)}, bad checksums {sum(p[4] != 'correct' for p in packets)}")
    injected = {}
    native = windows = 0
    for side, src, dst, flags, _, options in packets:
        if side == "WAN" and flags == "S":
            if f"TS val {MARKER}" in options:
                injected[(src, dst)] = None
            elif "TS val" in options:
                native += 1
        if side == "LAN" and flags == "S" and src.startswith(WINDOWS) and "TS val" not in options:
            windows += 1
    for side, src, dst, flags, _, options in packets:
        if side == "WAN" and flags == "S." and injected.get((dst, src), 0) is None:
            injected[(dst, src)] = "ts" if f"ecr {MARKER}" in options else "no"
    answers = collections.Counter(injected.values())
    to_windows = [p for p in packets if p[0] == "LAN" and p[3] == "S." and p[2].startswith(WINDOWS)]
    print(f"Windows SYNs without TS (LAN): {windows}; injected (WAN): {len(injected)}; "
          f"SYNs with their own TS: {native}")
    print(f"servers answering with TS {answers['ts']}, without {answers['no']}, "
          f"no SYN-ACK seen {answers[None]}")
    print(f"SYN-ACKs to Windows {len(to_windows)}, still carrying the marker "
          f"{sum(f'ecr {MARKER}' in p[5] for p in to_windows)}")
    print("declined by", sorted({d.rsplit('.', 1)[0] for (_, d), v in injected.items() if v == "no"}))
    resets = collections.Counter()
    seen = collections.Counter()
    # NAT keeps the client port, so a WAN connection is the LAN one with the same
    # client port and server.
    def port(address):
        return address.rsplit(".", 1)[1]

    for (client, server), answer in injected.items():
        if answer is None:
            continue
        synack = [p for p in to_windows if p[1] == server and port(p[2]) == port(client)]
        kind = "with TS" if synack and "TS val" in synack[0][5] else "without TS"
        seen[kind] += 1
        if any(p[0] == "LAN" and p[1].startswith(WINDOWS) and port(p[1]) == port(client) and
               p[2] == server and p[3].startswith("R") for p in packets):
            resets[kind] += 1
    for kind in ("with TS", "without TS"):
        print(f"Windows connections whose SYN-ACK arrived {kind}: {seen[kind]}, "
              f"ended by a Windows RST: {resets[kind]}")


def estimates(lines, fping_path):
    records = [l.split("; ") for l in lines if l.startswith("EST TCP_QUEUE;")]
    valid = [r for r in records if r[4] == "1"]
    print(f"TCP_QUEUE records {len(records)}, download valid {len(valid)}, "
          f"upload valid {sum(r[6] == '1' for r in records)}")
    tcp = collections.defaultdict(list)
    upload = collections.defaultdict(list)
    for r in valid:
        tcp[int(float(r[2])) // 10 * 10].append(int(r[5]))
    for r in records:
        if r[6] == "1":
            upload[int(float(r[2])) // 10 * 10].append(int(r[7]))
    fping = collections.defaultdict(list)
    if fping_path:
        for line in open(fping_path):
            if not line.startswith("#"):
                fields = line.split()
                fping[int(float(fields[0])) // 10 * 10].append(int(fields[1]))
    print("10 s from (UTC)  download n  mean ms  max ms | upload n  mean ms  max ms "
          "| fping n  mean ms  max ms")
    for start in sorted(tcp):
        t, u, f = tcp[start], upload.get(start, []), fping.get(start, [])
        line = f"{utc(start)}        {len(t):10d} {sum(t) / len(t) / 1000:8.1f} {max(t) / 1000:7.1f}"
        if u:
            line += f" | {len(u):8d} {sum(u) / len(u) / 1000:8.1f} {max(u) / 1000:7.1f}"
        else:
            line += " |        0        -       -"
        if f:
            line += f" | {len(f):7d} {sum(f) / len(f) / 1000:8.1f} {max(f) / 1000:7.1f}"
        print(line)


lines = open(sys.argv[1]).read().splitlines()
handshakes(lines)
estimates(lines, sys.argv[2] if len(sys.argv) > 2 else None)
