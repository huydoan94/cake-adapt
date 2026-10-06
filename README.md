# cake-adapt

cake-adapt is an OpenWrt daemon that keeps a line responsive under load by
adjusting the bandwidth of its CAKE shaper as the line's capacity changes. It
watches traffic and latency, and when delay starts to build up, it lowers the
shaper rate. When the line is busy and stays clear, it raises the rate again,
within limits you set.

It suits lines whose capacity varies, such as cellular, Starlink, cable and
busy DSL or fibre. cake-adapt is pre-1.0 software: try it on your own line
before relying on it.

## Thanks to cake-autorate

cake-adapt is a native C port of
[cake-autorate](https://github.com/lynxthecat/cake-autorate), created by
lynxthecat and developed with its contributors and community. Their work
designed and refined the control strategy cake-adapt follows:
- load and bufferbloat detection;
- latency baselines;
- rate changes;
- reflector handling;
- idle and stall behavior;
- logging.

Its documentation is the best explanation of how the method works.

cake-adapt is not affiliated with or endorsed by the cake-autorate
maintainers, and its bugs are its own. If cake-adapt is useful to you, please
also visit and support
[the original project](https://github.com/lynxthecat/cake-autorate).

## Requirements

- OpenWrt 25.12.
- An SQM setup that creates the CAKE qdiscs: one on the WAN interface for
  upload, and one on its `ifb4<interface>` for download, as `sqm-scripts`
  does. cake-adapt adjusts these qdiscs; it does not create them.
- Hardware flow offloading off: offloaded traffic bypasses CAKE.

The package pulls in its dependencies, including `fping`.

Do not run cake-adapt and cake-autorate (or anything else that changes the same
CAKE qdiscs) at the same time.

## Installation

Download the APK for your router's architecture from the
[GitHub releases](https://github.com/huydoan94/cake-adapt/releases), copy it
to the router, and install it:

```sh
scp cake-adapt-*.apk root@router:/tmp/cake-adapt.apk
ssh root@router
apk add --allow-untrusted /tmp/cake-adapt.apk
```

This installs the daemon, its service `/etc/init.d/cake-adapt` and the
configuration `/etc/config/cake-adapt`.

## Configuration

Edit `/etc/config/cake-adapt`. The packaged file lists the common options:

```uci
config cake_adapt 'main'
	option enabled '1'
	option interface 'wan'

	option adjust_dl_shaper_rate '0'
	option adjust_ul_shaper_rate '0'

	option min_dl_shaper_rate_kbps '5000'
	option base_dl_shaper_rate_kbps '20000'
	option max_dl_shaper_rate_kbps '80000'
	option min_ul_shaper_rate_kbps '5000'
	option base_ul_shaper_rate_kbps '20000'
	option max_ul_shaper_rate_kbps '35000'

	list reflectors '1.1.1.1'
	list reflectors '8.8.8.8'
	...
```

- **`interface`** is the Linux device with SQM's upload CAKE, as shown by
  `tc qdisc show`. Download uses `ifb4<interface>`. For another layout, set both `ul_if` and `dl_if`
  instead.
- **Rates** are in kbit/s, and each direction needs
  `minimum <= base <= maximum`:
  - minimum: the lowest rate the line delivers without bufferbloat;
  - base: the rate for a lightly loaded line;
  - maximum: the highest rate cake-adapt may try.
- **Reflectors** are hosts cake-adapt pings to measure delay. Six are used at
  a time, so keep at least six.

Rate adjustment is off in each direction until you turn it on. Start by
observing only, check the log, then enable one direction at a time:

```sh
uci set cake-adapt.main.adjust_dl_shaper_rate='1'
uci set cake-adapt.main.adjust_ul_shaper_rate='1'
uci commit cake-adapt
/etc/init.d/cake-adapt restart
```

The **[configuration reference](docs/configuration.md)** documents every
option and its default, along with multiple WAN links and importing a
cake-autorate configuration file.

## Running

Enable the service at boot and start it:

```sh
/etc/init.d/cake-adapt enable
/etc/init.d/cake-adapt start
```

Check it:

```sh
/etc/init.d/cake-adapt status
logread -e cake-adapt              # start, stop, warnings and errors
tail -f /var/log/cake-adapt.log    # the detailed log
tc qdisc show dev wan              # the upload CAKE rate it set
```

Logs live in `/var/log`, which on OpenWrt is RAM, so nothing is written to
flash by default. If a CAKE qdisc disappears, for example while SQM restarts,
cake-adapt pauses that direction and resumes when it returns.

## Documentation

- [Configuration reference](docs/configuration.md): every option, with
  defaults.
- [Technical overview](docs/technical.md): what the daemon does, how it
  differs from cake-autorate, and the test and profiling evidence.
- [Development](docs/development.md): building with the OpenWrt SDK, source
  layout and tests.
- [Flowcharts](flowchart/README.md): the event flow and controller decisions.

## License

cake-adapt is licensed under the GNU General Public License version 2 only.
See [`LICENSE`](LICENSE).
