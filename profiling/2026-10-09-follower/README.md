# Cost of the capture follower on the router (2026-10-09)

## Result

While `tools/capture-log.sh` followed the log, its follower on the router cost
about twice what cake-adapt itself does: 0.33% of one core on the x86 VM for
the follower and 0.11% for its SSH session, against 0.16% for the daemon. Almost
all of it was starting three processes every second (`ls`, `cat` and `sleep`,
each a fork and exec of BusyBox), about 13,700 system calls a minute. The
POSIX rewrite of the follower's watchdog cost the same as the `read -t`
version it replaced.

The follower now starts only the `cat` that copies new lines: the PC's
heartbeat, sent every second, is its clock instead of `sleep`, and `ls` runs
only when `cat` found nothing. It costs 19 ticks in 180 s instead of 59 (68%
less), with half the system calls, and captured the same lines across a
rotation.

## Setup

- OpenWrt 25.12 x86 VM (i686), `192.168.56.2`; BusyBox `sh` runs the follower.
- The installed 0.3.15 daemon on the emulated testbed with router-like log
  output (debug, processing, load, reflector, summary, CPU and memory records,
  2 MB rotation) under a steady 3 Mbit/s download, writing about 4 KB/s; the
  overseas router's log grew 4.7 KB/s.
- [`tools/run.sh`](tools/run.sh) on the PC and [`tools/fprof.sh`](tools/fprof.sh)
  on the VM: three minutes with no follower, three with `capture-log.sh -l`
  following the log over SSH, then one minute of `strace -f -tt` and one of
  `strace -f -c` on the follower. CPU is user + system ticks of 10 ms
  including waited-for children, so the follower's `ls`, `cat` and `sleep`
  are counted; one core for 180 s is 18,000 ticks.
- The test log followed the VM rules (inode unchanged in both runs), and no
  namespace, fping or follower was left.

## Numbers

| Over 180 s | committed (`07ab249`, `read -t`) | POSIX watchdog |
| --- | ---: | ---: |
| follower and its children | 59 ticks (0.33%) | 60 ticks (0.33%) |
| its dropbear session | 21 ticks (0.12%) | 18 ticks (0.10%) |
| cake-adapt, no follower / with follower | 30 / 29 ticks | 30 / 27 ticks |
| processes started per minute (strace) | 162: 54 each of `cat`, `ls`, `sleep` | 165: 55 each |
| system calls per minute | 13,625 | 13,730 |
| lines captured | 24,614 | 24,604 |

| Over 180 s | one `cat` per second (`raw/one-cat/`) |
| --- | ---: |
| follower and its children | 19 ticks (0.11%) |
| its dropbear session | 17 ticks (0.09%) |
| cake-adapt, no follower / with follower | 28 / 26 ticks |
| processes started per minute | 57, all `cat` |
| system calls per minute | 6,655 |
| lines captured; largest gap between timestamps | 24,539; 0.31 s, none out of order, across a rotation |

The SSH session's figure moves by a few ticks between runs; the heartbeat is
now one line a second instead of one every 5 s.

The system calls are almost all process start and teardown (`mmap2`,
`mprotect`, `open`, `close`, `rt_sigprocmask`, `wait4`), plus the `read`s and
`poll`s of the copies. On this BusyBox, `sleep` runs as its own process. A
slow router pays proportionally more per process start, so these figures
understate its share there.

`raw/committed-07ab249/`, `raw/posix/` and `raw/one-cat/` hold each run's samples, both
traces, the console and the capture's own events.

## The POSIX follower

The follower is POSIX sh: its watchdog no longer uses `read -t` (not in
POSIX, missing from dash). One subshell counts heartbeats into
`/tmp/cake-adapt-capture.<pid>.beats` with builtins only and ends the session
at end of input; another wakes every 30 s and ends it when the count has not
moved, so a silent network ends the follower within a minute instead of 30 s.
Checked with ShellCheck in POSIX mode and with dash and BusyBox ash locally
(all lines in order across a rotation, exit at end of input, exit 62–64 s
after the heartbeats stop, nothing left), and over SSH on the VM (stopping
the capture and killing the SSH client both remove the processes and the beat
file within 3 s).

The one-`cat` follower adds a FIFO beside the beat file for the ticks, and
was checked the same way: ShellCheck, dash and BusyBox ash locally (all lines
across a rotation, exit at end of input, exit 62–64 s after silence, both files
removed), and over SSH on the VM (stop and SSH kill clean within 3 s).

The first profiled POSIX version left its beat file behind when SSH ended the
session with SIGHUP; the follower now removes it on every exit, which is the
version tested over SSH above.
