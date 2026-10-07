# Plan: one internal unit per quantity

Status: implemented on 2026-10-06 in `3ec0d43` and `85a002c`.

## Base units (user, 2026-10-06, final)

| Quantity | Base unit | Type | Suffix |
| --- | --- | --- | --- |
| Data | bits and bytes; 1 byte = 8 bits, 1 KB = 1,024 bytes | integer | `_bits`, `_bytes` |
| Throughput | bit/s | integer | `_bits_per_second` |
| Time | microseconds, sub-microsecond results rounded half up | integer | `_us` |
| Ratio | per million: `RATIO_ONE_E6` (1,000,000) = 100%, 0.01 = 10,000; may exceed it (load 2,000,000 = 200%, rate factor 1,040,000) | integer | `_e6` (`_ratio_e6`, `_factor_e6`) |

**Rule:** every function computes in base units. A value converts only where
it enters or leaves the daemon:
- **Inputs:** UCI options, fping and IRTT output, CAKE's netlink attributes,
  `/proc` and eBPF records.
- **Outputs:** log records, netlink changes, uloop timers, fping arguments.

A function that mixes nanoseconds and microseconds converts the microseconds
to nanoseconds.

**No integer forms of ratios or kbit/s remain in computations.**
cake-autorate's integer arithmetic is reproduced in base units, keeping only
the two quantizations that change results, as named constants:
- `SHAPER_RATE_STEP_BITS_PER_SECOND` (1 kbit/s): shaper rates move in whole
  steps, as upstream truncates them and CAKE holds whole bytes/s.
- `ADJUSTMENT_STEPS` (thousandths): the delay adjustment between the minimum
  and maximum rate factors. Without it, 880 congestion-trace decisions differ
  by a few kbit/s.

**Unit conversions** are named functions in `common/utils.h`:
`bit_to_byte()`, `byte_to_bit()`, `bit_to_kbit()`, `kbit_to_bit()`,
`byte_to_kbyte()`, `kbyte_to_byte()`, `microsec_to_millisec()`,
`microsec_to_sec()` and `timespec_to_microsec()`. Elsewhere a value is
multiplied by a unit constant.

## cake-autorate's integer arithmetic, in base units

| Upstream (`ac75f49`) | cake-adapt |
| --- | --- |
| integer load % (kbit/s) vs whole-% threshold | `load_ratio()` (whole kbit/s, truncated to whole percent) vs `high_load_threshold_ratio` |
| kbit/s * factor per thousand / 1000 | bit/s * factor ratio, down to the rate step |
| adjustment in thousandths | ratio truncated to `ADJUSTMENT_STEPS` |
| (alpha*x + (10^6-alpha)*b)/10^6 | the same, with the per-million alpha ratio |
| kbit/s activity thresholds | bit/s comparisons |

`tests/controller/test_replay.c` still matches every recorded decision.

## Where each quantity converts

| Boundary | In | Out |
| --- | --- | --- |
| UCI | kbit/s options to bit/s, ms/s/min to µs, KB to bytes, decimal ratios to integers per million | `-L`, `-V` messages unchanged |
| CAKE netlink | bytes/s to bit/s (`byte_to_bit`) | bit/s to bytes/s (`bit_to_byte`) |
| Traffic counters | byte deltas over µs to bit/s (`bits_per_second`) | |
| fping / IRTT | ms RTT and ICMP ms to µs | interval in ms, IRTT seconds |
| `/proc` | kB to bytes, CPU ticks | |
| eBPF | ns records stay ns in the estimator; its interface takes µs | |
| Logs | | bit/s to kbit/s, bytes to KB, ticks to whole percent, µs to seconds |
| uloop / poll | | `microsec_to_millisec()` |

## Commits

Rebased on 2026-10-07 into one commit per quantity on `6751960`. Each commit
builds and passes the host tests on its own.

| Commit | Topic |
| --- | --- |
| `0269b2b` | tools: audit unit names and conversions |
| `32d0ef2` | Time: microseconds (`_us`), rounded; minute options in µs; exact fping RTT; estimator hands over µs |
| `07d6487` | Throughput: bit/s, with `byte_to_bit()` / `bit_to_kbit()` at CAKE and logs; rates from µs intervals |
| `1c307e5` | Ratios: integers per million (`_e6`); exact load, factors and byte rate step |
| `15a2e9b` | Sizes: bytes (KB = 1,024) and CPU ticks |

## Behavior differences against 0.3.7

All are small and intended:
- **Achieved rates** divide by elapsed microseconds instead of whole
  milliseconds, as upstream's `8000*bytes/interval_us`.
- **`ul_congest_ack_share`** keeps its fractional value instead of rounding to
  a whole percent. Whole-percent values give the same ceiling as before.
- **Minute options** accept fractions, and their overflow messages name the
  option.
- **IRTT** gets its session length in seconds (`600.000000s`).
- **Activity thresholds** (sleep, stall) compare bit/s instead of whole kbit/s.
- **Rate factors** with more than three decimals are used as configured
  instead of rounded to thousandths.

## Verification

Done:
- Host tests, sanitizers and the replay: 0 mismatching decisions.
- x86 and Filogic SDK builds of the final code (`918fdd0`, the same code as `85a002c` apart from member access).
- `-L` and `-V` compared with 0.3.7 on the VM for the kbit/s pass.

