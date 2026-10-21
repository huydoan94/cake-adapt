# Native ICMP: revised phase-3 gate, 3 alternating rounds (2026-10-20)

**Result: both native methods fail the gate, each on few metrics; fping stays
the default.** Native timestamps (`native-ts`) fail only upload-step added-RTT
p95, by a consistent 11 ms (101–110 ms against fping-ts 90.4–90.7 ms); every
other metric is within fping-ts's own round-to-round range or better. Native
echo fails steady-download goodput (about 6% lower in all rounds) and, by a
small margin, bidirectional p95 and upload goodput. Both use about a third
less combined daemon + pinger CPU, and reflector-loss detection matches.

Binary: `cde7c67`/`a3c13a6` (x86 SDK base package, SHA256
`cc140526769d2ea5a3295557a80081e764591bcf7abd44c1c5a9a5992e93e52b`), with the
due-time scheduling fix: native per-reflector reply spacing is now 300.0 ms
(310.0 ms before the fix; fping-ts 307.5 ms).

## Gate (user decision 2026-10-13)

3 rounds of each pair, alternating which method runs first. A metric passes
when native's median over the rounds is better than fping's, or worse by no
more than fping's range (max − min) over the rounds. Combined CPU must be
lower (medians) and every round's loss/recovery detection within
`analyze.py`'s allowances against the same round's fping case. Scored by
[`gate.py`](gate.py), which applies Codex's per-case `analyze()` from
`../2026-10-13-native-icmp/`; workloads and phases are that folder's
`run-plan.md` (six numeric reflectors at 300 ms, 8/40 Mbit/s with a 4 Mbit/s
upload step).

## native-ts against fping-ts

Added RTT p95 (ms), rounds 1–3:

| Phase | fping-ts | native-ts | Median Δ | fping range | |
| --- | --- | --- | ---: | ---: | --- |
| Upload steady | 72.0, 76.8, 74.7 | 79.7, 78.3, 76.8 | +3.6 | 4.8 | pass |
| Upload step | 90.6, 90.4, 90.7 | 101.7, 101.3, 109.5 | +11.1 | 0.3 | **fail** |
| Download steady | 45.0, 47.2, 43.2 | 43.7, 44.7, 44.6 | −0.4 | 4.0 | pass |
| Bidirectional | 73.4, 78.2, 65.4 | 63.8, 64.5, 80.5 | −8.9 | 12.8 | pass |

All delay-exposure (>15/30 ms) and goodput metrics pass. Combined CPU,
% of one core: fping-ts 1.07/0.89/0.95, native-ts 0.69/0.60/0.61.

Lead, not a proven cause: after the upload capacity drops to 4 Mbit/s,
native-ts first sets the upload shaper to 4 Mbit/s or below 1.11/1.15/1.22 s
after the drop, against 0.92/1.10/1.06 s for fping-ts (LOAD records); the
mean shaper rate during the low phase is similar. A slightly slower first
reaction fits a higher p95 in that phase only.

## native against fping

| Failing metric | fping | native | Median Δ | fping range |
| --- | --- | --- | ---: | ---: |
| Download steady goodput (Mbit/s) | 31.58, 32.45, 31.93 | 30.04, 29.83, 31.21 | −1.89 | 0.87 |
| Bidirectional added RTT p95 (ms) | 98.85, 103.9, 103.3 | 109.0, 98.6, 113.3 | +5.70 | 5.05 |
| Bidirectional upload goodput (Mbit/s) | 6.99, 6.83, 7.06 | 6.74, 6.82, 6.53 | −0.26 | 0.24 |

The other 18 metrics pass, several clearly better (upload-steady and
bidirectional p50 and delay exposure). Combined CPU: fping 1.07/0.87/0.86,
native 0.70/0.59/0.61.

The full per-metric table, including p99 and loss, is in
[`gate.json`](gate.json).

## Runs and limits

VM .2 (OpenWrt 25.12 i386), 2026-10-20 15:54–17:30 UTC; the installed
controller (main instance) kept running on its own qdiscs, the harness on
owned namespaces. The VM panicked twice under load (the known i386 panic),
during `r2-ts` and `r3-ts`; each lost pair was rerun from the start on the
new boot, and its partial output is kept with the raw data. So each round
ran on its own boot, both cases of a pair always on the same one; the test
log kept its inode within every case. The serial console logs are on the
Windows host and were not collected.

Three rounds give a small sample: the reference range is the spread of three
values, and a consistent difference (upload-step p95, download goodput) is
more telling than a marginal one. The testbed is an emulated line under
VirtualBox; router timings may differ.

Order (first method first): r1 fping-ts/native-ts and fping/native, r2
reversed, r3 as r1. Re-score with:

```sh
tar -xJf raw.tar.xz -C /tmp/gate && python3 gate.py /tmp/gate
```
