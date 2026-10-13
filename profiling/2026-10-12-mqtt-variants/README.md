# Three cake-adapt package variants (2026-10-12)

User decision (2026-10-12): the root Makefile builds `cake-adapt` (no MQTT, no
libmosquitto), `cake-adapt-mqtt` (`libmosquitto-nossl`) and
`cake-adapt-mqtt-ssl` (`libmosquitto-ssl`, `ca-bundle`). Each provides
`cake-adapt`; LuCI reads `/usr/share/cake-adapt/features` at run time.

## x86 SDK packages (0.3.17-r1, `NO_DEPS=1`, one variant per build)

Built with `tools/sdk-build.sh --prepare-only variant=<v> x86` then
`make package/cake-adapt/compile BUILD_VARIANT=<v> CONFIG_PACKAGE_<package>=m NO_DEPS=1`;
not copied to the repository's `bin/`.

| APK | sha256 | Declared depends (besides libc, libuci, libubox, libubus, libnl-tiny, libbpf, zlib, fping, bash) | Binary NEEDED (MQTT) | features file |
| --- | --- | --- | --- | --- |
| `cake-adapt-0.3.17-r1.apk` | `abf897accc40aaf13f77f9cd76bbc3ff8eef4489c81519fd4fb1c7827462e1e2` | none | no libmosquitto | empty |
| `cake-adapt-mqtt-0.3.17-r1.apk` | `00f03e11773c30338d8e6c21e578766ddf73e5ec7debc25271a46416842e9a3a` | `libmosquitto-nossl` | `libmosquitto.so.1` | `mqtt` |
| `cake-adapt-mqtt-ssl-0.3.17-r1.apk` | `48a31e13ec12a18706f5d059f7416e57be6e217587b5682356bf9f0e769f8d0a` | `libmosquitto-ssl`, `ca-bundle` | `libmosquitto.so.1` | `mqtt`, `tls` |
| `luci-app-cake-adapt-0.3.17-r1.apk` | `13d8bcf6b5b21313692c8806c0d3b3081b90615d79bfbc3297d7a5cdbba40ea8` | `cake-adapt`, `luci-base`, `luci-mod-status` | | |

Both Mosquitto variants share the soname `libmosquitto.so.1`, so the binary's
NEEDED entry cannot tell them apart; the package dependency selects the library.
Only the SSL binary references `mosquitto_tls_set`; the base binary has no
`mosquitto_` symbol. `cake-adapt-mqtt` and `cake-adapt-mqtt-ssl` provide
`cake-adapt=0.3.17-r1`.

## x86 VM (192.168.56.2), raw output in `vm/`

Before: `cake-adapt` 0.3.16-r1 and `luci-app-cake-adapt` 0.3.16-r1, instance
`main` controlling `eth1`/`ifb4eth1` (`vm-before.txt`).

1. `apk add` of the base package and LuCI upgraded both; the service was
   restarted by hand, ran 0.3.17-r1 and kept its qdiscs. Its features file is
   empty. `-V` accepted the original configuration and refused
   `mqtt_enabled '1'` with `option 'mqtt_enabled' requires the cake-adapt-mqtt or
   cake-adapt-mqtt-ssl package` (also with `mqtt_tls '1'`).
2. A temporary rpcd login holding only the `luci-app-cake-adapt` ACL group read
   the features file through uhttpd's `/ubus` (`file.read` returned `""`, then
   `"mqtt\n"`), and was denied another file. The installed `config.js` matches
   the source hash checked by `tests/luci_config.js`, which renders the view for
   each features file. No browser rendered the pages.
3. `apk add` of `cake-adapt-mqtt` while `cake-adapt` was installed was refused
   as a conflict (both are `cake-adapt`); after `apk del cake-adapt` (kept in
   place by LuCI's dependency, world updated) the same `apk add` replaced it,
   adding `libcares` and `libmosquitto-nossl` from the SDK's local packages.
   `/etc/config/cake-adapt` kept its checksum. The service restarted on its own
   and mapped `libmosquitto.so.2.0.22`. `-V` accepted MQTT and refused
   `mqtt_tls '1'` with `option 'mqtt_tls' requires the cake-adapt-mqtt-ssl
   package`.

After (`step3-final.txt`): `cake-adapt-mqtt`, `libcares`, `libmosquitto-nossl`
and `luci-app-cake-adapt` 0.3.17-r1 installed; original configuration (MQTT
unset, so off) and qdiscs; one daemon running; the rpcd test login, test
configurations and copied packages removed; `ubusd` and `rpcd` untouched; LuCI
`getFeatures` answers through uhttpd. `cake-adapt-mqtt-ssl` was built and
inspected but not installed on the VM.

## Fix round: the base package warns instead of refusing MQTT (raw output in `vm-warn/`)

User decision A (2026-10-12): in `cake-adapt`, `mqtt_enabled '1'` only warns;
`mqtt_tls` without TLS stays refused in every package. Step 1 above (the base
package refusing `mqtt_enabled` in `-V`) describes the first round's build and
no longer applies.

Rebuilt x86 packages (same steps as above): `cake-adapt-0.3.17-r1.apk`
`2549a7c0fe88b6afa8f6dfe5ea7405211467713389914608b64182ff5d9a5eeb` and
`cake-adapt-mqtt-0.3.17-r1.apk`
`ad8e13d660ed66f9bf1fa9aef32110e40ae670717929d9658ad7b22d59b23d15`, with the
same declared dependencies as before.

1. `apk del cake-adapt-mqtt && apk add --allow-untrusted ./cake-adapt-0.3.17-r1.apk`
   switched to the base package (LuCI kept the service up).
2. With `mqtt_enabled '1'` and `mqtt_host 'broker.example'` set in UCI, `-V`
   accepted the configuration. After a restart, pid 6604 ran without any
   libmosquitto mapping, and syslog showed `MQTT logging degraded: this package
   was built without MQTT support; install cake-adapt-mqtt or
   cake-adapt-mqtt-ssl`. Within 20 s its log had 376 DATA, 97 LOAD and 2
   SHAPER records (it set both CAKE qdiscs to the 20000 kbit base rate).
3. MQTT was turned off and the instance restarted. The original
   `/etc/config/cake-adapt` was restored (same checksum), and the VM was
   switched back to `cake-adapt-mqtt` (features `mqtt`, libmosquitto mapped).
   The copied packages were removed. `ubusd` and `rpcd` were not touched, and
   LuCI `getFeatures` answers through uhttpd.
