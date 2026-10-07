#!/bin/bash
# capture-log.sh [-l] [-s SIZE_MB] HOST LOCAL_FILE [REMOTE_LOG]
#
# Streams a router's cake-adapt log over SSH into LOCAL_FILE, for long
# recordings, and shows a status screen updated in place every second:
# - follows cake-adapt's in-place rotation without losing lines;
# - reconnects after SSH drops or router reboots; each connection replays the
#   router's .old and live log, and only the lines after the last one already
#   saved are written (all of them when it is not found, as after a reboot);
# - when LOCAL_FILE reaches SIZE_MB (default 1024), renames it to
#   <name>.<time>.log, compresses that with gzip in the background, and starts
#   a new LOCAL_FILE;
# - writes capture events (connect, disconnect, archive) to LOCAL_FILE.events;
# - shows the connection and capture state and the router's latest rates,
#   loads, delays, TCP queues, bufferbloat, memory, CPU and warnings. Sizes are
#   in MB (1,048,576 bytes), the capture's throughput in MB/s, link rates in
#   Mbit/s, times as date and time, and intervals in whole seconds. It needs
#   GNU awk (gawk). With -l, the log lines and events are printed to the
#   console instead, as they arrive.
#
# HOST is anything ssh accepts (user@address or a Host from ~/.ssh/config) and
# needs key authentication. REMOTE_LOG defaults to /var/log/cake-adapt.log.
# Stop with Ctrl-C. On the router the follower runs as
# "sh /tmp/cake-adapt-capture.sh"; it exits by itself within 30 s of the
# connection ending.
set -u

