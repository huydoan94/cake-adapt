# CAKE byte accounting, 2026-10-05

Both outgoing and pure-ACK counters now use the upload CAKE's non-GSO packet
charge. The exact packaged x86 filter and production capture path match the
kernel's independently reported min/max adjusted packet sizes for RAW,
Ethernet, signed overhead, MPU, ATM and PTM. Plain ack-filter was enabled;
both injected packets reached the tap, with no ring loss and exactly two
program runs per pair. The installed service/package/configuration and root
qdisc options remained unchanged; protected log inode stayed 183.

Each pair has one 66-byte timestamp-sized pure ACK (no timestamp option) and
one 1514-byte data frame. The before object is unchanged from a0b52af; the implementation is bbca799.

| Model | Before ACK / total bytes | After ACK / total bytes | Kernel min / max charge |
| --- | ---: | ---: | ---: |
| RAW, overhead0 | 66 / 1580 | 66 / 1580 | 66 / 1514 |
| Ethernet, overhead0, MPU0 | 66 / 1580 | 52 / 1552 | 52 / 1500 |
| Ethernet, overhead44, MPU0 | 66 / 1580 | 96 / 1640 | 96 / 1544 |
| Ethernet, overhead0, MPU84 | 66 / 1580 | 84 / 1584 | 84 / 1500 |
| Ethernet, overhead-20, MPU0 | 66 / 1580 | 32 / 1512 | 32 / 1480 |
| ATM, overhead0, MPU0 | 66 / 1580 | 106 / 1802 | 106 / 1696 |
| PTM, overhead0, MPU0 | 66 / 1580 | 53 / 1577 | 53 / 1524 |

The single-tagged fallback passed on the kernel: charged ACK/total counters
stay zero and both packets increment unaccounted_packets. Native tests cover Ethernet IPv4/IPv6/ARP and IP on PPP/NONE/RAWIP links.
They call the actual filter with substituted packet/map helpers and cover GSO/RAW GSO,
VLAN metadata and 802.1Q/AD, unknown protocol/link/framing, RAW unknown protocol,
and disabled accounting. Monitor tests cover degraded intervals, read failure,
counter reset, clean recovery, fractional rate arithmetic, model changes and
same-handle qdisc recreation. These seams and the scalar accounting tests pass
ASan/UBSan (LeakSanitizer disabled because this environment uses ptrace).
Both SDK builds pass; the controller replay retains 4703 matching decisions.

## Partial batch and retained failures

The first candidate preflight was interpreted despite correct byte counts.
i386 JIT rejects BPF-to-BPF calls and 64-bit division. Forcing the small shared
helper inline and retaining CAKE's 32-bit charge arithmetic fixed this: the
candidate-only retry has 8483 JIT bytes versus 6701 for before. The counter map
value grows from 24 to 32 bytes per possible CPU; a one-entry 24-byte config
map is added. These are ABI/capacity sizes, not total allocated memory.
The x86 packaged executable grows 4096 bytes (102405 to 106501); Filogic stays
98313 bytes. Two-packet run times are retained but establish no CPU-cost claim.

The first batch kernel check caught tc's Ethernet preset selecting MPU84;
explicit MPU0 fixed the intended test setup. The final retry then stopped at
the double-tagged data frame: the veth MTU rejected it with Message too large.
No more VM tests were run. QinQ, unknown-protocol, nonsplit-GSO and disabled
accounting kernel cases remain unverified; their relevant fallback branches
are covered locally. No generic offload parser was added. The harness
now sets MTU1600 for both isolated veth endpoints; that correction has not been
rerun. `raw/run-as-executed.sh` preserves the tested version.

This verifies supported packet charges and a fallback, not controlled download
headroom, throughput, internet reflectors, arm64 runtime, or scheduler/tuple
lifetime safety. Tuple-lifetime finding 2 remains deferred. The complete extended
batch is **partial**, not a passing acceptance campaign.

[PLAN.md](PLAN.md) records the limits, diagnosis and stopping decisions.
[tools/probe.c](tools/probe.c) uses the production capture for after and safely
reads the old counter layout for before. [tools/run.sh](tools/run.sh) preserves
VM state and removes namespace-owned processes on interruption. Raw results,
failed attempts, state snapshots, hashes and target/host outputs are in `raw/`.
Reproduction uses the current x86 SDK compiler, staged headers/libraries,
`capture.c`, `estimator.c` and `common/error.c`, with strict warnings and libbpf.
Use the exact packaged object and the documented bounded run plan.

The lifecycle source chart was updated with the capture reset; rendered flowchart
indexes remain user-managed.
