# Resource use compared with cake-autorate

## Result

On the same OpenWrt 25.12 x86 VM, with the same limits, reflectors and
workload, cake-adapt used about one fifth of the CPU of
[cake-autorate](https://github.com/lynxthecat/cake-autorate) `ac75f49`, and about
one tenth of its memory.

- **CPU, default logging:** 1.10% of one core while awake, against 5.38%
  (4.9× less). With every record output enabled: 1.22% against 9.21% (7.6×).
- **The controller alone:** both run the same fping, and fping costs the same
  in both. Without it, the controller uses 0.12% against 1.82% of a core when
  idle (15×), and 0.40% against 7.64% under bidirectional load (19×).
  Most of cake-adapt's total is fping.
- **Memory:** 2 processes (the daemon and fping) with at most 0.3 MB private
  memory and 1.9 MB resident, against up to 7 processes with 2.5 MB private
  memory and 9.5 MB resident.
- **Reaction time:** from fping's reply timestamp to the DATA record, the
  median is 0.13 ms against 0.92 ms; p99 is 3.6 ms against 20.8 ms; and the
  maximum is 22 ms against 85 ms.

Both controllers made the same kind of decisions in these runs: both raised the
shapers to the 50 Mbit/s maximum under load and entered IDLE once during the
idle period. The decision logic itself is checked separately by the
[controller comparison](../controller-comparison/README.md).

## CPU by phase

CPU is the share of one core charged to each controller's cgroup v2 group,
measured from `cpu.stat`. This is the method of upstream's own
`bench_cpu.sh`. The group contains the controller, fping, and every short-lived
child it starts (`tc`, subshells, `sleep`). Values are the mean of 3 rounds,
with the range across rounds in brackets.

Default logging (upstream defaults: `debug=1`, record outputs off):

| Phase | cake-autorate | cake-adapt | Ratio |
| --- | ---: | ---: | ---: |
| Startup, first 15 s | 3.77% [3.41–4.30] | 0.91% [0.77–1.07] | 4.1× |
| Idle, pinging, 40 s | 2.60% [2.44–2.88] | 0.76% [0.74–0.79] | 3.4× |
| Download, 45 s | 4.42% [3.80–5.23] | 0.99% [0.96–1.04] | 4.5× |
| Upload, 45 s | 6.01% [4.61–8.32] | 0.93% [0.76–1.18] | 6.5× |
| Bidirectional, 45 s | 8.16% [7.80–8.77] | 1.67% [1.56–1.83] | 4.9× |
| Idle sleep, 30 s | 0.33% [0.32–0.33] | 0.07% [0.07–0.07] | 4.6× |

Every record output enabled (DATA, LOAD, REFLECTOR, SUMMARY, SHAPER; 1 round):

| Phase | cake-autorate | cake-adapt | Ratio |
| --- | ---: | ---: | ---: |
| Startup, first 15 s | 6.07% | 1.12% | 5.4× |
| Idle, pinging, 40 s | 4.66% | 1.02% | 4.6× |
| Download, 45 s | 7.38% | 1.22% | 6.0× |
| Upload, 45 s | 11.07% | 0.96% | 11.6× |
| Bidirectional, 45 s | 13.22% | 1.66% | 7.9× |
| Idle sleep, 30 s | 0.50% | 0.09% | 5.6× |

While awake, record output costs cake-autorate a further 2–5 percentage points;
cake-adapt changes by at most 0.26 points.

### Where the time goes

`setup/breakdown.sh` repeated a 40 s idle and a 45 s bidirectional phase, with
default logging, reading each process's own CPU ticks. A process's ticks
include the children it has reaped, so `tc`, `sleep` and subshell work is
charged to the process that started it.

| | fping | Controller |
| --- | ---: | ---: |
| cake-autorate, idle | 0.82% | 1.82% |
| cake-adapt, idle | 0.90% | 0.12% |
| cake-autorate, bidirectional | 1.47% | 7.64% |
| cake-adapt, bidirectional | 1.31% | 0.40% |

cake-autorate's main shell, which parses replies and runs the controller, is
most of its cost; the remaining ticks are spread across its helper shells.

## Memory and processes

Sampled every 5 s from `/proc/<pid>/statm` for every process in the group.
This kernel has no `smaps`, so "private" is resident minus file-backed shared
pages. It excludes the bash and library text that cake-autorate's processes
share, and is the closer estimate of what each controller adds. Round 1
recorded resident memory only.

| | cake-autorate | cake-adapt |
| --- | ---: | ---: |
| Processes while awake | 6–7 | 2 |
| Processes in idle sleep | 5 | 1 |
| Private memory, awake | 2.43–2.51 MB | 0.22–0.28 MB |
| Private memory, peak | 2.54 MB | 0.30 MB |
| Resident memory (sum), peak | 9.5 MB | 1.9 MB |

System-wide process starts (`processes` in `/proc/stat`, default logging) were
4.5–11.7 per second with cake-autorate, 3.4–4.4 with cake-adapt, and 3.3–4.1
with no controller. These include the harness's memory sampler, which forks
once per sampled process every 5 s, about 1.6 per second for cake-autorate's
6–7 processes and 0.8 for cake-adapt's 2. Allowing for that, cake-adapt adds
almost nothing beyond starting fping. cake-autorate adds about 0.6 per second
when idle, about 2.5 under load, and about 6.6 per second during startup.

## Reaction time

With every record enabled, both controllers write a DATA record for each
reflector reply. The record carries fping's reply timestamp and the realtime
clock at processing (`PROC_TIME_US`):

| | Records | Median | p90 | p99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| cake-autorate | 4,944 | 0.92 ms | 3.42 ms | 20.82 ms | 84.67 ms |
| cake-adapt | 4,945 | 0.13 ms | 0.52 ms | 3.58 ms | 22.14 ms |

cake-autorate takes `PROC_TIME_US` while it formats the record, after its rate
decision. cake-adapt takes it just before running the controller, whose own
work is a few microseconds. The difference is far below the measured gap.

## Method

- **System:** OpenWrt 25.12 x86 VM, 2 vCPUs (Intel i7-9750H host),
  kernel 6.12.108. SQM-created CAKE on `eth1` (`800d`) and `ifb4eth1`
  (`800e`). The VM state was recorded beforehand (`setup/vm-state-before.txt`)
  and restored afterwards.
- **Controllers:** pristine cake-autorate `ac75f49` (`cake-autorate.sh`,
  `lib.sh`, `defaults.sh`) under bash, and the cake-adapt binary from the
  0.1.10 x86 package. That binary is byte-identical to a fresh SDK build of
  HEAD `1a37598`, which has the same sources as `5f5530d`.
- **Configuration:** identical for both: 10/20/50 Mbit/s minimum/base/maximum
  in each direction, adjustment enabled, the same 30 default reflectors in
  configured order (`randomize_reflectors=0`), and otherwise defaults
  (6 pingers, 0.3 s ping interval, 200 ms achieved-rate interval, sleep after
  60 s idle). See `setup/config.*.sh` and `setup/cake-adapt.uci.*`. The
  installed cake-adapt service was stopped for the whole test, and the two
  controllers never ran at the same time.
- **Workload** (`setup/run.sh`), each bounded: 15 s startup, 40 s idle, 45 s
  download, 45 s upload, 45 s download and upload together, then idle. The
  sleep window is 70–100 s after the load stops, by which time both
  controllers had entered IDLE (confirmed in their logs). Traffic went to a
  local endpoint through the VM's NAT gateway (`ip route get` confirmed
  `eth1`). The endpoint logged every transfer it received.
