# ubus status thread and instance lock on the x86 VM (2026-10-11)

Checks of two changes on the OpenWrt 25.12 x86 test VM (`192.168.56.2`,
32-bit kernel):

- `96bbd7b` serves the `cake-adapt.<section>` ubus status object from a thread
  of its own, so the main loop never calls libubus. Before it, a stopped
  `ubusd` froze the daemon (79 s without a DATA record in an earlier check).
- `33646d1` lets only one daemon run per instance name, through an exclusive
  lock on `/var/run/cake-adapt/<section>.lock`.

## Setup

- Package `cake-adapt-0.3.16-r1-x86.apk` built from `33646d1`
  (SHA-256 `4e564486f9ae3018cb47bc5ce8baf9af34bcc3dd172f777827b4f4a2a3740683`),
  installed over the 0.3.16-r1 build from `d0abad2`. The installed binary
  went from 114,709 to 114,693 bytes.
- The VM's existing `main` instance: `eth1`/`ifb4eth1`, both directions
  adjusted (10/20/50 Mbit/s), fping, `tcp_delay_attribution` and
  `tcp_ts_request` on, log file `/var/log/cake-adapt.log`.
- `ubusd` frozen with `kill -STOP` and released with `kill -CONT`; every script
  continues it on exit. `logread` and `ubus` need `ubusd`, so while it was
  stopped the checks read the daemon's log file instead.
- Download load in step 2: repeated 25 MB downloads from
  `speed.cloudflare.com` over `eth1` (about 16 Mbit/s each).
- The scripts are in `scripts/` (`lib.sh` is prepended to each, run as
  `cat lib.sh stepN.sh | ssh root@192.168.56.2 sh -s`); their output is in
  `raw/`.

## Results

| Step | Check | Result |
| --- | --- | --- |
| 1 | `ubus call cake-adapt.main status` | Same fields in the same order as the previous build (`raw/step1-fields.txt`). Published 0.5 s after start. |
| 2 | `ubusd` stopped 60 s under download load | Control went on: DATA records grew 3230 → 3818 in the first 30 s, the shaper moved (download 45.8 → 50 Mbit/s, upload 10.7 → 14.3 Mbit/s); nothing fatal logged. In an earlier idle attempt (no load reached the link) the daemon changed to IDLE on its own after 60 s while `ubusd` was stopped. A status call meanwhile hung until its 3 s watchdog (ubusd's side). After CONT the next call answered at once (`raw/step2-ubusd-stopped-60s.txt`). |
| 3 | Daemon started while `ubusd` stopped (SIGKILL; procd respawned it) | Started controlling (records, TCP capture); "status not on ubus yet (ubusd unavailable?); retrying" logged once, 10 s after start. Its ubus thread sat in `unix_stream_read_generic` (libubus' blocking hello read), the main thread in `epoll_wait`. After CONT: "status published on ubus as cake-adapt.main" within about 1 s (`raw/step3-4-…`, `raw/step3-published-after-cont.txt`). |
| 4 | SIGTERM while `ubusd` stopped and the thread stuck in the handshake | Exited 2090 ms after SIGTERM, with "ubus status thread did not stop within 2.0 s; leaving it behind" (procd allows 5 s). procd respawned it. |
| 4b | `/etc/init.d/cake-adapt stop` while `ubusd` stopped | The init script itself waits: it asks procd over ubus, so no signal reaches the daemon until `ubusd` runs again. After CONT the daemon exited in 110 ms (`raw/step4b-…`); with a connected thread, shutdown took about 0.1 s. |
| 5 | `ubusd` killed; procd restarted it | "lost the ubus connection; status cake-adapt.main returns when ubusd does" at once, published again 2 s later with the same daemon pid; status answers (`raw/step5-ubusd-killed.txt`). |
| A2 | Second daemon by hand: `/usr/sbin/cake-adapt -C /var/run/cake-adapt/main.uci -S main` | Exit 1 after 10 ms, syslog `ERROR: instance 'main' is already running (pid 11086); exiting`. The service's pid, log and qdiscs unchanged. `-V` and `-L` still exit 0 while the service runs, and `/etc/init.d/cake-adapt restart` works (`raw/stepA2-instance-lock.txt`). |

## Limits

- One run of each step on one VM; timings have 10 ms resolution (`/proc/uptime`).
- The log file rotated by size during step 2, so its line counts restart there;
  the DATA, LOAD and SHAPER records themselves show the loop running.
- A first run of steps 3 and 4 took the daemon's pid from `pidof`, which also
  matched short-lived `cake-adapt` processes at start-up; it was rerun taking
  the pid from the lock file, and only the rerun is kept.
- The `/etc/init.d/cake-adapt running` processes visible in step 3 are shell
  instances blocked on `ubus call service list`, not daemons.

## VM state afterwards

The new x86 package stays installed (the previous binary is kept as
`/root/cake-adapt.orig-0.3.16`); the `main` instance runs with its original
configuration, `ubusd` is running, and the CAKE qdiscs are as before
(`raw/vm-end-state.txt`).
