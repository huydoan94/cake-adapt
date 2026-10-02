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
| `run.sh NAME BINARY PINGER UP THR DOWN [DRAIN]` | One cake-adapt variant in `cpe`: idle, steady upload, upload while capacity drops to 4 Mbit/s, then both directions, with an independent 100 ms `fping` probe and `iperf3` JSON. The test log follows the VM rules. |
| `all.sh` | The six-variant comparison (`fping` and `fping-ts`, before and after the drain, default and 5/20/50 ms thresholds). |
| `analyze.py RESULTS` | Added RTT percentiles, time above 15/30 ms and capacity used per phase. |
| `capture.sh` | Fixed shapers that fill a known queue (upstream, downstream, both, none) while capturing TCP headers on `cwan` and sampling the real ISP backlog every 100 ms; input for `tools/tcp-timestamps/estimate.py`. |

Run background processes with `ip netns exec` directly rather than through a
shell function, so `$!` is the real process and stopping it works.

## Results (2026-10-02, one run per variant)

Upload added delay p99 / time above 30 ms / upload capacity used:

| Phase | cake-autorate behavior (`fping`) | drain + attribution + 5/20/50 (`fping`) | cake-autorate behavior (`fping-ts`) | drain + 5/20/50 (`fping-ts`) |
| --- | --- | --- | --- | --- |
| upload, steady | 131 ms / 46% / 91% | 81 ms / 27% / 90% | 91 ms / 25% / 87% | 47 ms / 11% / 87% |
| upload, capacity halves | 174 ms / 45% / 87% | 95 ms / 24% / 87% | 310 ms / 26% / 85% | 60 ms / 12% / 84% |
| both directions | 140 ms / 48% / 88% | 80 ms / 27% / 87% | 88 ms / 45% / 84% | 45 ms / 12% / 83% |

With both directions loaded, download got only 19–26% of its capacity with
`fping` in every variant, against 71–77% with `fping-ts`. Upload bloat delays
download's ACKs, which RTT/2 cannot tell apart from download bloat. That is the
gap the TCP-timestamp attribution in `tools/tcp-timestamps/` addresses.
