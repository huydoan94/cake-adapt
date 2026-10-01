# 2026-09-30: end-to-end run and CPU profile after the optimization pass

## Result

On the OpenWrt 25.12 x86 VM, the optimized daemon used about half the CPU of the
pre-optimization build under bidirectional load. It was also 38–51% lower when
idle and in the latency-spike workload, and 10–16% lower under one-way load.
Its own userspace code accounts for 4–9% of its samples; the rest is mostly
waiting in `epoll` and kernel packet processing charged to the process.

Syscalls in a 45 s window fell from 20,586 to 4,593. The end-to-end run passed
every scenario below, with no leftover processes and an unchanged test-log
inode.

## Environment

- VM: OpenWrt 25.12 x86 (i686), kernel 6.12.108, `perf` 6.12.108 (`raw/tools.txt`).
- Shaping: SQM-created CAKE on `eth1` (handle `800d`) and `ifb4eth1` (`800e`),
  both 20 Mbit/s. cake-adapt used minimum/base/maximum 10/20/50 Mbit/s in both
  directions.
- Load: a local endpoint at 10.0.3.2 served downloads over HTTP
  (`wget`, port 18080) and received uploads over `nc` (port 18081). `ip route get`
  confirmed the route left through `eth1`.
- Build: the OpenWrt SDK's `i486-openwrt-linux-musl-gcc` 14.3.0 with the SDK's
  `-Os` flags plus `-g3 -gdwarf-4 -fno-omit-frame-pointer`. The test binaries
  ran from an isolated directory. The installed package was not replaced.
- Flame graphs: [FlameGraph](https://github.com/brendangregg/FlameGraph) `41fee1f`.

## CPU use

`perf record -e cpu-clock -F 999 -g` was attached to the daemon for each 40 s
workload (`scripts/profile.sh`, `scripts/profile-before.sh`). CPU time is
user + system clock ticks from `/proc/<pid>/stat` (USER_HZ = 100, so 1 tick =
10 ms).

| Workload | Before (ticks/40 s) | After (ticks/40 s) | Change |
| --- | ---: | ---: | ---: |
| Idle, probing | 21 | 13 | −38% |
| Download | 31 | 28 | −10% |
| Upload | 32 | 27 | −16% |
| Bidirectional, mean of 3 runs | 62.7 (75, 61, 52) | 32.0 (28, 28, 40) | −49% |
| Latency spikes under download | 41 | 20 | −51% |

The 3 bidirectional runs alternated builds to limit drift
(`raw/bidirectional-repeats.txt`, `scripts/repeat.sh`). Single 40 s runs vary
by several ticks, so treat the one-way rows as small improvements, not exact
figures. Resident memory stayed between 1.10 and 1.12 MB in every workload.

The "before" binary was built with the same flags from the sources preceding
the optimization series. Its exact commit was not recorded with the capture.

In the latency workload, `ping_prefix_string` ran fping through
`tests/controller/fixtures/scripted-fping.sh`. That script injects
reproducible RTT episodes while a download runs, so the controller makes
congestion decisions.

### Where the samples go

`raw/breakdown.py` groups each perf stack by its leaf frame
(`raw/breakdown.txt`):

| Workload | Samples | epoll wait | softirq packet work | qdisc dump | cake-adapt userspace |
| --- | ---: | ---: | ---: | ---: | ---: |
| Idle | 99 | 99.0% | – | 1.0% | – |
| Download | 192 | 57.8% | 29.7% | 6.2% | 5.2% |
| Upload | 231 | 59.3% | 34.6% | 1.7% | 3.9% |
| Bidirectional | 224 | 44.6% | 46.9% | 4.0% | 4.0% |
| Latency | 160 | 38.8% | 45.6% | 6.2% | 8.8% |

With `cpu-clock`, sleeping in `epoll_pwait` is still sampled. Softirq time is
packet processing that the kernel happened to run while cake-adapt was the
current task. It is not work cake-adapt requested. The daemon's own parsing,
control, and logging take 9–14 samples per 40 s, so no further userspace
optimization is likely to matter at these rates.

Flame graphs: [idle](flamegraph-idle.svg), [download](flamegraph-download.svg),
[upload](flamegraph-upload.svg), [bidirectional](flamegraph-bidirectional.svg),
[latency](flamegraph-latency.svg). The perf scripts and folded stacks are in
`raw/`.

## Syscalls

`strace -c -f` over a 45 s window, one capture after each
optimization step (`syscalls/`):

| Capture | Step | Calls |
| --- | --- | ---: |
| `strace-head.txt` | before the optimization series | 20,586 |
| `strace-cake.txt` | interface index and MTU cached instead of re-read each sample | 5,555 |
| `strace-single.txt` | one qdisc dump serves both directions | 4,765 |
| `strace-uloop.txt` | pinger output and exits driven by ustream and `uloop_process` | 4,593 |

The first step removed per-sample `open`/`statx`/`mmap2`/`munmap` and the
socket, `ioctl` and `close` calls used for name and MTU lookups. The single dump
halved `sendmsg`. The remainder is dominated by `epoll_pwait` and pinger reads.

## End-to-end run

`scripts/e2e-final.sh` exercised the production-flag build in this order
(`e2e/phases.txt`). Qdisc rates were sampled every 2 s (`e2e/samples.txt`):

1. Idle probing (15 s): both shapers held the 20 Mbit/s base.
2. Download (40 s): download rose to the 50 Mbit/s maximum within about 5 s.
   It then held 49.5–50 Mbit/s, and after the load stopped it decayed by 1%
   steps toward base.
3. Upload (30 s): upload rose to 50 Mbit/s while download continued decaying.
4. Bidirectional (30 s): both directions held 49–50 Mbit/s, then decayed
   together.
5. `SIGUSR1` exported the active log as a compressed copy. `SIGUSR2` reset the
   1,329,217-byte log to 8,484 bytes in place. The inode stayed 150.
6. The CAKE qdisc was deleted and then exactly recreated, first on `eth1`, then
   on `ifb4eth1`. Each removal suspended monitoring and stopped fping, and each
   recreation was rediscovered from `RTM_NEWQDISC`.
7. Idle (75 s): the daemon entered IDLE and stopped fping. Download traffic
   woke it, and a new fping started.
8. `SIGTERM`: exit status 0, no fping or daemon left, no zombies, inode 150.

`scripts/e2e-congestion.sh` repeated the A/B workload with scripted latency
episodes (`e2e/congestion-cake-adapt.log.gz`, `e2e/congestion-workload.phases`).
The SUMMARY records show bufferbloat detected in both directions (`*_bb`
states). The shaper was cut to the 10 Mbit/s minimum and recovered to the
50 Mbit/s maximum, all within configured bounds.

The VM was restored afterwards to its recorded state: service enabled and
running, both qdiscs at 20 Mbit/s with their original options, and the test
area and endpoint removed.
