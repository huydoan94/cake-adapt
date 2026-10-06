# End-to-end test and profile after the refactors (2026-10-05)

## Result

Every scenario passed on the x86 VM, for today's code and for `d6564a6`, the
build from before the refactors. Both builds pass the same 21 automated
checks, with the same throughput.

The run found one bug, fixed in `05eb6b3`. A daemon killed with SIGKILL left
its fping running under init, and procd's respawn then started a second one.
The fix was verified on the VM.

Profiling shows no difference between the builds. Under load the daemon uses
about 0.25-0.4% of one core and 2.1 MB of memory. A 30-minute soak showed no
growth in memory, file descriptors or mappings.

Not covered: the arm64 VM (Filogic build) and the x86 clone were powered off
and could not be started from here. IRTT is not installed on the VM.

## Builds

| Name | Commit | Used for |
| --- | --- | --- |
| `head` (e2e) | `dbd5386` | e2e, variants, the first service test |
| `head` (profile) | `05eb6b3` | profiling, soak, the service test after the fix |
| `base` | `d6564a6` | the same e2e and profiling, for comparison |

All three were built with the SDK's compile command plus
`-g3 -gdwarf-4 -fno-omit-frame-pointer`, so perf could resolve stacks. The
service test used the SDK packages instead. The TCP filter source is the same
in all three.

## Environment

- OpenWrt 25.12 x86 VM (i686, kernel 6.12.108), `192.168.56.2`.
- All load ran on the emulated testbed (`tools/testbed/testbed.sh`), inside the
  VM, with the same line as the recent controlled runs:
  - ISP: 2.5 / 30 Mbit/s with a 500 ms buffer;
  - CAKE: ethernet overhead 44 and MPU 84, with plain `ack-filter` on upload;
  - shaper limits (minimum / base / maximum, kbit/s): 1,000 / 2,250 / 3,000 upload and 5,000 / 27,000 / 33,000 download;
  - reflectors: six, at `10.99.0.11` to `.16`, four of them active.
- `tcp_delay_attribution` and `ul_congest_ack_share 0.45` were on throughout.
  [`tools/cake-adapt.template`](tools/cake-adapt.template) has the full
  configuration.
- The installed service on `eth1` was stopped during profiling.
- The test log followed the VM rules. Its inode stayed 288 in every run, and
  no namespace, fping or iperf3 was left afterwards.

## End-to-end scenarios

[`tools/e2e.sh`](tools/e2e.sh) ran these phases in order. Timestamps are in
`e2e/<build>/phases`; shaper rates, CPU, RSS, file descriptors and child
processes were sampled every second (`samples`). The independent 100 ms probe
is in `probe.gz`.

| Phase | What happened (`head`) |
| --- | --- |
| startup, 12 s | Both CAKEs discovered, TCP measurement started, four pingers |
| download, 25 s | Download shaper 26-33 Mbit/s, 26.2 Mbit/s goodput; probe added p95 43 ms |
| upload, 20 s | Upload rose to the 3,000 kbit/s maximum and was cut on congestion; 2.2 Mbit/s goodput |
| bidirectional, 25 s | 22.9 / 1.5 Mbit/s; upload cut down to 1,629 kbit/s |
| capacity drop, 30 s | ISP upload 2.5 → 1.5 → 2.5 Mbit/s under upload; the shaper fell to the 1,000 kbit/s minimum, then recovered |
| log export, reset | SIGUSR1 wrote a compressed export; SIGUSR2 reset the 31.8 kB log to 11.2 kB in place |
| CAKE removal | Upload, then download CAKE deleted and recreated. Each suspended monitoring and restarted it on rediscovery; TCP measurement reopened for upload |
| external rate change | Upload CAKE set to 2,000 kbit/s outside the daemon; corrected within 5 s |
| IFB recreated | `ifb4cwan` deleted and recreated with a new index: found again, download recovered (25.1 Mbit/s) |
| one reflector down | `10.99.0.11` replaced by `10.99.0.15` within 3 s |
| all reflectors down, 20 s | STALL, a global timeout after 10 s, minimum rates enforced (1,000 / 5,000), pingers restarted. Afterwards, reflectors that had collected offences during the outage were replaced |
| idle, 35 s | IDLE after the 20 s threshold, minimum rates enforced, fping stopped |
| wake | A download woke it, and a new fping started |
| shutdown | SIGTERM, exit 0, no child or zombie left |

