# cake-adapt controlling cake_mq (2026-10-10)

## Result

cake-adapt now controls a root `cake_mq` (multi-queue CAKE, Linux 7.0,
backported in OpenWrt 25.12) like a single CAKE: it found both testbed roots,
made 901 bandwidth changes with no failure, and in every snapshot all four
per-queue children carried exactly the root's rate. Control was comparable to
plain CAKE in every phase but one: under bidirectional load `cake_mq` passed
only 3.3 Mbit/s of upload through a 6 Mbit/s shaper (plain CAKE about
7.3 Mbit/s), because `cake_mq` divides its rate equally among busy queues and
the upload stream shared it with the download's ACKs on another queue. That is
the kernel's design, not something cake-adapt can correct from outside.

## Setup

- OpenWrt 25.12 x86 VM (i686, kernel 6.12.108), `192.168.56.2`.
- The emulated testbed (`tools/testbed/testbed.sh`) with `CAKE_MQ=1`: `cwan`
  and `ifb4cwan` with 4 transmit queues and `cake_mq` on both; the comparison
  run used single-queue interfaces and plain `cake`. ISP 8 Mbit/s up,
  40 Mbit/s down (4 Mbit/s up during the capacity drop).
- The cake-adapt build with `cake_mq` support, run from an isolated directory
  (the installed package untouched), with `tcp_delay_attribution` on, through
  `tools/testbed/run.sh` ([`tools/mqrun.sh`](tools/mqrun.sh) ran both).
- During the `cake_mq` run the qdiscs were recorded every 20 s
  (`raw/mq-qdiscs-during.txt`).

## Control (`raw/analyze.txt`, from `tools/testbed/analyze.py`)

Added RTT in ms and share of capacity used:

| Phase | cake p95 | cake_mq p95 | cake used | cake_mq used |
| --- | ---: | ---: | ---: | ---: |
| upload steady | 72.4 | 71.9 | 85.6% | 84.7% |
| upload step | 92.4 | 89.0 | 84.1% | 82.5% |
| download steady | 36.5 | 34.6 | 80.4% | 77.9% |
| bidirectional | 68.5 | 32.5 | 79.9% up, 72.7% down | **33.6% up**, 81.5% down |

Bidirectional phase, after its first 10 s (`LOAD` records):

| | cake | cake_mq |
| --- | ---: | ---: |
| upload shaper | 5.1–11.1 Mbit/s (average 7.6) | 6.0 Mbit/s throughout |
| upload passed | 7.3 Mbit/s | 3.3 Mbit/s |
| shaper changes in the phase | 417 | 128 |

`cake_mq` (`net/sched/sch_cake.c`, OpenWrt backport
`700-05-v7.0-net-sched-sch_cake-share-shaper-state-across-sub-ins.patch`)
counts the sub-queues with traffic every 200 µs and gives each
`rate / active queues`. The download's ACKs kept a second upload queue active,
so the upload stream was held near half the rate; the unused part of the ACK
queue's share was not lent to it. cake-adapt saw the upload shaper underused
and had no reason to raise it. The lower delay in that phase follows from the
lower upload traffic.

## Limits

- One run per variant on the 32-bit x86 VM; the other phases differ within the
  run-to-run spread seen before.
- `veth` and IFB queue selection on the testbed is not a router's WAN driver;
  how evenly real traffic spreads over a Filogic WAN's queues, and whether one
  core can shape 300-500 Mbit/s there, were not measured.
- Raw logs: `raw/mq.tar.gz` and `raw/cake.tar.gz` (each run's results
  directory), with the qdisc snapshots and run output beside them.
