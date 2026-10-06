# TCP baseline and directional confidence mitigations

**Update 2026-10-05:** the directional agreement rule described here was
reverted in `d6564a6` after it caused the control regression in
[`2026-10-05-ack-control`](../2026-10-05-ack-control/README.md); see
[`2026-10-05-gpt-work-check`](../2026-10-05-gpt-work-check/README.md). The
HOLD/FOLLOW baseline policy remains in production.

Before: current-branch commit `518df81`. After: program commit `0321661`.
Tuple-lifetime finding 2 remains deferred; BPF source, layouts, maps and sampling
are unchanged. These changes affect the opt-in TCP attribution extension, whose
controller originates in [cake-autorate](https://github.com/lynxthecat/cake-autorate).

## Policy and deterministic comparison

The estimator holds its lowest floor unless a fresh independent fping observation
has less than 5 ms of added RTT. A traffic tick without that observation holds
the floor. Clear fping permits the existing two-bucket upward adaptation; lower
raw minima remain acceptable in either policy. This prevents an established
standing queue from aging into the baseline while fping still sees congestion.

The controller accepts a TCP proportional RTT split only when both quarter-share
direction classifications agree with download-delivery attribution. A conflict
keeps the original attribution and RTT/2. It does not identify receiver wait:
equal receiver wait and upload queue produce indistinguishable timestamps.

| Deterministic case | Before | After |
| --- | --- | --- |
| Empty-path calibration, then 80/20 ms through 70 s | Valid 0/0 ms | Valid 80/20 ms under HOLD |
| Partial loaded download, false 0/40 ms TCP pair, independent fping congestion | Download rises to 8.32 Mbit/s; upload cuts to 6 | Download cuts to 6 Mbit/s; upload stays at 8 |
| Estimator state on the 64-bit host | 13,056 bytes | 13,064 bytes |

The two probes reuse the existing test generators. Compile each against either
the archived HEAD source/tests or the current tree, using C11 and the project's
strict warning flags. `probe-estimator.c` links estimator.c;
`probe-controller.c` links controller.c and common/helpers.c with
`-Wl,--wrap=calloc`. Add both source and test roots to the include path. Raw
before/after outputs are the four `host-*.txt` files here.

Focused estimator/controller tests and all 4,703 upstream replay decisions pass.
ASan/UBSan pass, with leak detection disabled because of the known host ptrace
restriction. The final added strict-lower-floor regression also passes under
sanitizers. Both SDK package clean/compile tasks pass; no version was changed.
The selected VM executable and filter are extracted from the x86 APK, with
transfer identity checked by hashes. Before was compiled from a `git archive`
of `518df81` in an ignored build directory, using the same staged runtime libraries.
Packaged executable sizes are 102,405 bytes on x86 and 98,313 bytes on Filogic;
the before executable uses different build flags, so these are not size deltas.

## Bounded VM protocol

[`PLAN.md`](PLAN.md) records the planned cases and stopping conditions.
[`run-vm.sh`](run-vm.sh) runs the old and proposed daemons sequentially in isolated
testbed namespaces, with rate adjustment disabled: five seconds warmup, 70 seconds
of added 80/20-ms path delay, then eight seconds recovery. It also runs static
target estimator/controller assertions once. This checks daemon integration and
policy arithmetic, not customer throughput or accuracy against real ISP backlog.
The filter is temporarily placed at its required runtime path only if no original
filter exists. The installed service/package/configuration and original qdiscs
are untouched; the protected log inode and test-owned cleanup are checked.

To reproduce, populate a fresh VM directory with `before`, `after`, `filter.o`,
the two static test binaries, `tools/testbed/testbed.sh`, and the configuration
from `profiling/2026-10-04-flow-pair/cake-adapt.config`. Put any missing libbpf/libelf
runtime libraries in its `lib/` directory. Set `LD_LIBRARY_PATH` to that directory
and invoke the wrapper through foreground SSH with a 240-second host deadline.
Collect the directory, then run `python3 analyze.py COLLECTED_DIRECTORY` locally.
Never replace the protected test log or the user's tail process.

The first attempt stopped before traffic because the VM lacks `stat`; its initial
snapshot and diagnostic are retained. The corrected wrapper uses the established
`ls -i` inode check. No product conclusion follows from that failed attempt.

The corrected batch exited 0. Late in the added-delay period, the old pair's
median was 0.88/8.77 ms; the fixed pair retained 80.11/20.10 ms. After clearing
the path, the fixed median was 0.187/0.217 ms. Both target assertion binaries
passed, both filters were JITed, and receivers reported positive payload bytes
for every transfer. Raw logs, phase markers, receiver JSON and snapshots are in
[`raw/accepted/`](raw/accepted/); [`vm-analysis.txt`](vm-analysis.txt) contains the
checks and selected intervals. No CPU measurement is inferred from BPF fdinfo.

Original service PID 2646, package state, executable/config/init hashes, routes
and complete qdisc options matched before/after. The protected log retained
inode 183. Test namespaces, filter, daemon/fping children and payload directories
were gone after cleanup. See [`vm-cleanup.txt`](vm-cleanup.txt).

## Limits

An initial queue remains unknown, and clear fping on a different path can still
permit adaptation. Persistent independent congestion can retain a stale floor
after a route change. Receiver delay that agrees with the heuristic remains
ambiguous, and an accepted ratio may alter which latency threshold is crossed.
The conservative agreement rule also rejects some genuine TCP evidence that
contradicts delivery attribution. There is no claim here about improved customer
latency, 85% capacity retention under control, lifecycle acceptance, arm64 runtime,
or filter CPU cost. The opt-in features still require deployment-specific
validation; tuple lifetime and ACK-byte accounting are separate open findings.
