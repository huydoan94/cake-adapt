# Reference-only VM variation diagnostic

User selected this diagnostic on 2026-10-13. Question: how large are
reference-to-reference differences under the saved comparison's workload?
Exactly two sequential fping-ts cases, reference-a then reference-b, use the
already validated epoch-fixed base binary (SHA256
35ea0a92a4f042f6f88d4dffb9d7cd7ff67ad38b0d2f4d0d49378839951c1e48).
No new builds, candidate cases, tuning or acceptance changes.

Reuse run.sh/testbed.sh unchanged: six reflectors at 300 ms, independent
100-ms fping, UL/DL capacity 8/40 Mbit/s, upload step to 4 Mbit/s,
15-second initial idle and 60-second upload/step/download/bidirectional phases,
10-second gaps, same directional thresholds, rates, and reflector faults.
Distinct case names protect snapshots and archived results. Existing CPU,
log writer/inode, namespace, lock and cleanup guards apply.

One combined read-only VM preflight checks reachability, task-directory absence,
installed service identity, log inode, namespace state, CPU affinity, boot,
clock source and JIT mode. Review shell syntax and the small batch diff locally;
reuse passing unchanged harness tests. No smoke repeat is needed for case names.
Stop rather than overwrite any existing task directories or namespaces.
Record metadata before mutation; require unchanged boot after each case.
The batch deadline is 900 seconds; local SSH deadline 960 seconds. Run in the
background through the established helper. Archive each case immediately to
VM disk, collect into ignored build/, and verify before removing owned payloads.
Installed controller/configuration remain untouched. Preserve the fixed test
log inode. Stop on reboot, harness failure or incomplete data; no automatic rerun.

Analysis requires both complete cases, valid fault counters and goodput, CPU
identities and unchanged log inode. Retain raw logs and report signed/absolute
differences for p50/p95/p99, >15/>30 exposure, loss, UL/DL goodput, CPU and health.
Compare the absolute reference differences with the already saved candidate
differences, without selecting a new baseline or treating spread as a margin.
Two runs are preliminary, not confidence intervals. Gate remains failed.

Stop after collecting, analyzing and cleaning up this single pair. Report
whether reference variation can plausibly explain particular saved gaps, and
what remains unresolved. A changed acceptance design requires a separate
decision; do not advance migration.
