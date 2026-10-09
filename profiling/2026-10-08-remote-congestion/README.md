# Congestion beyond the ISP (2026-10-08)

**Result:** a queue on a path only some flows cross (as an overseas path,
with reflectors near the ISP) does not mislead the daemon. Remote congestion
alone flagged no bufferbloat and left both shapers at base. Mixed with a real
local queue, `tcp_delay_attribution` blamed the right direction, while with it
off the daemon cut download for an upload queue.

**What this is not:** a model of a congested shared ISP segment, which the
reflectors also see; that case is untested.

## Method (`tools/testbed/remote.sh`, `remote.py`)

A remote bottleneck in `inet` (4 Mbit/s up, 20 Mbit/s down, 300 ms buffers)
carries only ports 5203 and 5204; reflectors, the probe and the local servers
bypass it. The daemon (installed `3e23b04` build) controls both shapers.
Four runs alternate `tcp_delay_attribution` off, on, on, off.

## Results (`summary.txt`, `raw/`)

| Phase | Attribution | Added RTT p50/p95 ms | Flagged ul/dl | Shaper ul/dl Mbit/s | Throughput up/down |
| --- | --- | --- | --- | --- | --- |
| remote only | either | 1-3 / 5 | 0% / 0% | at base | the remote limit |
| local download + remote upload | off | 28-29 / 84 | 10-11% / 11-13% | 6.5 / 51 | 3.8 / 35.8 |
| | on | 21-24 / 38-39 | 0% / 18-27% | 6.8 / 51 | 3.8 / 36.3 |
| local upload + remote download | off | 42-47 / 120-122 | 37% / 39% | 8.0 / 20 | 7.0 / 13 |
| | on | 4 / 83-88 | 23-27% / 0% | 7.3 / 34-39 | 6.5 / 19.1 |
| local both ways | off | 38-54 / 110-158 | 36-46% / 36-47% | 8.0 / 13.6 | 7.0 / 8.9 |
| | on | 28-31 / 80-84 | 28-33% / 8-12% | 7.7 / 48 | 6.7 / 29.5 |

The fping bound keeps a remote TCP queue (210-250 ms) from outweighing a real
local one: with a remote upload queue and a local download queue, attribution
flagged only download.
