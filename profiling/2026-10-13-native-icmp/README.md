# Native ICMP control comparison: epoch bug fixed, gate unmet

2026-10-13, OpenWrt 25.12 i386 VM. The original four-case batch completed;
its native timestamp regression was traced to a saturated epoch conversion and
fixed in `c1b2110` (before the 2026-10-21 history rewrite; now part of `8ef4417`). A fresh timestamp reference/candidate pair confirms realistic
one-way samples and normal upload throughput. **Neither backend has passed the
full no-worse control gate. Fping remains the default; migration is blocked.**

The corrected pair uses the same fixed binary and boot for fping-ts/native-ts.
Combined daemon/pinger CPU is 1.583% versus 0.988% of one core, **37.56% lower**.
Daemon-only CPU rises: 114 versus 268 ticks over about 271 seconds. This moves
work into the daemon; it does not establish a reduction in daemon-only or whole
system CPU. Kernel softirq cost and router performance are not established.

The reference and fixed native timestamp idle OWD sums are 23 and 22 ms,
consistent with independent RTTs of 22.6 and 22.3 ms. Broken native timestamp
samples summed to zero. `us_to_ms()` is a rounded, saturated conversion for
uloop durations: modern epochs became `UINT_MAX`, producing a constant UTC
timestamp. Both native conversions now divide the 64-bit epoch before reducing
to milliseconds since midnight. The new modern fractional-epoch test checks
the send argument and both directional delays; it fails with the old source.
Focused host tests, required C style checks, the x86 base SDK build and Astra
Medium review pass.

The first batch took 1,437 seconds and used the unmodified candidate. It showed
native echo combined CPU 0.853% versus fping 2.514%, but worse delay exposure in
some phases and lower bidirectional download throughput. Broken native-ts
upload was about 1.9 Mbit/s, while download added RTT p95 reached 585–593 ms.
All original measurements, including failed comparisons, are retained in
[`acceptance/initial/`](acceptance/initial/table.md).

The VM rebooted after that batch. Saved results were recovered from `/root`
after its route returned; temporary local collection had disappeared. To avoid
comparing across boots, one fresh fping-ts/native-ts pair ran for 695 seconds,
with unchanged workloads and criteria. Compare this pair internally; differences
between the original and fixed batches cannot all be attributed to the patch.
The fixed binary hash, boot, allowed CPUs 0–3, `tsc` clock and USER_HZ are in its
environment file. BPF JIT was enabled at final cleanup; its original-batch mode
was not captured. No additional performance batch was run.

The table below reports the corrected pair. Full p50/p95/p99, loss and gate
failures are in [`acceptance/epoch-fixed/summary.json`](acceptance/epoch-fixed/summary.json).

| Backend | Daemon ticks | Daemon + pinger ticks | CPU (% of one core) |
| --- | ---: | ---: | ---: |
| fping-ts | 114 | 429 | 1.583 |
| native-ts | 268 | 268 | 0.988 |

| Backend | Phase | Added RTT p95 (ms) | Received probes >30 ms (%) | UL goodput (Mbit/s) | DL goodput (Mbit/s) |
| --- | --- | ---: | ---: | ---: | ---: |
| fping-ts | upload-steady | 76.30 | 25.42 | 6.95 | — |
| fping-ts | upload-step | 91.40 | 24.25 | 5.55 | — |
| fping-ts | download-steady | 42.20 | 23.42 | — | 32.14 |
| fping-ts | bidirectional | 74.20 | 45.35 | 6.62 | 26.70 |
| native-ts | upload-steady | 78.70 | 25.29 | 6.88 | — |
| native-ts | upload-step | 98.70 | 26.41 | 5.57 | — |
| native-ts | download-steady | 40.90 | 21.39 | — | 32.21 |
| native-ts | bidirectional | 69.80 | 44.61 | 6.59 | 28.68 |

| Backend | One-target detection (s) | All-target stall check (s) | Global timeout (s) | Reply after recovery (s) |
| --- | ---: | ---: | ---: | ---: |
| fping-ts | 0.851 | 0.423 | 10.027 | 0.036 |
| native-ts | 1.712 | 0.262 | 10.063 | 0.044 |

