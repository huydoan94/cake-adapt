# Developing cake-adapt

See the [technical overview](technical.md) for the design and the
[flowcharts](../flowchart/README.md) for the event flow.

## Building with an OpenWrt SDK

This repository is an OpenWrt package source tree. Clone it one level below an
OpenWrt 25.12 SDK's `package` directory and compile only this package:

```sh
cd /path/to/openwrt-sdk
git clone https://github.com/huydoan94/cake-adapt.git package/cake-adapt
make package/cake-adapt/compile V=s
```

The APK is written below:

```text
bin/packages/<architecture>/base/
```

GitHub Actions also discovers the OpenWrt 25.12 architecture list, runs the
host tests, builds each architecture, and publishes packages as workflow
artifacts. Tags beginning with `v` additionally publish the packages as GitHub
release assets.

## Editor setup

Editors resolve includes through `compile_commands.json`, which
`tools/compile-commands.sh` (or the VS Code task *Generate
compile_commands.json (IntelliSense)*) writes from the x86 SDK beside the
repository or `OPENWRT_SDK_X86`. It covers every C file under `src/`, `tests/`
and `tools/`, including the socket filter with the SDK's BPF clang and the
kernel headers, so libbpf's `<bpf/...>` and libnl-tiny resolve. The file holds
absolute paths and is not committed; regenerate it after adding or moving a
source file.

## Source layout

```text
src/
├── main.c              CLI, configuration loading, logging setup, lifecycle
├── monitor/            uloop event loop; one struct monitor, a part per file:
│   ├── monitor.c         loop setup and teardown, traffic tick, activity
│   │                     state, CPU and log timers, signals
│   ├── links.c           both directions' CAKE, achieved rates, qdisc events
│   ├── control.c         controller input, CAKE updates and readback, records
│   ├── pingers.c         pinger start, output, exit, restart, and grace
│   ├── reflectors.c      latency trackers, reflector order, health, replacement
│   └── tcpdelay.c        TCP capture, queue estimate, upload ACK rate
├── common/             shared constants, generic helpers, error formatting
├── config/             typed UCI loading (config.c), validation
│                       (validate.c), and built-in defaults
├── controller/         rate/congestion/activity policy (controller.c) and
│                       reflector health and selection (reflector.c)
├── latency/            pinger sessions, fping/IRTT backends, parsing, tracking
├── cake/               CAKE discovery, state decoding, and bandwidth updates
├── platform/           rtnetlink transport, traffic rates, CPU and memory sampling
└── logging/            syslog, structured records, rotation, export, reset
```

`controller/` has no knowledge of UCI, netlink, processes, or OpenWrt, and is
tested directly with synthetic inputs and recorded upstream traces. The
current-code [flowcharts](../flowchart/README.md) follow this layout.

Headers are included relative to `src/`. The tests in `tests/` mirror the same
directories; objects and test binaries are written below `build/`.

## Host tests

The controller and most measurement logic can be tested without rebuilding an
OpenWrt SDK:

```sh
make -C tests check check-netlink
```

`check` includes `test_replay`, which feeds two recorded cake-autorate traces
(`tests/controller/fixtures/`) through the controller. Delay counts and
bufferbloat decisions must match upstream exactly; because cake-adapt does not
copy upstream's integer truncations, average delays may differ by 1 µs, rates
by 0.5% and compensated thresholds by 3 µs.

The host needs a C compiler, `zlib`, and development headers for libnl 3. The
optional configuration test additionally needs `libuci` and `libubox`
development headers, but not the native libraries:

```sh
make -C tests check-config
```

If the host lacks those headers, `UCI_CFLAGS` can point at a directory that
exposes an OpenWrt SDK's staged `uci.h` and `libubox/` headers:

```sh
make -C tests check-config UCI_CFLAGS="-isystem /path/to/uci-headers"
```

A sanitizer run uses a separate object directory:

```sh
make -C tests check check-netlink OBJECT_DIR=../build/sanitize \
    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    LDFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
```

The production daemon itself is built with strict warnings, including
`-Wall`, `-Wextra`, `-Wpedantic`, `-Wformat=2`, `-Wshadow`, `-Wconversion`, and
`-Werror`.

## Recording a router's log

`tools/capture-log.sh` streams a router's cake-adapt log over SSH into a local
file for long recordings. It follows the daemon's in-place rotation, reconnects
after dropped connections or router reboots without losing lines (see the
limitations below for repeats), and archives the local file with gzip at a
size limit (1024 MB by default):

