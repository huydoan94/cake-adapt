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
(`tests/controller/fixtures/`) through the controller and fails on any
decision that differs from upstream.

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
