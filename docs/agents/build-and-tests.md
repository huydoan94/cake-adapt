# Agent guide: build and tests

Read before building, testing, changing an SDK or the build helpers. Part of [AGENTS.md](../../AGENTS.md).

## Build and tests

Use plain Make:

- repository `Makefile`: OpenWrt package definition;
- `src/Makefile`: daemon build; and
- `tests/Makefile`: focused host tests.

For long builds or host-test runs, use `sh tools/run-bg.sh build/<name>.log <command>`
then `sh tools/wait-for.sh build/<name>.log`, following the tool-wait and timeout
rules in [COLLABORATION.md](../../COLLABORATION.md).

Test sources mirror the `src/` subsystem directories; move a test with the
module it covers. Test objects and binaries are written below `build/tests/`.

`tests/controller/test_replay.c` replays recorded cake-autorate `ac75f49`
traces from `tests/controller/fixtures/`. Delay counts and bufferbloat flags
must match exactly; because cake-adapt does not copy upstream's truncations,
average delays may differ by 1 µs, rates by 0.5% and compensated thresholds by
3 µs. Keep it passing; a controller change that alters a replayed decision
beyond these tolerances is a divergence from upstream and needs an explicit
decision and documentation.
`extract-trace.py` and `scripted-fping.sh` reproduce the fixtures; the raw
source logs are in `profiling/controller-comparison/`.

Prefer the narrowest useful command. Do not trigger broad OpenWrt toolchain
builds for host-only work. Development, SDK validation, host-test headers,
IntelliSense and the test VM default to **x86**. Use `../openwrt-sdk-x86` or
`OPENWRT_SDK_X86`. Filogic/arm64 requires an explicit target-specific request;
the user's Filogic routers do not change the default. Missing x86 dependencies
are an environment limitation, not permission to switch SDKs or mix headers.

Use the `.vscode/tasks.json` tasks: the host test
task (`tools/host-test.sh`) runs `check`, `check-netlink`, `check-config` and
`check-tcpdelay`, where `check` also runs the init, import and LuCI script
tests (`node` and `python3` needed), and each SDK build task
(`tools/sdk-build.sh`) cleans and compiles the package in the SDK beside the
repository (`../openwrt-sdk-x86`, `../openwrt-sdk-filogic`) or in
`OPENWRT_SDK_X86` / `OPENWRT_SDK_FILOGIC`, and `luci-app-cake-adapt` too when
the SDK's `package/luci-app-cake-adapt` links to this repository's. Both
scripts keep the make and test output in their `build/` logs and print only
their own messages, plus the errors and last 30 lines of a failed log;
`VERBOSE=1` shows the full output. Do not guess other SDK locations, and keep
absolute or personal paths out of `.vscode/`. OpenWrt 25.12 is the only release target for now. Never touch
`openwrt-image-builder`.

Inspect SDK commands for dependency downloads, feed/configuration changes and
artifact copying before running them. New downloads, SDK/feed modifications or
installations need explicit authorization for the dependency and destination;
reuse an existing approval within that exact scope without asking again. Keep
local validation downloads, tools and caches in ignored `build/` unless a
different destination is approved. Do not install system-wide or download an
SDK merely because a build needs it. Record changes and remaining files in
`HANDOFF.md`.

`tools/sdk-build.sh` also copies successful packages into the repository's
`bin/`. Before invoking Make it checks declared build dependencies, staged
headers/libraries, the SDK compiler and BPF tools, and required LuCI processing
tools when linked. It reports the complete preparation plan first, registers
missing package links from existing local feeds, and stages missing target
libraries in dependency order. Check actual artifacts as well as stamps;
The helper builds the package variants named by `variant=base|mqtt|mqtt-ssl`,
or all three (`variant=all` or none; the VS Code tasks ask which), preparing each one's dependencies just before it. Mosquitto's shared
soname must match the variant: `base` (`cake-adapt`) links no MQTT library,
`mqtt` (`cake-adapt-mqtt`) uses `libmosquitto-nossl`, and `mqtt-ssl`
(`cake-adapt-mqtt-ssl`) uses `libmosquitto-ssl` and prepares OpenSSL first.
Package dependencies must agree with the linked variant. `cake-adapt-mqtt-ssl`
also requires runtime `ca-bundle` and enables TLS support in the daemon; TLS stays
opt-in and must verify certificate trust and hostname. The other variants must
reject active TLS configuration without a plaintext fallback; `cake-adapt` with
MQTT on warns, sends nothing and keeps controlling. LuCI reads the installed variant's
`/usr/share/cake-adapt/features` and shows only what it supports.
Use command-line
package selections and `NO_DEPS=1`, without manual saved-selection edits or
recursive kernel/toolchain builds. Missing SDK tools or unavailable local feeds
must stop preflight before dependency Make calls. `--prepare-only` checks and
prepares dependencies without building or copying app artifacts.
Keep the preflight staging contracts consistent with package dependencies and
the headers/libraries the daemon uses; a new unhandled library dependency must
fail before Make rather than silently bypass preparation.
For app validation without authorized artifact copying, use focused package
Make targets in the selected SDK after preparation; see `docs/development.md`. Avoid
recursive dependency builds when their side effects exceed the approved scope.

Generate `compile_commands.json` with `tools/compile-commands.sh` from the x86
SDK. Verify that a newly required header exists in that SDK's staged include
directory before claiming IntelliSense is fixed. Never add a Filogic header
fallback to conceal a missing x86 dependency. Regenerate the ignored database
after the selected SDK's headers are ready or source files move.

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

