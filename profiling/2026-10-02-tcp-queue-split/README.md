# fping alone vs. TCP-measured queues (2026-10-02)

**Result:** splitting fping's round-trip delay between the directions by the
queues measured from TCP timestamps, instead of RTT/2 each way, roughly
halved the added delay under bidirectional load (median 50 → 28 ms) and
raised download from 41% to 67% of capacity, for 6–7 points of upload
throughput. Upload-only bloat fell too (p99 177 → 123 ms). The estimate it
relies on tracked the emulated ISP's real queue with a correlation of
0.85–0.95 while the controller was running.

fping's RTT/2 reports a one-sided queue at half its size in each direction,
so an upload queue looks like half as much upload delay plus a false
download delay.

## What is compared

Off is fping alone (`tcp_delay_attribution 0`): RTT/2 each way, with a
detected bufferbloat attributed by download delivery. With
`tcp_delay_attribution` and `pinger_method 'fping'`, once the measured
queues total at least 5 ms:

- each reply's round-trip delta (download plus upload RTT/2 delta) is split
  between the directions in proportion to their measured queues, so a
  one-sided queue counts at its full size; and
- only a direction holding at least a quarter of the measured
  queue may be cut.

Below 5 ms, or without valid estimates, RTT/2 and the delivery heuristic
apply unchanged. The estimates are logged as `TCP_QUEUE` records with
`output_processing_stats`.

## Setup

OpenWrt 25.12 x86 VM (kernel 6.12.108), the
namespace testbed with a 4 Mbit/s up / 65 Mbit/s down bottleneck (500 ms
buffers, upload dropping to 2 Mbit/s mid-way through the step phase), shaper
bounds 1/3/5 up and 50/60/70 down Mbit/s, `connection_active_thr_kbps` 750,
fping with cake-autorate's default thresholds, no `ack-filter`. The ISP's tbf
backlog and rate were sampled every 100 ms (`BACKLOG=1`) as ground truth.

| Runs | Binary | `cake-adapt` SHA-256 |
| --- | --- | --- |
| A/B, off and on | the delay split, before `upload_ack_share_min` existed (it defaults to off) | `f9033be67d790b22…` |
| validation | a build of the same estimator that records `TCP_QUEUE` but does not split the delay | `cd78bf66ba263b08…` |

All used the BPF object `e9c04f912f6b63d0…`, placed in `/lib/bpf` for the
test and removed afterwards. Binaries ran in isolation in the `cpe`
namespace; the installed package and the VM's own SQM were untouched.

## Is the estimate right under control?

`raw/Q1-validation`, scored by `tools/testbed/queues.py`
([`validation.txt`](validation.txt)). Queue delays in ms:

| Phase, direction | True p50/p90/p99 | TCP estimate | fping RTT/2 delta | TCP corr | TCP false alarms | RTT/2 false alarms |
| --- | --- | --- | --- | --- | --- | --- |
| upload steady, up | 0 / 134 / 177 | 4 / 120 / 173 | 3 / 64 / 87 | 0.92 | 0.4% | 0.0% |
| upload steady, down | 0 / 0 / 0 | 0.2 / 0.4 / 0.8 | 3 / 64 / 87 | – | 0.0% | 37% |
| download steady, down | 14 / 46 / 80 | 12 / 46 / 109 | 9 / 29 / 64 | 0.95 | 0.0% | 0.0% |
| download steady, up | 0 / 0 / 0 | 0.2 / 0.3 / 0.5 | 9 / 29 / 64 | – | 0.0% | 28% |
| both, up | 43 / 81 / 91 | 31 / 75 / 85 | 26 / 40 / 46 | 0.94 | 0.2% | 18% |
| both, down | 2 / 39 / 71 | 0.3 / 37 / 70 | 26 / 40 / 46 | 0.95 | 0.0% | 52% |

A false alarm is an estimate above 15 ms while the true queue is below 5 ms.
The TCP estimate is a minimum over 100–200 ms windows, so it reads slightly
low on a fluctuating queue; RTT/2 reports a one-sided queue at half its size
and puts the other half on the idle direction.

## A/B

