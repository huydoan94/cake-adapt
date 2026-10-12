# Restart keeps a refused instance, unlimited respawn, ubus info in syslog, x86 VM (2026-10-11)

Checks of `513eea7`:

- `restart` (all, or `restart <section>`; LuCI's Restart button calls it
  through rpcd) checks each requested instance's new configuration before
  stopping it. A valid one stops and starts the instance (new pid) even when
  nothing changed; a refused one keeps it running its previous runtime file
  and pid, as a reload does.
- Each instance is registered with `respawn 3600 5 0`: retry 0 means procd
  never gives up, and it waits 5 s before each new start.
- `connected to ubus …` and `disconnecting from ubus` go through
  `log_system_message()`: syslog at info level, and a `SYSLOG` record in the
  daemon's log file.

## Setup

- Package `cake-adapt-0.3.16-r1.apk` built from `513eea7` (SHA-256
  `454ffd305d8740684bb519be8a5e09516f6662bb3189f0fa00809dac99f60a3e`,
  `raw/sha256.txt`, with the Filogic build), installed with
  `apk add --force-reinstall` over the 0.3.16-r1 build of `ad66881` on the x86
  VM (`192.168.56.2`). The previous binary, init script and configuration are
  kept in `/root/rollback/*.before-restart`. State before: `raw/vm-before.txt`
  (respawn was `3600 5 5`), install: `raw/install.txt`.
- The VM's single `main` instance (`eth1`/`ifb4eth1`).
- Scripts in `scripts/` (`cat lib.sh restart-cases.sh | ssh root@192.168.56.2 sh -s`,
  then `ubus-syslog.sh` the same way); output in `raw/`.

## Results

| Case | Check | Result |
| --- | --- | --- |
| R1 | `restart`, configuration unchanged | pid 5114 → 5367, same `main.uci`; `Stopped` then `Starting` 40 ms apart, no lock conflict |
| R2 | `min_dl` 60000 > `max_dl` 50000, `restart` | pid 5367 and file kept; syslog: `configuration error: download rates must satisfy minimum <= base <= maximum`, `configuration of instance 'main' refused`, `instance 'main' keeps its previous configuration /var/run/cake-adapt/main.uci` |
| R3 | same, `restart main` | pid 5367 kept, same three lines |
| R4 | configuration fixed, `restart main` | pid 5367 → 6091 |
| R5 | `stop`, refused configuration, `restart` | not started, no runtime file; `could not generate /var/run/cake-adapt/main.uci; not starting instance 'main'` |
| R6 | configuration restored, `start` | started; `service list` shows `respawn { "threshold": 3600, "timeout": 5, "retry": 0 }` |
| U1 | `ubusd` killed | errors as before, then `daemon.info cake-adapt[6562]: INFO: … connected to ubus; status published as cake-adapt.main` in `logread` 2 s later, and `SYSLOG; … connected to ubus …` in `/var/log/cake-adapt.log`; status answered, same daemon pid |

`restart main` touching only that instance is covered by `tests/test_init.sh`
(two configured instances); the VM runs one. Host tests: `raw/host-test.txt`.

## Fix after review (`6a79733`)

A named restart of a section that is disabled or no longer configured stops
its instance and removes its runtime file; a failed stop (any ubus status but
Not found) is logged and fails the restart; TERM is ignored while stopping.
Package SHA-256 `7e361b82082419e5fbc172e3b89f79391e6b1423222c11616432ee522dab7765`
(`raw/sha256.txt`, `raw/install-fix.txt`), cases in
`scripts/removed-sections.sh`, output in `raw/removed-sections.txt`:

| Case | Check | Result |
| --- | --- | --- |
| F0 | `restart tmpcheck` (never configured) | main untouched (pid 6562); procd logs its own `instance tmpcheck not found` at err, the script `no cake_adapt instance 'tmpcheck' configured` |
| F1 | section `main` deleted, `restart main` | pid 6562 stopped, `main.uci` removed |
| F2 | restored and started (7241), `enabled=0`, `restart main` | stopped, `main.uci` removed |
| F3 | configuration restored, `start` | running (7532), configuration identical to the original |

No second controller was started: `tmpcheck` was never added to UCI. The
failed-stop branch is covered by `tests/test_init.sh` only.

## Limits

- One run of each case.
- `disconnecting from ubus` was not seen on the VM: a killed `ubusd` closes the
  socket in a way libubus notices, which leads to the reconnect path (case 3),
  not to the `POLLHUP` path that logs it (case 4). That it goes to syslog at
  info is covered by `tests/monitor/test_publish.c` only.
- Unlimited respawn was checked as procd's registered values, not by making
  the daemon exit repeatedly. That retry 0 means unlimited is an assumption
  from procd's source behaviour, which the SDK does not contain
  (`procd.sh` only documents the three values as threshold, restart timeout
  and maximum failures).
