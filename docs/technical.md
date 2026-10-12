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

### Status on ubus

Each running instance registers one ubus object, `cake-adapt.<section>` (for
example `cake-adapt.main`), with one method, `status`, that takes no
arguments. It is answered from a snapshot of the state the monitor already
holds, taken on every traffic tick; nothing is stored or logged for it, so
there is no history.

Control never waits for ubus. libubus can block without a timeout (the
connection handshake, adding the object, writing a reply), so one thread of its
own owns the ubus connection, never registered with the event loop, and serves
the requests; the main thread only copies the snapshot into it under a mutex
and logs what it reports. Each ubus state is handled as
OpenWrt's dnsmasq handles it, with dnsmasq's levels. As in dnsmasq, the two
info messages below reach syslog at info level, like the daemon's start and
stop messages, and are also written to its log file
(`/var/log/cake-adapt[.<section>].log`); the warnings and errors reach both
too:

| State | Behaviour | Logged |
| --- | --- | --- |
| No `ubusd` | Retried every second; control goes on. | Error `cannot publish status on ubus: no connection`, once until a connection is made. |
| Connected by any attempt but the first (after no `ubusd`, a failed reconnection or a hang-up) | Published. Only the connection made by the daemon's very first attempt is not logged. | Info `connected to ubus; status published as cake-adapt.<section>`. |
| Connection lost | Reconnected at once, which adds the object again; if that fails, closed and retried as above. | Error `cannot reconnect to ubus: <reason>` only when the reconnection fails. |
| `POLLHUP` or `POLLERR` on the socket | Closed and retried as above. | Info `disconnecting from ubus`. |
| Object refused on the first connection | The daemon exits with status 1 through its normal shutdown, as dnsmasq does; procd starts it again. | Error `ubus error: <reason>; exiting`. |
| Object refused on a later connection | No further attempt; control goes on. | Error `ubus error: <reason>; status not published`. |

The thread never logs: it raises events that the main thread logs on its next
traffic tick, so several identical events within one tick are logged once.
libubus adds the object again on a reconnection but ignores a refusal there;
the thread then adds it once more to learn the reason, and handles a refusal
as on a later connection. A `ubusd` that is alive but frozen delays only the
answers. At shutdown the thread is given 2 s to stop; one still stuck inside
libubus is left behind with a warning, as the process is exiting anyway. On
the x86 test VM, control went on through 60 s of a stopped `ubusd` and SIGTERM
ended a daemon whose thread was stuck in 2.1 s
([evidence](../profiling/2026-10-11-ubus-thread/README.md)).

The daemon itself never waits on ubus, but the service does: `/etc/init.d/cake-adapt
stop`, `restart` and every other init action reach procd through ubus, so while
`ubusd` is frozen they wait and no signal reaches the daemon. Only a direct
signal, such as `kill -TERM <pid>` with the pid from
`/var/run/cake-adapt/<section>.lock`, stops it then.

```json
{
	"version": "0.3.16-r1",
	"pid": 4767,
	"started": 1791666047,
	"state": "running",
	"download": {
		"interface": "ifb4eth1",
		"available": true,
		"shaper_bps": 20000000,
		"achieved_bps": 15687,
		"load_e6": 784,
		"delay_us": 5048,
		"bufferbloat": false,
		"tcp_queue_us": 1200
	},
	"upload": { "...": "same fields as download" }
}
```

- `started` is the Unix time the status object was set up; `state` is
  `running`, `idle` or `stall` (the activity state).
- `shaper_bps` is the CAKE bandwidth read from the kernel, `achieved_bps` the
  traffic rate, `load_e6` their ratio per million (1000000 is a full shaper).
- `delay_us` is the controller's average added delay for the direction in
  microseconds (negative values are possible); `bufferbloat` is true while
  congestion is detected.
- `tcp_queue_us` appears only while the eBPF TCP queue estimate is valid.
- A direction without a CAKE qdisc reports only `interface` and
  `available: false`.

### Service start, reload and stop

The init script follows OpenWrt's dnsmasq. Each instance runs from a runtime
file the script generates, `/var/run/cake-adapt/<section>.uci`, built and
checked with the daemon's own validation (`cake-adapt -V`) in a staging
directory before it replaces the previous one. procd watches that file.

- **Reload** (a configuration change, LuCI's Save & Apply, or
  `/etc/init.d/cake-adapt reload`) registers the instances again without
  stopping anything. procd restarts only an instance whose command or runtime
  file changed, leaves the others running with the same pid, and stops the
  instances of sections that were removed or disabled.
- **A refused configuration** leaves the previous runtime file in place: the
  instance is registered unchanged and keeps running its previous
  configuration. The system log has the validation's reason followed by
  `configuration of instance '<section>' refused` and `instance '<section>'
  keeps its previous configuration …`. When `/etc/config/cake-adapt` cannot be
  read at all, or the runtime directory cannot be prepared, every instance
  with a runtime file keeps it the same way. A runtime directory that is a
  symbolic link is never read through: nothing is kept and every instance
  stops.
- **Without a previous runtime file** (the first start after boot, or after a
  stop, which removes the file) a refused configuration does not start:
  `could not generate …; not starting instance '<section>'`.
- **Restart** (`/etc/init.d/cake-adapt restart`, and LuCI's Restart button,
  which calls it through rpcd) checks each instance's new configuration before
  stopping it. A valid one stops the instance and starts it again with a new
  pid, even when nothing changed. A refused one keeps the instance running its
  previous configuration and pid, logged as on a reload; without a previous
  runtime file it does not start. An instance of a disabled or removed section
  stops, and `restart <section>` also removes its runtime file. If procd cannot
  stop an instance (a ubus failure other than not running), the system log says
  `could not stop instance '<section>' for restart …; it keeps running` and
  `restart` exits with an error.
- **One instance**: `start`, `stop`, `restart` and `reload` take an optional
  section name, for example `/etc/init.d/cake-adapt restart main`, and then act
  on that instance only.
- **Respawn never gives up**: each instance is registered with
  `respawn 3600 5 0`. procd starts an instance that exited again after 5 s,
  and retry 0 means without limit, so a daemon that keeps exiting (for
  example when `ubusd` refuses its status object at start) is retried every
  5 s instead of being abandoned after procd's default of 5 failures within
  an hour. `stop` still stops it.

### One daemon per instance

Each running daemon holds an exclusive lock on
`/var/run/cake-adapt/<section>.lock`, which also holds its pid. A second daemon
started with the same section name logs `instance '<section>' is already
running (pid N); exiting` and exits with status 1 before it opens the log file
or touches a qdisc; the first one keeps running. The kernel drops the lock when
the process exits, so procd's restart and respawn work as before. Validation
(`-V`) and listing (`-L`) take no lock.

## Evidence and design documents

- [Current-code flowcharts](../flowchart/README.md) — architecture, event flow,
  controller decisions, CAKE updates, and lifecycle behavior.
  [Open the rendered viewer](https://raw.githack.com/huydoan94/cake-adapt/luci-integration/flowchart/index.html).
- [Controller comparison with cake-autorate](../profiling/controller-comparison/README.md)
  — side-by-side VM runs and the replayed upstream traces.
- [Resource use compared with cake-autorate](../profiling/cake-autorate-resources/README.md)
  — CPU, memory, processes and reaction time under the same workload.
- [End-to-end run and profiling, 2026-09-30](../profiling/2026-09-30/README.md)
  — the final VM run, CPU before and after the optimization pass, flame graphs,
  and raw `perf` data.
  [Open the dashboard](https://raw.githack.com/huydoan94/cake-adapt/luci-integration/profiling/2026-09-30/index.html).
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
