# cake-adapt

`cake-adapt` is an OpenWrt-native C daemon that adjusts the bandwidth of
existing CAKE qdiscs as link capacity changes. It observes traffic load and
latency, detects congestion, and moves the upload and download shaper rates
within configured limits.

The project is aimed primarily at variable-rate links such as cellular,
Starlink, and cable connections. It is currently pre-1.0 software: test the
configuration on your own connection before relying on it in production.

## Acknowledgement and relationship to cake-autorate

This project exists because of
[cake-autorate](https://github.com/lynxthecat/cake-autorate), created and
developed by lynxthecat and its contributors. Their project established and
refined the control strategy that cake-adapt follows.

In particular, decisions involving load classification, latency baselines,
delay EWMA, bufferbloat detection, rate increases and decreases, refractory
periods, reflector management, idle/stall handling, and diagnostic logging are
based on cake-autorate's design. Its source, documentation, testing, and
community experience are the reference when implementing or reviewing those
decisions here.

`cake-adapt` is a native C reimplementation for OpenWrt. It is not affiliated
with or endorsed by the cake-autorate maintainers. Any implementation bugs in
cake-adapt belong to this project. Intentional behavioral differences should
be documented and tested rather than silently attributed to upstream.

If cake-adapt is useful to you, please also visit and support the
[original cake-autorate project](https://github.com/lynxthecat/cake-autorate).

## Current scope

cake-adapt currently:

- loads typed configuration through `libuci`;
- discovers existing CAKE qdiscs through rtnetlink;
- uses the configured `interface` as upload and derives download as
  `ifb4<interface>`, unless both directional interface options override it;
- measures traffic rates independently for upload and download;
- runs one `fping` process against multiple reflectors;
- tracks latency baselines and reflector health;
- detects high load, congestion, idle periods, and stalled connectivity;
- adjusts upload and download CAKE bandwidth independently when enabled;
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

## Data flow

```text
traffic counters     fping reflectors     current CAKE state
       |                     |                     |
       +---------------------+---------------------+
                             |
                             v
                  load and latency tracking
                             |
                             v
                 cake-autorate-derived controller
                             |
                             v
                    desired shaper rates
                             |
                             v
                     rtnetlink CAKE update
```

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

## Requirements

- OpenWrt with existing upload and ingress CAKE qdiscs
- `fping`
- `libuci`
- `libubox`
- `libnl-tiny`
- `zlib`

The OpenWrt package declares these runtime dependencies.

## Installation

Download the APK matching the router architecture from the project's GitHub
release artifacts, copy it to the router, and install it locally:

```sh
scp cake-adapt-*.apk root@router:/tmp/cake-adapt.apk
ssh root@router
apk add --allow-untrusted /tmp/cake-adapt.apk
```

The package installs:

```text
/usr/sbin/cake-adapt
/etc/init.d/cake-adapt
/etc/config/cake-adapt
/usr/libexec/cake-adapt/import
```

## Configuration

The packaged configuration is
[`files/cake-adapt.config`](files/cake-adapt.config). It contains the required
rate ranges and reflector list, plus commented advanced options with their
defaults.

Start in observation-only mode:

```uci
config cake_adapt 'main'
        option enabled '1'
        option interface 'eth1'

        option adjust_dl_shaper_rate '0'
        option adjust_ul_shaper_rate '0'

        option min_dl_shaper_rate_kbps '5000'
        option base_dl_shaper_rate_kbps '20000'
        option max_dl_shaper_rate_kbps '80000'

        option min_ul_shaper_rate_kbps '5000'
        option base_ul_shaper_rate_kbps '20000'
        option max_ul_shaper_rate_kbps '35000'

        list reflectors '1.1.1.1'
        list reflectors '1.0.0.1'
        list reflectors '8.8.8.8'
        list reflectors '8.8.4.4'
        list reflectors '9.9.9.9'
        list reflectors '9.9.9.10'
```

The shipped file contains a larger reflector pool. The number of active
reflectors defaults to six, so keep at least six list entries unless
`no_pingers` is reduced.

For each direction, rates must satisfy:

```text
minimum <= base <= maximum
```

Choose these values for the actual connection. The
[cake-autorate theory of operation](https://github.com/lynxthecat/cake-autorate#theory-of-operation)
is the primary reference for selecting minimum, base, and maximum rates:

- minimum is the lowest reliably bufferbloat-free capacity;
- base is the low-load steady-state rate; and
- maximum is the highest rate the controller may explore.

When the sleep function is enabled, `connection_active_thr_kbps` must not
exceed either minimum shaper rate. Invalid interface pairs, reflector counts,
rate ranges, or timing values prevent that instance from starting; the error is
written to syslog.

After validating observation and logging, enable adjustment for either or both
directions:

```sh
uci set cake-adapt.main.adjust_dl_shaper_rate='1'
uci set cake-adapt.main.adjust_ul_shaper_rate='1'
uci commit cake-adapt
/etc/init.d/cake-adapt restart
```

Enable one direction at a time for the first live test. Generate sustained
traffic through that direction and confirm both the structured `SHAPER` records
and the effective rate reported by `tc qdisc show`. The achieved traffic rate
and configured CAKE rate are different measurements; controller decisions use
traffic load together with latency and congestion state.

### Multiple instances

Each named `config cake_adapt` section is an independent `procd` instance. This
allows one service to control separate CAKE pairs, for example two WAN links:

```uci
config cake_adapt 'primary'
        option enabled '1'
        option config_file '/etc/cake-adapt/config.primary.sh'

config cake_adapt 'secondary'
        option enabled '1'
        option config_file '/etc/cake-adapt/config.secondary.sh'
```

The imported files supply the controller, interface, rate, reflector, and
logging settings, keeping the UCI sections small. Every enabled instance must
use its own upload and download CAKE qdiscs; do not configure two instances to
control the same interfaces.

Runtime UCI and logs are isolated by section name:

```text
/tmp/cake-adapt-config/primary/cake-adapt
/tmp/cake-adapt-config/secondary/cake-adapt
/var/log/cake-adapt.primary.log
/var/log/cake-adapt.secondary.log
```

The historical `main` section remains compatible with the unsuffixed
`/var/log/cake-adapt.log` filename.

### Standalone shell configuration

A standalone cake-adapt shell configuration can supply any recognized settings
without duplicating them in UCI:

```uci
config cake_adapt 'main'
        option enabled '1'
        option config_file '/etc/cake-adapt/config.main.sh'
```

For example, `/etc/cake-adapt/config.main.sh` can contain:

```bash
ul_if='eth1'
dl_if='ifb4eth1'

adjust_dl_shaper_rate=1
min_dl_shaper_rate_kbps=5000
base_dl_shaper_rate_kbps=20000
max_dl_shaper_rate_kbps=80000

adjust_ul_shaper_rate=1
min_ul_shaper_rate_kbps=5000
base_ul_shaper_rate_kbps=20000
max_ul_shaper_rate_kbps=35000

reflectors=(
    1.1.1.1
    1.0.0.1
    8.8.8.8
    8.8.4.4
    9.9.9.9
    9.9.9.10
)
```

The file only needs to contain values that should override UCI or the daemon's
built-in defaults. A reflector list must still be supplied by either UCI or the
standalone file.

Configuration precedence is:

```text
cake-adapt built-in defaults < UCI values < standalone config-file values
```

At service start, the init script creates one temporary UCI directory below
`/tmp/cake-adapt-config/<section>/` for each enabled instance. When a
`config_file` is selected, an isolated Bash helper loads that file and writes
its supported values into the corresponding section of the temporary UCI
package. Each daemon reads its named section through `libuci`; it never parses
shell syntax and never loads files from a cake-autorate installation.

The helper checks both shell files with `bash -n`, applies only option names
advertised by the installed cake-adapt binary, and writes scalar and reflector
values through the `uci` command. The result is validated by cake-adapt before
it is atomically published with mode `0600`. Invalid shell syntax, types,
ranges, interface pairing, or reflector data prevent the service from starting.

These configuration files are executable Bash, not passive data. Only select a
trusted root-owned file. The helper runs in a separate Bash process so its
variables and shell settings cannot modify the OpenWrt init shell.

Configured `ul_if` and `dl_if` values take effect as a pair and may name any two
valid Linux interfaces. When both are absent, cake-adapt retains the UCI
`interface` value and its derived `ifb4<interface>` download interface.

`enabled` and `config_file` remain UCI-owned. Intentional exclusions
such as `startup_wait_s`, remote reflector retrieval, and unsupported pinger
methods do not become supported merely by importing a file. A selected file
must be readable and all recognized values must pass the same typed validation
as UCI values. `procd` watches UCI and each selected configuration file, then
regenerates the effective file when either changes. The generated file is
temporary and must not be edited; change UCI or the standalone source file
instead.

To inspect the exact values used by an instance:

```sh
cat /tmp/cake-adapt-config/primary/cake-adapt
```

Do not leave cake-autorate running while cake-adapt rate adjustment is enabled;
the two programs must not control the same CAKE qdiscs concurrently.

Enable the service at boot with:

```sh
/etc/init.d/cake-adapt enable
```

## Operation and logging

Check service state and important operational messages:

```sh
/etc/init.d/cake-adapt status
logread -e cake-adapt
```

Inspect the live CAKE rates:

```sh
tc qdisc show dev eth1
tc qdisc show dev ifb4eth1
```

The historical `main` instance logs to:

```text
/var/log/cake-adapt.log
```

Other instances include their UCI section name:

```text
/var/log/cake-adapt.primary.log
/var/log/cake-adapt.secondary.log
```

The structured record names, field order, and units intentionally preserve
compatibility with cake-autorate analysis workflows. The local filenames use
the cake-adapt name. `log_file_path_override` changes only the containing
directory, not the filename.

Useful logging options are documented in the packaged UCI file. High-frequency
processing, load, reflector, summary, and CPU records are optional because
they increase CPU and storage use.

When file logging is enabled:

```sh
# Find the PID associated with each `-S <section>` argument.
pgrep -af cake-adapt

# Export a timestamped snapshot, compressed by default.
kill -USR1 INSTANCE_PID

# Reset the live log in place.
kill -USR2 INSTANCE_PID
```

Automatic rotation retains one `.old` file and truncates the live log in place
so an existing `tail -f` remains attached.

The daemon subscribes to CAKE qdisc lifecycle events. If a controlled qdisc is
removed, that direction is suspended and reported as degraded. Monitoring and
control resume when a matching CAKE qdisc reappears; the daemon does not need a
fixed startup delay.

For a first end-to-end check:

1. verify both CAKE qdiscs and record their starting bandwidths;
2. run cake-adapt in observation-only mode and confirm the `fping` child plus
   `LOAD`, `DATA`, and `REFLECTOR` records;
3. enable one direction and run a bounded sustained transfer through the
   corresponding interface;
4. confirm every applied rate stays within its configured minimum and maximum;
5. stop the transfer and confirm recovery toward the base rate; and
6. stop the service and confirm no cake-adapt or `fping` process remains.

## Building with an OpenWrt SDK

This repository is an OpenWrt package source tree. Clone it one level below an
OpenWrt 25.12 SDK's `package` directory and compile only this package:

```sh
cd /path/to/openwrt-sdk
git clone https://github.com/huydoan94/sqm-mon.git package/cake-adapt
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

## Host tests

The controller and most measurement logic can be tested without rebuilding an
OpenWrt SDK:

```sh
make -C tests check check-netlink
```

The host needs a C compiler, `zlib`, and development headers for libnl 3. The
optional configuration test additionally needs native `libuci` and `libubox`
development headers:

```sh
make -C tests check-config
```

The production daemon itself is built with strict warnings, including
`-Wall`, `-Wextra`, `-Wpedantic`, `-Wformat=2`, `-Wshadow`, `-Wconversion`, and
`-Werror`.

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

## License

cake-adapt is licensed under the GNU General Public License version 2 only.
See [`LICENSE`](LICENSE).
