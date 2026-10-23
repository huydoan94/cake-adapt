# Profile of 0.3.26 against 0.3.25 (2026-10-22)

[Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-10-22-profile-0.3.26/index.html): the tables and every flame graph on one page.

## Result

The 0.3.26 refactor (shared helpers, local pointers, the pinger method table,
`-D`) costs nothing at run time. With the native pinger, 0.3.26 uses 21-32
CPU ticks per 40 s against 0.3.25's 22-34 (0.5-0.8% of one core of the VM),
the same within the run-to-run spread, and about 10 kB less memory (2.35 MB
against 2.36 MB). Its system calls over 40 s of bidirectional load match the
log-writer-wake build: 7,351 calls with 79 `futex` (that profile: 7,396 and 79).
With fping as the pinger, 0.3.26 costs 19-44 ticks, 12-29 of them the fping
process, and 3.1 MB.

Flame graphs of every workload, with the log writer fix's beside them, are on
the [dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-10-22-profile-0.3.26/index.html)
([`index.html`](index.html)).

## Builds

| Name | Commit | Binary SHA-256 prefix | Pinger |
| --- | --- | --- | --- |
| `v0326-native` | the 0.3.26 tree (`src/` identical to `ed9b42c`) | `f61978176c7ca4a6` | native (default) |
| `v0326-fping` | the same binary | `f61978176c7ca4a6` | `pinger_method fping` |
| `v0325-native` | `77a2517` (0.3.25) | `ea429be1dd2e24d6` | native (default) |

Both were built as on 2026-10-21, with
[`../2026-10-21-profile/tools/build-variant.sh`](../2026-10-21-profile/tools/build-variant.sh)
(the x86 SDK's compile command plus `-g3 -gdwarf-4 -fno-omit-frame-pointer`,
from `git archive` exports; base variant, no MQTT); each build's TCP filter
object came from the same compile.

## Environment and method

The x86 VM clone (`192.168.56.5`, OpenWrt 25.12, kernel 6.12.108) with the
emulated testbed and [`tools/cake-adapt.template`](tools/cake-adapt.template),
run by [`tools/run-all.sh`](tools/run-all.sh) and [`tools/prof.sh`](tools/prof.sh)
from 16:14 to 17:04: three CPU passes per variant in a rotating order, then
one perf pass and one strace pass of `v0326-native`, seven 40-s workloads per
pass. CPU counts the daemon with all its threads and its live pinger child.
The installed service was stopped meanwhile and started again. Every run kept
the test-log inode, exited cleanly and left no namespace, fping or BPF
statistics behind (`raw/prof/*/inode`, `daemon-exit`, `leftovers`).

## CPU and memory

Medians of three passes, ticks of 10 ms per 40 s (40 ticks = 1% of one core);
every pass and the BPF figures are in [`profile-summary.md`](profile-summary.md),
made by [`tools/profsum.py`](tools/profsum.py):

| Workload | 0.3.25 native | 0.3.26 native | 0.3.26 fping total | of which fping |
| --- | ---: | ---: | ---: | ---: |
| idle | 22 | 21 | 19 | 12 |
| download | 34 | 32 | 38 | 25 |
| upload | 24 | 21 | 30 | 18 |
| bidirectional | 33 | 32 | 41 | 24 |
| congested | 32 | 31 | 41 | 23 |
| download6 | 31 | 32 | 44 | 29 |
| bidirectional6 | 33 | 31 | 43 | 25 |

The first workloads of 0.3.25's first pass read high (51 idle, 62 download,
against 21-22 and 32-34 in its other passes); the medians leave them out.
Resident memory, daemon plus pinger: 2,352-2,356 kB for 0.3.26 native,
2,360-2,368 kB for 0.3.25, 3,096-3,148 kB with fping.

The TCP filter costs the same in both builds: 2.8-3.2 µs per packet under
download load, 4.9-5.9 µs under upload, and the timestamp injector
0.3-1.0 µs.

## Where the samples go

perf `cpu-clock` at 999 Hz over 40 s per workload, one pass of `v0326-native`,
graphed with FlameGraph `41fee1f` by [`tools/make-graphs.sh`](tools/make-graphs.sh)
and classified by [`tools/breakdown.py`](tools/breakdown.py)
([`breakdown.txt`](breakdown.txt)). Under bidirectional load: 480 samples
(the log writer fix: 513), 39% waiting in the event loop, 20% the daemon's
own userspace code, 13% native ICMP probes, 12% netlink qdisc dumps, 8%
packet interrupts charged to the daemon. Against the log writer fix, the
split moves by at most 5-7 points under upload, congested, bidirectional and
both IPv6 loads. Download and idle differ more (download: 500 samples
against 413, 34% against 55% waiting in the event loop, 26% against 16%
native probes; idle: 233 against 179); these are single perf passes on
different VMs and days, and the three CPU passes per build above show no
difference between 0.3.25 and 0.3.26.

## System calls

`strace -c -f` over 40 s of bidirectional load, `v0326-native`
([`raw/prof/11-v0326-native-strace/strace.txt`](raw/prof/11-v0326-native-strace/strace.txt)):
7,351 calls, 616 of them errors (536 `recvmsg`, all 79 `futex` calls, and one
`restart_syscall`). `poll` 760, `epoll_pwait` 1,967, `recvmsg` 2,514, `sendmsg` 721,
`sendto` 537 (the probes), `futex` 79.

## Limits

The VM's CPU is shared with the Windows host, so compare builds within this
profile only. Three passes find differences of a few ticks; they cannot tell
apart the ±2 ticks seen here.