| Native method | CPU lower | Health matched | Strict gate passes |
| --- | --- | --- | --- |
| native-ts | True | True | False |

Both fault/recovery sequences satisfy the predeclared timing allowances:
one second for individual reflector detection, 0.5 seconds for stall checks,
global timeout and recovery, based on existing probe/timer cadence. TCP traffic
continues during ICMP loss; these are reflector-loss checks, not link outages.

Strict latency/throughput comparisons still fail, including upload p95 of
78.7 versus 76.3 ms and capacity-step p95 of 98.7 versus 91.4 ms. Steady upload
goodput is 6.88 versus 6.95 Mbit/s. Improved download metrics and lower combined
CPU do not establish overall control equivalence. No threshold was widened.

`run-plan.md` and `epoch-run-plan.md` record workloads, capacities, shaper bounds,
probe cadence, fault phases, deadlines and stop rules. `run.sh` serializes
controllers against owned namespace qdiscs. `batch.sh` reproduces the original
batch; `epoch-batch.sh` allows a validated method subset and a shorter deadline.
Both use six numeric reflectors at 300 ms, with independent 100-ms fping probes.
Epoch phase marks align samples directly. Delay exposure is the percentage of
received probe samples above 15/30 ms, approximating exposure rather than exact
elapsed time; losses are reported separately.

CPU snapshots include daemon own/waited-child ticks and live pinger ticks, with
bounded consistency checks. They exclude iperf and the independent probe, have
10-ms tick quantization and non-atomic sampling limits. Raw counters, stdout,
logs, iperf JSON, fault counters, routes and qdisc snapshots remain inspectable.
Large logs are losslessly gzip-compressed. Re-score with:

```sh
python3 analyze.py acceptance/initial
python3 analyze.py acceptance/epoch-fixed --pair
python3 test_analyze.py
sh test-proc.sh
```

Both scoring commands intentionally exit 1 while preserving the failed result.
The scorer requires complete case sets, successful throughput and applied fault
filters; malformed or incomplete evidence is rejected. `report.py` renders tables.
Astra Medium independently reviewed the corrected comparison and its limits.

Historical harness smoke failures remain in `failed-smoke.log`, `smoke-fixed/`
and `startup-diagnostic.log`: unavailable stat/procfs children and collision with
the installed main-instance lock. Unique sections, procfs PPID discovery,
device/inode writer checks and failure-safe archival were reviewed before the
successful batch. The earlier timing gate evidence lives in the spike report.

Final cleanup removed owned namespaces and collected task artifacts. The fixed
log retained inode 168 within the new boot and remains available for the user's
tail. Installed controller PID 3388 and its executable hash were unchanged in
that pair. No package, service configuration or original controller qdisc was
changed. `comparison-cleanup.log` records the final checks.

The [saved-record diagnostic review](tradeoffs.md), reproducible with
`diagnose.py`, finds no additional proven defect. `reply-diagnostic.json` retains
reply cadence, arrival-to-DATA delays and sample-weighted shaper medians. The approved
[reference-only variation diagnostic](reference-variation.md) is complete: upload
p95 reference gaps of 1.4/1.7 ms are smaller than the saved native gaps of
2.4/7.3 ms. Some other metrics vary more; no additional defect is established.
Phase 4 remains blocked; no unchanged candidate reruns or gate changes.

The subsequent [send-boundary diagnostic](send-diagnostic.md) completes one
instrumented native-ts case. Preparation is 3–5 µs at upload p95; the capture
age bound through completed send is 0.523/0.676 ms before timestamp rounding.
Deadline lateness is 5.924/6.081 ms at upload p95. Tails remain; no new defect
or gate pass is established. Next: equivalent fping/native actual-send capture.

✓ The [equivalent-ingress cadence pair](cadence-diagnostic.md) is complete:
zero buffer drops and all selected requests matched. Native median per-target
spacing is 2.456/1.311 ms slower under upload; originate→ISP p95 differs by
only +0.100/-0.078 ms. No new proven defect or acceptance claim. Next: verify
libubox timer-boundary behavior and quantify early callbacks/retries if needed.
