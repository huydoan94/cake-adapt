# TCP lifetime core kernel test — 2026-10-05

The exact x86 packaged filter loaded on OpenWrt 25.12-SNAPSHOT, kernel
6.12.108, and the corrected run passed **62 assertions with zero failures**.
This is a core lifetime test, not acceptance of the full tuple-reuse milestone.

The AF_PACKET filter was attached to `ca0` in a private network namespace.
Sending on `ca0` exercises outgoing packets; sending on its veth peer `ca1`
exercises incoming packets. No addresses or routes were configured there.
The installed daemon, package, configuration and CAKE qdiscs were preserved.

## Results and limits

- Map types, capacities, flags, key/value sizes and the record ABI passed.
- Active/passive open, SYN and SYN/ACK retransmissions, tuple reuse with lower
  timestamps, simultaneous open, ACK sequence wrap and midstream bootstrap passed.
- An old-generation echoed timestamp did not recover a predecessor's departure.
- Authentic successor records followed by delayed predecessor records preserved
  the estimator's successor generation and last accepted arrival time.
- `UINT32_MAX` was allocated once, then the allocator stayed zero and failed
  closed without producing a sample. The failure counter increased exactly once.
- Before intentional exhaustion, ring-full and generation-failure counters were zero.
- Package, daemon PID/status, interfaces, routes, qdisc configuration and file
  checksums matched before/after. The test-log inode remained **183**.

The verifier reported 481,926 processed instructions (limit 1,000,000).
This is verifier analysis work, not a per-packet instruction count or CPU measurement.

Allocator contention, independent LRU eviction/recovery, runtime CPU and memory
cost, sustained rate control, lifecycle recovery and arm64 execution remain
untested by this run. No production-quality or estimator-accuracy claim follows
from these functional checks.

## Raw evidence

- [Corrected assertions and verifier output](raw/corrected/runner.stdout)
- [Corrected stderr](raw/corrected/runner.stderr)
- [Input checksums](raw/corrected/input-checksums)
- Before/after snapshots: `raw/corrected/{before,after}.*`
- [Initial run](raw/initial/runner.stdout): 56 of 60 assertions passed. Four
  failures came from one test expecting an unmatched reply before the filter's
  4 ms sampling interval elapsed. The corrected harness waits 5 ms at that point.
  The candidate filter object was identical in both runs.

The initial network comparison also included the bridge garbage-collection
timer and therefore reported a harmless ticking timer as a change. The corrected
comparison uses the stable one-line link listing, retains complete link details
separately, and still compares routes and complete qdisc options.

Candidate object SHA-256:
`e4758f03b96e9cc4bf470a93a2a7dda0389b25db9c3e10a00ec2e8aa3f86b4ba`.
The object came from the x86 `cake-adapt-0.2.11-r1.apk` built during this
milestone; it was loaded from the isolated test directory, not installed.

## Reproduce

[runner.c](tools/runner.c) is built with the x86 SDK compiler, repository
`src/tcpdelay/estimator.c`, `-I src`, staged libbpf/libelf/zlib headers and
libraries, and the project's strict warning flags. It has a 120-second alarm.
Pass the exact APK-extracted object; do not replace `/lib/bpf`.

[run.sh](tools/run.sh) accepts an isolated directory containing `runner` and
`candidate.o`. On the authorized x86 VM:

```sh
sh run.sh /tmp/<isolated-test-directory>
```

It captures state under a unique `/root` result directory, owns and removes
only its private namespace and log hardlink, and preserves `/tmp/sqm-mon-test.log`.
It conservatively aborts if any process holds that log open, including a reader;
it does not stop the user's reader. It is an isolated dry-run wrapper and does
not launch the daemon or exercise `log_file_path_override`.

The first run's runner binary predates the sampling wait; the preserved source
is the corrected version. Neither compiled test binaries nor filter objects are
included in this evidence directory.
