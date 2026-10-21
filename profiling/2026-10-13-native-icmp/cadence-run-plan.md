# Equivalent timestamp-request cadence capture

2026-10-13. Existing tcpdump found on local host and authorized x86 VM; no
downloads/installations. Exactly one fping-ts/native-ts diagnostic pair using
the original validated epoch-fixed base binary SHA256
35ea0a92a4f042f6f88d4dffb9d7cd7ff67ad38b0d2f4d0d49378839951c1e48.
No extra per-packet daemon logging or production change. Not acceptance.

Question: do the two methods differ in timestamp-request spacing at equivalent
boundaries during upload, and where does originate-to-peer time accumulate?
Capture ICMP type 13 to the six 10.99.0.11–16 reflectors using identical tcpdump
filters/snaplen96 on isp:iwan ingress (after cpe CAKE, before simulated ISP UL
queue) and inet:inet0 ingress (after that queue/netem). These are kernel capture
timestamps at receiving interfaces, not application send/return or NIC wire
times. Originate-to-first-capture includes cpe scheduling/CAKE/transit and
millisecond quantization; do not label it pure timestamp-age bias. Both receive
namespaces share the same VM clock. Preserve PCAP and match full request tuples.

Two cases in fixed order fping-ts then native-ts, each 15s idle +60s steady UL
+10s idle +60s UL step (capacity8→4 at20s→8 at40s). Original thresholds/rates,
six reflectors300ms, independent100ms probe, unique sections/inodes and cleanup
unchanged. Explicit SEND_CAPTURE_ONLY guard ends run.sh after upload-step,
records end/CPU/qdiscs, and executes its existing archival cleanup. No download,
bidirectional or reflector faults in this narrowly targeted capture diagnostic.
Do not feed it to the four-phase acceptance scorer. Approximate300s total,
VM watchdog480s and SSH540s. Snapshot both capture processes' CPU before/after,
reject capture failures/kernel drops/truncation/unsupported framing/incomplete
cases/clock reversal/reboot; capture overhead is reported, not claimed absent.

Local shell syntax, parser regression checks and Astra review first. Reuse
unchanged full-workload helper evidence and do not rebuild the daemon. One
short capture preflight with the new harness is allowed if needed, then exactly
one planned pair. After a second environment/harness failure stop/reassess; no
automatic benchmarking repeats. Previously clean capability check is retained
as metadata. Batch rechecks no existing task paths/namespaces/log writer.
Bound capture startup to one second, verify both processes/listening, stop and
wait them before archival; cleanup traps also stop only owned captures.
SIGINT gets one second to flush, then owned lingering captures are killed;
nonzero capture exit or missing drop counters invalidates the capture.

Analyze idle/upload/step with minimum250/1000/1000 first-capture packets,
all six destinations, each first-capture packet matched once or counted
unmatched after a two-second drain, and no captures dropped by kernel.
Reject duplicate matches; unmatched packets are reported as path/capture
absence, never silently dropped from the denominator. Delay distributions
use matched packets and explicitly state their coverage. Report global/per-target spacing
p50/p95/p99/max, originate→first/peer capture and first→peer capture delay,
long-gap percentages, phase packet counts, probe loss, goodput and capture CPU.
Use first capture's phase membership for both timestamps to avoid boundary
queueing loss. Quantify DATA count and sample→controller age separately.
Packet loss/time gaps remain findings, not reasons to waive checks. Matching
requests must exclude the independent echo probe. Do not infer causal controller
regression from one sequential pair or compare its workload results as a new
acceptance baseline.

Persist each completed case and capture on VM disk immediately. Collect into
ignored build storage, verify, remove owned namespaces/processes/locks/payloads,
preserve /tmp/sqm-mon-test.log inode and installed controller PID/hash. Capture
boot, affinity, clock source, JIT, tcpdump version and artifact identities.
Stop and report after this pair. Only a demonstrated defect supports a scoped
fix; migration/default/gate changes remain blocked.
