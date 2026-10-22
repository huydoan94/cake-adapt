# LuCI app for cake-adapt: design

Date: 2026-10-10. Status: approved in conversation, awaiting review of this
written spec at that time. The 0.3.17 implementation and runtime evidence are
described in the [technical overview](../../technical.md#status-and-deferred-work).

Agent execution: this historical spec applies equally to Claude and Codex.
Use the corresponding route in [COLLABORATION.md](../../../COLLABORATION.md),
the current [agent guide](../../../AGENTS.md) and
`HANDOFF.md`. The two-SDK acceptance requirement below belongs
to this milestone; it does not change the x86 default for subsequent work or
authorize new downloads, copying, deployment or version changes.

## Goal

Let a router's owner configure cake-adapt and watch what it is doing from
LuCI, under **Services → CAKE Adapt**, without SSH. The app uses LuCI's
standard look only: stock form widgets, stock tables and buttons and the
theme's own CSS, with no custom stylesheet and no third-party JavaScript.

## Decisions

| Topic | Decision |
| --- | --- |
| Purpose | Configure and see live status |
| Options exposed | Everyday settings up front; every other option in an Advanced tab |
| Live status | Current values per instance, plus a rolling chart of rates and delay |
| Status source | The daemon publishes status over ubus; LuCI polls it through rpcd |
| Packaging | A separate, architecture-independent `luci-app-cake-adapt` package |
| Instances | Add and remove instances in LuCI; stock Start / Stop / Restart and enable-at-boot controls |

## Architecture

Two packages from this repository.

1. **`cake-adapt`** (existing) gains a ubus status object. Through its
   existing `uloop`, the daemon connects to `ubusd` and registers one object
   per instance, `cake-adapt.<section>`, with a `status` method answered from
   values it already holds. New dependency: `libubus`, part of every OpenWrt.
2. **`luci-app-cake-adapt`** (new), in `luci-app-cake-adapt/` with its own
   package `Makefile`, `PKGARCH:=all`, depending on `cake-adapt` and
   `luci-base`. Its files:
   - `root/usr/share/luci/menu.d/luci-app-cake-adapt.json`: the menu entry
     under Services, with the Status and Configuration pages;
   - `root/usr/share/rpcd/acl.d/luci-app-cake-adapt.json`: read and write on
     UCI `cake-adapt`, ubus `cake-adapt.*` → `status`, and the service's
     init actions;
   - `htdocs/luci-static/resources/view/cake-adapt/status.js`;
   - `htdocs/luci-static/resources/view/cake-adapt/config.js`.

   No CSS file and no third-party JavaScript; charts are SVG drawn the way
   LuCI's Status → Realtime Graphs draws them.

Boundaries: the daemon does not know about LuCI and only publishes status over
ubus. The LuCI app reads UCI and ubus and controls the service through the
init script; it never parses logs or runs `tc`.

## Daemon: ubus status

- **Object:** `cake-adapt.<section>` per running daemon, one method,
  `status`. The Status page finds instances by listing `cake-adapt.*`.
- **Reply**, in the project's base units (LuCI formats them):

  ```json
  {
    "version": "0.3.16-r1", "pid": 1234, "started": 1791650000,
    "state": "running",
    "download": {
      "interface": "ifb4wan",
      "available": true,
      "shaper_bps": 85000000,
      "achieved_bps": 61200000,
      "load_e6": 720000,
      "delay_us": 3400,
      "bufferbloat": false,
      "tcp_queue_us": 2100
    },
    "upload": { "...": "the same fields" }
  }
  ```

  - `state` is `running`, `idle` or `stall`, the activity state.
  - `started` is the daemon's start time in seconds since the epoch.
  - A direction whose CAKE is missing has `"available": false` and no
    numbers.
  - `tcp_queue_us` appears only while TCP attribution has an estimate.
- **Code:** `monitor/status.c` (prefix `status_`), the part of
  `struct monitor` that owns the ubus connection. A pure function fills a
  plain C status structure from the monitor's parts; a thin layer turns that
  structure into the blobmsg reply. The connection uses `ubus_auto_connect`,
  which reconnects by itself.
- **Failure:** no `ubusd`, or `ubusd` restarting, is never fatal. The daemon
  logs one warning and keeps controlling CAKE; status returns when ubus does.
- **Cost:** nothing until asked; one reply per poll. No timers, no stored
  history: the chart's history lives in the browser.

## LuCI pages

### Services → CAKE Adapt → Status

- Top: the service state with the stock Start / Stop / Restart buttons and the
  enable-at-boot switch, through the init script.
- One standard section per instance, titled with its name:
  - its state (running / idle / stall) and start time;
  - a stock `cbi` table with a Download and an Upload row and the columns
    shaper rate, achieved rate, load %, added delay, bufferbloat (yes/no) and
    TCP queue ("–" when none);
  - two charts covering the last 5 minutes (150 points at a 2 s poll): rates
    (shaper and achieved, both directions) and added delay (both directions).
- Refresh every 2 s through LuCI's poller, only while the page is open.
- A configured instance that publishes no status shows as not running; with
  none running, the standard notice links to Configuration.

### Services → CAKE Adapt → Configuration

- A `form.Map` over `/etc/config/cake-adapt`; each `cake_adapt` section is an
  instance, with stock add (named) and delete controls.
- Tabs per instance:
  - **General:** `enabled`, `interface` or the `ul_if`/`dl_if` pair, download
    and upload adjustment;
  - **Shaper rates:** minimum, base and maximum for download and upload, in
    kbit/s;
  - **Reflectors:** the `reflectors` list;
  - **Advanced:** every other option, under headings that follow
    `docs/configuration.md` (delay detection, timing, logging, TCP
    measurement and the rest).
- Validation: stock datatypes in the browser, plus
  minimum ≤ base ≤ maximum. On Save & Apply, LuCI's normal apply; the existing
  reload trigger restarts the service, and the init script's `-V` check still
  refuses a bad configuration, with the reason in syslog.
- Help text comes from `docs/configuration.md`, shortened.

## Testing

- **Daemon:** a host test of the status-structure function with synthetic
  states (running, idle, one direction's CAKE missing, TCP estimate present
  and absent). On VM .2, `ubus call cake-adapt.main status` during a
  controlled testbed run (load, idle, CAKE removed and re-created), checked
  against that run's log records; and `ubusd` restarted while the daemon
  runs, which keeps controlling CAKE and publishes again.
- **LuCI app:** host checks with `node --check` on both views, a test that
  compares the form's option names with the daemon's option table
  (`src/config/config.c`) so the Advanced tab cannot drift, and validation of
  the menu and ACL JSON. On VM .2, with LuCI installed (the VM image may be
  changed freely): rpcd checks as the browser makes them (login, UCI read and
  write, ubus status, the init actions the ACL allows), a Save & Apply round
  trip into `/var/run/cake-adapt/<section>.uci` with a daemon restart, and a
  refused configuration leaving the old one running with the reason in
  syslog. The look in a browser is the owner's check at
  `http://192.168.56.2`; the report says which checks were run and which need
  a person.
- **Packages:** both SDK builds of `cake-adapt` and the `luci-app-cake-adapt`
  package, with checksums.

## Documentation

A README section on the LuCI app, and the ubus status object described in
`docs/technical.md`.

## Out of scope

- Historical graphs beyond the open page's 5 minutes (no stored history).
- Any change to how the daemon measures or decides.
- Custom styling of any kind.
