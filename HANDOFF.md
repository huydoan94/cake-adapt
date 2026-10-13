# Current handoff

Updated 2026-10-13. The short current state: what is next, the user decisions
and approvals in force, and where recent work lives. Verify worktree and
environment facts before relying on them, and keep this file current at stop
points. Keep it short: move finished detail to `docs/history/` (the full
earlier handoff is [docs/history/handoff-2026-10-12.md](docs/history/handoff-2026-10-12.md)).

## Next and open

- **Done 2026-10-13 (committed):** cheaper Codex and Claude sessions, seven
  steps approved one by one: `AGENTS.md` split into a core guide plus
  `docs/agents/*.md`; this short handoff (history in `docs/history/`) and
  scoped session-start reading; Codex works directly with a reviewer only for
  risky changes; `.ignore` keeps raw evidence, generated flowchart output,
  replay fixtures and `docs/history/` out of `rg`; quiet `tools/host-test.sh` and
  `tools/sdk-build.sh` output (`VERBOSE=1` for all of it); "Working efficiently"
  habits in `COLLABORATION.md`. Outside the repo, with the user's yes:
  `~/.codex/config.toml` has `generate_memories = false` and
  `use_memories = false` (backup `~/.codex/config.toml.bak-2026-10-13`), and
  `~/.codex/quick.config.toml` sets `model_reasoning_effort = "low"` for
  `codex --profile quick`.
- **Tidy-up (2026-10-13, uncommitted):** Claude now works directly by default
  too (`COLLABORATION.md`), and `tests/test_docs.sh` (in `make check`) fails when
  `AGENTS.md` exceeds 24 KiB or `HANDOFF.md` 8 KiB. Claude's private memory was
  cut, at the user's request, to the three notes the repository must not or
  cannot hold.
- **Open decision (security):** `ping_prefix_string`, `ping_extra_args` and
  `config_file` make the daemon run a command or a Bash file as root, and the
  `luci-app-cake-adapt` ACL grants write access to UCI `cake-adapt`, so that
  group is root-equivalent. Options laid out to the user: restrict the values
  in the daemon and init script (recommended: absolute, root-owned,
  not group/world-writable executables and files; plain fping options), document
  the group as root-equivalent, or both. No change made yet.
- **Deferred by the user ("circle back later"):** the version/docs mismatch
  with `main` (v0.3.18 is tagged there; this branch was bumped to 0.3.19-r1 on 2026-10-13, at the user's request) and
  the size of the SDK dependency helper.
- **Not yet proven:** `cake-adapt-mqtt-ssl` and LuCI packages not rebuilt
  after the last variants fix round; only a real CI run proves every
  architecture builds OpenSSL and the SSL variant; the stuck-DNS path was
  tested with a simulated resolver; MQTT overhead during live control is
  unmeasured.
- **VM 192.168.56.2:** runs `cake-adapt-mqtt` with its original configuration
  (MQTT off) and LuCI installed.

## User decisions in force

- Current local branch only; no other branches or remotes. Commits only when
  asked; no version bumps, release copies or deployment without an explicit
  request.
- x86 is the default for development, SDK validation, host-test headers,
  IntelliSense and VM tests (`OPENWRT_SDK_X86` or `../openwrt-sdk-x86`);
  Filogic/arm64 only on an explicit request.
- New downloads, SDK/feed changes and installs need approval for their exact
  scope; reuse an approval already granted for that scope; keep local
  validation tools and caches under ignored `build/`; never install
  system-wide.
- No Windows mounts or host, no production routers.
- Codex follows the Codex workflow in `COLLABORATION.md` ("make your own
  version"); do not ask again merely because Claude is unavailable. Report
  self-review as self-review.
- MQTT ships as three package variants: `cake-adapt` (no MQTT),
  `cake-adapt-mqtt` (non-SSL, the default MQTT choice) and
  `cake-adapt-mqtt-ssl` (verified TLS). In the base package `mqtt_enabled=1`
  warns and the instance keeps controlling; `mqtt_tls` without TLS is refused
  in every package (no plaintext fallback). CI is all-or-nothing: one variant
  failing on any architecture publishes nothing.
- SDK builds prepare their dependencies automatically, without hand edits to
  the SDK `.config`.

## Approvals in force

Task-scoped, not permission for new SDKs or system installs (exact identities
and history are in the archive):

- Mosquitto 2.0.22 for local validation, under `build/mqtt-validation/`.
- The Graphviz renderer `@hpcc-js/wasm-graphviz` 1.29.2 under
  `build/graphviz-renderer/` (npm cache `build/graphviz-npm-cache/`).
- x86 SDK: Mosquitto and c-ares package links, their approved source archives
  and OpenSSL source in `dl/`, and the staged outputs of the variant builds.
- Filogic SDK: Mosquitto 2.0.22, OpenSSL 3.5.7, c-ares 1.34.6 and
  ca-certificates 20260601 sources with the feed links and staging they needed.
  This does not make Filogic the default.
- Local commits on the current branch when the user asks for them.

## Recent work (newest first)

| Work | Commits | Evidence |
| --- | --- | --- |
| Docs for the MQTT sender threads and join helper | `4e2ed07` | — |
| Three package variants | `f6cd35b`, `71a0eaf`, `906e4eb` | `profiling/2026-10-12-mqtt-variants` |
| Shared bounded thread join | `7a10482` | — |
| MQTT sender fixes (wake batching, no thread cancel) | `12046bc` | — |
| Optional SSL MQTT builds (superseded by the variants) | `c04dd04` | `profiling/2026-10-12-mqtt-ssl-build` |
| SDK dependency preparation | `3bb37e5`, `c04dd04` | — |
| Optional MQTT log forwarding | `0d4803f`, `a4302ae` | `profiling/2026-10-11-mqtt-logging` |
