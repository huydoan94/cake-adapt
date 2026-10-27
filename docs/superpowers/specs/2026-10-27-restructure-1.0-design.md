# Whole-project restructure toward 1.0 — design

Date: 2026-10-27. Status: approved by the user on 2026-10-27, with `control.c` renamed to `shaper.c`.

## Why

Since 0.3.0 the code grew one change at a time. The user wants one pass over
the whole project, done at once rather than in chunks, so the result reads as
one design: one name per concept, one home per piece of logic, nothing
written twice. The release after it is the first stable version, 1.0.0.

A read of all 17,375 lines of `src/` (2026-10-27) found no large duplicated
subsystem: the 0.3.27 restructure already gave each concern its own file. What
remains is drift: the same thing under several names, a few things written two
or three times, some files with mixed jobs, and stale comments.

## Goals

1. **One name per concept**, under one naming rule (below), applied to every
   file, function, type, enum constant and header guard in `src/` and `tests/`.
2. **Nothing written twice**: the download/upload mirroring collapsed where
   the two directions behave alike, and each smaller duplicate kept once.
3. **One job per file**, with each file in the directory that owns it.
4. **No stale text**: comments, docs and flowcharts describe the code as it is.
5. **Version 1.0.0** once the restructure is verified.

## Non-goals

- No behaviour change of any kind: the same decisions, rates, records, log
  messages (text and level), syslog lines, timings and resource use.
- No new features and no tuning; the open items in `docs/agents/status.md`
  stay open.
- No change to anything outside the daemon's code: UCI option names, the
  `/etc/cake-adapt/exec` options, record names and columns, the ubus object and
  its fields, MQTT topics, the importer's input, the init script's interface,
  command-line options, package names and variants.

## Anti-goals (a failure even if it builds)

- Any replay mismatch, any byte of difference in the simulator's output, or
  any changed record or log line in the end-to-end run.
- A rename that makes a reader look in two places (for example, a function
  whose prefix names neither its file nor its type).
- New layers, wrapper libraries or one-use helpers (AGENTS.md).
- Doing it in pieces across sessions or releases (user, 2026-10-27).

## Constraints

Everything in `AGENTS.md`, `CODING_STYLE.md` and `docs/agents/` holds, in
particular:

- named values only in `src/constants/`, one header per topic;
- `sh tools/style-check.sh` clean, strict warnings, clang-format 21;
- headers included relative to `src/`; a subsystem-private header is not
  included from outside it;
- the pipeline measurement → controller → desired CAKE state → kernel stays
  explicit; the controller knows nothing of UCI, sockets, netlink or OpenWrt;
- program changes and evidence in separate commits; no history rewriting; no
  push; no AI trailer.

## The naming rule

1. A file's stem names its concern; a directory names a subsystem.
2. Every non-static function, type and enum in a file starts with the file's
   stem, or with `<subsystem>_<stem>` when the stem alone is too generic
   (`tcpdelay_capture_*` in `tcpdelay/capture.c`). A subsystem's main file
   (`controller/controller.c`) uses the subsystem name.
3. Enum constants are the enum's type name in upper case plus the value
   (`enum controller_activity_state` → `CONTROLLER_ACTIVITY_RUNNING`).
4. Header guards are `<DIR>_<STEM>_H_INCLUDED`.
5. Units as suffixes, as now: `_us`, `_bps`, `_bits`, `_bytes`, `_ratio_e6`.
6. Internal names of a UCI setting follow its option name, minus the
   direction prefix where the value now lives per direction (for example
   `shaper_rate_adjust_up_load_low` → `rate_adjust_up_load_low_ratio_e6`, no
   longer `rate_adjust_up_low_load_ratio_e6`).
7. One vocabulary for latency measurement: a **pinger** sends probes (the
   method, its executable, its queries); a **latency session** owns the
   running pinger; a **reflector** is a probed host; **native** means the
   daemon's own ICMP probes and **child** a pinger process. "Backend" and
   "method" as separate words go.

