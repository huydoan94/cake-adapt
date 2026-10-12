# Service reload and ubus states like dnsmasq, x86 VM (2026-10-11)

Checks of two changes against OpenWrt's dnsmasq (2.93 and its init script):

- `4fbaa0c` (init): reload registers the instances again without stopping
  them; procd watches each instance's generated `/var/run/cake-adapt/<section>.uci`
  and restarts only changed instances. A refused configuration keeps the
  previous file, so the instance keeps running. `start`, `stop`, `restart`
  take a section name.
- `dabe4d0` (publish): the ubus thread handles each ubus state as dnsmasq
  does and the main thread logs it with dnsmasq's levels.

## Setup

- Package `cake-adapt-0.3.16-r1-x86.apk` built from the tree of `ad66881`
  (SHA-256 `05ca9ced9276cd2cfaf3e3a7dff5dbb026f687b4230aa2b8c0842c078e027897`,
  `raw/sha256.txt`), installed with `apk add --force-reinstall` over the
  0.3.16-r1 build from `7bebb24` on the x86 VM (`192.168.56.2`). State before:
  `raw/vm-before.txt`, install: `raw/install.txt`.
- The VM's single `main` instance (`eth1`/`ifb4eth1`, both directions
  adjusted). No second running instance: two daemons on one CAKE are not
  allowed, so a disabled `second` section shows that an unchanged instance
  keeps its pid.
- Scripts in `scripts/` (`cat lib.sh init-cases.sh | ssh root@192.168.56.2 sh -s`,
  and `ubus-cases.sh` alone); output in `raw/`.

## Results

| Case | Check | Result |
| --- | --- | --- |
| A1 | reload, no change | pid 5205 kept, `main.uci` unchanged |
| A2 | reload, `base_dl_shaper_rate_kbps` 20000 → 21000 | restarted (5205 → 5704) with the new file |
| A3 | reload, `min_dl` 60000 > `max_dl` 50000 | pid 5704 and file kept; syslog: `configuration error: download rates must satisfy minimum <= base <= maximum`, `configuration of instance 'main' refused`, `instance 'main' keeps its previous configuration /var/run/cake-adapt/main.uci` |
| A4 | reload, `min_dl` fixed and a disabled `second` section added | main's file back to the A2 content, pid 5704 kept; `instance 'second' disabled` |
| A5 | `stop second`, `stop main`, `start main`, `restart main`, `start nosuch` | `stop second` left main; `stop main` stopped it and removed `main.uci`; `start main` started it; `restart main` gave a new pid; `start nosuch` logged `no cake_adapt instance 'nosuch' configured` |
| A6 | `enabled=0`, reload | instance stopped, file removed; re-enabled, started |
| A7 | section `main` deleted, reload | instance stopped, file removed |
| A8 | stop, then start with `min_dl` 60000 | not started; syslog has the `-V` reason and `could not generate …; not starting instance 'main'` |
| B1 | `ubusd` killed (cases 1, 2, 3) | at once `cannot reconnect to ubus: Connection failed` and `cannot publish status on ubus: no connection` (errors), then `connected to ubus; status published as cake-adapt.main` (info) 2 s later when procd had respawned `ubusd`; status answered; same daemon pid |
| B2 | `ubusd` stopped 15 s | the main loop kept its 5 LOAD records a second (`raw/ubus-b2-records.txt`; the line was idle, so no DATA); nothing logged; status answered after CONT |

Cases 4 (`POLLHUP` without an end libubus noticed) and 5 (object refused,
first or later connection) are covered by host tests only
(`tests/monitor/test_publish.c`); the VM's `ubusd` ACLs were not touched.
Host tests, and the publish test under ASan/UBSan and TSan, passed
(`raw/host-test.txt`, `raw/sanitizers.txt`).

## Limits

- One run of each case. Timings have 1 s resolution.
- The first B1 run is kept as `raw/ubus-cases-run1-truncated.txt` and
  `raw/ubus-run1-vm-stall.txt`: the VM kernel reported RCU stalls ("timer
  wakeup didn't happen for 6520 jiffies") at the same time, every timer on the
  VM stopped for about 65 s, procd took two minutes to respawn `ubusd`, and the
  SSH session timed out. The daemon logged the same three messages in the
  same order once the VM resumed. The VM was rebooted and B1/B2 were run again;
  only the rerun is in the table.
- The info messages (`connected to ubus …`) go to the daemon's log file;
  syslog shows the errors.
- The binary installed before the test was not kept (its copy was overwritten
  by the init script's in the rollback directory); it can be rebuilt from
  `7bebb24`.

## VM state afterwards

The new x86 package stays installed; `/etc/config/cake-adapt` is identical to
the one before the test, `main` runs, `ubusd` runs, and no test file is left
(`raw/vm-end-state.txt`). The CAKE rates differ only by the controller's own
changes since the reboot.
