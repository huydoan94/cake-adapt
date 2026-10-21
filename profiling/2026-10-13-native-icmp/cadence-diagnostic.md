# Equivalent ingress capture: small cadence difference, no proven control defect

2026-10-13. One bounded fping-ts/native-ts pair used the unchanged epoch-fixed
binary on one boot, with identical capture filters and positions. Each case
ran initial idle, steady upload and the upload capacity step only. Both captures
report zero kernel-buffer drops; all selected first-capture requests have one
matching reflector capture. Independent upload probes lost no packets.
**This is diagnostic evidence, not a new acceptance baseline. Migration stays
blocked and fping remains the default.**

| Phase | Method | Selected/matched requests | Median per-target ISP ingress gap (ms) | Global gap p95 (ms) | Originate→ISP ingress p95 (ms) | ISP→reflector ingress p95 (ms) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Idle | fping-ts | 292 / 292 | 309.205 | 56.200 | 1.076 | 17.597 |
| Idle | native-ts | 291 / 291 | 310.949 | 55.893 | 1.044 | 16.131 |
| Upload steady | fping-ts | 1168 / 1168 | 307.039 | 56.458 | 5.708 | 81.722 |
| Upload steady | native-ts | 1163 / 1163 | 309.495 | 56.749 | 5.808 | 87.561 |
| Upload step | fping-ts | 1170 / 1170 | 307.624 | 56.570 | 6.366 | 107.549 |
| Upload step | native-ts | 1165 / 1165 | 308.935 | 56.711 | 6.288 | 104.376 |

Native's median per-target gaps are 2.456 ms larger in steady upload and
1.311 ms larger in the capacity step. Both exceed the configured 300-ms period
at this post-CAKE capture point. First-boundary p95 timing differs by only
+0.100/-0.078 ms under upload; this pair does not reveal a large extra native
originate-to-first-boundary delay. Queue/transit p95 is higher for native in
steady upload and lower during the step. One sequential pair cannot establish
the cause or the effect of cadence on controller behavior.

The first capture is ISP iwan ingress, after cpe CAKE and before the simulated
uplink queue; the second is inet0 ingress at the reflector side after that
queue/netem. Kernel capture timestamps are receiving-interface observations.
They do not identify application sendto entry/return or physical NIC transmission.
Originate→first includes CAKE, transit and millisecond timestamp rounding;
do not label it pure timestamp bias. The idle between-boundary delay is about
11 ms at median, consistent with the configured 10-ms uplink netem plus
scheduling/queue effects. Full p50/p95/p99/max values and matched coverage are
in [summary.json](acceptance/cadence-diagnostic/summary.json).

Steady-upload goodput is 6.864/6.813 Mbit/s; step goodput 5.537/5.520 Mbit/s
(fping/native). These values are diagnostic only, with capture enabled and an
upload-only workload. They do not revise the saved full comparison or turn
improvements into acceptance. DATA populations can differ slightly from capture
counts because each is assigned by its own observation time. Fping's DATA
arrival field is its printed receive time; native's is the kernel receive time,
so arrival-to-DATA distributions are not pure processing-cost comparisons.

Two tcpdump processes consume 3/7 ticks over 148.71/148.69 s: 0.020%/0.047%
of one core at USER_HZ=100. This excludes startup and kernel packet-copy/filter
costs and has 10-ms tick quantization. Identical capture setup controls the
observation method; it does not establish zero perturbation or equal total
observer overhead. Capture stderr, process CPU snapshots, PCAPs, configs,
iperf JSON, phase marks and daemon/probe records are retained.

Reproduce:

```sh
python3 capture.py acceptance/cadence-diagnostic
python3 test-capture.py acceptance/epoch-fixed/native-ts
```

Parser tests cover both byte orders/precisions, request tuples, malformed and
truncated framing, protocol/fragments/duplicates, capture drops, known 20-ms
path delay, 300-ms spacing and CPU units. Substantial unmatched coverage is
reported even with an empty peer capture, with null delay metrics; only
first-capture minima apply. That reporting issue was caught and fixed by Astra
Medium review. Shell syntax and complete synthetic case checks pass.

`cadence-run-plan.md`, `cadence-batch.sh` and `capture-run.sh` preserve the bounded
question and upload-only early exit. Capture drain/stop/wait precedes archival,
with signal escalation bounded; existing controller/log/namespace guards remain.
Both methods use binary SHA256
`35ea0a92a4f042f6f88d4dffb9d7cd7ff67ad38b0d2f4d0d49378839951c1e48`.
Metadata records boot, CPUs 0–3, tsc, JIT enabled and tcpdump 4.99.6/libpcap
1.10.6 (32-bit build, 64-bit time_t, TPACKET_V3). No downloads/build/install or
production source change. Installed PID 3388/hash remain unchanged; owned
namespaces, processes, locks and payloads were removed after collection.
Fixed test-log inode 168 remains.

## Next hypothesis: early timer callbacks and retries

Source inspection of the SDK's already cached libubox 2026.06.19~7dd12784
archive shows that timeout arm times retain microseconds, while tv_diff,
poll-wait calculations and timeout dispatch use whole-millisecond differences.
A same-second deadline 900 µs in the future has a zero-millisecond difference
and can be dispatched early. Native refuses an early send and re-arms a
rounded-up millisecond timeout; repeated early callbacks are a possible source
of overhead. No callback-count measurement yet establishes this on the VM.
Do not say that libubox floors the stored arm time or attribute the observed
cadence difference to this mechanism without verification.

Cached archive SHA256:
`82f1a83069c0636722fc1c57d567a21f8b14d14904c8bf10059d2a467edd2a10`.
Next: verify the VM/library implementation match, reproduce the timer-boundary
case locally, and measure aggregate callback attempts versus sends if needed
with one bounded, low-overhead diagnostic. Avoid per-callback logging. Only a
demonstrated defect supports a scoped fix and changed-input acceptance run;
no timer redesign, interval tuning or threshold waiver on this evidence alone.
