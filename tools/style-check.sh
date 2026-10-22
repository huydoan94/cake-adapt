#!/bin/sh
# style-check.sh: checks C style rules that review keeps finding, before a change
# is reported or committed.
#
# Usage: sh tools/style-check.sh [FILE...]
#
# Without FILE arguments it checks the lines added since HEAD in tracked C files
# under src/ and tests/ (git diff HEAD) and every line of new, untracked C files
# there, so existing code does not drown out the change. With FILE arguments it
# checks every line of those files.
#
# Rules (docs/agents/c-style.md):
# - no member-access chain mixing -> and . (take a local pointer instead; the
#   declaration taking it, such as "struct t *t = &p->a.t;", is that fix);
# - no literal true or false passed as a function argument (atomic stores
#   assign a value and are allowed);
# - every checked file is clang-format-stable (the SDK's clang-format, when
#   found; set CLANG_FORMAT to override).
# A line may opt out of the first two rules with the comment "style: allow".
#
# Exit status: 0 clean, 1 findings (printed as FILE:LINE: rule: text).
set -u

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo" || exit 1
mixed='->[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?\.[A-Za-z_]|\.[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?->'
literal_bool='[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(.*[(,][[:space:]]*(true|false)[[:space:]]*[,)]'
# Taking a local pointer is the fix for a mixed chain, not an instance of one.
local_pointer='^[[:space:]]*(const[[:space:]]+)?(struct[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*[*][[:space:]]*(const[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*&[^;]*;[[:space:]]*$'
# An atomic store assigns a value; it is not a flag passed to a function.
atomic_store='atomic_(init|store|exchange)(_explicit)?[[:space:]]*\('
findings=$(mktemp)
lines=$(mktemp)
trap 'rm -f "$findings" "$lines"' 0

# Prints FILE:LINE:TEXT for the lines to check.
changed_lines() {
	git diff -U0 HEAD -- 'src/*.c' 'src/*.h' 'tests/*.c' 'tests/*.h' |
		awk '/^\+\+\+ b\//{file = substr($0, 7)} /^@@/{split($3, a, ","); line = substr(a[1], 2)}
			/^\+[^+]/{print file ":" line ":" substr($0, 2); line++} /^ /{line++}'
	git ls-files --others --exclude-standard -- 'src/*.c' 'src/*.h' 'tests/*.c' 'tests/*.h' |
		while IFS= read -r file; do awk -v f="$file" '{print f ":" NR ":" $0}' "$file"; done
}

if [ "$#" -gt 0 ]; then
	for file in "$@"; do awk -v f="$file" '{print f ":" NR ":" $0}' "$file"; done >"$lines"
	files=$*
else
	changed_lines >"$lines"
	files=$(cut -d: -f1 "$lines" | sort -u)
fi

grep -v 'style: allow' "$lines" | awk -F: '{text = $0; sub(/^[^:]*:[^:]*:/, "", text)
	if (text ~ /^[[:space:]]*(\/\/|\/\*|\*)/) next
	print}' >"$lines.code" || :
grep -E -- "$mixed" "$lines.code" | awk -v pattern="$local_pointer" '{text = $0
	sub(/^[^:]*:[^:]*:/, "", text); if (text !~ pattern) print}' | sed 's/^\([^:]*:[^:]*\):/\1: mixed -> and . chain:/' >>"$findings"
grep -E -- "$literal_bool" "$lines.code" | grep -vE 'return[[:space:]]+(true|false)' |
	grep -vE -- "$atomic_store" |
	sed 's/^\([^:]*:[^:]*\):/\1: literal true\/false argument:/' >>"$findings"
rm -f "$lines.code"

clang_format=${CLANG_FORMAT:-}
[ -n "$clang_format" ] || for candidate in \
	"${OPENWRT_SDK_X86:-$repo/../openwrt-sdk-x86}/staging_dir/host/llvm-bpf/bin/clang-format" \
	clang-format; do
	if command -v "$candidate" >/dev/null 2>&1; then clang_format=$candidate; break; fi
done
for file in $files; do
	case $file in *.c | *.h) ;; *) continue ;; esac
	[ -f "$file" ] || continue
	if [ -n "$clang_format" ]; then
		"$clang_format" --dry-run -Werror "$file" >/dev/null 2>&1 ||
			echo "$file:1: not clang-format-stable" >>"$findings"
	fi
done
[ -n "$clang_format" ] || echo "style-check: clang-format not found; format not checked" >&2

if [ -s "$findings" ]; then
	cat "$findings"
	exit 1
fi
echo "style-check: clean ($(wc -l <"$lines") lines in $(printf '%s\n' $files | grep -c .) files)"
