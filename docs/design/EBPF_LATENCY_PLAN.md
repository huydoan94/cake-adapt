# Plan: replace fping with eBPF latency measurement

Status: sketch, 2026-10-06. Nothing is implemented. The decisions in the last
section are open.

## Recommendation in short

The measurement can come from the TCP filter that already runs. It records
when each outgoing TSval leaves and when the remote echoes it, so every echo
yields one round trip from the router to the remote host and back. It covers:
- the access link in both directions;
- none of cake-adapt's own CAKE queues, which is the delay the controller
  should react to.

Two gaps decide whether "pure eBPF" can be the default:

1. **Traffic without TCP timestamps gives no samples.** That covers:
   - QUIC (YouTube, Google, Meta, Cloudflare);
   - other UDP: games, calls, VPNs;
   - TCP from Windows hosts, which by default do not negotiate timestamps
     (to be confirmed on the user's LAN).

   A line loaded only by such traffic would have no latency signal.
2. **Nothing measures the path while it is idle,** so baselines go stale. That
   matters less, because the controller does not act on an idle line anyway.

The proposal is to build the passive signal as a new, opt-in latency source,
in phases. Measure coverage on real routers first, then run the signal in
shadow next to fping, and only then let it drive the controller. Whether a
low-rate fallback prober is needed is decided by data from phase 0, not up
front.

## What fping provides today, and the replacement for each

| fping provides | Used by | eBPF replacement |
| --- | --- | --- |
| RTT samples at a steady cadence | trackers, then the controller's delay thresholds and detection window | external RTT from TSval/TSecr echoes, resampled to a fixed cadence |
| Per-reflector baselines | `latency/tracker.c` | per-path minimum RTT, keyed by remote prefix |
| Reflector health and rotation | `controller/reflector.c`, `monitor/reflectors.c` | path selection: use the paths with the most recent samples, and drop silent ones |
| Global stall: no replies while traffic flows | `monitor.c` STALL | "no signal under load" state, with a policy decision (see the last section) |
| A queue bound for the TCP estimator (`cb348e4`) | `monitor/tcpdelay.c` | the flow's own RTT delta bounds its two one-way estimates; nothing external needed |
| Idle and sleep behavior, with fping stopped | activity state machine | unchanged: the capture stays attached and costs nothing without packets |

## Signal design

### Raw samples (exists)

`incoming()` in `tcpdelay.bpf.c` looks up the departure of the TSval that a
new TSecr echoes. Each sample carries `arrival_ns` and `departure_ns`. The tap
is an `AF_PACKET` socket on the upload interface:
- outgoing packets are seen after the root qdisc;
- incoming packets are seen before the ingress redirect to the IFB.

`arrival − departure` therefore excludes both CAKE queues and includes:
- the ISP upload queue;
- the ISP download queue;
- the remote host's own delay before echoing.

This differs from fping. fping's probes pass through both CAKEs, so its delta
includes CAKE's own sojourn, a few ms at CAKE's targets. Thresholds keep their
meaning, but the passive signal is slightly cleaner.

### From samples to a controller input (new)

1. **Per-path baseline.** Key paths by remote /24 (IPv4) or /48 (IPv6), so
   that flows to one CDN node share a baseline:
   - the baseline falls at once to a new minimum;
   - it rises slowly, like cake-autorate's baseline EWMA, to follow route
     changes;
   - delta = sample − baseline.
2. **Noise.** Delayed ACKs, server pauses and application-limited senders only
   *add* delay, by up to tens of ms. A real standing queue at the bottleneck
   raises *every* flow crossing it. So in each window of 50-100 ms, take the
   **minimum delta across active paths**, or a low percentile when there are
   many. It rejects one-flow noise and keeps a shared queue.
3. **Fixed cadence.** The detection window (`bufferbloat_detection_window`,
   `bufferbloat_detection_thr`) counts samples, and its timing meaning comes
   from fping's steady rate. Emit one aggregated sample per window, so N of M
   samples still means roughly the same time span. Without traffic, a window
   emits nothing. That is not a loss, the same as a quiet reflector today.
4. **Direction split.** One-way attribution remains the estimator's job:
   remote clock tick fit, floors and 100 ms windows. The difference: the
   sample's own RTT delta now bounds `upload_queue + download_queue`. That
   replaces the fping bound and removes the drift failure mode by
   construction.
5. **Virtual reflectors.** To reuse the tracker, controller and logs, present
   up to `no_pingers` active paths as reflectors. The address is the path
   prefix, and the sequence is per path.
   - Health checks and rotation become path selection: a path silent for
     longer than a timeout is replaced by the busiest other path.
   - Record names and fields stay the same. Only the reflector column holds
     a prefix.

### Known measurement hazards

- **TSecr echo rules (RFC 7323):** the remote echoes the most recent in-order
  segment's TSval. After loss or reordering, echoes lag. The filter samples a
  departure only on an exact match, but the first sample after recovery can
  be inflated. The per-window minimum absorbs it.
- **Download flows:** the round trip is the router's ACK out, then the
  server's next data in. When the server is application-limited (video
  pacing), the gap includes its idle time. The minimum across paths handles
  it, but a single paced flow alone is a weak signal.
