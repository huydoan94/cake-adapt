# Profiling and behavior evidence

Raw evidence from the OpenWrt 25.12 x86 test VM, kept with the code it
measured. Each directory has its own README with the method, the result, and
the scripts that produced it.

| Directory | What it shows |
| --- | --- |
| [`2026-10-03-arm64-vm`](2026-10-03-arm64-vm/README.md) | The Filogic build on an emulated arm64 VM: the arm64 verifier accepts the TCP filter and the CLI, attribution, fping-ts and lifecycle runs all pass. Functional only; the emulated CPU makes timings and sample counts unrepresentative. |
| [`2026-10-03-sample-thinning`](2026-10-03-sample-thinning/README.md) | Sampling TCP timestamps at most every 4 ms per flow makes the filter 10–34% cheaper with the queue estimate as accurate as before (correlation 0.87–0.95); over five rounds it costs about +10 ms at the bidirectional p99 and 1–3 ms in download phases. Shipped at 4 ms. |
| [`2026-10-03-fping-ts-testbed`](2026-10-03-fping-ts-testbed/README.md) | fping-ts on the testbed, where ICMP timestamps pass: its one-way delays track the real queues (upload correlation 0.94–0.97, false alarms ≤ 0.6% against 27–40% for RTT/2), and controlling with it cuts upload p95 from 102–104 to 60–65 ms and lifts bidirectional download from 38–41% to 69–72%. A sudden capacity drop peaked higher than with fping; internet reflectors remain unverified. |
| [`2026-10-03-ebpf-filter-cost`](2026-10-03-ebpf-filter-cost/README.md) | The TCP filter costs 1.5–1.9 µs per packet on the x86 VM (130 ns of it the kernel's): about 45% flow state, 45% the departures map, 10% the ring buffer. Counter changes and per-CPU LRU lists gain at most a few percent (LRU lists are 6% slower); only thinning the samples could cut it substantially. The kept filter still tracks the real queue (correlation 0.89–0.92). |
| [`2026-10-03-ebpf-design-history`](2026-10-03-ebpf-design-history/README.md) | Design record: why fping alone is insufficient, the feasibility spike and CPU cost that justified the eBPF socket filter, the fixed-cap attempt that preceded the dynamic ACK share, and the net before/after numbers. No raw data; links to the two entries below for evidence. |
| [`2026-10-03-ack-share-dynamic`](2026-10-03-ack-share-dynamic/README.md) | Download ACKs on a 900 kbit/s upload, off vs. `upload_ack_share_min 0.45`: ACKs took 46–74% and a 200 kbit/s UDP stream lost up to 48%; with the option, ACKs use what other upload traffic leaves, down to 45%, the stream loses 0–5.5%, and download gives up 4–12 Mbit/s while held. |
| [`2026-10-02-tcp-queue-split`](2026-10-02-tcp-queue-split/README.md) | fping alone vs. `tcp_delay_attribution`: the TCP-timestamp queue estimate follows the emulated ISP's real queue under control (correlation 0.85–0.95, against RTT/2's half-size, two-sided reading); splitting fping's delay by it took median added RTT under bidirectional load from 50 to 28 ms and download from 41% to 67% of capacity. |
| [`2026-09-30`](2026-09-30/README.md) | **Current.** End-to-end run and CPU profile after the optimization pass: flame graphs for five workloads, CPU before and after, syscall counts, and the qdisc lifecycle, idle, signal and shutdown checks. [Dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-09-30/index.html). |
| [`cake-autorate-resources`](cake-autorate-resources/README.md) | CPU, memory, process and reaction-time comparison with cake-autorate `ac75f49` under the same workload: about 5× less CPU (7.6× with all records enabled), a tenth of the memory, and a 7× faster median reply-to-decision time. |
| [`controller-comparison`](controller-comparison/README.md) | Side-by-side runs with cake-autorate `ac75f49` and the two traces the controller replays with no mismatching decisions. |
| [`2026-09-26`](2026-09-26/README.md) | Superseded first flame graphs, from before the optimization pass and the source restructure. |

Historical reports use `upload_ack_share_min`, now named
`upload_ack_congested_share`. Their raw configurations and records retain the
name used when the measurements were collected.

Open `index.html` locally, or use the
[hosted index](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/index.html).
The hosted links read the `main` branch. They use raw.githack because GitHub's
raw-file host disables the JavaScript inside FlameGraph SVGs that drives hover,
search, and click-to-zoom.
