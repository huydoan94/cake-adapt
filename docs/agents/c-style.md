# Agent guide: C style and formatting

Read before editing C. Part of [AGENTS.md](../../AGENTS.md); [CODING_STYLE.md](../../CODING_STYLE.md) holds the reusable principles.

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
Move generic names such as `read_u32()` or `fraction_to_ratio_e6()` to
`common/helpers` when they are shared; keep module-specific helpers local and
`static`.

Put shared semantic string values in `common/constants.h` and built-in defaults
in `config/defaults.c`/`config/defaults.h`. Keep complete diagnostics, format
strings, and module-owned schemas beside the code that uses them. Tests should
keep literal expected values when importing the production constant would make
the check tautological.

Every fixed string value, meaning a name, token or label stored or passed as
data, is a named constant in `common/constants.h`, or a `#define` beside the
file's other names when only that file uses it (as `capture.c` names the BPF
program and maps). Only format strings and complete messages stay inline; do
not split a message into fragments that are passed around as values.

`sh tools/style-check.sh` checks the changed lines for the two rules review
keeps finding (mixed `->`/`.` chains and literal `true`/`false` arguments) and
for clang-format stability; run it before reporting a C change. `make check`
runs it on every tracked file in `src/` and `tests/`, which are kept clean.

Never pass a literal `true` or `false` as a function argument. Give the call a
meaning instead: separate functions sharing a static worker, an enum, a
pointer that is `NULL` or not, or a variable whose name says what it holds.

Objects keep their own settings, set once through an init function or a
setter (`tcpdelay_estimator_set_bound(estimator, queue_bound_us)`), and are
passed around instead of long argument lists or values copied between structs.
Callbacks find their owner with `container_of()`, as uloop does, not through a
`void *` context. Never mix `->` and `.` in one member-access chain: take a
local pointer (`struct monitor_tcp *tcp = &monitor->tcp;`).

Write quantities for quick visual understanding. Express durations and rates
in readable units using the existing unit constants (for example,
`10U * SECOND` or `5U * MEGABIT`) rather than long digit strings. Derive unit
conversions from the existing unit table instead of repeating numeric factors
or inventing another scale. Distinguish a percentage from a ratio and from its
fixed-point representation; preserve fractional values without integer
truncation. Names, comments, configuration units, and calculations must agree.

Every computation and comparison uses the base units, as integers:

- data in bits and bytes (a KB is 1,024 bytes) and rates in bit/s;
- time in microseconds, a fraction of a microsecond rounding to the nearest
  (half or more up); the TCP filter and estimator keep kernel nanoseconds
  internally and hand over rounded microseconds;
- ratios as integers per million (`RATIO_ONE_E6` is 1.0, 0.01 is 10,000),
  which may exceed one million.

Convert only where a value enters or leaves the daemon (UCI, the kernel,
uloop, pinger arguments and output, logs), through the named helpers. A name
holding a quantity ends in its unit: `_us`, `_ns`, `_ms` (boundary only),
`_sec`, `_bps`, `_kbps` (boundary only), `_byte_ps`, `_bytes`, `_bits`,
`_ticks` or `_e6`. The unit constants in `constants.h` keep full names
(`MICROSECOND`, `MILLISECOND`, `SECOND`, `MICROSECONDS_PER_SECOND`,
`NANOSECONDS_PER_MICROSECOND`); durations use `SECOND`, conversions the
`*_PER_*` constants. Do not copy upstream's integer truncations (whole kbit/s,
per-thousand factors, whole-percent loads): exact math wins over matching
cake-autorate digit for digit.

Do not repeat the program name in every log message because the backend already
identifies the service.

### Formatting

C follows the Linux kernel style used by OpenWrt's libubox and unetd.
`.clang-format` is the kernel's own configuration, with checkpatch's 100-column
limit and one deliberate exception: the parameter and argument layout shown
below. Format every changed C file with clang-format (the SDK's
`staging_dir/host/llvm-bpf/bin/clang-format` works) and keep every file
clang-format-stable: running it again changes nothing. Never hand-format
against it. Where a preferred layout cannot be expressed in clang-format, take
clang-format's output and drop the preference.

- tabs for indentation, 8 columns wide, and lines up to 100 columns; string
  literals are never split;
- function braces on their own line, other braces on the statement line;
- no braces around a single-statement body, unless its condition spans lines:

```c
if (result < 0)
	return -1;
```

- a declaration, definition or call whose parameters or arguments do not fit
  on one line puts each on its own line, one tab in, with the closing
  parenthesis on its own line:

```c
int tcpdelay_capture_open(
	struct tcpdelay_capture *capture,
	const char *object_path,
	const char *interface,
	struct tcpdelay_estimator *estimator,
	char *error,
	size_t error_size
)
{
```

```c
log_message(
	LOG_LEVEL_WARNING,
	"TCP measurement degraded: capture failed: %s",
	strerror(errno)
);
```

- compound conditions break after the operator and align with the condition;
- for a callee name of seven characters or fewer (`memcpy`, `printf`, `read`),
  clang-format aligns arguments after the parenthesis instead; accept that, and
  give the project's own helpers longer names;
- a multi-line top-level initializer ends with a trailing comma, so each entry
  gets its own line, as in unetd; nested designated initializers keep
  clang-format's own layout.

Commit a pure reformat on its own, apart from any logic change.

Write code that reads like `unetd`'s `wg-user.c`: small static functions with
a module prefix that form a little internal API, and trivial operations from
`common/utils.h` instead of open-coded arithmetic. Comments should explain
formulas, invariants, units, ownership, or non-obvious kernel/upstream
behavior. Do not narrate obvious syntax. Do not reformat unrelated working
code during a focused change.

Keep a call inside its condition. Never add a `ret` or `result` temporary only
to move a call out of an `if`; solve layout in `.clang-format` or by shortening
the call itself.

Logic, performance and reliability come before binary size. Measure and report
size, but accept negligible growth and do not propose size-only changes.

Keep strict warnings enabled:

```text
-Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -Werror
```

Disable a warning only for a narrow, documented reason.

Generated `.o` and `.d` files and test binaries belong in `build/`, never in
`src/` or `tests/`.

