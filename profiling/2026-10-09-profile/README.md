# Profile of 0.3.15 against 0.3.12 (2026-10-09)

## Result

0.3.15 uses the same CPU and memory as 0.3.12: 0.15% of one core idle and
0.25–0.42% under load, 2.1 MB resident, with equal syscall counts. On IPv4
traffic the injector costs half as much per packet as in 0.3.12
(354–420 ns against 721–740 ns under download and bidirectional load), since it
now leaves at the protocol check; on IPv6 traffic its cost is unchanged. The
TCP filter costs the same. Nothing in the daemon is worth optimizing at these
rates.

## Builds

| Name | Commit | Binary SHA-256 prefix |
| --- | --- | --- |
| `head` | `661f385` (0.3.15) | `3b2f2aeafea6f3e2` |
| `base` | `0fde12a` (0.3.12) | `d29e91c9cff5e23c` |

Both were compiled from `git archive` exports with the x86 SDK's own compile
command (taken from `make package/cake-adapt/compile V=s`), adding only
`-g3 -gdwarf-4 -fno-omit-frame-pointer` so perf can resolve stacks. Each
build's TCP filter object came from the same compile; `head`'s has the same
instructions as the installed 0.3.15 package's (`llvm-objdump -d` identical).
Both ran with `tcp_delay_attribution` and `tcp_ts_request` on (0.3.12 reads
the same option name).

## Environment

- OpenWrt 25.12 x86 VM (i686, kernel 6.12.108), `192.168.56.2`.
- The emulated testbed with the line of `2026-10-05-e2e`: ISP 2.5 / 30 Mbit/s,
  CAKE with Ethernet overhead 44 and MPU 84 and plain `ack-filter` on upload,
  shaper limits 1,000 / 2,250 / 3,000 kbit/s up and 5,000 / 27,000 /
  33,000 kbit/s down, six reflectors, four active
  ([`tools/cake-adapt.template`](tools/cake-adapt.template)).
- The installed service on `eth1` was stopped during the runs and started
  again after them. The test log followed the VM rules; its inode was
  unchanged in every run, and no namespace, fping or BPF statistics setting
  was left (`raw/prof/*/inode`, `leftovers`).

## Method

[`tools/run-all.sh`](tools/run-all.sh) ran [`tools/prof.sh`](tools/prof.sh)
ten times, alternating builds: CPU passes head, base, base, head, head, base,
then one perf pass and one strace pass per build. `prof.sh` is the
2026-10-05 script with two changes: the injector's BPF statistics are read
apart from the filter's, and two IPv6 workloads were added (`download6`,
`bidirectional6`, the same iperf3 loads to `fd99::2`). Each workload lasts
40 s. CPU is user + system ticks of 10 ms; one core for 40 s is 4,000 ticks.

## CPU, memory and BPF cost

[`profile-summary.md`](profile-summary.md), from
[`tools/profsum.py`](tools/profsum.py):

| Workload | CPU ticks / 40 s, head (median) | base (median) | % of one core (head) | Filter ns/run head / base | Injector ns/run head / base | RSS head / base |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| idle | 5 / 6 / 6 (6) | 7 / 6 / 7 (7) | 0.15 | 1380 / 1223 | 684 / 1536 | 2.1 / 2.1 MB |
| download | 13 / 11 / 11 (11) | 13 / 13 / 12 (13) | 0.28 | 2754 / 2983 | 354 / 721 | 2.1 / 2.1 MB |
| upload | 16 / 10 / 10 (10) | 9 / 10 / 11 (10) | 0.25 | 4697 / 5317 | 552 / 1044 | 2.1 / 2.1 MB |
| bidirectional | 18 / 17 / 15 (17) | 17 / 15 / 15 (15) | 0.42 | 3168 / 2723 | 399 / 725 | 2.1 / 2.1 MB |
| congested | 17 / 16 / 14 (16) | 16 / 13 / 15 (15) | 0.40 | 2845 / 2789 | 420 / 740 | 2.1 / 2.1 MB |
| download6 | 14 / 14 / 12 (14) | 13 / 12 / 14 (13) | 0.35 | 2757 / 2933 | 649 / 690 | 2.1 / 2.1 MB |
| bidirectional6 | 16 / 15 / 14 (15) | 14 / 13 / 20 (14) | 0.38 | 2878 / 2782 | 730 / 678 | 2.1 / 2.1 MB |

The CPU medians differ by at most two ticks, within the spread of each
build's own runs. The filter's per-packet cost moves both ways by up to 15%,
which is the VM's run-to-run noise (its program changed little between the
builds). The injector's IPv4 saving is consistent in every workload; idle
numbers rest on about 500 runs and are noisy. Both builds see the same packet
counts.

## Where the samples go

perf `cpu-clock` at 999 Hz on each daemon, one pass per build, classified
with [`tools/breakdown.py`](tools/breakdown.py) (unchanged from 2026-10-05).
`head`:

| Workload | Samples | epoll wait | softirq packet work | qdisc dump | userspace |
| --- | ---: | ---: | ---: | ---: | ---: |
| idle | 107 | 89% | 3% | – | 7% |
| download | 213 | 46% | 11% | 11% | 29% |
| upload | 181 | 51% | 4% | 19% | 22% |
| bidirectional | 369 | 30% | 18% | 18% | 30% |
| congested | 285 | 43% | 11% | 18% | 24% |
| download6 | 168 | 45% | 9% | 9% | 30% |
| bidirectional6 | 262 | 43% | 10% | 17% | 27% |

Flame graphs: [idle](flamegraphs/flamegraph-idle.svg),
[download](flamegraphs/flamegraph-download.svg),
[upload](flamegraphs/flamegraph-upload.svg),
[bidirectional](flamegraphs/flamegraph-bidirectional.svg),
[congested](flamegraphs/flamegraph-congested.svg),
[download over IPv6](flamegraphs/flamegraph-download6.svg),
[bidirectional over IPv6](flamegraphs/flamegraph-bidirectional6.svg).
Folded stacks for both builds are in `folded/`, made with FlameGraph
`41fee1f` (`stackcollapse-perf.pl --kernel`).

In this single pass `head` had more userspace samples than `base` under IPv4
bidirectional load (109 against 59) and fewer under IPv6 bidirectional load
(70 against 95). Almost all of them are unresolved musl `libc` frames (74 of
109); no cake-adapt function stands out, and the three CPU passes show no
difference, so this is sampling noise.

## Syscalls

`strace -c -f` over 40 s of bidirectional load (`raw/prof/09-head-strace`,
`10-base-strace`): 5,804 calls for `head` and 5,887 for `base`, the same mix
as on 2026-10-05: `recvmsg`, `epoll_pwait`, `read`, `sendmsg` and `poll`,
plus 75 `bpf` calls for the counters.

## Limits

- One 32-bit x86 VM; no router and no arm64 build were profiled.
- One perf and one strace pass per build; the CPU comparison rests on three
  passes each.
