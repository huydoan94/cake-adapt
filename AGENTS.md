# cake-adapt — Agent Guide

**At session start, resumption after a long break (more than 2 hours), the primary agent reads
[`COLLABORATION.md`](COLLABORATION.md) and [`HANDOFF.md`](HANDOFF.md) together
with this guide; in `COLLABORATION.md`, the shared roles and your own provider's
workflow are enough at session start. Before editing code, read
[`CODING_STYLE.md`](CODING_STYLE.md); read the topic files below and other
documentation only as the task needs them.
Inspect the current worktree and identify unfinished work before continuing.
Delegated agents use a scoped task brief with the applicable policy excerpts.**

Claude and Codex each have an explicit workflow in `COLLABORATION.md`. Select
the workflow for the active primary agent; the Claude model requirement does
not apply to Codex. Preserve user decisions and scoped approvals in `HANDOFF.md`
so resumption does not depend on the user repeating earlier instructions.

This file holds the rules that always apply. The detailed rules live in topic
files under `docs/agents/`; they are as binding as this file. Read the ones a
task touches before acting:

| Before you… | Read |
| --- | --- |
| change controller behaviour, defaults, records or reflectors | [`docs/agents/upstream.md`](docs/agents/upstream.md) |
| add, move or own code; touch interfaces, ingress or kernel integration | [`docs/agents/modules.md`](docs/agents/modules.md) |
| edit C | [`docs/agents/c-style.md`](docs/agents/c-style.md) |
| build, test, change an SDK or the build helpers | [`docs/agents/build-and-tests.md`](docs/agents/build-and-tests.md) |
| plan controller or TCP-delay estimator work, or pick up an open item | [`docs/agents/status.md`](docs/agents/status.md) |
| touch a test VM | [`docs/agents/vm-testing.md`](docs/agents/vm-testing.md) |

Searches with `rg` (and tools built on it) skip raw evidence, generated
flowchart output, replay fixtures and `docs/history/` through `.ignore`; read
such a file by its path or search it with `rg --no-ignore`.

Rules from those files that apply even when you do not read them:

- Never bump `PKG_VERSION` or `PKG_RELEASE`, copy release artifacts, deploy, or
  publish unless the user explicitly asks.
- New downloads, SDK/feed changes and installations need the user's approval
  for that exact scope; development, SDK validation and VM testing default to
  x86 (Filogic/arm64 only on an explicit request).
- Never run cake-autorate and cake-adapt, or any two controllers, against the
  same CAKE qdiscs.
- Unit tests are part of every behavioral change; keep strict warnings and
  clang-format-stable C.
- Recommend plain CAKE `ack-filter` only; never `ack-filter-aggressive`.

## Product contract

`cake-adapt` is an OpenWrt-native C daemon for adapting the bandwidth of
existing CAKE qdiscs. The package, executable, service, syslog identifier, and
UCI package are all named `cake-adapt`, as is the GitHub repository; a local
checkout may still be named `sqm-mon` for historical reasons. The daemon
package is built in three variants that each provide `cake-adapt`: `cake-adapt`
(no MQTT), `cake-adapt-mqtt` and `cake-adapt-mqtt-ssl`. A second package,
`luci-app-cake-adapt`, adds the LuCI pages; all are released together with
the same version.

CAKE remains a Linux kernel qdisc. Never reimplement packet scheduling in
userspace.

Development proceeds in this order:

```text
observe -> verify -> control existing CAKE -> replace cake-autorate behavior
        -> optionally replace the required parts of sqm-scripts
```

The current daemon observes and controls CAKE instances created by the existing
SQM setup. A root `cake_mq` (multi-queue CAKE) is controlled like a single
CAKE through its shared settings; the kernel splits its rate equally among
busy queues (`profiling/2026-10-10-cake-mq`). It does not yet own CAKE or IFB
creation, ingress redirection, `ctinfo`, `mirred`, or cleanup. Do not advance that boundary without an explicit
user decision and verified behavior at the current stage.

Replacing all historical `sqm-scripts` behavior is not a goal. If native SQM
ownership is added later, implement only the deployment's required behavior.


## Upstream behavioral reference (summary)

