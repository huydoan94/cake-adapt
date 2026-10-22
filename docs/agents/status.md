# Agent guide: project status and deferred work

Read when planning work on the controller, the TCP-delay estimator or open items. Part of [AGENTS.md](../../AGENTS.md).

## Project status and deferred work

The cake-autorate parity and refactor sequence is complete. The controller
replays two recorded upstream traces with no mismatching decision (within the
replay's tolerances since the exact-math change of 2026-10-06), live
side-by-side VM runs agree per phase, and the final end-to-end VM run and
profiling are recorded under `profiling/` (`controller-comparison/` and
`2026-09-30/`), as is the resource comparison with cake-autorate
(`cake-autorate-resources/`). The flowcharts in `flowchart/` are generated from
`flowchart-data.json` by `generate.mjs`; regenerate them when event ordering or
module ownership changes.

Since parity, these deliberate departures are implemented, opt-in, and
measured (evidence directories under `profiling/`, indexed in its README):

- `tcp_delay_attribution` (`native` or `fping`, the echo pingers): the eBPF
  TCP queue estimate splits the round-trip delta between the directions
  (`2026-10-02-tcp-queue-split`);
- `ul_congest_ack_share` (named `upload_ack_share_min` until 2026-10-04):
  download is held so its ACKs leave other upload traffic room, down to the
  configured share (`2026-10-03-ack-share-dynamic`); since 2026-10-05 its byte
  counters use the upload CAKE's packet charge (`2026-10-05-ack-accounting`);
- the filter samples at most every 4 ms per flow
  (`2026-10-03-sample-thinning`, with its cost in `2026-10-03-ebpf-filter-cost`);
- `fping-ts` was verified on the testbed (`2026-10-03-fping-ts-testbed`), and
  the Filogic build ran on an emulated arm64 VM (`2026-10-03-arm64-vm`);
- `tcp_ts_request` (named `tcp_timestamp_inject` until 0.3.12, needs
  `tcp_delay_attribution`): timestamps are added to IPv6 SYNs without one, so
  Windows clients' flows can be measured (`2026-10-07-tcp-timestamp-injection`,
  `2026-10-08-tcp-inject-clock`, `2026-10-09-tcp-inject-ipv6`). IPv4
  injection was removed in 0.3.15 because NAT hides which client a SYN came
  from; IPv4 is being deprecated, so do not bring it back.

`profiling/2026-10-03-ebpf-design-history/` records why these designs were
chosen.

On 2026-10-03 the C sources moved to the kernel `.clang-format` (`aecacfa`) and
a cleanup pass removed duplication, dead code and hand-written arithmetic
(`bdbdfa6`). Host tests, sanitizers, both SDK builds and the replay pass.

The TCP-delay estimator is under review in `docs/design/EBPF_REVIEW.md`, whose section 9 is
the working order. Findings and their state:

- new flow hiding a queue: fixed, both directions come from one flow (`20cc841`,
  `2026-10-04-flow-pair`);
- standing queue becoming the baseline: mitigated, the floor is held while
  fping sees congestion (`0321661`, `2026-10-05-tcp-confidence`);
- ACK-byte accounting: fixed, counters use CAKE's packet charge (`bbca799`,
  `2026-10-05-ack-accounting`);
- tuple reuse: open and accepted as rare; a reused tuple only removes that
  flow's estimate. Rejected samples no longer keep a stale slot fresh
  (`7665e7e`); both full tuple-lifetime designs were reverted (see below);
- sustained change in remote reply timing read as upload queue: open and
  uncommon; the user will decide later whether to take it up.

On 2026-10-05 a guard that used TCP queue shares only when they agreed with the
delivery heuristic caused a measured control regression and was reverted
(`d6564a6`); the controller matches `518df81` again. Controlled VM comparisons
with raw evidence are in `profiling/2026-10-05-gpt-work-check/` and
`profiling/2026-10-05-ack-control/`. The earlier attempted VM run in
`profiling/2026-10-04-ebpf-vm/` produced no `TCP_QUEUE` records and validates
nothing; its conclusions are withdrawn.

