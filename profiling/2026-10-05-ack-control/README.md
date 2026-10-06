# Bounded ACK/control comparison, 2026-10-05

**Update 2026-10-05:** the regression reported below was traced to the
directional agreement guard of `0321661`, not to the ACK accounting. With only
that guard reverted, three repetitions restored mixed download to about 20
Mbit/s and mixed p95 to about 63 ms; the guard was then reverted (`d6564a6`).
See [`2026-10-05-gpt-work-check`](../2026-10-05-gpt-work-check/README.md).

This compares the opt-in TCP attribution and upload ACK ceiling together,
using the controller derived from [cake-autorate](https://github.com/lynxthecat/cake-autorate).
Tuple-lifetime finding 2 remains deferred. Production code, defaults and versions
are unchanged by this measurement work.

Variants: `off` uses current packaged binary bbca799 with both extensions off;
`before` uses the retained 518df81 binary and previous filter with both on;
`after` uses current packaged bbca799 binary/filter with both on. This compares
the combined confidence/accounting mitigations, not separate causal effects.
Before was built with different optimization/debug flags; compare measured
daemon CPU as these artifacts, not a compiler-controlled implementation delta.
BPF objects are the exact retained packaged objects; hashes are in
[artifact-identity.txt](artifact-identity.txt) and each raw run.

[PLAN.md](PLAN.md) fixes one repetition per variant, short idle/upload/download/
mixed/recovery phases, 2.5/30 Mbit/s ISP bottlenecks, 10 ms each direction, CAKE
Ethernet overhead 44/MPU 84, plain ack-filter, both directions controlled and
a 160-byte UDP echo every 20 ms. Kernel TBF adds 30 to the Ethernet skb length,
then clamps to 84; this matches CAKE's IPv4 length + 44. The
[Linux rate conversion](https://github.com/torvalds/linux/blob/v6.12/include/net/sch_generic.h#L1193)
and [tc TBF options](https://github.com/iproute2/iproute2/blob/main/tc/q_tbf.c)
were inspected before execution; raw qdisc readback verifies the selected settings.

The read-only C sampler queries the daemon's existing program and counter map
at 500 ms, enables BPF runtime statistics with a reference-counted FD, and records
real/monotonic clocks, daemon user+system ticks/RSS, actual program runs/time,
ring loss, incomplete accounting and host CPU counters. All test descendants
inherit CPU 0 affinity. `memlock` is the kernel fdinfo field, retained as reported;
map value/capacity and fdinfo charges are not a total resident/peak-memory claim.
Daemon CPU is ticks/HZ divided by measured elapsed time, as percent of one core.
BPF cost is run-time delta/run-count delta and runtime/elapsed as percent of
one core; stats/probes/tcpdump add measurement overhead in every variant. Host
busy percentage covers all cores/background work and is not attributed to BPF.

Independent fping 100 ms and timestamped UDP echo provide latency; each uses its
own idle median. Added-delay percentiles and fraction above 30 ms exclude the
first 2 and final 1 seconds of each phase. Received iperf TCP bytes and receiver
duration give goodput; they are full-transfer averages, unlike steady-window
latency/CPU/headroom. PCAP after CAKE counts pure ACKs independently, charged
as max(frame_length+30,84). Headroom is current upload shaper budget minus
charged pure ACK rate; it is room for other upload traffic, not unused capacity
when TCP upload/UDP is already filling it. The report also records 0.5 s ACK-share
p95; bursts may exceed a long-term share reservation. Per-CPU map reads and
PCAP timestamp boundaries are not atomic: ACK crosschecks allow 1% or 500 bytes,
whichever is larger. Ring/capture loss or any unsupported packet fails validation.

## Outcome

The acceptance batch exited0 and every measurement gate passed. **Performance
acceptance failed:** current download goodput stayed well below 85% nominal
capacity, and mixed latency/goodput regressed versus the previous-on artifact.
No retuning, extra load run, production edit or deployment followed. One
ordered sample does not establish causality, but this material regression
prevents a claim that the mitigations improve controlled behavior.

| Variant | Upload goodput Mbit/s | Download goodput Mbit/s | Mixed UL / DL Mbit/s | Added RTT p95 upload / download / mixed ms |
| --- | ---: | ---: | ---: | ---: |
| off | 2.150 | 18.245 | 1.718 / 15.476 | 94.4 / 65.3 / 91.4 |
| before | 2.092 | 22.805 | 1.633 / 20.171 | 90.5 / 47.1 / 65.2 |
| after | 2.093 | 15.780 | 1.843 / 10.988 | 116.9 / 38.0 / 93.9 |

Current download is 52.6% of the 30 Mbit/s nominal bottleneck, versus 76.0% before
and 60.8% off. Current upload is 83.7% of 2.5 Mbit/s, versus 83.7% before and 86.0% off.
TCP framing overhead, the continuous64 kbit/s-payload UDP echo and probes
consume capacity, but cannot explain download falling to 52.6%. Current mixed
download fell 45.5% (20.171 to 10.988 Mbit/s) while upload increased 12.8%
(1.633 to 1.843); added RTT p95 increased 44.0% (65.2 to 93.9 ms). Current download
p95 improved 47.1 to 38.0 ms, accompanied by 30.8% lower goodput (22.805 to 15.780).

| Mixed phase | Charged ACK kbit/s | ACK share mean / p95 | Headroom kbit/s | Daemon CPU one core | BPF ns/run | BPF CPU one core | Actual BPF runs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| off | 424.1 | 15.9% / 31.1% | 2244 | 0.37% | 0 | 0.000% | 0 |
| before | 524.1 | 20.8% / 31.6% | 1996 | 0.41% | 2769 | 0.560% | 44729 |
| after | 346.6 | 13.1% / 27.7% | 2309 | 0.38% | 3191 | 0.445% | 29403 |

The larger current ACK headroom comes with lower download throughput; it is
not a standalone win. Measured p95 ACK shares remained below 45% in this
profile, so it does not stress the ceiling at its configured share. Mixed
filter cost rose about 15.2% per call (2769 to 3191 ns); lower traffic reduced
absolute BPF CPU (0.560% to 0.445% of one core). These are costs under different
resulting packet mixes, not an isolated instruction-cost benchmark. Daemon
CPU differences are small/coarsely sampled and binaries use different flags.

All UDP echoes were received (off 4154/4154, before 4146/4146, after 4141/4141).
Their mixed added-delay p95 also regressed (64.7 to 93.4 ms). Current time above
30 ms added RTT in mixed load was 65.0% versus 58.5% before; recovery p95 was 4.5 ms
versus 5.7 ms. Idle ICMP medians were24.6/23.5/23.1 ms (off/before/after).

All filters were JITed; counters had no ring loss/incomplete accounting;
tcpdump dropped no captured packets. PCAP/map ACK bytes matched exactly:
before 2212478 raw bytes and after 2262872 charged bytes over each sampler
window. Max daemonRSS was 1232/1904/1928 KiB (off/before/after), not incremental
BPF resident allocation. CPU/map metadata and qualified fdinfo memory fields
are in each samples file.

Original service PID 2649, package/files/config/init hashes, complete root qdisc
options, stable links/routes and namespaces matched before/after. Protected
log inode 163 was unchanged. Final audit found no owned executable, namespace
or filter; collected test payloads were removed. See vm-cleanup.txt. The model
did not exercise long baseline aging, receiver wait changes, churn, qdisc lifecycle
or ARM execution. Keep the extensions opt-in; diagnose the observed
control regression before making deployment claims.

## Retained harness failures

The first preflight failed in the measurement sampler, which fed returned
program-info lengths back as buffer capacities without buffers. Zeroing the
info struct before statistics queries fixes that. An SSH PTY retry did not
start the job; plain SSH works, but the VM lacks timeout. A small locally
tested C deadline supervisor replaces that dependency (normal exit, HUP
cleanup, KILL fallback and external TERM tests in deadline-tests.txt). It blocks
signals across fork/setup; the child restores its mask before exec, the parent
installs handlers/arms the alarm before unblocking.

The final preflight produced valid measurements and restored every compared
state field/inode, but exited1 because the leftover audit counted its expected
parent deadline supervisor. The audit now exempts only that parent. The
supervisor exited with the completed foreground job. Reuse those validated
measurements without another preflight; preserve the original exit 1. All
failed output and snapshots remain in raw/. Only the originally planned
acceptance batch follows; there are no extra soaks or parameter retuning.

## Reproduction

Populate a fresh VM directory with the exact retained `before`, `after`,
`before.o`, `after.o`, staged libbpf.so.1/libelf.so.1 under lib/, compiled
`sample`/`udpping`, and `tools/run.sh`/`tools/testbed.sh`. Both C tools compile
with C11 and all project strict warnings; sample uses src/ and staged libbpf
headers and links libbpf/libelf/zlib. Clang-format is stable; both scripts pass
sh -n. The production unit/sanitizer/replay/SDK results are reused unchanged.

Set LD_LIBRARY_PATH to the isolated lib directory. Invoke through foreground
SSH with host deadline 330 s: `sample --limit 300 sample --pin sh run.sh DIR acceptance`
(paths must be absolute on the VM).
The wrapper refuses existing namespaces/filter or active protected-log writers,
hardlinks the authorized log in its own directory and verifies restoration.
Collect each result directory and root snapshots before removing payloads, then
run `python3 tools/analyze.py raw/results/acceptance`. Do not install a package,
replace the protected log or touch the user's tail process.

A single short, ordered run on this x86 testbed is not statistical significance,
customer-link acceptance, arm64 behavior, tuple safety or a claim about receiver
delay identification. A failure to keep about 85% capacity or material latency
regression is a finding to report, not permission to retune or keep testing.
