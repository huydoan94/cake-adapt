# Why native-ts fails upload-step p95 (2026-10-21)

**Result:** the 11-ms upload-step p95 gap comes from where each controller's
upload sawtooth happens to be when the capacity drops, not from slower
detection or reaction by native-ts. The test drops the upload capacity at a
fixed instant, and native-ts's sawtooth arrives there about 1.7 s behind
fping-ts's in every round. No native-ts defect is shown, and no fix is
indicated. Analysis of the saved records only (`drop.py`, `raw.tar.xz`); no
new runs.

**Follow-up:** the jittered rerun in
[`../2026-10-21-native-icmp-jitter/`](../2026-10-21-native-icmp-jitter/README.md)
confirms that the single fixed drop decided the result (native-ts wins
upload-step p95 with random drop times). It also shows that the shaper rate
at a drop barely predicts the peak (correlation 0.04 over 18 drops), so the
rate explanation below is at most part of the story.

## The gap is in the first 3 seconds after the drop

Independent probe RTT p95 (ms), rounds 1–3:

| Window | fping-ts | native-ts |
| --- | --- | --- |
| Step, before the drop | 94, 89, 99 | 99, 97, 100 |
| First 3 s after the drop | 244, 211, 249 | 332, 427, 518 |
| Rest of the low phase | 132, 123, 125 | 133, 127, 130 |
| 20 s after recovery | 94, 93, 97 | 106, 97, 92 |

## Detection and reaction are not slower

Seconds after the drop:

| | First delayed sample | First bufferbloat | First upload cut |
| --- | --- | --- | --- |
| fping-ts | 0.28, 0.15, 0.22 | 0.42, 0.35, 0.38 | 0.14, 0.15, 0.07 |
| native-ts | 0.21, 0.19, 0.15 | 0.39, 0.39, 0.36 | 0.21, 0.19, 0.04 |

Both cut in the same steps once bufferbloat is seen (DATA and SHAPER traces).
Native-ts samples are fresher: median sample age (log time − ICMP timestamp)
0.14–0.19 ms against 0.34–0.43 ms. It has fewer samples in the first second
(11–14 against 15–16) because its upload delays grow faster, which follows
from the higher starting rate below.

## It is the shaper rate at the drop

Median upload shaper rate in the last second before the drop: fping-ts
5348/6577/5604 kbit/s, native-ts 6854/7124/7649 kbit/s. Over the whole step
phase before the drop, native-ts runs *lower* (median 7342/7417/7451 against
7808/7662/7673). The difference is the sawtooth phase at the drop instant:

| | First cut after the step starts (s) | Sawtooth period (s) | Last cut before the drop (s earlier) |
| --- | --- | --- | --- |
| fping-ts | 2.6, 2.7, 2.7 | 3.24, 3.04, 3.21 | 1.19, 2.07, 1.25 |
| native-ts | 3.9, 3.9, 3.5 | 3.33, 3.24, 3.39 | 2.81, 3.18, 2.97 |

So fping-ts has just cut when the drop comes, and native-ts is about 3 s into
its ramp, further above the new 4 Mbit/s.

The offset comes from the earlier phases, as a cascade:

1. **End of upload-steady.** fping-ts finishes upload-steady with its shaper
   at 9174/8256/8085 kbit/s, native-ts at 5862/6670/7110 kbit/s.
2. **Idle gap.** In the 10-s idle gap both decay only part of the way toward
   the 6000-kbit/s base.
3. **Start of upload-step.** fping-ts starts upload-step above 6500 kbit/s (at
   0.03–0.05 s), native-ts reaches it only at 0.64–0.97 s. So fping-ts reaches
   its first bufferbloat about 1 s sooner.
4. **The drop.** With nearly equal periods, that offset is still there 20 s
   later, when the capacity drops.

## Limits

That the offset is the same in all three rounds may be chance or may be
systematic. The sawtooth periods differ by about 0.1–0.2 s, possibly from the
300-ms against 307-ms sample cadence, and over the 60-s steady phase that
shifts where each controller stands at the phase boundaries. Either way, a
drop at one fixed instant measures the sawtooth phase as much as the
reaction. Settling it needs drops at several offsets within the phase (for
example 3 drops per run at jittered times), which is a harness change, not a
native change.

Native echo's failures (download goodput −6%, two marginal bidirectional
metrics) are not analysed here.
