# Reference variation: upload regressions remain unresolved

2026-10-13. The user-approved diagnostic completed exactly two fping-ts cases,
reference-a then reference-b, using the already validated epoch-fixed binary
and unchanged full workloads. Both completed on boot
`41490fdc-fa1d-4190-89f8-684d6078a14b`, also used by the saved corrected native
pair. The batch exited 0, with no main-phase independent-probe losses. It ran
with a recorded phase span of 707.8 seconds from the first case start through
the second case health-end; both fault/recovery sequences are included.

**The migration gate remains failed.** These references do not replace the
saved baseline. Two sequential runs establish an observed difference, not a
confidence interval or an acceptance margin.

| Phase | Reference A/B added RTT p95 (ms) | Absolute reference difference (ms) | Saved native minus reference (ms) |
| --- | ---: | ---: | ---: |
| Upload steady | 77.0 / 78.4 | 1.4 | +2.4 |
| Upload step | 96.2 / 94.5 | 1.7 | +7.3 |
| Download steady | 42.8 / 47.2 | 4.4 | -1.3 |
| Bidirectional | 72.6 / 81.5 | 8.9 | -4.4 |

The saved upload p95 gaps exceed this pair's reference differences; this pair
does not explain them away. Capacity-step p99 varies by 46.3 ms, close to the
saved candidate's 47.3-ms increase. Bidirectional p95 varies more than the
saved improvement. Variation is metric-specific: neither every difference nor
every improvement can be classified as noise from this small diagnostic.

| Phase/direction | Reference A/B goodput (Mbit/s) | Absolute reference difference | Saved native minus reference |
| --- | ---: | ---: | ---: |
| Upload steady UL | 6.865 / 6.865 | <0.001 | -0.064 |
| Upload step UL | 5.639 / 5.450 | 0.189 | +0.014 |
| Download steady DL | 31.684 / 32.362 | 0.678 | +0.071 |
| Bidirectional UL | 6.620 / 6.473 | 0.147 | -0.026 |
| Bidirectional DL | 27.769 / 27.630 | 0.139 | +1.975 |

Steady-upload goodput is nearly identical between references, while the saved
native case delivered 0.064 Mbit/s less. Some other saved goodput gaps fit
within these observed differences. Native's saved bidirectional download gain
is larger. None establishes causation or substitutes for the original failed
gate. Full precision, p50/p99, exposure, loss, CPU and health differences are in
[summary.json](acceptance/reference-variation/summary.json).

Combined CPU is 1.258%/1.173% of one core: 341/318 ticks over 271.12/271.06
seconds. Daemon-only counts are 93/87 ticks. This reference difference is
0.085 percentage points. It does not revise the saved native pair's CPU result;
these references also differ from that pair's 1.583% reference CPU. Kernel,
whole-system and router CPU remain unmeasured. One-target detection is
1.729/1.809 s; all-target stall check 0.311/0.387 s; global timeout
10.111/9.987 s; first reply after recovery 0.021/0.056 s.

`reference-batch.sh` retains the reviewed namespace, writer/inode, cleanup,
archive and 900-second watchdog safeguards. Its only workload change is two
distinct names with literal fping-ts; boot checks and JIT capture were added.
Astra Medium's bounded static review found no blocking issue. The primary
verified binary identity, analyzed complete cases and self-reviewed the report.
Analyzer checks confirm zero differences for identical input, unchanged gate
status, and rejection of changed boots/incomplete cases. Reproduce from this
directory:

```sh
python3 variation.py acceptance/reference-variation acceptance/epoch-fixed/summary.json
```

Raw case logs are losslessly compressed, with configs, phase marks, iperf JSON,
fault counters, CPU snapshots and qdisc snapshots retained. Metadata records
CPUs 0–3, tsc clock, BPF JIT enabled and binary identities. Installed daemon PID
3388 and its hash stayed unchanged. Owned namespaces, processes, locks and VM
payloads were removed after collection; fixed test-log inode 168 was preserved.
No package, service configuration, controller source, defaults or gate changed.

Next: investigate a concrete candidate measurement/scheduling hypothesis for
the upload p95 and steady-upload goodput gaps, using existing evidence first.
The diagnostic found no additional proven code defect. Do not repeat candidates
unchanged to seek a pass or proceed to legacy removal/default switching.
