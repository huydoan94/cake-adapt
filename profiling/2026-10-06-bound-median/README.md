# TCP queue bound: median across reflectors (2026-10-06)

## Result

The TCP queue bound now ignores a single slow reflector.

**Before:** the bound was fping's largest added round-trip delay over the
current and previous second, across all replies. One reflector answering late
opened it, and the TCP upload estimate filled it, although the line had no
queue. On both of the user's routers on 2026-10-06, upload estimates of
20-72 ms each matched, to 0.1 ms, the single slowest fping reply in the
2.5 s before.

**Now:** each pinger slot keeps its own largest added delay over those two
seconds, and the bound is the lower median across the slots that replied.
- A queue on the access link delays every reflector, so the median follows it.
- A slow reflector delays only its own replies, and the median ignores it.
- A reflector whose baseline sits too high cannot hold the bound down.

The windows live in the reflectors part (`monitor/reflectors.c`), which
already receives every reply with its slot. `tcp_observe()` asks it for the
bound.

Measured on the x86 VM against 0.3.7 (`0fcd86a`, the same daemon as the
`9e8dd91` package):

- **Slow-reflector scenario:** with 10.99.0.13 answering 50 ms late for one
  second in every five, the upload estimate's maximum fell from 67.9 to
  13.5 ms. Its p99 fell from 50.9 to 9.4 ms, and samples above 20 ms from
  21 to 0.
- **Control:** eight alternating repetitions on the bloated testbed showed no
  regression. Throughput was equal, and the added-delay tails matched within
  the run-to-run ranges. Upload p95 was 65.3 ms against 72.5.

## Builds

| Name | Source | Daemon SHA-256 |
| --- | --- | --- |
| `b037` / `prev` | `0fcd86a`, 0.3.7 | `20407370…` |
| `median` / `final` | `0fcd86a` plus this change | `a0b94d48…` |

Both use the same TCP filter (`22a05d0e…`). x86 SDK builds; full hashes are in
[`artifacts.sha256`](artifacts.sha256).

## Slow-reflector scenario

[`tools/spike.sh`](tools/spike.sh) on the emulated testbed:
- ICMP from 10.99.0.13 goes through an `ets` band whose netem delay switches
  to 50 ms for one second in every five (`sch_prio` is not built on the VM);
- a 300 kbit/s upload and a 3 Mbit/s download run;
- each build runs observation-only for 100 s with `tcp_delay_attribution`.

[`tools/spike.py`](tools/spike.py) gives [`spike.txt`](spike.txt):

| | `prev` | `final` |
| --- | ---: | ---: |
| fping RTT increase from 10.99.0.13, p95 / max | 55.5 / 75.8 ms | 56.3 / 72.4 ms |
| Other reflectors, p95 | 9.6 ms | 10.4 ms |
| TCP upload estimate p95 / p99 / max | 2.2 / 50.9 / 67.9 ms | 2.8 / 9.4 / 13.5 ms |
| Upload estimates above 20 ms | 21 | 0 |
| TCP download estimate p95 / max | 3.0 / 15.9 ms | 3.5 / 8.7 ms |
| "bufferbloat attribution changed" lines | 80 | 74 |

The raw logs are in `raw/spike/`.

## Controlled runs

The same harness and testbed as
[`2026-10-06-tcp-bound-check`](../2026-10-06-tcp-bound-check/README.md), with
`tcp_delay_attribution 1` and `ul_congest_ack_share 0.45`:

```sh
sh ../2026-10-05-gpt-work-check/tools/rep.sh mrepN b037:b037:on median:median:on   # odd N
sh ../2026-10-05-gpt-work-check/tools/rep.sh mrepN median:median:on b037:b037:on   # even N
```

Eight repetitions in alternating order. [`summary.md`](summary.md) gives the
median and [range]:

| Phase | Metric | `b037` | `median` |
| --- | --- | ---: | ---: |
| upload | upload Mbit/s | 2.1 | 2.1 |
| upload | added p95 ms | 72.5 [64.0–83.2] | 65.3 [59.4–106.0] |
| download | download Mbit/s | 22.6 | 22.7 |
| download | added p95 ms | 45.2 [42.7–60.9] | 46.4 [42.6–61.1] |
| mixed | upload / download Mbit/s | 1.6 / 20.0 | 1.6 / 20.0 |
| mixed | added p95 ms | 61.8 [59.8–70.8] | 66.1 [59.6–76.2] |
| mixed | time above 30 ms | 54.9% | 54.8% |
| recovery | added p95 ms | 4.1 | 5.2 |

After four repetitions the download p95 leaned 5 ms worse. With eight, it is
equal, so that was noise. Mixed p95 leans 4 ms higher with overlapping ranges,
while its time above 30 ms is the same and its p50 lower.

`raw/mrepN/<variant>/` holds each run's daemon log (gzipped), iperf JSON,
probes, phases, qdiscs, sampler output and BPF fdinfo. The packet captures
were used only for the analyzer's ACK cross-check, which passed for every run;
they are replaced by their SHA-256 in [`raw/pcap.sha256`](raw/pcap.sha256).
The analyzer is `2026-10-05-ack-control/tools/analyze.py` with
`2026-10-05-gpt-work-check/tools/analyze.py.diff`. Its per-run output is in
`mrepN-analysis.json`, and [`tools/summarize.py`](tools/summarize.py) produced
the summary.

## VM state

- Every run exited 0. Every daemon exited 0, the test-log inode stayed 149,
  and no namespace was left.
- The filter object was restored to `3f1ca1de…` after each batch.
- The VM's root filesystem (98 MB) filled up while copying results to
  `/root` during repetition 3. The copy failed, but the run itself had
  already finished and its results were still in `/tmp`, so they were
  recovered. Later results went straight to WSL, and `/root/bound-check` was
  removed.

## Limits

- Testbed reflectors share one emulated path. On the router, reflectors differ
  by path, which is the case the median is for. The router logs motivated the
  change; they have not yet run it.
- With only one or two pingers, the lower median is the minimum. A single
  reflector's queue sample then counts fully, as before.

Collected by Claude directly at the user's request, outside the designated
agents of COLLABORATION.md.
