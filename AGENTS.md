# cake-adapt — Agent Guide

**This guide extends the user's general working definitions in
`~/ai-working-definitions/` (outside this repository; see its `README.md`).
Where both apply, this repository's rules win.** At session start, resumption
after a long break (more than 2 hours), or after lost context, the primary agent
reads this guide, [`HANDOFF.md`](HANDOFF.md) and
[`COLLABORATION.md`](COLLABORATION.md), which names the general files to read
at session start. Before editing code, read [`CODING_STYLE.md`](CODING_STYLE.md).
Inspect the current worktree and identify unfinished work before continuing.
Delegated agents use a scoped task brief with the applicable policy excerpts.
Preserve user decisions and scoped approvals in `HANDOFF.md` so resumption does
not depend on the user repeating earlier instructions.

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
- Before reporting or committing a C change, run `sh tools/style-check.sh` and
  fix what it reports: no member-access chain mixing `->` and `.` (take a local
  pointer), no literal `true`/`false` argument, clang-format-stable files. These
  are the findings review keeps repeating.
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
matching within its tolerances. The daemon's own ICMP probes are the default
pinger, with `fping`, `fping-ts` and `irtt` as fallbacks; rate adjustment
stays opt-in. The full rules are in
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
abstraction layers, wrapper libraries, or one-use helpers. Keep one concern
per file: when a file holds several (a transport and the data it serves, a
session and its process mechanics), split it by concern within its
subsystem, and give each part of a shared state its own header (user,
2026-10-22).

Include project headers relative to `src/`, for example
`#include "latency/tracker.h"`. A header shared only inside one subsystem stays
in that directory and is not included from outside it.

Named values live in `src/constants/`, one header per topic (`units.h`,
`options.h`, `logging.h`, `latency.h`, `tcpdelay.h`, ...), never in the file
that uses them, even when only one file does (user decision 2026-10-22): a
constant is found by its topic, and its prefix names the topic where it can.
Each file includes the topic headers it uses. Only feature-test macros,
include guards, function-like macros and the UCI defaults in
`config/defaults.h` stay outside; `make check` enforces it
(`tests/test_constants.sh`).


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
  committed files. References to `~/ai-working-definitions/` are intended: the
  repository serves the user's own agents, and other people bring their own
  rules (user decision 2026-10-13).

Use observation-only mode when either the configuration or runtime behavior has
not yet been verified. Never run rate control concurrently with cake-autorate
or another process controlling the same qdiscs.

## Logging and failure behavior

All application logging goes through `logging/` (`log.h` for messages,
`records.h` for the structured records).

Syslog must make these conditions visible even when detailed file output is
disabled:

- service start and stop;
- disabled configuration when started by `procd`;
- invalid or unreadable configuration;
- missing interfaces or CAKE qdiscs;
- measurement, pinger (native ICMP or `fping`), and netlink failures; and
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

Follow the general collaboration rules (roles, direct work by default, one
bounded reviewer for risky changes, briefs, sandbox failures) and this
project's risky areas and helpers in `COLLABORATION.md`. Delegation never
expands the user's authority or overrides this guide's branch, remote, Windows,
version and deployment limits.

## Test machines and permissions

All work happens only in WSL and the test VMs. Never read, list, write or run
anything under `/mnt/c`, `/mnt/d` or any other Windows mount, and never touch
the Windows host, unless the user says exactly what to do. The only allowed
access to the Windows host is pinging `192.168.56.1` (also as a test
reflector). Never connect to the user's production routers, and never write
their hostnames in any file, commit or report.

- **x86_64 VM, `192.168.56.2`:** the primary test VM, on the OpenWrt
  snapshot (x86_64), so that cake-adapt follows the next OpenWrt release (user
  decision 2026-10-27). Its image comes from `~/openwrt/custom/vm/build.sh`,
  with perf, strace, gdb, bpftool, the testbed's kernel modules and tools, and
  an SFTP server.
- **x86 VM clone, `192.168.56.5`:** a second VM for parallel work, such as
  soak tests and profiles. Until the user rebuilds it from the snapshot image
  it runs OpenWrt 25.12 x86, a 32-bit kernel that panics about once per 25-60
  minutes of load in the i386 exception-entry path, with or without
  cake-adapt loaded; a reboot mid-test there is not evidence against
  cake-adapt. Check the serial console log, copy it before the VM is powered
  off (VirtualBox truncates it on start), and rerun the lost run.
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
- restructure `daemon/` (named `monitor/` until 2026-10-27) and move
  responsibilities between its files and the controller (done on 2026-10-03:
  one `struct monitor`, now `struct daemon`, a part per file, load
  classification in the controller);
- change packages, configuration and files on the test VMs without restoring
  them, recording what changed;
- rebuild the VM image from the snapshot source in `~/openwrt` with
  `custom/vm/build.sh`, pulling the source first (never touch
  `openwrt-image-builder`, and leave the Filogic `custom_build.sh` and its
  lists alone).

Keep program changes and measurement material (tools, evidence) in separate
commits, and keep evidence to before/after comparisons. Branch, remote, push,
and version management stay with the user.

## Change discipline

The general rules are in `~/ai-working-definitions/change-discipline.md`. These
must hold even when that file is not read:

- Work only on the checked-out local branch; never touch another branch or a
  remote, and never fetch, pull or push.
- Never rebase, squash, amend or otherwise rewrite history.
- Commit only when asked or as part of an authorised task, with explicit paths;
  never commit a temporary plan document (such as `PLAN-*.md` at the root or
  `docs/superpowers/plans/`) or `HANDOFF.md` (it serves only the user's own
  agents); no `Co-Authored-By` or other AI trailer.
- `AGENTS.md` is user-owned policy: edit it only when asked. Explain a conflict
  between a request and this guide before changing direction.
- Inspect `git status` first and preserve unrelated user changes.
- Keep the tracked generated flowcharts current when their sources change;
  commit them only when that change is authorised.

## Working style

The general style is in `~/ai-working-definitions/working-style.md` (report
outcome first with evidence; ask after laying out options and wait; explain with
worked examples; verify in increasing realism; keep raw evidence) and
`efficiency.md` (one task per session, quiet tools, waiting without polling).
In this project: state the limits of each result (simulation finds candidates,
replay checks matching decisions, live runs establish behaviour), and update the
documentation and generated flowcharts when behaviour or ownership changes.
