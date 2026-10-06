# Why TCP-measured queues and ACK accounting, not fping alone

This is a design record, not raw evidence: it walks through the steps that
led to `tcp_delay_attribution` and `upload_ack_share_min`, with the numbers
that decided each step. The intermediate code and full per-step evidence are
not kept in this repository; this page is what replaces them. The final
evidence (fping alone vs. the shipped eBPF behavior) is in
[`2026-10-02-tcp-queue-split`](../2026-10-02-tcp-queue-split/README.md) and
[`2026-10-03-ack-share-dynamic`](../2026-10-03-ack-share-dynamic/README.md).

All of this is in service of one goal: reduce bufferbloat on an asymmetric
line, specifically the owner's (download 50/60/70 Mbit/s, upload 1/3/5
Mbit/s, min/base/max). Nothing here is unrelated work.

## The problem with fping alone

cake-autorate's reference pinger, `fping`, reports one round-trip delay.
cake-adapt splits it into "download's delay" and "upload's delay" by
assuming each direction gets RTT/2. Two failure modes follow from that:

1. **A one-sided queue reads at half its real size**, and the other,
   unaffected direction reads a false delay of the same size. A 170 ms
   upload queue shows as roughly 85 ms on each side.
2. **Download's ACKs queue behind upload**, so upload bloat delays
   download's measured RTT even when download has no queue of its own.
   Delivery-rate heuristics (is download delivering its full shaper rate?)
   cannot tell that apart from real download bloat.

Both are structural: no amount of threshold tuning on `fping`'s numbers can
fix them, because the information to tell the directions apart was never in
RTT/2 to begin with.

## Step 1: attribute by download delivery (shipped, no eBPF)

Before any kernel work, cake-adapt attributes a detected bufferbloat to a
direction using load alone: download delivering its full shaper rate has no
standing queue of its own, so the RTT is blamed on upload; download loaded
but delivering less than its shaper rate is the bottleneck itself. This
needs no extra measurement and is the default with `fping`.

