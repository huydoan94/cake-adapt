# The TCP filter's own cost, and what reducing it can and cannot buy (2026-10-03)

**Result:** the TCP-timestamp socket filter costs about 1.5–1.9 µs per packet
on the x86 test VM (a 32-bit kernel with the x86-32 BPF JIT under
VirtualBox), of which about 130 ns is the kernel's cost of running any filter.
Of the filter's own time, about 45% is flow-state handling, about 45% the
departures map (one insert per new outgoing TSval, one lookup per changed
reply), and about 10% the ring buffer. Every change that leaves the
measurement unchanged was worth at most a few percent:

| Change | Effect | Kept |
| --- | --- | --- |
| One per-CPU counter entry instead of six, unread counters removed | 462 → 374 BPF instructions; per-packet time within noise | yes (`86441ea`) |
| Counters looked up only by outgoing packets | −1.2% over 5 rounds, inside the ±3% spread | yes |
| Clock read only when a departure is recorded | within noise | no |
| Per-CPU LRU lists (`BPF_F_NO_COMMON_LRU`) on both hash maps | **+6%, slower** | no |

The only large lever left is thinning the samples: recording a departure and
emitting a record at most once per N ms per flow instead of on every TSval.
In this testbed most packets carry a new TSval (Linux ticks every 1 ms and
each flow sends under one packet per ms), so the "once per TSval" limit
barely limits. Thinning changes what the estimator sees, so it needs the
queue validation below redone before it can ship; it is not done.

For scale: at the owner's 70 Mbit/s download (about 8,700 packets/s both
ways) 1.9 µs per packet is about 1.7% of one core of this VM.

## Method

`tools/testbed/bench.sh` runs cake-adapt in the testbed's `cpe` namespace
with `tcp_delay_attribution` and `upload_ack_share_min 0.45`, installs the
filter object under test, warms up 10 s, then applies 60 s of 4 downloads and
1 upload. With `kernel.bpf_stats_enabled=1` the kernel keeps the filter's
`run_cnt` and `run_time_ns`, which `bench.sh` reads from the daemon's
`/proc/<pid>/fdinfo` before and after the loaded minute; it also reports the
daemon's CPU ticks (10 ms each). `ADJUST=0` holds both shapers at their base
rate so variants see the same traffic. Variants alternate within each round
because the VM's speed drifts between batches (the last batch below ran
about twice as slow as the first, with a tight spread inside it).

Bisection variants stop the program early at successive points with a
condition the verifier cannot fold, so later code is kept but skipped.

All runs used the same daemon build within a comparison; the test log kept
inode 140 (VM1) throughout, and `/lib/bpf` was removed after each batch.

## Results

ns per packet, one value per 60 s run. Raw lines for every run are in
`raw/benchmarks.txt`.

**Kernel overhead vs. the filter** (16:50, adjusting shapers): a filter that
returns at once costs 122, 144, 130 ns; the full filter 1921, 1891, 1813 ns.

**Where the time goes** (16:58, fixed shapers, 2 rounds):

| Filter stops after… | Run 1 | Run 2 | Added |
| --- | --- | --- | --- |
| counter lookup and byte count | 297 | 279 | +160 over an empty filter |
| IP and TCP header loads | 466 | 505 | +200 |
| options load and timestamp search | 578 | 579 | +95 |
| full (flow state, departures, ring buffer) | 1749 | 1770 | +1180 |
| full, clock read only when needed | 1672 | 1783 | none measurable |

**Counter consolidation** (`86441ea`, 16:28, 4 alternating rounds): before
1916, 1872, 1982, 1932; after 1849, 1781, 2222, 1785. The first attempt was
rejected by the 6.12 verifier (`EACCES`: clang spilled an undefined register
after a helper call); initializing two locals fixed it, and the daemon
reported degraded TCP measurement and kept running as designed.

