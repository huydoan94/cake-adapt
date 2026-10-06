# Sampling TCP timestamps at most every 4 ms per flow (2026-10-03)

**Result:** throttling the TCP filter to at most one recorded departure and
one unmatched reply sample per flow per 4 ms makes the filter 10–34% cheaper
per packet and cuts the daemon's CPU slightly, while the queue estimate stays
as accurate as before (correlation 0.87–0.95 with the real ISP queue, false
alarms ≤ 0.9%, in every variant). The controller pays a small latency cost:
over five alternating rounds, bidirectional p99 added RTT rose from 67.5 to
77.0 ms and download-only latency by 1–3 ms, while medians stayed within the
run-to-run spread. The owner chose to ship 4 ms for the CPU saving.

A 10 ms interval saved 26–49% but cost more latency in its two rounds and
was not pursued.

## The rule

Linux TCP timestamps tick every 1 ms, and in the testbed each flow sends less
than one packet per millisecond, so the old "once per TSval change" limit
recorded a departure and emitted a ring-buffer record for almost every
packet. Thinning is by time rather than by TSval value, because LAN clients'
timestamp clocks tick at different rates:

- a departure is recorded for a new TSval only if the flow's last recorded
  departure is at least 4 ms old;
- a reply whose new TSecr echoes a recorded departure is always sampled (the
  first such reply has the smallest upstream delay of its group);
- any other change of TSval or TSecr is sampled at most once per 4 ms, which
  keeps the downstream estimate of flows that only receive.

`TCPDELAY_SAMPLE_INTERVAL_NS` in `src/tcpdelay/record.h` sets the interval.

## Cost

`tools/testbed/bench.sh` with `ADJUST=0` (fixed shapers, same traffic), ns
per packet from the kernel's BPF statistics (`raw/cost.txt`):

| Round | Base | 4 ms | 10 ms |
| --- | --- | --- | --- |
| 1 (VM in its slow state) | 2939 | 2656 (−10%) | 2180 (−26%) |
| 2 | 2344 | 1556 (−34%) | 1201 (−49%) |

Daemon CPU ticks per loaded minute: 22/17 base, 21/14 at 4 ms, 14/13 at
10 ms. At the owner's line rate the whole filter is under 2% of one core of
this VM, so the saving is under 1% of a core.

## Accuracy and control

`tools/testbed/run.sh` with `TCP_ATTRIBUTION=1 BACKLOG=1`, fping, scored by
`queues.py` and `analyze.py` (`raw/Q*-queues.txt`, `raw/latency.txt`).

The TCP queue estimate tracks the emulated ISP's sampled backlog equally well
with and without thinning: correlation 0.87–0.95 in every loaded phase and
direction, false alarms at most 0.9%, and 89–98% of upload queues above 30 ms
detected. Thinning nudges estimates slightly up toward the true queue, as
fewer samples catch fewer dips in each 100 ms window.

Controller outcome, mean [min–max] of five rounds each (base vs. 4 ms):

| Phase | Base | 4 ms |
| --- | --- | --- |
| bidirectional p50 / p95 / p99 | 21.6 / 50.9 / 67.5 [60.1–72.2] ms | 22.1 / 54.3 / 77.0 [72.2–83.2] ms |
| bidirectional download share | 74.5% | 72.6% |
| download-only p50 / p95 / p99 | 12.6 / 29.8 / 35.7 ms | 13.6 / 32.1 / 38.7 ms |
| download-only time above 30 ms | 4.7% [3.3–5.7] | 7.2% [5.3–9.8] |
| upload-only p95 / p99 | 66.1 / 77.7 ms | 65.8 / 79.5 ms |

The upload capacity-drop phase's p99 varied 176–268 ms (base) and 179–466 ms
(4 ms) between rounds and is too noisy to compare. No run logged a warning,
an error or a dropped ring-buffer record. The first two rounds alone
suggested a larger median cost; the three extra rounds showed that part was
noise.

## Setup and artifacts

OpenWrt 25.12 x86 VM, `tools/testbed` (8 up / 40 down Mbit/s, 500 ms
buffer). Daemon `2533a1a162f8d41b…` throughout; filter objects base
`bd18ef363edea4d6…` (`65fa6a2`), 4 ms `0499bf9f043c96b8…` (identical
instructions to the shipped SDK build `3071218f0d657fe9…`, which differs
only in BTF paths), 10 ms `e9aa016b3045d40f…`. The test log kept inode 140.

The VM panicked once during the first batch, in the 32-bit kernel's
exception entry (`raw/vm1-panic-serial.txt`), the same crash seen with the
committed filter and, on VM2, with no filter loaded. The lost runs were
repeated, and every later run moved its results to disk as soon as it ended.

## Files

- `raw/cost.txt`: the cost benchmark's summary lines.
- `raw/Q<round>-<base|t4|t10>/`: each accuracy run's probe, backlog samples,
  iperf3 JSON, phase marks and daemon log (`cake-adapt.log.gz`).
- `raw/Q*-queues.txt`, `raw/latency.txt`: `queues.py` and `analyze.py`.
- `raw/vm1-panic-serial.txt`: the panic from VM1's serial console.
