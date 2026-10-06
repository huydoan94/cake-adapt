# Bounded lifetime-stream preflight

✓ Plan and minimal preflight harness pass bounded source review, strict x86/arm64
links, shell/format checks and local mocked cleanup cases. Primary accepted the
ownership/missing-log corrections. ✓ The corrected i386 preflight passed
10 assertions with JIT and preserved operational state/log inode;
[evidence](../../profiling/2026-10-05-tcp-lifetime-stream/README.md) records the
initial missing-utility failure and its affected rerun. This checks the smallest
useful kernel path before expanding the remaining TL-05 harness cases.

## Question and fixed scope

Can the exact current APK-extracted filter load with JIT on the authorized i386
OpenWrt kernel, and can one non-timestamp SYN reach the real production capture
consumer and create pending native lifetime state?

Use `run.sh --preflight OBJECT RESULT_DIR` exactly once on `192.168.56.2` after
local acceptance of the corrected wrapper. The runner checks current map schemas,
program identity/JIT length, record ABI and the SYN state. No normal-mode cases,
control workload, benchmark, package installation or service restart belong to
this preflight. It does not establish calibration, recovery, concurrency, latency,
throughput or broad architecture acceptance.

The input is the x86 filter extracted from the passing `0.2.11-r1` APK. APK
SHA-256 is `0cad228933b8e35b606bfab503ee0f958d0cfc84f914ac770edd1c68f4df43ff`;
extracted filter SHA-256 is
`1c8b9082581e838ea898f4c02fca2e3e23a0d0bcaeb8e6b3b7daf68fda78de22`.
Before transfer verify both hashes against the build/instruction inventory,
retain the successful build log and production source/checkpoint `885f7d9`,
and confirm production inputs have no changes since that checkpoint. Record
relevant source hashes, not just the unchanged package version. Previously
built packages with the same version are not substitute inputs.
The runner cross-links the current production capture, lifetime and estimator
code, with only application logging supplied by a tool shim. Preserve the bundle
layout expected by `run.sh`, including its source files and runner. Record the
runner and complete relevant source hashes immediately before transfer and verify
the bundle checksum on the VM. Never replace the installed executable or filter.

## Required local gate

- Strict C11 compile/link for existing x86 and arm64 toolchains.
- Shell syntax and clang-format stability; `git diff --check`.
- The logging shim emits libbpf diagnostics to captured stderr; it does not
  discard the verifier evidence required below.
- Local mocked-command checks exercise alias-writer refusal and failed-setup
  cleanup before using the wrapper on a VM.
- Bounded reviewer acceptance of writer detection, early traps, deadlines,
  failed-setup cleanup and assertions. Review corrections only after the first
  bounded review; do not repeat a broad audit.
- Inspect the final wrapper's actual metadata and cleanup commands against this
  plan before invoking it. If its behavior differs, correct the tool or plan first.

## Execution and metadata

Before transferring or creating any remote files, capture a read-only baseline
over bounded SSH to local storage: kernel/release, service/configuration, installed
package/files, daemon/fping processes, links/rules/routes/namespaces and complete
qdisc state. If that baseline fails, stop before transfer. The wrapper's before/
after snapshots remain the operational comparison around namespace/log mutation.

Use one isolated temporary bundle and a new result directory. Bound transfer
and SSH connections: connect timeout five seconds, transfer limit 30 seconds,
and remote invocation limit 180 seconds. The wrapper requests cleanup at 150 seconds and enforces a 160-second hard
deadline; the runner has a 120-second alarm. If the hard deadline interrupts
cleanup, the result is inconclusive and restoration cannot be claimed. Reserve
bounded cleanup time after stopping the runner. Keep the foreground SSH command attached until its exit.

Before network-namespace or log mutation, retain kernel/release information,
installed package and service state, exact daemon/fping PIDs, configuration and
installed file checksums, root links/rules/routes, complete qdisc handles/options/
rates, and namespace state. Record `/proc/sys/net/core/bpf_jit_enable` if present;
the loaded program's nonzero JIT length is the actual execution-mode gate.
Retain stdout/stderr, exit status and verifier diagnostics supplied by libbpf.

The only network mutation is an owned, uniquely named namespace with a veth pair,
no configured addresses or routes. Frames remain in that namespace. No host CAKE
qdisc, traffic control setting, service, configuration or installed package is
changed. Existing interface names outside that namespace are unrelated.

For `/tmp/sqm-mon-test.log`, reject a symlink/nonregular file and refuse to truncate
while any process has its device/inode open for writing, including through aliases.
Do not terminate an unrelated writer or the user's reader. Create the file once
only if absent, record its inode, truncate in place, and hard-link the isolated
expected log name. Never delete, rename or replace the authorized log.

## Acceptance, cleanup and stopping rules

Pass requires every preflight assertion, nonzero JIT length, normal bounded exit,
and unchanged operational before/after snapshots. Record namespace, log-directory,
wrapper/runner identity and watchdog/marker ownership in the result directory
before relevant mutations. A normal success or failure must
stop/reap only the test runner within a bound, remove only the owned namespace,
log hardlink and directory, and confirm the log inode and package/service/process/
file/qdisc/root-network state. Keep complete raw snapshots; exclude ticking
statistics from equality claims. No test-owned process or namespace may remain.

If the hard deadline kills the wrapper before cleanup completes, classify the
invocation as inconclusive. Perform one bounded follow-up audit (30 seconds),
using the result directory's ownership record and the pre-transfer baseline.
Before removing a resource or signalling a process, verify its identity still
matches that test's recorded ownership; never sweep a name prefix or kill a
reused/unrelated PID. Remove only proven test-owned resources, then capture
post-state and log inode. If identity, audit or restoration cannot be verified,
stop with that unresolved state rather than claiming a restored pass.

Collect results with a bounded transfer and verify their checksums before deleting
only the test-created remote bundle/result paths. Preserve failing evidence and
diagnose it locally. A failed assertion or unavailable facility is a stop point,
not permission for a broader run. A rerun needs a specific corrected input and
targets only the failed case. After two harness/environment failures, stop all
VM execution and reassess the plan. Do not infer a product failure from sandbox
restrictions or a known unrelated VM panic. A panic makes the invocation
inconclusive: stop VM work, preserve the serial-console log before restart or
power-off, and never claim restoration without post-state evidence. Use only an
authorized WSL/VM console-log location; if none is available, request the log
from the user without touching Windows. A rerun targets that lost invocation
and still counts toward the two harness/environment-failure limit.

After a passing preflight, implement/review the remaining TL-05 cases and write
one acceptance batch with fixed repetitions. Arm64 and missing architecture
resources retain separate acceptance gaps; this i386 preflight cannot close them.

## Completed local gate

✓ Strict x86/arm64 links, clang-format stability, shell syntax and diff checks.
✓ Reviewer accepted map metadata, sequence-wrap/retransmission assertions,
writer detection, normal cleanup and diagnostics. Primary accepted the final
ownership record and missing-log corrections.
✓ `python3 tools/tcp-lifetime/check-wrapper.py` passes alias-writer refusal,
failed-veth cleanup, read-only-holder preservation and missing-log rejection.
No real kernel deadline, packet path or runtime claim follows from these mocks.
