#!/bin/sh
# Focused run-bg/wait-for regressions; no VM, SDK changes or downloads.
set -eu
repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
cleanup() {
	# Every fixture command is bounded; finish them before removing their logs.
	for log in "$work"/*.log; do
		[ -f "$log" ] || continue
		sh "$repo/tools/wait-for.sh" --timeout 8 "$log" >/dev/null 2>&1 || :
	done
	rm -rf "$work"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM
fail() { printf 'wait helper test failed: %s\n' "$*" >&2; exit 1; }
expect_exit() {
	expected=$1
	shift
	if "$@" >"$work/result" 2>&1; then actual=0; else actual=$?; fi
	[ "$actual" -eq "$expected" ] || fail "expected exit $expected, got $actual"
}
one_line() { [ "$(wc -l <"$work/result")" -eq 1 ] || fail 'expected exactly one output line'; }

literal='a value with spaces; $(exit 9)'
log=$work/success.log
expect_exit 0 sh "$repo/tools/run-bg.sh" "$log" sh -c 'printf "%s\n" "$1"; printf "stderr\n" >&2' sh "$literal"
one_line
grep -Eq '^started [0-9]+, log ' "$work/result" || fail 'missing launch PID'
expect_exit 0 sh "$repo/tools/wait-for.sh" --timeout 5 "$log"
one_line
grep -q '^done: exit 0 after ' "$work/result" || fail 'missing success summary'
[ "$(sed -n '1p' "$log")" = "$literal" ] || fail 'arguments were reinterpreted'
grep -q '^stderr$' "$log" || fail 'stderr was not logged'

log=$work/failure.log
expect_exit 0 sh "$repo/tools/run-bg.sh" "$log" sh -c 'echo boom; exit 3'
expect_exit 3 sh "$repo/tools/wait-for.sh" --timeout 5 "$log"
grep -q '^done: exit 3 after ' "$work/result" || fail 'missing failure summary'
grep -q '^boom$' "$work/result" || fail 'failure log was not printed'

log=$work/'long job.log'
printf '9\n' >"$log.status"
printf 'previous log\n' >"$log"
expect_exit 0 sh "$repo/tools/run-bg.sh" "$log" sh -c 'echo working; sleep 5; echo finished'
one_line
worker_pid=$(sed -n 's/^started \([0-9][0-9]*\), log .*/\1/p' "$work/result")
[ -n "$worker_pid" ] && kill -0 "$worker_pid" || fail 'launch did not leave a running worker'
[ ! -e "$log.status" ] || fail 'stale status survived or launch waited for completion'
kill -HUP "$worker_pid"
expect_exit 2 sh "$repo/tools/run-bg.sh" "$log" sh -c 'echo incorrect'
one_line
expect_exit 124 sh "$repo/tools/wait-for.sh" --timeout 1 "$log"
grep -q '^still running after ' "$work/result" || fail 'missing timeout summary'
grep -q '^working$' "$work/result" || fail 'timeout log tail was not printed'
expect_exit 0 sh "$repo/tools/wait-for.sh" --timeout 6 "$log"
one_line
grep -q '^previous log$' "$log" || fail 'launch did not append'
grep -q '^finished$' "$log" || fail 'timed-out or disconnected job was stopped'
if grep -q '^incorrect$' "$log"; then fail 'concurrent job was launched'; fi
[ "$(cat "$log.status")" = 0 ] || fail 'wrong completion status'

expect_exit 2 sh "$repo/tools/run-bg.sh" "$work/missing/log" sh -c ':'
one_line
expect_exit 2 sh "$repo/tools/run-bg.sh" "$work/not-started" cake-adapt-no-such-command
one_line
mkdir "$work/directory"
expect_exit 2 sh "$repo/tools/run-bg.sh" "$work/directory" sh -c ':'
one_line
expect_exit 2 sh "$repo/tools/wait-for.sh" --timeout invalid "$log"
one_line
expect_exit 2 sh "$repo/tools/wait-for.sh" --timeout 99999999999999999999 "$log"
one_line
expect_exit 0 sh "$repo/tools/wait-for.sh" --timeout 00008 "$log"
one_line
printf 'broken\n' >"$work/bad.status"
expect_exit 2 sh "$repo/tools/wait-for.sh" --timeout 0 "$work/bad"
one_line
printf '256\n' >"$work/bad.status"
expect_exit 2 sh "$repo/tools/wait-for.sh" --timeout 0 "$work/bad"
one_line
printf '124\n' >"$work/bad.status"
printf 'command timed out\n' >"$work/bad"
expect_exit 124 sh "$repo/tools/wait-for.sh" --timeout 0 "$work/bad"
grep -q '^done: exit 124 after ' "$work/result" || fail 'command exit 124 confused with wait timeout'
printf '%s\n' 'wait helper tests passed'
