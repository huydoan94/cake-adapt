# Upload estimates when servers delay their echo (2026-10-08)

**Result:** the TCP upload estimate was not fooled by servers that idle or
think before answering. On every phase with an empty real upload queue it
stayed below 12 ms at p99 with at most 0.3% false alarms, and real upload
queues were detected 88-100% of the time. A cap on echo waits was therefore
not built.

**What this is not:** a test against servers other than Linux, which ACK a
request promptly and answer later; a server that holds its ACK until the
answer is untested.

## Why

On a 0.3.8 router, upload estimates of 20-347 ms came with fping deltas of
9-17 ms. The suspicion was server think time read as upload queue; the
likelier explanation is real delay on the overseas paths those flows cross,
which local reflectors do not see (see `2026-10-08-remote-congestion`).

## Method (`tools/testbed/echo.sh`, `tcpthink.c`)

Shapers fixed above the bottleneck, so every queue forms at the emulated ISP
and its 100 ms backlog samples are the truth for `queues.py`. Installed
`3e23b04` build (the estimator is unchanged since). Runs: `echo-base-1`
(app-limited downloads and bulk), `echo-base-2` (plus constant think times,
of which 50 and 200 ms idled the daemon), `echo-base-3` (random 0-50 and
0-200 ms think times with a steady 5 Mbit/s download), `echo-think-1` (think
phases alone, `connection_active_thr_kbps` 50).

## Results (`summary.txt`)

| Phase, true upload queue 0 | Upload estimate p50/p90/p99 ms | False alarms |
| --- | --- | --- |
| download bursts every 100 / 500 ms | 1.2-2.6 / 3.9-7.9 / 5.9-9.9 | 0% |
| constant 20 ms think | 0.6 / 1.7 / 2.9 | 0% |
| random 0-50 ms think | 0.0-1.5 / 2.1-4.7 / 3.1-11.0 | 0-0.3% |
| random 0-200 ms think | 0.5-0.6 / 1.6-2.5 / 2.2-9.3 | 0% |

A constant think time becomes part of the flow's floor and reads as zero.
