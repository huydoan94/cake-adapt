# Portable TCP lifetime stream

Status: ✓ The selected protocol, implementation, focused host checks, replay,
sanitizers, packaged-object instruction inspection and existing x86/Filogic
package builds pass. Kernel verification, remaining architecture coverage and
runtime/cost acceptance are pending. This records the implementation selected
for the [work orders](TUPLE_LIFETIME_WORK_ORDERS.md). The one-time implementation
pass has ended; it does not establish support or readiness. The detailed
[capability inventory](TUPLE_LIFETIME_CAPABILITIES.md) records available and
missing target resources.

## Ownership and observation contract

Keep lifetime decisions and authoritative departures in one userspace writer.
BPF supplies raw packet evidence, byte counters and optional timing thinning.
The monitor supplies availability to the existing controller; controller policy
and CAKE scheduling remain unchanged.

The lifetime processor owns handshake state, generation allocation, departure
matching and calibration resets. Its integer updates need no cross-CPU atomics.
The BPF program has no connection-generation allocator, lifecycle publication
state, try-lock ownership protocol or architecture-specific synchronization.

All successfully parsed SYN, FIN and RST packets produce lifecycle evidence,
even without TCP timestamp options. They bypass timing thinning. Timestamped
ordinary packets supply outgoing departure or incoming echo evidence. A racy
BPF sampling map may skip ordinary timing data; it cannot suppress a lifecycle
event. An optional echo map may permit additional samples, but it does not decide
which connection owns a departure.

## Why stream ordering is sufficient for observed handshakes

