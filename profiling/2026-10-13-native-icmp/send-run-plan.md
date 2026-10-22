# Bounded native timestamp send-boundary diagnostic

2026-10-13, upload diagnosis task 3. Exactly one native-ts case, same full
workload as the saved comparisons; not an acceptance/performance rerun.
Question: does native originate-to-send age or deadline lateness reach the
scale of saved 2.4/7.3-ms upload p95 gaps under the existing control load?

Use an isolated diagnostic binary compiled from a source copy with the existing
x86 SDK compiler and staged dependencies; only monitor/pingers.c differs. Never replace repository/SDK source or
installed service. Its instrumentation reports already captured callback,
realtime and actual pre-send clocks, scheduled send deadline, originate field,
serial, target and send result after each send. Add one diagnostic-only monotonic read immediately after native_tick returns,
before logging, reported as returned_us. Cap at 8000 records per session.
Do not move capture points, change scheduling, rates, thresholds or packets.
Existing DATA records provide receive capture and processing epochs. Capture
clock age through completed send is bounded above by returned_us minus callback_us, because
realtime is captured after callback_monotonic. The pre-send-to-return interval includes possible preemption, syscall time and
post-syscall session work; it cannot separate them or measure NIC transmission.
Report originate floor quantization (<1 ms) separately and include it in the
originate-age-through-return upper bound. Added logging can perturb future callbacks;
diagnostic timings are observations from this build, not acceptance metrics.

Reuse reviewed epoch-batch/run.sh namespace, writer/inode, archive and cleanup
guards. Use literal native-ts as the only case; VM deadline 480 seconds, SSH 540 seconds. Full
15-second idle, four 60-second load phases, 10-second gaps and original health
fault/recovery sequence remain unchanged. Refuse existing task paths/namespaces.
Capture binary/hash, boot, affinity, tsc and JIT; require unchanged boot.
One combined preflight, no unchanged smoke campaign. Stop on reboot, missing
markers, malformed evidence, failed harness or exceeded deadline; no rerun.

Local verification: no production source diff; SDK auto-removed its packaged
intermediates, so rebuild the isolated copy with the saved strict compile flags; formatter stability and patch
style checks; bounded Astra review of instrumentation and batch; synthetic
analyzer checks for units, phase alignment and invalid capture ordering.
Reuse prior unchanged workload/CPU/log helper validation.

Analyze idle and each loaded phase separately: marker count/failures,
callback-to-pre-send and callback-to-return upper-bound age p50/p95/p99/max, send deadline lateness,
global/per-target actual pre-send gaps and receive-to-DATA delay. Require
at least 250 records in initial idle and 1000 in each main phase, six targets,
correct originate modulo-day conversion, strictly increasing serials and clocks,
and complete case/fault/goodput evidence via the existing scorer.
Report fractions with age/lateness above 1, 2.4 and 7.3 ms. Small age supports
deprioritizing this hypothesis in this run, not proving it impossible generally.
Large ages identify a measurement issue to reproduce and isolate, not causation
or permission to modify thresholds. Cadence requires separate attribution if
lateness is material; fping's send clocks are not available in saved DATA logs.

Persist completed raw results on VM disk immediately, collect into ignored build
storage, verify, remove only owned namespaces/processes/payloads, preserve fixed
/tmp/sqm-mon-test.log inode and installed controller. Stop and report after this
single diagnostic. No new SDK/dependency downloads, installation, version bump,
release/deployment, remote/other-branch or router access.