[`tools/check.py`](tools/check.py) verifies these from the logs:
[`e2e/head-checks.txt`](e2e/head-checks.txt) and
[`e2e/base-checks.txt`](e2e/base-checks.txt), 21 PASS each. The only warnings
are the expected ones while a qdisc or the IFB is missing.

Other runs:

- `e2e/head-rotation`: the same run with a 500 kB log limit. The log rotated
  twice; the export and reset still worked, and the inode was unchanged.
- `e2e/head-fping-ts` ([`tools/variants.sh`](tools/variants.sh)): fping-ts
  reports one-way delays, for example 52 ms down and 11 ms up while download
  was congested. It detected download bufferbloat, and exited cleanly.
- `e2e/head-no-filter`: with `/lib/bpf/cake-adapt-tcpdelay.o` missing, TCP
  measurement reports "degraded" once and is not retried. Control carried on:
  477 DATA records and 162 shaper changes.

### Service and package lifecycle

[`tools/service.sh`](tools/service.sh) ran on the VM's real service on `eth1`
(`e2e/service.txt`):

- the package installs over the old one and keeps `/etc/config/cake-adapt`;
- start, restart and reload (after a config change) each replace the daemon
  and its fping;
- an invalid configuration is reported through syslog and does not start;
- a disabled configuration logs "disabled by configuration" and does not
  start;
- stop leaves no daemon.

**Bug found:** after `kill -9`, procd respawned the daemon, but the old fping
kept running under init. uloop ignores SIGPIPE, and an ignored signal
survives `exec`, so the orphan ignored its failed writes; its mask read
`SigIgn: 0x1000`. `05eb6b3` restores SIGPIPE's default when spawning
pingers. On the VM the new fping shows `SigIgn: 0`, and after `kill -9` the
old fping exits by itself. A latency test now covers it, and it fails
without the fix.

## Profile

[`tools/prof.sh`](tools/prof.sh) ran five 40 s workloads on the testbed:
- idle (probing only);
- 4-stream download;
- upload;
- both directions;
- congested: both directions while the ISP upload alternates between 2.5 and 1.5 Mbit/s every 5 s.

There were three passes per build, alternating
(`profile/cpu*`), with nothing attached to the daemon. One extra pass per
build ran under perf and one under strace. CPU is user + system ticks
(10 ms each). The filter's runs and cost come from the kernel's BPF
statistics.

| Workload | `head` ticks / 40 s (median) | `base` ticks / 40 s (median) | % of one core (`head`) | Filter runs | RSS |
| --- | --- | --- | ---: | ---: | ---: |
| idle | 5 / 5 / 4 (5) | 7 / 6 / 5 (6) | 0.12 | ~1,050 | 2.1 MB |
| download | 9 / 12 / 10 (10) | 25 / 15 / 12 (15) | 0.25 | ~98,000 | 2.1 MB |
| upload | 9 / 9 / 10 (9) | 15 / 9 / 7 (9) | 0.23 | ~12,600 | 2.1 MB |
| bidirectional | 16 / 15 / 15 (15) | 43 / 16 / 11 (16) | 0.38 | ~89,000 | 2.1 MB |
| congested | 15 / 14 / 12 (14) | 41 / 15 / 17 (17) | 0.35 | ~76,000 | 2.1 MB |

The two builds are equal within run-to-run noise. The high first `base` pass
did not repeat. The filter took 1.9-2.3 µs per packet under load, the same
order as the earlier filter-cost evidence. Its program is identical in both
builds, so the per-run differences in `profile/cpu-summary.md` are noise as
well.

### Where the samples go

perf `cpu-clock` at 999 Hz on the `head` daemon. Each stack was classified
with [`tools/breakdown.py`](tools/breakdown.py), the 2026-09-30 script plus
a category for the BPF ring buffer.