[cake-autorate](https://github.com/lynxthecat/cake-autorate) is the origin of
the controller design and the baseline to beat: since 2026-10-01 the goal is to
minimize bufferbloat, not to port faithfully. Departures are opt-in (or a
documented deliberate default), measured on the testbed with raw evidence under
`profiling/`, and documented; with every departure off, the replay test keeps
matching within its tolerances. `fping` is the only supported pinger; rate
adjustment stays opt-in. The full rules are in
[`docs/agents/upstream.md`](docs/agents/upstream.md).

## Architecture and ownership

Keep this pipeline explicit:

```text
measurement -> controller decision -> desired CAKE state -> kernel update
```

- Measurement code does not choose rates.
- The controller does not know about UCI, filesystems, sockets, netlink, `tc`,
  interfaces, IFB, or OpenWrt.
- The CAKE layer does not measure traffic or latency.
- The netlink layer contains transport and message mechanics, not policy.

`src/` is organized by subsystem. `main.c` stays at the top level; every other
module belongs to exactly one subsystem directory. Put new code in the
directory that owns the concept, and do not create a new directory or nested
layer without a real ownership reason. Do not add unnecessary
abstraction layers, wrapper libraries, or one-use helpers.

Include project headers relative to `src/`, for example
`#include "latency/tracker.h"`. A header shared only inside one subsystem stays
in that directory and is not included from outside it.


## Configuration rules

The shipped UCI file must remain safe and readable:

- `enabled`, `interface` (or a complete `ul_if`/`dl_if` pair), both adjustment
  flags, both min/base/max rate sets, and the reflector list remain explicit in
  the template.
- Keep optional settings commented and grouped by purpose.
- Optional defaults must match cake-autorate unless a deliberate difference is
  documented in code and user-facing documentation.
- Require `minimum <= base <= maximum` and values representable by CAKE.
- Validate configuration before starting control.
- Never embed developer-specific paths, usernames, SDK locations, or secrets in
  committed files.

Use observation-only mode when either the configuration or runtime behavior has
not yet been verified. Never run rate control concurrently with cake-autorate
or another process controlling the same qdiscs.

## Logging and failure behavior

All application logging goes through `logging/log.c`.

Syslog must make these conditions visible even when detailed file output is
disabled:

- service start and stop;
- disabled configuration when started by `procd`;
- invalid or unreadable configuration;
- missing interfaces or CAKE qdiscs;
- measurement, `fping`, and netlink failures; and
- degraded operation or failed kernel changes.

High-frequency measurement records remain optional. When changing kernel state,
read it back or otherwise verify the effective configuration where practical.

One failed optional measurement must not terminate the daemon. Continue with
the available directions and report degraded state. Fatal errors are reserved
for states where continued operation would be incorrect. Bound all operations
that can wait; never wait forever for an interface, qdisc, child process, or
netlink reply.

Preserve log-file inode continuity during reset and rotation so `tail -f`
remains attached.

Default logs are `/var/log/cake-adapt.log` for the historical `main` section
and `/var/log/cake-adapt.<section>.log` for named instances. `SIGUSR1` exports
the active and retained logs; `SIGUSR2` resets them in place. Compatibility with
cake-autorate analyzers applies to structured records, not local filenames.

## Delegation and sandbox

At session start, resumption, and after a long break, the primary agent reads
`COLLABORATION.md` and `HANDOFF.md` alongside this guide and inspects the current
worktree and unfinished work. Follow the matching provider's roles and handoff
workflow for diagnosis, implementation, review, verification, and acceptance. Do not run parallel
writers on overlapping files. Delegation does not expand the user's granted
authority or override the branch, remote, Windows, version, and deployment
restrictions in this guide.

Use the matching Claude or Codex workflow and context budget in
`COLLABORATION.md`: short per-task files, context-free delegation when used,
at most one implementer and one bounded review, as each workflow defines,
and only the checks selected for the affected behavior. Reports and evidence
should be sufficient and concise. Broad reviews, exhaustive testing, detailed
documentation and instructions to finish every plan are task-specific requests,
not standing rules unless the user explicitly makes them so. Preserve required
safety checks and runtime evidence for behavioral claims.

Sandbox failures such as `Read-only file system` or `socket: Operation not
permitted` are not product failures. Use an existing narrow approval or return
the exact command and justification to the primary agent. Never work around a
denied approval or request a broad shell/interpreter prefix.

## Test machines and permissions

All work happens only in WSL and the test VMs. Never read, list, write or run
anything under `/mnt/c`, `/mnt/d` or any other Windows mount, and never touch
the Windows host, unless the user says exactly what to do. The only allowed
access to the Windows host is pinging `192.168.56.1` (also as a test
reflector). Never connect to the user's production routers, and never write
their hostnames in any file, commit or report.

- **x86 VM, `192.168.56.2`:** the primary test VM (OpenWrt 25.12 x86, a 32-bit
  kernel). It and its clone panic about once per 25-60 minutes of load in the
  i386 exception-entry path, with or without cake-adapt loaded; a reboot
  mid-test is not evidence against cake-adapt. Check the serial console log,
  copy it before the VM is powered off (VirtualBox truncates it on start), and
  rerun the lost run.
- **x86 VM clone, `192.168.56.5`:** a second x86 VM for parallel work, such as
  soak tests.
- **arm64 VM, `192.168.50.5`:** OpenWrt 25.12 `armsr/armv8`, which ran the
  Filogic build before the user deployed it on real routers. It is no longer
  needed before a deployment. Its CPU is emulated, so it gives functional
  results only, never timings. Use it only for an explicitly requested arm64
  check; x86 remains the default. The user resets it.

During long VM runs, move each run's results to disk (for example under
`/root`) as soon as it ends, so a panic loses only the run in progress, and
remove them after collecting them. `tools/testbed/` holds the emulated bloated
ISP (`testbed.sh`), a full controlled run (`run.sh`), the filter-cost
benchmark (`bench.sh`, using `kernel.bpf_stats_enabled`) and the scoring
scripts (`queues.py`, `analyze.py`, `shapers.py`); its README explains them.

