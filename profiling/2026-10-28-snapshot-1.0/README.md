# 1.0.0 on the OpenWrt snapshot (x86_64), against 0.3.30 (2026-10-28)

## Result

On the new test platform, the OpenWrt snapshot on x86_64 (user decision
2026-10-27), 1.0.0 behaves as 0.3.30 did:

- the lifecycle on VM .2 passes 27 of 30 checks: the known "same daemon"
  failure after the ubusd restart, and two that follow from the snapshot's
  procd not starting `ubusd` again after it is killed; cake-adapt kept
  sampling throughout;
- under load on VM .5 the builds overlap in every phase once enough rounds are
  run: three full rounds overlap in three phases, and the fourth (upload
  steady), which leaned against 1.0.0 there and on 2026-10-27, overlaps
  completely in six focused rounds, with the emulated ISP's real queue no
  larger;
- CPU (0.47-0.68% of one core), memory (about 2,690 kB) and system calls
  (7,008, 77 `futex`) are the same within the passes' spread.

## Platform

Both VMs run the image built from `~/openwrt` (`7cd9ef1f22`, r37759,
kernel 6.18.55) with `custom/vm/build.sh`: x86_64, ext4, perf events on, the
testbed's modules and tools, perf, strace, gdb, bpftool and an SFTP server.
Each has 4 virtual CPUs under VirtualBox on Hyper-V (WSL2).

VM .2 was set up as the old .2 ([`setup/apply.out`](setup/apply.out); the
image's defaults in [`setup/uci-image-default`](setup/uci-image-default)): SQM
`piece_of_cake` on `eth1` at 20/20 Mbit/s, and cake-adapt on `eth1` adjusting
both directions between 10, 20 and 50 Mbit/s, with every record, debug, TCP
delay attribution and timestamp requests, and the old reflector list.

## Builds

| Name | Source | Binary SHA-256 prefix |
| --- | --- | --- |
| `v0330` | 0.3.30 (`b4f205f`) | `8880a36e0b4e9e16` |
| `v100` | 1.0.0 (`9352ccc`) | `5b929b6c072714d9` |

x86_64 debug builds from [`tools/build-variant.sh`](tools/build-variant.sh),
which takes the snapshot tree's own compile command
([`tools/compile-line.txt`](tools/compile-line.txt), the `cake-adapt-mqtt`
variant) and adds `-g3 -gdwarf-4 -fno-omit-frame-pointer`. The lifecycle used
the tree's `cake-adapt-mqtt` and `luci-app-cake-adapt` 1.0.0-r1 packages,
the same build as the image's.

## Lifecycle (VM .2)

[`tools/lifecycle.sh`](tools/lifecycle.sh), the 2026-10-27 script with the
version at 1.0.0; the reinstall must keep the image's binary (it did). Results
in [`lifecycle/`](lifecycle/). Two earlier attempts are in
[`lifecycle/earlier-attempts/`](lifecycle/earlier-attempts/): the VM rebooted
during the first one's upgrade step, leaving no trace (no pstore, no serial
log); in the second, after ubusd was killed, procd's children that should
start it again hung in `synchronize_rcu_normal` and `cgroup_kn_lock_live`, and
nothing could reach ubus until the VM was reset.

In the counted run, after `kill ubusd` procd did not start it again, without
hanging this time: "same daemon" fails as with 0.3.26 and 0.3.30, "ubus object
back" and "stopped" fail because nothing reaches ubus, and "still sampling"
passes. The VM was rebooted before the profile.

## Under load (VM .5)

[`tools/control.sh`](tools/control.sh): three rounds of
`tools/testbed/run.sh` per build, alternating which went first, native pinger,
cake-autorate's delay thresholds (10, 30, 60), 16:38-17:07, each run 4 min
47 s; the profile ran on VM .2 at the same time. Results in
[`control/`](control/), scored by `tools/testbed/analyze.py`
([`control/analyze.txt`](control/analyze.txt)) and `shapers.py`
([`control/shapers.txt`](control/shapers.txt)).

