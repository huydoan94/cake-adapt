# Native ICMP probes: implementation and validation

The native probes are the default `pinger_method`: `native` (IPv4/IPv6 echo)
or `native-ts` (IPv4 timestamp). fping, fping-ts and irtt remain as fallbacks
the user chooses; nothing switches between them automatically. The user
accepted native on 2026-10-21 after the testbed comparisons in
`profiling/2026-10-21-native-icmp-jitter/`: with jittered capacity drops
native-ts matched fping-ts within the testbed's noise.

`latency/icmp.c` owns nonblocking raw sockets, interface binding, protocol and
identifier receive filters, packet encoding/decoding and receive timestamps.
`latency/native.c` owns the send cadence, bounded request storage, matching and
timeouts. `monitor/pingers.c` registers sockets/timers and passes samples into
the existing reflector and controller path. Legacy backends are retained only
for the planned comparison and migration gate.

Echo payloads carry a random session token and full request serial. A reply must
match its source address, socket identifier, sequence and pending request; echo
also matches token/serial and timestamp replies match the original UTC
milliseconds. Duplicates, foreign replies and expired requests are ignored.
The 10-second timeout and minimum 1-ms send spacing prevent live 16-bit sequence
aliases. Request storage scales with timeout/spacing rather than allocating
65,536 entries per reflector. Sends preserve fping's millisecond-rounded period
and minimum spacing. Each next send is scheduled from the previous one's due
time, not from when it went out, so timer lateness does not stretch the period
(the testbed's per-target gaps were about 310 ms for 300 ms when scheduled from
the send). A callback a whole spacing late restarts the schedule from now and
sends once, without a catch-up burst.

Receive timestamps use `SO_TIMESTAMPNS_NEW` with Linux's two signed 64-bit
fields, including on i386 with 64-bit libc `time_t`. Linux recommends the new
timestamp ABI for 32-bit time safety ([kernel documentation](https://docs.kernel.org/networking/timestamping.html)).
When timestamps are unavailable, samples use the read-time clock and log one
warning per session. The existing sample contract keeps realtime
`timestamp_us`/`[seconds.fraction]` for records; response age and RTT use the
converted monotonic receive time. Timestamp probes keep fping-ts's signed raw
clock offsets, zero fields and midnight discontinuities; the existing tracker
handles its marked rollover samples. They do not assume synchronized clocks.

Temporary restrictions: native methods require numeric reflector addresses
to avoid blocking DNS in the control loop, and reject pinger prefixes and extra
arguments. The legacy backends still accept their existing inputs. Hostname
support must be settled before a final replacement is accepted.

Validation includes packet/checksum/length/type/identifier tests, IPv6 echo,
fixed-width ancillary timestamps beyond 2038, request expiry, duplicate/foreign
and late replies, sequence wrap, midnight rollover, socket failure cleanup,
bounded callback work, timer registration/deletion and configuration/LuCI rules.
Send failure advances only that reflector's slot and records degraded state;
other reflectors continue, and successful sends recover the state. Native health
and stale-response checks retain the accepted monotonic receive timestamp, so
a realtime clock step between acceptance and recording cannot rewrite it.
`tests/latency/icmp_driver.c` is a bounded spike driver, built explicitly with
`make -C tests ../build/tests/bin/icmp_driver`; it is not installed.

After the user brought the x86 VM online on 2026-10-13, native IPv4/IPv6 echo
and timestamp probes received every expected reply, all kernel-timestamped.
The original 100-µs median RTT gap was not met: the last paired network gap was
694.5 µs and loopback echo gap 104 µs. The send timestamp now excludes packet
preparation, and relative waits use a refreshed monotonic clock. Focused tests
and Astra Medium review cover those fixes. Runtime evidence and limitations
are in [the spike report](../../profiling/2026-10-13-native-icmp-spike/README.md).
The revised timing gate subsequently passed quiet-host and VM-loopback checks,
then one serialized network batch: 300 replies per cohort, native/fping median
gap 65 µs within the two fping-only medians' 150-µs spread. All 300 native
replies were kernel-timestamped; see [the gate report](../../profiling/2026-10-13-native-icmp-spike/interleaved-gate/README.md).
Reduced CPU, control equivalence and router performance remain unproven.
Comparative runs and lifecycle checks
are still required before deleting legacy code or changing the default; the
field run stays with the user.

The first daemon control comparison exposed a native timestamp conversion bug:
`us_to_ms()` rounds and saturates durations for uloop timers, so modern epoch
times became a constant timestamp. Native timestamp requests and arrivals now
divide the 64-bit epoch by `MILLISECOND` before reducing it to UTC milliseconds
since midnight. A modern fractional-epoch regression checks the transmitted
origin and both one-way delays; it fails against the original conversion.
The x86 SDK build and Astra Medium review pass.
