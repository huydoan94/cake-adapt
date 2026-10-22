# Saved timestamp comparison: diagnostic review

The corrected same-boot pair still fails its declared acceptance gate. Reviewing
its DATA records identifies no additional proven implementation defect. This
review uses existing evidence only; no VM run, tuning or gate change was made.

| Phase | Reference/native samples | Reference/native median reply gap (ms) | Reference/native median UL rate (Kbit/s) | Reference/native median DL rate (Kbit/s) |
| --- | ---: | ---: | ---: | ---: |
| Upload steady | 1174 / 1164 | 308.72 / 311.13 | 7220 / 7092.5 | 30000 / 30000 |
| Upload step | 1169 / 1169 | 308.26 / 308.92 | 6009 / 6181 | 30000 / 30000 |
| Download steady | 1169 / 1165 | 308.49 / 310.37 | 6084 / 6000 | 46293 / 46761 |
| Bidirectional | 1173 / 1172 | 309.00 / 309.46 | 7723 / 7802.5 | 47228 / 49407 |

Native has slightly fewer samples in some phases, but no gross cadence collapse.
Its median kernel arrival-to-DATA delay is 0.265–0.306 ms under load versus
0.548–0.673 ms for fping's printed arrival-to-DATA delay. These arrival clocks
have different capture points; this is not a pure event-loop cost comparison.
Reply gaps include changing network delay, and do not measure actual send gaps.
Rate medians are sample-weighted snapshots, not elapsed-time averages or goodput.

Native's larger bidirectional download rate accompanies 7.4% more download
goodput and lower p95 delay. Its smaller steady upload rate accompanies 1.0%
less upload goodput and 2.4 ms higher p95. In the capacity-step phase, p95 is
7.3 ms higher despite slightly higher goodput. This is mixed evidence; it does
not establish whether VM variation, sampling differences or a remaining defect
causes the failures. Source review confirms the same timestamp directional
formulas and per-reflector sample sequence numbering. Native scheduling is based
on actual sends; the reply logs alone cannot justify changing its scheduler.

Reproduce the diagnostic independently of the unchanged acceptance scorer:

```sh
python3 diagnose.py acceptance/epoch-fixed > reply-diagnostic.json
```

## Reference-only diagnostic (approved and completed)

The user approved one **reference-only calibration pair**, using two distinctly named
fping-ts cases with the existing corrected binary and unchanged full workloads,
on one boot. Preserve both logs separately; the existing method-named batch
must be adjusted to avoid overwriting repeated methods. Bound the batch at
900 seconds and SSH at 960 seconds, stop on a reboot or harness failure, retain
each completed case on VM disk and verify the existing cleanup invariants.

Report reference-to-reference absolute differences for every current latency,
exposure, loss, goodput and CPU metric. Neither run is a replacement reference
for the saved candidate; do not select the more favorable baseline or claim
acceptance. Two runs only give a preliminary spread, not a confidence interval.
If their spread is comparable to the saved regressions, propose a separately
approved, predeclared repeated alternating comparison with practical margins.
If it is clearly smaller, investigate the candidate's measurement/control path
before any migration. This diagnostic changes the experiment's question rather
than rerunning candidates until one passes. The current strict gate remains
failed throughout.

The completed result is in [reference-variation.md](reference-variation.md). The
record above preserves its original question and limits; no acceptance margin
was derived or applied.
