# Emulated bloated-ISP testbed

Network namespaces on an OpenWrt VM that reproduce real bufferbloat with real
TCP, isolated from the VM's own WAN and SQM:

```text
[cpe]  cwan: CAKE egress (upload); ingress -> ifb4cwan CAKE (download)
  | veth
[isp]  netem 10 ms base delay + tbf bottleneck with a 500 ms buffer, each way
  | veth
[inet] iperf3 servers (5201, 5202), reflectors 10.99.0.11-16, probe 10.99.0.20
```

The image needs `kmod-veth`, `kmod-netem`, `iperf3`, `tcpdump` and `ip-full`.
Copy the scripts to `/tmp/cake-adapt-test/` on the VM.

| Script | Purpose |
| --- | --- |
| `testbed.sh up \| down \| rate UL DL` | Create or remove the namespaces; change the bottleneck rates (Mbit/s) without flushing the queue. Defaults are 8 Mbit/s up and 40 Mbit/s down. |
| `run.sh NAME BINARY PINGER UP THR DOWN` | One cake-adapt variant in `cpe`: idle, steady upload, upload while capacity drops, steady download, then both directions, with an independent 100 ms `fping` probe and `iperf3` JSON. The test log follows the VM rules. The line comes from the environment (defaults in brackets): bottleneck `UL_CAP` [8], `DL_CAP` [40] and `UL_STEP` [4] in Mbit/s; shaper bounds `UL_MIN`/`UL_BASE`/`UL_MAX` [2000/6000/12000] and `DL_MIN`/`DL_BASE`/`DL_MAX` [10000/30000/60000] in kbit/s; `ACTIVE_THR` [2000] for `connection_active_thr_kbps`; `TCP_ATTRIBUTION` [0] and `ACK_FILTER` [0]; `BACKLOG=1` samples the emulated ISP's queues every 100 ms. |
| `analyze.py RESULTS` | Added RTT percentiles, time above 15/30 ms and capacity used per phase. |
| `queues.py RUN` | Scores a run's `TCP_QUEUE` estimates and fping's RTT/2 delta against the sampled ISP backlog (needs `BACKLOG=1` and `output_processing_stats`): percentiles, correlation, detection and false alarms per phase and direction. |
| `acks.sh MODE [NAME]` | CAKE just below a 1 up / 30 down Mbit/s bottleneck with ack-filter `MODE` (`no-ack-filter` or `ack-filter`); 1, 4 and 8 downloads, with and without an upload, while capturing upload after CAKE and probing ICMP and 64 and 200 kbit/s UDP. Starts the `udpping` echo in `inet` if needed. With `DAEMON=<binary>` the daemon controls download (upload fixed) with `upload_ack_congested_share` `ACK_SHARE`, following the test-log rules. |
| `acks.py RUN` | ACK share of upload, probe latency and loss, goodput, ISP backlog and (with the daemon) the download shaper per phase of an `acks.sh` run. |
| `bench.sh NAME BINARY OBJECT` | The TCP filter's own cost: installs `OBJECT` as `/lib/bpf/cake-adapt-tcpdelay.o`, runs the daemon with `tcp_delay_attribution` and `upload_ack_congested_share` under 60 s of 4 downloads and 1 upload, and reports the filter's runs and nanoseconds per run from the kernel's BPF statistics (enable `kernel.bpf_stats_enabled` first) and the daemon's CPU ticks. `ADJUST=0` keeps the shapers fixed so filter variants see the same traffic. Follows the test-log rules. |
| `udpping.c` | Constant-rate UDP echo client and server (`udpping -s PORT`) for VoIP-like latency and loss; build with the OpenWrt toolchain. |
| `shapers.py RESULTS` | Per-phase shaper and achieved rates, time with the upload shaper above the bottleneck, and shaper cuts, from the daemon's LOAD and SHAPER records. |

Run background processes with `ip netns exec` directly rather than through a
shell function, so `$!` is the real process and stopping it works.

Results made with these scripts are under [`profiling/`](../../profiling/README.md).