The [kernel ring-buffer contract](https://docs.kernel.org/bpf/ringbuf.html)
orders consumption by reservation, including reservations on different CPUs.
It does not promise packet-hook entry order or immediate availability of a busy
record. We do not infer either from it.

The synchronous AF_PACKET capture points supply the needed TCP causality:
outbound taps execute before driver transmission, and inbound taps execute
before the ingress redirect/protocol delivery. A successor connection's
handshake-dependent data cannot precede completion of the relevant SYN/SYN-ACK
callback. Before that callback returns, it must submit its lifecycle evidence
or publish a loss marker; an unreportable loss instead sets a sticky fatal latch.

A callback paused before publication overlaps the consumer snapshot. That
snapshot may legitimately precede the reset. Predecessor traffic processed
during this interval still belongs to the predecessor. If a retransmission
completes first, it provides the same reset evidence. The delayed original has
an earlier captured time and is rejected against the retained lifecycle boundary.

An older record reserved before a reset is consumed before the reset, which
then clears its calibration. A packet captured earlier but reserved after the
reset is rejected by its capture time. A snapshot with remaining ring data
withholds queue validity for that drain. Ring-position snapshots are only
conservative vetoes, not an atomic producer-quiescence proof.

The reviewer accepted this causal argument after challenging pre-reservation
and pre-marker intervals. Future concurrency tests must preserve actual TCP
causality rather than independently injecting impossible successor traffic.
The hook-ordering assumption still needs kernel/runtime verification on the
selected target configurations.

## Record and publication contract

Every record is exactly 64 bytes, with all fields initialized:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `arrival_ns`, 64 bits | Monotonic time captured before parsing. |
| 8 | `flow`, 36 bytes | Normalized endpoint addresses and network-order ports. |
| 44 | `tsval`, 32 bits | Host-native TCP timestamp, meaningful only with its presence flag. |
| 48 | `tsecr`, 32 bits | Host-native echoed TCP timestamp. |
| 52 | `sequence`, 32 bits | Host-native TCP sequence number. |
| 56 | `acknowledgment`, 32 bits | Host-native TCP acknowledgment number. |
| 60 | `metadata`, 32 bits | Lifecycle/ACK flags, direction, timestamp presence, protocol version and epoch. |

Metadata uses bits 0-7 for SYN/FIN/RST/ACK evidence in their TCP flag positions;
the producer does not publish PSH/URG/ECN flags in this version. Bit 8 indicates
outgoing direction, bit 9 indicates timestamp presence, bits 10-15 hold protocol
version 1, and bits 16-31 hold the epoch.
The producer and consumer use the SDK's matching BPF/host byte order. Fixed-width
fields and size/offset assertions preserve 32/64-bit ABI compatibility. Runtime
compatibility includes exact size, version, field validity and expected map
schemas; matching size alone does not establish compatibility.

Generation identifiers are assigned only by userspace. They combine the epoch
in the upper 32 bits with a monotonic local ID in the lower 32 bits. Zero and ID
exhaustion are unavailable states. No identifier wraps silently. Reopening a
capture creates a new ring and resets the estimator, so an earlier capture's
records cannot enter it.

## Loss, invalidation and recovery

The BPF producer and native consumer share these control maps:

| Map | Type and bound | Writers / lifetime |
| --- | --- | --- |
| `stream_epoch_v1` | Non-LRU hash, one 32-bit key/value | Userspace publishes the current epoch through hash replacement. BPF reads it once for an event. |
| `stream_losses` | Non-LRU hash, at most 64 epoch markers | BPF inserts persistent loss evidence; nobody deletes markers while attached. |
| `stream_fault` | Per-CPU array, one 32-bit fatal latch per CPU | BPF writes one idempotently when loss publication itself fails. Never cleared while attached. |

Epochs are restricted to 1 through 64. A required reservation failure inserts a
marker for the event's captured epoch. If insertion fails and lookup does not
find an existing marker, that CPU's fatal latch is set before returning. A
delayed old-epoch failure cannot overwrite the new epoch's status.

The consumer reads current loss evidence and all possible CPUs' fatal latches
before and after each bounded drain. Failed status reads do not establish absence
of loss. Malformed current-epoch records, local lifecycle capacity failure and ID
exhaustion also invalidate timing. Old-epoch records are discarded.

On recoverable loss, reset all calibration and authoritative departure/lifetime
caches, preserving the configured baseline policy. Withhold TCP queue validity
for at least one second. Then publish the next epoch, reset owned state and allow
fresh calibration from new evidence. Retain every old loss marker. ACK/upload
counter maps and the packet socket stay active across this timing recovery.

A fatal latch or exhausted epoch budget disables timing until a genuine capture
recreation. Incrementing the epoch cannot clear a fatal latch. Status-read failure
also withholds confidence; recovery must never assume a failed lookup means the
marker was absent. There is no unconditional reopen loop under sustained loss.

Ring-full diagnostics remain distinct from lost-lifecycle confidence. Any required
record loss is conservatively treated as stream uncertainty because the consumer
cannot prove that losing it was harmless. This may reduce availability under load;
the later coverage/cost gate must evaluate it.

## Native lifecycle transitions

The fixed lifecycle table retains identities within an epoch; it does not evict
one tuple and then bootstrap that tuple while its older history is still trusted.
Full capacity invalidates the broader timing stream and uses epoch recovery.
Departure-cache replacement may only make an echo unmatched; it cannot match a
different lifetime or change the first departure of an existing key.

| Evidence | Transition / measurement effect |
| --- | --- |
| First ordinary timestamped packet with no prior lifecycle evidence | Allocate a bootstrap generation with unknown provenance; begin fresh calibration. |
| New SYN ISN differing from the current or pending identity | Immediately forget that tuple's calibration, retain pending evidence, and withhold ordinary timing until confirmation. |
| Exact retransmission for the current active or pending handshake | Retain the generation/calibration or pending state, as applicable. |
| SYN using an older remembered ISN | Withdraw confidence before considering its pair; do not assume an old ISN could never be reused legitimately. |
| SYN/ACK acknowledging the opposite pending ISN plus one | Confirm only the matching pair. Sequence wrap is intentional modulo-32 arithmetic. |
| Confirmed current active pair retransmitted | Reuse the current generation without resetting its calibration. |
| Confirmed older pair from retained history | Do not reactivate it; leave timing unavailable. |
| Fresh confirmed pair | Allocate a new generation and start new calibration; retain the completed pair in history. |
| Simultaneous open | Keep both pending SYNs and confirm the matching pair idempotently. |
| Mismatched SYN/ACK | Cannot publish a new active generation. |
| FIN or RST | Conservatively quarantine timing while retaining confirmed identity/history; a fresh credible pair is needed to resume. |
| Record captured before the retained lifecycle boundary | Reject without refreshing estimator freshness or reintroducing old calibration. |
| Completed-pair history exhausted | Quarantine that tuple instead of silently overwriting evidence. |

SYN/SYN-ACK records do not establish a timing floor. Ordinary timestamp evidence
after confirmation does. The estimator continues rejecting reordered timestamps
within a generation and treating timestamp wrap correctly. A newer generation
resets clock fit, floors and windows; delayed lower-generation samples cannot
refresh the LRU position. Forgetting a flow must preserve correct scanning of
other estimator slots, including holes in the table.

## Deterministic traces for the unit-test sources

These define the unit-test cases and later kernel cases. Host checks exercise
native code and helper stubs; they do not establish kernel ordering. `G1` and `G2` mean
successive native generations; `E1` and `E2` mean capture epochs.

| Ordered evidence | Expected outcome |
| --- | --- |
| Bootstrap timestamp data; SYN ISN A; SYN/ACK acknowledging A+1; ordinary replacement data | Bootstrap calibration forgotten; only the confirmed pair feeds fresh calibration. |
| Confirmed G1; replacement with lower timestamp offset; sufficient ordinary clock-fit data | G2 starts a new clock fit rather than rejecting every sample against G1. |
| Confirmed G1; replacement with higher timestamp offset | G2 cannot reuse G1's floors or departure cache. |
| Confirmed pair (A, B); SYN A; SYN/ACK with peer ISN C | Keep enough pending evidence to recognize the new pair (A, C); allocate G2. |
| Confirmed pair (A, B); retransmitted SYN A and matching SYN/ACK B | Preserve G1 for the complete current pair. |
| Both sides' pending SYNs; matching SYN/ACKs in either direction | Confirm the same simultaneous-open pair idempotently. |
| New boundary; older captured SYN, SYN/ACK, FIN, RST or ordinary record reserved later | Ignore the older record before any state mutation or estimator refresh. |
| G2; delayed G1 estimator sample | Reject G1 without moving calibration, freshness or LRU backward. |
| First departure (tuple, G1, TSval); duplicate departure; echo | Match the first retained departure; the same TSval under G2 cannot match G1. |
| A new outgoing TSval first observed inside the thinning interval; later repeat | Skip that value consistently rather than recording the later repeat as its first departure. |
| E1 record reserved; required publication fails; current loss marker observed | Withdraw all E1 timing confidence, including records consumed during that drain. |
| E1 recovery cooldown; publish E2; delayed E1 marker/record arrives | Retain old loss evidence, discard old records, calibrate only E2; ACK counter baselines continue. |
| Loss-marker insertion fails and lookup finds no marker | Sticky fatal on any possible CPU disables timing until capture recreation. |
| Lifecycle table full, exhausted generation ID, exhausted pair history | Full table/ID invalidate the epoch; pair-history exhaustion quarantines that tuple without replacement. |
| Status lookup fails, epoch budget exhausted, or ring remains backlogged | No valid controller queue input; fatal/read/exhaustion remain disabled, backlog only vetoes that snapshot. |

Concurrency fixtures must respect synchronous packet-hook causality. A successor
packet that depends on delivery of the SYN/SYN-ACK must not be injected while its
required callback is artificially held before publication. Delayed predecessor
traffic may be interleaved, and must never reintroduce retained old calibration.

## Numerical bounds and later acceptance gates

These are proposed acceptance limits, not measured results:

- 1,024 native lifecycle slots, 8,192 native departure entries, and eight retained
  completed pairs per tuple; native cache storage at most 1 MiB.
- Existing 256 KiB ring and 4 ms ordinary timing cadence. A drain processes at
  most 4,096 records before yielding; busy/backlogged data vetoes that snapshot.
- One-second timing recovery cooldown, 64 epochs per capture, no acquisition
  retry/spin loops and no deletion of live loss markers.
- With uninterrupted qualifying timestamped traffic at the supported clock
  cadence, recovery/calibration should provide usable fresh measurement within
  four seconds of detected recoverable loss. No bound is claimed when qualifying
  traffic or independent confidence is absent.
- On identical steady workloads and execution modes, target BPF time per call
  at most twice the current baseline, and incremental combined BPF/daemon CPU
  at most two percentage points of one core. These provisional limits must be
  accepted or revised before a performance batch; do not revise after seeing
  a failed run simply to obtain a pass.
- Steady eligible-flow measurement coverage at least 90% of the baseline under
  the same traffic. Forced failure cases must withdraw confidence regardless
  of their coverage. ACK accounting retains its existing semantic expectations.

Excessive invalidation, history exhaustion or recovery cycles are acceptance
failures or explicit capacity limits, not permission for more soak tests or
controller retuning. Existing end-to-end latency/throughput requirements remain
separate, including the unresolved ACK-control regression.

## Architecture inventory and present limits

✓ The existing x86/generic and mediatek/filogic package builds and native ABI
assertions pass. The [detailed capability inventory](TUPLE_LIFETIME_CAPABILITIES.md)
records exact local compiler/kernel/library metadata, packaged artifact hashes,
emitted helper instructions and missing resources for every other required row.
No VM was accessed for those inventory/build checks.

The available SDK headers expose the bounded consume and ring-position APIs.
Availability on every selected SDK is still a matrix task. The common algorithm
uses ordinary hash/LRU/per-CPU-array/ring helpers and no CPU-family branches;
this alone is not evidence that every target kernel enables them or JITs the
resulting instructions. Big-endian transfer, remaining native ABIs, actual kernel
paths and cost remain open. Historical x86 kernel 6.12.108 is not a live refresh,
and the available arm64 emulator supplies functional coverage only.

## Host verification at the end of implementation

Focused strict-warning builds and units cover the estimator, native lifetime
processor, real filter source with mocked helpers, real capture source with mocked
map/ring/clock calls, monitor queue/ACK gating, and existing ACK accounting.
Both lower and higher replacement timestamp offsets regain a valid directional
pair after fresh synthetic calibration; predecessor generations leave it unchanged.
The unchanged controller replay matches all 4,703 decisions from its two traces.
AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer pass for native
estimator, lifetime, capture and monitor units. LeakSanitizer initially could not
run under the sandbox's tracing; the same binaries passed outside that sandbox.

The final host batch caught missing test headers/a logging stub and an echo-test
fixture that inherited the preceding case's deliberately poisoned epoch. Those
test sources were corrected; production loss semantics were retained. No VM,
profiling or end-to-end work was performed. These results close host checks only;
they do not close the architecture, kernel, lifecycle or cost acceptance gates.

## Passive-observation limits

Loss before the observation point, parse coverage gaps, an exactly reused tuple
and handshake identity, or a delayed on-wire handshake after a complete capture
reset may be indistinguishable from new evidence. The design does not authenticate
packets or implement TCP receive windows. A delayed predecessor FIN/RST may
conservatively withdraw a newer connection's timing; it must retain identity
instead of silently switching back to an old lifetime. A flow starting during
congestion still has unknown baseline provenance.

These limitations do not excuse detected internal reset-evidence loss or retained
old calibration being reused after a credible new handshake. Those are the
specific failures this implementation is intended to solve.
