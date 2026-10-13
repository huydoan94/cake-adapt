#!/bin/sh
# wait-for.sh: waits quietly for a run-bg.sh completion record.
# Usage: sh tools/wait-for.sh [--timeout SECONDS] LOG
# Default timeout: 1800 s. A timeout leaves the job running; waiting again resumes.
# Exit status: command's status, 124 wait timed out, 2 invalid input/status or I/O.
# A command that itself exits 124 prints "done", not "still running".
set -u

fail() {
	printf '%s\n' "$*" >&2
	exit 2
}

timeout=1800
if [ "${1:-}" = --timeout ]; then
	[ "$#" -ge 3 ] || fail 'usage: wait-for.sh [--timeout SECONDS] LOG'
	timeout=$2
	shift 2
fi
[ "$#" -eq 1 ] && [ -n "$1" ] || fail 'usage: wait-for.sh [--timeout SECONDS] LOG'
case $timeout in
''|*[!0-9]*) fail 'timeout must be a non-negative integer number of seconds' ;;
esac
# Strip leading zeroes before shell arithmetic (which otherwise treats them as octal).
while [ "${timeout#0}" != "$timeout" ]; do timeout=${timeout#0}; done
timeout=${timeout:-0}
[ "${#timeout}" -le 9 ] || fail 'timeout is too large'
log=$1
case $log in /*) ;; *) log=$(pwd)/$log ;; esac
clock() {
	# Linux uptime is monotonic; changing the wall clock cannot extend the wait.
	read -r uptime_text idle_text </proc/uptime || fail 'cannot read the clock'
	current_sec=${uptime_text%%.*}
}
clock
started=$current_sec

while :; do
	clock
	elapsed=$((current_sec - started))
	if [ -f "$log.status" ] && [ ! -d "$log.lock" ]; then
		status=$(cat "$log.status") || fail "cannot read status: $1"
		case $status in ''|*[!0-9]*) fail "invalid exit status: $1" ;; esac
		[ "${#status}" -le 3 ] && [ "$status" -le 255 ] || fail "invalid exit status: $1"
		printf 'done: exit %s after %s s (log %s)\n' "$status" "$elapsed" "$1"
		if [ "$status" -ne 0 ]; then tail -n 30 "$log"; fi
		exit "$status"
	fi
	if [ "$elapsed" -ge "$timeout" ]; then
		printf 'still running after %s s (log %s)\n' "$elapsed" "$1"
		if [ -f "$log" ]; then tail -n 5 "$log"; fi
		exit 124
	fi
	remaining=$((timeout - elapsed))
	pause=2
	[ "$remaining" -ge "$pause" ] || pause=$remaining
	sleep "$pause" || fail 'wait was interrupted'
done