**Measured effect:** on the namespace testbed (4 Mbit/s up / 65 Mbit/s down
bottleneck, the owner's shaper bounds), this stopped upload bloat from also
cutting the idle download shaper (which had been dropping to its minimum
about 23 times per upload-only phase). Under bidirectional load it raised
download from 40% to 54% of capacity by halving wrong-direction download
cuts.

**What it did not fix:** added RTT was unchanged in every phase (upload
p99 ~175 ms whether or not attribution ran). The delay itself still came
from RTT/2 under-reporting a one-sided queue; attribution only decides which
direction's *shaper* reacts, not whether the *delay measurement* is right.
This is why the next step measures the queue directly instead of inferring
it from load.

## Step 2: measure the queue directly from TCP timestamps

### Feasibility spike: a passive offline estimator

Before touching the kernel, a pure-Python offline estimator was built
against `tcpdump` captures and the namespace testbed's real ISP backlog as
ground truth, to check whether TCP timestamps carry enough signal before
spending effort on an in-kernel implementation.

Method: for each TCP flow with timestamps, relative to the flow's own
minimums,
- **downstream** = arrival time − remote TSval × remote clock tick (changes
  only with the downstream one-way delay);
- **upstream** = remote TSval × tick − our departure time of the segment
  whose TSval is echoed back (TSecr) (changes only with the upstream
  one-way delay), kept as a 100 ms window minimum because delayed ACKs only
  add to it.

The remote clock tick is fitted per flow and snapped to standard rates (1,
4, 10, 100 ms) within 5%; without snapping, a growing queue stretches
arrival times enough to drift the raw fit by about 400 ms over 20 s.

**Validation** (170,484 captured TCP packets, none dropped, against the
testbed's real backlog):

| Phase | Upstream true / estimated | Downstream true / estimated | Attribution correct |
| --- | --- | --- | --- |
| upstream bloated | 392 / 390 ms | 0 / 0.2 ms | 100% |
| downstream bloated | 0 / 0.2 ms | 48 / 50 ms | 97% |
| both shapers too high | 274 / 273 ms | 0 / 0.4 ms | 88% |
| nothing bloated | 0 / 0.2 ms | 0 / 0.2 ms | 100% |

In the "both" phase, the downstream queue never actually built up — upload
bloat was delaying download's ACKs instead — and the estimator correctly
read that as an upload-only queue. This is the exact bidirectional-load
failure mode fping/RTT-2 cannot resolve, confirmed here with real captured
traffic before any kernel code was written.

### Why eBPF instead of tc or tracepoints

The estimator needs packet timestamps at specific points: after our own
upload CAKE queue, and before the ingress redirect to the IFB, so the
measured delay is the ISP's queue, not ours. Three kernel attachment points
were considered:

- **tc `clsact`**: egress `clsact` runs *before* the root qdisc, so a
  departure timestamp taken there would include our own CAKE queueing time,
  which is exactly the delay we need to exclude.
- **Tracepoints/fentry on the real post-qdisc hooks**: precise, but need
  `BPF_EVENTS` and BTF, which OpenWrt target kernels generally do not enable.
- **A socket filter on an `AF_PACKET` socket**: sees packets exactly after
  egress qdiscs and before ingress redirection, and needs only
  `BPF_SYSCALL`, which is far more commonly available on OpenWrt targets.

The socket filter was chosen for that reason alone — not because it is the
theoretically fastest option, but because it is the one that actually works
on the kernels this package ships to.

### Spike: an eBPF socket filter, measured against `tcpdump`

A throwaway eBPF program (an `AF_PACKET` socket filter, not kept in this
repository) parsed Ethernet/IPv4/TCP timestamps directly in the kernel:
outgoing packets recorded the first departure of each (flow, TSval) in an
LRU map; incoming packets looked up the departure their TSecr echoed and
emitted a 40-byte sample to a ring buffer.

**Accuracy**, on the testbed (8 Mbit/s up / 40 Mbit/s down), scored against
`tcpdump` captured in parallel: every incoming timestamped packet (79,487)
was matched to its departure, no ring-buffer overflow, and:

| Phase | Upstream true / eBPF | Downstream true / eBPF | Attribution correct |
| --- | --- | --- | --- |
| upstream bloated | 326 / 313 ms | 0 / 0.2 ms | 100% |
| downstream bloated | 0 / 0.2 ms | 86 / 84 ms | 100% |
| both shapers too high | 351 / 345 ms | 0 / 0.3 ms | 93% |
| nothing bloated | 0 / 0.2 ms | 0 / 0.2 ms | 100% |

**CPU cost**, at a fixed rate cap (180–270 Mbit/s reached), CPU-% per
100 Mbit/s averaged over three rounds (~±10% spread between runs):

| Condition | Upload | Download |
| --- | ---: | ---: |
| nothing attached | 27 | 20 |
| eBPF spike | 30 | 23 |
| `tcpdump` | 27 | 20 |

The spike emitted one sample and one wakeup per incoming packet and
attempted an LRU-map insert per outgoing packet — both since optimized away
in the shipped filter (`tcpdelay.bpf.c`), which rate-limits samples (first to
once per TSval/TSecr change, since
[`2026-10-03-sample-thinning`](../2026-10-03-sample-thinning/README.md) to at
most one per flow per 4 ms) and submits without wakeups
(`BPF_RB_NO_WAKEUP`). This spike result is why those two optimizations were
made non-negotiable in the real implementation rather than left as later
cleanup.

### Shipped: `tcp_delay_attribution`

The real filter and estimator (`src/tcpdelay/`) generalized the spike to
IPv4 and IPv6, hardened flow-table handling, and fed per-direction queue
estimates into the controller: once the measured queues total at least
5 ms, fping's round-trip delta is split between the directions by their
measured share instead of RTT/2 each way, and only a direction holding at
least a quarter of the measured queue is cut.

**Validated under the running controller** (not just offline against
`tcpdump`), correlation against the real ISP backlog 0.85–0.95; full numbers
in [`2026-10-02-tcp-queue-split`](../2026-10-02-tcp-queue-split/README.md).
Headline: median added RTT under bidirectional load 50 → 28 ms, download
41% → 67% of capacity — the latency improvement step 1 could not produce,
because step 1 never corrected the underlying RTT/2 measurement.

## Step 3: download ACKs can fill a slow upload

Separately from queueing delay, on a highly asymmetric line (the owner's
1 Mbit/s upload class) a fast download's ACK stream can itself be a large
share of upload capacity, independent of any ISP bufferbloat. This was
raised as a direct concern: when upload is saturated, other traffic (ICMP,
UDP, a competing upload) should not be starved by ACKs.

### Baseline: how much of upload do ACKs actually take?

Measured on a 900 kbit/s upload with CAKE at fixed rates (no cake-adapt, so
only CAKE's own per-flow scheduling was in effect), classifying captured
upload packets as pure ACKs (no payload, only the ACK flag of
SYN/FIN/RST/ACK) or other:

- ACKs took 56–77% of upload across 1–8 parallel downloads.
- CAKE's per-flow fairness already protected *sparse* traffic: ICMP and a
  64 kbit/s UDP stream stayed lossless throughout, because CAKE serves
  flows below their fair share first.
- Traffic *above* its fair share was not protected: a 200 kbit/s UDP stream
  lost 10–61% of its packets with 4+ downloads, and a bulk upload got
  squeezed to 0.08–0.10 Mbit/s.
- Plain CAKE `ack-filter` (the only ACK-filter mode this project uses —
  `ack-filter-aggressive` is explicitly excluded) helped at moderate load
  (200 kbit/s stream loss 10% → 1% with 4 downloads) but little at heavy
  load (45% → 38% with 8 downloads), where ACKs still took 65–75%.

This confirmed the concern was real at the project's actual target ratio
(the owner's line is about 20:1 down:up), and that `ack-filter` alone does
not fully address it under heavy download load.

### First attempt: a fixed ACK cap

The first design held download so its ACKs never exceeded a fixed 45% of
the upload shaper rate, whenever upload was under high load. This worked —
the 200 kbit/s stream's loss fell to 0–5.5% — but at a flat cost: download
was held to 12–14 Mbit/s (from 24) with 4–8 parallel downloads, *even when*
other upload traffic needed far less than the remaining 55%. A single
download, whose ACKs already sat under 45%, was unaffected, which showed
the fix was too blunt for the common case of light competing traffic.

### Shipped: dynamic `upload_ack_share_min`

The fix was made dynamic: the eBPF filter counts both pure-ACK bytes and
total upload bytes over the same window, and ACKs are allowed to use
whatever the *other* upload traffic leaves free (minus 5% headroom so that
other traffic's growth is visible), only ever being held down to the 45%
floor when other traffic actually needs the room. 45% is a floor, not a
reservation — when ACKs need less, non-ACK traffic uses the rest, with no
code change needed for that direction since the ceiling the controller
computes is simply absent whenever ACKs are already under the allowance.

**Measured effect** (full numbers in
[`2026-10-03-ack-share-dynamic`](../2026-10-03-ack-share-dynamic/README.md)):
with downloads alone, download kept 16–20 Mbit/s instead of the fixed cap's
12–14, because the competing traffic (just the probes, ~320 kbit/s) left
ACKs about 55% of upload rather than the fixed 45%. Under genuine heavy
competition (a bulk upload), the dynamic rule reduces to the same 45% floor
as the fixed cap, and the result matches it.

## Net result

| | fping alone (no eBPF) | Shipped (eBPF: `tcp_delay_attribution` + `upload_ack_share_min`) |
| --- | --- | --- |
| Bidirectional added RTT (median) | 50 ms | 28 ms |
| Bidirectional download share | 41% | 67% |
| 200 kbit/s UDP stream loss, 8 downloads, slow upload | up to 48% | 0–5.5% |
| Wrong-direction download cuts under upload bloat | ~23 per phase | ~0 |

Every number above is reproducible with the scripts in
[`tools/testbed/`](../../tools/testbed/README.md); the two linked evidence
directories are the exact runs these totals come from.