| Phase | Build | p50 | p95 | > 30 ms | p50 per round |
| --- | --- | ---: | ---: | ---: | --- |
| upload steady | 0.3.30 | 23.7 | 85.2 | 44.2% | 26.0, 23.5, 23.7 |
| | 1.0.0 | 36.4 | 91.4 | 55.1% | 36.4, 42.3, 35.1 |
| upload step | 0.3.30 | 28.4 | 106.1 | 48.8% | 36.8, 28.4, 19.4 |
| | 1.0.0 | 27.9 | 131.4 | 47.2% | 29.5, 15.3, 27.9 |
| download steady | 0.3.30 | 3.1 | 74.3 | 13.1% | 2.4, 3.1, 9.7 |
| | 1.0.0 | 5.1 | 78.8 | 13.6% | 5.1, 6.6, 0.6 |
| bidirectional | 0.3.30 | 42.2 | 93.2 | 58.0% | 44.8, 42.2, 32.6 |
| | 1.0.0 | 30.3 | 92.5 | 50.2% | 30.3, 29.8, 37.7 |

Added RTT in ms, medians of three rounds. Upload steady separated against
1.0.0, but inconsistently: 1.0.0 made more upload cuts and spent less time
with the shaper above the bottleneck, yet saw more delay and pushed more
upload. Both builds sampled the link every 200 ms and logged about the same
number of DATA and SHAPER records.

### Upload steady, six rounds

[`tools/upload.sh`](tools/upload.sh) and
[`tools/run-upload.sh`](tools/run-upload.sh) (`run.sh` cut after its
upload-steady phase): six rounds per build, alternating, with the emulated
ISP's queues sampled every 100 ms (`BACKLOG=1`), 17:55-18:18, nothing else
running on the host. Results in [`upload/`](upload/).

| | 0.3.30 | 1.0.0 |
| --- | ---: | ---: |
| p50, median of six | 33.8 | 30.1 |
| p50 per round | 38.1, 36.7, 33.5, 25.6, 28.4, 34.1 | 33.3, 33.5, 23.1, 19.7, 26.9, 39.6 |
| p95, median of six | 102.3 | 103.5 |
| upload used | 89.9-92.1% | 90.0-92.1% |
| bottleneck queue, median of runs | 33 kB | 27 kB |
| upload cuts per run | 65-76 | 65-75 |

The rounds overlap completely and the real queue is no larger with 1.0.0: the
earlier separation was run-to-run noise. `analyze.py` now scores a run that
stops after some phases.

## CPU, memory and system calls (VM .2)

[`tools/run-all.sh`](tools/run-all.sh) and [`tools/prof.sh`](tools/prof.sh):
three CPU passes per build in a rotating order, then one perf and one strace
pass of 1.0.0, seven 40-s workloads per pass, 16:41-17:17. Every run kept the
test-log inode, exited cleanly and left nothing behind. Medians, ticks of
10 ms per 40 s, with each pass ([`profile-summary.md`](profile-summary.md)):

| Workload | 0.3.30 | 1.0.0 |
| --- | ---: | ---: |
| idle | 19 (28, 19, 19) | 20 (33, 20, 19) |
| download | 22 (32, 22, 19) | 25 (32, 25, 25) |
| upload | 19 (23, 17, 19) | 19 (24, 19, 19) |
| bidirectional | 26 (48, 26, 25) | 26 (34, 26, 26) |
| congested | 26 (31, 26, 23) | 27 (32, 26, 27) |
| download6 | 26 (27, 25, 26) | 26 (33, 24, 26) |
| bidirectional6 | 26 (27, 25, 26) | 27 (36, 25, 27) |

Resident memory 2,684-2,696 kB for both. strace over 40 s of bidirectional
load: 7,008 calls, 77 `futex`. perf ([`breakdown.txt`](breakdown.txt), flame
graphs in [`flamegraphs/`](flamegraphs/)): under bidirectional load the daemon
waits in its event loop 47% of its samples, runs its own userspace 15%, the
native probes 13% and the netlink dump 11%. `breakdown.py` now also knows the
x86_64 syscall names.

## Limits

The two VMs share the Windows host, and the profile on .2 ran during the last
two load rounds on .5; compare builds within a run only. Three rounds cannot
separate differences smaller than the testbed's spread, as the upload-steady
phase showed; the evidence that behaviour is unchanged remains the replay test
and the byte-identical simulator (2026-10-27), which these runs corroborate on
the new platform.
