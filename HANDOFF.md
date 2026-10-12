# Current handoff

Updated 2026-10-12. This records the active work and its limits; refresh worktree
and environment facts before relying on them. Read [AGENTS.md](AGENTS.md),
[COLLABORATION.md](COLLABORATION.md), [CODING_STYLE.md](CODING_STYLE.md) and the
task's relevant docs before continuing. Keep this file current at stop points.

## Standing user decisions

- Work on the current local branch only. No other branches, remotes, commits,
  version bumps, release artifact copies or deployment without an explicit
  request. Both packages remain at 0.3.17-r1.
- Development, SDK validation, host-test headers, IntelliSense and VM testing
  default to x86. Use `OPENWRT_SDK_X86` or `../openwrt-sdk-x86`. Filogic/arm64
  requires an explicit target-specific request. The user's router architecture
  does not select the development target.
- New downloads, SDK/feed changes and installations require approval for their
  exact scope. Reuse an approval already granted for that scope. Keep approved
  local validation dependencies and caches under ignored `build/` unless another
  location was explicitly approved; do not install system-wide.
- Do not access Windows mounts/host or production routers. The prior Windows
  application work was cancelled and cleaned up; it is not an active task and
  does not require another .NET SDK download.
- The user said "make your own version" when Claude Opus 5.5 agents were
  unavailable. Codex follows the Codex workflow in `COLLABORATION.md`; do not
  ask again merely because Claude is unavailable. Preserve project behavior
  and verification requirements. Describe self-review as self-review.
- The user explicitly requested updating all relevant docs to include that
  workflow. `AGENTS.md`, collaboration/style guides, development/editor/test
  docs, product docs and design/spec notices now link to the same rules.
- The user chose **non-SSL MQTT** on 2026-10-12. Use `libmosquitto-nossl`;
  remove the MQTT OpenSSL/CA-bundle dependencies and TLS/CA controls. Plain
  username/password authentication remains supported; traffic and credentials
  are unencrypted. Keep all queue, retry, logging and default-off behavior.

## Completed work: optional MQTT log forwarding

The user requested sending the cake-adapt daemon's log data to MQTT with settings
in LuCI, disabled by default. Connection attempts retry every 10 seconds
indefinitely. The daemon must not wait for networking; offline or full-queue logs
are silently dropped instead of buffered for replay.

The implementation adds `src/logging/mqtt.c` and its header, typed
UCI/default/validation options, LuCI controls, logging/lifecycle integration,
package dependencies, queue tests and a real-library broker fixture. The main
logging path uses a try-lock and 32 slots of 2,048 bytes; each accepted line is
published with QoS 0 and no retention. Separate sender and network threads own
broker operations. Shutdown waits on the main thread are bounded. Existing log
output flags and schemas remain in effect.

The current dependency is `libmosquitto-nossl`. `mqtt_tls` and `mqtt_ca_file`
were removed from the typed model, constants/defaults, validation, shipped UCI,
LuCI and tests. The sender makes no TLS API calls. The broker fixture now takes
`--library` explicitly so it records and loads the actual non-SSL library.

Changed subsystems: root and source Makefiles; `src/common/constants.h`;
`src/config/`; `src/logging/`; `src/main.c`; shipped UCI; LuCI configuration;
`tests/config/`, `tests/logging/`, `tests/luci_config.js` and `tests/Makefile`;
configuration/technical docs; flowchart data and generated output; profiling
index and MQTT evidence. Inspect `git status --short` for the exact current list
and preserve all existing changes.

## Current non-SSL verification

The [MQTT report](profiling/2026-10-11-mqtt-logging/README.md) and its
[metadata](profiling/2026-10-11-mqtt-logging/metadata.json) identify tested sources,
dependency archives, package checksums and limits. Reuse results only while those
inputs remain unchanged.

- Configuration, LuCI, logging, defaults and MQTT queue checks passed. The queue
  tests passed AddressSanitizer/UndefinedBehaviorSanitizer outside the
  ptrace-restricted sandbox. Current `mqtt.c` passes the generated x86 editor
  command with strict warnings and staged headers.
