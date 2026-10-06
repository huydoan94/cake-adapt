# Tuple-lifetime finding 2: work orders

Status: selected protocol implemented with unit tests and bounded source review;
focused host checks pass. These are persistent project work orders, not a session
handoff. ✓ The existing x86 and Filogic package builds and producer instruction
inspection pass. Architecture coverage, kernel checks, VM, profiling and
end-to-end acceptance remain pending. The saved patch has not been restored.
Implementation checkpoint: `885f7d9` (`tcpdelay: isolate tuple lifetimes in an
ordered capture stream`). This identifies the host-tested source; it does not
mark target acceptance complete.

## Completion tracking

A leading `✓` marks a completed task, using the evidence and acceptance scope
stated below. An unchecked task remains incomplete; a completed source task does
not imply its kernel, architecture or performance validation has passed.

- ✓ Selected protocol and transition/record contract (TL-02).
- ✓ Production implementation and focused host unit/replay/sanitizer checks.
- ✓ Acceptance limits frozen before target runs (TL-01.4).
- Partial: [architecture capability inventory](TUPLE_LIFETIME_CAPABILITIES.md) (TL-01.3).
- ✓ Producer branch tests and packaged-object instruction inspection (TL-03.5).
- ✓ Existing x86 and Filogic package builds and target ABI assertions. Other
  required target builds remain open (TL-06.2).
- ✓ Minimal harness/local review and i386 JIT preflight: 10 assertions pass
  ([evidence](../README.md)).
- In progress: remaining kernel harness cases and acceptance plan (TL-05).
- Pending: remaining target builds/ABI/kernel coverage and cost/lifecycle
  acceptance (TL-06/TL-07).

## One-time implementation mode (2026-10-05)

✓ The one-time feature/unit-test implementation pass is finished. Execution was
deferred until its end, then focused host checks, upstream replay and sanitizers
passed. That exception does not apply to this continuation: the normal bounded
validation gates below apply. Other branch, deployment and policy restrictions
remain unchanged. No kernel or runtime acceptance follows from host checks.

The [selected lifetime-stream design](TUPLE_LIFETIME_DESIGN.md) records ownership,
ordering, loss/recovery, ABI and numerical bounds. TL-01/TL-02 design work is
recorded there; the architecture capability inventory remains partial. TL-03 and
TL-04 source implementation and host unit checks are complete. ✓ Instruction
inspection and both existing SDK package builds pass; kernel and remaining
architecture gates stay open. TL-05 harness work is in progress; TL-06 target
validation and TL-07 measurement remain pending. Host checks and source review
do not satisfy those acceptance gates.

## Problem and intended result

TCP queue measurement currently identifies a connection using its endpoint
addresses and ports. Those values identify a tuple, not a connection lifetime.
When another connection reuses that tuple, it can inherit the predecessor's
remote clock fit, delay floors, sampling state and departure entries.

In the reproduced lower-timestamp case, replacement samples are rejected as
reordered, and both directional estimates remain unavailable after seven seconds.
The committed freshness fix prevents those rejected samples from refreshing the
flow's LRU position; it does not reset its calibration. A replacement with larger
timestamps can instead be accepted into old calibration. Departures keyed only
by tuple and timestamp can also collide between lifetimes. The seven-second result
is a host reproduction, not a measured recovery bound for every real connection.

The intended result is a measurement pipeline that starts fresh calibration on
credible replacement evidence, matches departures only within that lifetime,
rejects delayed predecessor records, and withdraws confidence when its own
pipeline loses evidence needed to establish the lifetime. fping and the daemon
must remain usable when optional TCP measurement becomes unavailable.

This solves continuity and isolation of TCP measurement. It does not prove that
the initial floor represents an empty queue, distinguish receiver ACK delay from
queueing, or fix the separate control-performance regression. It does not change
rate-control defaults, own SQM setup, or implement a full TCP stack.

## Established evidence and rejected shortcuts

