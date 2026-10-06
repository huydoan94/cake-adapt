#!/usr/bin/env python3
"""Check the bounded baseline-retention batch; no network-capacity claims."""
import json
import statistics
import sys
from pathlib import Path

root = Path(sys.argv[1])
for variant in ("before", "after"):
    run = root / "results" / variant
    phases = {name: int(when) for when, name in
              (line.split() for line in (run / "phases").read_text().splitlines())}
    pairs = []
    for line in (run / "cake-adapt.log").read_text().splitlines():
        fields = [field.strip() for field in line.split(";")]
        if fields[0] == "TCP_QUEUE" and fields[4] == fields[6] == "1":
            pairs.append((float(fields[2]), int(fields[5]), int(fields[7])))
    intervals = {
        "early": (phases["add-delay"] + 2, phases["add-delay"] + 8),
        "late": (phases["clear-delay"] - 8, phases["clear-delay"] - 2),
        "recovered": (phases["clear-delay"] + 4, phases["end"] - 1),
    }
    medians = {}
    for phase, (start, end) in intervals.items():
        selected = [(dl, ul) for when, dl, ul in pairs if start <= when < end]
        assert len(selected) >= 10, (variant, phase, "too few complete pairs")
        medians[phase] = tuple(statistics.median(values) for values in zip(*selected))
        print(variant, phase, "pairs", len(selected), "median_dl_ul_us", medians[phase])
    assert medians["early"][0] >= 50000, (variant, "baseline not established")
    if variant == "before":
        assert medians["late"][0] < 10000, "before did not reproduce expiry"
    else:
        assert medians["late"][0] >= 50000 and medians["late"][1] >= 10000
        assert medians["recovered"][0] < 10000, "after did not recover"
    for direction in ("upload", "download"):
        result = json.loads((run / f"{direction}.json").read_text())
        assert "error" not in result, result
        received = result["end"]["sum_received"]["bytes"]
        assert received > 0, (variant, direction, "no received payload")
        print(variant, direction, "receiver_bytes", received)
    assert "prog_jited:1" in (run / "bpf-fdinfo").read_text().replace("\t", "")

for suffix in ("files", "packages", "service", "daemon-pids", "qdiscs", "routes", "namespaces"):
    assert (root / f"before.{suffix}").read_bytes() == (root / f"after.{suffix}").read_bytes(), suffix
assert (root / "after.namespaces").read_text().strip() == ""
assert (root / "inode-before").read_bytes() == (root / "inode-after").read_bytes()
print("baseline retention, recovery, received payloads, and restoration checks passed")
