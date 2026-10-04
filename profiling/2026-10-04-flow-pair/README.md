# TCP-delay fix 1: keep a directional pair from one flow

Date: 2026-10-04. Before source: local commit
`14f881b5ce26a1c2d41152be2623af15a59eba2f`. After source: the accompanying
estimator change. The unchanged before estimator is preserved in
[`before-src/`](before-src/tcpdelay/estimator.c).

This is an opt-in TCP measurement change, not a change to the upstream-derived
controller. The two cake-autorate replay fixtures still match all 4,703 decisions.

## Problem and fix

The old estimator took independent directional minima across flows. A new
connection calibrated on a congested path could report relative zero for
download, erase an established download estimate, and leave another flow's
upload estimate untouched. That synthesized a pair from unrelated baselines.

The new estimator keeps short result windows per flow. It selects fresh
download observations, prefers a flow with both directions available, and then
prefers the earliest measurement history. Both directions come from that flow.
The current/previous 100-ms minimum windows, clock fitting, 30-second floor
buckets and controller thresholds are unchanged. No capture/filter change.

## Deterministic before/after evidence

[`probe.c`](probe.c) uses the test suite's synthetic timestamp source. The path
is initially unqueued; established flow A then sees 80 ms download and 20 ms
upload added delay. Flow B begins only after that delay is present.

| Situation | Before DL / UL | After DL / UL |
| --- | --- | --- |
| A alone | 80 / 20 ms | 80 / 20 ms |
| A plus newly fitted download-only B | 0 / 20 ms | 80 / 20 ms |
| A plus newly fitted complete B | 0 / 0 ms | 80 / 20 ms |
| A expires; only download-only B remains | 0 / unavailable | 0 / unavailable |
| A expires; only complete B remains | 0 / 0 ms | 0 / 0 ms |
| All samples expire | unavailable / unavailable | unavailable / unavailable |

Raw host outputs: [`host-before.txt`](host-before.txt),
[`host-after.txt`](host-after.txt). Parenthesized `0` means unavailable, not a
measured empty queue. The last three rows deliberately show the limits: the fix
does not invent an uncongested baseline or retain stale history forever.

Additional unit cases verify that an older download-only flow is not combined
with a newer complete flow, and that partial measurements remain available
after a complete pair expires. Existing reordering, timestamp rollover,
clock-period, floor-expiry and delayed-ACK tests remain enabled.

### Reproduce from the repository root

```sh
pair_work=$(mktemp -d)
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror \
    -Iprofiling/2026-10-04-flow-pair/before-src -Isrc \
    profiling/2026-10-04-flow-pair/probe.c \
    profiling/2026-10-04-flow-pair/before-src/tcpdelay/estimator.c \
    -o "$pair_work/before"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror \
    -Isrc profiling/2026-10-04-flow-pair/probe.c src/tcpdelay/estimator.c \
    -o "$pair_work/after"
"$pair_work/before"
"$pair_work/after"
make -C tests ../build/tests/bin/test_estimator
./build/tests/bin/test_estimator
```

The host task (`check check-netlink check-config`), focused ASan/UBSan test,
and exact x86 SDK clean/compile task passed. The first sanitizer invocation
could not use LeakSanitizer because of the sandbox's ptrace restriction. A
narrowly escalated rerun with `ASAN_OPTIONS=detect_leaks=1` passed (exit 0),
with ASan, UBSan and leak detection enabled. There is no new allocation or
resource ownership in this change. [Sanitizer stdout](sanitizers.txt).

On the 64-bit host, the estimator grows from 11,072 to 13,056 bytes: **1,984
extra bytes per instance**. Result selection now scans at most 32 flow slots,
instead of two global windows. This is a bounded additional operation, not a
measured claim of unchanged CPU cost.

On the 32-bit x86 VM, the preserved before/after probes report 9,784 to
11,520 bytes: **1,736 extra bytes per instance**. Both probes show the same
80/20-ms regression and fix as the host, and the focused estimator unit test
passes on target. Raw outputs:
[`vm/probe-before.txt`](vm/probe-before.txt),
[`vm/probe-after.txt`](vm/probe-after.txt),
[`vm/test-estimator.txt`](vm/test-estimator.txt).

## Integration protocol

[`run-vm.sh`](run-vm.sh) and [`cake-adapt.config`](cake-adapt.config) reproduce
the focused integration run with `tools/testbed/testbed.sh`. Copy those files,
the freshly built executable, and the BPF object into a fresh VM temporary
directory; put the configuration in `uci/cake-adapt` and set its
`log_file_path_override` to the directory's `logs/` using `uci -c`. Run:

```sh
timeout 90 sh run-vm.sh WORKDIR BINARY OBJECT
```

