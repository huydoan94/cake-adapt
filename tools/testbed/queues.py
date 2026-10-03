#!/usr/bin/env python3
"""Score the daemon's TCP_QUEUE estimates against the emulated ISP's real queues.

queues.py RUN_DIR

RUN_DIR is one run.sh result made with TCP_ATTRIBUTION=1, BACKLOG=1 and
output_processing_stats: it holds backlog, phases-epoch and cake-adapt.log
(or .gz). Each 100 ms backlog sample is converted to delay at the bottleneck's
rate at that moment and compared with the latest estimate before it.
"""
import bisect
import gzip
import os
import statistics
import sys

# Thresholds for scoring detection and false alarms.
PRESENT_MS = 5.0
DETECTED_MS = 15.0
BLOATED_MS = 30.0
UNITS = {"bit": 1, "Kbit": 1e3, "Mbit": 1e6, "Gbit": 1e9}


def rate_bits(text):
    for unit in sorted(UNITS, key=len, reverse=True):
        if text.endswith(unit):
            return float(text[: -len(unit)]) * UNITS[unit]
    raise ValueError(text)


def size_bytes(text):
    """tc prints sizes within a few bytes of a KiB or MiB multiple as K or M."""
    for suffix, scale in (("M", 1024 * 1024), ("K", 1024)):
        if text.endswith(suffix):
            return float(text[:-1]) * scale
    return float(text)


def open_text(path):
    return gzip.open(path + ".gz", "rt") if os.path.exists(path + ".gz") else open(path)


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int(fraction * len(values)))]


def score(title, estimates, truth, marks):
    """Print one table: estimates are (time, down valid, down ms, up valid, up ms)."""
    times = [estimate[0] for estimate in estimates]
    print(title)
    print(f"{'phase':16} {'dir':4} {'n':>4} | {'true p50/p90/p99':>19} | {'est p50/p90/p99':>19} | "
          f"{'corr':>5} | {'detected':>9} | {'false':>6}")
    for index, (start, name) in enumerate(marks[:-1]):
        if name in ("idle", "start", "capacity-low", "capacity-back"):
            continue
        end = next(t for t, n in marks[index + 1:] if n in ("idle", "end"))
        rows = []
        for t, true_up, true_down in truth:
            if not start + 2 <= t < end:
                continue
            k = bisect.bisect_right(times, t) - 1
            # Both are written per reflector reply; older than 0.5 s is stale.
            if k < 0 or t - times[k] > 0.5:
                continue
            _, down_valid, down, up_valid, up = estimates[k]
            if down_valid and up_valid:
                rows.append(((true_up, up), (true_down, down)))
        for column, direction in ((0, "up"), (1, "down")):
            pairs = [row[column] for row in rows]
            if not pairs:
                print(f"{name:16} {direction:4} no valid estimates")
                continue
            true = [pair[0] for pair in pairs]
            estimated = [pair[1] for pair in pairs]
            try:
                correlation = f"{statistics.correlation(true, estimated):5.2f}"
            except statistics.StatisticsError:
                correlation = "  -  "
            bloated = [pair for pair in pairs if pair[0] > BLOATED_MS]
            detected = (
                f"{100 * sum(pair[1] > DETECTED_MS for pair in bloated) / len(bloated):4.0f}% /{len(bloated):3d}"
                if bloated else "        -"
            )
            false = 100 * sum(pair[0] < PRESENT_MS and pair[1] > DETECTED_MS for pair in pairs) / len(pairs)
            spread = lambda values: "/".join(f"{percentile(values, f):.1f}" for f in (0.5, 0.9, 0.99))
            print(f"{name:16} {direction:4} {len(pairs):4d} | {spread(true):>19} | {spread(estimated):>19} | "
                  f"{correlation} | {detected:>9} | {false:5.1f}%")
    print()


def main(run):
    estimates = []
    fping = []
    # A rotated log continues in .old; read it first.
    lines = []
    if os.path.exists(f"{run}/cake-adapt.log.old") or os.path.exists(f"{run}/cake-adapt.log.old.gz"):
        lines += open_text(f"{run}/cake-adapt.log.old").readlines()
    lines += open_text(f"{run}/cake-adapt.log").readlines()
    for line in lines:
        fields = [field.strip() for field in line.split(";")]
        if fields[0] == "TCP_QUEUE" and len(fields) == 8:
            estimates.append((
                float(fields[2]),
                fields[4] == "1", int(fields[5]) / 1000,
                fields[6] == "1", int(fields[7]) / 1000,
            ))
        elif fields[0] == "DATA" and len(fields) > 19:
            # DL_OWD_DELTA_US and UL_OWD_DELTA_US: fping's RTT/2 delta per reply.
            fping.append((float(fields[2]), True, int(fields[14]) / 1000, True, int(fields[19]) / 1000))
    truth = []
    for line in open_text(f"{run}/backlog"):
        fields = line.split()
        # The first sampler's sed missed K and M sizes, leaving a field out; skip those.
        if len(fields) == 5 and fields[1].endswith("bit") and fields[3].endswith("bit"):
            t, up_rate, up_bytes, down_rate, down_bytes = fields
            truth.append((
                float(t),
                size_bytes(up_bytes) * 8 / rate_bits(up_rate) * 1000,
                size_bytes(down_bytes) * 8 / rate_bits(down_rate) * 1000,
            ))
    marks = [(float(t), name) for t, name in (line.split() for line in open(f"{run}/phases-epoch"))]

    print("Per phase and direction, in ms: true and estimated queue p50/p90/p99, their")
    print(f"correlation, detection (estimate > {DETECTED_MS:.0f} ms while the true queue is > {BLOATED_MS:.0f} ms)")
    print(f"and false alarms (estimate > {DETECTED_MS:.0f} ms while the true queue is < {PRESENT_MS:.0f} ms).")
    print()
    if estimates:
        score("TCP_QUEUE estimates", estimates, truth, marks)
    if fping:
        score("fping per-direction delta (RTT/2, DATA records)", fping, truth, marks)


main(sys.argv[1])