Recommend plain CAKE `ack-filter` only; never use or recommend
`ack-filter-aggressive`.

Remaining work must remain behavior-first:

- keep raw logs, traces, profiler output, and reproducibility metadata for any
  new runtime claim;
- never run cake-autorate and cake-adapt concurrently against the same qdiscs.

Do not claim parity for a new behavior from host tests alone. The native
ICMP probes are the default pinger (user decision 2026-10-21, after the
testbed gate and jittered-drop comparisons); `fping` and `fping-ts` remain
fallbacks, and `irtt` stays experimental until it has fixture and runtime
verification against a real server.

### Current status (2026-10-21)

- This branch's packages are at 0.3.21-r1 (`main` has v0.3.18). The user builds them and runs them on their
  Filogic routers; the arm64 VM is no longer needed before a deployment.
- 0.3.16 added the per-instance runtime file (`<section>.uci`, UCI wins over
  `config_file`) and `cake_mq` control (`2026-10-10-cake-mq`). 0.3.17 added the
  ubus status object `cake-adapt.<section>` served from its own thread, the
  per-instance lock, the dnsmasq-style reload/restart and `respawn 3600 5 0`,
  and `luci-app-cake-adapt` (`2026-10-10-luci-app`, `2026-10-11-*`).
- 0.3.18-0.3.19 added MQTT log forwarding in three package variants
  (`cake-adapt`, `cake-adapt-mqtt`, `cake-adapt-mqtt-ssl`;
  `2026-10-11-mqtt-logging`, `2026-10-12-mqtt-variants`).
- 0.3.21 (0.3.20 was never released):
  - native ICMP probes are the default pinger, with fping, fping-ts and irtt
    as fallbacks and fping no longer a package dependency
    (`2026-10-13-native-icmp*`, `2026-10-20-native-icmp-gate`,
    `2026-10-21-native-icmp-jitter`);
  - `config_file`, `ping_prefix_string`, `ping_extra_args` and
    `log_file_path_override` are read only from the root-only
    `/etc/cake-adapt/exec`, refused in `/etc/config/cake-adapt`, which LuCI
    can write (`2026-10-21-phase4-lifecycle`);
  - the importer runs under plain sh, Bash is needed only for a
    `config_file`;
  - a writer thread does all logging, so a stuck logd or slow storage never
    stops control; its queue overwrites and counts its oldest lines when full
    (`2026-10-21-log-writer`).
- Tuple-lifetime handling was tried twice and reverted both times: first an
  atomics-based design that the 32-bit x86 JIT cannot compile
  (`2026-10-05-tcp-lifetime-deferred`), then an ordered capture stream that
  measured neutral at about 50-64% more filter time per packet for a rare case
  (`5458100`, design, tools and unfinished work archived in
  `2026-10-05-tcp-lifetime-stream`). Do not restart it without a user decision
  and a concrete deployment need.
- Open and waiting on the user:
  - field data from the routers on whether `TCP_INJECT` shows `ACCEPTED` close
    to `INJECTED` with few `STALLED`, and an on/off testbed comparison of
    whether injection reduces bufferbloat;
  - `cake_mq` on 300-500 Mbit/s sites, observed by the user in the field;
  - the estimator's sustained change in remote reply timing (the user decides
    later whether to take it up);
  - the LuCI pages' look, which the user checks in a browser;
  - the `MEMORY` record's `PEAK_RSS_KB` (VmHWM) is unreliable: analyses take
    the maximum `RSS_KB`, and a daemon fix waits for a planned build.
- The user keeps the version bump, pushing, the router install, fping-ts on
  internet reflectors, and the root README's own prose. The agent keeps
  `profiling/README.md` and the flowchart output current.

