# cake-adapt configuration

cake-adapt is configured through UCI in `/etc/config/cake-adapt`. Each
`config cake_adapt '<name>'` section runs as its own instance. The packaged file
lists only the common options; everything else uses the defaults below.

Change options with `uci` or by editing the file, then restart the service:

```sh
uci set cake-adapt.main.adjust_dl_shaper_rate='1'
uci commit cake-adapt
/etc/init.d/cake-adapt restart
```

An invalid configuration stops that instance from starting, and the reason is
written to syslog (`logread -e cake-adapt`). Option names and meanings follow
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
| `pinger_method` | string | `fping` | `fping` measures round trips and gives each direction half. `fping-ts` uses ICMP timestamps for separate one-way delays; it accepts IPv4 reflectors only and is verified only on a test bench. `irtt` is experimental, has not been run against a real server, and needs the separate `irtt` package. |
| `ping_extra_args` | string | *(empty)* | Extra arguments for the pinger. |
| `ping_prefix_string` | string | *(empty)* | Command words placed before the pinger, for example a wrapper. |
| `irtt_session_duration_m` | decimal | `10` | Length of each IRTT session (`irtt` only). |

Reflectors come only from this list; cake-autorate's remote reflector list is
not supported. An instance whose pinger executable is missing does not start.

## Output and logging

Logs go to `/var/log/cake-adapt.log` for the `main` section and
`/var/log/cake-adapt.<section>.log` for other sections. On OpenWrt `/var` is in
RAM, so the defaults never write to flash.

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `log_to_file` | boolean | `1` | Write the log file. |
| `log_file_path_override` | string | *(empty)* | Directory for the log file instead of `/var/log`; the file name stays the same. A directory on flash wears it. |
| `log_file_max_size_KB` | integer | `2000` | Rotate when the log reaches this size. One `.old` copy is kept. |
| `log_file_max_time_mins` | decimal | `10` | Rotate after this time. |
| `log_file_buffer_timeout_ms` | integer | `500` | Longest time a record waits in the write buffer. |
| `log_file_export_compress` | boolean | `1` | Compress the export written on `SIGUSR1`. |
| `debug` | boolean | `1` | Include debug messages in the log file. |
| `log_DEBUG_messages_to_syslog` | boolean | `0` | Also send debug messages to syslog. |
| `output_processing_stats` | boolean | `0` | Log a `DATA` record per ping reply (and `TCP_QUEUE` records with `tcp_delay_attribution`). |
| `output_load_stats` | boolean | `0` | Log a `LOAD` record per traffic sample. |
| `output_reflector_stats` | boolean | `0` | Log `REFLECTOR` records at each reflector comparison. |
| `output_summary_stats` | boolean | `0` | Log a `SUMMARY` record per ping reply. |
| `output_cake_changes` | boolean | `0` | Log a `SHAPER` record for each rate change. |
| `output_cpu_stats` | boolean | `0` | Log CPU usage. |
| `output_cpu_raw_stats` | boolean | `0` | Log raw CPU counters. |
| `output_memory_stats` | boolean | `0` | Not in cake-autorate: log the daemon's own memory every 10 s as a `MEMORY` record: resident (`RSS_KB`), peak resident (`PEAK_RSS_KB`), resident heap (`RSS_ANON_KB`) and data size (`DATA_KB`), from `/proc/self/status`. About 60,000 lines a week; use it to spot leaks on long runs. |

The structured records keep cake-autorate's names, fields and units, so its
log analysis tools work on them. The `output_*` records are frequent: enable
them for diagnosis, not permanently.

Signals: `SIGUSR1` exports the current logs, and `SIGUSR2` empties them in
place. A `tail -f` stays attached across rotation and reset.

## TCP measurement (eBPF)

These options are not in cake-autorate. They load an eBPF socket filter,
`/lib/bpf/cake-adapt-tcpdelay.o`, on the upload interface. Without kernel BPF
support the instance logs a warning and carries on without them.

| Name | Type | Default | Description |
| --- | --- | --- | --- |
| `tcp_delay_attribution` | boolean | `0` | With `pinger_method fping`: split fping's round-trip delay between upload and download by queue delays measured from TCP timestamps, instead of half each way. Only the direction holding the queue is then slowed. |
| `ul_congest_ack_share` | decimal | `0` | From `0` to `1`; `0` disables it. While upload is busy, download is held back so that its ACKs leave room for other upload traffic, but never below this share of the upload rate, for example `0.45`. It is not a reservation: when ACKs need less, other traffic uses the rest. |

For a slow upload, CAKE's plain `ack-filter` on the upload qdisc also helps.
Do not use `ack-filter-aggressive`.

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

## Standalone shell configuration

`config_file` (string, UCI only) names a cake-adapt shell configuration in
cake-autorate's format, so an existing configuration can be reused:

```uci
config cake_adapt 'main'
	option enabled '1'
	option config_file '/etc/cake-adapt/config.main.sh'
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

Precedence is built-in defaults, then UCI, then the file. `enabled` and
`config_file` stay in UCI.

How the file is handled:

- At service start, a separate Bash process reads the file. It applies only
  the option names cake-adapt knows, and writes the result to
  `/tmp/cake-adapt-config/<section>/cake-adapt`.
- That result is validated like UCI; cake-adapt itself never executes the
  file.
- The service reloads when the file or UCI changes.

The file is executable Bash: use only a trusted, root-owned file.
