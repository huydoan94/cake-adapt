# Agent guide: upstream behavioral reference

Read when a change touches controller behaviour, defaults, records or reflectors. Part of [AGENTS.md](../../AGENTS.md).

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

Since 2026-10-01 the goal is to minimize bufferbloat, not to port faithfully:
cake-autorate is the origin of the design and the baseline to beat. Departures
are allowed when they are opt-in (or a documented deliberate default), measured
on the testbed against the upstream-identical behavior with raw evidence under
`profiling/`, and documented in code and user-facing docs. With every departure
switched off, the replay test must keep matching within its tolerances. Judge a change by latency
first (added delay percentiles, time bufferbloated) with bounded throughput
loss (keep at least about 85% of capacity unless the user agrees otherwise).

Port behavior deliberately and verify it. Preserve record names, field order,
units, headers, and content exactly where cake-autorate log compatibility is
intended. Document and test intentional differences instead of silently
diverging or attributing local bugs to upstream.

Keep every supported cake-autorate option in the typed UCI model. Parsing an
option does not mean its behavior is implemented; do not claim support until
the corresponding path is tested. The daemon's own ICMP probes (`native`,
`native-ts`) are the default pinger, a documented deliberate difference
(user decision 2026-10-21); `fping`, `fping-ts` and `irtt` remain as
fallbacks with their options. Reflector targets come from local UCI
configuration rather than a remotely retrieved list. `fping-ts` and
`native-ts` are verified on the emulated testbed but not on internet
reflectors; the IRTT backend is experimental and must be described as such.

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

