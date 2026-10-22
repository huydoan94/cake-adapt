#!/usr/bin/env python3
"""Score the predeclared gate; no repeated runs or fitted tolerance."""
import csv
from decimal import Decimal, ROUND_HALF_UP
import json
import pathlib
import re
import statistics
import sys

FPING = re.compile(r"^\[[0-9.]+\] 10\.99\.0\.11\s+: \[0\], \d+ bytes, ([0-9.]+) ms")


def native_values(path):
    fields = None
    rows = []
    for line in path.read_text().splitlines():
        if line.startswith("serial,"):
            fields = next(csv.reader([line]))
        elif re.match(r"^\d+,", line):
            values = next(csv.reader([line]))
            assert fields and len(values) == len(fields), line
            rows.append(dict(zip(fields, values)))
    assert all(row["kernel_timestamp"] == "1" for row in rows)
    return [int(row["rtt_us"]) for row in rows]


def summarize(values):
    ordered = sorted(values)
    assert ordered
    return {
        "samples": len(values),
        "p5_us": ordered[int((len(values) - 1) * .05)],
        "median_us": statistics.median(ordered),
        "p95_us": ordered[int((len(values) - 1) * .95)],
        "minimum_us": min(ordered),
        "maximum_us": max(ordered),
    }


def analyze(root, count):
    timeline = list(csv.DictReader((root / "timeline.csv").open()))
    expected = [("fping-before", "fping")] * count
    expected += [("interleaved", b) for _ in range(count) for b in ("native", "fping")]
    expected += [("fping-after", "fping")] * count
    assert [(r["batch"], r["backend"]) for r in timeline] == expected
    assert all(r["status"] == "0" for r in timeline)
    assert all(int(r["end_cs"]) >= int(r["start_cs"]) for r in timeline)
    assert all(int(b["start_cs"]) >= int(a["end_cs"]) for a, b in zip(timeline, timeline[1:]))
    results = {}
    for batch in ("fping-before", "interleaved", "fping-after"):
        text = (root / f"{batch}.fping").read_text()
        values = [int((Decimal(m.group(1)) * 1000).quantize(Decimal(1), rounding=ROUND_HALF_UP))
                  for line in text.splitlines() if (m := FPING.match(line))]
        assert len(values) == count, (batch, len(values), count)
        results[batch + "-fping"] = summarize(values)
    values = native_values(root / "interleaved.native")
    assert len(values) == count, ("native", len(values), count)
    results["interleaved-native"] = summarize(values)
    spread = abs(results["fping-before-fping"]["median_us"] - results["fping-after-fping"]["median_us"])
    gap = abs(results["interleaved-native"]["median_us"] - results["interleaved-fping"]["median_us"])
    results["gate"] = {"median_gap_us": gap, "baseline_median_spread_us": spread,
                       "passed": gap <= spread, "requests": len(timeline),
                       "native_kernel_timestamps": len(values), "overlap_observed": False}
    return results


if __name__ == "__main__":
    root = pathlib.Path(sys.argv[1])
    result = analyze(root, int(sys.argv[2]) if len(sys.argv) > 2 else 300)
    (root / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    sys.exit(0 if result["gate"]["passed"] else 1)
