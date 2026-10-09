# TCP filter fast path, and the first router cost (2026-10-08)

**Result:** an uncommitted fast path for the TCP filter was measured against
the committed filter (`f2e68e1`, 0.3.9) on both x86 test VMs and on the
user's Filogic router. Nowhere was it faster, so it was dropped:

- 32-bit VM: 3–7% more filter time per packet in all four cases;
- x86_64 VM: 5% more with 4 flows, too noisy to read with 1 flow;
- router: the filter dropped 7%, but the injector, whose per-packet path the
  change does not touch, dropped 9% in the same windows. Normalized by the
  injector, the fast path measured 2% slower, within noise.

The router runs also give the first real-hardware cost of the committed
build: **about 2.0 µs per packet for the TCP filter and 0.37 µs for the
injector**, about 1.1% of one Cortex-A53 core at 5,500 packets per second.

**What this is not:** an accuracy result. Every figure is measured with the
kernel's BPF statistics on, which add two clock reads per run. The VM figures
compare builds under a fixed `iperf3` load; the router figures come from the
user's own traffic, one 60-second window per run.

## The fast path

Three changes to `tcpdelay.bpf.c` (the user's, not committed):

1. a per-CPU cache of 64 slots per direction holding the flow, TSval and
   TSecr, checked before the shared flow map, so a packet repeating them skips
   the shared lookup and update;
2. a 12-byte read of the option area, accepting NOP, NOP, timestamp directly
   and passing any other layout to the full parser;
3. no zeroing of the 40-byte options buffer, with a new check that the parser
   never reads a kind's length byte past the loaded bytes.

The verifier accepted it on the 6.12 i386, 6.18 x86_64 and Filogic kernels.
Running as root, the kernel lets programs read uninitialized stack, so
acceptance alone does not prove that only loaded bytes are read; the added
bound check does that by inspection. Object checksums are in
`raw/objects.sha256` (`base.o` committed, `new.o` the fast path; both built
with the x86 SDK's BPF clang, used unchanged on both VMs).

## 32-bit VM, kernel 6.12 (`raw/vm2-i386/`)

Only the BPF object is swapped (`tools/campaign.sh`), the service is stopped,
and each case alternates committed, fast path, fast path, committed. Each run
is 20 s of downloads then 20 s of uploads (`tools/cost-flows.sh`, injection
on). Filter time per run:

| Client timestamps | Flows | Committed | Fast path | Difference |
| --- | --- | --- | --- | --- |
| on | 4 | 1,941 ns | 2,039 ns | +98 ns, +5% |
| on | 1 | 1,676 ns | 1,751 ns | +75 ns, +4% |
| off | 4 | 1,202 ns | 1,240 ns | +38 ns, +3% |
| off | 1 | 1,140 ns | 1,225 ns | +85 ns, +7% |

Each difference is smaller than the spread between repeat runs of one build,
but all four point the same way. Throughput was the same (about 36 Mbit/s
down, 7 Mbit/s up); `TCP_QUEUE` records per run were 953–981 for both.

## x86_64 VM, snapshot kernel 6.18 (`raw/vm3-x86_64/`)

The same objects and method, client timestamps on only, with
`tools/progstats.c` in place of `bpftool`, which the VM lacks:

| Flows | Committed | Fast path | Difference |
| --- | --- | --- | --- |
| 4 | 1,685 ns (1,736 / 1,633) | 1,770 ns (1,750 / 1,792) | +85 ns, +5% |
| 1 | 1,335 ns (1,464 / 1,241) | 1,355 ns (1,329 / 1,377) | +20 ns, within noise |

With 1 flow, download throughput varied from 13.6 to 26 Mbit/s between runs
and the committed runs alone differ by 223 ns. The 4-flow result matches the
32-bit VM, so the slowdown is not only the i386 JIT.

## Filogic router (`raw/router/`)

`tools/bpfcost.sh 60` on the router, under its normal traffic. It reads the
counters from the daemon's program descriptors in `/proc`, because
`bpftool-full` printed nothing there (`bpftool-minimal` works):

| Run | Build | Filter | Injector | Filter ÷ injector |
| --- | --- | --- | --- | --- |
| 1 | committed | 2,018 ns | 371 ns | 5.44 |
| 2 | committed | 2,004 ns | 377 ns | 5.32 |
| 3 | fast path | 1,817 ns | 336 ns | 5.41 |
| 4 | fast path | 1,923 ns | 346 ns | 5.56 |

The two committed runs agree within 1%. The injector checks each packet's
headers in place and reaches the option parser only for SYNs, so it serves as
a control: both programs got cheaper together, and the fast-path runs came
straight after an install and restart, the committed ones after the daemon
had been running for a while. The runs were not alternated back to back, and
the fast-path build's map count was not checked on the router.

## Bufferbloat test on the committed build (`raw/router/`)

An online bufferbloat test at about 02:01 graded A+: +4.2 ms added under
download (121.8 Mbit/s), +0.0 ms under upload (42.7 Mbit/s) and +11.0 ms
bidirectional (`bufferbloat-test.png`). In the log (`log-01.56-02.04.txt`)
both shapers rose to their maximums, 145 and 73 Mbit/s, and returned to
90 and 50 Mbit/s afterwards. Injection had enabled timestamps on 96
connections by 02:04 (86 accepted, 10 declined, no rejection or failure).
One test, at night when the ISP is least loaded, and not compared against
injection off; it is not evidence for any one change.