**The C refactor** (`5f1baf6`…`bf6e521`, 18:12, 4 rounds): filter 2057, 1978,
2285, 2220 before and 1852, 2610, 2347, 2378 after, with identical BPF
instructions, so this spread is the VM's noise; daemon 25, 25, 27, 32 ticks
before and 27, 38, 33, 32 after, 0.4–0.6% of one core either way. Host
microbenchmarks of the same two trees: fping line parsing 167–171 ns before,
167–168 after; `controller_update` 95–97 ns before, 93–99 after, with
identical decisions.

**Final variants** (20:23, fixed shapers): first batch, 3 rounds, of which the
third was disturbed:

| Variant | Round 1 | Round 2 | Round 3 |
| --- | --- | --- | --- |
| base (committed filter) | 1890 | 1526 | 1398 |
| outgoing-only counters | 1701 | 1462 | 2009 |
| stops after flow state | 695 | 752 | 1293 |
| per-CPU LRU | 1587 | 1441 | 3183 |

Second batch, 5 rounds, tight spread:

| Variant | Mean | Runs |
| --- | --- | --- |
| base | 2900 | 2829, 2952, 2939, 2855, 2926 |
| outgoing-only counters | 2864 | 2839, 2848, 2886, 2799, 2949 |
| per-CPU LRU | 3086 | 3129, 2993, 3142, 2958, 3212 |
| outgoing-only counters, no ring buffer | 2571 | 2658, 2539, 2505, 2540, 2614 |

## The kept filter still measures correctly

One `tools/testbed/run.sh` pass (`TCP_ATTRIBUTION=1 BACKLOG=1`, fping) with
the final filter, scored by `queues.py` against the emulated ISP's sampled
backlog (`raw/valid-final-queues.txt`): the TCP queue estimate correlates
0.89–0.92 with the real queue in every loaded phase and direction (0.85–0.95
in [`2026-10-02-tcp-queue-split`](../2026-10-02-tcp-queue-split/README.md)),
detects 90–94% of queues above 30 ms, and raises false alarms on at most 1.0%
of samples, against 22–24% for fping's RTT/2 in the same run. Bidirectional
median added RTT was 19.6 ms with download at 72% of capacity
(`raw/valid-final-latency.txt`).

## The VM panics are not the filter

During this work the test VMs panicked twice in the 32-bit kernel's
exception-entry code (`<ENTRY_TRAMPOLINE>`). The second time
(`raw/vm2-panic-serial.txt`) the VM was running a load-only phase with no
cake-adapt filter loaded and an installed cake-adapt without TCP measurement,
so the filter is not involved. The owner's router is arm64 and does not use
this entry path.

## Artifacts

x86 builds from the OpenWrt 25.12 SDK:

| | `cake-adapt` | `cake-adapt-tcpdelay.o` |
| --- | --- | --- |
| before consolidation (`9128a3d`) | `0b79fccf80c555c1…` | `ee8c275074df8ae5…` |
| consolidation (`86441ea`) | `43f0191ece10b658…` | `655a1751d849b43e…` |
| after the C refactor | `4168f02e18780794…` | `4a5d8748b95b5bf5…` |
| final (this change) | `2533a1a162f8d41b…` | `bd18ef363edea4d6…` |

## Files

- `raw/benchmarks.txt`: every benchmark's summary line (run count, ns per
  run, daemon ticks), including the matrix runs of the panic investigation
  and the runs whose filter did not load.
- `raw/valid-final/`: the validation run (probe, backlog samples, iperf3
  JSON, phase marks, daemon log as `cake-adapt.log.gz`).
- `raw/valid-final-{queues,latency,shapers}.txt`: `queues.py`, `analyze.py`
  and `shapers.py` output for it.
- `raw/vm2-panic-serial.txt`: the second panic from VM2's serial console.

## Reproduce

On the VM with the testbed scripts and builds in `/tmp/cake-adapt-test/`:

```sh
./testbed.sh up
sysctl -w kernel.bpf_stats_enabled=1
ADJUST=0 sh ./bench.sh E1-final ./final/cake-adapt ./final/cake-adapt-tcpdelay.o
TCP_ATTRIBUTION=1 BACKLOG=1 sh ./run.sh valid-final ./final/cake-adapt fping 30 15 60
sysctl -w kernel.bpf_stats_enabled=0
./testbed.sh down
```