```sh
tools/capture-log.sh root@router cake-adapt.log            # status screen
tools/capture-log.sh -s 512 root@router cake-adapt.log     # archive at 512 MB
tools/capture-log.sh -l root@router cake-adapt.log         # print lines instead
```

The router needs SSH key authentication; nothing is installed on it. Capture
events (connect, disconnect, archive, stop) go to `cake-adapt.log.events`.

By default the console shows a status screen that is redrawn in place every
second (this one was captured from the test VM's emulated ISP under load):

```text
Connection   connected since 2026-10-07 11:50:10 (45 s), reconnects 0
Throughput   download 22.5 Mbit/s (2.7 MB/s), upload 2.7 Mbit/s (328.4 KB/s)
Log capture  450.6 KB this run at 11.9 KB/s; file 683.5 KB of 1.0 GB; 0 archived
Router       cake-adapt 0.3.8-r1, PID 6388, started 2026-10-07 11:49:46 (1 min 9 s ago)
Last record  2026-10-07 11:50:55 (0 s ago)

                 achieved          shaper   load condition     avg delay  delayed  TCP queue
Download      22.5 Mbit/s     25.2 Mbit/s    86% dl_high          1.6 ms      0/6     445 µs
Upload         2.7 Mbit/s      3.0 Mbit/s    89% ul_high          2.8 ms      0/6     2.7 ms

Bufferbloat  last 60 s: download 196 (16.4%), upload 203 (17.0%) of 1192 samples; 242 shaper changes
Daemon       RSS 2.1 MB, peak 2.1 MB, heap 228.0 KB; router CPU 25%
Warnings     0 seen; last -
```

Each value takes the largest unit in which it is at least 1.0: sizes from B to
GB (1 KB is 1,024 bytes), link rates from bit/s to Gbit/s with their byte rate
beside them, delays from µs to s. Times are local date and time, and
intervals whole seconds, minutes, hours and days (`1 min 9 s`, `3 h 4 min`).

- **Throughput** and the **achieved** and **shaper** columns come from the
  router's latest `LOAD` record, written with every traffic sample, or from
  `SUMMARY` when `LOAD` records are off.
- **Load**, **condition**, **avg delay** and **delayed** come from the latest
  `DATA` and `SUMMARY` records, and **TCP queue** from `TCP_QUEUE`. While the
  line is idle the daemon stops pinging, so these keep their last values.
- **Log capture** is the log data saved to the local file, not router traffic;
  **archived** counts the files renamed and compressed at the size limit
  during this run.
- **Bufferbloat** counts the `SUMMARY` records of the last 60 s of router time
  with a `_bb` condition, and the `SHAPER` records in the same minute.
- **Daemon** memory comes from `MEMORY` records and the router's CPU use from
  `CPU` records, shown only when `output_memory_stats` and `output_cpu_stats`
  are on.
- **Warnings** are counted over the last 3,000 lines of the file and
  everything after.

The screen reads the file's history once at start and then takes each line
from the capture itself, so it keeps working when the local file is on a
Windows drive (`/mnt/c`, `/mnt/d`), where following a growing file can fail.
It needs GNU awk (`gawk`); `-l` does not.

Known limitations, not yet fixed:

- **Start time after a reboot.** A router without a battery-backed clock boots
  with a restored time (saved at shutdown, or the newest file in `/etc`), and
  cake-adapt may start before NTP corrects it, so its start record carries the
  restored time. The status screen corrects **Router** for this, without
  changing the log: a run is on a restored clock when it starts earlier than
  the previous run's last record, or when its first forward jump crosses a
  capture disconnect (the router was down then, yet its records before the
  jump are stamped earlier). The start then moves by the jump and shows
  "corrected from" the logged time. The real time between the last record
  before the jump and the first after it is unknown, so the corrected start
  can be late by up to that gap; and a reboot the capture did not see, with a
  clock restored to just after the previous run, stays uncorrected. In one
  capture the start record said 13:09:34 and 10 s later the records jumped
  24.5 minutes forward; in another the start said 02:28:58, the capture had
  lost the router at 02:29:12, and the clock jumped 66 s, so the start shows
  as 02:30:04.
- **Warning times** show the record's `LOG_DATETIME`, the router's local time,
  while every other time on the screen is the PC's local time.
- **Repeated history.** After a reconnect, the capture skips the replayed log
  up to the last line it already has. If that line was cut off when the
  connection dropped, nothing matches and the whole router log is written
  again. One capture holds the same start record twice, 10,000 lines apart,
  with identical timestamps; the cut-off line is the likely cause, not yet
  confirmed.
