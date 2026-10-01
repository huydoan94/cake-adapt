# Controller comparison with cake-autorate

## Result

cake-adapt makes the same controller decisions as
[cake-autorate](https://github.com/lynxthecat/cake-autorate) `ac75f49`
(3.3.0-PRERELEASE). Two recorded cake-autorate traces replay through the
cake-adapt controller with no mismatching decisions:

| Trace | Samples | Mismatches |
| --- | ---: | ---: |
| `cake-autorate-ac75f49-ab.trace`: download/upload workload through real fping | 2,402 | 0 |
| `cake-autorate-ac75f49-congestion.trace`: the same workload with scripted latency episodes | 2,301 | 0 |

Each replayed sample checks the shaper rate, the delayed-sample count, the
average delay, the bufferbloat flag, and all three compensated thresholds, in
both directions. The replay runs as a host test,
[`tests/controller/test_replay.c`](../../tests/controller/test_replay.c), so
any future divergence fails `make -C tests`.

The live side-by-side runs found three differences, all in logging and the
first decision, not in rate control. All three are fixed:

- `ff6773a`: the first decision always writes the base rate and logs SHAPER,
  as upstream's first `set_shaper_rates` does.
- `d684377`: the reflector delta EWMA updates before the first achieved-rate
  sample, because upstream's load percentage starts at 0.
- `2a20d15`: DATA records carry the pinger's own timestamp token in
  `ICMP_TIMESTAMP`.

## Method

Both controllers ran alone, one after the other, on the OpenWrt 25.12 x86 VM
against the same SQM-created CAKE qdiscs (`eth1` and `ifb4eth1`). Before each
run, `setup/run.sh` reset both qdiscs to 20 Mbit/s and confirmed that no other
controller or fping was running. Both used minimum/base/maximum 10/20/50
Mbit/s, the same 30 reflectors, and all record outputs
enabled (`setup/cake-autorate.config.primary.sh` and the equivalent UCI
section).

`setup/workload.sh` is the shared two-minute workload, every transfer bounded:
idle 10 s, download 40 s, idle 20 s, upload 30 s, idle 20 s. Traffic went to
a local endpoint so both runs saw the same path.

Live runs cannot be compared sample by sample, because reflector replies
arrive at different moments. They were compared per phase with
`setup/compare.py`: rate ranges, load conditions, bufferbloat counts, and the
distribution of rate-change ratios. The exact check is the replay.

For the replay traces, cake-autorate's DATA record was changed by one field
(`setup/cake-autorate-t_start_us.diff`). `PROC_TIME_US` carries `t_start_us`,
the time its refractory periods are compared against. With the logged
processing time, 3 decay steps land one sample late because the logged time
is not the one the decision used. `tests/controller/fixtures/extract-trace.py`
turns a log into a trace. `scripted-fping.sh`, used through
`ping_prefix_string`, reproduces the latency episodes.

## Runs

| Directory | Contents |
| --- | --- |
| `run1-live` | cake-autorate `ac75f49` and cake-adapt before the three fixes. Per-phase behavior matched; the differences listed above were found here. |
| `run2-replay-ab` | Instrumented cake-autorate (source of the ab trace) and cake-adapt with the fixes. |
| `run3-overload` | cake-autorate with 200 Mbit/s maximums to force congestion. The NAT path dropped packets instead of queueing them, so no bufferbloat was recorded; the VM NIC link also flapped once. Kept as evidence, not used for the replay. |
| `run4-replay-congestion` | Instrumented cake-autorate with scripted latency (source of the congestion trace). It covers cuts from the minimum to the maximum factor. |
| `setup` | Configurations, workloads, runner, instrumentation diff, comparison script, and the VM state recorded beforehand. |

Logs are gzipped. `SHA256SUMS` covers the uncompressed logs, and the replay
fixtures name their source logs by those hashes.

To compare a run, decompress it into a directory as `autorate.log`,
`adapt.log`, `autorate.phases` and `adapt.phases`, then run:

```sh
python3 setup/compare.py <directory>
```
