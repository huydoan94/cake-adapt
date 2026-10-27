# Agent guide: module boundaries and OpenWrt integration

Read before adding or moving code, or touching interfaces, ingress or kernel integration. Part of [AGENTS.md](../../AGENTS.md).

### Module boundaries

- `main.c`: CLI, startup, logging initialization, configuration loading,
  top-level lifecycle, and orderly shutdown only.
- `daemon/`: the running daemon: uloop orchestration and coordination between
  measurements, controller decisions, qdisc lifecycle, timers, and signals. All
  state is one `struct daemon` in the private `state.h`, passed to every
  function; each file owns one part of it, declared with its functions in the
  header of the same name (`links.h`, `pingers.h`, ...), and prefixes its
  functions with that part's name. `daemon.h` is the only public header.
  - `daemon.c`: loop setup and teardown, the traffic tick (`daemon_tick`)
    and the activity state machine.
  - `upkeep.c` (`upkeep`): CPU and memory records, the log file's timer, and
    the export and reset signals.
  - `links.c` (`links`): both directions' CAKE discovery, achieved rates,
    compensated traffic cadence, and qdisc lifecycle events.
  - `shaper.c` (`shaper`): drives the CAKE shapers with the controller's
    decisions: controller configuration and input, CAKE bandwidth changes
    with readback, and minimum-rate enforcement.
  - `report.c` (`report`): what the shaper and the activity state report: the
    messages for a changed decision, the `DATA` and `SUMMARY` records, and the
    names they print (`report.h` holds the activity names inline, for the
    status object too).
  - `pingers.c` (`pingers`): the pinger session: start, retries, restart,
    suspension, setup grace, and per-reply latency processing.
  - `native_pinger.c` (`native_pinger`): the native ICMP sockets and probe
    timer on uloop.
  - `child_pingers.c` (`child_pingers`): the fping and irtt children on uloop:
    their output, exits, SIGKILL escalation and irtt's staggered starts.
  - `reflectors.c` (`reflectors`): latency trackers, reflector ordering,
    scheduled comparison and replacement, and health checks.
  - `tcp.c` (`tcp`): the TCP capture, its queue estimate, the upload ACK
    rate, and the timestamp injector's attachment and `TCP_INJECT` records.
  - `status.c` (`status`): the instance's status snapshot from state the
    daemon already holds.
  - `publish.c` (`publish`): the status object on ubus: its reply, its
    identity and the messages for what its server reports; the main thread
    only replaces the snapshot each traffic tick and logs those events. It
    stays apart from `status.c` so its tests can supply any snapshot.
- `constants/`: every named value, one header per topic: `units.h` (units and
  conversions), `program.h` (identity, build features, runtime directory),
  `options.h` (UCI option names, booleans, configuration limits), `states.h`,
  `text.h`, `logging.h`, `mqtt.h`, `latency.h` (pinger methods, command lines,
  output grammar, timing), `icmp.h`, `cake.h` (qdiscs, netlink, accounting),
  `proc.h`, `controller.h` (fixed policy), `tcpdelay.h` (filter object, maps,
  packet layout, estimator) and `ubus.h`. Headers include only the topics
  they build on; each source file includes the topics it uses.
- `common/`: generic operations named for what they do, used by every
  subsystem. A module never keeps its own copy of one.
  - `arith.h`: trivial integer arithmetic as `static inline` functions:
    saturation (`saturating_add`, `saturating_sub`, `saturating_mul`,
    `signed_sum`), `min_u64`, `max_u64`, `mul_div`, rounding
    (`rounded_divide`, `signed_rounded_divide`), ratios per million
    (`ratio_of`, `ratio_of_rounded_up`) and `ARRAY_SIZE`.
  - `convert.h`: unit conversions where a value enters or leaves the daemon
    (`byte_to_bit`, `bit_to_kbit`, `byte_to_kbyte`, `us_to_ms`,
    `us_to_timeout_ms`, `ns_to_us`, `timespec_to_us`, `us_to_sec_decimal`).
  - `direction.h`: `enum direction` (`DIRECTION_DOWNLOAD`, `DIRECTION_UPLOAD`,
    `DIRECTION_COUNT`) and its names. Everything kept per direction is an
    array indexed by it, so code that treats both alike is one loop.
  - `clock.c`: clock readings (`read_clock_us`) and `interval_elapsed`.
  - `rate.c`: rates from counters (`bps`), serialization times
    (`serialization_us`) and ratios (`fraction_to_ratio_e6`, `load_ratio_e6`).
  - `text.c`: decimal parsing (`parse_unsigned`).
  - `random.c`: uniform selection and shuffling from a supplied entropy source.
  - `error.c`: shared error-buffer formatting.
