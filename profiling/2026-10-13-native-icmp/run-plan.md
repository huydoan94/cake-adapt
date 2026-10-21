# Native ICMP phase 3 bounded batch
Primary implements harness and analyzes; Astra Medium reviews only harness/result.
No daemon changes, branch/commit/version/dependency/install or production actions.
Question: does native echo/timestamp preserve control/loss response and reduce
whole daemon+pinger CPU versus fping/fping-ts on the i386 test VM?
One preflight smoke (native, shortened load; fault phases retained); then four
sequential cases: fping,native,fping-ts,native-ts, one each, original run.sh
15s idle/60s upload/10s idle/60s capacity-step/10s idle/60s download/10s idle/60s bidirectional.
Same binary,6 numeric reflectors,300ms interval; thresholds10/30/60ms;
TCP attribution and ACK filter off, 8/40Mbit bottleneck,upload step4Mbit;
2/6/12Mbit UL bounds,10/30/60Mbit DL bounds. Installed controller stays isolated.
After main end: sustained TCP upload during15s one-reflector ICMP loss,12s recovery,
25s all-reflector ICMP loss,12s recovery. Health records/logs retained.
CPU raw proc stat at phase boundaries: daemon own and waited-child ticks plus live
direct child own/waited ticks, excluding independent probe/iperf. This survives
pinger restart; report quantized ticks, duration and separate daemon own cost.
Metadata: binary/script/config hashes,kernel/boot,CPUallowedmask,clocksource,USER_HZ,
fixed test-log inode,installed controller PID/hash,route,qdiscs.
Metrics: independent probe added RTT p50/p95/p99,time>15/30ms,loss;
iperf received throughput/capacity fraction;shapers;health warning and recovery times.
Acceptance: strict no-worse reported latency/time-bloated,matched loss/stall response,
lower total daemon+pinger CPU with native and no daemon fping child. Single batch
has scheduler noise and cannot establish statistical equivalence; no arbitrary
performance tolerance or repeated runs to seek a pass. Regression stops migration.
Deadline1800s remote/1860s SSH. Each completed case saved /root immediately.
Refuse preexisting task directory/namespaces or other writer to fixed log. Never
replace/delete fixed log; create once/truncate/hardlink,verify inode after each.
Always cleanup owned namespace processes,partial results and owned namespaces;
remove remote task artifacts only after collection. Do not touch installed daemon,
its configuration/qdiscs, ubusd, user's tail or production devices. Preserve failed
smoke; after second harness/environment failure stop and reassess.

## Stop and reassessment
Two smoke failures retained (missing stat; daemon main-instance collision plus
kernel missing proc children). No acceptance batch run. Revised local harness:
unique nativecheck_* UCI section/lock/log basename; foreground startup diagnostic;
proc stat PPID child discovery; device+inode resolved through mountinfo for writers;
cleanup disables errexit and attempts archival despite diagnostic failure;
builtin Bash watchdog timer avoids missing children interface/sleep orphan.
Local syntax and live-proc/hardlink identity checks pass. Next task starts with
one affected smoke of this revised harness before the four-case batch.