| Workload | Samples | epoll wait | softirq packet work | qdisc dump | BPF ring buffer | userspace |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| idle | 86 | 81% | – | 2% | – | 9% |
| download | 180 | 48% | 19% | 13% | 6% | 10% |
| upload | 191 | 83% | 5% | 6% | 1% | 4% |
| bidirectional | 256 | 40% | 20% | 12% | 6% | 18% |
| congested | 210 | 60% | 13% | 11% | 4% | 10% |

Flame graphs: [idle](flamegraphs/flamegraph-idle.svg),
[download](flamegraphs/flamegraph-download.svg),
[upload](flamegraphs/flamegraph-upload.svg),
[bidirectional](flamegraphs/flamegraph-bidirectional.svg),
[congested](flamegraphs/flamegraph-congested.svg). The folded stacks for both
builds are in `profile/folded/`.

The daemon's own work under bidirectional load is about 46 samples in 40 s,
roughly 0.1% of a core:
- unresolved musl `libc` frames, the largest part of it;
- reply handling and the controller, through `control_update`;
- the TCP ring-buffer drain (`drain` → `add_record` → the estimator), 13-15 samples.

Nothing is worth optimizing at these rates.

### Syscalls

`strace -c -f` over 40 s of bidirectional load (`profile/strace-*`):
5,761 calls for `head` and 5,805 for `base`. Most are `recvmsg`, `read`,
`epoll_pwait`, `sendmsg` and `poll`. There are also 75 `bpf` calls for the
counters.

Two small observations, not changed:
- **Netlink receive buffer:** libnl-tiny's `nl_recv` allocates a 16 KiB buffer
  for every receive (`calloc(1, getpagesize() * 4)`), which musl maps and
  unmaps, so a qdisc dump costs two `mmap`/`munmap` pairs. Avoiding it would
  mean replacing libnl's receive loop, for a few microseconds per tick.
- **`/etc/TZ`:** musl reopens it about once per second when log timestamps
  are formatted.

## Soak

[`tools/soak.sh`](tools/soak.sh) ran `head` for 15 two-minute cycles
(30 minutes). Each cycle had:
- both directions, then download, then upload;
- one removal and recreation of the upload CAKE, which reloads the TCP filter;
- an idle stretch long enough for the daemon to sleep and be woken by the next cycle.

It used the default 2 MB log limit, so rotation repeated (`soak/`).

| Sampled every 10 s | First | Range | Last |
| --- | ---: | ---: | ---: |
| RSS (kB) | 1,372 | 1,372-2,136 | 2,136 |
| At each cycle start (kB) | 2,124 | 2,124-2,136 | 2,124 |
| File descriptors at each cycle start | 24 | 24-24 | 24 |
| Memory mappings | 54 | 54-58 | 57 |
| Zombies | 0 | 0 | 0 |

There was no growth after the first cycle. CPU averaged 0.69% of one core
over the 30 minutes, including 15 filter reloads. The daemon exited with 0
and left nothing behind. The retained logs cover the last six cycles; their
only warnings are the expected ones while the upload CAKE was missing.

## Observations

- **STALL flapping:** with every reflector down, the state alternated between
  RUNNING and STALL every 1-3 s. The stall check compares both directions'
  rate with `connection_stall_thr_kbps` (10 kbit/s), and the testbed's 100 ms
  probe sat right at that threshold. cake-autorate `ac75f49` uses the same
  two conditions with no hysteresis, so this is faithful porting.
- **Bursty load:** the 800 kbit/s keep-alive in the lifecycle phases is
  bursty at the 200 ms traffic cadence. The upload shaper therefore stepped
  between 2,970 and 3,000 kbit/s: one increase per high-load sample, and a
  1% decay on idle samples.

## State

[`vm-state-before.txt`](vm-state-before.txt) and
[`vm-state-after.txt`](vm-state-after.txt) record the VM before and after.

The VM is test-only, and its installed package was replaced:
- package: `cake-adapt` from `05eb6b3` (binary SHA-256 `483b5df5…`) replaces the `d6564a6` package;
- `/etc/config/cake-adapt`: unchanged;
- the service: running;
- `kernel.bpf_stats_enabled`: back to 0;
- test files: removed.

Collected by Claude directly at the user's request, outside the designated
agents of COLLABORATION.md.
