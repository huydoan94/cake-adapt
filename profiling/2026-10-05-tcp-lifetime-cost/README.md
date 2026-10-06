# TCP lifetime filter cost probe

This is a bounded x86 VM microbenchmark comparing the pre-lifetime filter with
its packaged lifetime-aware replacement. It measures socket-filter execution
through a scoped `bpf_enable_stats(BPF_STATS_RUN_TIME)` fd, not sender syscall
latency. Each object ran inside a private veth namespace, with the runner pinned
to CPU 0. The wrapper disabled IPv6 only inside each test namespace to prevent
link-local setup traffic from affecting packet counts. Three baseline/candidate
pairs ran in sequence; each workload drained the ring every 64 packets and
reported exact program run-count and ring-full deltas.

The fixed workloads are reproducible and intentionally small. The steady flow
sends the three-way handshake and 400 bidirectional timestamp pairs: 803
injected packets. Tuple churn sends 64 three-packet handshakes: 192 packets.
Every measured workload had an exact BPF run count and zero ring-full events.
The corrected run's before/after service, process, package, qdisc, interface,
namespace and test-log snapshots matched; `/tmp/sqm-mon-test.log` remained inode
183.

| Workload | Baseline median (range) | Candidate median (range) | Median ratio |
| --- | ---: | ---: | ---: |
| Steady, ns per BPF run | 444 (392–558) | 5,212 (5,036–5,571) | 11.7x |
| Tuple churn, ns per BPF run | 514 (497–698) | 7,889 (7,610–8,032) | 15.3x |

This is an observed execution-cost regression on the tested x86 VM. The
baseline filter was JIT compiled (`prog_jited=1`, 3,728 translated bytes,
6,701 JIT bytes); the candidate ran in the interpreter (`prog_jited=0`, 25,384
translated bytes, no JIT bytes). That mode difference is a major confounder for
attributing the cause to the lifetime changes or extrapolating to Filogic. CPU
acceptance therefore remains open pending a comparable JIT mode and a
controlled daemon run. These microbenchmark figures do not measure controller
latency or end-to-end throughput.

Kernel map `fdinfo` reported 1,039,176 bytes of `memlock` for the baseline maps
and 2,166,504 bytes for candidate maps, an increase of 1,127,328 bytes
(108.5%). The shared ring map accounts for 275,148 bytes in each total. This is
kernel BPF map memory accounting, not daemon RSS. Program memlock was separate:
4,096 bytes baseline and 28,672 bytes candidate. Raw map layouts, per-map
`memlock`, program JIT state and run records are retained below `raw/`.

The baseline source used to produce `baseline.raw.o` has SHA-256
`eafc5b14e248b45bf7d9297110a1a9d7976ebf20dcd04d24e3eb144c0a60d9a8`, matching
`git show HEAD:src/tcpdelay/tcpdelay.bpf.c` at collection time. The exact object
hashes are baseline `2d0607df1cfe5e1ae7eeb2278b5933b5c5befb91bea81a272401908b0f65d884`
and packaged candidate `e4758f03b96e9cc4bf470a93a2a7dda0389b25db9c3e10a00ec2e8aa3f86b4ba`.
The corrected runner hash is recorded in `raw/corrected-three-pairs/inputs.sha256`.

`tools/runner.c` builds with the x86 SDK staged headers and libraries using the
strict warning flags below. The VM scripts under `tools/` create and remove
only private namespaces and capture state even when a run fails. They never
change the installed object, daemon, configuration, qdiscs, global JIT setting,
or test-log inode.

```sh
sdk=../openwrt-sdk-x86
staging=$sdk/staging_dir
STAGING_DIR=$staging "$staging/toolchain-i386_pentium4_gcc-14.3.0_musl/bin/i486-openwrt-linux-musl-gcc" \
  -std=c11 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror \
  -I src -isystem "$staging/target-i386_pentium4_musl/usr/include" \
  profiling/2026-10-05-tcp-lifetime-cost/tools/runner.c \
  -L "$staging/target-i386_pentium4_musl/usr/lib" \
  -Wl,-rpath-link,"$staging/target-i386_pentium4_musl/usr/lib" -lbpf -lelf -lz \
  -o /tmp/cake-lifetime-cost-runner

: "${BASELINE_OBJECT:?Path to the recorded baseline.raw.o}"
: "${CANDIDATE_OBJECT:?Path to the recorded candidate.package.o}"
timeout 60 scp -O /tmp/cake-lifetime-cost-runner \
  "$BASELINE_OBJECT" \
  "$CANDIDATE_OBJECT" \
  profiling/2026-10-05-tcp-lifetime-cost/tools/run.sh \
  profiling/2026-10-05-tcp-lifetime-cost/tools/evidence.sh \
  root@192.168.56.2:/tmp/
timeout 300 ssh root@192.168.56.2 \
  'chmod 700 /tmp/run.sh /tmp/evidence.sh; /tmp/evidence.sh /tmp/cake-lifetime-cost-runner /tmp/baseline.raw.o /tmp/candidate.package.o repeat'
```

The raw directory `three-pairs-before-jit-state-capture/` is the first repeat
set, retained for audit. `corrected-three-pairs/` contains the reported set,
with program JIT state included. The capacity README's reproduction now builds
in a `mktemp` directory and checks the extracted packaged object checksum before
transfer.

Compiled inputs are not committed. Their identities are preserved in
`input-artifacts.sha256`; local copies were retained under ignored
`build/profiling/2026-10-05-tcp-lifetime-cost/inputs/`. The baseline is from
commit `7665e7e575c69cff0d06a4c9f1070f99df3f9517`; the candidate is the
APK-extracted object identified above. Supply and verify those inputs before
using the historical reproduction command. The candidate implementation is
[deferred](../2026-10-05-tcp-lifetime-deferred/README.md); these commands are
not an instruction to resume testing.