## Decided changes

### Directories and files

| Now | After | Why |
| --- | --- | --- |
| `monitor/` | `daemon/` | it holds the whole running daemon, not monitoring |
| `struct monitor` (`monitor/loop.h`) | `struct daemon` (`daemon/state.h`) | the same |
| `monitor/monitor.{c,h}` | `daemon/daemon.{c,h}` | main loop, traffic tick, activity |
| `monitor/status.c` + `monitor/publish.c` | `daemon/status.{c,h}` | one concept: the status object, built and served |
| `monitor/control.c` | `daemon/shaper.{c,h}` (`shaper_*`) | it drives the CAKE shaper with the controller's decisions; "control" read like "controller" |
| DATA/SUMMARY records, state-change messages and name tables from `control.c` | `daemon/report.{c,h}` | `shaper.c` keeps feeding the controller and applying bandwidth |
| `monitor/tcpdelay.{c,h}` | `daemon/tcp.{c,h}` | its functions are `tcp_*`; avoids two `tcpdelay` files |
| other `monitor/*` | `daemon/*`, same stems | |
| `common/utils.h` | `common/arith.h` (saturating, min/max, rounding, ratios) and `common/convert.h` (unit and time conversions) | names that say what is inside |
| `common/helpers.{c,h}` | split by concern, below | a grab bag of 13 functions |
| `parse_unsigned` | `common/text.{c,h}` | used by config and the parser |
| `read_clock_us`, `interval_elapsed` | `common/clock.{c,h}` | |
| `bps`, `serialization_us`, `fraction_to_ratio_e6`, `load_ratio_e6` | `common/rate.{c,h}` | |
| `random_below`, `shuffle` | static in `daemon/reflectors.c` | its only user |
| `response_monotonic_us`, `response_stale` | `latency/response.{c,h}` | used by the native pinger and the pinger session |
| `thread_join_within` | `platform/thread.{c,h}` | used by the log writer, MQTT and ubus threads |
| `absolute_path` | `platform/path.{c,h}` | used by config and main |
| `lock_pid_file` | static in `platform/instance.c` | its only user |
| `latency/pinger.h` (private ops) | `latency/children.h` | it is the child-pinger interface |
| method queries in `latency/latency.{c,h}` | `latency/pinger.{c,h}` (public) | rule 7 |
| `tests/monitor/` | `tests/daemon/` | mirrors `src/` |

### Renames inside files

- `enum pinger_method` stays; `latency_method()` → `pinger_method_parse()`,
  `latency_method_name()` → `pinger_method_name()`, `latency_native()` →
  `pinger_is_native()`, `latency_backend_executable()` →
  `pinger_executable()`, `latency_shared_delay()` → `pinger_shared_delay()`,
  `latency_timestamps()` → `pinger_uses_timestamps()`,
  `latency_check_backend()` → `pinger_check()`.
- `CONTROLLER_RUNNING/IDLE/STALL` → `CONTROLLER_ACTIVITY_RUNNING/IDLE/STALL`;
  `activity_init/update` → `controller_activity_init/update` (public type).
- `health_*` in `controller/reflector.c` → `reflector_health_*`.
- `monitor_degraded/recovered` → `daemon_degraded/recovered`.
- `controller_config` rate fields renamed by rule 6.
- Every remaining function, type and constant checked against the rule.

### Duplicates removed

- **Directions.** A `enum direction { DIRECTION_DOWNLOAD, DIRECTION_UPLOAD,
  DIRECTION_COUNT }` in `common/direction.h`, with its short and long names.
  Paired `download`/`upload` members become arrays indexed by it in the
  controller (config, input, output, state), the latency tracker and
  observation, the daemon's links and status, the TCP estimate and the record
  structs. Code that treats both alike becomes one loop; code that does not
  (shared-delay attribution, the ACK share on download) indexes explicitly.
  Record columns keep their order. The flat UCI config struct stays flat and
  is mapped per direction once, in `daemon/shaper.c`.
