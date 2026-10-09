# cake-adapt technical overview

This page covers what the daemon does, how it differs from
[cake-autorate](https://github.com/lynxthecat/cake-autorate), and the evidence
behind it. For installation and configuration, see the [README](../README.md)
and the [configuration reference](configuration.md); for building and testing,
see [development](development.md).

## Scope

cake-adapt currently:

- loads typed configuration through `libuci`;
- discovers existing CAKE qdiscs through rtnetlink;
- uses the configured `interface` as upload and derives download as
  `ifb4<interface>`, unless both directional interface options override it;
- measures traffic rates independently for upload and download;
- runs one `fping` process against multiple reflectors;
- tracks latency baselines and reflector health;
- detects high load, congestion, idle periods, and stalled connectivity;
- adjusts upload and download CAKE bandwidth independently when enabled, and
  reads each change back from the kernel to verify it;
- reacts when CAKE qdiscs disappear or reappear;
- integrates with OpenWrt `procd`; and
- emits cake-autorate-style statistics for compatible analysis workflows.

The daemon does **not** currently create CAKE, IFB, ingress redirection,
`ctinfo`, or `mirred` configuration. An existing SQM setup must create the
upload and download CAKE qdiscs before cake-adapt can control them.

Do not run cake-adapt with rate adjustment enabled at the same time as
cake-autorate or another program that changes the same CAKE qdiscs.

cake-adapt does not source, execute, or depend on files from a cake-autorate
installation. The upstream project is a behavioral and algorithmic reference,
not a runtime dependency.

## Differences from cake-autorate

Controller decisions match cake-autorate `ac75f49`: recorded upstream traces
replay through the cake-adapt controller with the same delay counts and
bufferbloat decisions (see
[the controller comparison](../profiling/controller-comparison/README.md)).
Since 2026-10-06 cake-adapt computes exactly instead of copying upstream's
shell-integer truncations: rates are bit/s rather than whole kbit/s (CAKE
holds whole bytes/s), loads and rate factors are exact ratios rather than
whole percent and per-thousand steps, and delays round to the nearest
microsecond. On the replayed traces this moves shaper rates by at most 0.12%
and thresholds by at most 2 µs. The other differences are in integration and
safety:

- Rate adjustment is opt-in. `adjust_dl_shaper_rate` and
  `adjust_ul_shaper_rate` default to `0`, so a new installation only observes.
- There is no `startup_wait_s`. cake-adapt follows CAKE through kernel
  `RTM_NEWQDISC`/`RTM_DELQDISC` events. While either qdisc is missing, the
  pingers stop and control is suspended; it resumes when the qdisc returns.
- Achieved rates come from the CAKE qdiscs' own byte counters, read with the
  rest of the CAKE state in one rtnetlink dump, instead of from interface
  statistics files.
- Each bandwidth change goes to the existing qdisc through rtnetlink instead
  of `tc`, and is read back from the kernel to verify the effective rate.
- Reflectors come only from local configuration. Remote reflector-list
  retrieval is not implemented.
- `fping` is the supported pinger. The `fping-ts` method (fping with ICMP
  timestamps, giving separate download and upload delays) is implemented as in
  cake-autorate and verified on the emulated testbed, where its one-way delays
  track the real queues and it beats fping's RTT/2 in every phase but a sudden
  capacity drop
  ([evidence](../profiling/2026-10-03-fping-ts-testbed/README.md)). It is not yet
  verified on internet reflectors, whose clocks and timestamp support vary, so
  it is not yet supported for production use. `fping-ts` accepts only IPv4
  reflectors, because ICMP timestamps are IPv4 only; cake-autorate leaves
  fping to fail on IPv6 targets at runtime.
- **The IRTT backend (`pinger_method 'irtt'`) is experimental.** It follows
  cake-autorate and passes its host tests, but it has never run against a
  real IRTT server and no runtime behavior has been verified. Do not use it
  for production shaping.
- Deliberate departures aimed at less bufferbloat (the replayed upstream
  traces still match with them off):
  - With `fping`, whose RTT/2 is one delay for both directions, a detected
    bufferbloat is attributed to a direction by download delivery: download
    delivering its full shaper rate has no standing queue.
  - `tcp_delay_attribution` (default off, `fping` only): measure each
    direction's queueing delay from TCP timestamps with an eBPF socket filter
    on the upload interface (`/lib/bpf/cake-adapt-tcpdelay.o`, loaded with
    libbpf). It sees packets after upload CAKE and before the ingress IFB, and
    measures TCP timing changes that include both path delay and remote
    response timing. Once measured queues total at least 5 ms, fping's
    round-trip delay is split by their measured shares, and only a direction
    holding at least a quarter of the queue is then cut. With
    `output_processing_stats`
    the estimates are logged as `TCP_QUEUE` records.
    Directional estimates now come from one flow: prefer a fresh complete pair,
    then the longest measurement history, instead of independent minima across
    flows. This keeps a new connection's congested baseline from overwriting a
    fresh established pair. Estimates remain relative to per-flow floors; age
    alone cannot prove that a new flow's initial path was uncongested.
    A queue on the access link delays fping too, so fping's largest added
    round-trip delay over the current and previous second bounds every flow's
    estimate: a flow's baseline falls to new minima and rises only as far as
    that bound requires. This keeps standing queues visible while fping sees
    them, and stops remote clock drift from growing into a false queue. A new
    flow's initial queue remains unknown, and TCP timestamps cannot distinguish
    sustained receiver ACK wait from upload queueing.
    See the [flow-pair regression evidence](../profiling/2026-10-04-flow-pair/README.md),
    the [queue-bound check](../profiling/2026-10-06-tcp-bound-check/README.md)
    and the [router run](../profiling/2026-10-06-router-libreqos/README.md).
  - `ul_congest_ack_share` (default `0`, off): download ACKs can fill a slow
    upload. The same eBPF filter splits upload, after the upload CAKE, into
    pure ACKs and everything else. While upload is above `high_load_thr`,
    ACKs may use what the other traffic leaves free, less 5% headroom so its
    growth shows; download is held so its ACKs fit, but never so far that
    they fall below this share of the upload shaper rate (for example
    `0.45`). It is not a reservation: while ACKs need less, other traffic
    uses the rest. Download is never held below its minimum rate.
    Both byte counters use the upload CAKE's overhead, minimum packet size
    (MPU), RAW setting and ATM/PTM framing, with a fresh capture when those
    settings or the qdisc change. Accounting supports non-GSO, untagged
    Ethernet IP/ARP and IP packets on PPP or raw-IP links; RAW also permits
    other untagged packet protocols. Tagged packets, post-qdisc GSO aggregates
    and unknown framing disable the ACK ceiling for that sampling interval,
    report degradation, and recover after a clean interval. TCP timestamp
    measurement continues. This is conservative coverage, not full offload
    or tunnel accounting.
    This setting was renamed from `upload_ack_share_min`; update existing UCI
    and standalone shell configurations to `ul_congest_ack_share`.
- Configuration is typed UCI. A cake-autorate configuration file can be
  imported (see [Standalone shell configuration](configuration.md#standalone-shell-configuration)),
  but it is validated like UCI and never sourced by the daemon.

## Data flow

```text
 CAKE qdisc dump (traffic timer)          fping reflector replies
   bytes, bandwidth, MTU                            |
            |                                       v
            v                           OWD baseline and delta
     achieved rates                     per reflector
            |                                       |
            +------------------+--------------------+
                               |  once per reply
                               v
                cake-autorate-derived controller
                               |
                               v
                     desired shaper rates
                               |
                               v
               rtnetlink CAKE update + readback

 RTM_NEWQDISC / RTM_DELQDISC events -> rediscover or suspend
```

As in cake-autorate, the controller runs once for every reflector reply,
using the most recent achieved rates. The traffic timer refreshes CAKE state
and achieved rates and drives the idle/stall state machine.

For a normal SQM interface named `eth1`, cake-adapt uses:

```text
upload:    eth1
download:  ifb4eth1
```

The IFB name follows the convention used by SQM and is truncated when needed
to fit Linux's interface-name limit. For setups that do not follow this
convention, set both `ul_if` and `dl_if`:

```uci
option ul_if 'wan'
option dl_if 'download'
```

The directional pair takes precedence over `interface`. Setting only one is a
configuration error. When all three have values, cake-adapt logs a syslog
warning that `interface` was overridden.

## Evidence and design documents

- [Current-code flowcharts](../flowchart/README.md) — architecture, event flow,
  controller decisions, CAKE updates, and lifecycle behavior.
  [Open the rendered viewer](https://raw.githack.com/huydoan94/cake-adapt/further-integration/flowchart/index.html).
- [Controller comparison with cake-autorate](../profiling/controller-comparison/README.md)
  — side-by-side VM runs and the replayed upstream traces.
- [Resource use compared with cake-autorate](../profiling/cake-autorate-resources/README.md)
  — CPU, memory, processes and reaction time under the same workload.
- [End-to-end run and profiling, 2026-09-30](../profiling/2026-09-30/README.md)
  — the final VM run, CPU before and after the optimization pass, flame graphs,
  and raw `perf` data.
  [Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/further-integration/profiling/2026-09-30/index.html).
- [All profiling evidence](../profiling/README.md), including the superseded first
  capture.
- [TCP-delay review](design/EBPF_REVIEW.md) — the eBPF queue estimator's findings
  and their state.

## Project direction

The development sequence is deliberately incremental:

```text
observe
  -> verify
  -> control existing CAKE
  -> replace cake-autorate behavior
  -> optionally take ownership of SQM orchestration
```

Replacing CAKE itself is not a goal. CAKE remains the Linux kernel qdisc.
Taking ownership of IFB creation, DSCP restoration, ingress redirection, and
CAKE setup is a possible later phase and must not be confused with the current
daemon behavior.

## Status and deferred work

The cake-autorate parity and refactor sequence is complete:

- Controller decisions match cake-autorate `ac75f49` on two replayed traces
  (2,402 and 2,301 samples): delay counts and bufferbloat decisions exactly,
  rates within 0.5% since exact math replaced upstream's truncations. Live
  side-by-side VM runs behave the same per phase
  ([comparison](../profiling/controller-comparison/README.md)).
- A final end-to-end run on the OpenWrt 25.12 x86 VM covered sustained
  download, upload and bidirectional load, congestion and recovery within
  bounds, qdisc removal and re-creation on both interfaces, idle sleep and
  wake, log export and in-place reset, and clean shutdown with no leftover
  processes ([results](../profiling/2026-09-30/README.md)).
- After the optimization pass, bidirectional CPU use roughly halved and
  syscalls fell from 20,586 to 4,593 per 45 s. The daemon's own code is 4–9%
  of its sampled CPU time.
- Against cake-autorate under the same workload, cake-adapt uses about a fifth
  of the CPU including fping (1.10% against 5.38% of a core while awake), a
  tenth of the memory, and reacts to a reply 7× faster at the median
  ([comparison](../profiling/cake-autorate-resources/README.md)).
- Host tests, sanitizer runs, and the x86 SDK build pass.

Deferred work:

- Additional pinger backends. `fping` is the supported production backend;
  `fping-ts` needs verification on internet reflectors, and the experimental
  IRTT backend needs fixtures and runtime verification against an IRTT
  server before either is supported.
- Native SQM ownership (CAKE, IFB, `ctinfo` and `mirred` setup) remains a
  possible later phase. It requires an explicit decision and is not part of
  the current daemon.
