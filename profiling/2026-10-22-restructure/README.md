# The 2026-10-22 restructure against 0.3.26

[Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-10-22-restructure/index.html): the tables and every flame graph on one page.

## Result

The restructure (constants in `src/constants/`, one file per concern in
logging, the controller, the pinger and config, the ubus server thread in
`platform/ubus.c`; commits `1f9bc0b`..`7813321`, released as 0.3.27) changes
no behaviour that these runs can see:

- the lifecycle on VM .2 passes 29 of 30 checks, the same as 0.3.26: the one
  failure is the same restart by the init script while ubusd was down after a
  reload; the ubus status reply has the same fields in the same order;
- under load, the restructured build and 0.3.26 overlap round for round in
  every phase;
- CPU is the same within the run-to-run spread, memory the same, and system
  calls under bidirectional load are 7,355 with 79 `futex` (0.3.26: 7,351 and
  79).

On the host, the replay test matches as before and the closed-loop simulator's
output is byte-identical to the previous controller's in all four modes
(checked at the controller commit, `4d982b1`).

## Builds

| Name | Source | Binary SHA-256 prefix |
| --- | --- | --- |
| `v0326` | 0.3.26 before the restructure (`ed9b42c` tree) | `f61978176c7ca4a6` |
| `vrestr` | the restructured tree (`28c6c2a`, before the 0.3.27 bump) | `2923c137aaee6a68` |

Debug builds made as on 2026-10-21 with
[`../2026-10-21-profile/tools/build-variant.sh`](../2026-10-21-profile/tools/build-variant.sh).
`vrestr` reports 0.3.26-r1: it was built before the version bump, which changed
nothing else. The lifecycle used the x86 SDK packages of the same tree
(`cake-adapt-mqtt` and `luci-app-cake-adapt`, reinstalled over 0.3.26-r1).

## Lifecycle (VM .2)

[`tools/lifecycle.sh`](tools/lifecycle.sh), the 0.3.26 script with a forced
reinstall and a check that the binary changed; results in
[`lifecycle/`](lifecycle/) (`steps` is the run's output). All checks pass
except "same daemon" after the ubusd restart: at that moment procd ran the
init script, which could not reach ubus ("Failed to connect to ubus",
[`lifecycle/syslog-tail`](lifecycle/syslog-tail)) and restarted the
instance, as with 0.3.26 ([`../2026-10-22-e2e-0.3.26`](../2026-10-22-e2e-0.3.26/README.md)).
[`lifecycle/status-before`](lifecycle/status-before) (0.3.26) and
[`lifecycle/status-native`](lifecycle/status-native) (restructured) have the
same fields, order and types.

## Under load (VM .5)

[`tools/control.sh`](tools/control.sh): three rounds of `tools/testbed/run.sh`
per build, alternating which went first, native pinger, cake-autorate's delay
thresholds (10, 30, 60), 22:38-23:07; every run took 4 min 47 s. Results in
[`control/`](control/), scored by `tools/testbed/analyze.py`
([`control/analyze.txt`](control/analyze.txt)) and `shapers.py`
([`control/shapers.txt`](control/shapers.txt)).

A first attempt on VM .2 was discarded: the host stalled that VM 34 times for
76-165 s each (VirtualBox "Giving up catch-up attempt"), stretching runs from
4:47 to 28-33 minutes; .2 was rebooted and the runs repeated on .5.

Medians of three rounds; added RTT in ms, time with more than 30 ms added:

| Phase | Build | p50 | p95 | > 30 ms | p50 per round |
| --- | --- | ---: | ---: | ---: | --- |
| upload steady | 0.3.26 | 18.1 | 106.8 | 44.0% | 41.3, 11.7, 18.1 |
| | restructured | 36.7 | 101.0 | 53.4% | 36.7, 32.1, 40.0 |
| upload step | 0.3.26 | 9.6 | 148.8 | 39.9% | 9.6, 10.2, 8.8 |
| | restructured | 10.3 | 147.0 | 40.8% | 8.0, 10.3, 11.1 |
| download steady | 0.3.26 | 20.5 | 82.5 | 33.7% | 20.5, 29.8, 11.4 |
| | restructured | 21.1 | 78.0 | 37.5% | 21.1, 26.8, 17.8 |
| bidirectional | 0.3.26 | 24.8 | 106.8 | 48.3% | 34.3, 23.2, 24.8 |
| | restructured | 32.8 | 106.3 | 51.9% | 32.7, 32.8, 35.5 |

The per-round values overlap in every phase. The spread between rounds of one
build is as large as the gap between builds: 0.3.26's bidirectional p50 was
24.8 ms here against 39.1 ms on .2 earlier the same day, which also explains
that run's apparent gap to 0.3.25.

## CPU, memory and system calls (VM .5)

[`tools/run-all.sh`](tools/run-all.sh) and [`tools/prof.sh`](tools/prof.sh):
three CPU passes per build in a rotating order, then one perf and one strace
pass of the restructured build, seven 40-s workloads per pass, 21:13-21:49;
every run took 4 min 55 s, kept the test-log inode and left nothing behind.
Medians, ticks of 10 ms per 40 s ([`profile-summary.md`](profile-summary.md)):

| Workload | 0.3.26 | restructured |
| --- | ---: | ---: |
| idle | 19 | 19 |
| download | 31 | 37 |
| upload | 22 | 20 |
| bidirectional | 30 | 36 |
| congested | 34 | 35 |
| download6 | 37 | 31 |
| bidirectional6 | 29 | 31 |

The differences go both ways and lie within each build's passes (download:
0.3.26 41/30/31, restructured 50/37/32). Resident memory 2,360-2,368 kB for
both. strace over 40 s of bidirectional load: 7,355 calls, 79 `futex`
([`raw/prof/08-vrestr-native-strace/strace.txt`](raw/prof/08-vrestr-native-strace/strace.txt)).
perf under bidirectional load: 467 samples (0.3.26: 480), 41% waiting in the
event loop, 20% userspace, 15% native probes, 12% netlink dumps, 8% packet
interrupts, within two points of 0.3.26 ([`breakdown.txt`](breakdown.txt)).

## Limits

The VMs share the Windows host's CPU, which stalled .2; compare builds within
this run only. Three rounds cannot separate differences smaller than the
testbed's spread; the evidence that behaviour is unchanged is the code review,
the replay test and the byte-identical simulator, which these runs only
corroborate.
