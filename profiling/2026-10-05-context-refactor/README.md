# Context refactor on the VM (2026-10-05)

**Result:** `74d9697` measures the same as `final` (`d6564a6`) in four
alternating repetitions of the controlled run. Removing and recreating each
CAKE qdisc while it runs suspends, recovers and restarts measurement as before.
The refactor was meant to change no behavior, and nothing here shows one.

## Variants

| Variant | Source | What it is |
| --- | --- | --- |
| `final` | `d6564a6` | the build the VM ran before, from `2026-10-05-gpt-work-check` |
| `refactor` | `74d9697` | settings kept in their objects, shorter argument lists |

Both run with `tcp_delay_attribution 1` and `ul_congest_ack_share 0.45`.
`artifacts.sha256` holds the binaries' and filters' hashes. The filter object
is byte-identical: the refactor did not touch `tcpdelay.bpf.c`.

## Results

Median [min-max] of four repetitions (`conf3` to `conf6`, alternating order;
`summary.md` has every phase and metric). Same testbed and harness as
[`2026-10-05-gpt-work-check`](../2026-10-05-gpt-work-check/README.md): a
2.5 / 30 Mbit/s ISP with a 500 ms buffer, CAKE overhead 44 / MPU 84 with plain
ack-filter, a 64 kbit/s UDP echo and a 100 ms fping probe.

| Variant | Mixed DL Mb/s | Mixed p95 ms | Upload-only p95 ms | Download-only DL Mb/s / p95 ms | Filter ns/packet (mixed) | Daemon CPU (mixed) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `final` | 19.7 [19.3-21.0] | 65.5 [62.1-70.2] | 71.7 [59.4-84.6] | 23.2 / 43.2 [40.6-50.5] | 2,646 | 0.4% |
| `refactor` | 20.3 [19.5-20.9] | 64.7 [64.1-65.2] | 74.1 [61.7-81.2] | 22.5 / 42.3 [40.5-58.8] | 2,587 | 0.3% |

Every run logged about 1,625 `DATA` and as many `TCP_QUEUE` records, 342 to
366 `SHAPER` changes, and no warning. Every daemon exited cleanly.

## Lifecycle

[`tools/lifecycle.sh`](tools/lifecycle.sh) ran the `refactor` build on the
same testbed under upload load and changed the qdiscs it controls
(`raw/lifecycle/`, times in `phases`):

- upload CAKE deleted: "CAKE removed … monitoring suspended", TCP measurement
  closed; recreated: CAKE, traffic, latency and TCP measurement all restarted;
- download CAKE deleted and recreated: the same, without reopening the TCP
  capture, which watches upload;
- upload bandwidth changed outside the daemon (to 2,000 kbit/s): the
  controller wrote its own rate back;
- SIGTERM: "received signal 15; shutting down", then "Stopped".

`daemon-exit` reads 143 because the script started the daemon through a shell
function and the signal ended that wrapper; the daemon's own log shows the
clean shutdown. The test-log inode was unchanged, and no test process or
namespace remained afterwards.

## Limits

One x86 VM and four repetitions of a short scripted workload. No arm64/Filogic
run, no IDLE/sleep cycle, no IRTT or fping-ts session, no real link.

## Method

[`tools/run-matrix.sh`](../2026-10-05-gpt-work-check/tools/run-matrix.sh),
[`rep.sh`](../2026-10-05-gpt-work-check/tools/rep.sh) and the patched
[`analyze.py`](../2026-10-05-gpt-work-check/tools/analyze.py) are unchanged
from the earlier check. [`tools/summarize.py.diff`](tools/summarize.py.diff)
only adds the `refactor` variant. `conf3-6-analysis.json` is the analyzer
output. `raw/confN/<variant>/` has each run's daemon log (gzipped), iperf JSON,
probes, phases, qdisc state, sampler output and BPF fdinfo. Packet captures
were used only for the ACK cross-check, which passed, and are kept as SHA-256.
`raw/conf6-batch/` has the last batch's state snapshots.

Afterwards the VM's installed service was running again, unchanged. It still
runs the `final` package.
