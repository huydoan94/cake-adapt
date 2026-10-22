# Developing cake-adapt

See the [technical overview](technical.md) for the design and the
[flowcharts](../flowchart/README.md) for the event flow.

## Agent workflows and resumption

Both Claude and Codex follow [AGENTS.md](../AGENTS.md) and
[CODING_STYLE.md](../CODING_STYLE.md), which extend the user's general working
definitions in `~/ai-working-definitions/` (outside this repository): roles,
model and delegation rules, efficiency and test-system discipline.
[COLLABORATION.md](../COLLABORATION.md) holds cake-adapt's additions. Read `HANDOFF.md`
for the active task, decisions, scoped approvals, evidence and unfinished work.
Keep it current so a resumed session does not need the user to repeat the chat.

Development, SDK validation, host-test headers, IntelliSense and VM validation
default to **x86**. Use `OPENWRT_SDK_X86` or `../openwrt-sdk-x86`. Filogic or
arm64 checks require an explicit target-specific request. The router's
architecture and another SDK's installed dependencies do not change the default.

Before running a build, check whether it will download dependencies, modify
feeds or SDK configuration, install tools, or copy artifacts. Obtain approval
for new downloads and environment changes; continue without asking again when
the exact scope is already authorized. Approved local validation dependencies,
tools and their caches belong in ignored `build/` unless another destination
was approved. A Filogic dependency approval does not authorize x86 SDK changes,
system installation or deployment. Record downloads and changes in the handoff.

## Building with an OpenWrt SDK

This repository is an OpenWrt package source tree. Clone it one level below an
OpenWrt 25.12 SDK's `package` directory and compile only this package.

The following is manual setup guidance. Agents working in an existing checkout
use its package link and selected SDK; cloning, SDK downloads and feed changes
are separate actions requiring authorization under the agent guide.

```sh
cd /path/to/openwrt-sdk
git clone https://github.com/huydoan94/cake-adapt.git package/cake-adapt
make package/cake-adapt/compile V=s
```

The APK is written below:

```text
bin/packages/<architecture>/base/
```

The LuCI app is a second package in `luci-app-cake-adapt/`. It uses the LuCI
feed's `luci.mk`, so the SDK needs the `luci` feed installed; link the
directory as `package/luci-app-cake-adapt` and compile it the same way. It has
no architecture (`all`). `tools/sdk-build.sh x86` (or `filogic`, or an SDK
directory) cleans and builds the three daemon variants, and `luci-app-cake-adapt`
too when that link exists, copies the packages to `bin/`, and says why a build
failed. `tools/host-test.sh` and `tools/sdk-build.sh` keep the make and
test output in their `build/` logs and print only their own messages, plus the
errors and last 30 lines of a failed log; `VERBOSE=1` shows the full output.

### Package variants

The root Makefile builds three variants of the daemon package, each with its
own build directory (`build_dir/.../cake-adapt-<variant>/`):

| Variant (`BUILD_VARIANT`) | Package | MQTT | Package dependencies |
| --- | --- | --- | --- |
| `base` (default variant) | `cake-adapt` | none (`MQTT_SUPPORTED=0`) | no libmosquitto, no c-ares |
| `mqtt` | `cake-adapt-mqtt` | plain (`MQTT_SUPPORTED=1`) | `+libmosquitto-nossl` |
| `mqtt-ssl` | `cake-adapt-mqtt-ssl` | plain or TLS (`MQTT_TLS_SUPPORTED=1`) | `+libmosquitto-ssl +ca-bundle` |

`make package/cake-adapt/compile` builds every selected variant; an SDK with
`CONFIG_ALL` selects all three. To build one, name it on the command line, for
example `make package/cake-adapt/compile BUILD_VARIANT=mqtt
CONFIG_PACKAGE_cake-adapt-mqtt=m NO_DEPS=1`. `package/cake-adapt/clean` removes
the packages of every variant from the SDK's `bin/`, so copy one out before
cleaning for the next. The helper builds the variants named by `variant=`
arguments, or all three in the order above (`variant=all` or no `variant=`). The
VS Code SDK build tasks ask which variant to build, `all` by default:

```sh
sh tools/sdk-build.sh x86                                  # all three, then LuCI
sh tools/sdk-build.sh variant=all x86                      # the same
sh tools/sdk-build.sh variant=mqtt x86                     # cake-adapt-mqtt only
sh tools/sdk-build.sh --prepare-only variant=mqtt-ssl x86  # its dependencies only
```

