# Agent guide: module boundaries and OpenWrt integration

Read before adding or moving code, or touching interfaces, ingress or kernel integration. Part of [AGENTS.md](../../AGENTS.md).

### Module boundaries

- `main.c`: CLI, startup, logging initialization, configuration loading,
  the per-instance lock (`/var/run/cake-adapt/<section>.lock`, so a second
  daemon with a running instance's name exits with an error), top-level
  lifecycle, and orderly shutdown only.
- `monitor/`: uloop orchestration and coordination between measurements,
  controller decisions, qdisc lifecycle, timers, and signals. All state is one
  `struct monitor` in the private `loop.h`, passed to every function; each
  file owns one part of it, declared with its functions in the header of the
  same name (`links.h`, `pingers.h`, ...), and prefixes its functions with
  that part's name. `monitor.h` is the only public header.
  - `monitor.c`: loop setup and teardown, the traffic tick
    (`monitor_tick`) and the activity state machine.
  - `upkeep.c` (`upkeep`): CPU and memory records, the log file's timer, and
    the export and reset signals.
  - `links.c` (`links`): both directions' CAKE discovery, achieved rates,
    compensated traffic cadence, and qdisc lifecycle events.
  - `control.c` (`control`): controller configuration and input, CAKE
    bandwidth changes with readback, minimum-rate enforcement, and controller
    records.
  - `pingers.c` (`pingers`): pinger start, output, exit, restart, setup grace,
    and per-reply latency processing.
  - `reflectors.c` (`reflectors`): latency trackers, reflector ordering,
    scheduled comparison and replacement, and health checks.
  - `tcpdelay.c` (`tcp`): the TCP capture, its queue estimate, the upload ACK
    rate, and the timestamp injector's attachment and `TCP_INJECT` records.
  - `status.c` (`status`): the instance's status snapshot from state the
    monitor already holds.
  - `publish.c` (`publish`): the status object on ubus, served from its own
    thread so a stalled `ubusd` never blocks control; the main thread only
    copies a snapshot under a mutex each traffic tick and logs what the thread
    reports.
- `constants/`: every named value, one header per topic: `units.h` (units and
  conversions), `program.h` (identity, build features, runtime directory),
  `options.h` (UCI option names, booleans, configuration limits), `states.h`,
  `text.h`, `logging.h`, `mqtt.h`, `latency.h` (pinger methods, command lines,
  output grammar, timing), `icmp.h`, `cake.h` (qdiscs, netlink, accounting),
  `proc.h`, `controller.h` (fixed policy), `tcpdelay.h` (filter object, maps,
  packet layout, estimator) and `ubus.h`. Headers include only the topics
  they build on; each source file includes the topics it uses.
- `common/`
  - `utils.h`: trivial operations as `static inline` functions: saturating
    arithmetic (`saturating_add`, `saturating_sub`, `saturating_mul`,
    `signed_sum`), `min_u64`, `max_u64`, `mul_div`, rounding
    (`rounded_divide`, `signed_rounded_divide`), ratios per million
    (`ratio_of`, `ratio_of_rounded_up`), unit conversions (`byte_to_bit`,
    `bit_to_kbit`, `byte_to_kbyte`, `us_to_ms`, `ns_to_us`, `timespec_to_us`,
    `us_to_sec_decimal`), and `ARRAY_SIZE`. A module never keeps its own copy
    of one.
  - `helpers.c`: generic operations too large to inline, such as numeric
    parsing, random selection, clocks (`read_clock_us`), bounded thread joins
    (`thread_join_within`, for the ubus and MQTT threads), rates from counters
    (`bps`), and ratios (`fraction_to_ratio_e6`, `load_ratio_e6`).
  - `error.c`: shared error-buffer formatting.
- `config/`
  - `config.c`: typed UCI loading and conversion through `libuci`, driven by
    one table of typed option bindings (`options[]`, whose order is also the
    `-L` listing, and `-D` prints each one's default in its UCI units for the
    LuCI app); never parse `/etc/config/cake-adapt` manually.
  - `validate.c`: cross-option validation of a loaded configuration
    (`config_validate`), run before control starts.
  - `defaults.c` and `defaults.h`: built-in application and configuration
    defaults.
- `controller/`
  - `controller.c`: platform-independent load classification, autorate,
    congestion, and activity policy; keep it directly unit-testable with
    synthetic inputs.
  - `reflector.c`: reflector health, comparison, and rotation policy over
    latency trackers.
- `latency/`
  - `icmp.c`: the native probes' IPv4/IPv6 raw sockets, wire packets,
    identifier filters and fixed-width kernel receive timestamps.
  - `native.c`: native probe scheduling (the default pinger), outstanding
    requests, expiry and reply matching; its samples use the same
    logging/tracker contract as the fping and irtt fallbacks.
    `monitor/pingers.c` registers its sockets and timer with uloop. Legacy
    backends remain until the native live comparison passes.
  - `latency.c`: pinger session and child-process ownership, line splitting,
    and dispatch to the active backend. Children are reaped by the monitor
    through `uloop_process`. The only synchronous, bounded waits are shutdown
    (`latency_stop_now`) and a child whose pipe setup failed (`start_child`).
  - `fping.c` and `irtt.c`: backend-specific launch arguments and scheduling.
  - `pinger.h`: private interface between the session and its backends;
    each backend exports a `struct pinger_ops` (name, line parser, exit
    handler) that the session calls through.
  - `parser.c`: pinger output parsing into latency samples.
  - `tracker.c`: per-reflector baseline and delta EWMA tracking.
- `tcpdelay/`: passive per-direction queue measurement from TCP timestamps.
  - `tcpdelay.bpf.c`: `AF_PACKET` socket filter on the upload interface (it
    sees packets after the root qdisc and before the ingress redirect). It
    counts upload and pure-ACK bytes, records departures and emits reply
    samples to a ring buffer, each at most once per flow per
    `TCPDELAY_SAMPLE_INTERVAL_NS`, without wakeups. The same object holds the
    experimental `tcp_ts_request` program (`inject_egress`, tcx egress on
    the upload interface), which only adds a timestamp option to outgoing
    IPv6 SYNs that lack one and leaves IPv4 untouched; the filter observes
    the SYN-ACKs, resets and first acknowledged data that answer them.
  - `record.h`: layouts and limits shared by the filter and userspace.
  - `inject.h`: the injection maps' layouts and its inject-or-skip policy,
    unit-tested: each client address gets 1, then 0x80000000 after a stall,
    then its learned clock; a server (address and port) that refuses or
    declines a timestamp is skipped for every client. Both last a day.
  - `capture.c`: loading and attaching the filter with libbpf, draining the
    ring buffer, and reading the counters.
  - `injector.c`: attaching the capture's injection program with a tcx link
    owned by the daemon, its network offset, and summing its counters.
  - `estimator.c`: per-flow one-way queue estimates (remote clock tick fit,
    floors, window minimums); platform-independent and unit-tested.
  The filter object is installed as `/lib/bpf/cake-adapt-tcpdelay.o`; a filter
  the kernel rejects degrades TCP measurement, never the daemon.
- `cake/cake.c`: CAKE and root `cake_mq` discovery, state decoding, and
  CAKE-specific operations; `cake/accounting.h` holds the upload CAKE's packet
  charge settings shared with the socket filter.
- `platform/`
  - `netlink.c`: low-level rtnetlink requests, replies, events, and timeouts.
  - `traffic.c`: counter samples and achieved-rate calculations only.
  - `cpu.c`: CPU sampling and usage calculations only.
  - `memory.c`: the daemon's memory sampling from `/proc/self/status`, and
    the peak resident size it has seen (VmHWM lags).
- `logging/`: all logging, cake-autorate-compatible records, rotation,
  export, reset behavior, and submission to the optional MQTT sender.
  `log.h` is the interface for messages, the log file and the writer;
  `records.h` for the structured records; `output.h` is private to the
  directory.
  - `log.c`: levels, messages, syslog, the line every part writes and the
    date-time cache; shutdown.
  - `file.c`: the log file, its buffer, rotation by age and size, export
    and reset, keeping the file's inode.
  - `writer.c`: the writer thread, which does every file, stdout and syslog
    write; the main thread only queues (a bounded FIFO that overwrites and
    counts its oldest lines when full), never waits, and wakes the writer
    only for syslog entries, requests, unbuffered lines or a half-full queue.
  - `records.c`: the record formats and headers, which start every rotated
    or reset file.
- `logging/mqtt.c`: asynchronous MQTT transport through libmosquitto, with optional verified TLS;
  owns its bounded queue, sender/network threads, connection retries and cleanup. It
  never cancels a thread: a network thread stuck in name resolution is joined
  within a bound or left behind with its own references, at most one at a time.
  It never makes controller decisions. Logging submissions never wait for a broker
  or queue space; offline, contended, full or oversized submissions drop silently.
- `logging/mqtt_none.c`: the same interface without libmosquitto, for the
  `cake-adapt` package (`MQTT_SUPPORTED=0`): sends nothing; a start with MQTT
  on fails, which `main.c` logs as a warning while the instance runs.

Outside `src/`:

- `files/cake-adapt.init`: the procd service, modelled on dnsmasq. Each
  instance runs from its own runtime file,
  `/var/run/cake-adapt/<section>.uci`, which procd watches. Reload restarts
  only changed instances; reload and restart keep an instance whose new
  configuration `cake-adapt -V` refuses running its previous file;
  `start`, `stop`, `restart` and `reload` take an optional section; instances
  are registered with `respawn 3600 5 0` (never give up).
- `files/cake-adapt.import` (installed as `/usr/libexec/cake-adapt/import`):
  builds the runtime file with `uci get`/`uci set`, filling options UCI leaves
  unset from the standalone shell `config_file`; UCI always wins.
  `config_file`, `ping_prefix_string`, `ping_extra_args` and
  `log_file_path_override` come only from the
  root-only `/etc/cake-adapt/exec` (read with `uci -c /etc/cake-adapt`), which
  LuCI cannot write, and are refused in
  `/etc/config/cake-adapt`. A separate Bash reader (`import-bash`) sources the
  shell file; the importer itself is plain sh. The
  daemon's `-C` takes that one file and makes a relative `-C` or
  `log_file_path_override` path absolute.
- `files/cake-adapt.config`: the shipped UCI template;
  `files/cake-adapt-exec.config`: the root-only one, installed as
  `/etc/cake-adapt/exec` (mode 0600).
- `luci-app-cake-adapt/`: the Status view (per-instance state from procd,
  actions, and line charts from the ubus status) and the Configuration view
  (Advanced sub-tabs, on-screen checks of every daemon validation rule). Use
  stock LuCI widgets and theme classes only, with no custom CSS or third-party
  JavaScript. `tests/test_luci.sh` checks that the Configuration view names
  exactly the daemon's options.

Before adding a function, search for an existing equivalent. Consolidate
duplicate conversions, parsing, percentage, timing, and bounds logic in the
appropriate existing module.

## OpenWrt and kernel integration

Prefer supported libraries and kernel APIs over handwritten or process-based
substitutes:

- `libuci` for configuration;
- `libubox`/`uloop` for the event loop;
- `libubus` for the status object;
- rtnetlink/libnl for interface and qdisc operations;
- `procd` for service lifecycle; and
- kernel CAKE, IFB, `ctinfo`, and `mirred` facilities.

Do not repeatedly spawn `tc` or `ip` from the daemon. Use them only as external
diagnostic tools. Prefer event subscriptions such as `RTM_NEWQDISC` and
`RTM_DELQDISC` over periodic existence polling when the kernel already exposes
the lifecycle.

Use another OpenWrt feed library when it materially removes correct, maintained
code. Before adding a dependency, confirm it is available for supported targets
and that the reduction justifies flash, memory, and maintenance cost. Remove
dependencies that are no longer used.

### Interface convention

UCI normally contains one interface name:

```text
upload   = <interface>
download = ifb4<interface>
```

Derive the IFB name using Linux interface-size limits. A paired `ul_if` and
`dl_if` setting may override the derived pair for nonstandard deployments. Both
must be present; reject a partial pair. If `interface` and both overrides are
set, use the explicit pair and warn through syslog. Never hardcode a WAN device.

### Future ingress ownership

If cake-adapt later replaces SQM setup, preserve this ingress path:

```text
WAN ingress -> ctinfo zone 0 -> mirred redirect -> IFB -> CAKE
```

Outbound DSCP is stored in conntrack mark bits 0-5. Bits 6-31 belong to other
uses and must remain unchanged:

```text
ct mark = (ct mark & 0xffffffc0) | dscp
```

Do not replace this with an ingress path that loses DSCP restoration before
CAKE.

