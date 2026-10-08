# eBPF A/B check of the injection fix (2026-10-08)

**Result:** B (`146a751`: injected TSval 1, stall detection, pause) matches A
(`f2e68e1`, the build the routers ran, same code as the installed `3e23b04`)
in every eBPF behaviour the testbed covers, costs no more, and differs only
where intended: client clocks that look older than the injected value now
stall visibly and pause injection instead of hanging silently.

**What this is not:** a router timing. Costs are from the 32-bit x86 VM
(`192.168.56.2`, kernel 6.12) with the kernel's BPF statistics on, for
comparing builds only.

## Method

Each variant ran its own daemon with its own BPF object (the counters layout
changed), in blocks alternating A B B A, with the VM's service stopped
(`tools/ebpf-campaign.sh`, `ebpf-campaign2.sh`). The first campaign filled
the VM's root filesystem during its ACK block, which silently emptied the
later results (`acks-A-144315` lost, `acks-B-143844` partial, both dropped);
its cost blocks also overlapped runs on the other VM on the same host. The
second campaign (results kept in RAM) completed the ACK block, reran
injection and clocks, and repeated both cost blocks twice over with nothing
else running. ACK runs' raw packet captures are not kept. 64-bit and IPv6
checks ran on `192.168.56.3` (x86_64 snapshot, kernel 6.18,
`tools/v6.sh`, `v6-csum.sh`).

## Results

| Area | A | B |
| --- | --- | --- |
| Verifier, 32-bit 6.12 and 64-bit 6.18 | accepted | accepted (filter 6,424 against 6,016 B translated) |
| Filter, injection off, `bench.sh` (`campaign2/bench-*`) | 1,281-1,975 ns, mean 1,591 | 1,387-1,548 ns, mean 1,447 |
| Filter, injection on (`campaign2/cost-*`) | 907-1,177 ns, mean 1,029 | 899-976 ns, mean 943 |
| Injector, injection on | 378-499 ns, mean 447 | 414-443 ns, mean 430 |
| Control, `run.sh` (`campaign1/run-*`) | added RTT and capacity within B's range | within A's run-to-run spread in all phases |
| TCP queue accuracy (`queues.py`) | correlation 0.83-0.92, false alarms ~0% | 0.77-0.91, ~0% |
| ACK accounting, `acks.sh` 0.45 (`campaign1/acks-A-142941`, `acks-B-143413`, `campaign2/acks-*`) | ACK share 49.5-61.5%, download held 13.7-27 Mbit/s | 48.6-61.5%, 13.4-27 Mbit/s |
| `inject.sh`, 32-bit (`campaign2/inject-*`) | as designed | identical outcomes and counters, `stalled=0` |
| `inject.sh`, 64-bit and IPv6 (`x86_64-ipv6/`) | - | identical over IPv4 and IPv6; offload off: 22/22 SYN checksums correct, `InCsumErrors` 0 |
| Young client clock (`clock.sh` 2) | 40/40 ok | 40/40 ok on 32-bit, 64-bit and IPv6 |
| Random client clocks (`clock.sh` 1) | 19 of 40 hung silently (`2026-10-08-tcp-inject-clock`) | 7-9 failed in the first minute, then paused; round 2 all ok |

The first campaign's cost blocks (`campaign1/bench-*`, `cost-*`) showed B up
to 25% slower; they overlapped the other VM's runs, and the clean repeat does
not reproduce it.
