# Native ICMP spike: functional paths and revised timing gates pass

The historical batches below missed the original gate. The revised gate is
now met by the quiet-host loopback check (`wsl-loopback/`), serialized VM
loopback diagnosis (`vm-loopback-diag/`) and [interleaved network batch](interleaved-gate/README.md):
65-µs median gap within a 150-µs baseline spread, 300 replies per cohort.
This does not establish controller equivalence or CPU savings.

2026-10-13, OpenWrt 25.12 x86 VM, i686 kernel 6.12.108. Native backend remains
experimental and fping remains the default. No control comparison, CPU saving,
throughput equivalence, default switch or router behavior is established here.

The isolated testbed uses `cpe`, `isp` and `inet` namespaces, IPv4 10.99.0.11
and IPv6 fd99::11, 10-ms netem delay in each direction, and quiet CAKE shapers.
The installed controller continued running against its own eth1/IFB qdiscs;
no second controller was started on those qdiscs. Each case sent 30 packets at
100-ms intervals. Native raw sockets were bound to the selected interface,
and every native reply carried a valid fixed-width kernel receive timestamp.

| Batch / target | Native median RTT (µs) | fping median RTT (µs) | Absolute gap (µs) |
| --- | ---: | ---: | ---: |
| Initial sequential IPv4 | 22329.5 | 23550 | 1220.5 |
| Initial sequential IPv6 | 21867.5 | 22350 | 482.5 |
| Initial sequential ICMP timestamp | 21794 | 22350 | 556 |
| Concurrent paired IPv4 | 21833.5 | 21200 | 633.5 |
| Concurrent paired loopback echo | 113.5 | 34.5 | 79 |
| Concurrent paired loopback timestamp | 106 | 26 | 80 |
| Send-boundary-fixed paired IPv4 | 22844.5 | 22150 | 694.5 |
| Send-boundary-fixed loopback echo | 130.5 | 26.5 | 104 |
| Send-boundary-fixed loopback timestamp | 99.5 | 25 | 74.5 |

All nine cases received 30/30 native and fping replies; all 270 native samples
had kernel timestamps. The initial sequential comparison missed the plan's
100-µs median-gap gate. One paired diagnosis removed sequential run differences
and isolated loopback clock behavior. Static diagnosis found native's send
timestamp preceded packet preparation; it now reads monotonic time after
preparation immediately before `sendto()`. A regression test proves RTT excludes
a simulated 2-ms preparation delay and preserves send spacing. The affected
spike was repeated once for that changed input and still missed the gate.
Further VM runs stopped; no threshold was relaxed.

Network RTT ranges were roughly 20–59 ms across the batches. The sample count,
VM scheduler and different probe send times do not identify the source of the
remaining gap or establish a statistically repeatable backend regression.
Functional socket/packet/ABI checks are successful; measurement equivalence is
not accepted. A bounded diagnostic must separate clock/packet-path bias from VM
timing variability before control comparison or migration. Do not rerun until
this changes the plan or input.

Astra Medium also found that arming a relative timer with the pre-send clock
would count preparation delay again. Daemon and driver now refresh monotonic
time before arming their waits; a monitor seam checks 50 ms remaining after
2 ms preparation. That final scheduling fix is unit/target-tested and reviewed,
not included in the last runtime batch above. It does not turn that batch into
an acceptance pass.

Raw output and JSON summaries are in `initial/`, `paired/` and `sendtime/`.
The initial environment file records kernel/boot and executable checksums.
`spike.sh`, `paired-sendtime.sh` and `testbed.sh` reproduce the collected cases.
For the intermediate paired script change the output directory names back;
the probe commands are identical. Legacy SCP initially refused a source path
ending in `/.`; copying the named directory fixed collection without reruns.

No package was installed on the VM, no service or ubusd was stopped, and the
authorized daemon test-log path was not opened by these standalone probes.
Owned namespaces and temporary payloads were removed after collection.
`cleanup.log` records remaining processes and the installed executable checksum.
