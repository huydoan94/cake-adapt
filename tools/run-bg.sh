#!/bin/sh
# run-bg.sh: starts a detached command and atomically records its exit status.
# Usage: sh tools/run-bg.sh LOG COMMAND [ARGUMENT...]
# Output goes to LOG (appended); completion goes to LOG.status. One job per log.
# Exit status: 0 started, 2 invalid arguments or log/launch setup failed.
set -u

fail() {
	printf '%s\n' "$*" >&2
	exit 2
}

[ "$#" -ge 2 ] || fail 'usage: run-bg.sh LOG COMMAND [ARGUMENT...]'
display_log=$1
shift
[ -n "$display_log" ] || fail 'log path is empty'
command -v "$1" >/dev/null 2>&1 || fail "command not found: $1"
command -v nohup >/dev/null 2>&1 || fail 'nohup is not available'
command -v setsid >/dev/null 2>&1 || fail 'setsid is not available'
case $display_log in
/*) log=$display_log ;;
*) log=$(pwd)/$display_log ;;
esac
[ -d "$(dirname "$log")" ] || fail "log directory does not exist: $display_log"
[ ! -e "$log" ] || [ -f "$log" ] || fail "log is not a regular file: $display_log"
lock=$log.lock
mkdir "$lock" 2>/dev/null || fail "log is already in use or cannot be locked: $display_log"
trap 'rmdir "$lock" 2>/dev/null || :' 0
( : >>"$log" ) 2>/dev/null || fail "cannot append to log: $display_log"
rm -f "$log.status" 2>/dev/null || fail "cannot remove stale status: $display_log"

# Ignore HUP before forking, so even an immediate terminal disconnect is safe.
# A new session also survives a tool ending the original process group.
# All worker descriptors are redirected; none keep the calling tool's pipe open.
trap '' HUP
setsid nohup sh -c '
	set -u
	log=$1
	lock=$2
	shift 2
	trap '\''rm -f "$lock/status"; rmdir "$lock" 2>/dev/null || :'\'' 0
	status=0
	("$@") || status=$?
	printf "%s\n" "$status" >"$lock/status" &&
		mv -f "$lock/status" "$log.status" || {
		printf "cannot publish exit status for %s\n" "$log" >&2
		exit 2
	}
' run-bg-worker "$log" "$lock" "$@" </dev/null >>"$log" 2>&1 &
worker_pid=$!
trap - 0
printf 'started %s, log %s\n' "$worker_pid" "$display_log"
