# Checking the 2026-10-05 TCP-delay work (2026-10-05)

**Result:** the controller guard added in `0321661` (TCP queue shares are used
only when they agree with the delivery heuristic) causes the regression that
[`2026-10-05-ack-control`](../2026-10-05-ack-control/README.md) reported.
With it, the TCP extensions perform like having them off under upload and
mixed load. Removing only that guard from today's code (`noguard`) restores
the earlier results. The other changes of that day (ACK accounting in CAKE
units, the standing-queue floor hold, the tuple-lifetime stream) measured
neutral here: no gain and no loss, except a higher filter cost.

GPT's single-run numbers reproduce within this run-to-run spread.

## Variants

All builds come from their exact commits with the SDK's own x86 compile
command (`sources.txt`, `artifacts.sha256`). Every filter loaded with JIT.

| Variant | Source | Extensions | What it is |
| --- | --- | --- | --- |
| `off` | `c103528` | off | upstream-style controller, no TCP measurement |
| `before` | `518df81` | on | GPT's "before" |
| `after` | `bbca799` | on | GPT's "after": guard + ACK accounting + floor hold |
| `head` | `c103528` | on | today's code: `after` + tuple-lifetime stream |
| `noguard` | `c103528` minus [`tools/noguard-reverts.patch`](tools/noguard-reverts.patch) | on | `head` with only the controller guard reverted |

"Extensions on" is `tcp_delay_attribution 1` and `ul_congest_ack_share 0.45`.

## Results

Median [min-max] of three repetitions, each in a different variant order
(`summary.md` has every phase and metric). Testbed: 2.5 / 30 Mbit/s ISP with
a 500 ms buffer, 10 ms each way, CAKE ethernet overhead 44 / MPU 84 with plain
ack-filter, plus a 64 kbit/s UDP echo and a 100 ms fping probe.

| Variant | Mixed DL Mb/s | Mixed p95 ms | Mixed > 30 ms | Upload-only p95 ms | Download-only DL Mb/s / p95 ms | Filter ns/packet (download) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `off` | 12.0 [9.4-13.0] | 93.4 [92.0-97.8] | 60.5% | 102.4 [90.8-108.0] | 17.9 / 67.1 | - |
| `before` | **20.4** [20.2-21.2] | **63.4** [57.5-78.8] | 53.4% | **65.1** [64.4-74.1] | **22.3** / 45.3 | 2,341 |
| `after` | 11.4 [11.3-12.6] | 91.0 [90.0-93.5] | 65.9% | 101.0 [96.5-104.0] | 17.7 / 40.0 | 2,689 |
| `head` | 11.8 [10.7-12.2] | 92.3 [91.8-93.7] | 61.4% | 96.7 [89.8-106.3] | 17.9 / 37.8 | 3,494 |
| `noguard` | **20.4** [20.0-20.7] | **64.6** [59.6-72.7] | 53.2% | **63.4** [60.3-124.1] | **22.7** / 45.2 | 3,850 |

Mixed upload goodput is 1.5-1.6 Mb/s with the extensions working (`before`,
`noguard`) and 1.8 Mb/s for `off`, `after` and `head`, which leave the upload
queue uncut. Recovery p95 was 3-6 ms for every variant.

### Compared with GPT's single run

| Variant | GPT mixed DL / p95 | This check (median) |
| --- | ---: | ---: |
| `off` | 15.5 / 91.4 | 12.0 / 93.4 |
| `before` | 20.2 / 65.2 | 20.4 / 63.4 |
| `after` | 11.0 / 93.9 | 11.4 / 91.0 |

## What this shows

- **The guard is the regression.** `after` and `head` match `off` on upload
  and mixed load; `noguard` matches `before`. The guard drops the TCP split
  whenever it disagrees with the delivery heuristic, and with upload
  bufferbloat while download is busy it always disagrees, so download gets
  cut and the upload queue stays (upload-only p50 rises from about 8 to
  46 ms). Its only gain is download-only p95 (45 to 38-40 ms), bought with 21%
  less download throughput.
- **The other changes are neutral in this workload.** `noguard` contains the
  ACK accounting, the floor hold and the lifetime stream and measures the same
  as `before`. The workload is short, so it does not exercise the cases those
  changes target (queues lasting over a minute, receiver delay, tuple reuse,
  small-packet upload); it shows they do no harm here, not that they help.
- **The lifetime stream costs filter time, not memory.** Per packet the filter
  went from 2,341 ns (`before`) to 2,689 ns with ACK accounting and
  3,494-3,850 ns with the lifetime stream, +49-64% over `before`, all JIT
  compiled. Total map memory is unchanged at about 1.05 MB (the earlier +108%
  belonged to the reverted design). Daemon CPU stayed at 0.2-0.4% of a core.

## After the reverts

The guard (`d6564a6`) and the tuple-lifetime stream (`5458100`) were then
reverted. Two more repetitions compared the result, `final` (`d6564a6`), with
`before` and `off`, in alternating order (`confirm-summary.md`,
`conf1-2-analysis.json`, `raw/conf1-2/`):

| Variant | Mixed DL Mb/s | Mixed p95 ms | Upload-only p95 ms | Download-only DL Mb/s / p95 ms | Filter ns/packet (download) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `off` | 11.4 | 93.5 | 108.0 | 18.4 / 67.8 | - |
| `before` | 20.1 | 67.5 | 72.6 | 22.9 / 41.8 | 2,951 |
| `final` | **21.1** | **59.7** | **69.9** | **23.0** / 46.0 | 2,858 |

`final` matches `before` within the run-to-run spread, keeps the ACK
accounting and floor hold, and its filter cost is back to the level before the
lifetime stream. The VM's installed package was replaced with the `final` x86
package (`cake-adapt-0.2.11-r1.apk`, SHA-256
`687322c55e7c2e9ffecd1fc556c9f7120c7b316477636c865829214d8546838b`).

## Limits

One x86 VM, three repetitions of a short scripted workload; ranges, not
statistics. No arm64/Filogic run, no real link, no long-running queue, no
tuple-reuse or receiver-delay scenario.

## Method and reproduction

[`tools/run-matrix.sh`](tools/run-matrix.sh) is GPT's
[`run.sh`](../2026-10-05-ack-control/tools/run.sh) with the same testbed,
phases, CAKE/ISP settings, probes and sampler, changed only to take variants
as `name:build:on|off`, stop the installed service for the batch, swap the
filter per variant (restored afterwards) and record state snapshots without
failing on them; the VM is test-only. `tools/sample.c.diff` raises the
deadline cap from 320 to 900 s for a five-variant batch;
`tools/analyze.py.diff` selects the raw or CAKE-charged ACK-byte cross-check by
build rather than by variant name. [`tools/rep.sh`](tools/rep.sh) runs one
repetition over SSH; [`tools/summarize.py`](tools/summarize.py) aggregates the
analyzer's JSON (`rep1-3-analysis.json`).

`raw/repN/<variant>/` holds each run's daemon log (gzipped), iperf JSON,
probes, phase marks, qdisc state, sampler output and BPF fdinfo. Packet
captures were used only for the analyzer's ACK cross-check, which passed for
every run, and are replaced by their SHA-256.

Collected by Claude directly at the user's request, outside the designated
agents of COLLABORATION.md.
