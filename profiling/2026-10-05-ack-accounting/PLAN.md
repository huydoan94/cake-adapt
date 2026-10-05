# ACK accounting acceptance

Question: do both byte counters use CAKE's non-GSO charge, and does incomplete
coverage reliably invalidate the optional ACK ceiling?

Use the exact packaged before/after x86 filter, production capture.c for after,
and deterministic raw Ethernet frames in a fresh IPv6-disabled veth namespace.
The before object is the unchanged filter from a0b52af; retain its SHA256.
One preflight: raw before/after pair plus stale-object rejection. One batch:
Ethernet normalization, +44 overhead, MPU84, -20 overhead, ATM, PTM, VLAN/QinQ
fallback, unknown EtherType fallback, RAW unknown EtherType, nonsplit GSO and
RAW GSO fallback, and accounting-disabled compatibility. One repetition per
case; no performance/throughput claim or additional soak.

Require literal ACK/total/bad-packet counts, no ring loss, exact program runs,
and nonzero x86 JIT length. For supported cases compare CAKE's independently
reported min/max adjusted size with the small/data charges. Host tests cover
ATM/PTM boundaries, rate precision, failure/recovery and same-handle qdisc
recreation. Runtime metadata: object hashes, kernel, affinity, JIT lengths,
map types/values/capacity, raw run count/time (not a CPU-cost estimate). Map
sizes describe ABI and configured capacity, not total allocation.

Strict-warning compile and shell syntax/cleanup review precede transfer. Run
only isolated probes; leave the installed daemon/package/config and original
qdiscs running unchanged. Capture service, process, files, packages, root links,
routes, qdisc options, namespace list before/after. Protect the existing test
log inode; refuse any active writer, truncate in place and hardlink its expected
name in the isolated log directory. Never touch the user's reader.

Deadline: 45 seconds preflight, 90 seconds batch; foreground SSH, bounded
transfer/connect deadlines, namespace cleanup on EXIT/HUP/TERM. Retain failed
output. A retry must address a specific failure; stop/reassess after two
harness/environment failures. Remove only test payloads after collecting raw
results. No rate-control or customer-link performance claim follows this run.

## Reassessment after preflight

The older SCP server rejected a dot-directory name; explicit filenames fixed
transfer. Preflight counters and stale-object rejection passed, but the new
object was interpreted. Pinned i386 JIT source and local disassembly identified
two unsupported instructions introduced by the shared helper: a BPF-to-BPF
call and 64-bit ATM division. CAKE charges fit its existing 32-bit packet
arithmetic; force this helper inline and retain 32-bit framing arithmetic.
Local disassembly must show no subprogram or 64-bit division before retry.
This is a bounded arithmetic correction, not a filter/lifetime redesign.
The state comparison also included a naturally ticking bridge GC timer; retain
raw link snapshots but normalize that timer alone for configuration comparison.
Retry only the failed candidate RAW/JIT case with the rebuilt exact object,
then run the original single batch. No repeated baseline or performance runs.

## Batch setup correction and final stopping point

Candidate JIT retry passed (8483 JIT bytes; exactly two runs). The batch's
first kernel-reference assertion stopped before the candidate ran: tc's
`ethernet` preset also sets MPU84, while the intended model used MPU0. Retain
that qdisc/output and specify MPU explicitly in every case. The kernel check
stays unchanged. Original VM configuration/process/package/qdisc and log inode
checks passed cleanup. One final retry of the remaining batch is permitted;
any further failure ends VM execution for this attempt. No other test campaign.

Final retry: supported scalar models and single-tagged fallback passed; the
double-tagged data frame exceeded the default veth MTU. Execution stopped as
planned. Namespace/process/config/qdisc restoration and inode checks passed.
The local harness now uses MTU1600; it was not rerun. Remaining fallback paths
were checked through the actual filter's native helper seam and sanitizers.
Test payloads were collected and removed; no further VM tests are authorized by
this batch.
