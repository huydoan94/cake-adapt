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
- treats the configured interface as upload and derives download as
  `ifb4<interface>`;
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
to fit Linux's interface-name limit. Separate upload and download interface
options are intentionally not required.

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

After validating observation and logging, enable adjustment for either or both
directions:

```sh
uci set cake-adapt.main.adjust_dl_shaper_rate='1'
uci set cake-adapt.main.adjust_ul_shaper_rate='1'
uci commit cake-adapt
/etc/init.d/cake-adapt restart
```

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

File logging defaults to:

```text
/var/log/cake-autorate.log
```

This filename and the structured record formats intentionally preserve the
connection to cake-autorate's analysis workflow. `log_file_path_override`
changes the containing directory, not the filename.

Useful logging options are documented in the packaged UCI file. High-frequency
processing, load, reflector, summary, and CPU records are optional because
they increase CPU and storage use.

When file logging is enabled:

```sh
# Export a timestamped snapshot, compressed by default.
kill -USR1 "$(pidof cake-adapt)"

# Reset the live log in place.
kill -USR2 "$(pidof cake-adapt)"
```

Automatic rotation retains one `.old` file and truncates the live log in place
so an existing `tail -f` remains attached.

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
optional configuration test additionally needs UCI headers:

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