Permissions the user has granted:

- deviate from cake-autorate to reduce bufferbloat, as described above;
- restructure `monitor/` and move responsibilities between its files and the
  controller (done on 2026-10-03: one `struct monitor`, a part per file, load
  classification in the controller);
- change packages, configuration and files on the test VMs without restoring
  them, recording what changed;
- rebuild the VM image with `openwrt-dev-builder` (never touch
  `openwrt-image-builder`).

Keep program changes and measurement material (tools, evidence) in separate
commits, and keep evidence to before/after comparisons. Branch, remote, push,
and version management stay with the user.

## Change discipline

- Work only on the currently checked-out local branch. Never inspect, switch,
  modify, rewrite, delete, or otherwise touch any other branch or remote.
  Never fetch, pull, push, force-push, or contact a remote; branch and remote
  management belongs exclusively to the user.
- Never rebase, squash, amend or otherwise rewrite history; a follow-up is a
  new commit on top.
- Commit only when asked or as part of an authorised task. Stage with explicit
  pathspecs, never commit a temporary plan doc (such as
  `docs/superpowers/plans/`), and add no `Co-Authored-By` or other AI
  attribution trailer.
- A temporary plan doc the user keeps in the repository wins over lagging
  `AGENTS.md` or README text: point out the conflict and follow the plan.
- Treat `AGENTS.md` as user-owned policy; edit it only when explicitly asked.
- Inspect `git status` first and preserve unrelated user changes.
- Verify a surprising observation once before changing known-working code.
- Keep targeted changes reviewable; do not mix an unrelated refactor into a
  feature or bug fix.
- During an explicit cleanup pass, remove duplication and unreachable checks,
  but retain guards for real external failures.
- Do not silently change architecture, defaults, or compatibility behavior.
- If a request conflicts with this guide, explain the conflict before changing
  direction.
- Do not commit machine-specific files or build outputs. Maintain the tracked
  generated flowcharts when their sources change; include them in a commit only
  when committing that change is authorized.

## Working style and collaboration

- Optimize for quick comprehension: small functions, clear ownership, readable
  units, and an execution path that can be followed without unnecessary
  indirection. Use `CODING_STYLE.md` for the reusable engineering principles;
  this guide and `.clang-format` govern the current project's conventions.
- During cleanup, prove callers and validated invariants before deleting a
  condition or code path. Check resource lifetime, leaks, memory corruption,
  and performance as well as behavior; code movement alone is not evidence
  that the result is equivalent.
- Plan substantial work in an explicit order with acceptance criteria and
  evidence requirements. When a milestone or stop point is agreed, track it,
  report completion and remaining work, and honor the requested stopping point.
  Keep commits focused when committing is authorized.
- Verify changes in increasing realism as appropriate: focused host tests,
  sanitizers, SDK builds, controlled VM runs, then real-world validation.
  Simulation identifies candidates; replay checks matching decisions; live
  tests establish runtime behavior. State the limits of each result.
- Preserve raw graphs, logs, traces, and before/after measurements so the user
  can independently judge conclusions. Update documentation and generated
  flowcharts when the documented behavior or ownership changes.
- Lead reports with the outcome and explain consequential decisions with
  concrete evidence. Separate observations, assumptions, and unverified
  claims; identify completed work, remaining work, and blockers plainly.
  Keep updates concise and avoid filler or unsupported praise.
- Continue authorized work without repeated confirmation. Explain a material
  scope change or conflict before acting; do not infer permission for branch,
  remote, version, publication, deployment, or Windows operations beyond the
  user's explicit instructions.
- Keep `HANDOFF.md` current with decisions, approvals, verified results and
  unfinished work. Follow the Codex workflow in `COLLABORATION.md` when using
  Codex; unavailable Claude tools do not require a repeated permission question.
- After laying out options, ask which one the user wants and wait for the
  answer before acting on any of them.
- When asked what something means, answer with a concrete worked example.
- Run long VM runs and builds in the background and wait for their completion
  instead of blocking or polling.
