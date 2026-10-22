# cake-adapt configuration

cake-adapt is configured through UCI in `/etc/config/cake-adapt`. Each
`config cake_adapt '<name>'` section runs as its own instance. The packaged file
lists only the common options; everything else uses the defaults below.

For development or agent work, follow the [development guide](development.md)
and the Claude or Codex route in [COLLABORATION.md](../COLLABORATION.md).

Change options with `uci`, by editing the file, or in LuCI under
**Services → CAKE Adapt → Configuration** (package `luci-app-cake-adapt`),
then reload or restart the service:

```sh
uci set cake-adapt.main.adjust_dl_shaper_rate='1'
uci commit cake-adapt
/etc/init.d/cake-adapt restart
```

An invalid configuration is refused, and the reason is written to syslog
(`logread -e cake-adapt`): a running instance keeps running its previous
configuration, and one that is not running does not start. Option names and meanings follow
[cake-autorate](https://github.com/lynxthecat/cake-autorate)'s configuration,
so its documentation also applies; differences are noted below.

- [Basic](#basic)
- [Shaper rates](#shaper-rates)
- [Reflectors and pinger](#reflectors-and-pinger)
- [Output and logging](#output-and-logging)
- [TCP measurement (eBPF)](#tcp-measurement-ebpf)
- [Idle, sleep and stall](#idle-sleep-and-stall)
- [Controller tuning](#controller-tuning)
- [Reflector health](#reflector-health)
- [Timing](#timing)
- [Multiple instances](#multiple-instances)
- [Options only root sets](#options-only-root-sets)
- [Standalone shell configuration](#standalone-shell-configuration)

Booleans are `0` or `1`. Durations and rates carry their unit in the option
name.

## Basic

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | boolean | `0` | Run this instance. |
| `interface` | string | *(none)* | Upload interface: the one with the root CAKE qdisc that SQM creates. Download uses `ifb4<interface>`, truncated to Linux's 15-character limit. |
| `ul_if` | string | *(none)* | Upload interface, overriding `interface`. Must be set together with `dl_if`. |
| `dl_if` | string | *(none)* | Download interface, overriding `interface`. Must be set together with `ul_if`. |
| `adjust_dl_shaper_rate` | boolean | `0` | Adjust the download CAKE rate. With `0` the direction is only observed. cake-autorate defaults to `1`. |
| `adjust_ul_shaper_rate` | boolean | `0` | Adjust the upload CAKE rate. With `0` the direction is only observed. cake-autorate defaults to `1`. |

Either `interface` or both `ul_if` and `dl_if` are required. Setting only one
of `ul_if` and `dl_if` is an error. When all three are set, the pair wins and a
warning is logged.

cake-adapt adjusts existing CAKE qdiscs. An SQM setup must create them first;
cake-adapt follows them as they appear and disappear.

The root qdisc may also be `cake_mq`, the multi-queue CAKE (Linux 7.0,
backported in OpenWrt 25.12), which runs one CAKE per transmit queue so
shaping can use several CPU cores. cake-adapt reads and sets its shared rate
like a single CAKE's. Note that `cake_mq` divides the rate equally among the
queues that are busy: when traffic is uneven, for example one bulk upload
beside a download's ACKs, the busy flow gets only its share. On the testbed
with 4 queues, a bidirectional load passed 3.3 Mbit/s of upload through a
6 Mbit/s shaper (plain CAKE: about 7 Mbit/s;
`profiling/2026-10-10-cake-mq`). Use it where one core cannot shape the line
rate.

## Shaper rates

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `min_dl_shaper_rate_kbps` | integer | `5000` | Lowest download rate. |
| `base_dl_shaper_rate_kbps` | integer | `20000` | Download rate at low load. |
| `max_dl_shaper_rate_kbps` | integer | `80000` | Highest download rate. |
| `min_ul_shaper_rate_kbps` | integer | `5000` | Lowest upload rate. |
| `base_ul_shaper_rate_kbps` | integer | `20000` | Upload rate at low load. |
| `max_ul_shaper_rate_kbps` | integer | `35000` | Highest upload rate. |

Each direction requires `minimum <= base <= maximum`. As in cake-autorate:

- **minimum:** the lowest rate the line reliably delivers without
  bufferbloat;
- **base:** the steady rate when the line is lightly loaded;
- **maximum:** the highest rate the controller may try.

See cake-autorate's
[theory of operation](https://github.com/lynxthecat/cake-autorate#theory-of-operation)
for choosing them.

## Reflectors and pinger

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `reflectors` | list | *(required)* | Hosts to ping. They should answer ICMP reliably and quickly, such as public DNS servers. |
| `no_pingers` | integer | `6` | Reflectors pinged at a time; from 1 up to the number of reflectors. |
| `reflector_ping_interval_s` | decimal | `0.3` | Interval between pings to each active reflector. |
| `randomize_reflectors` | boolean | `1` | Shuffle the reflector list at start. |
| `pinger_method` | string | `native` | `native` sends ICMP echo requests from the daemon itself and gives each direction half the round trip. `native-ts` sends ICMP timestamp requests for separate one-way delays (IPv4 only). The fallbacks run a separate program, whose package must be installed: `fping` and `fping-ts` as their native counterparts, and `irtt`, which is experimental and has not been run against a real server. |
| `irtt_session_duration_m` | decimal | `10` | Length of each IRTT session (`irtt` only). |

Reflectors come only from this list; cake-autorate's remote reflector list is
not supported.

`native` is a deliberate difference from cake-autorate, whose default is
fping: the probes need no child process and no extra package, and used less
CPU in total than fping on the test VM. They take numeric IP addresses only, so a lookup
that depends on the link being measured never delays them; for a hostname,
use a fallback. `native` and `native-ts` refuse `ping_extra_args` and
`ping_prefix_string`, and never switch to another method on their own. See
[native ICMP probes](design/native-icmp.md) for the design and its limits.

fping is not installed with cake-adapt. An instance set to `fping` or
`fping-ts` without it, or to `irtt` without irtt, does not start, and syslog
says which package to install. Their extra arguments (`ping_extra_args`)
and wrapper (`ping_prefix_string`) are set in `/etc/cake-adapt/exec` (see
[Options only root sets](#options-only-root-sets)).

## Output and logging

Logs go to `/var/log/cake-adapt.log` for the `main` section and
`/var/log/cake-adapt.<section>.log` for other sections. On OpenWrt `/var` is in
RAM, so the defaults never write to flash. Another directory is set with
`log_file_path_override`, which only root sets (see
[Options only root sets](#options-only-root-sets)).

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `log_to_file` | boolean | `1` | Write the log file. |
| `mqtt_enabled` | boolean | `0` | Forward generated cake-adapt records to MQTT on a background sender; requires the `cake-adapt-mqtt` or `cake-adapt-mqtt-ssl` package. |
| `mqtt_host` | string | *(empty)* | Broker hostname or IPv4/IPv6 address, without a URL scheme, brackets or port. Required when MQTT is enabled. |
| `mqtt_port` | integer | `1883` | Broker TCP port, 1–65535. Set the broker's TLS port when TLS is enabled (commonly 8883). |
| `mqtt_topic` | string | `cake-adapt/log` | Publish topic, without MQTT wildcards (`+`, `#`). Use distinct topics for instances or routers as needed. |
| `mqtt_username` | string | *(empty)* | Optional broker username. |
| `mqtt_password` | string | *(empty)* | Optional password; requires a username. Stored in UCI and the instance runtime file. |
| `mqtt_tls` | boolean | `0` | Use certificate-verified TLS; requires the `cake-adapt-mqtt-ssl` package. |
| `mqtt_ca_file` | string | `/etc/ssl/certs/ca-certificates.crt` | CA certificate file for TLS; an absolute path is required when TLS is enabled. |
| `log_file_max_size_KB` | integer | `2000` | Rotate when the log reaches this size. One `.old` copy is kept. |
| `log_file_max_time_mins` | decimal | `10` | Rotate after this time. |
| `log_file_buffer_timeout_ms` | integer | `500` | Longest time a record waits in the write buffer. |
| `log_file_export_compress` | boolean | `1` | Compress the export written on `SIGUSR1`. |
| `debug` | boolean | `1` | Include debug messages in the log file. |
| `log_DEBUG_messages_to_syslog` | boolean | `0` | Also send debug messages to syslog. |
| `output_processing_stats` | boolean | `0` | Log a `DATA` record per ping reply (and `TCP_QUEUE` records with `tcp_delay_attribution`, and a `TCP_INJECT` record each minute with `tcp_ts_request`). |
| `output_load_stats` | boolean | `0` | Log a `LOAD` record per traffic sample. |
| `output_reflector_stats` | boolean | `0` | Log `REFLECTOR` records at each reflector comparison. |
| `output_summary_stats` | boolean | `0` | Log a `SUMMARY` record per ping reply. |
| `output_cake_changes` | boolean | `0` | Log a `SHAPER` record for each rate change. |
| `output_cpu_stats` | boolean | `0` | Log CPU usage. |
| `output_cpu_raw_stats` | boolean | `0` | Log raw CPU counters. |
| `output_memory_stats` | boolean | `0` | Not in cake-autorate: log the daemon's own memory every 10 s as a `MEMORY` record: resident (`RSS_KB`), peak resident (`PEAK_RSS_KB`: the highest VmHWM or sampled RSS, as the kernel updates VmHWM lazily), resident heap (`RSS_ANON_KB`) and data size (`DATA_KB`), from `/proc/self/status`. About 60,000 lines a week; use it to spot leaks on long runs. |

MQTT settings are in LuCI under **Advanced → Logging**. Each MQTT message is
one generated log line, unchanged, without the terminating newline: structured
records, headers, and messages allowed by the existing output and debug options.
It sends only this instance's cake-adapt records, independently of `log_to_file`;
it does not collect other services' syslog or replay existing log files.

What MQTT can do depends on the installed package variant; every variant keeps
all the MQTT options in its configuration:

| Package | `mqtt_enabled '1'` | `mqtt_tls '1'` (with MQTT on) | LuCI shows |
| --- | --- | --- | --- |
| `cake-adapt` | warning, nothing sent | refused | no MQTT settings |
| `cake-adapt-mqtt` (`libmosquitto-nossl`) | plain MQTT | refused | MQTT settings without TLS |
| `cake-adapt-mqtt-ssl` (`libmosquitto-ssl`) | plain MQTT or TLS | verified TLS | MQTT and TLS settings |

With `cake-adapt`, `mqtt_enabled '1'` does not stop the instance: it starts,
controls CAKE as usual, sends nothing, and logs the warning `MQTT logging
degraded: this package was built without MQTT support; install cake-adapt-mqtt or
cake-adapt-mqtt-ssl`; its broker settings are not checked. A refused TLS setting
stops `cake-adapt -V` and the start of that instance with `option 'mqtt_tls'
requires the cake-adapt-mqtt-ssl package`; the daemon never sends plain traffic in
place of TLS. Settings a package cannot use are kept in UCI, and are checked
again only once MQTT is on. LuCI reads the installed variant from
`/usr/share/cake-adapt/features` (empty, `mqtt`, or `mqtt` and `tls`), hides
what it cannot use, and applies the same rules on screen; while an instance of
`cake-adapt` still has `mqtt_enabled` on, LuCI shows that switch with a warning
(saving is not blocked) so it can be turned off. A daemon older than the
variants has no features file, so LuCI hides its MQTT settings; install the
daemon and LuCI packages of the same release.

With `cake-adapt-mqtt-ssl`, enable `mqtt_tls`, set the broker's TLS port, and use
`mqtt_ca_file` for the CA certificate file. Its default is supplied by the
package's `ca-bundle` dependency. Custom CA files must use absolute paths.
Certificates and broker hostnames are always verified; there is no insecure mode
or plaintext fallback.

TLS remains off by default. Plain connections send traffic, including
username/password credentials, unencrypted. `cake-adapt-mqtt` has no OpenSSL or
CA-bundle dependency, and `cake-adapt` no MQTT library at all.

The main thread only tries to copy a record into a 32-record queue (64 KiB when
enabled). A sender thread and a network thread using libmosquitto handle publishing,
DNS and authentication. Disconnected, busy or full queues silently drop
records. Delivery uses QoS 0 with no retained messages or disk spool; records
longer than 2047 bytes are also dropped. Each connection attempt has a five-second
handshake deadline; attempts start every ten seconds while disconnected, with
no retry limit. DNS resolution and network-thread cleanup stay off the control
thread: a broker name that cannot be resolved delays only MQTT, never rate
control, and at most one such lookup is left running. Shutdown waits at most
about one second for the sender. Queued records
are discarded on disconnect, so reconnect does not replay stale logs.

The structured records keep cake-autorate's names, fields and units, so its
log analysis tools work on them. The `output_*` records are frequent: enable
them for diagnosis, not permanently.

Signals: `SIGUSR1` exports the current logs, and `SIGUSR2` empties them in
place. A `tail -f` stays attached across rotation and reset.

Logging never holds up measurement and control: a separate thread writes the
log file, the terminal and syslog, and does rotation, export and reset, from a
queue of up to 128 KB. If it falls behind, for example while logd is stuck,
the queue keeps the newest lines: the oldest waiting ones make room and are
counted, and the log says how many at the point where they are missing (`N
older log lines and M syslog messages were overwritten: the log writer fell
behind`). A `SIGUSR1` or
`SIGUSR2` request is never lost this way.

## TCP measurement (eBPF)

These options are not in cake-autorate. They load an eBPF socket filter,
`/lib/bpf/cake-adapt-tcpdelay.o`, on the upload interface. Without kernel BPF
support the instance logs a warning and carries on without them.

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `tcp_delay_attribution` | boolean | `0` | With an echo pinger (`native` or `fping`): split its round-trip delay between upload and download by queue delays measured from TCP timestamps, instead of half each way. Only the direction holding the queue is then slowed. |
| `ul_congest_ack_share` | decimal | `0` | From `0` to `1`; `0` disables it. While upload is busy, download is held back so that its ACKs leave room for other upload traffic, but never below this share of the upload rate, for example `0.45`. It is not a reservation: when ACKs need less, other traffic uses the rest. |

For a slow upload, CAKE's plain `ack-filter` on the upload qdisc also helps.
Do not use `ack-filter-aggressive`.

### TCP timestamp injection (experimental)

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `tcp_ts_request` | boolean | `0` | Experimental; needs `tcp_delay_attribution`. Renamed from `tcp_timestamp_inject`, which is no longer read: update existing UCI configurations. Adds a TCP timestamp to clients' IPv6 SYNs that lack one, so the filter can measure those connections too. Windows does not send TCP timestamps; with this option on, its IPv6 connections are measured in both directions. IPv4 is never changed. |

Windows opens TCP connections without timestamps, so the TCP filter cannot
measure them. With `tcp_ts_request`, a second program in the same eBPF
object rewrites such an IPv6 SYN on its way out of the upload interface: it
gets a timestamp, the server answers with its own, and a client that accepts it
(Windows does) then sends timestamps itself. Clients that already send
timestamps, and every IPv4 packet, are not touched.

Adding that option to a SYN is the only change made to any packet. A server
that does not take the timestamp is remembered by its address and port for a
day, and SYNs to it are left alone meanwhile, from every client: it answered
without a timestamp, reset the SYN, or answered only once the SYN was resent
without the timestamp, or its answer never got through to the client (the
client resent its SYN after the SYN-ACK, or reset the connection before
sending anything). Whatever refused it on the way, the router sees the same
refused handshake. The connection that was refused is not rescued; the next
ones are. A stall (below) is not a refusal and skips nothing.

The `TCP_INJECT` record counts the SYNs injected (`INJECTED`), the refused
handshakes and the SYNs left alone because their server is skipped
(`SKIPPED`), the injected handshakes that worked (`ACCEPTED`: the server
acknowledged the client's first data, which a stalled handshake never gets),
and the stalled handshakes (`STALLED`).

**Client clocks.** A client that adopts the timestamp then sends values from
its own clock, and servers drop segments whose value looks older than the last
one they saw (PAWS, RFC 7323), comparing 32-bit values by their signed
difference: one injected value suits only clocks within 2^31 ticks after it.
Windows counts milliseconds since boot, and Fast Startup keeps that uptime
running across shutdowns, so clocks of any age are common. A clock outside the
window makes the server drop the client's handshake reply and send its SYN-ACK
again: the handshake stalls, and that connection fails.

Each IPv6 client keeps its own address, so it gets its own value:

1. 1 while nothing is known about it, which suits uptimes up to 24.8 days;
2. 0x80000000 after a handshake with 1 stalled, which suits uptimes of 24.8
   to 49.7 days;
3. its own clock once learned, from the first connection it adopts our
   timestamp on (usually the stalled one already), which suits any uptime.

What is known about a client lasts a day. A stall skips no server. IPv4 is
left alone: behind NAT every client shares the router's address, so one value
would have to suit every client's clock, and the clients it does not suit stall
their connections. IPv6 behind NAT66 or NPTv6 is not handled either; there,
leave the option off.

A Windows PC you manage can send timestamps itself, which needs no injection
at all (as administrator: `netsh int tcp set global timestamps=enabled`). The programs are
attached with tcx, so SQM's qdiscs are not affected, and the kernel removes
them when cake-adapt stops, however it stops. It is opt-in because it changes
clients' SYNs: the rewrite is the kind a router's MSS clamping already does,
but adds an option instead of changing one.

## Idle, sleep and stall

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `enable_sleep_function` | boolean | `1` | Stop pinging when the line is idle. |
| `connection_active_thr_kbps` | integer | `2000` | Traffic above this rate counts as active. With sleep enabled, it must not exceed either minimum shaper rate. |
| `sustained_idle_sleep_thr_s` | decimal | `60.0` | Idle time before sleeping. |
| `min_shaper_rates_enforcement` | boolean | `0` | Set both rates to their minimum on sleep or a global ping timeout. |
| `stall_detection_thr` | integer | `5` | Consecutive missed replies, with traffic below `connection_stall_thr_kbps`, that mean a stalled connection. |
| `connection_stall_thr_kbps` | integer | `10` | Traffic below this rate allows a stall. |
| `global_ping_response_timeout_s` | decimal | `10.0` | No reply from any reflector for this long restarts the pingers. |

cake-autorate's `startup_wait_s` does not exist: cake-adapt follows the kernel's
qdisc events instead of waiting a fixed time.

## Controller tuning

The defaults are cake-autorate's and suit most lines.

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `dl_owd_delta_delay_thr_ms` | decimal | `30.0` | Download delay increase that counts as bufferbloat. |
| `ul_owd_delta_delay_thr_ms` | decimal | `30.0` | Upload delay increase that counts as bufferbloat. |
| `dl_avg_owd_delta_max_adjust_up_thr_ms` | decimal | `10.0` | At high load, the download increase shrinks from its largest step at no added delay to its smallest at this average delay increase. |
| `ul_avg_owd_delta_max_adjust_up_thr_ms` | decimal | `10.0` | The same for upload. |
| `dl_avg_owd_delta_max_adjust_down_thr_ms` | decimal | `60.0` | On bufferbloat, the download cut grows from its smallest step at `dl_owd_delta_delay_thr_ms` to its largest at this average delay increase. |
| `ul_avg_owd_delta_max_adjust_down_thr_ms` | decimal | `60.0` | The same for upload. |
| `bufferbloat_detection_window` | integer | `6` | Recent samples examined for bufferbloat. |
| `bufferbloat_detection_thr` | integer | `3` | Delayed samples within the window that mean bufferbloat. |
| `high_load_thr` | decimal | `0.75` | Share of the shaper rate in use that counts as high load. |
| `shaper_rate_min_adjust_down_bufferbloat` | decimal | `0.99` | Smallest cut on bufferbloat (multiplier). |
| `shaper_rate_max_adjust_down_bufferbloat` | decimal | `0.75` | Largest cut on bufferbloat (multiplier). |
| `shaper_rate_min_adjust_up_load_high` | decimal | `1.0` | Smallest increase at high load (multiplier). |
| `shaper_rate_max_adjust_up_load_high` | decimal | `1.04` | Largest increase at high load (multiplier). |
| `shaper_rate_adjust_down_load_low` | decimal | `0.99` | Decay toward base from above at low load (multiplier). |
| `shaper_rate_adjust_up_load_low` | decimal | `1.01` | Recovery toward base from below at low load (multiplier). |
| `bufferbloat_refractory_period_ms` | integer | `300` | Minimum time between cuts. |
| `decay_refractory_period_ms` | integer | `1000` | Minimum time between low-load decay steps. |
| `alpha_baseline_increase` | decimal | `0.001` | How fast a reflector's baseline rises. |
| `alpha_baseline_decrease` | decimal | `0.9` | How fast a reflector's baseline falls. |
| `alpha_delta_ewma` | decimal | `0.095` | Smoothing of the delay increase. |

## Reflector health

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `reflector_health_check_interval_s` | decimal | `1.0` | Interval between health checks. |
| `reflector_response_deadline_s` | decimal | `1.0` | A reflector silent for longer commits an offence. |
| `reflector_misbehaving_detection_window` | integer | `60` | Health checks examined for offences. |
| `reflector_misbehaving_detection_thr` | integer | `3` | Offences within the window that replace a reflector. |
| `reflector_replacement_interval_mins` | decimal | `60` | Interval for replacing a random reflector anyway. |
| `reflector_comparison_interval_mins` | decimal | `1` | Interval for comparing active reflectors. |
| `reflector_sum_owd_baselines_delta_thr_ms` | decimal | `20.0` | Baseline difference from the best reflector that replaces one. |
| `reflector_owd_delta_ewma_delta_thr_ms` | decimal | `10.0` | Delay-increase difference from the best reflector that replaces one. |
| `retain_reflector_stats` | boolean | `1` | Keep a replaced reflector's baseline for when it is used again. |

## Timing

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `monitor_achieved_rates_interval_ms` | integer | `200` | Traffic sampling interval. |
| `monitor_cpu_usage_interval_ms` | integer | `2000` | CPU sampling interval, with `output_cpu_stats`. |
| `if_up_check_interval_s` | decimal | `10.0` | Retry interval for a missing interface or qdisc, and for a pinger that failed to start. |

## Multiple instances

Each named section runs separately, for example for two WAN links:

```uci
config cake_adapt 'primary'
	option enabled '1'
	option interface 'wan1'
	...

config cake_adapt 'secondary'
	option enabled '1'
	option interface 'wan2'
	...
```

Each instance needs its own pair of CAKE qdiscs; two instances must never
control the same ones. Logs are named after the section, for example
`/var/log/cake-adapt.primary.log`.

## Options only root sets

Four options make root run a program or a file, choose its arguments, or
choose where it writes, so they are not read from `/etc/config/cake-adapt`,
which the LuCI app can change. They are set in `/etc/cake-adapt/exec`, a file in UCI syntax that
only root may read or change and the LuCI app cannot reach, in a
`cake_adapt_exec` section named after the instance:

```uci
config cake_adapt_exec 'main'
	option config_file '/etc/cake-adapt/config.main.sh'
	option ping_prefix_string 'mwan3 use wan'
	option ping_extra_args '-S 192.0.2.1'
	option log_file_path_override '/tmp/cake-adapt'
```

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `config_file` | string | *(empty)* | A cake-autorate format shell configuration that fills the options the instance's section leaves unset (below). |
| `ping_prefix_string` | string | *(empty)* | Command words placed before `fping` or `irtt`, for example a wrapper that runs it in another routing context. |
| `ping_extra_args` | string | *(empty)* | Extra arguments for `fping` or `irtt` (irtt's `-o`, for one, writes a file as root). `native` and `native-ts` refuse it. |
| `log_file_path_override` | string | *(empty)* | Directory for the log file instead of `/var/log`; the file name stays the same. A relative path is taken from where the daemon starts (`/` for the service). A directory on flash wears it. Root-only because root writes the log there. |

Any of them set in `/etc/config/cake-adapt` refuses the instance's
configuration, and syslog says where it belongs; a running instance keeps
its previous configuration, and the LuCI page shows a warning. So does an
`/etc/cake-adapt/exec` that uci cannot parse, rather than being read as
empty. `cake-adapt` started by hand without `-C` reads
`/etc/config/cake-adapt` and refuses `ping_prefix_string`,
`ping_extra_args` and `log_file_path_override` there too. procd does not watch `/etc/cake-adapt/exec`:
after changing it, run `/etc/init.d/cake-adapt reload`.

## Standalone shell configuration

`config_file` names a cake-adapt shell configuration in cake-autorate's
format, so an existing configuration can be reused:

```uci
config cake_adapt 'main'
	option enabled '1'
```

```bash
ul_if='eth1'
dl_if='ifb4eth1'
adjust_dl_shaper_rate=1
min_dl_shaper_rate_kbps=5000
base_dl_shaper_rate_kbps=20000
max_dl_shaper_rate_kbps=80000
reflectors=(1.1.1.1 1.0.0.1 8.8.8.8 8.8.4.4 9.9.9.9 9.9.9.10)
```

Precedence is built-in defaults, then the file, then UCI: an option set in the
UCI section keeps its UCI value, and the file fills in only the options UCI
leaves unset. To take a value from the file, remove that option from the
section, as in the minimal section above. `enabled` stays in UCI, and
`config_file` in `/etc/cake-adapt/exec`.

How the file is handled:

- At service start, each instance gets its own runtime file,
  `/var/run/cake-adapt/<section>.uci`, from which it runs. It holds
  only that instance's section, copied from UCI with `uci get` and `uci set`
  for every option cake-adapt knows; a separate Bash process reads the shell
  file and fills in the options UCI leaves unset.
- That result is validated like UCI; cake-adapt itself never executes the
  file.
- The service reloads when `/etc/config/cake-adapt` changes. A change to
  `/etc/cake-adapt/exec` or the shell file is not watched: it is picked up at the next `/etc/init.d/cake-adapt reload` or
  `restart`.

The file is executable Bash: use only a trusted file that only root can
change. Bash is not
a package dependency, so install it (`bash`) to use `config_file`. Without
it, the instance does not start and syslog says `cannot read Bash file
<path>: bash is not installed`; instances configured only in UCI never need
Bash.
