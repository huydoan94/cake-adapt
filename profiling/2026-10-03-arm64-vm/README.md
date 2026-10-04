# The Filogic build on an emulated arm64 VM (2026-10-03)

**Result:** the Filogic SDK build (Cortex-A53) runs on an OpenWrt 25.12.5
`armsr/armv8` VM, the arm64 kernel's verifier accepts the TCP filter
(including 4 ms sample thinning), and the full testbed set passes: CLI exit
codes, a controlled fping run with TCP attribution, an fping-ts run, and the
lifecycle run (idle to IDLE and back, both CAKE qdiscs deleted and
re-created, a scheduled reflector replacement, SIGTERM). No run logged an
error.

**What this is not:** a performance or real-hardware result. The VM is a
QEMU `dummy-virt` machine whose arm64 CPU appears to be emulated on an x86
host: the filter took 17 µs per packet (1.5–2.9 µs on the x86 VM) and the
daemon about 5% of a core. Under that load the runs produced about a third
of the x86 runs' samples, so accuracy here is a functional check only. The
package was not installed (its architecture, `aarch64_cortex-a53`, differs
from the VM's `aarch64_generic`); the binary and filter ran in isolation.

## Results

- **TCP queue estimate** (`raw/A-fping-queues.txt`): upload correlation
  0.96–0.98 with the real ISP queue, as on x86; download 0.75 alone and
  0.64–0.80 under bidirectional load, weaker than x86's 0.87–0.95, with 97–172
  samples per phase against about 520 on x86.
- **fping-ts** (`raw/A-fping-ts-queues.txt`): one-way delays correlate
  0.78–0.97 upload and 0.63–0.89 download, false alarms at most 4.6%.
- **Control** (`raw/latency.txt`): fping with attribution kept upload p95 at
  73 ms and download at 91% of capacity alone and 74% under bidirectional
  load; no losses.
- **Lifecycle** (`raw/A-life/`): every path taken once, as on x86, with the
  same warnings as the x86 lifecycle runs. One of them, `TCP delay records
  dropped`, comes from the IDLE window: the ring buffer is drained only when
  the controller runs, so records pile up while pingers are stopped. It
  dropped 641 records here, against 3,926 on x86 without thinning; the
  dropped records are stale by the time pingers resume.

## Setup

OpenWrt 25.12.5 `armsr/armv8`, kernel 6.12.94, 4 Cortex-A53 cores (as
reported), 480 MB. Packages installed for the test are listed in
`raw/packages-after.txt` against `raw/packages-before.txt`. Builds from the
Filogic SDK: `cake-adapt` `cfaf6c0d28320dfe…`, filter `fc6a360fe2c5f394…`
(source of `9467d38`). Test log inode 125.

## Files

- `raw/progress.txt`: the driver's log, including CLI checks and the cost
  benchmark's summary line.
- `raw/A-fping/`, `raw/A-fping-ts/`, `raw/A-life/`: the runs' probe, backlog,
  iperf3 JSON, phases and daemon log (`cake-adapt.log.gz`).
- `raw/*-queues.txt`, `raw/latency.txt`: `queues.py` and `analyze.py`.
