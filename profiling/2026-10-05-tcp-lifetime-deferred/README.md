# Tuple-lifetime implementation deferred — 2026-10-05

The user requested a short continuation and rollback of the entire uncommitted
tuple-lifetime implementation if finishing it required a larger redesign.
That stop condition was reached. The eight source/test files were restored to
the committed baseline recorded in `base-commit.txt`. The committed
accepted-sample freshness fix remains; full tuple-reuse handling is deferred.
Policy changes and existing evidence were preserved. No VM was accessed, no
package was rebuilt or installed, and no branch, version or commit was changed
during this diagnosis and rollback.

## Reason for stopping

The candidate contains `cmpxchg32_32` atomic operations. The existing OpenWrt
6.12.108 kernel source, `arch/x86/net/bpf_jit_comp32.c:2461`, sends both word
and double-word `BPF_ATOMIC` instructions to `notyet`, which returns `-EFAULT`.
This explains why the recorded x86 candidate ran interpreted while the
baseline was JIT compiled. The source excerpt and its full-file checksum are
preserved in `kernel-jit-excerpt.txt`.

Avoiding those operations while preserving concurrent lifetime publication
requires a synchronization redesign or a kernel change, rather than a small
compiler-setting fix. Neither was attempted. The diagnosis does not establish
Filogic performance; the local arm64 JIT source has atomic handling, but that
is not a substitute for a target run.

## Preserved work

- `implementation.patch` contains all reverted source/test changes and the
  candidate's review-document changes relative to `base-commit.txt`.
- `implementation.sha256` verifies the patch. Its reverse application was
  checked before rollback and its forward application was checked afterward.
- [Core lifetime evidence](../2026-10-05-tcp-lifetime-core/README.md).
- [Capacity and observed cross-CPU race](../2026-10-05-tcp-lifetime-capacity/README.md).
- [Measured x86 cost and memory charge](../2026-10-05-tcp-lifetime-cost/README.md).

Historical lifetime harnesses depend on the deferred candidate's headers and
estimator; they are not runnable against the restored source without restoring
that candidate in an explicitly authorized future task. Previously built SDK
packages and extracted candidate objects likewise do not represent the restored
source and must not be treated as current deployment artifacts.

To inspect the saved work, use `git apply --stat implementation.patch`. Do not
apply it automatically or restart the VM campaign; resumption requires a new
bounded plan addressing the JIT and synchronization trade-off.