Each variant's dependencies are prepared just before it is built:
`base` needs no MQTT library, `mqtt` stages `libmosquitto-nossl` and c-ares, and
`mqtt-ssl` stages OpenSSL first, then `libmosquitto-ssl`. The two Mosquitto
variants share their staged headers and soname, so building `mqtt` after
`mqtt-ssl` (or the reverse) rebuilds the staged library, and the last MQTT
variant built leaves its library staged. The preflight also checks that each
variant's declared Mosquitto dependency (and `ca-bundle` for `mqtt-ssl`) is the
one it is built against. Logs are `build/sdk-<name>-<variant>.log` and
`build/sdk-<name>-luci.log`; copies keep the package name, so they are distinct,
for example `bin/cake-adapt-mqtt-ssl-0.3.26-r1-x86.apk`. The LuCI package is
built once: it reads the installed variant's `/usr/share/cake-adapt/features` at
run time and works with any of them.

Before invoking Make, the helper checks the SDK compiler and BPF tools, the
daemon's declared dependencies, staged headers and linker libraries, and the
tools actually used by the linked LuCI package. It reports the full preparation
plan before building any dependency. Missing target libraries are staged in
dependency order: UCI, libubox, ubus, libnl-tiny, libbpf, zlib and the selected
Mosquitto, with their build prerequisites such as libelf, c-ares, Lua and JSON-C.
Checks include required pkg-config metadata and shared-library ELF class,
byte order and machine matching the SDK's runtime. An existing stamp cannot hide
a missing header, broken library link or an SSL Mosquitto library overwriting
the shared soname. Unrepresented new package/build dependencies fail preflight
until their staging requirements are added to `tools/sdk-dependencies.sh`.

Command-line package selections avoid manual `.config` edits. All package builds
use `NO_DEPS=1`, so unrelated selected variants cannot trigger recursive
OpenSSL/kernel builds. Missing SDK tools or an unavailable local feed stop
preflight with a diagnostic before dependency builds start. The helper registers
packages from existing local feeds; it does not fetch or update feeds. Dependency
compilation can download source archives absent from the SDK's `dl/` cache.

To check and prepare dependency headers, libraries and metadata without building
the app or copying artifacts, run `sh tools/sdk-build.sh --prepare-only
variant=<variant> x86` (or `filogic` for that explicitly selected target). The usual VS Code SDK build tasks prepare them
automatically; no manual dependency selections in `.config` are needed.

SSL dependency builds disable optional OpenSSL engines and retain `NO_DEPS=1` to
keep kernel packages out of the build. `cake-adapt-mqtt-ssl` supports the
**MQTT TLS** and **MQTT CA certificate file** settings in LuCI's Advanced Logging
tab. TLS remains off until enabled there or through `mqtt_tls`; certificate and
hostname verification are mandatory. The other variants reject enabled TLS
configuration, and `cake-adapt` rejects enabled MQTT. The default CA file is
supplied by `ca-bundle`; an absolute path to a custom CA file can be configured
instead.

For agent validation without authorized artifact copying, run the focused
`package/cake-adapt/clean` and `package/cake-adapt/compile` Make targets, with
`BUILD_VARIANT` and the variant's `CONFIG_PACKAGE_` selection, in the
selected SDK after `--prepare-only` for that variant, and the equivalent LuCI targets when applicable. Package builds
may recursively download or rebuild dependencies; inspect the selected packages
and existing staging first. Use `NO_DEPS=1` only after the required dependencies
are built and staged, when it keeps validation within the approved scope. A
missing library must be prepared in that selected SDK under an applicable
approval or reported as a blocker. Do not switch SDKs to obtain a passing build.

`cake-adapt-mqtt` depends on `libmosquitto-nossl`; it does not bring in OpenSSL
or CA certificates. `cake-adapt-mqtt-ssl` depends on `libmosquitto-ssl`, which
requires OpenSSL, and on the runtime `ca-bundle`.
The earlier TLS prototype also used `libmosquitto-ssl`. An SDK
that selects optional OpenSSL AF_ALG or `/dev/crypto` engines can therefore pull kernel-module package
targets into an ordinary recursive package build. In the earlier Filogic run,
the AF_ALG dependency entered `package/kernel/linux/compile`, which repackaged
many selected modules; the log does not show a full kernel image compilation.
Inspect the SDK's `tmp/.packagedeps` and engine selection before running a
recursive build. Only `cake-adapt-mqtt-ssl` has that MQTT dependency chain;
header-only staging for editor use requires no kernel targets.
SDKs may still select both Mosquitto variants through `CONFIG_ALL`; our package
build helper checks the actual staged library, rather than trusting package
stamps, and repairs it before the app build when necessary. MQTT preparation
builds only the selected library variant and its required dependencies. Existing source
archives are reused; approval rules still apply to any new download or SDK modification.

