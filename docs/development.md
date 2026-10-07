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
after dropped connections or router reboots without losing or repeating lines,
and archives the local file with gzip at a size limit (1024 MB by default):

```sh
tools/capture-log.sh root@router cake-adapt.log            # status screen
tools/capture-log.sh -s 512 root@router cake-adapt.log     # archive at 512 MB
tools/capture-log.sh -l root@router cake-adapt.log         # print lines instead
```

The router needs SSH key authentication; nothing is installed on it. Capture
events (connect, disconnect, archive, stop) go to `cake-adapt.log.events`.

By default the console shows a status screen that is redrawn in place every
second:

```text
Connection   connected since 2026-10-07 11:29:40 (40 s), reconnects 0
Capture      0.3 MB this run, 0.01 MB/s, file 1.1 MB of 1024.0 MB, 0 archived
Router       cake-adapt 0.3.8-r1 PID 2653, started 2026-10-07 11:27:26 (174 s ago)
Last record  2026-10-07 11:30:20 (0 s ago)

                 achieved          shaper   load condition    avg delay  delayed  TCP queue
Download     12.34 Mbit/s    70.00 Mbit/s    18% dl_low          2.6 ms      0/6     0.8 ms
Upload        0.81 Mbit/s    14.00 Mbit/s     6% ul_idle         2.6 ms      0/6     2.0 ms

Bufferbloat  last 60 s: download 0 (0.0%), upload 0 (0.0%) of 845 samples; 3 shaper changes
Daemon       RSS 2.1 MB, peak 2.1 MB, heap 0.2 MB; CPU 1%
Warnings     0 seen; last -
```

Sizes are in MB (1,048,576 bytes) and the capture's throughput in MB/s; link
rates are in Mbit/s, as the shaper settings are; times are local date and
time, and intervals whole seconds. Rates, loads, conditions and delays come
from the router's latest `SUMMARY`, `DATA` and `TCP_QUEUE` records, memory and
CPU from `MEMORY` and `CPU` records (shown only when the router's
`output_memory_stats` and `output_cpu_stats` are on), and the bufferbloat count
from the `SUMMARY` records of the last 60 s of router time. Warnings are
counted over the last 3,000 lines of the file and everything after. The screen
needs GNU awk (`gawk`); `-l` does not.
