# cake-adapt — Agent Guide

## Product contract

`cake-adapt` is an OpenWrt-native C daemon for adapting the bandwidth of
existing CAKE qdiscs. The package, executable, service, syslog identifier, and
UCI package are all named `cake-adapt`, as is the GitHub repository; a local
checkout may still be named `sqm-mon` for historical reasons.

CAKE remains a Linux kernel qdisc. Never reimplement packet scheduling in
userspace.

Development proceeds in this order:

```text
observe -> verify -> control existing CAKE -> replace cake-autorate behavior
        -> optionally replace the required parts of sqm-scripts
```

The current daemon observes and controls CAKE instances created by the existing
SQM setup. It does not yet own CAKE or IFB creation, ingress redirection,
`ctinfo`, `mirred`, or cleanup. Do not advance that boundary without an explicit
user decision and verified behavior at the current stage.

Replacing all historical `sqm-scripts` behavior is not a goal. If native SQM
ownership is added later, implement only the deployment's required behavior.

## Upstream behavioral reference

[cake-autorate](https://github.com/lynxthecat/cake-autorate) and its community
are the source of the controller design and operational behavior being ported.
Honor that work in user-facing documentation and link upstream when describing
the origin of the algorithm.

Treat upstream cake-autorate as the reference for:

- load classification and rate decisions;
- latency baseline and delta tracking;
- bufferbloat detection and refractory periods;
- reflector selection, health, and replacement;
- idle, sleep, stall, and recovery behavior;
- defaults and configuration meaning; and
- profiling/statistics log formats.

Port behavior deliberately and verify it. Preserve record names, field order,
units, headers, and content exactly where cake-autorate log compatibility is
intended. Document and test intentional differences instead of silently
diverging or attributing local bugs to upstream.

Keep every supported cake-autorate option in the typed UCI model. Parsing an
option does not mean its behavior is implemented; do not claim support until
the corresponding path is tested. `fping` is the only supported pinger for now,
and reflector targets come from local UCI configuration rather than a remotely
retrieved list.

Upstream may be consulted during implementation, but production code, init
scripts, and configuration must never source, execute, or read files from an
installed cake-autorate instance. Use the upstream name for attribution and
compatibility descriptions, not for local identifiers, filenames, or runtime
dependencies.

One deliberate default difference is safety-related: upload and download rate
adjustment remain opt-in.

Do not add `startup_wait_s`. React to interface/qdisc readiness through kernel
lifecycle state, and let the activity controller idle naturally when no traffic
is passing.

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

### Module boundaries

- `main.c`: CLI, startup, logging initialization, configuration loading,
  top-level lifecycle, and orderly shutdown only.
- `monitor/`: uloop orchestration and coordination between measurements,
  controller decisions, qdisc lifecycle, timers, and signals. All state is one
  `struct monitor` in the private `loop.h`, passed to every function; each
  file owns one part of it and prefixes its functions with that part's name.
  `monitor.h` is the only public header.
  - `monitor.c`: loop setup and teardown, the traffic tick
    (`monitor_tick`), the activity state machine, CPU and log timers, and
    signals.
  - `links.c` (`links`): both directions' CAKE discovery, achieved rates,
    compensated traffic cadence, and qdisc lifecycle events.
  - `control.c` (`control`): controller configuration and input, CAKE
    bandwidth changes with readback, minimum-rate enforcement, and controller
    records.
  - `pingers.c` (`pingers`): pinger start, output, exit, restart, setup grace,
    and per-reply latency processing.
  - `reflectors.c` (`reflectors`): latency trackers, reflector ordering,
    scheduled comparison and replacement, and health checks.
  - `tcpdelay.c` (`tcp`): the TCP capture, its queue estimate, and the upload
    ACK rate.
- `common/`
  - `constants.h`: shared semantic names, paths, modes, and state tokens; keep
    prose diagnostics, format strings, and module-owned record schemas local.
  - `utils.h`: trivial operations as `static inline` functions, such as
    saturating arithmetic, `mul_div`, percentages, rounding, and
    `ARRAY_SIZE`. A module never keeps its own copy of one.
  - `helpers.c`: generic operations too large to inline, such as numeric
    parsing, random selection, clocks, and rate conversions.
  - `error.c`: shared error-buffer formatting.
- `config/`
  - `config.c`: typed UCI loading and conversion through `libuci`; never
    parse `/etc/config/cake-adapt` manually.
  - `validate.c`: cross-option validation of a loaded configuration
    (`config_validate`), run before control starts.
  - `defaults.c` and `defaults.h`: built-in application and configuration
    defaults.
- `controller/`
  - `controller.c`: platform-independent autorate, congestion, and activity
    policy; keep it directly unit-testable with synthetic inputs.
  - `reflector.c`: reflector health, comparison, and rotation policy over
    latency trackers.
- `latency/`
  - `latency.c`: pinger session and child-process ownership, line splitting,
    and dispatch to the active backend. Children are reaped by the monitor
    through `uloop_process`. The only synchronous, bounded waits are shutdown
    (`latency_stop_now`) and a child whose pipe setup failed (`start_child`).
  - `fping.c` and `irtt.c`: backend-specific launch arguments and scheduling.
  - `pinger.h`: private interface between the session and its backends;
    each backend exports a `struct pinger_ops` (name, line parser, exit
    handler) that the session calls through.
  - `parser.c`: pinger output parsing into latency samples.
  - `tracker.c`: per-reflector baseline and delta EWMA tracking.
- `cake/cake.c`: CAKE discovery, state decoding, and CAKE-specific operations.
- `platform/`
  - `netlink.c`: low-level rtnetlink requests, replies, events, and timeouts.
  - `traffic.c`: counter samples and achieved-rate calculations only.
  - `cpu.c`: CPU sampling and usage calculations only.
- `logging/log.c`: all logging, cake-autorate-compatible records, rotation,
  export, and reset behavior.

Before adding a function, search for an existing equivalent. Consolidate
duplicate conversions, parsing, percentage, timing, and bounds logic in the
appropriate existing module.

## OpenWrt and kernel integration

Prefer supported libraries and kernel APIs over handwritten or process-based
substitutes:

- `libuci` for configuration;
- `libubox`/`uloop` for the event loop;
- rtnetlink/libnl for interface and qdisc operations;
- `procd` for service lifecycle; and
- kernel CAKE, IFB, `ctinfo`, and `mirred` facilities.

Do not repeatedly spawn `tc` or `ip` from the daemon. Use them only as external
diagnostic tools. Prefer event subscriptions such as `RTM_NEWQDISC` and
`RTM_DELQDISC` over periodic existence polling when the kernel already exposes
the lifecycle.

Use another OpenWrt feed library when it materially removes correct, maintained
code. Before adding a dependency, confirm it is available for supported targets
and that the reduction justifies flash, memory, and maintenance cost. Remove
dependencies that are no longer used.

### Interface convention

UCI normally contains one interface name:

```text
upload   = <interface>
download = ifb4<interface>
```

Derive the IFB name using Linux interface-size limits. A paired `ul_if` and
`dl_if` setting may override the derived pair for nonstandard deployments. Both
must be present; reject a partial pair. If `interface` and both overrides are
set, use the explicit pair and warn through syslog. Never hardcode a WAN device.

### Future ingress ownership

If cake-adapt later replaces SQM setup, preserve this ingress path:

```text
WAN ingress -> ctinfo zone 0 -> mirred redirect -> IFB -> CAKE
```

Outbound DSCP is stored in conntrack mark bits 0-5. Bits 6-31 belong to other
uses and must remain unchanged:

```text
ct mark = (ct mark & 0xffffffc0) | dscp
```

Do not replace this with an ingress path that loses DSCP restoration before
CAKE.

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

## C and naming style

Use C11, simple data flow, and explicit ownership. Prefer standard, libc,
OpenWrt, and kernel APIs over custom equivalents. Avoid clever macros and
defensive layers for impossible states already excluded by this program's own
validated call paths.

Use concise module-oriented identifiers:

```c
config_load();
controller_update();
tracker_update();
log_message();
```

Do not blanket-prefix functions, constants, or types with `sqm_mon_` or
`cake_adapt_`. Add a prefix only to prevent a concrete collision or ambiguity.
Move generic names such as `read_u32()` or `percentage_of()` to
`common/helpers` when they are shared; keep module-specific helpers local and
`static`.

Put shared semantic string values in `common/constants.h` and built-in defaults
in `config/defaults.c`/`config/defaults.h`. Keep complete diagnostics, format
strings, and module-owned schemas beside the code that uses them. Tests should
keep literal expected values when importing the production constant would make
the check tautological.

Do not repeat the program name in every log message because the backend already
identifies the service.

### Formatting

C follows the OpenWrt/Linux kernel style used by libubox and unetd, as encoded
in the repository's `.clang-format`. Format every changed C file with
clang-format (the SDK's `staging_dir/host/llvm-bpf/bin/clang-format` works):

- tabs for indentation, 8 columns wide, and lines up to 100 columns; string
  literals are never split;
- function braces on their own line, other braces on the statement line;
- no braces around a single-statement body, unless its condition spans lines:

```c
if (result < 0)
	return -1;
```

- parameters and arguments packed onto as few lines as fit, aligned with the
  opening parenthesis:

```c
int controller_update(struct controller *controller, const struct controller_input *input,
		      struct controller_output *output)
{
```

- compound conditions break after the operator and align with the condition.

Write code that reads like `unetd`'s `wg-user.c`: small static functions with
a module prefix that form a little internal API, and trivial operations from
`common/utils.h` instead of open-coded arithmetic. Comments should explain
formulas, invariants, units, ownership, or non-obvious kernel/upstream
behavior. Do not narrate obvious syntax. Do not reformat unrelated working
code during a focused change.

Keep strict warnings enabled:

```text
-Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror
```

Disable a warning only for a narrow, documented reason.

Generated `.o` and `.d` files and test binaries belong in `build/`, never in
`src/` or `tests/`.

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

## Build and tests

Use plain Make:

- repository `Makefile`: OpenWrt package definition;
- `src/Makefile`: daemon build; and
- `tests/Makefile`: focused host tests.

Test sources mirror the `src/` subsystem directories; move a test with the
module it covers. Test objects and binaries are written below `build/tests/`.

`tests/controller/test_replay.c` replays recorded cake-autorate `ac75f49`
traces from `tests/controller/fixtures/` and requires every decision to match.
Keep it passing; a controller change that alters a replayed decision is a
divergence from upstream and needs an explicit decision and documentation.
`extract-trace.py` and `scripted-fping.sh` reproduce the fixtures; the raw
source logs are in `profiling/controller-comparison/`.

Prefer the narrowest useful command. Do not trigger broad OpenWrt toolchain
builds for host-only work. Use the `.vscode/tasks.json` tasks: the host test
task runs `check`, `check-netlink` and `check-config`, and each SDK build task
cleans and compiles the package in the SDK beside the repository
(`../openwrt-sdk-x86`, `../openwrt-sdk-filogic`) or in `OPENWRT_SDK_X86` /
`OPENWRT_SDK_FILOGIC`. Do not guess other SDK locations, and keep absolute or
personal paths out of `.vscode/`. OpenWrt 25.12 is the only release target for
now. Never touch `openwrt-image-builder`.

Do not bump `PKG_VERSION` or `PKG_RELEASE`, copy artifacts, deploy, or publish a
release unless the user explicitly asks. When asked to build both SDKs, run the
x86 and Filogic tasks concurrently. Verify the selected artifact and its
destination with checksums.

The native `check-config` target needs both `libuci` and `libubox` development
headers but no native libraries. When the host lacks them, point `UCI_CFLAGS` at
a directory exposing the SDK's staged `uci.h` and `libubox/` headers; otherwise
treat the missing headers as an environment limitation, but the production SDK
build must still pass.

Unit tests are part of every behavioral change. Cover the affected branches,
especially controller state transitions, min/base/max bounds, congestion and
recovery, missing/invalid samples, counter reset, reflector failure, activity
state, and qdisc disappearance/reappearance.

Do not weaken correct production code to accommodate a host test environment.
Use a suitable test seam, mock, adapter, dependency, or on-target integration
test instead.

Before declaring work complete:

1. inspect existing behavior and relevant upstream behavior;
2. make the smallest coherent change;
3. run focused host tests, plus sanitizer builds for structural or
   memory-sensitive changes;
4. build the affected SDK package when appropriate;
5. test on the VM when runtime/kernel behavior changed; and
6. verify actual service, process, log, and qdisc state.

## Project status and deferred work

The cake-autorate parity and refactor sequence is complete. The controller
replays two recorded upstream traces with no mismatching decision, live
side-by-side VM runs agree per phase, and the final end-to-end VM run and
profiling are recorded under `profiling/` (`controller-comparison/` and
`2026-09-30/`), as is the resource comparison with cake-autorate
(`cake-autorate-resources/`). The flowcharts in `flowchart/` are generated from
`flowchart-data.json` by `generate.mjs`; regenerate them when event ordering or
module ownership changes.

Remaining work must remain behavior-first:

- keep raw logs, traces, profiler output, and reproducibility metadata for any
  new runtime claim;
- never run cake-autorate and cake-adapt concurrently against the same qdiscs.

Do not claim parity for a new behavior from host tests alone. `fping` remains
the only supported production pinger until every additional backend has
independent parser, lifecycle, fixture, and runtime verification.

## VM testing and delegation

For testing or deployment, delegate routine build/deploy/verification to one
available Luna-class lower-cost subagent with low reasoning effort and minimal
context. One agent may run independent x86 and Filogic builds concurrently.
Keep architecture, production changes, ambiguous diagnosis, destructive
actions, and final acceptance on the primary model.

Sandbox failures such as `Read-only file system` or `socket: Operation not
permitted` are not product failures. Use an existing narrow approval or return
the exact command and justification to the primary agent. Never work around a
denied approval or request a broad shell/interpreter prefix.

The authorized OpenWrt VM test log is always:

```text
/tmp/sqm-mon-test.log
```

At the beginning of a test:

1. stop every process writing that file;
2. create it once with `touch` if it does not exist, then record its inode;
3. truncate it in place with `: > /tmp/sqm-mon-test.log`;
4. create an isolated log directory and hard-link its expected
   `cake-adapt[.<section>].log` name to that file; and
5. point `log_file_path_override` at the isolated directory.

Never delete, move, replace, or pipe through `tee` to this file. Do not start or
stop the user's `tail -f`. At the end, confirm the inode is unchanged.

Before mutating the VM, record its service configuration, running processes,
installed package/version state, relevant files, and complete CAKE qdisc
handles, options, and rates. Before replacing an installed package, make sure
the exact rollback artifact exists. If it does not, extract and run the test
binary in isolation instead of replacing the package.

Bound every transfer and network operation. Prefer a controlled local endpoint
when public upload/download services are unreliable, verify that `ip route get`
selects the intended interface, and confirm the receiver actually got the test
payload. Exercise sustained download, upload, and bidirectional load; inspect
`LOAD`, `DATA`, and `SHAPER` records; verify rate bounds and recovery; and test
qdisc disappearance/reappearance only after capturing enough state to recreate
the exact original qdisc.

After the test, stop test processes, remove only test-created payloads, and
restore the exact original service, package, configuration, and qdisc state.
Confirm the test-log inode is unchanged and no test-owned daemon or `fping`
child remains. Use exact executable paths or `pidof` for process checks because
`pgrep -af cake-adapt` can match the audit command itself. Do not kill an
unrelated legacy process merely because it owns an `fping` child.

## Change discipline

- Work only on the currently checked-out local branch. Never inspect, switch,
  modify, rewrite, delete, or otherwise touch any other branch or remote.
  Never fetch, pull, push, force-push, or contact a remote; branch and remote
  management belongs exclusively to the user.
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
- Do not commit generated or machine-specific files.