Two alternating rounds of off and on, all with backlog sampling. Means of
the two rounds from [`analyze.txt`](analyze.txt); added RTT is from the
independent 100 ms probe, over its idle median (about 21 ms).

| Phase | Variant | Added RTT p50 / p99 | Time > 30 ms | Upload used | Download used |
| --- | --- | --- | --- | --- | --- |
| both directions | off | 49.5 / 102.3 ms | 68% | 83% | 41% |
| both directions | **on** | **27.8 / 73.0 ms** | **46%** | 76% | **67%** |
| upload steady | off | 6.0 / 176.8 ms | 31% | 82% | – |
| upload steady | **on** | **4.0 / 123.3 ms** | **25%** | 85% | – |
| upload, capacity drop | off | 8.7 / 244.8 ms | 38% | 84% | – |
| upload, capacity drop | on | 5.7 / 340.3 ms | **27%** | 84% | – |
| download steady | off | 20.7 / 94.8 ms | 31% | – | 82% |
| download steady | **on** | **12.3 / 83.6 ms** | **15%** | – | 79% |

Per round, the split's bidirectional download was 64.8% and 69.1% (off:
42.9%, 39.3%), and its median added RTT 27.1 and 28.4 ms (off: 52.7, 46.2).

## Findings

1. **The estimate is usable as a delay signal.** Under the running
   controller it follows the real queue (correlation 0.85–0.95 across the
   split and validation runs), detects 89–100% of samples where the true
   queue exceeds 30 ms, and almost never reports a queue in an idle direction
   (at most 0.9%, against 11–52% for RTT/2).
2. **The split cuts bufferbloat in every steady phase.** The time above
   30 ms fell from 68% to 46% with both directions loaded, from 31% to 25%
   for upload alone and from 31% to 15% for download alone.
3. **Download recovers under bidirectional load,** from 41% to 67% of
   capacity, because upload's queue, which delays download's ACKs, is now
   drained.
4. **Upload pays 6–7 points under bidirectional load** (83% → 76%); in the
   upload-only phases it used slightly more (82% → 85%).
5. **The capacity-drop tail is noisy.** Its p99 varied from 175 to 455 ms
   between runs of both variants; the split's mean (340 ms) comes from one
   455 ms run. The time above 30 ms in that phase still fell (37–38% → 27%).
6. **The upload shaper still overshoots** the 4 Mbit/s bottleneck about
   63% of the time under bidirectional load (off: 72%;
   [`shapers.txt`](shapers.txt)): increases under high load continue between
   detections. Cuts are now sized to the full queue, so episodes end sooner.

## Files

- `raw/Q1-validation/`: the validation run (its log rotated at 2 MB; the
  earlier part is `cake-adapt.log.old.gz`, and its backlog has 42 samples
  that the first sampler recorded without a value).
- `raw/S<round>-<off|split>/`: the A/B runs (off and on), each with the
  probe, the daemon log (DATA, LOAD and SHAPER records, plus TCP_QUEUE when
  on), iperf3 JSON, uptime and wall-clock phase marks, capacities and the
  100 ms backlog samples.
- `validation.txt`, `queues-S1-split.txt`, `queues-S2-split.txt`:
  `tools/testbed/queues.py` per run.
- `analyze.txt`, `shapers.txt`: `tools/testbed/analyze.py raw` and
  `tools/testbed/shapers.py raw`.

## Reproduce

On the VM with the testbed scripts in `/tmp/cake-adapt-test/`, the BPF object
in `/lib/bpf/` and a binary in `/tmp/cake-adapt-test/bin/`:

```sh
./testbed.sh up
UL_CAP=4 DL_CAP=65 UL_STEP=2 UL_MIN=1000 UL_BASE=3000 UL_MAX=5000 \
DL_MIN=50000 DL_BASE=60000 DL_MAX=70000 ACTIVE_THR=750 BACKLOG=1 \
TCP_ATTRIBUTION=1 ./run.sh NAME /tmp/cake-adapt-test/bin/cake-adapt fping 10 30 60
./testbed.sh down
```

Then on the host: `tools/testbed/queues.py raw/NAME`, `analyze.py raw` and
`shapers.py raw`.