If the VM has no `timeout`, bound the SSH command from the host instead, using
a PTY (`ssh -tt`) so timeout disconnects reach the script's HUP cleanup trap.
Always reconnect and verify cleanup; do not infer success from a disconnect.

The script refuses existing namespaces or a pre-existing runtime BPF object.
It temporarily installs the filter at the daemon's exact runtime path and
captures its program fdinfo, start record and actual `TCP_QUEUE` records.
It uses 80/2-Mbit/s CAKE shapers with rate adjustment disabled. Established
upload/download traffic warms up for five seconds. Netem delay then increases
by 80 ms downstream and 20 ms upstream; a new download starts four seconds
later. All payloads stay between controlled local endpoints. This intentionally
changes **path delay**, not a measured ISP backlog; it exercises baseline
history and connection churn without claiming accuracy against real queues.

The original service/package and eth1/ifb4eth1 qdiscs must remain unchanged.
The live test log is truncated in place, its inode checked before/after, and
all test-owned namespaces/processes and the temporary runtime filter removed.

### Collected integration results

The bounded run exited 0 on the OpenWrt 25.12 i686 VM (Linux 6.12.108).
The installed older daemon lacked the filter and libbpf dependencies. The fresh
SDK executable, filter, libbpf and libelf were copied into an isolated temporary
directory; libraries were selected with `LD_LIBRARY_PATH`, not installed.
Only the filter was temporarily copied to its required runtime path. The
executable and filter hashes in [`vm/artifacts.sha256`](vm/artifacts.sha256)
match the SDK's freshly built artifacts. [`source.sha256`](source.sha256)
identifies the accompanying source from the repository root.

[`vm/bpf-fdinfo`](vm/bpf-fdinfo) identifies an attached socket-filter program
(`prog_type=1`, JIT enabled, tag `e45e774793fd1fbd`). The
[`daemon log`](vm/cake-adapt.log) contains the capture-start message and
**457 `TCP_QUEUE` records, 396 with both directions valid**. Early invalid
records precede remote-clock calibration; they are not measurements of zero.
The fdinfo runtime counters are zero and are not used for a performance claim.
Non-runtime setup/capture retries are retained in
[`failed-attempts.txt`](failed-attempts.txt); they were not mixed into this run.

The epoch markers in [`vm/phases`](vm/phases) define these conservative
analysis intervals: one second after adding delay until the new flow starts,
then three seconds after the new flow starts until it finishes. Median values
below use only records with both validity flags set:

| Interval | Complete pairs | DL minimum / median / maximum | UL minimum / median / maximum |
| --- | ---: | --- | --- |
| Established flow with added path delay | 59 | 80.134 / 80.365 / 82.198 ms | 25.572 / 27.319 / 49.373 ms |
| New connection has had time to fit its clock | 117 | 80.101 / 80.691 / 87.475 ms | 20.174 / 28.881 / 44.010 ms |

Download did not collapse to a new flow's relative zero in this run. This is
consistent with the deterministic result, but not a controlled live
before/after accuracy comparison: only the fixed daemon ran live. Upload is
not an exact 20-ms reading; receiver timing and other variations remain in the
measurement. This does not resolve the separate sustained delayed-ACK finding.

Receiver summaries prove payloads arrived:

| Transfer | Receiver bytes | Raw JSON |
| --- | ---: | --- |
| Upload | 2,621,440 | [upload.json](vm/upload.json) |
| Established download | 94,896,128 | [established-download.json](vm/established-download.json) |
| New download | 31,588,352 | [new-download.json](vm/new-download.json) |

The installed daemon remained PID 2653. Original UCI and init contents and the
installed executable checksum were identical before/after. Original CAKE
handles/options remained eth1 `800f:` at 20 Mbit/s diffserv3, ingress `ffff:`,
and ifb4eth1 `8010:` at 20 Mbit/s besteffort; their counters naturally advanced.
Full [before](vm/original-qdiscs-before.txt) and
[after](vm/original-qdiscs-after.txt) qdisc dumps are retained. Test namespaces,
daemon, fping children and iperf processes were gone and the temporary runtime
filter was removed. The protected log's
[before](vm/inode-before) and [after](vm/inode-after) inode is **165**.

## Limits and remaining findings

Age is a conservative history preference, not proof that a baseline was empty.
A complete pair can outrank an older partial flow. Multiple remote paths may
differ; this policy is not a universal path classifier. Once established
samples expire, only new flows' relative baselines remain. Persistent queue
absorption, tuple reuse, sustained receiver ACK delay, and CAKE byte-accounting
differences are still separate findings in [`EBPF_REVIEW.md`](../../EBPF_REVIEW.md).
No claim about customer latency reduction or throughput retention follows
from these synthetic tests or the short integration check.
