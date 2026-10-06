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
# Stop with Ctrl-C.
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
keepalive=@@cake-adapt-capture-keepalive@@

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
		-v keepalive="$keepalive" -v size="$(stat -c %s "$local_file" 2>/dev/null || echo 0)" \
		-v events="$events" -v last_file="$last_file" '
	function archive(   name, stamp) {
		close(file)
		"date +%Y%m%d-%H%M%S" | getline stamp
		close("date +%Y%m%d-%H%M%S")
		name = file
		sub(/\.log$/, "", name)
		name = name "." stamp ".log"
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
	$0 == keepalive { next }
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

ssh_pid=
trap 'event "stopped"; [ -n "$ssh_pid" ] && kill "$ssh_pid" 2>/dev/null; exit 0' INT TERM

event "capturing $host:$remote_log into $local_file, archiving at $limit_mb MB"
# Runs on the router (BusyBox sh): replays the .old copy and the live log up
# to its current size, prints the marker, then follows the live log by byte
# offset once a second. tail -F would lose what was written in the second
# before a rotation, which cake-adapt does by copying the log to .old and
# truncating it in place: when the log shrinks, the rest of the old content is
# read from .old at the same offset. The log cannot regrow past the old offset
# within a second, since it rotates at 2 MB or after 10 minutes of growth.
# After 10 quiet seconds it sends the keepalive line, between whole lines only,
# so that a follower whose connection is gone fails to write and exits.
follower='f=$1
cat "$f.old" 2>/dev/null
offset=$(wc -c < "$f" 2>/dev/null || echo 0)
head -c "$offset" "$f" 2>/dev/null
echo "$2"
quiet=0
while :; do
	size=$(wc -c < "$f" 2>/dev/null || echo 0)
	if [ "$size" -lt "$offset" ]; then
		tail -c +$((offset + 1)) "$f.old" 2>/dev/null
		offset=0
		continue
	fi
	if [ "$size" -gt "$offset" ]; then
		tail -c +$((offset + 1)) "$f" 2>/dev/null | head -c $((size - offset))
		offset=$size
		quiet=0
	elif [ $((quiet += 1)) -ge 10 ]; then
		quiet=0
		[ "$offset" -eq 0 ] || [ -z "$(tail -c +$offset "$f" | head -c 1)" ] &&
			echo "$3"
	fi
	sleep 1
done'

while true; do
	event "connecting"
	ssh -o BatchMode=yes -o ConnectTimeout=10 \
		-o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$host" \
		"sh -c '$follower' follower '$remote_log' '$marker' '$keepalive'" \
		< /dev/null > >(write) &
	ssh_pid=$!
	wait "$ssh_pid"
	status=$?
	ssh_pid=
	event "disconnected (ssh exit $status), retrying in 10 s"
	sleep 10
done
