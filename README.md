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
  does. cake-adapt adjusts these qdiscs; it does not create them. A root
  `cake_mq` (multi-queue CAKE) is adjusted like a single CAKE; see the
  [configuration reference](docs/configuration.md#basic) for how it shares
  the rate.
- Hardware flow offloading off: offloaded traffic bypasses CAKE.

The package pulls in its dependencies. cake-adapt pings reflectors itself, so
no ping program is needed; `fping` or `irtt` are needed only to use them as a
fallback (`pinger_method`).

Do not run cake-adapt and cake-autorate (or anything else that changes the same
CAKE qdiscs) at the same time.

## Installation

Each release has three variants of the daemon for every architecture; install
one of them:

| Package | MQTT log forwarding | Extra dependencies |
| --- | --- | --- |
| `cake-adapt` | no | none |
| `cake-adapt-mqtt` | yes, plain MQTT | `libmosquitto-nossl` |
| `cake-adapt-mqtt-ssl` | yes, plain or TLS | `libmosquitto-ssl`, OpenSSL, `ca-bundle` |

Pick `cake-adapt` unless you want its logs sent to an MQTT broker. Download the
APK of that variant for your router's architecture from the
[GitHub releases](https://github.com/huydoan94/cake-adapt/releases), copy it
to the router, and install it, for example:

```sh
scp x86_64_cake-adapt-0.3.30-r1.apk root@router:/tmp/cake-adapt.apk
ssh root@router
apk add --allow-untrusted /tmp/cake-adapt.apk
```

The variants install the same files and each provides `cake-adapt`, so only
one can be installed. To switch to another variant from a downloaded file:

```sh
apk del cake-adapt && apk add --allow-untrusted ./<variant>.apk
```

Use the name of the installed variant in `apk del` (for example
`cake-adapt-mqtt`). The two steps are needed because a package installed from a
local file is pinned to that file; a variant installed from a package feed can be
switched with one `apk add`. Without `luci-app-cake-adapt` installed, the
service is down between the two commands. A modified `/etc/config/cake-adapt` is
kept, with the new default beside it as `/etc/config/cake-adapt.apk-new`.

Installing a variant installs the daemon, its service `/etc/init.d/cake-adapt` and the
configurations `/etc/config/cake-adapt` and `/etc/cake-adapt/exec` (root
only).

When upgrading from 0.3.19 or earlier:

- cake-adapt now pings reflectors itself (`pinger_method` `native`), and
  fping is no longer installed with it; apk may remove it in the upgrade. An
  instance that sets `pinger_method` `fping` or `fping-ts` needs it: run
  `apk add fping` too. An instance that left `pinger_method` unset now uses
  `native`, which takes numeric reflector addresses only.
- `config_file`, `ping_prefix_string`, `ping_extra_args` and
  `log_file_path_override` are refused in
  `/etc/config/cake-adapt`; move them to `/etc/cake-adapt/exec` (see
  [Options only root sets](docs/configuration.md#options-only-root-sets)). The web interface is a separate
package, `luci-app-cake-adapt`, installed the same way (see [LuCI](#luci)).

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
- **Reflectors** are hosts cake-adapt pings to measure delay, as numeric IP
  addresses (the default pinger takes no hostnames). Six are used at a time,
  so keep at least six.

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

With several instances, `start`, `stop` and `restart` also take a section
name, for example `/etc/init.d/cake-adapt restart main`, and then act on that
instance only. A configuration change is applied with
`/etc/init.d/cake-adapt reload`, which restarts only the instances whose
configuration changed. `restart` restarts every requested instance even when
nothing changed. With either, a configuration the daemon refuses leaves the
instance running its previous configuration, and the reason is in the system
log.

Check it:

```sh
/etc/init.d/cake-adapt status
logread -e cake-adapt              # start, stop, ubus, warnings and errors
tail -f /var/log/cake-adapt.log    # the detailed log
tc qdisc show dev wan              # the upload CAKE rate it set
```

Logs live in `/var/log`, which on OpenWrt is RAM, so nothing is written to
flash by default. If a CAKE qdisc disappears, for example while SQM restarts,
cake-adapt pauses that direction and resumes when it returns.

The current development work also adds optional MQTT log forwarding, disabled
by default and configurable in LuCI. It sends logs asynchronously, drops them
silently while offline or when its bounded queue cannot accept them, and retries
connections every 10 seconds indefinitely. See the
[MQTT configuration](docs/configuration.md#output-and-logging) and
[validation limits](profiling/2026-10-12-mqtt-ssl-build/README.md). The non-SSL
packages passed x86 builds and a bounded observation-only daemon check on the
x86 VM; production resource usage remains unmeasured.

MQTT needs the `cake-adapt-mqtt` or `cake-adapt-mqtt-ssl` package; with
`mqtt_enabled` on, `cake-adapt` runs as usual, sends nothing and logs a warning,
and LuCI hides its MQTT settings. Only
`cake-adapt-mqtt-ssl` adds TLS settings in LuCI, with broker certificate and
hostname verification. TLS is opt-in; plain connections send log data and
optional username/password credentials unencrypted.

## LuCI

The `luci-app-cake-adapt` package adds a LuCI interface. Install it next to
cake-adapt (it depends on `luci-base`, `luci-mod-status` and `cake-adapt`),
then open **Services → CAKE Adapt**. The daemon publishes its state on ubus as
`cake-adapt.<section>`, which the Status page reads every 2 seconds.

**Status** has one block per instance. Its state, in the theme's colours, is
taken from procd's process state (running and its pid, refreshed on every
poll) and not from the daemon's ubus answers: Running (green), Running with
its live status unavailable (amber) when the process runs but its status does
not answer, Stopped (red), Disabled (grey) for an instance that does not
run and is disabled in the configuration, and State unavailable (grey) when
procd does not answer. A label beside it shows the
daemon's activity (running, idle or stall), and the line below the pid, start
time and version. Each instance has its own Start, Restart and Stop buttons,
which run `/etc/init.d/cake-adapt start|restart|stop <section>`: Start is
greyed out while the instance runs or while it is disabled in the
configuration, Stop while it does not run, and all three for a user without
write access. Restart keeps an instance whose new configuration is refused
running its previous one; the page then says so (the pid did not change), as
it says when a Start or Restart left the instance not running, and points to
the system log (`logread -e cake-adapt`). Whether the service starts at boot is left to
System → Startup. A table has one row per direction with these columns: the shaper rate cake-adapt has set on CAKE,
the rate actually achieved, the load (achieved over shaper rate), the added
delay (current latency above its baseline), whether bufferbloat is detected,
and the TCP queue estimate (blank unless `tcp_delay_attribution` is on). Three
charts show the last five minutes as lines, the newest sample at the right
edge: one for download and one for upload, each with the shaper rate (blue,
solid) and the achieved rate (green, dashed), and one for the added delay,
with download (blue, solid) and upload (green, dashed).

**Configuration** edits `/etc/config/cake-adapt` in tabs: General (enable,
interface or the `ul_if`/`dl_if` pair, rate adjustment), Shaper rates
(minimum, base and maximum for each direction), Reflectors, and Advanced,
whose own tabs group the other options: Pinger, Logging, TCP measurement,
Idle and stall, Controller, Reflector health and Timing. The options that make
root run something, choose its arguments or choose where it writes
(`config_file`, `ping_prefix_string`, `ping_extra_args` and
`log_file_path_override`) are not on the page: only root sets them, in
`/etc/cake-adapt/exec`. **Save & Apply** commits the change and
reloads the service, which restarts only the instances whose configuration
changed. Before saving, the page checks every option in the
browser, against the values shown on screen and not the saved ones, with the
same rules as the daemon (an empty field counts as the daemon's default, the
placeholder): a field the rules name turns red and says why, a change in one
field re-checks the others that depend on it, and Save and Save & Apply refuse
while any field is invalid. A configuration the daemon still refuses does not
replace the running one: the instance keeps running its previous
configuration, and the reason is in the system log (`logread -e cake-adapt`).
An instance that was not running is not started until the configuration is
fixed.

## Documentation

- [Configuration reference](docs/configuration.md): every option, with
  defaults.
- [Technical overview](docs/technical.md): what the daemon does, how it
  differs from cake-autorate, and the test and profiling evidence, with links
  to every design document and dashboard.
- [Development](docs/development.md): building with the OpenWrt SDK, source
  layout and tests; [testbed](tools/testbed/README.md) and
  [simulator](tools/simulator/README.md).
- [Flowcharts](flowchart/README.md): the event flow and controller decisions.
  [Open the rendered viewer](https://raw.githack.com/huydoan94/cake-adapt/main/flowchart/index.html).
- [Profiling and behaviour evidence](profiling/README.md): every measured run.
  [Open the evidence index](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/index.html) or the latest profile's
  [dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-10-22-log-writer-wake/index.html).
- [Agent guide](AGENTS.md) and its topic rules:
  [project status](docs/agents/status.md), [modules](docs/agents/modules.md),
  [upstream behaviour](docs/agents/upstream.md),
  [build and tests](docs/agents/build-and-tests.md),
  [C style](docs/agents/c-style.md) and [VM testing](docs/agents/vm-testing.md);
  the [coding style](CODING_STYLE.md) and the
  [collaboration workflows](COLLABORATION.md) for Claude and Codex.
  Development defaults to x86.

## License

cake-adapt is licensed under the GNU General Public License version 2 only.
See [`LICENSE`](LICENSE).
