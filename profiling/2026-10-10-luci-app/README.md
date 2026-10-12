# luci-app-cake-adapt on the x86 VM (0.3.16-r1)

Checks that the LuCI app installs, is permitted exactly what its ACL grants,
serves its views, and that Save & Apply restarts or refuses the service.
The look of the pages was not checked (needs a person's eyes:
`http://192.168.56.2/cgi-bin/luci/admin/services/cake-adapt`).

## Setup

OpenWrt 25.12 x86 VM 192.168.56.2 (just rebooted, cake-adapt 0.3.16-r1 running,
no rpcd/LuCI). Installed from the feeds: `luci` (with `curl` for the checks);
then `bin/cake-adapt-0.3.16-r1-x86.apk` and `bin/luci-app-cake-adapt-0.3.16-r1.apk`
(freshly built from `e578a89`+README, checksums in `raw/sha256.txt`) and
restarted rpcd and uhttpd. The original service configuration is `raw/uci-original.txt`.
No real LuCI user has only the app's groups, so a temporary rpcd login `lucitest`
(groups `luci-app-cake-adapt` and `luci-base`; uci methods come from `luci-base`) was
added for the checks and removed afterwards.

## Checks and results

- Step 1 (`raw/step1.txt`): `view/cake-adapt/{config,status}.js` installed.
  `ubus call rc list '{"name":"cake-adapt"}'` returns `start 95, stop 10, enabled true,
  running true` - this rpcd has the `running` field, so the Status view uses it.
- Step 2 (`raw/step2.txt`), over `/ubus` with the `lucitest` session: `cake-adapt.main status`,
  `uci get cake-adapt` and `rc list` return data; `network.interface dump` is
  "Access denied". With only the app's group, `uci get` is denied too: the uci
  methods are granted by the `luci-base` group that every LuCI login has.
- Views (`raw/step-views.txt`): after a real LuCI login (`luci_username` form) the
  menu has Services -> CAKE Adapt (`satisfied: true`) with Status and Configuration;
  both view JS files are served (HTTP 200) and the Status page route returns 200.
- Step 3 (`raw/step3.txt`), `uci set` + `uci apply {"rollback":false}`: base_dl
  20000 -> 25000 changed `/var/run/cake-adapt/main.uci` and the PID (2975 -> 6604).
  min_dl 30000 (above base): the daemon is stopped and not restarted; syslog has
  "download rates must satisfy minimum <= base <= maximum" and "not starting
  instance 'main'"; `rc list` shows `running: false`. Restoring both values started
  PID 7305. Note: the refused run leaves the old `main.uci` in place.
- Final: UCI export identical to the original (`raw/uci-final.txt`), cake-adapt running,
  both CAKE qdiscs at 20Mbit. LuCI, curl stay installed.

## Limits

Not a browser: JavaScript execution, form layout, the charts and the service
buttons were not exercised. `raw/luci-files.txt` shows the package also ships
`*.js.o` files next to the views (made by luci.mk, not by this repository).
Filogic build needed `ubus` linked into the SDK (`package/feeds/base/ubus`),
as already done for x86; it is SDK-local, not in the repository.
