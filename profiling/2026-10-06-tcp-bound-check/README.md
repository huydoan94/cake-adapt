# Controlled check of the TCP queue bound (2026-10-06)

## Result

The fping queue bound (`cb348e4`) controls the same as the build before it
(`283c635`), within run-to-run noise:
- three alternating repetitions on the x86 VM's emulated bloated line;
- the same throughput in every phase;
- equal or lower added-delay p95: 61.8 ms against 70.0 ms with both directions
  loaded, and 47.6 ms against 60.9 ms for the UDP probe under download.

With both directions loaded, time above 30 ms was 58% against 55%, with
overlapping ranges.

Both builds measure queues in the loaded direction and keep the other
direction near zero. The bound cut the largest download estimates under load
from 45-90 ms to 33-46 ms.

The drift fix itself is covered by the unit tests in `4f5cedc` and by the
router run in [`2026-10-06-router-libreqos`](../2026-10-06-router-libreqos/README.md).
This check only shows that the bound did not cost control on a line where the
TCP estimates matter.

## Builds

| Name | Commit | Binary SHA-256 | Filter SHA-256 |
| --- | --- | --- | --- |
| `prev` | `283c635` (0.3.2, HOLD/FOLLOW floors) | `d3d7d8e9…` | `e4ea902d…` |
| `fix2` | `cb348e4` (fping bound over the current and previous second) | `5b3c5843…` | `90663df0…` |

Both are x86 SDK builds of those commits; full hashes
are in [`artifacts.sha256`](artifacts.sha256) and commits in
[`sources.txt`](sources.txt). `fix2` does not include `3455e22`, which only
moves the filter load ahead of the reflector timers at startup.

## Method

The harness, testbed, phases and configuration are those of
[`2026-10-05-gpt-work-check`](../2026-10-05-gpt-work-check/README.md):
- the line: 2.5 / 30 Mbit/s ISP with a 500 ms buffer;
- rates of 1,000 / 2,250 / 3,000 kbit/s upload and 5,000 / 27,000 / 33,000 kbit/s
  download (minimum / base / maximum);
- `tcp_delay_attribution 1` and `ul_congest_ack_share 0.45` (`on` variants);
- phases: idle, upload, quiet, download, quiet, mixed, recovery.

Each repetition ran with that directory's `tools/rep.sh`:

```sh
sh ../2026-10-05-gpt-work-check/tools/rep.sh rep1 prev:prev:on fix2:fix2:on
sh ../2026-10-05-gpt-work-check/tools/rep.sh rep2 fix2:fix2:on prev:prev:on
sh ../2026-10-05-gpt-work-check/tools/rep.sh rep3 prev:prev:on fix2:fix2:on
```

The analyzer is `2026-10-05-ack-control/tools/analyze.py` with
`2026-10-05-gpt-work-check/tools/analyze.py.diff` applied. Its ACK cross-check
against the packet captures passed for every run. The captures are replaced
by their SHA-256 in [`raw/pcap.sha256`](raw/pcap.sha256).
[`tools/summarize.py`](tools/summarize.py) is that directory's summarizer with
these variant names. [`tools/queues.py`](tools/queues.py) gives the TCP
estimates per phase.

Outputs:
- [`summary.md`](summary.md): median [min-max] over the three repetitions;
- `rep*-analysis.json`: the analyzer's per-run results;
- [`queues.txt`](queues.txt): valid samples, p50 / p95 / max (ms) per phase;
- `raw/repN/<variant>/`: each run's gzipped daemon log, iperf JSON, probes,
  phase marks, qdiscs, sampler output and BPF fdinfo.

## Summary (median of three)

| Phase | Metric | `prev` | `fix2` |
| --- | --- | ---: | ---: |
| upload | upload Mbit/s | 2.1 | 2.1 |
| upload | added p95 ms | 67.1 | 64.9 |
| download | download Mbit/s | 22.4 | 22.9 |
| download | added p95 ms | 46.5 | 44.2 |
| download | UDP probe p95 ms | 60.9 | 47.6 |
| mixed | upload / download Mbit/s | 1.5 / 20.0 | 1.6 / 20.7 |
| mixed | added p95 ms | 70.0 | 61.8 |
| mixed | time above 30 ms | 54.8% | 58.2% |
| recovery | added p95 ms | 3.7 | 3.9 |

Filter time per packet and daemon CPU are equal within noise.

## TCP estimates

In every run the loaded direction's p95 was 26-77 ms and the idle direction's
was at most 1.2 ms. Maximums under load, across the three repetitions:

| Phase | Direction | `prev` | `fix2` |
| --- | --- | ---: | ---: |
| download | down | 45.6-89.6 | 32.8-40.3 |
| mixed | down | 44.3-86.6 | 42.4-45.8 |
| mixed | up | 63.5-83.4 | 60.5-73.6 |

The three-second quiet gaps and the recovery phase hold a few samples (4-22)
from flows still draining the 500 ms ISP buffer. Some read 45-58 ms in both
builds. A queue that is still draining is real delay, and fping is
still elevated then too.

## VM state

- Test log `/tmp/sqm-mon-test.log`: inode 149 before and after every
  repetition.
- Every daemon exited with status 0 and no namespaces were left.
- The installed `05eb6b3` service was stopped for each batch and restarted
  afterwards, with its own fping.
- `/lib/bpf/cake-adapt-tcpdelay.o` was restored to the installed package's
  filter (`3f1ca1de…`) after each batch. Before this check it was also
  restored from an earlier interrupted run, which had left the `prev` filter
  in place.

Collected by Claude directly at the user's request, outside the designated
agents of COLLABORATION.md.
