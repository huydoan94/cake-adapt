# The restructure toward 1.0 against 0.3.30 (2026-10-27)

[Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-10-27-restructure-1.0/index.html): the tables and every flame graph on one page.

## Result

The whole-project restructure (design and as-built notes in
[`docs/superpowers/specs/2026-10-27-restructure-1.0-design.md`](../../docs/superpowers/specs/2026-10-27-restructure-1.0-design.md):
`daemon/`, `shaper.c` and `report.c`, the split `common/`, `latency/pinger.c`,
per-direction arrays and one naming rule) changes no behaviour these checks
can see:

- on the host, every suite passes, including both replay traces, and so do
  the address and undefined-behaviour sanitizer builds; the closed-loop
  simulator's output is byte-identical to 0.3.30's in all four modes and for
  every scenario's trace;
- the x86 SDK builds all three daemon packages and `luci-app-cake-adapt`;
- the lifecycle on VM .2 passes 29 of 30 checks, as 0.3.26 and the
  2026-10-22 restructure did, with the same one failure; the ubus status reply
  has the same fields in the same order and types;
- under load the two builds overlap round for round in three of the four
  phases; in the fourth, upload step, the restructured build's median added
  delay was lower in all three rounds (see below);
- memory is the same; CPU differs by a few ticks per 40 s either way, and in
  the two IPv6 workloads the restructured build's passes were all a little
  higher, while the TCP filter, whose instructions are identical in both
  builds, ran slower in the same passes; system calls under bidirectional
  load are 7,416 with 79 `futex` (0.3.30's code: 7,355 and 79).

## Builds

| Name | Source | Binary SHA-256 prefix |
| --- | --- | --- |
| `v0330` | 0.3.30 (`b4f205f`) | `3ec7e67f04f81771` |
| `vnew` | the restructured working tree, before its commit | `189c1f9989aa3aef` |

Debug builds made with [`tools/build-variant.sh`](tools/build-variant.sh), the
2026-10-21 script, which now also builds the working tree (`WORKTREE`) and
finds the repository from its new depth. Both report 0.3.30-r1. The lifecycle
used the x86 SDK packages of the restructured tree (`cake-adapt-mqtt` and
`luci-app-cake-adapt` 0.3.30-r1), installed over 0.3.26-r1.

## Host checks

`sh tools/host-test.sh` (all four suites), `sh tools/style-check.sh`, the
suites built with `-fsanitize=address,undefined` and every test binary run
under them, and the simulator built from `b4f205f` and from the working tree,
run in its four modes (`--owd`, `--attribute`, both, neither) with each
scenario's `--trace`, the 32 outputs compared byte for byte: identical. The
sanitizers report one leak, in `test_log`'s stuck-writer test, which
deliberately leaves a writer behind; 0.3.30 reports the same 131,208 bytes.

## Lifecycle (VM .2)

[`tools/lifecycle.sh`](tools/lifecycle.sh), the 2026-10-22 script with the
version check at 0.3.30; results in [`lifecycle/`](lifecycle/) (`steps` is
the run's output). The first attempt ended when the VM rebooted during the
fping fallback step, after 21 passes and no failure
([`lifecycle/first-attempt/steps`](lifecycle/first-attempt/steps)); this VM's
kernel panics about once per 25-60 minutes of load, with or without
cake-adapt. The VM was put back on 0.3.26-r1 with the attempt's settings
undone, and the check run again. It passes everything but "same daemon" after
the ubusd restart: procd ran the init script while ubusd was down ("Failed to
connect to ubus", [`lifecycle/syslog-tail`](lifecycle/syslog-tail)) and it
restarted the instance, as with 0.3.26. [`lifecycle/status-before`](lifecycle/status-before)
(0.3.26) and [`lifecycle/status-native`](lifecycle/status-native)
(restructured) have the same fields, order and types. VM .2 is left on
0.3.30-r1 with its original configuration.

## Under load (VM .5)

[`tools/control.sh`](tools/control.sh): three rounds of `tools/testbed/run.sh`
per build, alternating which went first, native pinger, cake-autorate's delay
thresholds (10, 30, 60), 16:35-17:04; every run took 4 min 47 s. Results in
[`control/`](control/), scored by `tools/testbed/analyze.py`
([`control/analyze.txt`](control/analyze.txt)) and `shapers.py`
([`control/shapers.txt`](control/shapers.txt)).

Medians of three rounds; added RTT in ms, time with more than 30 ms added:

| Phase | Build | p50 | p95 | > 30 ms | p50 per round |
| --- | --- | ---: | ---: | ---: | --- |
| upload steady | 0.3.30 | 22.6 | 107.3 | 45.9% | 22.6, 20.6, 36.8 |
| | restructured | 33.9 | 104.8 | 51.0% | 35.6, 33.9, 24.0 |
| upload step | 0.3.30 | 14.2 | 152.4 | 42.5% | 14.2, 15.2, 10.5 |
| | restructured | 7.4 | 145.2 | 38.9% | 7.5, 6.4, 7.4 |
| download steady | 0.3.30 | 18.1 | 86.4 | 29.2% | 18.1, 19.5, 16.0 |
| | restructured | 15.8 | 90.1 | 29.2% | 13.6, 16.6, 15.8 |
| bidirectional | 0.3.30 | 22.9 | 104.3 | 45.1% | 38.6, 19.1, 22.9 |
| | restructured | 24.6 | 109.8 | 47.5% | 32.6, 24.6, 24.5 |

In three phases the per-round values overlap. In the upload step the
restructured build's p50 was lower in every round (6.4-7.5 ms against
10.5-15.2), while its p95 overlapped. With three rounds a clean split of this
kind happens by chance about one time in ten, and the decision code is the
same: the replay matches and the simulator's output is byte-identical. The
2026-10-22 run, with the same code on both sides of its comparison, saw p50s of
8.0-11.1 ms in this phase. The difference is therefore not evidence of a
behaviour change; more rounds would be needed to say more.

## CPU, memory and system calls (VM .5)

[`tools/run-all.sh`](tools/run-all.sh) and [`tools/prof.sh`](tools/prof.sh):
three CPU passes per build in a rotating order (17:05-17:35, before any other
load on the host), then one perf and one strace pass of the restructured
build, seven 40-s workloads per pass. The VM stopped answering during the
first perf pass; after it came back the perf and strace passes were run again
(17:45-17:51), while an OpenWrt image build was loading the host. Every run kept
the test-log inode, exited cleanly and left nothing behind
(`raw/prof/*/inode`, `daemon-exit`, `leftovers`). Medians, ticks of 10 ms per
40 s, with each pass ([`profile-summary.md`](profile-summary.md)):

| Workload | 0.3.30 | restructured |
| --- | ---: | ---: |
| idle | 18 (18, 18, 23) | 20 (18, 20, 25) |
| download | 31 (31, 28, 35) | 32 (31, 32, 44) |
| upload | 22 (22, 22, 19) | 24 (24, 22, 24) |
| bidirectional | 30 (30, 34, 29) | 37 (37, 49, 34) |
| congested | 39 (31, 39, 63) | 32 (37, 32, 31) |
| download6 | 32 (33, 30, 32) | 37 (37, 35, 39) |
| bidirectional6 | 31 (31, 29, 31) | 36 (37, 36, 32) |

Resident memory is 2,352-2,364 kB for both. In download6 and bidirectional6
every restructured pass was higher, by about 5 ticks per 40 s (0.1% of one
core). In the same passes the TCP filter took 3,543 ns per packet against
2,901 under download6, although the two filter objects have identical
instructions (`llvm-objdump -d`; only the renamed enum names in their debug
information differ): the VM ran slower during those passes, and 0.3.30's
passes show swings of the same size (congested 31-63). The run cannot separate
a difference this small from the host.

strace over 40 s of bidirectional load: 7,416 calls, 79 `futex`
([`raw/prof/08-vnew-native-strace/strace.txt`](raw/prof/08-vnew-native-strace/strace.txt));
the same code as 0.3.30 made 7,355 and 79 on 2026-10-22. perf: the daemon's
own userspace is 9-20% of its samples across the workloads and the native
probes 15-21% ([`breakdown.txt`](breakdown.txt)), the split seen on
2026-10-22; the sample counts themselves (312-922) are not comparable, as the
host was busy.

## Limits

The VMs share the Windows host's CPU; compare builds within this run only.
Three rounds cannot separate differences smaller than the testbed's spread;
the evidence that behaviour is unchanged is the replay test and the
byte-identical simulator, which these runs only corroborate.
