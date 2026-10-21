# Native ICMP: revised network timing gate

2026-10-13, branch `icmp-native`, source commit `12ef66a`. One unloaded
acceptance batch on the OpenWrt i386 test VM passes the revised phase-1 gate.
The daemon implementation and installed service were unchanged.

| Cohort | Replies | Median RTT (µs) | p5 (µs) | p95 (µs) |
| --- | ---: | ---: | ---: | ---: |
| fping before | 300 | 21650 | 20500 | 27000 |
| Interleaved fping | 300 | 21700 | 20500 | 28400 |
| Interleaved native | 300 | 21635 | 20473 | 27439 |
| fping after | 300 | 21500 | 20500 | 26400 |

The formula was fixed before collection: absolute interleaved median gap must
be no larger than the absolute difference between the two fping-only medians.
**65 µs ≤ 150 µs: pass.** All 1,200 requests succeeded; all 300 native replies
had kernel receive timestamps. This is a median-agreement result for one batch,
not statistical equivalence, tail-latency parity or a router performance claim.
Control behavior, throughput, loss/stall response and CPU savings remain pending.

`run.sh` creates the isolated testbed using `testbed.sh`: 10-ms netem delay in
each direction to 10.99.0.11, CAKE at 6 Mbit/s upload and 30 Mbit/s download.
`probes.sh` sends one packet per process, waits for that process to exit, then
starts the next. The order is 300 fping-only requests, 300 alternating native/
fping pairs, then 300 fping-only requests. Each backend has a nominal 200-ms
interval; alternating turns are 100 ms apart. Bash timed reads implement pacing
at the recorded 10-ms `/proc/uptime` resolution. No two measurement probes run
simultaneously. Process waits enforce serialization; the timeline alone cannot
resolve overlap shorter than 10 ms. Per-packet process startup differs from
the daemon's persistent event-loop sockets and is unsuitable for CPU comparison.

`acceptance/` contains complete raw stdout, exit statuses, timeline, environment,
qdisc snapshots, cleanup and `summary.json`. Re-score with
`python3 analyze.py acceptance`. The scorer validates exact order/counts,
successful exits, recorded nonoverlap and native kernel timestamps. Decimal
conversion avoids floating-point artifacts in fping's millisecond values.
`mock.sh` and `test_analyze.py` cover serialization and rejection of malformed
evidence; they do not substitute for the collected batch.

`failed-smoke/` preserves the initial smoke failure: BusyBox sleep rejected a
fractional delay. `smoke-fixed/` preserves the successful eight-request check
after switching to Bash timed reads. Destinations are separate and never reused;
only one full acceptance batch was run (186 seconds, exit 0).

Cleanup removed the owned namespaces and both task artifact directories after
collection. The installed controller remained PID 3420 with the same executable
hash and boot ID; no package, service configuration, controller qdisc or daemon
test log was changed. `final-cleanup.log` records the final artifact removal.
Astra Medium independently approved the counts, formula and bounded conclusion.

Together with the earlier quiet-host and VM-loopback diagnostics, this completes
the revised phase-1 gate. Phase 3's controlled backend comparisons remain next;
fping stays the default until the migration gates pass.
