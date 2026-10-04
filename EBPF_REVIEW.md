# eBPF TCP-delay review

Review date: 2026-10-03. Source snapshot: `7b52801304cccb224cef2a08ef9ba31d3e4d23f8`.
The working tree was clean before this document was added.

The original findings and reproductions below describe that snapshot. The first
aggregation fix is now implemented and described under [Fix 1](#fix-1-2026-10-04),
with [separate before/after evidence](profiling/2026-10-04-flow-pair/README.md).
The remaining four findings have not been changed by that fix.

## Evidence status update (2026-10-04)

The attempted VM run did **not** validate the eBPF estimator or any of the
findings below. Its [`cake-adapt.log`](profiling/2026-10-04-ebpf-vm/raw/vm-ebpf/cake-adapt.log)
contains zero `TCP_QUEUE` records, 5,574 `DATA` records, and 1,119 `SHAPER`
records. The earlier report mistakenly presented fping RTT/2 statistics as
eBPF results. The reported correlations of 0.96 and 0.91 and 73.9% download
false-alarm rate are withdrawn; they do not validate estimator accuracy or an
aggregation weakness. The phase marks also include interrupted and repeated
runs, and the copied binary and filter object were not verified against the
reviewed source or runtime attachment. See the
[corrected VM run audit](profiling/2026-10-04-ebpf-vm/README.md) for the raw
record counts and remaining verification gaps.

The first four findings below remain host-side synthetic estimator
reproductions. The fifth is a source-based accounting risk. None of these five
findings has been live-validated by that earlier attempted VM run. This update does not
replace or invalidate the reproducible host examples; it narrows what the
failed VM attempt can support.

The option called `upload_ack_share_min` at the review snapshot was renamed to
`ul_congest_ack_share` (`OPTION_UL_CONGEST_ACK_SHARE`) by commits `034c1b5` and
`14f881b`. References below use the current option name where applicable and
identify the snapshot-era name for context.

The original review was analysis, not an implementation or deployment
recommendation. Production code, configuration, defaults, and VM state were not
modified during the original October 3 host review. The October 4 attempted VM
run is documented separately above and in its linked audit. The examples below
exercise the reviewed userspace estimator snapshot; they do not establish how
frequently these cases occur on an internet connection.

## Main conclusion

The eBPF capture is a reasonable way to obtain an extra measurement without
changing CAKE's scheduling. The principal weakness is the confidence assigned
to that measurement: a fresh, numerically valid result is treated as strong
enough evidence to redistribute fping's latency and exclude a direction from
congestion backoff.

However, a valid result can mean any of these things:

- a mature flow measured an increase above a previously observed low-delay state;
- a new flow established its baseline while the link was already congested;
- an old baseline expired while the queue remained full; or
- a receiver changed its ACK timing, without a change in link queueing.

Those are not equivalent observations. The current interface does not express
the difference.

Keep the distinction between the two eBPF-dependent features:

| Feature | Measurement it uses | Main risk discussed here |
| --- | --- | --- |
| `tcp_delay_attribution` | TCP timestamps and matched departures | Incorrect directional confidence can misattribute fping delay. |
| `ul_congest_ack_share` (named `upload_ack_share_min` at the review snapshot) | Upload byte counters and pure-ACK byte counters | Counted bytes need not equal CAKE's bandwidth-accounted bytes. |

These are deliberate local extensions, not defects attributed to
[cake-autorate](https://github.com/lynxthecat/cake-autorate), whose controller
design is the project's upstream reference.

## Findings at a glance

Priority is a proposed review order, not a claim about observed customer impact.

| Priority | Finding | Evidence | Possible consequence |
| --- | --- | --- | --- |
| High | A newly calibrated flow can hide an established download queue. | Estimator reproduction: 80/20 ms becomes 0/20 ms. | Controller attributes all measured delay to upload and excludes download from the congestion-cut path. |
| High | Reusing a connection tuple can preserve the previous connection's timestamp state. | Estimator reproduction: neither direction is valid after seven seconds of replacement traffic. | TCP attribution becomes unavailable until recovery or replacement of that state. |
| Medium | A persistent queue becomes the new baseline. | Estimator reproduction: unchanged 80/20 ms becomes 0/0 ms after about a minute. | Directional evidence disappears; the controller returns to its delivery-rate heuristic. |
| Medium | A sustained change in receiver ACK delay resembles upload queueing. | Estimator reproduction: no link queues, but 40 ms is reported on upload. | If fping also detects delay, its attribution can be wrong. |
| Medium | Raw packet bytes and CAKE-accounted bytes are different quantities. | Source inspection and kernel accounting rules; no live reproduction in this review. | The ACK allowance can be optimistic on some link configurations. |

The first four results were reproduced on the host with the reviewed estimator snapshot.
Their downstream rate-control consequences are derived from source inspection,
not from a new end-to-end VM run.

## 1. Current measurement-to-control path

```text
WAN upload interface
  outgoing packet after root qdisc -> departure map: (flow, TSval) -> local time
  incoming TCP packet             -> remote TSval + echoed departure, if found
                                      |
                                      v
                               eBPF ring buffer
                                      |
                                      v
                           capture.c drains samples
                                      |
                                      v
                         estimator.c maintains flows
                         - fit remote timestamp tick
                         - subtract each flow's floor
                         - calculate each flow's window minimums
                         - select one flow to supply the available directions
                                      |
                                      v
                         monitor/tcpdelay.c supplies
                         download/upload queue + validity
                                      |
                 fping RTT delta -----+
                                      v
                         controller.c splits RTT delta
                         and assigns directional blame
                                      |
                                      v
                         existing CAKE bandwidth update
```

Relevant sources:

- [`tcpdelay.bpf.c`](src/tcpdelay/tcpdelay.bpf.c): parsing, departure matching,
  counters, sampling, and ring-buffer submission.
- [`record.h`](src/tcpdelay/record.h): shared layouts and filter limits.
- [`capture.c`](src/tcpdelay/capture.c): libbpf loading, attachment, consumption,
  and cleanup.
- [`estimator.c`](src/tcpdelay/estimator.c): flow lookup, tick fitting, floors,
  and measurement windows (per-flow after the first fix below).
- [`monitor/tcpdelay.c`](src/monitor/tcpdelay.c): capture lifecycle and conversion
  into controller inputs.
- [`controller.c`](src/controller/controller.c): `controller_update()`,
  `adjust_rate()`, and `download_ceiling()`.
- [`defaults.h`](src/config/defaults.h): measurement windows and policy thresholds.

### What is actually measured?

Use these symbols, expressed in one common time unit:

```text
A = local arrival time of the incoming TCP packet
D = local departure time of the packet whose timestamp it echoes
R = remote TSval progress multiplied by the fitted remote clock tick

download raw value = elapsed local arrival time - R
upload raw value   = R - relative local departure time

reported queue = raw value - minimum raw value retained for that flow
```

The code uses offsets relative to the first arrival, so the absolute remote
clock offset need not be synchronized with ours. A matched departure is needed
for upload; an incoming timestamp alone can produce a download observation.

Important: these are **changes above a per-flow floor**, not a direct reading of
an ISP's backlog. Interpreting that change as an actual queue assumes that the
floor represents a sufficiently uncongested path and that other timing effects
are small or filtered out.

Current operational values:

| Item | Value | Meaning |
| --- | --- | --- |
| Remote tick candidates | 1, 4, 10, 100 ms | The estimator snaps to one candidate within 5%. |
| Tick-fit history | At least 2 seconds | Calibration duration, not proof that the path was uncongested. |
| Per-flow floor | Current and preceding 30-second buckets | Old low-delay observations eventually expire. |
| Result windows | Current and preceding 100-ms windows | At the reviewed snapshot, independent minimums across flows; after fix 1, per-flow minima with one selected flow supplying the directional pair. |
| Userspace flow slots | 32 | New tuples replace the least recently seen slot when full. |
| Attribution gate | Both directions valid and sum at least 5 ms | Below this, the existing delivery-rate heuristic remains in use. |
| Directional blame | At least one quarter of the measured total | A smaller share is excluded from the congestion-cut path. |

The filter records departures at most once per flow per 4 ms; unmatched incoming
samples are also rate-limited. A reply with a changed echo matching a recorded
departure can bypass the unmatched-reply interval check. Consequently, “every
incoming record is independently capped at 4 ms” is not the exact implementation
contract.

### What does the controller do with the estimate?

For a shared-delay pinger, when the attribution gate passes:

```text
total = measured download queue + measured upload queue

download attributed = download queue >= total / 4
upload attributed   = upload queue   >= total / 4

download delay input = fping round-trip delta * download queue / total
upload delay input   = remaining fping round-trip delta
```

The code uses multiplication to avoid truncating the quarter-share comparison.
The estimator's queue total does not need to agree with fping's delta before
that proportional split is accepted.

In `adjust_rate()`, a congestion reduction requires congestion detection,
directional attribution, and an elapsed refractory period. The TCP result is
therefore more than informational: it can remove the permission to cut a
direction's rate. Other rate paths, including the ACK ceiling, still exist;
this is not a claim that every possible rate change is blocked.

## 2. Finding: a new flow hides an existing queue

### Reproduced sequence

1. Flow A sends timestamped samples for three seconds on an uncongested path.
2. Its baseline and 1-ms remote clock are established.
3. The path gains an 80-ms download queue and a 20-ms upload queue.
4. A reports the expected 80/20 ms.
5. Flow B starts while those same queues remain present. B has incoming TCP
   timestamps but no matched departure in the supplied records.
6. B calibrates against the already-congested path. Its download queue is
   constant relative to its own starting floor, so it reports zero.
7. The aggregate download window accepts B's zero as its minimum. The aggregate
   upload window still receives A's 20 ms.

| Observation | Actual supplied download queue | Actual supplied upload queue | Aggregate download estimate | Aggregate upload estimate |
| --- | ---: | ---: | ---: | ---: |
| Only mature A | 80 ms | 20 ms | 80 ms, valid | 20 ms, valid |
| A plus newly calibrated B | 80 ms | 20 ms | 0 ms, valid | 20 ms, valid |

The problem is not that B failed to fit its clock. Its clock fit is correct.
The problem is treating B's unknown initial queue as a known empty queue and
letting its relative zero override A's already-established evidence.

### Why that matters to control

Suppose fping's added RTT is 100 ms:

```text
Before B contributes:
  TCP queues = 80 + 20 = 100 ms
  download gets 80 ms; upload gets 20 ms
  download has >= 25% of the queue; upload does not

After B contributes:
  TCP queues = 0 + 20 = 20 ms
  total still exceeds the 5-ms attribution gate
  download gets 0 ms; upload gets all 100 ms
  download is no longer attributed congestion
```

The physical queue in this example did not move from download to upload. Only
the composition of the aggregate estimate changed.

If B supplies zero for **both** directions and both aggregate minimums become
zero, the total falls below 5 ms and the controller falls back to its heuristic.
That is a different outcome. The reproduction intentionally gives B only a
download observation to demonstrate the dangerous mixed result.

### Classification and proposed direction

This is a confirmed aggregation/confidence weakness. Passive observation cannot
prove that a new flow's initial state was uncongested merely by waiting two
seconds. Simply increasing the warm-up interval is not a complete solution.

Consider retaining baseline provenance and preventing an unqualified new-flow
zero from vetoing mature evidence. Directional minimums from different flows or
destinations also should not automatically be treated as one coherent path
measurement. The exact confidence/fallback policy needs measurement before it
is chosen; replacing minimums with maximums blindly could overreact to outliers.

### Fix 1 (2026-10-04)

The short download and upload windows now belong to each flow. Result selection
ignores expired windows, prefers a complete directional pair, then chooses the
flow whose history began earliest. Both values therefore come from one flow,
instead of combining independently calibrated baselines. Minima within that
flow's windows, clock fitting, floor retention and controller policy are unchanged.

The exact synthetic 80/20-ms case now remains 80/20 ms when a new flow joins,
whether it has only download observations or a complete pair. When established
samples expire, the estimate switches to an eligible fresh flow; it does not
keep the old upload alongside a new flow's download. See
[raw before/after outputs and reproduction instructions](profiling/2026-10-04-flow-pair/README.md).

The full host suite, both upstream replays (4,703 decisions, no mismatch),
ASan/UBSan with leak detection, x86 SDK build, and the focused 32-bit VM unit
test pass. A separate isolated live run verified actual filter attachment and
457 `TCP_QUEUE` records. After a new connection calibrated, the download
estimate remained near the injected 80-ms path-delay increase. That is a
functional integration check, not a measured reduction in bufferbloat or a
live before/after accuracy benchmark; see the linked evidence for raw records
and limits. The original installed package, service and qdisc settings were
unchanged and the test-log inode was preserved.

This is a limited confidence policy, not proof of an empty baseline. A complete
pair can outrank an older download-only history. If every fresh flow began
during congestion, its unknown initial queue remains unknowable here. Tuple
reuse, floor expiry, receiver delay and byte accounting are separate unresolved
findings. The original reproduction and its historical output below remain
unchanged for review.

### Remaining confidence limit: every flow starts during congestion

Fix 1 preserves an established flow's estimate when a later flow joins. It
cannot help when all eligible flows establish their floors after a queue is
already present. For example, if every flow starts while the path has an
80-ms download queue and a 20-ms upload queue, each flow records those delays
as part of its own initial floor. Their later relative estimates can all be
0/0 ms. Selecting the oldest flow does not provide clean history when that
oldest flow also began during congestion. Near-simultaneous starts provide no
meaningful confidence advantage, and exact first-arrival ties are resolved by
flow-table iteration order.

With both directional windows valid but their sum below the 5-ms gate, the
controller falls back to its existing delivery-rate heuristic. That fallback
does not establish which direction contains the queue. Do not assume fping
will recover the missing evidence: if its own latency baseline was also
established after congestion began, the added delay may already be in that
baseline. A previously established clean fping baseline is a different case,
but does not make the passive directional estimate trustworthy.

This uncertainty is not fixed by issue 1, a longer wait, or replacing minima
with maxima or medians. No statistic over these relative measurements can
reconstruct an initial queue that was never observed before calibration. The
all-flows-start-congested case remains an explicit confidence limitation for
any later policy decision.

## 3. Finding: a reused tuple inherits an old timestamp clock

### Reproduced sequence

The userspace key contains the endpoint addresses and ports. It does not include
a connection generation, and `flow_for()` returns an existing matching tuple
without an idle-expiry check.

An earlier connection establishes a remote timestamp near 1,000,000 ticks.
A replacement connection with the same tuple starts with an offset near 100
ticks. The subtraction against the old last timestamp produces a negative
signed step, so the sample is rejected as reordered.

However, `last_seen_ns` is refreshed **before** that rejection. Replacement
traffic can therefore keep the stale slot looking recently active while none
of its samples are accepted.

Observed result after seven seconds of replacement traffic:

```text
download valid = false
upload valid   = false
```

This is not an assertion that all tuple reuse fails. A different new offset may
take another path. This specific legal timestamp change demonstrates that tuple
identity alone is insufficient to identify a continuous remote clock.

Per-connection random timestamp offsets are recommended by
[RFC 7323, section 7](https://www.rfc-editor.org/rfc/rfc7323.html#section-7).
The new connection need not preserve the old connection's timestamp value.

### Effect and proposed direction

In this example TCP attribution becomes unavailable and fping remains available.
The daemon does not need to terminate. Persistent unrelated traffic can also
evict the slot, but relying on eviction is not a bounded recovery policy.

Distinguish connection restart from reordering using credible lifecycle or
expiry evidence, then restart calibration when justified. Keep the signed
timestamp rollover handling: resetting on every negative step would incorrectly
treat ordinary reordering as a new connection.

The BPF departure map is separately keyed by tuple and TSval and uses
`BPF_NOEXIST`. Stale departure collisions across connection generations deserve
a kernel-path test too; this review's userspace reproduction does not prove
that additional case.

## 4. Finding: a standing queue becomes the baseline

### Reproduced sequence

```text
0-3 seconds:   flow sees no queue and establishes its floor
3-4 seconds:   download queue = 80 ms, upload queue = 20 ms
4-62 seconds:  the same queues remain continuously present
```

Output near the beginning: 80/20 ms, both valid.

Output at the end: 0/0 ms, both valid.

`floor_update()` keeps the minimum in the current and preceding 30-second
buckets. Once the old empty-queue observations leave both buckets, the smallest
remaining raw values already contain the standing queue. Subtracting that floor
then reports zero. The exact transition time depends on the bucket phase;
the two-bucket scheme does not preserve an empty-path baseline indefinitely.

### Why the design exists

Rolling floors allow clock drift or genuine path-delay changes to be followed.
This is an intentional trade-off, not a memory-management error. With passive
timestamps alone, persistent queueing and an increased propagation delay can
look similar.

### Correctly bounded conclusion

This case loses directional TCP evidence. It does **not** disable fping or all
congestion handling: a 0/0-ms total fails the 5-ms gate, so the controller uses
its delivery-rate attribution heuristic again.

The review question is whether that loss of evidence during sustained congestion
is acceptable. Consider separating clock-drift compensation from upward baseline
adaptation, or inhibiting baseline rise when independent evidence indicates
congestion. Any such rule must also handle real route changes and must not
permanently freeze a stale floor.

## 5. Finding: sustained delayed ACKs look like upload queueing

### Concrete example

Imagine an outgoing packet crossing an unchanged upload path. The remote host
waits 40 ms before sending a reply that echoes its timestamp. The interval from
our departure to the reply's remote timestamp includes that receiver wait.

The upload raw value therefore contains both network timing and remote response
timing. The comment that only upstream delay varies is conditional, not a
general TCP guarantee.

The existing delayed-ACK unit test includes frequent replies with no added ACK
wait. A minimum can select those prompt replies and reject the delayed ones.
That behavior is useful, but it depends on prompt replies being present.

The additional reproduction does this instead:

1. Calibrate for three seconds with no queues and prompt replies.
2. Keep both link queues at zero.
3. Add a 40-ms receiver wait to every subsequent reply for one second.

Observed result:

```text
download = 0 ms, valid
upload   = 40 ms, valid
```

No prompt reply remains in the result windows, so the window minimum cannot
remove the changed receiver delay. A constant receiver wait present from the
start can be absorbed into the initial floor; a change after calibration is
the failure demonstrated here.

### Effect and proposed direction

This alone does not demonstrate a rate cut: rate control still uses fping
latency and the normal congestion conditions. It does demonstrate false
directional evidence. If an unrelated download queue raises fping RTT at the
same time, that RTT can be incorrectly assigned to upload.

Treat this as a measurement ambiguity, not something solved by another map or
a more precise local timestamp. Consider consistency checks against independent
latency evidence and conservative fallback for unsupported directional claims.
Validate with sustained ACK-policy changes, not only intermittent delayed ACKs.

## 6. Finding: ACK-byte accounting is not CAKE bandwidth accounting

The filter increments its outgoing and pure-ACK counters with `skb->len`.
`download_ceiling()` then subtracts the derived rates from the configured upload
shaper bandwidth and reserves headroom in that same bandwidth budget.

CAKE's effective packet charge can include configured overhead, minimum packet
size, and ATM/PTM framing. Its GSO handling can also account for segment headers.
Thus, a raw observed byte rate is not automatically a byte rate in the shaper's
accounting model. See Linux v6.12
[`cake_calc_overhead()` and `cake_overhead()`](https://raw.githubusercontent.com/torvalds/linux/v6.12/net/sched/sch_cake.c).

Illustrative arithmetic, not a measured link configuration:

```text
Assume a pure ACK is observed as 80 bytes but CAKE charges 100 bytes.
At 1,000 ACKs per second:

  counter rate = 80 * 1,000 * 8  = 640 kbit/s
  shaper cost  = 100 * 1,000 * 8 = 800 kbit/s

  difference = 160 kbit/s
```

On a slow upload, this can be a meaningful part of the capacity intended to
remain available for other traffic. Both ACK and non-ACK accounting need
consistent units; correcting only one counter can still give the wrong budget.

This review confirms the raw-counter implementation and the kernel's accounting
distinction. It does not quantify the error on a customer's actual overhead,
encapsulation, offload, or CAKE settings.

Proposed direction: establish a common accounting basis for the measured traffic
and upload budget, preferably from existing kernel data where suitable. Do not
reimplement CAKE scheduling or duplicate its entire accounting machinery merely
to fix this estimator. Test small packets and configured overhead explicitly.

## 7. What is sound, and what remains unproven

### Keep the existing strengths

- The measurement/controller/kernel-update boundaries remain explicit.
- libbpf owns loading and ring-buffer mechanics; the estimator is directly
  unit-testable without kernel attachment.
- Optional TCP capture failure is reported as degradation, not daemon failure.
- State resets when capture is opened for a recreated interface.
- Timestamp rollover and reordered samples have explicit handling.
- Capture uses bounded maps and a bounded ring buffer.
- The monitor already drains the ring on traffic ticks as well as controller
  runs, including when pingers are idle. Earlier evidence of idle-related ring
  accumulation should not be presented as a confirmed current defect.

### Avoid overclaiming the observation point

The normal transmit packet tap runs before the driver's transmit call. The
receive packet tap is before normal ingress classification/redirection. This
supports the selected post-qdisc/pre-IFB observation design, but it does not
isolate every delay to the ISP: driver, hardware, receiver, and path effects
can remain. Kernel reference:
[`xmit_one()` and `__netif_receive_skb_core()`](https://raw.githubusercontent.com/torvalds/linux/v6.12/net/core/dev.c).

### Coverage and performance still need targeted verification

TCP without timestamps cannot supply this queue measurement. The parser handles
IPv4 TCP and IPv6 with TCP directly following the IPv6 header; it does not walk
IPv6 extension headers. Capture visibility for the chosen logical WAN device,
encapsulation, and offload settings must be checked on the actual deployment.
These are coverage limits, not proof that every unsupported packet causes a bug.

The tick whitelist also limits accepted remote clocks. Snapping avoids a known
raw-fit drift problem, but it is not a general remote-clock synchronizer.

The new-flow reproduction combines flows with different calibration histories
on the same supplied path. Different internet destinations add another concern:
per-direction minimums can originate from unrelated routes. That broader case
was not independently reproduced here.

Previously measured benefits and costs remain useful evidence within their
tested conditions:

- [Design history](profiling/2026-10-03-ebpf-design-history/README.md).
- [Queue-split runtime comparison](profiling/2026-10-02-tcp-queue-split/README.md).
- [ACK-share runtime comparison](profiling/2026-10-03-ack-share-dynamic/README.md).
- [Filter cost](profiling/2026-10-03-ebpf-filter-cost/README.md).
- [Sampling trade-off](profiling/2026-10-03-sample-thinning/README.md).
- [Emulated arm64 functional verification](profiling/2026-10-03-arm64-vm/README.md).

Those successful runs do not invalidate the counterexamples. Conversely, the
counterexamples do not establish that the option performs worse in every
workload. No new physical Filogic timing, CPU, leak, or sanitizer claim is made
by this review.

## 8. Reproduction and raw output

The original host reproduction compiled the reviewed `estimator.c` with strict warnings.
It reuses the test's synthetic flow generator and runs its estimator tests
first. At the review date, the extra cases only printed observations; fix 1
has since added permanent assertions for the new-flow case. Running this
harness against the changed source now retains 80/20 ms in that case. The
historical output below is not the current expected result for fix 1; use its
[separate probe and preserved before source](profiling/2026-10-04-flow-pair/README.md)
for an explicit before/after comparison.

Save the following as a temporary `review.c`. From the repository root, compile
with `-I.` and `-Isrc` as shown below. No VM, libbpf, socket privileges, or
OpenWrt SDK is needed for this estimator-only check.

```c
#define main existing_estimator_tests
#include "tests/tcpdelay/test_estimator.c"
#undef main

static void print_result(const char *label, struct tcpdelay_estimator *estimator, uint64_t now)
{
	struct tcpdelay_estimate result;

	result_after(estimator, now, &result);
	printf("%s: dl=%lld us (%d), ul=%lld us (%d)\n",
	       label,
	       (long long)result.download_queue_microseconds,
	       result.download_valid,
	       (long long)result.upload_queue_microseconds,
	       result.upload_valid);
}

int main(void)
{
	struct tcpdelay_estimator estimator;
	struct remote a = remote_flow(50001U, MILLISECOND, 1000000U);
	struct remote b = remote_flow(50002U, MILLISECOND, 2000000U);
	uint64_t now;

	existing_estimator_tests();

	/* Mature congested A, then B calibrates on the same congested path. */
	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &a, 0U, 3000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&a,
		now,
		4000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	print_result("known congested flow", &estimator, now);
	b.departures = false;
	for (; now < 7000U * MILLISECOND; now += MILLISECOND) {
		send_sample(&estimator, &a, now, 80U * MILLISECOND, 20U * MILLISECOND, 0U);
		send_sample(&estimator, &b, now, 80U * MILLISECOND, 20U * MILLISECOND, 0U);
	}
	print_result("same queues, with new download flow", &estimator, now);

	/* Same tuple, replacement connection with a lower timestamp offset. */
	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &a, 0U, 3000U * MILLISECOND, 0U, 0U);
	a.tsval_base = 100U;
	now = send_span(&estimator, &a, now, 10000U * MILLISECOND, 0U, 0U);
	print_result("reused tuple, seven seconds later", &estimator, now);

	/* Keep a standing queue beyond the retained empty-path floor. */
	a.tsval_base = 1000000U;
	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &a, 0U, 3000U * MILLISECOND, 0U, 0U);
	now = send_span(
		&estimator,
		&a,
		now,
		4000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	print_result("persistent queue initially", &estimator, now);
	now = send_span(
		&estimator,
		&a,
		now,
		62000U * MILLISECOND,
		80U * MILLISECOND,
		20U * MILLISECOND
	);
	print_result("identical persistent queue after a minute", &estimator, now);

	/* No network queue; only the remote response delay changes. */
	tcpdelay_estimator_init(&estimator);
	now = send_span(&estimator, &a, 0U, 3000U * MILLISECOND, 0U, 0U);
	for (; now < 4000U * MILLISECOND; now += MILLISECOND)
		send_sample(&estimator, &a, now, 0U, 0U, 40U * MILLISECOND);
	print_result("no link queues, remote ACK delay changed to 40ms", &estimator, now);
	return 0;
}
```

Compile and run, substituting the temporary file locations you chose:

```sh
cc -std=c11 -O2 \
  -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror \
  -I. -Isrc /tmp/review.c src/tcpdelay/estimator.c -o /tmp/review
/tmp/review
```

Output observed in this review:

```text
tcpdelay estimator tests passed
known congested flow: dl=80000 us (1), ul=20000 us (1)
same queues, with new download flow: dl=0 us (1), ul=20000 us (1)
reused tuple, seven seconds later: dl=0 us (0), ul=0 us (0)
persistent queue initially: dl=80000 us (1), ul=20000 us (1)
identical persistent queue after a minute: dl=0 us (1), ul=0 us (1)
no link queues, remote ACK delay changed to 40ms: dl=0 us (1), ul=40000 us (1)
```

Parentheses contain validity: `1` means valid, `0` means unavailable. A zero
printed with validity `0` is not a measured empty queue.

Limits of this harness:

- It injects userspace samples directly, at a synthetic 1-ms cadence, bypassing
  the BPF filter's sampling and departure matching.
- The remote clocks are ideal 1-ms clocks; packet generation, delay, and tuple
  changes are explicitly supplied.
- It runs existing estimator tests, not the full host suite or controller replay.
- It shows estimator behavior, not real packet loss, live shaper rates, CPU
  overhead, or customer impact.

## 9. Working order and status

This was the original proposed order. Fix 1 above implements the scoped
new-flow aggregation change and its regression tests. The other findings and
the broader confidence policy remain for separate decisions.

1. **Lock down the demonstrated cases with regression tests.** Add assertions
   for new-flow baseline contamination, tuple reuse, persistent queueing, and
   sustained receiver delay. Retain reordering and rollover coverage. Add a
   controller test showing exactly when questionable evidence overrides the
   fallback heuristic.
2. **Define measurement confidence before rate policy.** Distinguish fresh
   samples from a credible baseline and compatible flow/path evidence. Specify
   when the controller may use a queue ratio and when it must fall back. Do not
   assume a zero proves absence of congestion.
3. **Repair flow lifetime and aggregation together with focused tests.** Bound
   recovery after credible connection restart without destroying reorder
   filtering. Prevent a new unknown baseline from silently overriding mature
   directional evidence. Check BPF departure generations independently.
4. **Resolve baseline/ACK-delay ambiguity explicitly.** Choose a documented,
   measured trade-off for persistent queues, clock drift, path changes, and
   receiver timing. Avoid replacing a short implementation with a complex
   estimator whose confidence still cannot be justified.
5. **Validate the ACK accounting basis.** Exercise CAKE overhead and minimum
   packet sizes, then measure whether the dynamic allowance actually leaves the
   intended upload room. Keep plain `ack-filter`, not aggressive filtering.
6. **Run controlled before/after evidence.** Compare the current option-on
   behavior, options-off baseline, and proposed changes under the same workloads.
   Include flows starting after congestion, short-lived connection churn,
   queues lasting more than two floor buckets, sustained delayed ACKs, and
   small-packet upload. Include download, upload, bidirectional load, recovery,
   and qdisc lifecycle checks.

Acceptance should include added-delay percentiles, time bufferbloated,
throughput, wrong-direction attribution, rejected samples, capture loss, CPU,
and memory. Keep at least about 85% of capacity unless a different trade-off is
explicitly agreed. Keep the upstream decision replay matching with departures
disabled. Preserve raw evidence so results can be reviewed independently.

The most valuable first change is not replacing eBPF. It is preventing
uncertain passive timing evidence from confidently vetoing the response to
real congestion.