- Native non-SSL broker acceptance passed exact payloads/QoS 0, offline loss,
  silent CONNACK withholding, 10-second retries, recovery, credentials and
  shutdown. The driver's `/proc` mappings separately verified the exact hashed
  non-SSL library. Results are `nossl-results.json`, `nossl.log`,
  `nossl-sanitizers.log` and `nossl-loaded-library.json` in the MQTT evidence.
- The x86 daemon and LuCI packages built at 0.3.17-r1, plus c-ares 1.34.6 and
  `libmosquitto-nossl` 2.0.22. All SDK builds used `NO_DEPS=1`. The daemon's APK
  names the non-SSL library and no MQTT OpenSSL/CA dependency. The library's
  dynamic dependencies are c-ares, libgcc and libc, with no OpenSSL.
- The packaged daemon and its two libraries passed bounded observation-only
  acceptance from an isolated `/tmp` directory on the authorized x86 VM. Ten
  delivered records matched file-log lines, including periodic MEMORY output;
  the daemon survived broker outage and exited with its fping child. The original
  service (PID 3389), qdisc settings and log inode 179 were preserved. Test payload
  and lock were removed. Native/SDK/VM results do not establish real-link overhead.
  The first harness wrongly required CPU records in STALL; the locally reviewed
  rerun used the activity-independent MEMORY record and retained the first evidence.
- The implementation agent could not start because of model capacity, so the
  primary implemented directly. A separate bounded reviewer checked the source,
  config/auth transition and VM harness. Reported harness cleanup findings were
  fixed and rechecked; no remaining blocker was reported.
- Current flowcharts were regenerated with the already approved local renderer.
  Commit preparation caught a sandbox-blocked earlier refresh; regeneration
  completed outside that restriction and every referenced source hash matched.
  The manifest identifies source commit `3bb37e5` (the committed SDK build change).
  No version bump, release artifact copy, new download or production deployment
  was performed.

## Superseded TLS prototype evidence

The earlier TLS-capable implementation's report and metadata are preserved as
`README-tls.md` and `metadata-tls.json` in the MQTT evidence directory. The
following results apply to those old hashes, not the current non-SSL revision:

- Focused logging, queue, configuration, init/import, LuCI, replay and monitor
  host checks passed. Netlink checks passed outside the restricted sandbox.
- Native Mosquitto 2.0.22 acceptance passed for exact payloads, offline loss,
  reconnect cadence, a peer withholding CONNACK, credentials, TLS trust and
  hostname verification, and bounded shutdown. Queue and TLS teardown cases
  passed AddressSanitizer/UndefinedBehaviorSanitizer.
- Both daemon and LuCI packages built in the existing Filogic SDK under the
  specifically approved validation scope. Final builds used staged dependencies
  and `NO_DEPS=1`; no deployment or copy to repository `bin/` occurred. The first
  recursive attempt started repackaging selected kernel modules and was stopped.
- All 16 current-code flowcharts were regenerated with the approved local
  Graphviz renderer. Policy-only documentation edits do not change their source
  hashes or require another rendering run.
- Target module compilation included x86, but that TLS prototype had **not** passed
  a full x86 SDK package build or full-daemon x86 VM acceptance. Filogic packaging
  and native broker checks do not satisfy those remaining checks. No field
  resource-cost or real-link control claim is established.

## Downloads and environment changes already authorized

These are task-scoped approvals, not permission for new SDKs or system installs:

- Mosquitto 2.0.22 was approved for local real-library validation. Its source,
  build and fixtures remain under `build/mqtt-validation/`.
- The Graphviz renderer was approved under ignored `build/`: package
  `@hpcc-js/wasm-graphviz` 1.29.2 (Graphviz 16.1.0) in
  `build/graphviz-renderer/`, npm cache in `build/graphviz-npm-cache/`.
- Missing dependencies were explicitly approved in the **existing Filogic SDK**:
  Mosquitto 2.0.22, OpenSSL 3.5.7, c-ares 1.34.6 and ca-certificates 20260601
  sources in its `dl/`, with feed package links, build selection and staging
  changes needed for those builds. The SDK's package outputs remain there.
  Exact source/package identities are in the MQTT metadata.
