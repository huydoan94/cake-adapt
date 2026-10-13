# Collaboration in cake-adapt

This file extends the user's general working definitions in
`~/ai-working-definitions/` (outside this repository): roles, the Claude and
Codex workflows, delegation, briefs, efficiency, waiting without polling and
test-system discipline live there. Here are only cake-adapt's additions; where
both apply, this repository's rules win. `AGENTS.md` holds the standing project
rules and `HANDOFF.md` the current state.

What to read at session start: in `~/ai-working-definitions/`,
`working-style.md`, `change-discipline.md`, `efficiency.md` and, in
`collaboration.md`, the roles and your own provider's workflow; then this file.
Read the rest of `collaboration.md` before delegating, and `test-systems.md`
before touching a VM, an SDK or the build environment.

## Risky changes in this project

A change in these areas gets one bounded independent reviewer (model per the
general provider rules); other changes get a self-review:

- C threading, memory or lifetime (the ubus and MQTT threads, the TCP capture);
- kernel, netlink, qdisc or eBPF interaction;
- the init script, the importer or service lifecycle;
- packaging, the SDK helpers (`tools/sdk-build.sh`, `tools/sdk-dependencies.sh`)
  or CI;
- the LuCI ACL, credentials, or options that run commands as root.

## Project paths and helpers

- Briefs for delegated work go in `/tmp/cake-adapt-tasks/<task>.md`.
- Long work: `sh tools/run-bg.sh build/<name>.log <command>` (one log per job),
  then `sh tools/wait-for.sh build/<name>.log`; on "still running" (exit 124)
  call it again. For a long VM job, run the local `ssh` command through
  `tools/run-bg.sh`.
- `tools/host-test.sh` and `tools/sdk-build.sh` are quiet by default
  (`VERBOSE=1` for full output); full logs are in `build/`.
- Keep `HANDOFF.md` short: finished detail goes to `docs/history/`.
- The Codex quick profile (`codex --profile quick`) suits docs and small edits
  here.

## Environment and authorisation

- Development, SDK validation, host-test headers, IntelliSense and VM testing
  default to x86 (`OPENWRT_SDK_X86` or `../openwrt-sdk-x86`). Filogic or arm64
  work needs an explicit target-specific request; the user's router
  architecture does not change the default.
- Approved local validation tools and caches stay under ignored `build/` unless
  another destination was approved; the approvals in force are listed in
  `HANDOFF.md`.
- VM work follows `~/ai-working-definitions/test-systems.md` plus this
  project's rules in `AGENTS.md` (test machines and permissions) and
  [`docs/agents/vm-testing.md`](docs/agents/vm-testing.md) (the fixed test log
  and its inode, change recording, ubusd cleanup).
