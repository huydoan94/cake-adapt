# End-to-end check of 0.3.26 (2026-10-22)

## Result

0.3.26 installs, upgrades and runs its whole service lifecycle on the x86 VM:
28 of 29 checks pass. The one failure is the daemon being restarted when
ubusd was killed seconds after a reload. Done by hand with nothing else
pending, the same ubusd restart kept the daemon. Under load on the emulated
ISP, 0.3.26 controls like 0.3.25 in three of the four phases. In the
bidirectional phase its median added delay was higher in all three rounds;
the code paths that decide rates are unchanged between the two builds (below),
so this is most likely the testbed's spread, not settled by three rounds.

## Lifecycle

[`lifecycle.sh`](lifecycle.sh) on `192.168.56.2` (OpenWrt 25.12 x86) with the
0.3.26 packages (`cake-adapt-mqtt`, `luci-app-cake-adapt`, fping kept
installed): upgrade, `-D` defaults directly and through rpcd as LuCI asks for
them, start with the native pinger, reload, an exec-file change, a refused
`config_file` in `/etc/config/cake-adapt`, the fping fallback and back,
a ubusd restart, stop and start, and the test log's inode across all of it.
Results in [`lifecycle/`](lifecycle/) (`steps` is the run's output).

| Check | Result |
| --- | --- |
| version 0.3.26, fping kept, exec file mode 0600 | pass |
| `-D` lists every option, native is the default, `-D` alone, rpcd runs `-D`, the ACL grants it | pass |
| running, no fping child, native samples, ubus status, LuCI answers | pass |
| same pid after a reload without change; restart on an exec-file change; the file's value imported and in the runtime | pass |
| `config_file` in `/etc/config/cake-adapt` refused and logged; the instance keeps running | pass |
| fping child running and sampling; back to native | pass |
| **same daemon after ubusd restart** | **fail** |
| still sampling, ubus object back, LuCI answers after the ubusd restart | pass |
| stop, start, test-log inode kept | pass |

**The failure.** The step kills ubusd 7 s after the previous step's
`/etc/init.d/cake-adapt reload` (back to native) started pid 8713. At the
moment ubusd went away, procd ran the init script, which could not reach ubus
(syslog 16:38:35: "Failed to connect to ubus", "Failed to parse JSON data"),
so it could not compare the running instance and restarted it as 9223
([`lifecycle/syslog-tail`](lifecycle/syslog-tail)). Repeated by hand with no
reload just before, ubusd's restart left the daemon (pid 10590) running and its
status object came back. What made procd run the script at that moment is
not established; the daemon itself kept running until the init script
replaced it, and sampling and the status object recovered either way.

[`lifecycle-run1/`](lifecycle-run1/) is the first run, with two failures:
"same daemon" as above, and "kept running", a test fault since fixed. The
test read the pid with `pidof`, which also caught the configuration
validator (`cake-adapt -V`) and the exiting daemon; it now reads
`/var/run/cake-adapt/main.lock`.

## Under load: 0.3.26 against 0.3.25

[`control.sh`](control.sh) ran three rounds of `tools/testbed/run.sh` per build
on the same VM, alternating which build went first (16:45-17:14), with the
native pinger and cake-autorate's delay thresholds (10, 30, 60). The builds
are the profile's
([`../2026-10-22-profile-0.3.26`](../2026-10-22-profile-0.3.26/README.md#builds)).
Every run took 4 min 47 s and the VM did not stall or reboot during them.
Results in [`control/`](control/), scored with `tools/testbed/analyze.py`
([`control/analyze.txt`](control/analyze.txt)) and `shapers.py`
([`control/shapers.txt`](control/shapers.txt)).

Medians of the three rounds; added RTT in ms over the idle base (21.3-22.0 ms),
the share of time with more than 30 ms added, and the capacity used:

| Phase | Build | p50 | p95 | > 30 ms | upload used | download used |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| upload steady | 0.3.25 | 33.1 | 99.6 | 52.4% | 92.4% | |
| | 0.3.26 | 27.3 | 103.0 | 47.9% | 91.3% | |
| upload step | 0.3.25 | 5.3 | 149.6 | 37.8% | 85.4% | |
| | 0.3.26 | 4.4 | 155.0 | 36.4% | 84.4% | |
| download steady | 0.3.25 | 20.7 | 85.6 | 31.1% | | 85.6% |
| | 0.3.26 | 21.9 | 78.7 | 32.7% | | 86.8% |
| bidirectional | 0.3.25 | 17.8 | 109.6 | 44.2% | 82.8% | 38.4% |
| | 0.3.26 | 39.1 | 101.1 | 59.6% | 86.7% | 39.5% |

No run lost a probe. Two differences hold in all three rounds:

- bidirectional: 0.3.26's p50 was 36.8-44.5 ms against 12.2-29.0, its time
  above 30 ms 52-60% against 43-49%, and its upload shaper sat above the
  bottleneck for 51-63% of the phase against 43-47%; its p95 was lower
  (98.7-102.0 against 105.2-113.2);
- download steady: 0.3.26 cut its upload shaper 9-21 times against 28-33.

Three rounds against three cannot separate these from the testbed's spread
(the smallest two-sided p of a rank test on 3 + 3 runs is 0.1), and the
source between the builds does not change a decision: the controller,
`monitor/control.c`, `monitor/tcpdelay.c`, the TCP filter, the native pinger
and the latency tracker differ from 0.3.25 only by helpers, local pointers,
merged `switch` cases and conditions rewritten by De Morgan's laws, each
equivalent to the old code (`git diff 77a2517 ed9b42c -- src`). The replay
test matches as before. More rounds would settle it.

Both builds leave tens of milliseconds of delay on this testbed with
cake-autorate's thresholds; this compares the two builds, not the controller
against a target.