limit_mb=1024
print_lines=0
while getopts ls: option; do
	case $option in
	l) print_lines=1 ;;
	s) limit_mb=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -lt 2 ] || [ $# -gt 3 ]; then
	echo "usage: $0 [-l] [-s SIZE_MB] HOST LOCAL_FILE [REMOTE_LOG]" >&2
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
	if [ "$print_lines" = 1 ]; then
		printf '%s %s\n' "$(date '+%F %T')" "$1" | tee -a "$events" >&2
	else
		printf '%s %s\n' "$(date '+%F %T')" "$1" >> "$events"
	fi
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
		-v events="$events" -v last_file="$last_file" -v print_lines="$print_lines" '
	function exists(path) {
		return system("test -e \"" path "\"") == 0
	}
	function archive(   base, name, stamp, n, message) {
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
		message = "echo \"$(date \"+%F %T\") archived " name ".gz\""
		if (print_lines)
			system(message " | tee -a \"" events "\" >&2")
		else
			system(message " >> \"" events "\"")
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
		if (print_lines) {
			print line
			fflush()
		}
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

# The status screen. It follows LOCAL_FILE (tail -F also follows a new file
# after an archive) and the events file, and redraws once a second on a tick
# that carries the time, the file size and the terminal width. Each source prefixes its lines (L:
# log, E: event, T: tick); sed -u writes whole lines, so the sources never
# interleave mid-line.
dashboard() {
	{
		# The router's last start, which may lie far back in the file.
		grep -a 'Starting cake-adapt' "$local_file" 2>/dev/null | tail -n 1 | sed 's/^/L /'
		tail -n 3000 -F "$local_file" 2>/dev/null | sed -u 's/^/L /' &
		tail -n 4 -F "$events" 2>/dev/null | sed -u 's/^/E /' &
		while sleep 1; do
			columns=$(stty size < /dev/tty 2>/dev/null | cut -d' ' -f2)
			echo "T $(date +%s) $(stat -c %s "$local_file" 2>/dev/null || echo 0) ${columns:-120}"
		done
	} | gawk -v host="$host" -v remote="$remote_log" -v file="$local_file" \
		-v limit="$limit_bytes" '
	function mb(bytes) { return sprintf("%.1f MB", bytes / 1048576) }
	function mbit(kbps) { return sprintf("%.2f Mbit/s", kbps / 1000) }
	function ms(us) { return sprintf("%.1f ms", us / 1000) }
	function when(t) { return t ? strftime("%Y-%m-%d %H:%M:%S", t) : "-" }
	function ago(t) { return t ? sprintf("%d s", now - t) : "-" }
	# Lines are cut at the terminal width, so the screen never wraps.
	function line(text) { printf "%s\033[K\n", substr(text, 1, columns) }
	function count_recent(flags,   i, n) {
		n = 0
		for (i = bb_first; i <= bb_last; i++)
			if (bb_time[i] >= record_time - 60 && index(bb_flags[i], flags))
				n++
		return n
	}
	function recent_samples(   i, n) {
		n = 0
		for (i = bb_first; i <= bb_last; i++)
			if (bb_time[i] >= record_time - 60)
				n++
		return n
	}
	function share(n, total) { return total ? sprintf("%d (%.1f%%)", n, 100 * n / total) : "0" }
	function direction(name, d,   queue) {
		queue = queue_valid[d] ? ms(queue_us[d]) : "-"
		line(sprintf("%-9s %15s %15s %6s %-12s %9s %8s %10s", name, mbit(achieved[d]),
			mbit(shaper[d]), load[d] == "" ? "-" : load[d] "%", condition[d],
			ms(delay[d]), delayed[d] "/6", queue))
	}
	function draw(   i, samples, status_text) {
		printf "\033[H"
		line("cake-adapt capture: " host ":" remote " -> " file)
		line("now " when(now))
		line("")
		status_text = status
		if (status == "connected")
			status_text = "connected since " when(connected_at) " (" ago(connected_at) ")"
		else if (status_at)
			status_text = status " since " when(status_at) " (" ago(status_at) ")"
		line(sprintf("%-12s %s, reconnects %d", "Connection", status_text, reconnects))
		line(sprintf("%-12s %s this run, %.2f MB/s, file %s of %s, %d archived", "Capture",
			mb(captured), rate / 1048576, mb(size), mb(limit), archives))
		line(sprintf("%-12s %s %s, started %s (%s ago)", "Router",
			version == "" ? "cake-adapt (start not seen)" : "cake-adapt " version,
			pid == "" ? "" : "PID " pid, when(started), ago(started)))
		line(sprintf("%-12s %s (%s ago)", "Last record", when(record_time), ago(record_time)))
		line("")
		line(sprintf("%-9s %15s %15s %6s %-12s %9s %8s %10s", "", "achieved", "shaper", "load",
			"condition", "avg delay", "delayed", "TCP queue"))
		direction("Download", 1)
		direction("Upload", 2)
		line("")
		samples = recent_samples()
		line(sprintf("%-12s last 60 s: download %s, upload %s of %d samples; %d shaper changes",
			"Bufferbloat", share(count_recent("D"), samples), share(count_recent("U"), samples),
			samples, shaper_changes()))
		line(sprintf("%-12s RSS %s, peak %s, heap %s; CPU %s", "Daemon",
			memory_rss == "" ? "-" : mb(memory_rss * 1024),
			memory_peak == "" ? "-" : mb(memory_peak * 1024),
			memory_anon == "" ? "-" : mb(memory_anon * 1024),
			cpu == "" ? "-" : cpu "%"))
		line(sprintf("%-12s %d seen; last %s", "Warnings", warnings,
			last_warning == "" ? "-" : last_warning))
		line("")
		line("Recent capture events:")
		for (i = 1; i <= 4; i++)
			line("  " (event_line[i] == "" ? "" : event_line[i]))
		printf "\033[J"
		fflush()
	}
	function shaper_changes(   i, n) {
		n = 0
		for (i in shaper_time)
			if (shaper_time[i] >= record_time - 60)
				n++
			else
				delete shaper_time[i]
		return n
	}
	BEGIN {
		FS = "; "
		status = "starting"
		opened = systime()
		bb_first = 1
		printf "\033[?25l\033[H\033[2J"
	}
	/^T / {
		split($0, tick, " ")
		now = tick[2]
		columns = tick[4]
		if (base_size == "")
			base_size = tick[3]
		else if (tick[3] < size)
			carried += size
		size = tick[3]
		captured = carried + size - base_size
		history_time[++ticks] = now
		history_bytes[ticks] = captured
		old = ticks > 10 ? ticks - 10 : 1
		rate = now > history_time[old] ? (captured - history_bytes[old]) / (now - history_time[old]) : 0
		delete history_time[ticks - 11]
		delete history_bytes[ticks - 11]
		draw()
		next
	}
	/^E / {
		text = substr($0, 3)
		for (i = 1; i < 4; i++)
			event_line[i] = event_line[i + 1]
		event_line[4] = text
		message = substr(text, 21)
		stamp = substr(text, 1, 19)
		gsub(/[-:]/, " ", stamp)
		t = mktime(stamp)
		# Events from before this screen started are shown, not counted.
		if (t < opened - 1)
			next
		if (message ~ /^connecting/) {
			if (status == "connected" || status == "disconnected")
				reconnects++
			status = "connecting"
			status_at = t
		} else if (message ~ /^disconnected/) {
			status = "disconnected"
			status_at = t
		} else if (message ~ /^archived/) {
			archives++
		} else if (message ~ /^stopped/) {
			status = "stopped"
			status_at = t
		}
		next
	}
	{
		record = substr($0, 3)
		n = split(record, f, "; ")
		type = f[1]
		if (status == "connecting") {
			status = "connected"
			connected_at = now ? now : systime()
		}
		if (f[3] ~ /^[0-9]+\.[0-9]+$/)
			record_time = int(f[3])
		if (type == "SUMMARY" && n >= 13) {
			achieved[1] = f[4]; achieved[2] = f[5]
			delayed[1] = f[6]; delayed[2] = f[7]
			delay[1] = f[8]; delay[2] = f[9]
			condition[1] = f[10]; condition[2] = f[11]
			shaper[1] = f[12]; shaper[2] = f[13]
			flags = (f[10] ~ /_bb$/ ? "D" : "") (f[11] ~ /_bb$/ ? "U" : "")
			bb_time[++bb_last] = record_time
			bb_flags[bb_last] = flags
			while (bb_first < bb_last && bb_time[bb_first] < record_time - 60) {
				delete bb_time[bb_first]
				delete bb_flags[bb_first]
				bb_first++
			}
		} else if (type == "DATA" && n >= 7) {
			load[1] = f[6]; load[2] = f[7]
		} else if (type == "TCP_QUEUE" && n >= 8) {
			queue_valid[1] = f[5]; queue_us[1] = f[6]
			queue_valid[2] = f[7]; queue_us[2] = f[8]
		} else if (type == "MEMORY" && n >= 8) {
			memory_rss = f[5]; memory_peak = f[6]; memory_anon = f[7]
		} else if (type == "CPU" && n >= 5) {
			cpu = f[5]
		} else if (type == "SHAPER") {
			shaper_time[++shaper_count] = record_time
		} else if (type == "WARNING" || type == "ERROR") {
			warnings++
			last_warning = f[2] " " f[4]
		} else if ((type == "SYSLOG" || type == "INFO") && f[4] ~ /^Starting cake-adapt /) {
			split(f[4], words, " ")
			version = words[3]
			pid = words[6]
			sub(/,$/, "", pid)
			started = int(f[3])
		}
	}'
}

# One line every 5 s for the router-side watchdog; it ends with the connection.
heartbeat() {
	while echo; do
		sleep 5
	done
}

ssh_pid=
dashboard_pid=
stop() {
	event "stopped"
	[ -n "$ssh_pid" ] && kill "$ssh_pid" 2>/dev/null
	if [ -n "$dashboard_pid" ]; then
		# The screen's tails and ticker run in its own process group.
		kill -- -"$dashboard_pid" 2>/dev/null
		printf '\033[?25h\n'
	fi
	exit 0
}
trap stop INT TERM
if [ "$print_lines" = 0 ]; then
	touch "$local_file" "$events"
	set -m
	dashboard &
	dashboard_pid=$!
	set +m
fi

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
