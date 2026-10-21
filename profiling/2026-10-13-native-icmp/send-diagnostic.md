# Native timestamp send diagnostic: preparation is small, cadence unresolved

2026-10-13. One full native-ts diagnostic case completed on the same VM boot
as the retained comparisons. It uses an isolated source copy with only the
reviewed monitor instrumentation; production source, installed daemon, packets,
rates, thresholds and migration gate are unchanged. **This is not an acceptance
comparison and establishes no new production defect.**

| Phase | Send markers | Callback→pre-send p95 (ms) | Callback→completed-send bound p95 (ms) | Originate age including rounding bound p95 (ms) | Send deadline lateness p95 (ms) | Per-target pre-send gap median (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Initial idle | 291 | 0.003 | 0.176 | 1.011 | 5.743 | 309.940 |
| Upload steady | 1165 | 0.003 | 0.523 | 1.230 | 5.924 | 310.174 |
| Upload step | 1156 | 0.005 | 0.676 | 1.315 | 6.081 | 310.810 |
| Download steady | 1155 | 0.007 | 1.556 | 1.959 | 6.342 | 310.404 |
| Bidirectional | 1167 | 0.003 | 0.699 | 1.286 | 5.348 | 308.200 |

Preparation between the callback clock and pre-send clock is only 3–5 µs at
upload p95. Originate uses a realtime clock captured between those two clocks.
This small interval does not support changing its capture location as the
explanation for the saved 2.4/7.3-ms upload p95 gaps in this run.

The completion bound includes possible preemption, sendto, and session tail
work. It reaches 5.483 ms maximum in steady upload, 23.916 ms in upload step,
and 195.421 ms in download. It is an upper bound on capture age through syscall
completion, not a measurement of NIC transmission or pure timestamp bias.
Originate floors the realtime capture to milliseconds: that fraction is added
separately to the conservative bound. In upload, those bounds exceed 2.4 ms for
0.687%/1.730% of sends and 7.3 ms for 0%/0.606%. Small p95 values do not rule
out the observed tails or establish their effect on control.

As a cross-check specific to this testbed, all six reflector namespaces share
the VM realtime clock. Match DATA sequence/target to sends and reconstruct peer
receive from originate epoch plus raw UL OWD, allowing 1 ms uncertainty. For
pre-send-to-return intervals >2.4 ms, 3 of 34 matched download requests reached
the peer before the conservative return boundary; none of the 5/19 upload
outliers satisfy that check. Thus some long return intervals include time after
peer receipt, and cannot all be called originate-to-wire delay. This check is
invalid for independently clocked internet reflectors.

Deadline lateness is substantial even during idle: 5.743 ms at p95, versus
5.924/6.081 ms under upload. Median per-target pre-send gaps are about 308–311 ms
for configured 300 ms, consistent with scheduling from actual sends and global
50-ms spacing. Existing fping DATA gives reply gaps, not its actual send clocks;
the evidence cannot yet show a candidate-specific cadence defect. Logging after
each captured send can perturb subsequent callbacks. Do not attribute these
instrumented timings to the uninstrumented daemon or to routers.

The independent probe loses one sample in download and none in the other main
phases. Complete workload/fault/health/CPU records are retained, but latency,
goodput and CPU from this instrumented case are diagnostic only. No baseline
was replaced and no acceptance threshold changed.

Reproduce with the existing scorer beside the diagnostic analyzer:

```sh
python3 diagnose-send.py acceptance/send-diagnostic
python3 test-diagnose-send.py acceptance/epoch-fixed/native-ts
```

`send-run-plan.md`, `instrumented-batch.sh` and `send-instrumentation.diff`
record the bounded question, single-case workload, capture points and patch.
SDK auto-removal left no reusable intermediate objects, so an isolated source
copy was compiled with the saved strict x86 SDK flags/staged dependencies.
No SDK/source/dependency change or download. Diagnostic binary SHA256:
`15edd174325120f035efc494f4bbe51630b594c9499bbe92417b0917f5e3d8d7`.
Initial local build/selector failures were corrected before VM execution.
Final strict compile/link, formatter and changed-line style checks pass.
Default production style check passes; whole-file inspection also exposes
unchanged legacy style findings, which were not refactored in this diagnostic.
Astra Medium reviewed the capture boundaries and single-case harness; analyzer
clock ordering and pre-logging capture placement findings were fixed and approved.
Synthetic tests reject backwards clocks before phase filtering, negative receive
delays and invalid completion ordering, and verify units/bounds.

Raw records and metadata are in `acceptance/send-diagnostic/`. All marker clocks
increase before phase filtering; main serials are contiguous, six targets are
present and counts meet the declared minimum. Boot is unchanged, CPUs 0–3,
tsc, JIT enabled. Installed PID 3388 and hash remain unchanged; owned VM
namespaces, processes, locks and payloads were removed after collection. Fixed
test-log inode 168 remains. No default switch or legacy removal.

Next: obtain actual fping and native send cadence with equivalent capture points,
preferably using an already available packet-capture facility rather than more
logging in one method. First check local/test-VM capabilities and write a bounded
paired diagnostic, controlling its own capture cost. Distinguish kernel queue
entry from peer arrival and syscall return. Do not move timestamp fields or
change scheduling on this evidence alone. Stop before another acceptance run
until there is a demonstrated defect or a separately agreed acceptance design.
