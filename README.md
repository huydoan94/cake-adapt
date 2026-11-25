# cake-adapt

cake-adapt keeps your internet connection responsive when it is busy. It runs
on an OpenWrt router and adjusts the speed of SQM's CAKE shaper as your line's
real capacity changes: it slows the shaper down when delay starts to build up,
and speeds it back up when the line is clear, within limits you set.

It helps most on lines whose speed varies: cellular, Starlink, cable, and busy
DSL or fibre.

## Thanks to cake-autorate

cake-adapt is a native C port of
[cake-autorate](https://github.com/lynxthecat/cake-autorate), created by
lynxthecat and developed with its community. Their work designed the method
cake-adapt follows, and its documentation is the best explanation of how it
works. cake-adapt is not affiliated with the cake-autorate maintainers, and its
bugs are its own. If cake-adapt is useful to you, please support
[the original project](https://github.com/lynxthecat/cake-autorate) too.

## What you need

- OpenWrt 25.12.
- SQM with CAKE already set up on your WAN (for example with `sqm-scripts`).
  cake-adapt adjusts those shapers; it does not create them.
- Hardware flow offloading turned off, since offloaded traffic skips CAKE.

Don't run cake-adapt together with cake-autorate, or anything else that changes
the same shapers.

## Install

Download the package for your router from the
[releases page](https://github.com/huydoan94/cake-adapt/releases) and install
it:

```sh
scp x86_64_cake-adapt-1.0.5-r1.apk root@router:/tmp/cake-adapt.apk
ssh root@router
apk add --allow-untrusted /tmp/cake-adapt.apk
```

Pick `cake-adapt` unless you want its logs sent to an MQTT broker; then use
`cake-adapt-mqtt`, or `cake-adapt-mqtt-ssl` for TLS. For the web interface,
also install `luci-app-cake-adapt` the same way.

## Set up

Edit `/etc/config/cake-adapt`:

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

- **interface**: the device SQM shapes for upload (see `tc qdisc show`).
- **Rates** in kbit/s, per direction:
  - minimum: the lowest speed your line still handles without lag;
  - base: the speed for a quiet line;
  - maximum: the highest speed cake-adapt may try.
- **reflectors**: addresses cake-adapt pings to measure delay. Keep at least
  six, as numeric IP addresses.

Rate changes start switched off, so you can watch first. When the log looks
right, turn them on:

```sh
uci set cake-adapt.main.adjust_dl_shaper_rate='1'
uci set cake-adapt.main.adjust_ul_shaper_rate='1'
uci commit cake-adapt
/etc/init.d/cake-adapt restart
```

Every option is in the [configuration reference](docs/configuration.md).

## Run and check

```sh
/etc/init.d/cake-adapt enable
/etc/init.d/cake-adapt start

logread -e cake-adapt             # start, stop, warnings and errors
tail -f /var/log/cake-adapt.log   # what it measures and decides
```

After a configuration change, run `/etc/init.d/cake-adapt reload`. If the new
configuration is refused, cake-adapt keeps the old one and logs why. With
several instances, add the section name: `/etc/init.d/cake-adapt restart main`.
`cake-adapt -v` prints the installed version.

Logs stay in RAM (`/var/log`), so nothing is written to flash.

## Usage

### Web interface

`luci-app-cake-adapt` adds **Services → CAKE Adapt**:

- **Status**: each instance's state, Start/Restart/Stop buttons, the current
  rates, load and added delay, and five-minute charts.
- **Configuration**: every setting in tabs, checked before saving. **Save &
  Apply** reloads the service.

### Package variants

| Package | MQTT log forwarding |
| --- | --- |
| `cake-adapt` | no |
| `cake-adapt-mqtt` | yes |
| `cake-adapt-mqtt-ssl` | yes, with TLS |

Only one can be installed. To switch:

```sh
apk del cake-adapt && apk add --allow-untrusted ./<variant>.apk
```

(use the installed variant's name in `apk del`). Your configuration is kept.

### MQTT

With an MQTT variant, set the broker in LuCI or in the
[MQTT options](docs/configuration.md#output-and-logging). Logs are dropped
while the broker is unreachable, and it reconnects every 10 seconds. Without
TLS, logs and passwords are sent unencrypted.

### Upgrading from 0.3.19 or earlier

- cake-adapt now pings by itself and no longer installs `fping`. If you set
  `pinger_method` to `fping` or `fping-ts`, run `apk add fping`.
- `config_file`, `ping_prefix_string`, `ping_extra_args` and
  `log_file_path_override` now belong in `/etc/cake-adapt/exec`
  ([details](docs/configuration.md#options-only-root-sets)).

## More

- [Configuration reference](docs/configuration.md): every option and its
  default.
- [How it works](docs/technical.md), and how it differs from cake-autorate.
- [Development](docs/development.md): building and testing.
- [Flowcharts](https://raw.githack.com/huydoan94/cake-adapt/main/flowchart/index.html)
  and [test evidence](profiling/README.md).

## License

GNU General Public License version 2 only. See [`LICENSE`](LICENSE).
