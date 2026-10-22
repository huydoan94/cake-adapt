# Profile of 0.3.21 against 0.3.15 (2026-10-21)

[Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/icmp-native/profiling/2026-10-21-profile/index.html): the tables and every flame graph on one page.

## Result

0.3.21 with its default native pinger uses about as much CPU in total as
0.3.15 with fping (33 against 25 ticks per 40 s idle, 55-69 against 51-57
under load: 0.8% and 1.4-1.7% of one core of the VM), and 0.5 MB less memory
(2.36 MB, no fping process). The native probes cost the daemon about what
the fping process cost.

The daemon's own CPU, fping left out, doubled from 0.3.15 to 0.3.21 (8 to 18
ticks idle, 17-24 to 33-43 under load, with fping as the pinger in both). Most
of the rise is the log writer thread added in 0.3.21: the main loop wakes it
for every record, about 5,000 `futex` calls and as many thread switches per
40 s of bidirectional load, where 0.3.15 makes none. Waking it only when the
queue fills up, for syslog entries and requests, or at the buffer timeout
would remove that; done and measured in
[`2026-10-22-log-writer-wake`](../2026-10-22-log-writer-wake/README.md).

## Builds

| Name | Commit | Binary SHA-256 prefix | Pinger |
| --- | --- | --- | --- |
| `v0321-native` | `2802243` (0.3.21) | `2f95442580e10d25` | native (default) |
| `v0321-fping` | the same binary | `2f95442580e10d25` | `pinger_method fping` |
| `v0315` | `661f385` (0.3.15, the 2026-10-09 profile's commit) | `4a6762a8469ba5f6` | fping |

Both were compiled from `git archive` exports with the x86 SDK's own compile
command (`tools/sdk-compile-line.txt`, from `make package/cake-adapt/compile
V=s`), adding `-g3 -gdwarf-4 -fno-omit-frame-pointer`
([`tools/build-variant.sh`](tools/build-variant.sh)); base variant, no MQTT.
Each build's TCP filter object came from the same compile.

## Environment and method

As on 2026-10-09 (OpenWrt 25.12 x86 VM, `192.168.56.2`; the emulated
testbed, ISP 2.5 / 30 Mbit/s; [`tools/cake-adapt.template`](tools/cake-adapt.template)),
with [`tools/run-all.sh`](tools/run-all.sh) and [`tools/prof.sh`](tools/prof.sh):
three CPU passes per variant in a rotating order, then one perf pass and one
strace pass each, seven 40-s workloads per pass. "Idle" is a quiet link before
the daemon sleeps (60 s), so the pinger still runs. Changes from 2026-10-09:

- CPU now includes the pinger child: the daemon's own ticks with all its
  threads, the children it waited for, and its live fping process, also
  reported apart (`daemon_ticks`, `pinger_ticks`, `pinger_rss_kb`);
- perf and strace follow the daemon and its fping process;
- 0.3.21 takes its configuration as one file (`-C`), 0.3.15 as a directory.

The installed service was stopped for the runs (20:55-21:57) and started
again; every run kept the test-log inode and left no namespace, fping or BPF
statistics behind (`raw/prof/*/inode`, `leftovers`). The VM ran slower than
on 2026-10-09 (0.3.15's daemon alone: 17 ticks under download, 11 then), so
compare builds within this profile.

## CPU and memory

Medians of three passes, ticks of 10 ms per 40 s (40 ticks = 1% of one core);
all passes and the BPF figures are in [`profile-summary.md`](profile-summary.md):

| Workload | 0.3.15 daemon | 0.3.21 daemon (fping) | 0.3.21 daemon (native) | 0.3.15 total | 0.3.21 fping total | 0.3.21 native total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| idle | 8 | 18 | 33 | 25 | 35 | 33 |
| download | 17 | 33 | 56 | 52 | 69 | 56 |
| upload | 13 | 26 | 43 | 36 | 45 | 43 |
| bidirectional | 21 | 43 | 55 | 52 | 78 | 55 |
| congested | 24 | 38 | 64 | 57 | 67 | 64 |
| download6 | 17 | 36 | 69 | 53 | 75 | 69 |
| bidirectional6 | 20 | 41 | 63 | 51 | 72 | 63 |

Resident memory, daemon plus pinger: 2.36 MB native, 3.1 MB with fping
(0.75 MB of it fping), 2.9 MB for 0.3.15.

## Where the samples go

perf `cpu-clock` at 999 Hz, one pass per variant, classified with
[`tools/breakdown.py`](tools/breakdown.py) ([`breakdown.txt`](breakdown.txt)).
At idle, 162 of the native
variant's 492 samples are in stacks through `futex_wake` and 315 in the
scheduler's task switch, against none and 177 (fping included) for 0.3.15;
under load 0.3.21 also has 3-9 times the softirq work charged to it (209-291
against 31-63 samples), packet processing that runs on its more frequent
switches. strace over
40 s of bidirectional load: `futex` 4,874 calls (native) and 5,260 (fping)
for 0.3.21, none for 0.3.15; the other calls match.

Flame graphs ([FlameGraph](https://github.com/brendangregg/FlameGraph)
`41fee1f`, `stackcollapse-perf.pl --kernel`; folded stacks in `folded/`), by
variant and workload: `flamegraphs/<variant>-<workload>.svg`, for example
[native, idle](flamegraphs/v0321-native-idle.svg),
[native, bidirectional](flamegraphs/v0321-native-bidirectional.svg),
[fping, bidirectional](flamegraphs/v0321-fping-bidirectional.svg),
[0.3.15, bidirectional](flamegraphs/v0315-bidirectional.svg).

## Limits

- One 32-bit x86 VM; no router and no arm64 build were profiled.
- One perf and one strace pass per variant; the CPU comparison rests on three
  passes each, and the VM's speed drifts between days.
- The native pinger's own cost is read from the difference between the two
  0.3.21 variants, which share everything else.
