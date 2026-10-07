# cake-adapt on an OpenWrt snapshot (2026-10-07)

**Result:** the current code (`b28b1ee`, packaged as `0.3.7-r1` because the
0.3.8 bump is only on `main`) builds without warnings in the x86/64 snapshot
SDK and runs on a snapshot VM with no change. The 6.18 kernel's verifier
accepts the TCP filter built with LLVM 22, all 33 lifecycle checks pass, and a
controlled testbed run with TCP attribution behaves as on 25.12. No run logged
an error or warning.

**What this is not:** a performance comparison. It is one run, with slightly
tighter upload bounds than the 25.12 reference, and the filter's cost and the
daemon's CPU were not measured. A 32-bit (x86/generic) snapshot build also
compiled cleanly but was not run.

## Environment (`raw/environment.txt`, `raw/build.txt`)

- VM `192.168.56.3`: OpenWrt SNAPSHOT `r36953-0ab51fb9c1`, `x86/64`, kernel
  6.18.55. Newer than 25.12: libbpf 1.7.0, libubox 2026.07.21, sqm-scripts
  1.8.0, iperf3 3.22.
- SDK `openwrt-sdk-x86-64_gcc-14.4.0_musl` from the same snapshot: gcc 14.4.0,
  llvm-bpf 22.1.3. Package SHA-256 `35bbaae2…f45366`.
- SQM `layer_cake.qos` on `eth1` at 30/30 Mbit/s; the cake-adapt
  configuration of the 25.12 test VM (internet reflectors), with
  `tcp_delay_attribution` on.

## Results

- **Lifecycle** (`raw/lifecycle/`, script `tools/lifecycle.sh`): 33 of 33.
  CLI listing and validation, stop/start/restart with one `fping`, procd
  respawn after SIGKILL, the download CAKE deleted and re-created exactly,
  an SQM restart, SIGUSR1 export, SIGUSR2 reset and size rotation keeping the
  inode, disabled and invalid configurations refused and reported in syslog.
- **Controlled run** (`raw/fping-tcp/`; `tools/testbed/run.sh` with
  `UL_CAP=2 DL_CAP=30 UL_STEP=1`, upload 1000/1800/2400 kbit/s, download
  5000/27000/33000 kbit/s, `ACTIVE_THR=500`, `ACK_FILTER=1`,
  `TCP_ATTRIBUTION=1`, `BACKLOG=1`): daemon exit 0, nothing left running, no
  probe loss.

| | snapshot | 25.12 reference |
| --- | --- | --- |
| TCP queue estimate vs the ISP backlog, correlation | 0.77–0.92 | 0.74–0.89 |
| TCP queue false alarms | ≤ 0.2% | ≤ 0.2% |
| added RTT p95, download / bidirectional | 38 / 68 ms | 37 / 75 ms |
| capacity used, download / upload | 84% / 83–88% | 84% / 81–89% |

Per phase: `raw/latency.txt` and `raw/queues.txt`, from `analyze.py` and
`queues.py`. The reference (`raw/*-25.12-reference.txt`) is the 0.3.8 run of
2026-10-07 on the 25.12 x86 VM with upload bounds 1000/2000/3000 kbit/s; the
snapshot's better upload-step numbers follow from its lower upload maximum,
not from the platform.
