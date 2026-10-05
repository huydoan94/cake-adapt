# TCP lifetime capacity and ordering checks — 2026-10-05

The exact x86 packaged filter passed the map-capacity, real-entry eviction, and
one cross-CPU generation race batches on OpenWrt 25.12, kernel 6.12.108. The
final run passed **149 assertions with zero failures**. The installed daemon,
package, configuration, and CAKE qdiscs were not changed.

Each of the seven bounded LRU maps reached its declared entry count. One
overflow update evicted the oldest key and reclaimed 127 entries on this
kernel, so the harness records the observed post-reclaim count instead of
assuming the map stays exactly full. The semantic batch seeded and verified
eviction of a real active-flow entry in every map. Missing lifecycle, identity,
and pending-side evidence failed closed and recovered after a new handshake;
SYN retransmission retained its generation; an evicted confirmed pair allowed
one newer generation; sampling state recovered with a matched departure; and
an evicted departure emitted a sample without upload evidence.

On two online CPUs, simultaneous pinned senders confirmed the same pending
pair. Per-CPU upload counters increased by 66 bytes on CPUs 0 and 1. Both
confirmations allocated candidates, while the final pair, identity, sampling
state, and active/latest lifecycle agreed on generation 14. This is one
observed interleaving. It does not prove exhaustive cross-CPU ordering or CAS
contention behavior. Performance and long-running lifecycle recovery remain
untested, so issue 2 stays open.

The final test-log inode was 183 before and after. Package, relevant files,
processes, stable links/routes/qdiscs, and namespace state matched. The exact
filter object SHA-256 was
`e4758f03b96e9cc4bf470a93a2a7dda0389b25db9c3e10a00ec2e8aa3f86b4ba`.
Raw outputs, input checksums, verifier log, and complete before/after snapshots
are under [`raw/`](raw/).

The first smoke capture is retained as `raw/first-run/`. Its runner checks
incorrectly required post-overflow occupancy to remain at `max_entries`; the
follow-up corrected that harness assumption. The stable network comparison
also excludes a bridge garbage-collection countdown while retaining complete
link details in the raw snapshots.

## Reproduction

From the repository root, build the runner with the x86 SDK staged headers and
libraries, and extract the filter from the recorded package:

```sh
sdk=../openwrt-sdk-x86
staging=$sdk/staging_dir
work=$(mktemp -d /tmp/cake-lifetime-capacity.XXXXXX)
trap 'rm -rf "$work"' EXIT
STAGING_DIR=$staging "$staging/toolchain-i386_pentium4_gcc-14.3.0_musl/bin/i486-openwrt-linux-musl-gcc" \
  -std=c11 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror -pthread \
  -I src -isystem "$staging/target-i386_pentium4_musl/usr/include" \
  profiling/2026-10-05-tcp-lifetime-capacity/tools/runner.c src/tcpdelay/estimator.c \
  -L "$staging/target-i386_pentium4_musl/usr/lib" \
  -Wl,-rpath-link,"$staging/target-i386_pentium4_musl/usr/lib" -lbpf -lelf -lz \
  -o "$work/runner"
mkdir "$work/extracted"
"$staging/host/bin/apk" --allow-untrusted extract --destination "$work/extracted" \
  "$sdk/bin/packages/i386_pentium4/base/cake-adapt-0.2.11-r1.apk"
cp "$work/extracted/lib/bpf/cake-adapt-tcpdelay.o" "$work/candidate.o"
printf '%s  %s\n' \
  e4758f03b96e9cc4bf470a93a2a7dda0389b25db9c3e10a00ec2e8aa3f86b4ba \
  "$work/candidate.o" | sha256sum -c -
mkdir -p /tmp/cake-adapt-tcp-lifetime-capacity
scp -O "$work/runner" "$work/candidate.o" \
  profiling/2026-10-05-tcp-lifetime-capacity/tools/run.sh \
  profiling/2026-10-05-tcp-lifetime-capacity/tools/evidence.sh \
  root@192.168.56.2:/tmp/cake-adapt-tcp-lifetime-capacity/
ssh -o BatchMode=yes root@192.168.56.2 \
  'chmod 700 /tmp/cake-adapt-tcp-lifetime-capacity/run.sh /tmp/cake-adapt-tcp-lifetime-capacity/evidence.sh'
timeout 300 ssh root@192.168.56.2 \
  '/tmp/cake-adapt-tcp-lifetime-capacity/evidence.sh /tmp/cake-adapt-tcp-lifetime-capacity/candidate.o'
```

`evidence.sh` captures the baseline, preserves the test-log inode, and verifies
state restoration. `run.sh` creates and removes only a private network
namespace. The runner alarm is 180 seconds. The VM uses legacy SCP because its
OpenSSH SFTP server is unavailable.