GitHub Actions also discovers the OpenWrt 25.12 architecture list, runs the
host tests, builds the three daemon variants for each architecture (one
`package/cake-adapt/compile` with the SDK's default `CONFIG_ALL`), checks that
exactly one package of each variant was built, and publishes them, with the
LuCI package, as workflow artifacts. Publishing is all or nothing: one variant
failing on any architecture publishes no release. Tags beginning with `v` additionally publish the packages as GitHub
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

The database supplies include paths; it does not install headers. For a new
library such as libmosquitto, verify that its header (for example `mosquitto.h`)
exists under the selected x86 SDK's staged include directory. If it is missing,
prepare the dependency there only within an authorized scope, then regenerate
the database. Do not add Filogic paths to an x86 command or silently change the
generator's target. Report an unresolved include as unresolved.

## Source layout

```text
src/
├── main.c              CLI, configuration loading, logging setup, lifecycle
├── monitor/            uloop event loop; one struct monitor, a part per file:
│   ├── monitor.c         loop setup and teardown, traffic tick, activity
│   │                     state, CPU and log timers, signals
│   ├── status.c          pure status snapshot of the current state (no I/O)
│   ├── publish.c         ubus object cake-adapt.<section> answering status
│   │                     requests from the snapshot, reconnecting to ubusd
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
├── tcpdelay/           eBPF TCP filter and timestamp injector, their loaders,
│                       and the per-flow queue estimator
├── platform/           rtnetlink transport, traffic rates, CPU and memory sampling
└── logging/            syslog, records, rotation/export/reset and optional MQTT
files/
├── cake-adapt.init     procd service: runtime files, reload, restart, respawn
├── cake-adapt.import   builds /var/run/cake-adapt/<section>.uci from UCI
└── cake-adapt.config   the shipped UCI template
luci-app-cake-adapt/    Status and Configuration views, menu and rpcd ACL
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

`tools/host-test.sh` (the VS Code test task) runs `check`, `check-netlink`,
`check-config` and `check-tcpdelay`, taking the `uci.h`, libubox and libbpf
headers from the x86 SDK. `check` also runs the init script, importer and
LuCI tests (`tests/test_init.sh`, `test_import.sh`, `test_luci.sh`); the LuCI
test needs `node` and `python3`.

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
exposes the selected x86 SDK's staged `uci.h` and `libubox/` headers:

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

The ordinary logging tests use an MQTT stub, and `test_mqtt_none` checks the
`cake-adapt` package's no-op sender. `check-config` builds the configuration
tests once per variant (`test_config`, `test_config_mqtt`,
`test_config_mqtt_ssl`). The dedicated queue tests use `make -C tests check-mqtt`,
which runs them as built for `cake-adapt-mqtt` and for `cake-adapt-mqtt-ssl`, with
real libmosquitto headers and linkage, configured through `MQTT_CFLAGS` and
`MQTT_LIBS` when needed. Broker validation uses
`tests/logging/mqtt_integration.py` with an approved local library and broker
build. Commands, source identities and validation limits are in the
[initial MQTT evidence](../profiling/2026-10-11-mqtt-logging/README.md) and
[current optional TLS evidence](../profiling/2026-10-12-mqtt-ssl-build/README.md).
Build `mqtt_driver` with `MQTT_TLS_SUPPORTED=1` for the TLS driver tests and `--tls-only` for the TLS broker
cases; `--tls-stalled-only` checks the first socket deadline. Native broker
checks and an SDK build do not establish full-daemon behavior on the x86 VM.

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
The small follower the script runs on the router is final (it starts one
process a second; its cost is in `profiling/2026-10-09-follower`); changes
belong on the PC side.

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

## Watching flow offloading

`tools/offload-dashboard.sh` shows a router's software flow offloading over
SSH, redrawn in place: the flowtable's devices, offloaded connections by
protocol, the share of active TCP offloaded, conntrack table use, and each
CPU's utilization. Offloaded traffic bypasses CAKE, so this shows how much of
the line cake-adapt cannot see.

```sh
tools/offload-dashboard.sh root@router          # every 2 s, table every 10 s
tools/offload-dashboard.sh -i 5 -t 30 root@router eth1
```

The optional last argument is the WAN device; it defaults to the device of
the router's `wan` interface. Walking the conntrack table costs the router the most, so connection counts
refresh only every `-t` seconds (default 10). It needs `gawk` on the PC and
SSH key authentication, and reconnects after dropped connections and reboots.