On hold at the user's request:
- the `-L` and `-V` comparison for the final code;
- the controlled before/after VM runs.

## Not changed

- `AGENTS.md` (user-owned) still names `5U * MEGABIT` and percent ratios in
  its examples. `MEGABIT` exists again, so only the ratio wording is stale.

## Ratio storage (user, 2026-10-06, final)

Ratios were briefly `double`. On the i386 x86 target, GCC uses x87 with 80-bit
intermediates. There, 1,000,000 bit/s × 0.99 truncated to 989,999, which the
1 kbit/s step turned into 989,000. Ratios are therefore integers per million,
with the same `_ratio` names. Any computation that yields a ratio stores it per
million, truncated (`fraction_to_ratio()`, `load_ratio()`), and `ratio_of()`
applies one exactly. The replay matches on the host and with the i386 SDK
toolchain. The x86 daemon has 10 x87 instructions left, in fping input
parsing, messages and an overflow fallback; it had 117.

## Router-log replay (2026-10-06)

`scratchpad/.../replay/replay.c` replays events from both routers' 0.3.7 logs
(771,868 DATA samples, with LOAD and TCP_QUEUE records) through the trackers,
controller and activity state, built from `6751960` and from the new code, on
x86_64 and with the i386 x87 toolchain. It runs three modes: upstream; full
(shared delay, TCP queues, ACK share); and stress (lower thresholds, larger
ACK rate).

The first run differed: the exact load ratio called 75.05% high where upstream's
whole percent does not. `load_ratio()` now uses upstream's grid. After that
fix, all 12 outputs are byte-identical to the old code's.

## Rounded microseconds and no upstream truncations (2026-10-06, final)

Two decisions by the user replace parts of the sections above:
- **Accurate math over cake-autorate's integers.**
  - `load_ratio()` is the exact ratio, and the factor adjustment is exact.
  - Rates move in CAKE's own step of whole bytes/s (8 bit/s) instead of whole
    kbit/s.
  - `test_replay.c` now requires identical delayed-sample counts and
    bufferbloat flags. It allows rates within 0.5%, thresholds within 3 µs
    and averages within 1 µs; the worst cases seen are 0.119%, 2 µs and 1 µs.
- **Time is microseconds, rounded.** A computation that yields a fraction of
  a microsecond rounds it: 0.5 µs or more is 1 µs, anything less is 0, ties
  away from zero for negative values. This covers:
  - the baseline and delta EWMAs and the average delay;
  - the TCP queue split and the serialization time;
  - fping's RTT halves, now parsed exactly as a decimal instead of through
    `strtod`;
  - timestamps with more than six decimals, clock readings, and the
    estimator's hand-over from kernel nanoseconds;
  - the per-pinger interval for the stall timeout and the IRTT spacing.

  Helpers: `rounded_divide()`, `signed_rounded_divide()`,
  `nanosec_to_microsec()`, `signed_nanosec_to_microsec()`.

Effect on the router logs, against the previous code: baselines sit 87 µs
(SaskTel) and 53 µs (other router) higher at the median. Deltas are lower by
the same, and no delta grew. The baseline EWMA with alpha 0.001 still cannot
follow a rise below 500 µs, half of the old 1 ms dead band. An all-nanosecond
version measured almost the same (89 and 52 µs).

## Naming (user, 2026-10-06)

- **Per-million values** carry their scale as a suffix, as in `ratio_e6`:
  `high_load_threshold_ratio_e6`, `fraction_to_ratio_e6()`, `load_ratio_e6()`,
  `RATIO_ONE_E6`, `RATIO_PERCENT_E6`, `SATURATION_ENTER_RATIO_E6`, and locals
  such as `factor_e6` and `adjustment_e6`.
- **Microseconds are `us`:**
  - fields and variables: `_microseconds` becomes `_us`;
  - constants: `_MICROSECONDS` becomes `_US`, `US_PER_SECOND`,
    `NANOSECONDS_PER_US`, and the unit constant `US`;
  - helpers: `us_to_millisec()`, `us_to_sec()`, `timespec_to_us()`,
    `nanosec_to_us()`, `signed_nanosec_to_us()`, `read_clock_us()`,
    `serialization_us()`.

  IRTT's text-unit tokens (`IRTT_UNIT_MICROSECONDS`) keep their names. The
  rename changes no machine code: `.text` and `.rodata` are identical before
  and after it, for both x86 and Filogic.

## Abbreviated unit names (user, 2026-10-07)

Unit words in identifiers are abbreviated: second → `sec`, millisecond → `ms`,
microsecond → `us`, nanosecond → `ns`, bit/s → `bps`, kbit/s → `kbps`,
byte/s → `byte_ps`; `bit` and `byte` stay. The base unit constants keep
their full names with no shorthand (`MICROSECOND`, `MILLISECOND`, `SECOND`,
`MICROSECONDS_PER_SECOND`, `NANOSECONDS_PER_MICROSECOND`, ...). Examples: `us_to_millisec()` → `us_to_ms()`, `nanosec_to_us()` →
`ns_to_us()`, `bits_per_second()` → `bps()`, `rate_bits_per_second` →
`rate_bps`. Ordinal "second" (first/second arguments) is not a unit and keeps
its name. String values (UCI options, log columns, IRTT unit tokens) are
unchanged. The x86 daemon's `.text` is byte-identical before and after; only
the exported symbols `bits_per_second` and `datetime_seconds` change names.
