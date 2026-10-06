# fping-ts on a network that passes ICMP timestamps (2026-10-03)

**Result:** on the namespace testbed, where ICMP timestamp requests reach the
reflectors and come back, `pinger_method 'fping-ts'` measures each direction's
queue separately and correctly. Against the emulated ISP's real backlog its
upload delay correlates 0.94–0.97 and its download delay 0.73–0.93, it detects
every queue above 30 ms, and it reports delay on an empty direction in at most
0.6% of samples; fping's RTT/2 does that 27–40% of the time. Controlling with
fping-ts instead of fping (both without `tcp_delay_attribution`) cut upload
p95 added RTT from 102–104 to 60–65 ms, bidirectional p95 from 90–99 to 57 ms,
and raised bidirectional download from 38–41% to 69–72% of capacity, with no
wrong-direction download cuts during upload-only load (0 against 27–40).

One phase was worse: when the emulated upload capacity halves mid-transfer,
fping-ts's p99 added RTT was 312 and 448 ms against 170 and 214 ms. The
controller reacted as fast (first upload cut 0.27–0.30 s after the drop
against 0.30–0.37 s, then the same sequence of maximum cuts) and from a lower
shaper rate and a smaller standing queue, yet the real queue peaked about
twice as high (368–489 ms against 183–222 ms at 4 Mbit/s). The extra queue
forms before any controller could act; it is probably how the TCP sender
behaves when CAKE rather than the ISP was the bottleneck going in, but two
runs each cannot establish that.

**What this does not verify:** internet reflectors. Their clocks are not
synchronized with ours, some answer with non-standard timestamps, and many
networks drop ICMP timestamps (the VM's VirtualBox NAT drops them for every
internet host). The parser's handling of unsynchronized clocks and midnight
rollover was checked earlier against a real host (192.168.56.1); shaping on
internet one-way delays remains unverified, so fping-ts stays short of
production support.

## Setup

OpenWrt 25.12 x86 VM, the `tools/testbed` namespaces (8 up / 40 down Mbit/s
bottleneck with a 500 ms buffer, 10 ms netem each way, upload capacity
dropping to 4 Mbit/s for 20 s in the upload-step phase). All namespaces share
one kernel clock, so ICMP timestamp one-way delays are exact here.
`raw/icmp-timestamp-check.txt` shows a reflector answering through the
emulated ISP (Receive − Originate = 10 ms, Localreceive − Transmit = 11 ms).

Two alternating rounds of `run.sh` with `BACKLOG=1`: fping-ts, then fping,
both with default thresholds 30/15/60 ms and no TCP attribution (it requires
fping). x86 `cake-adapt` `2533a1a162f8d41b…`. The test log kept inode 140.

## Files

- `raw/T<round>-<fping-ts|fping>/`: probe, backlog samples, iperf3 JSON,
  phase marks, daemon log (`cake-adapt.log.gz`).
- `raw/T*-queues.txt`: `queues.py` per run; its last table scores the
  pinger's per-direction delta from the DATA records (fping-ts's own one-way
  delays, or fping's RTT/2).
- `raw/latency.txt`, `raw/shapers.txt`: `analyze.py` and `shapers.py`.

## Reproduce

```sh
./testbed.sh up
BACKLOG=1 sh ./run.sh T1-fping-ts ./cake-adapt fping-ts 30 15 60
BACKLOG=1 sh ./run.sh T1-fping ./cake-adapt fping 30 15 60
./testbed.sh down
python3 queues.py results/T1-fping-ts
```