- `config/`
  - `config.c`: typed UCI loading and conversion through `libuci`; never
    parse `/etc/config/cake-adapt` manually.
  - `options.c`: the one table of typed option bindings (`options[]`, whose
    order is also the `-L` listing and the loading order) and each option's
    value in its UCI units, which `-D` prints for the LuCI app; `options.h`
    is private to `config/`.
  - `describe.c`: the `-L` and `-D` listings, and the startup messages that
    say what was loaded and whether the instance controls CAKE.
  - `validate.c`: cross-option validation of a loaded configuration
    (`config_validate`), run before control starts.
  - `defaults.c` and `defaults.h`: built-in application and configuration
    defaults.
- `controller/`
  Platform-independent policy, directly unit-testable with synthetic inputs.
  `controller.h`, `activity.h` and `reflector.h` are its interfaces;
  `direction.h` (one direction's share of an update) is private to the
  directory.
  - `controller.c`: set-up, and one update of both directions in order.
  - `rate.c`: the rate arithmetic: cuts on bufferbloat, increases under
    high load, decay toward the base rate, clamping.
  - `congestion.c`: each direction's line state (saturation hysteresis) and
    bufferbloat detection over the delay window.
  - `coupling.c`: the rules that tie the directions together: the download
    ceiling from upload's ACK share, queue attribution, delivery ratio, and
    serialization compensation.
  - `activity.c`: the load classes cake-autorate prints, and the
    running/idle/stall state.
  - `reflector.c`: reflector health (`reflector_health_*`), comparison, and
    rotation policy over latency trackers.
- `latency/`
  - `icmp.c`: the native probes' IPv4/IPv6 raw sockets, wire packets,
    identifier filters and fixed-width kernel receive timestamps.
  - `native.c`: native probe scheduling (the default pinger), outstanding
    requests, expiry and reply matching; its samples use the same
    logging/tracker contract as the fping and irtt fallbacks.
    `daemon/native_pinger.c` registers its sockets and timer with uloop.
  - `pinger.c`: the pinger methods (`enum pinger_method`): what each runs and
    how it measures (`pinger_is_native`, `pinger_shared_delay`, ...).
  - `latency.c`: the latency session, line splitting, and dispatch to the
    child pinger.
  - `children.c`: the pinger children: command words, spawning, stopping
    and their exits. Children are reaped by the daemon through
    `uloop_process`. The only synchronous, bounded waits are shutdown
    (`latency_stop_now`) and a child whose pipe setup failed (`child_start`).
    `children.h` is the private interface between the session and its child
    pingers; each exports a `struct child_ops` (name, line parser, exit
    handler) that the session calls through.
  - `fping.c` and `irtt.c`: each child pinger's launch arguments and scheduling.
  - `response.c`: reply timestamps: their age (`response_stale`) and their
    place on the monotonic clock (`response_monotonic_us`).
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
  - `path.c`: absolute paths (`path_absolute`) and the pid lock file
    (`path_lock_pid`).
  - `thread.c`: bounded joins of the helper threads (`thread_join_within`,
    for the log writer, MQTT and ubus threads).
  - `instance.c`: the per-instance lock (`/var/run/cake-adapt/<section>.lock`,
    so a second daemon with a running instance's name exits with an error).
  - `netlink.c`: low-level rtnetlink requests, replies, events, and timeouts.
  - `traffic.c`: counter samples and achieved-rate calculations only.
  - `ubus.c`: one ubus object served from a thread of its own, answering from
    a snapshot copied under a lock, so a stalled `ubusd` never blocks
    control; it connects, adds the object and reconnects as dnsmasq does, and
    reports events for its caller to log.
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
- `logging/mqtt.c`: asynchronous MQTT transport through libmosquitto, with optional verified TLS,
  configured by `struct mqtt_settings` (declared in `mqtt.h`, embedded in the configuration);
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

