#!/bin/bash
# capture-log.sh [-s SIZE_MB] HOST LOCAL_FILE [REMOTE_LOG]
#
# Streams a router's cake-adapt log over SSH into LOCAL_FILE and to the
# console, for long recordings:
# - follows cake-adapt's in-place rotation without losing lines;
# - reconnects after SSH drops or router reboots; each connection replays the
#   router's .old and live log, and only the lines after the last one already
#   saved are written (all of them when it is not found, as after a reboot);
# - when LOCAL_FILE reaches SIZE_MB (default 1024), renames it to
#   <name>.<time>.log, compresses that with gzip in the background, and starts
#   a new LOCAL_FILE;
# - writes capture events (connect, disconnect, archive) to LOCAL_FILE.events
#   and stderr.
#
# HOST is anything ssh accepts (user@address or a Host from ~/.ssh/config) and
# needs key authentication. REMOTE_LOG defaults to /var/log/cake-adapt.log.
# Stop with Ctrl-C. On the router the follower runs as
# "sh /tmp/cake-adapt-capture.sh"; it exits by itself within 30 s of the
# connection ending.
set -u

limit_mb=1024
while getopts s: option; do
	case $option in
	s) limit_mb=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -lt 2 ] || [ $# -gt 3 ]; then
	echo "usage: $0 [-s SIZE_MB] HOST LOCAL_FILE [REMOTE_LOG]" >&2
	exit 2
fi
host=$1
local_file=$2
remote_log=${3:-/var/log/cake-adapt.log}
events=$local_file.events
# The last line written before an archive, while LOCAL_FILE is still empty.
last_file=$local_file.last
limit_bytes=$((limit_mb * 1024 * 1024))
marker=@@cake-adapt-capture-replay-end@@
remote_script=/tmp/cake-adapt-capture.sh

event() {
	printf '%s %s\n' "$(date '+%F %T')" "$1" | tee -a "$events" >&2
}

# Replay lines are held until the marker, then written from after the last
# saved line. Each header is written once per file, or again when it changes.
write() {
	local last_line

	if [ -s "$local_file" ]; then
		last_line=$(tail -n 1 "$local_file")
	else
		last_line=$(cat "$last_file" 2>/dev/null)
	fi
	LAST_LINE=$last_line LC_ALL=C awk -F'; ' \
		-v file="$local_file" -v limit="$limit_bytes" -v marker="$marker" \
		-v size="$(stat -c %s "$local_file" 2>/dev/null || echo 0)" \
		-v events="$events" -v last_file="$last_file" '
	function exists(path) {
		return system("test -e \"" path "\"") == 0
	}
	function archive(   base, name, stamp, n) {
		close(file)
		"date +%Y%m%d-%H%M%S" | getline stamp
		close("date +%Y%m%d-%H%M%S")
		base = file
		sub(/\.log$/, "", base)
		base = base "." stamp
		name = base ".log"
		# A second archive within the same second gets a suffix.
		for (n = 2; exists(name) || exists(name ".gz"); n++)
			name = base "-" n ".log"
		print last > last_file
		close(last_file)
		system("mv \"" file "\" \"" name "\" && (gzip \"" name "\" &)")
		system("echo \"$(date \"+%F %T\") archived " name ".gz\" | tee -a \"" events "\" >&2")
		size = 0
		split("", header)
	}
	function emit(line,   type) {
		type = line
		sub(/; .*/, "", type)
		if (type ~ /_HEADER$/) {
			if (header[type] == line)
				return
			header[type] = line
		}
		print line >> file
		fflush(file)
		print line
		fflush()
		last = line
		size += length(line) + 1
		if (size >= limit)
			archive()
	}
	BEGIN { replaying = 1; last = ENVIRON["LAST_LINE"] }
	replaying && $0 == marker {
		start = 1
		for (i = held; i >= 1; i--)
			if (held_line[i] == last) {
				start = i + 1
				break
			}
		for (i = start; i <= held; i++)
			emit(held_line[i])
		split("", held_line)
		replaying = 0
		next
	}
	replaying { held_line[++held] = $0; next }
	{ emit($0) }'
}

# One line every 5 s for the router-side watchdog; it ends with the connection.
heartbeat() {
	while echo; do
		sleep 5
	done
}

ssh_pid=
trap 'event "stopped"; [ -n "$ssh_pid" ] && kill "$ssh_pid" 2>/dev/null; exit 0' INT TERM

event "capturing $host:$remote_log into $local_file, archiving at $limit_mb MB"
# Runs on the router (BusyBox sh) as /tmp/cake-adapt-capture.sh, so that ps
# shows it by name; each session writes it afresh under a temporary name and
# renames it, so a follower still running keeps its own copy. It replays the
# .old copy and the live log, prints
# the marker, then follows the live log once a second. The log stays open on
# descriptor 3, so each read continues where the last stopped instead of
# rereading the file: its position comes from /proc/self/fdinfo, and its size
# from ls, which reads no content. tail -F would lose what was written in the
# second before a rotation, which cake-adapt does by copying the log to .old and
# truncating it in place: when the log shrinks below the position, the rest of
# the old content is read from .old at the same offset. The log cannot regrow
# past that offset within a second, since it rotates at 2 MB or after 10
# minutes of growth.
#
# The PC sends a heartbeat line every 5 s on the follower's stdin. A watchdog
# beside the follower reads them; at end of input (the session closed) or after
# 30 s without one (the network silently gone), it kills the whole session's
# process group, including a reader blocked writing to a dead connection.
follower='#!/bin/sh
# Written by capture-log.sh for each SSH session; safe to delete.
f=$1
exec 4<&0
(
	while read -r -t 30 beat; do :; done
	kill -TERM 0
) <&4 &
cat "$f.old" 2>/dev/null
exec 3< "$f"
cat <&3
echo "$2"
while :; do
	set -- "$1" "$2" $(ls -ln "$f" 2>/dev/null)
	size=${7:-0}
	while read -r key value; do
		[ "$key" = pos: ] && offset=$value
	done < /proc/$$/fdinfo/3
	if [ "$size" -lt "$offset" ]; then
		tail -c +$((offset + 1)) "$f.old" 2>/dev/null
		[ $? -lt 128 ] || exit
		exec 3< "$f"
		continue
	fi
	if [ "$size" -gt "$offset" ]; then
		cat <&3
		[ $? -lt 128 ] || exit
	fi
	sleep 1
done'

while true; do
	event "connecting"
	ssh -o BatchMode=yes -o ConnectTimeout=10 \
		-o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$host" \
		"printf '%s\\n' '$follower' > $remote_script.\$\$ &&
		mv -f $remote_script.\$\$ $remote_script &&
		exec sh $remote_script '$remote_log' '$marker'" \
		< <(heartbeat) > >(write) &
	ssh_pid=$!
	wait "$ssh_pid"
	status=$?
	ssh_pid=
	event "disconnected (ssh exit $status), retrying in 10 s"
	sleep 10
done
