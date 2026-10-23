#!/bin/sh
# Named values live in src/constants/, one header per topic, so a constant is
# found by its topic rather than by searching the files that use it. Outside
# it only feature-test macros, include guards, function-like macros, attribute
# shorthands and the configuration defaults (config/defaults.h) may #define.
set -eu
cd "$(dirname "$0")/../src"
status=0
define='^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Z_][A-Z_0-9]*([[:space:]]|$)'
for file in $(find . -name '*.[ch]' ! -path './constants/*' ! -path './tcpdelay/include/*' | sort); do
	grep -nE "$define" "$file" |
		grep -vE '#[[:space:]]*define[[:space:]]+(_GNU_SOURCE|_POSIX_C_SOURCE|[A-Z_0-9]+_H(_INCLUDED)?|INJECT_INLINE)([[:space:]]|$)' |
		{ [ "$file" = ./config/defaults.h ] && grep -vE 'define[[:space:]]+DEFAULT_' || cat; } |
		while IFS= read -r line; do
			printf '%s:%s: constant outside src/constants/\n' "${file#./}" "$line"
		done
done >"${TMPDIR:-/tmp}/constants-outside.$$"
if [ -s "${TMPDIR:-/tmp}/constants-outside.$$" ]; then
	cat "${TMPDIR:-/tmp}/constants-outside.$$" >&2
	status=1
fi
rm -f "${TMPDIR:-/tmp}/constants-outside.$$"
duplicates=$(grep -hoE "$define" constants/*.h | awk '{print $2}' | grep -v '_INCLUDED$' | sort | uniq -d)
if [ -n "$duplicates" ]; then
	printf 'defined twice in src/constants/: %s\n' $duplicates >&2
	status=1
fi
[ "$status" -eq 0 ] && echo 'constants all in src/constants/'
exit "$status"