- On 2026-10-12 the user explicitly requested staging headers in the **x86 SDK**
  before explaining the kernel build. All four public Mosquitto 2.0.22 headers
  were copied from the already approved local source into the selected x86
  SDK's `staging_dir/target-i386_pentium4_musl/usr/include/`. No downloads,
  dependency builds, feed/config changes, libraries or package stamps were
  involved. Header checksums and destinations are recorded in
  `build/mqtt-validation/x86-header-staging.json`.
- For the requested non-SSL switch, the x86 SDK now has package links for
  Mosquitto and c-ares, copies of the existing approved source archives in `dl/`,
  and compiled/staged non-SSL library outputs. No new sources were downloaded.
  Package selection was limited through Make command arguments; all dependency
  builds and package validation used `NO_DEPS=1`. Outputs remain in the SDK.
  Local non-SSL library/subscriber builds remain under
  `build/mqtt-validation/nossl/`. The earlier TLS build remains separate for
  historical evidence and as a plain-listener broker fixture.
- `tools/sdk-build.sh` now requires staged dependencies, checks that the actual
  Mosquitto library is non-SSL, and uses `NO_DEPS=1` for both package builds.
  SDK `CONFIG_ALL` may still select SSL variants; the helper does not recursively
  build them. Its diagnostic retry keeps `NO_DEPS=1`, and an isolated preflight
  rejected the actual older SSL library before any build or copy. It still copies
  artifacts when invoked, so use direct package
  targets for validation unless that copy is explicitly authorized.

The Filogic approval was given after that target had been proposed; it does not
make Filogic the default or authorize using its headers to fix x86 IntelliSense.
No SDK download or system-wide install was needed for this MQTT work. Do not
redownload existing validation tools just to resume the task.

## x86 editor and remaining checks

`tools/compile-commands.sh` is restored to its x86-only behavior.
`compile_commands.json` was regenerated with x86 paths and is ignored; no
Filogic include fallback remains. The previously missing x86 `mosquitto.h` and
the other public headers are now staged from the matching 2.0.22 source. The
production `src/logging/mqtt.c` passes `-fsyntax-only -Werror` with its generated
x86 editor command. All four staged header hashes match their source. This
resolves the missing include in that command; no editor UI result was observed.
That original operation was header-only. The subsequent non-SSL switch built
and staged the actual x86 library and passed the daemon/LuCI package builds.

Retain raw evidence and follow the fixed test-log inode and test-process cleanup
rules. Field resource usage and the cost of high-frequency MQTT records during
live rate control remain unmeasured; do not infer those from native acceptance
or the observation-only VM check. No new build, download or VM run was performed
for the earlier documentation-only reconciliation itself.

## Why the earlier build entered kernel targets

The earlier MQTT prototype added `libmosquitto-ssl`, which brings in OpenSSL. The normal
package Make target recursively follows the SDK's generated dependency graph.
In the existing Filogic SDK, `libopenssl-afalg=m` selects `kmod-crypto-user`,
making `openssl/compile` depend on `package/kernel/linux/compile`.
`libopenssl-devcrypto=m` also selects the separate cryptodev kernel module.
The generated graph is in that SDK's `tmp/.packagedeps`; the engine dependencies
are defined in its `feeds/base_root/package/libs/openssl/Makefile`.

`build/mqtt-validation/filogic-build.log` shows entry into `package/kernel/linux`
and 282 `apk mkpkg` commands for kernel-module packages before the job was
stopped. It shows repackaging existing `.ko` files, not a full kernel source or
image compilation. Selecting a kernel-module dependency enters the common
kernel package recipe, which handles many selected module packages. The initial
recursive command was too broad for this validation; final builds used prepared
dependencies and `NO_DEPS=1`, with unneeded OpenSSL engines excluded for those
commands. Do not remove dependencies while still promising the behavior that
requires them.
The current user-directed non-SSL switch deliberately removes TLS support and
that OpenSSL dependency chain. The existing SDK's unrelated OpenSSL packages
and historical Filogic outputs have not been removed.

## Local commit authorization

On 2026-10-12 the user explicitly requested focused commits with accurate
messages and file ownership. The series separates MQTT implementation/tests, SDK
build behavior, workflow documentation, and raw validation evidence/flowcharts.
This authorization covers local commits on the current branch only; versions,
remotes and production deployment remain with the user. Previously passing checks
are reused while their recorded source inputs remain unchanged.