- **Activity state names**: one table giving both the lower-case (ubus) and
  upper-case (log) forms.
- **Per-CPU counter sums** in `tcpdelay/capture.c` and `tcpdelay/injector.c`:
  one helper over the 64-bit counter fields.
- **Failure and recovery logging**: the pinger, TCP and netlink flags go
  through `daemon_degraded/recovered` where the message and level stay
  identical; any that would change a message keeps its own code, with a
  comment saying why.

### Stale text

- `latency.h` "transitional native backend", `config.c` "legacy single-target
  input" and every other comment that no longer matches the code.
- `docs/` (except the user's own README prose, below), `CODING_STYLE.md`
  (the naming rule), `docs/agents/modules.md` (the new layout), the module
  table in `AGENTS.md` (only the paths, as part of this approved task), and
  the flowcharts regenerated from `flowchart-data.json`.

### Also read in the same pass

The LuCI views (`config.js` 789 lines, `status.js` 439), the init script, the
importer and `tools/`: the same rules, behaviour unchanged. Their findings join
the same change.

## How it is done

One working pass over the whole tree, then one program commit
("restructure: one naming rule, one home per concern"). No intermediate
releases.

Verification, in increasing realism:

1. `make check` (unit tests, constants check, style check), the sanitizer
   build, and both replay traces matching as before.
2. The closed-loop simulator's output byte-identical to 0.3.30's in all four
   modes.
3. x86 SDK build of all three daemon variants and `luci-app-cake-adapt`.
4. The e2e lifecycle on VM .2, same checks as 0.3.30.
5. An alternating A/B run against 0.3.30 on VM .5 under load, and a CPU,
   memory and system-call profile with flame graphs.

Evidence goes to `profiling/2026-10-27-restructure-1.0/` in its own commit.

## Version 1.0.0

After verification, the version becomes 1.0.0 in its own commit, as the user
asked ("this time we will go to first stable version"). From then on, the
interfaces listed under non-goals count as stable: changing one is a
deliberate, documented decision. The root README's "pre-1.0 software"
sentence is the user's prose and is left for the user to change.

## Left to the builder

- Exact names of static functions, where the rule allows more than one.
- Whether a direction pair that is used asymmetrically in one spot is still
  better as an array (the default is the array).
- The order of work within the single pass.

## As built (2026-10-27)

Where the result differs from the plan above, and why:

- `status.c` and `publish.c` stay two files (in `daemon/`): the publish tests
  supply any snapshot by replacing `status_snapshot()`, which needs it in
  another file. Each keeps one concern: building the snapshot, serving it.
- `random_below` and `shuffle` stay shared, in `common/random.c`, where
  their unit tests reach them; the pid lock moved with `path_absolute` to
  `platform/path.c` (`path_lock_pid`) for the same reason.
- The per-CPU counter sums in `tcpdelay/capture.c` and `injector.c` stay:
  they sum different structures, and one helper would need type punning.
- The latency session's degraded message keeps its own code: the shared
  `daemon_degraded()` formats into a fixed buffer that could cut a long error
  short. The native pinger's recovery message now uses `daemon_recovered()`.
- Added beyond the plan: the SUMMARY record is written from the DATA
  record's structure (`struct log_summary_record` removed); the activity
  state names are one pair of inline tables in `daemon/report.h`, used by
  the log and the status object; the queue delays the controller receives
  are `delay_us[DIRECTION_COUNT]`; `struct netlink_qdisc_*`,
  `enum writer_entry_kind`, `enum links_cake_state` and
  `enum links_traffic_state` follow the naming rule.
- The LuCI views, the init script, the importer and `tools/` were scanned
  for the same problems; their names are set by UCI options and ubus fields,
  which do not change, and nothing needed restructuring.
- Dated design documents under `docs/design/` and the evidence under
  `profiling/` keep the paths of their time.
