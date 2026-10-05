# Bounded controlled comparison

Question: compare latency, received TCP throughput, upload ACK headroom and CPU
cost with both extensions off, their previous implementation (`518df81`), and
current packaged implementation (`bbca799`). One repetition each in that order.
This combines confidence and accounting changes; it does not isolate either.
Reuse exact retained binaries/objects and staged libraries; no package install,
SDK rebuild, production change, tuple-lifetime work or parameter tuning.

Fresh cpe/isp/inet testbed each variant, refuse existing namespaces/filter.
CAKE plain ack-filter, Ethernet overhead 44 MPU 84, both directions controlled.
Upload min/base/max 1000/2250/3000 kbit/s; download 5000/27000/33000.
ISP 2500/30000 kbit/s, 10 ms each way, 500 ms buffer, TBF overhead30 MPU 84:
L2 skb length +30 equals IPv4 length +44. Fixed UDP echo 160 bytes/20 ms
(64 kbit/s payload) competes with TCP/ACKs throughout the measured phases.
Extensions off uses tcp_delay_attribution=0 / ul_congest_ack_share=0; on uses
1 / 0.45. Other controller settings unchanged.

One preflight, current binary only: 4 s idle then 5 s simultaneous P4 download
and single upload, 2 s recovery. Check configuration, route, positive receiver
payloads, PCAP, latency, actual JIT/program runs/counters and daemon shutdown.
Deadline90 s. One acceptance batch: each variant 8 s idle, 15 s upload, 3 s
quiet, 20 s P4 download, 3 s quiet, 25 s simultaneous P4 download/single upload,
8 s recovery. About85 s each, deadline330 s. No additional load/soak runs.

Capture hashes, kernel, affinity CPU 0 for all test processes, clock tick rate,
program/map metadata and fdinfo memory counters, BPF exact run/time deltas,
ring loss/unaccounted packets, daemon ticks/RSS and host CPU counters at 500 ms.
BPF stats enabled by a reference-counted FD, no global sysctl change.
Capture outbound PCAP after CAKE, timestamped independent fping at100 ms,
UDP RTT/received counts, receiver iperf JSON, phase timestamps, raw daemon
LOAD/DATA/TCP_QUEUE/SHAPER records and final qdisc state. Exclude initial2 s
and final 1 s of each phase from latency/headroom/CPU summaries. Report receiver
goodput against nominal capacity and explained packet overhead; target85%
capacity for sustained one-direction load, latency first. Mixed load shares
upload with ACKs, UDP and data: report its tradeoff explicitly. Require valid
measurements, bounded configured shapers and clean restoration. A material
regression stops further experiments; do not retune to force a passing result.
One short batch is evidence for this testbed only, not production acceptance.

Snapshot installed service/process/package/config/files, complete root qdisc
options, links/routes/namespaces before mutation. Original service keeps its
own root qdiscs. Temporarily create the otherwise absent filter path; remove
it and a newly created parent directory at cleanup. Never overwrite an
existing filter. Protect /tmp/sqm-mon-test.log inode, refuse active writers,
truncate in place and hardlink isolated cake-adapt.log; leave reader alone.
All test processes run inside owned namespaces, terminate/wait then kill if
needed before deleting namespaces; reap shell children and preserve failing
outputs. Restore/check root state, inode and no owned executables remain.
Foreground SSH with HUP cleanup, connect5 s/transfers30 s. After two harness
or environment failures stop/reassess; rerun only specific affected cases.

## Preflight sampler correction

The first preflight stopped on sampler `bpf_prog_get_info_by_fd`: Bad address.
The program-info output contains nonzero instruction/info lengths but no output
buffers; feeding that output into the next query requests copies to null
pointers. Clear the struct before each statistics-only query. This affects
only the measurement harness. Retain failed preflight output; clean daemon
shutdown and root-state/log-inode restoration passed. Retry only the short
current preflight as `preflight-retry`, then require local validation before
the acceptance batch. A second harness/environment failure stops VM execution
for reassessment.

The first retry transport exited255 before the remote command created any
retry file. A single read-only check confirmed the VM was alive and no test
namespace/filter remained. Stop and reassess the two failures: the sampler
query is locally corrected; use plain foreground SSH instead of the failed
PTY transport, with a VM-side `timeout -s HUP` so disconnects still leave a
bounded cleanup path. Retry only the same preflight, not an acceptance run.
No further VM retry follows another harness/environment failure.

## Local reassessment of the launch blocker

The revised launch stopped before setup: `timeout` is absent from the VM.
No namespace/filter/test process was created by either failed launch. Keep
execution paused until the deadline is implemented in the existing C helper
and reviewed/tested locally. The helper forks the command, sends HUP at its
deadline, waits up to10 seconds for shell cleanup, then kills a stuck child.
Local tests cover normal exit, HUP cleanup and the KILL fallback. This removes
the unsupported VM command; no package/tool installation or product edit.

Replace the preceding no-retry stopping point only after this local
reassessment: one final corrected preflight using plain foreground SSH and
`sample --limit80 sample --pin sh...`, then local validation; if it fails,
no further execution. If it passes, run the originally planned single
acceptance batch under a300-second VM deadline/330-second host deadline.
The phase list, variants, repetitions and acceptance gates remain unchanged.

The final preflight produced valid measurements, graceful daemon exit, zero
ring/capture loss/unaccounted packets, JIT execution and independent ACK-byte
agreement. All restoration snapshots and inode163 matched. Its exit 1 was
solely the leftover-executable audit counting the expected deadline supervisor,
which must stay alive until run.sh exits. Exempt only run.sh's recorded parent
PID, retaining all other executable checks. The supervisor returns/exits after
waitpid, as confirmed by completion of the foreground SSH job. Preserve exit 1
and the raw snapshots; accept the reviewed measurements without repeating
preflight. Proceed only with the original single acceptance batch.
