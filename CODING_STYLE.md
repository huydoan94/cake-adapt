# Coding style in cake-adapt

This file extends the user's general engineering principles in
`~/ai-working-definitions/engineering-principles.md` (outside this repository).
The project's own conventions take precedence: the C rules in
[`docs/agents/c-style.md`](docs/agents/c-style.md), the module map in
[`docs/agents/modules.md`](docs/agents/modules.md) and `.clang-format`.

cake-adapt's additions to the general principles:

- C follows the Linux kernel style of OpenWrt's libubox and unetd, encoded in
  `.clang-format`; format every changed C file with the SDK's
  `staging_dir/host/llvm-bpf/bin/clang-format`.
- Names follow one rule, so a name says where its code lives: a function or
  type starts with its file's stem or its subsystem, enum constants start with
  their enum's name, and everything kept per direction is an array indexed by
  `enum direction` (the rule in full is in `docs/agents/c-style.md`).
- Every quantity uses the base units (bits/bytes, bit/s, microseconds, ratios
  as integers per million) and a unit suffix; convert only where a value enters
  or leaves the daemon (details in `docs/agents/c-style.md`).
- Dependencies, SDK changes and tool installs follow the x86 default and the
  approvals in [COLLABORATION.md](COLLABORATION.md) and `HANDOFF.md`.
- The test VMs may keep test changes, recorded in the run's evidence; process
  cleanup and the fixed test-log inode still apply
  ([`docs/agents/vm-testing.md`](docs/agents/vm-testing.md)).
- The generated flowcharts in `flowchart/` are kept current when their sources
  change and committed only when that change is authorised.