- **Hardware or software flow offload:** offloaded flows bypass both the
  qdisc and the tap. SQM already requires offload off. Check it on Filogic.
- **Map pressure:** `TCPDELAY_DEPARTURES` 8192 and `TCPDELAY_FLOW_STATES` 1024
  are LRU maps. At 145 Mbit/s with many flows, check eviction counts before
  trusting coverage numbers.
- **CPU:** the filter already runs per packet, at about 2-3 µs on the x86 VM.
  New userspace work is per sample. The Cortex-A53 needs a measurement at
  full line rate, not an assumption.

## Changes by module

The pipeline stays `measurement -> controller decision -> desired CAKE state
-> kernel update`.

- **`tcpdelay/tcpdelay.bpf.c`:**
  - phase 0: coverage counters, per direction: TCP bytes with and without
    timestamps, UDP 443 (QUIC), other UDP, and RTT samples emitted;
  - later, the extra echo types from "Covering traffic without TCP
    timestamps", each a separate map key type, to keep the verifier cost
    bounded on the Cortex-A53;
  - no change to sampling, except perhaps always emitting a matched echo even
    when the ring is busy.
- **`tcpdelay/record.h`:** new counter fields; the 64-byte record is
  unchanged.
- **`tcpdelay/estimator.c`:** take the per-sample RTT delta as the queue bound
  instead of the external fping bound (`tcpdelay_estimator_set_bound` goes
  away or becomes internal).
- **`latency/` (new source, e.g. `passive.c`):**
  - path table, baselines and windowed minimum;
  - emits latency samples to the same consumer the fping parser feeds;
  - a `struct pinger_ops`-like entry, but with no child process; it is fed
    from the capture drain.

  Whether it belongs in `latency/` (the source concept) or `tcpdelay/` (the
  data owner) is a design choice; `latency/` keeps the controller side
  unchanged.
- **`monitor/pingers.c` / `reflectors.c`:** when the passive source is
  selected:
  - no child start, restart or exit handling;
  - reflector health becomes path timeout and replacement;
  - startup waits for the first samples instead of pinger setup grace.
- **`monitor/monitor.c`:** the stall definition for a source that is silent
  by nature when no TCP-timestamp traffic flows (see decision 2).
- **`config/`:** `pinger_method` gains a value such as `tcp-passive`; fping
  stays the default.
  - The reflector list is unused in this mode; the configuration is still
    validated.
  - New options are needed only for the path prefix length and window,
    preferably as built-in defaults first.
- **`logging/`:** existing records keep their format. A `PASSIVE` record
  (debug, optional) logs per-window path count, sample count, min delta and
  the fping delta when both run, for shadow comparison.
- **Unchanged:** the controller, the replay test (it runs with fping
  semantics), CAKE, netlink and the ACK share.

## Phases, acceptance and evidence

### Phase 0: coverage census (small; decides everything else)

- Add the coverage counters and a periodic `DEBUG` or summary line. No
  control change.
- Run on the user's routers for at least one normal day: streaming, calls,
  gaming, Windows and other hosts.
- **Output:**
  - per loaded second (traffic above the active threshold), the RTT samples
    per second;
  - for the echo types not yet implemented, how many samples each would have
    given (SYNs, QUIC Initials, DNS queries, upload segments without
    timestamps);
  - the share of bytes that is TCP with timestamps, TCP without, QUIC and
    other UDP;
  - map evictions.
- **Gate:** report the share of loaded seconds with fewer than about 10
  samples per second, in each direction. If loaded periods without signal are
  common, the fallback prober (decision 1) is required, not optional.

### Phase 1: shadow signal

- Implement the path baselines, windowed minimum and `PASSIVE` record. fping
  keeps control.