- [Finding 2](../../../EBPF_REVIEW.md#3-finding-a-reused-tuple-inherits-an-old-timestamp-clock)
  records the original reproduction and acceptance requirements.
- The [deferred patch](../../2026-10-05-tcp-lifetime-deferred/README.md)
  introduced handshake pairing and generation-qualified measurement. Its
  historical functional results do not describe current production source.
- Its atomic instructions prevented JIT compilation on the tested i386 kernel.
  The [cost comparison](../../2026-10-05-tcp-lifetime-cost/README.md)
  compared different execution modes; it does not establish portable runtime cost.
- The [resumption analysis](../../2026-10-05-tcp-lifetime-resume/README.md)
  rejects replacing those atomics with a lossy try-lock alone. A replacement SYN
  can lose acquisition; its SYN/ACK then fails against the old pending ISN;
  ordinary replacement data can still pass the old generation's identity check.
- A failure counter alone does not invalidate already buffered or published
  measurement. A global try-lock has the same lost-reset-evidence problem.
- Resetting on every backward timestamp, adding an arbitrary idle timeout,
  relying on LRU eviction, or quietly accepting interpreter execution is not an
  accepted resolution. Socket-filter spin-lock helpers are not an accepted
  replacement under the checked kernel contract.

The selected protocol is recorded in [the design](TUPLE_LIFETIME_DESIGN.md).
Source review and implementation authorization do not establish runtime acceptance.

## Portability contract

The solution must support multiple OpenWrt architectures through one common
algorithm and protocol. i386 and arm64 are available test machines, not the
definition of support.

1. Target OpenWrt 25.12. Choose supported kernel APIs, helpers, map types and
   BPF instructions from a documented common capability baseline. Do not require
   a custom kernel or a JIT patch to make the design correct.
2. Do not introduce CPU-family branches, target-specific synchronization,
   assembly, or separate lifetime semantics for different architectures.
   Capability handling may differ only in availability, never in correctness.
3. Build the same BPF source as `bpfel` or `bpfeb` using the SDK's target
   endianness. A single compiled object is not required to work across both
   byte orders. The package already passes the SDK's `BPF_TARGET` to the build.
4. Use fixed-width shared fields and explicit size/offset assertions. Preserve
   the 64-byte sample record requirement. Specify map/ring field byte order and
   network-field conversion; test alignment, padding and malformed records.
   Never assume that a 64-bit load/store is atomic on every supported machine.
5. Preserve normal timestamp wrap and sequence-number wrap semantics independently
   of host integer width. Lifetime identity must not silently wrap into an old
   identity, and capture restarts must not import records from an earlier capture.
6. Inspect emitted instructions and prove verifier acceptance and execution mode
   on representative kernels. When a target runs the baseline with JIT, the new
   implementation must retain JIT execution. Compiler success alone is insufficient.
7. Unsupported optional measurement must produce a visible degraded state while
   normal daemon operation continues. This is a failure path, not successful TCP
   support. Do not omit a difficult architecture and call the solution portable.
8. Missing SDKs, emulators, kernels or hardware are explicit verification gaps.
   Do not acquire new machines, touch Windows, or expand deployment permissions
   just to fill the matrix. Record the gap and the resource needed to close it.

The [OpenWrt 25.12.0 package architecture index](https://downloads.openwrt.org/releases/25.12.0/packages/)
confirms the following architecture families exist in that release. The table is
a required investigation/build matrix, not a claim that cake-adapt or its TCP
filter has already been validated on them. TL-01 must pin exact target, subtarget,
SDK, kernel configuration and ABI for each row; do not infer those from names alone.

| Family and representative package ABI | Width / byte order to verify | Planned evidence |
| --- | --- | --- |
| x86: `i386_pentium4` | 32 / little | Existing x86 SDK and VM; functional, JIT and cost baseline. |
| x86: `x86_64` | 64 / little | SDK build, ABI checks and representative kernel load. |
| ARM: `arm_cortex-a7` | 32 / little | SDK build, ABI checks and representative kernel load. |
| ARM: `armeb_xscale` | 32 / big | Capability investigation, SDK build and endian checks. |
| AArch64: `aarch64_cortex-a53`, `aarch64_generic` | 64 / little | Filogic SDK plus existing emulated arm64 VM; functional only. |
| MIPS: `mips_24kc` | 32 / big | SDK build, ABI/endian checks and representative kernel load. |
| MIPS: `mipsel_24kc` | 32 / little | SDK build, ABI checks and representative kernel load. |
| MIPS64: `mips64_mips64r2`, `mips64el_mips64r2` | 64 / both | SDK builds, ABI/endian checks and capability investigation. |
| PowerPC: `powerpc_8548`, `powerpc64_e5500` | 32/64 / big | SDK builds, ABI/endian checks and capability investigation. |
| RISC-V: `riscv64_generic` | 64 / little | SDK build, ABI checks and representative kernel load. |
| LoongArch: `loongarch64_generic` | 64 / little | SDK build, ABI checks and capability investigation. |

At least one big-endian kernel must exercise actual map/ring exchange and packet
parsing before claiming endian runtime coverage. Every selected row needs a
capability result and build result. A representative runtime result covers only
the tested kernel/configuration; untested configurations remain listed as such.

## Delivery order and ownership

Order progress is recorded above. Dependencies normally require completion gates;
the completed one-time implementation mode allowed source work before executable
validation. Each target run still requires the locally reviewed bounded plan.

| Order | Deliverable / problem it solves | Depends on | Accountable role |
| --- | --- | --- | --- |
| TL-01 | Measurement contract and target matrix; prevents an architecture-specific design. | None | Primary |
| TL-02 | Reviewed lifetime/loss/recovery protocol; prevents stale confidence after lost reset evidence. | TL-01 | Primary, with designated implementer and reviewer |
| TL-03 | Kernel producer and shared ABI; isolates departures and samples by lifetime. | TL-02 | Designated implementer |
| TL-04 | Userspace consumer and monitor integration; resets calibration and safely handles uncertainty. | TL-02, TL-03 interface | Designated implementer |
| TL-05 | Bounded kernel harness and run plan; makes failure/order cases reproducible. | TL-03, TL-04 | Designated implementer; primary accepts plan |
| TL-06 | Cross-architecture build and functional evidence; verifies the common design beyond two VMs. | TL-01, TL-03, TL-04, TL-05 | Designated implementer; primary accepts results |
| TL-07 | Cost, lifecycle and final acceptance; checks that correctness remains usable under load. | TL-06 | Primary, implementer and bounded reviewer |

Use the roles and model choices in [COLLABORATION.md](../../../COLLABORATION.md). Delegate
through short task files with `fork_turns="none"`; never send conversation history.
Use one implementer and one bounded reviewer. No parallel writers on the shared
ABI or overlapping subsystem files. Each task return states changed files, checks,
evidence, unresolved limits and whether its acceptance gate passed.

### TL-01 — Define the contract and portability baseline

**Problem:** a target-specific workaround can pass a local test while failing
another JIT, userspace ABI or byte order. Passive capture also has limits that
must not be hidden behind a claim of perfect connection identity.

**Tasks:**

- ✓ **TL-01.1:** Write invariants for a trusted lifetime, handshake confirmation,
  timestamp/sequence wrap, reordered data and departure matching. State how
  bootstrap works when capture begins during an established connection.
- ✓ **TL-01.2:** Distinguish ordinary sample loss from loss of lifecycle evidence.
  List detected capture loss, undetected loss before the observation point,
  exact tuple/ISN reuse, closure ambiguity and capture restart limits.
  No passive observer can promise recovery from evidence it never observes.
- **TL-01.3:** Fill the architecture matrix with exact OpenWrt target/subtarget,
  package ABI, endian/width, kernel version/configuration, map/helper support,
  available JIT instructions, libbpf/toolchain versions and test resources.
  Reuse existing SDKs first; unavailable rows get an explicit blocker.
- ✓ **TL-01.4:** Define numerical limits for maps, ring storage, retries, invalidation
  latency, recovery, retained state and acceptable measurement coverage/cost.
  Separate recovery after sufficient new evidence from a promise to recover
  when no qualifying packets arrive. Freeze these limits before acceptance runs.

**Read / permitted changes:** current `src/tcpdelay/`, `src/monitor/tcpdelay.c`,
`Makefile`, `src/Makefile`, `.vscode/tasks.json`, and linked evidence; documentation
only. Do not restore the patch or access VMs for this order.

**Output and gate:** a written contract, capability matrix and bounded budgets.
The primary identifies feasible common APIs and resources needed for missing
rows. No architecture may be silently removed to make the design pass.

### ✓ TL-02 — Select and review the lifetime/loss protocol

**Problem:** making updates exclusive does not make lost handshake evidence safe.
Old state, in-flight records and later packets can disagree about which lifetime
is current. Independent LRU maps add eviction and publication hazards.

**Tasks:**

- ✓ **TL-02.1:** Compare a small number of concrete alternatives, including an
  ordered lifecycle event path and a bounded uncertainty/invalidation protocol.
  A capture-wide failure latch is only a candidate: specify its measurement
  outage, counter impact, recovery and behavior during sustained contention.
- ✓ **TL-02.2:** For each viable alternative, name the writer/owner of every state
  item, synchronization/publication point, loss indicator and ordering rule.
  Explain why losing a new SYN cannot leave the predecessor trusted. Include
  a record already reserved or submitted when loss is detected.
- ✓ **TL-02.3:** Define handshake retransmission, simultaneous-open, mismatched
  SYN/ACK, delayed predecessor events, ID exhaustion, map update failure,
  eviction and restart transitions. Specify FIN/RST handling or its explicit
  limits; a delayed termination packet must not erase a newer lifetime.
- ✓ **TL-02.4:** Audit map-value pointer lifetime across helper calls and unrelated
  flow eviction. Define safe ownership/snapshot/revalidation rules without
  assuming that lookup pointers provide a transaction across several maps.
- ✓ **TL-02.5:** Write deterministic event traces for loss, publication and recovery
  races. Have the designated reviewer challenge the invariants against those
  traces and the TL-01 capability baseline. Record rejected designs and why.

**Read / permitted changes:** saved patch and current producer/consumer callers;
design documentation and minimal test-only models. No production implementation
or VM work until the design gate passes.

**Output and gate:** one selected protocol, transition table, ABI proposal,
counter semantics, recovery bounds, resource budget and focused test list.
It must resolve the missed-SYN counterexample and use one portable algorithm.
If none satisfies those conditions, stop with the concrete blocker. Do not
silently choose a weaker algorithm or expand into a kernel/attachment redesign.

### ✓ TL-03 — Implement the kernel producer and shared record contract

**Problem:** stale departures and sampling state can survive tuple reuse; a
producer can emit a plausible record without a trustworthy lifetime.

**Tasks:**

- ✓ **TL-03.1:** Implement only the selected lifecycle protocol. Capture credible
  evidence at the intended packet observation point and classify loss before
  declaring subsequent measurement trustworthy. Define behavior when a SYN
  lacks a usable timestamp or a packet cannot be parsed.
- ✓ **TL-03.2:** Scope departure and thinning state to the selected lifetime.
  Preserve first-departure semantics, supported timestamp parsing, existing
  sampling cadence and ring-buffer wakeup behavior. Old entries may remain in
  bounded storage but must never match a replacement lifetime.
- ✓ **TL-03.3:** Define explicit record fields, initialization, event discrimination
  if needed, and fixed offsets within 64 bytes. Check map layouts and capacities.
  Use the same source for both BPF byte orders; do not add a CPU-specific branch.
- ✓ **TL-03.4:** Keep upload/pure-ACK accounting independent of timing confidence
  wherever the selected protocol allows. If recovery recreates the capture,
  specify and test how counter baselines restart and ACK intervals become
  unavailable rather than yielding a false rate.
- ✓ **TL-03.5:** Add focused tests for helper/map/ring failure and lifecycle branches
  using the production code or a justified seam. Audit cleanup on every path.
  Inspect compiled BPF instructions against TL-01's common baseline.

**Files:** `src/tcpdelay/tcpdelay.bpf.c`, `record.h`, focused filter tests and their
Make targets. Shared-header ownership is coordinated with TL-04.

**Output and gate:** reviewed producer plus branch tests and instruction inspection.
No stale lifetime may be emitted as trusted after detected reset-evidence loss.
Current CAKE accounting behavior must retain its existing regression checks.
Keep producer/consumer ABI changes together in a coherent program commit;
intermediate incompatible artifacts are never deployment candidates.

### ✓ TL-04 — Integrate userspace calibration and degraded operation

**Problem:** recognizing lifetimes in BPF does not help if the consumer accepts an
old record, retains old clock floors, or tells the controller stale data is valid.

**Tasks:**

- ✓ **TL-04.1:** Validate object/map/record compatibility before attach and before
  record consumption. Reject stale objects and malformed/unknown records even
  when their byte length matches; do not infer compatibility from size alone.
- ✓ **TL-04.2:** Reset one tuple's calibration only on the selected credible lifetime
  transition. Reject predecessor records without refreshing accepted-sample
  freshness or LRU order. Preserve legitimate timestamp wrap and reordered-data
  rejection within a lifetime.
- ✓ **TL-04.3:** Apply uncertainty/invalidation before publishing controller queue
  validity, including records already buffered when loss becomes visible.
  Implement the reviewed recovery procedure and expiry/resource bounds.
- ✓ **TL-04.4:** Keep fping operational and report degradation/recovery through
  existing logging. Do not flood syslog or terminate the daemon for optional
  capture failure. Preserve qdisc/interface/model-change reset behavior.
- ✓ **TL-04.5:** Add estimator and monitor tests covering lower and higher replacement
  timestamp offsets, old queued records, unavailable evidence, counter restart,
  capture reopen and recovery. Reuse existing baseline/confidence tests unchanged.

**Files:** `src/tcpdelay/{capture.c,capture.h,estimator.c,estimator.h,record.h}`,
`src/monitor/{tcpdelay.c,loop.h}` and matching tests as required by the design.
Do not move controller policy into measurement code.

**Output and gate:** consumer and monitor agree with the producer's ordering
contract; unavailable lifetime evidence never supplies valid directional input.
Focused tests, relevant sanitizer checks and unchanged upstream replay pass.

### TL-05 — Prepare the bounded kernel regression harness

**Problem:** synthetic userspace tests cannot prove kernel departure matching,
publication, JIT execution or target cleanup. Earlier harness assumptions and
repeated VM attempts must not be repeated.

**Tasks:**

- ✓ **TL-05.1:** Adapt useful historical runner code to the new ABI without importing
  the old implementation's conclusions. Exercise the exact built object and
  the real consumer path; include source/object checksums.
- **TL-05.2:** Cover active/passive opens, retransmissions, simultaneous open,
  sequence/timestamp wrap, lower/higher replacement timestamps, colliding old
  departures, delayed predecessor records and capture bootstrap/restart.
- **TL-05.3:** Force lifecycle-evidence loss, ring-full, failed map updates,
  eviction, capacity pressure, ID exhaustion and the selected protocol's
  contention/recovery paths. Include a record in flight during invalidation.
- **TL-05.4:** Select a bounded set of cross-CPU event schedules and unrelated-flow
  LRU churn. Derive assertions from actual helper/LRU contracts, including
  batch eviction; do not require exhaustive scheduler interleavings.
- **TL-05.5:** Write cases, fixed repetition counts, deadlines, metadata, cleanup
  and stopping rules before any VM mutation. Strict-compile the harness, check
  shell syntax and failure cleanup locally, then obtain bounded review.

**Files:** tools and a new before/after evidence directory under `profiling/`;
no production logic changes in the harness order.

**Output and gate:** locally reviewed run plan and harness. Use at most one short
preflight followed by one planned acceptance batch per new environment. After two
harness/environment failures, stop and reassess; rerun only failed cases with a
specific corrected input. No new soak campaign follows a regression automatically.

### TL-06 — Verify architecture coverage and kernel behavior

**Problem:** a passing i386 or arm64 test cannot establish support for other
instruction sets, byte orders or userspace layouts.

**Tasks:**

- **TL-06.1:** Run focused host checks and sanitizer builds; compile ABI assertions
  with each selected target compiler. Verify serialization/packet fixtures on
  both byte orders, not just a successful `bpfeb` compilation.
- **TL-06.2:** Build packages for the TL-01 matrix using each exact target SDK.
  Run the existing x86 and Filogic build tasks concurrently. Additional SDK
  paths must be explicitly supplied; do not guess locations or modify unrelated
  toolchains. Missing resources remain blockers, not a reason to drop coverage.
- **TL-06.3:** On representative kernels, record feature/configuration evidence,
  verifier output, object identity and actual JIT/interpreter mode. Exercise
  same-lifetime and replacement-lifetime paths plus one forced loss/recovery
  case. Run the TL-05 functional batch on the existing x86 and arm64 VMs.
- **TL-06.4:** Exercise actual parsing and map/ring transfer on at least one
  big-endian kernel with 32-bit ABI coverage. Obtain missing runtime resources
  through the normal scope/permission process; do not claim a host fixture is
  a big-endian kernel execution.
- **TL-06.5:** Test unavailable capabilities and incompatible objects as separate
  failure cases: daemon/fping survive, TCP input becomes unavailable, and the
  user can identify the reason. Keep those outcomes separate from support passes.

**Output and gate:** a matrix with separate build, ABI, kernel, execution-mode,
functional and unverified columns. Common source and semantics pass on verified
targets; every missing required check is visible. Do not mark broad portability
complete with only the two existing little-endian machines.

### TL-07 — Measure cost and make final acceptance reviewable

**Problem:** a correct measurement that loses most observations, falls back to a
slow interpreter, or repeatedly resets under load can be operationally unusable.

**Tasks:**

- **TL-07.1:** Prepare one bounded before/after cost batch using identical traffic
  and declared repetitions. Record CPU affinity, JIT mode, actual program calls,
  BPF time per call and total CPU, daemon CPU, map/ring memory, record loss,
  invalidations, observation coverage and recovery duration. Explain memory
  accounting; verifier instruction counts are not runtime CPU cost.
- **TL-07.2:** Exercise sustained download, upload and bidirectional load, tuple
  reuse during load, capture restart and qdisc disappearance/reappearance after
  preserving exact original qdisc state. Check service, process, log, rate bounds
  and recovery. ARM emulation supplies functional results only, not cost claims.
- **TL-07.3:** Compare results against the budgets fixed in TL-01/TL-02. Stop on
  material regression; do not retune controller policy to make lifetime handling
  look successful. Any rate-control performance claim needs its own valid
  before/after latency and throughput comparison and the existing capacity rule.
- **TL-07.4:** Have the designated reviewer check final producer/consumer diff,
  invariants, resource ownership and evidence limits. Update the finding's
  status only to the extent verified; document remaining passive-observation
  limits, missing architecture checks and deployment restrictions.

**Output and gate:** reviewable program changes and separate tools/evidence
commits, a completed acceptance checklist, and an honest support matrix.
No version bump, remote operation, router installation or release is included.

## Checks and operational boundaries

✓ Focused behavioral tests accompany TL-03/TL-04 and passed at the end of the
implementation pass, including replay and native sanitizer checks. Reuse those
results while their inputs remain unchanged. Harness checks use strict warnings,
clang-format stability and shell syntax. Existing SDK commands remain in
[.vscode/tasks.json](../../../.vscode/tasks.json). No source-only review implies execution.

Before any VM mutation, the run plan must require fresh service/process/package/
configuration/link/route/qdisc snapshots and exact restoration on x86. Prefer
isolated test artifacts to package replacement. Preserve `/tmp/sqm-mon-test.log`
in place, record its inode, use the isolated hard-link log directory, and verify
the inode after cleanup. Leave the user's `tail -f` alone. Bound transfers and
commands; remove only test-owned files/processes. The arm64 installation exception
and all Windows, branch, remote and deployment limits remain those in `AGENTS.md`.

The separate [ACK-control regression](../../2026-10-05-ack-control/README.md)
remains open. Its presence neither expands these work orders nor permits a new
claim of production readiness from lifetime tests alone.

If a work order needs a larger design or new authority, stop at that gate,
describe the concrete change and retain evidence. Preserve policy and existing
accepted fixes. Outside the one-time implementation pass, revert an unsuccessful
tuple-lifetime implementation attempt under the user's scope stop condition.
The completed implementation remains a reviewable checkpoint while acceptance
gates or usage limits leave validation pending; do not present it as complete.
No standing authorization to continue across these gates is implied.
