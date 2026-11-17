# Waking the log writer only when needed (2026-10-22)

[Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-10-22-log-writer-wake/index.html): the tables and every flame graph on one page.

## Result

The 0.3.21 main loop woke the log writer thread for every record. Waking it
only for syslog entries, export and reset requests, lines written out at once
(no buffer timeout, or a terminal) and a half-full queue cuts the daemon's CPU
by a fifth to a third: 20 against 31 ticks per 40 s on a quiet link, 22-34
against 35-48 under load. With the native pinger the whole daemon now costs
less than 0.3.15 and its fping process together under every load, and 4
ticks more on a quiet link, where the daemon itself sends the probes. strace
over 40 s of bidirectional load counts 79 `futex` calls against 4,874, and
7,396 system calls in all against 12,135.

## Builds

| Name | Source | Binary SHA-256 prefix |
| --- | --- | --- |
| `vfix-native` | `2802243` plus the change (working tree, `git stash create` `bcdddae`) | `3834e7fd48aaa545` |
| `v0321-native` | `2802243` (0.3.21), as in [`2026-10-21-profile`](../2026-10-21-profile/README.md) | `2f95442580e10d25` |
| `v0315` | `661f385` (0.3.15), as there | `4a6762a8469ba5f6` |

Debug builds from the x86 SDK's compile command, as in the 2026-10-21 profile
(its `tools/build-variant.sh`); the native pinger for both 0.3.21 builds,
fping for 0.3.15.

## Method

The 2026-10-21 profile's method and testbed on the x86 VM `192.168.56.2`, with
[`tools/prof.sh`](tools/prof.sh) (adds the `vfix-native` variant) and
[`tools/run-fix.sh`](tools/run-fix.sh): three CPU passes per build in a
rotating order, then one perf and one strace pass of the fix, seven 40-s
workloads per pass. "Idle" is a quiet link before the daemon sleeps (60 s),
so the pinger still runs; a sleeping daemon was not measured.

Runs 01-08 ran from 09:50 to 10:30. A copy error deleted runs 09-11 on the VM
before they were kept; they were rerun from 10:40 to 10:52, so the fix's third
CPU pass did not rotate with the others, and `raw/prof-fix/progress` lists
only the rerun. Every run kept the test-log inode and left no namespace, fping
or BPF statistics behind (`inode-*`, `leftovers`); the installed service and
filter were restored (`apk audit` clean).

## CPU

Medians of three passes, ticks of 10 ms per 40 s (40 ticks = 1% of one core);
all passes, BPF figures and memory are in [`profile-summary.md`](profile-summary.md):

| Workload | 0.3.21 native | fix, native | 0.3.15 total (daemon + fping) | 0.3.15 daemon alone |
| --- | ---: | ---: | ---: | ---: |
| idle | 31 | 20 | 16 | 5 |
| download | 45 | 32 | 35 | 10 |
| upload | 35 | 22 | 25 | 9 |
| bidirectional | 41 | 33 | 40 | 15 |
| congested | 48 | 32 | 35 | 14 |
| download6 | 45 | 32 | 37 | 11 |
| bidirectional6 | 44 | 34 | 40 | 14 |

Resident memory is unchanged: 2.36 MB for both 0.3.21 builds, 2.9 MB for
0.3.15 with fping. The VM ran faster than on 2026-10-21 (0.3.21 native 41-48
under load, 55-69 then), so compare within this table.

## Where the time went

| | 0.3.21 native (2026-10-21) | fix |
| --- | ---: | ---: |
| `futex` calls, 40 s bidirectional | 4,874 | 79 |
| all system calls | 12,135 | 7,396 |
| idle perf samples: all / through `futex_wake` / task switch | 492 / 162 / 315 | 179 / 0 / 128 |
| bidirectional perf samples: same | 978 / 182 / 381 | 513 / 0 / 220 |

The 79 remaining `futex` calls are the writer's timer expiring every 500 ms.
The perf counts compare passes on different days, so they show where the
samples went rather than the size of the saving; the CPU table measures that.
Flame graphs: `flamegraphs/vfix-native-<workload>.svg`, for example
[idle](flamegraphs/vfix-native-idle.svg) and
[bidirectional](flamegraphs/vfix-native-bidirectional.svg), against the
2026-10-21 profile's `v0321-native-*.svg`; folded stacks in `folded/`,
categories in [`breakdown.txt`](breakdown.txt).

## Limits

- One 32-bit x86 VM, three CPU passes per build, one perf and one strace pass
  of the fix.
- Lines now reach the log file up to the buffer timeout later than the
  writer could have written them, the same wait as in the file buffer before
  0.3.21; a crash loses queued lines as it lost buffered ones. Host tests in
  `tests/logging/test_log.c` cover the wake rules.