- **Order:** `setup/all.sh` ran three rounds of no controller, cake-autorate
  and cake-adapt in a rotated order to spread drift, then one round of each
  controller with all records enabled.
- **Clean exits:** every run ended with an empty cgroup and no fping, daemon
  or upstream runtime directory left (`raw/*/leftover`).

`setup/analyze.py raw` reproduces `analysis.txt` from the raw files. Each run
directory holds `snaps` (cgroup and system counters, CAKE byte counters and
rates at each phase boundary), `memory`, the final `cpu.stat`, the exit
status, and the controller's own log (gzipped). `SHA256SUMS` covers the
uncompressed logs.

## Caveats

- `cpu.stat` and per-process ticks include softirq packet processing that the
  kernel happened to run while a controller process was current. This adds
  noise under load for both controllers. It is one reason the upload and
  bidirectional ranges are wide.
- The user/system split in `cpu.stat` is estimated from timer ticks, so it is
  not meaningful at around 1% CPU.
- System-wide CPU above the no-controller baseline (`analysis.txt`) is only
  useful in the idle phases. Under load, traffic through the VM's NAT path
  varied between runs by more than the controllers' cost; the baseline runs
  even moved more bytes in the download phase than the controller runs did.
- These are single-VM measurements on an x86 host. Absolute numbers on a
  router CPU will be higher; the ratios are the transferable result.
