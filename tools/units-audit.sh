#!/bin/sh
# Lists, per source file, the unit-suffixed identifiers and the unit
# conversions, so a unit cleanup can be tracked from one run to the next.
#
# Usage: tools/units-audit.sh [SOURCE_DIR]   (default: src)
#
# Base units (UNITS_PLAN.md): bits and bytes (rates in bit/s, suffix _bps), microseconds
# (suffix _us, sub-microsecond results rounded), and ratios as integers per
# million (suffix _e6). Anything else printed here should be a conversion at a boundary
# (configuration, kernel, pinger output, logs or timers) or one of
# cake-autorate's integer forms (whole_hundred, per_thousand, per_million).
set -eu

source_dir=${1:-src}

suffixes='e6|E6|us|US|bps|kbps|byte_ps|per_million|per_thousand|whole_hundred|whole_thousand|whole_million|ratio|minutes|sec|ms|ns|percent|kilobytes|bytes|bits|ticks'
conversions='KILOBIT|MEGABIT|THOUSAND|MILLION|KILOBYTE|BITS_PER_BYTE|PER_THOUSAND|PER_MILLION|MICROSECONDS_PER|NANOSECONDS_PER'

find "$source_dir" -name '*.[ch]' | sort | while read -r file; do
	identifiers=$(grep -oE "\\b[a-z_]*_($suffixes)\\b" "$file" | sort | uniq -c |
		awk '{ printf "%s(%s) ", $2, $1 }')
	scales=$(grep -oE "\\b($conversions)[A-Z_]*\\b|\\b[a-z]+_to_[a-z_]+\\(" "$file" | tr -d '(' |
		sort | uniq -c | awk '{ printf "%s(%s) ", $2, $1 }')
	[ -n "$identifiers$scales" ] || continue
	printf '%s\n' "$file"
	[ -z "$identifiers" ] || printf '  names: %s\n' "$identifiers"
	[ -z "$scales" ] || printf '  scales: %s\n' "$scales"
done

printf '\nTotals by unit suffix:\n'
find "$source_dir" -name '*.[ch]' -exec grep -ohE "\\b[a-z_]*_($suffixes)\\b" {} + |
	sed -E "s/.*_($suffixes)\$/\\1/" | sort | uniq -c | sort -rn
