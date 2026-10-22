# Logging off the main loop: a stuck logd (2026-10-21)

**Result:** before this change a stuck logd froze cake-adapt's measurement and
control: on VM .2 the main loop stopped about 4 s into a 20-s logd pause and
stayed stopped until logd resumed. With the log writer thread the main loop
keeps working through the whole pause; the writer drops what does not fit
(160 lines here) and says so once logd is back. Stopping the service while
logd is stuck takes about 3 s, within procd's 5-s kill.

Why: musl's `syslog()` sends to `/dev/log` and waits when logd's socket
queue is full. Linux queues only `max_dgram_qlen` (10 on the VM) datagrams
for a receiver that is not reading; with logd paused, BusyBox `logger` (the
same `syslog()`) hung on its 11th message until logd resumed, then all 30
arrived.

Method ([`measure.sh`](measure.sh), run on VM .2 by each build in turn):

1. Set `log_DEBUG_messages_to_syslog`, so debug messages also go to syslog.
2. Read `ubus call cake-adapt.main status` once a second: 10 s normal, 20 s
   with logd paused (`kill -STOP`), 10 s after it resumes.
3. During the pause, send ten `SIGUSR1` export requests 1 s apart. Each logs
   two debug messages to syslog: more than the kernel queues.

`download.delay_us` changes with nearly every reflector sample, so distinct
values per window show whether the main loop kept working; a frozen value
means it stopped.

| Build | Before | logd paused | After |
| --- | --- | --- | --- |
| 0.3.19-r1 (`8aa6e75`, no writer thread) | 10 of 10 | **4 of 20** (frozen at 1999 µs for 16 s) | 10 of 10 |
| 0.3.20-r1 (this change) | 10 of 10 | 20 of 20 | 10 of 10 |

The new build logged "1 log lines dropped" and "159 log lines dropped: the
log writer fell behind" once logd resumed. Raw samples and summaries are in
`before-change/` and `after-change/` (0.3.20 is the working tree's version
number, not a release).

Shutdown: with logd paused and the syslog queue full, `/etc/init.d/cake-adapt
stop` ended the daemon after 3.0 s (the writer's 2-s drain bound, measured
with a 1-s poll), and `start` brought it back normally.

The VM was returned to its configuration without
`log_DEBUG_messages_to_syslog`, running the new build; exports were removed.

## Addendum: overwrite the oldest instead of dropping the newest

The user asked to keep logs complete where possible and the newest lines
above all: the queue is now a bounded FIFO whose oldest waiting lines make
room for new ones, counted and reported before the oldest line left
("N older log lines and M syslog messages were overwritten"); an overwritten
`SIGUSR1`/`SIGUSR2` request still runs. Same method, same VM, the new build
(`overwrite-oldest/`):

| Build | Before | logd paused | After |
| --- | --- | --- | --- |
| 0.3.20-r1, overwrite oldest | 10 of 10 | 20 of 20 | 10 of 10 |

The log file and syslog then said "88 older log lines and 1 syslog messages
were overwritten: the log writer fell behind".
