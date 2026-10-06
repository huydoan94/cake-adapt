# Lifetime-stream kernel validation

**Reverted 2026-10-05.** The lifetime stream (`885f7d9`) was reverted at the
user's request: it handles a rare case (a reused TCP tuple only removes that
flow's estimate until the slot is replaced) at 49-64% more filter time per
packet, and measured neutral otherwise in the
[VM check](../2026-10-05-gpt-work-check/README.md). It can be restored by
reverting the revert. Its design documents are in [`design/`](design/), the VM
preflight tool in [`tools/`](tools/) (paths inside describe its old
`tools/tcp-lifetime/` location), and the uncommitted work in progress at the
time in [`uncommitted-work-2026-10-05.patch`](uncommitted-work-2026-10-05.patch).

✓ The corrected bounded i386 preflight passed all 10 assertions with zero
failures on OpenWrt 25.12-SNAPSHOT, kernel 6.12.108. The exact current filter
loaded with JIT (`jited_prog_len=13455`, `xlated_prog_len=7456`), current map
schemas passed, and one non-timestamp SYN reached the production native
lifetime consumer. This is a minimal preflight, not full finding-2 acceptance.

The first bounded i386 preflight failed before loading BPF because the VM has
no `stat` command. Exit status was 127. This is a harness dependency failure;
there is no verifier, JIT or lifetime kernel result from this invocation.

✓ The read-only baseline was captured before transfer. All 63 isolated bundle
file hashes matched. Required before/after snapshots show unchanged package,
service/process, BPF settings, installed/configuration files, network, qdiscs
and namespaces. No runner, test namespace or log hardlink was started/created.
The uniquely named bundle was removed after evidence collection.

The authorized log was absent initially. The wrapper created it once as allowed,
then failed while trying to record its inode; it was retained, never replaced
or removed. Final `ls -di` identifies the created inode in the cleanup output.
This does not establish a successful before/after log-inode check.

Raw evidence is in [preflight-initial](preflight-initial/): baseline, ownership
manifest, file verification, operational snapshots, stdout/stderr and exit code.
Its `inputs/` files preserve the exact failing wrapper/runner/shim source. The
filter and production source identities are recorded in the checksums and
`build-provenance`. Compiled artifacts are intentionally excluded.

✓ The correction uses standard `stat(2)` in the existing runner rather than
adding a VM package dependency. Local strict x86/arm64 links, formatting/shell
checks and four mock cleanup cases passed before one affected preflight rerun.

✓ Corrected raw evidence is in [preflight-corrected](preflight-corrected/).
All 63 bundle files verified before execution. The collected archive's SHA-256
matched on the VM and host before isolated file cleanup. Package/service/process,
installed/configuration files, BPF settings, network, full qdisc options and
namespaces match before/after. The authorized log remained device/inode `19:288`;
its final `ls -di` inode is also 288. The wrapper reaped the runner/watchdogs,
removed its namespace and log hardlink/directory, and exited with state status
zero. The isolated bundle was removed after collection. No daemon/fping was
started and no installed artifact or CAKE setting was changed.

The exact filter SHA-256 is
`1c8b9082581e838ea898f4c02fca2e3e23a0d0bcaeb8e6b3b7daf68fda78de22`.
Source checkpoint remains `885f7d9`; both tested harness versions are preserved
under `inputs/`. No performance or calibration result follows from ten preflight
assertions. The full functional/loss/capacity/cross-CPU batch, arm64 execution,
big-endian and other architecture resources, and cost/lifecycle acceptance remain
pending. The initial environment failure was corrected and the affected rerun
passed; no further VM run belongs to this preflight batch.