- **Testbed** (VM, `tools/testbed`), each against fping's delta on the same
  run:
  - single and multi-flow TCP up and down;
  - a standing ISP queue;
  - capacity steps;
  - a remote clock drift of ±2%;
  - application-limited (paced) download;
  - UDP-only load (expect no signal and confirm the state is reported).
- **Router:** shadow logging during LibreQoS runs and normal use.
- **Acceptance:**
  - the passive delta tracks fping's delta (scatter plot and correlation per
    phase);
  - it reacts to queue onset no later than fping;
  - false bloat on an idle-but-noisy line is no more frequent than fping's.

### Phase 2: opt-in control

- `pinger_method tcp-passive` drives the controller through virtual
  reflectors.
- Implement the chosen no-signal policy.
- Host tests:
  - path baselines and windowed minimum with synthetic samples;
  - path timeout and replacement;
  - no-signal state transitions;
  - the estimator's intrinsic bound, including the drift case from the
    `4f5cedc` tests.
- **Testbed A/B,** three alternating repetitions against fping, as in
  `2026-10-06-tcp-bound-check`:
  - added delay p95 and time above 30 ms no worse;
  - throughput at least 85% of fping's;
  - CPU and RSS reported.

### Phase 3: router validation

- Opt-in on the user's routers: LibreQoS runs plus at least one long run (one
  hour or more) per router.
- Raw logs go under `profiling/`.
- Default stays fping until the user decides otherwise.

## Covering traffic without TCP timestamps (discussed 2026-10-06)

The user proposed two ways. The recommendation is to layer them: passive TCP
timestamps, then the extra echo types below, then fping as the last resort.

### Our own echo matching (no conntrack)

Flows are keyed by address and port in the filter's own LRU map, as today.
The tap sees post-NAT packets on the WAN side, which is the side being
measured, so conntrack adds nothing. A socket filter cannot query it anyway:
conntrack kfuncs exist only for TC/XDP programs and need kernel BTF.

| Traffic | Echo matched in the filter | Samples |
| --- | --- | --- |
| TCP upload without timestamps (Windows) | end sequence of an outgoing segment → the ACK that covers it; retransmitted ranges skipped (Karn's rule) | continuous while uploading |
| TCP download without timestamps | SYN out → SYN-ACK in | one per connection |
| QUIC | client Initial → first server long-header packet | one per connection |
| DNS, from the router and clients | query ID → response, per resolver baseline | frequent while browsing |
| ICMP echo from LAN hosts | id/seq → reply | as hosts send |
| Games, calls, VPN (other UDP) | none reliable: traffic flows both ways continuously, with no echo | none |

All of them feed the same departure map and sample record, with a type field
in `reserved`. The per-window minimum across paths absorbs delayed-ACK and
resolver noise. The remaining blind case is a long single download over QUIC
or without timestamps. It yields samples only when connections open, so the
middle of the download is barely covered.

### fping as the last resort

fping is used only when the line is loaded and the passive windows stay
empty. Two variants:
- **Standby:** fping runs at a very low rate while the line is loaded, and its
  samples reach the controller only when the passive signal is silent. It
  reacts at once, for a small constant cost.
- **On demand:** fping starts when the signal goes silent, and the current
  rate is held until its first replies. The spawn plus setup grace takes
  seconds while the line is loaded and blind.

Phase 0 counts samples per echo type and the loaded seconds left empty after
each layer. That shows how often fping would be needed and which variant is
worth it.

## Decisions for the user

1. **Fallback.** Chosen in principle on 2026-10-06: our own echo matching,
   then fping. Open questions:
   - standby or on-demand fping;
   - whether an in-daemon prober should later replace fping in this role:
     its own socket, with `SO_TIMESTAMPING`, and no child process.

   eBPF itself cannot originate probes. Decide after phase 0.
2. **Policy when loaded with no signal** (QUIC or UDP only):
   - hold the current rate;
   - decay toward the base rate;
   - treat it as cake-autorate's stall, with minimum rates;
   - or use the fallback prober.

   Holding risks bloat if capacity drops, and the minimum rate costs
   throughput.
3. **Path key:** a /24 or /48 prefix (recommended, since it pools a CDN node's
   flows) or the exact remote address.
4. **Scope of "eliminate":** whether fping stays as a supported backend.
   Recommended: yes, because the replay test and cake-autorate parity depend
   on it, and the passive source arrives as an experimental option, like
   IRTT.

## Rough size

- Phase 0: about a day, including a router build.
- Phase 1: a few days, plus testbed work.
- Phase 2: about a week with tests and the A/B evidence.
- Phase 3: depends on router time.

Most new code is userspace path tracking. The filter needs only counters.
