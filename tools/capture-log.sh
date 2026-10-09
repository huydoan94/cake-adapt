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
#   loads, delays, TCP queues, bufferbloat, memory, CPU and warnings; a
#   direction's row turns yellow while it has bufferbloat. Each
#   value takes the largest unit in which it is at least 1.0 (sizes in B to GB
#   of 1,024, rates in bit/s to Gbit/s, delays in µs to s); times show as date
#   and time and intervals in seconds, minutes, hours and days. It needs GNU
#   awk (gawk). With -l, the log lines and events are printed to the
#   console instead, as they arrive.
#
# HOST is anything ssh accepts (user@address or a Host from ~/.ssh/config) and
# needs key authentication. REMOTE_LOG defaults to /var/log/cake-adapt.log.
# Stop with Ctrl-C. On the router the follower runs as
# "sh /tmp/cake-adapt-capture.sh", a POSIX sh script; it exits by itself
# within a minute of the connection ending.

# It needs bash (process substitution); started with sh, it runs itself with bash.
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
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

# With the status screen, standard output is the screen's feed: log lines
# prefixed "L ", events "E ".
event() {
	local text

	text="$(date '+%F %T') $1"
	printf '%s\n' "$text" >> "$events"
	if [ "$print_lines" = 1 ]; then
		printf '%s\n' "$text" >&2
	else
		printf 'E %s\n' "$text"
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
		"date \"+%F %T\"" | getline stamp
		close("date \"+%F %T\"")
		message = stamp " archived " name ".gz"
		print message >> events
		close(events)
		if (print_lines) {
			print message > "/dev/stderr"
		} else {
			print "E " message
			fflush()
		}
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
		print (print_lines ? "" : "L ") line
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
	# While the router replays its log, the screen hears how much has
	# arrived, every 64 KB.
	replaying {
		held_line[++held] = $0
		held_bytes += length($0) + 1
		if (!print_lines && held_bytes >= reported + 65536) {
			reported = held_bytes
			print "R " held_bytes
			fflush()
		}
		next
	}
	{ emit($0) }'
}

# The status screen. It reads LOCAL_FILE's history once, then takes new lines
# and events from the capture itself through the FIFO $feed rather than
# following the file: reading a file on a Windows drive (/mnt/c, /mnt/d) while
# it grows can fail with "No data available", which stops tail -F. It redraws
# once a second on a tick that carries the time, the file size and the
# terminal width. Lines are prefixed L (log), E (event), T (tick), H (history
# read only for the router's clock), S (a loading step, drawn at once so the
# screen is never blank while a large file is read) or R (how much of the
# router's replay has arrived), and every writer writes whole lines, so they
# never interleave mid-line.
#
# The router's start time is shown corrected for its clock: after a reboot the
# router runs on a restored clock, saved at shutdown or taken from its files,
# until NTP steps it. A run is on such a clock when it starts earlier than the
# previous run's last record, or when its first forward jump of more than
# CLOCK_STEP_S crosses a capture disconnect: the router went down at that
# disconnect (this machine's clock), yet the run's records before the jump are
# stamped earlier. The jump is the step, and the start moves by it; the real
# gap inside the jump is unknown, so the corrected start may be a little late.
# A run's first jump is checked only within its first 30 minutes. The log is
# unchanged.
dashboard() {
	local start_line

	{
		# Each step that can take long is announced first, so the screen shows
		# what it is waiting for instead of staying blank.
		columns=$(stty size < /dev/tty 2>/dev/null | cut -d' ' -f2)
		echo "S ${columns:-120} $(stat -c %s "$local_file" 2>/dev/null || echo 0) reading the router's last start in $local_file"
		# The router's last start, which may lie far back in the file, with the
		# records before it and the first part of its run for the clock check.
		start_line=$(grep -an 'Starting cake-adapt' "$local_file" 2>/dev/null | tail -n 1 | cut -d: -f1)
		# Capture disconnects, as D <epoch> in this machine's clock.
		sed -n 's/^\([0-9-]* [0-9:]*\) disconnected.*/D \1/p' "$events" 2>/dev/null
		if [ -n "$start_line" ]; then
			sed -n "$((start_line > 20 ? start_line - 20 : 1)),$((start_line + 50000))p; $((start_line + 50000))q" \
				"$local_file" | sed 's/^/H /'
		fi
		echo "S ${columns:-120} 0 reading the last 3,000 lines"
		tail -n 3000 "$local_file" 2>/dev/null | sed 's/^/L /'
		tail -n 4 "$events" 2>/dev/null | sed 's/^/E /'
		echo "S ${columns:-120} 0"
		cat "$feed" &
		while sleep 1; do
			columns=$(stty size < /dev/tty 2>/dev/null | cut -d' ' -f2)
			echo "T $(date +%s) $(stat -c %s "$local_file" 2>/dev/null || echo 0) ${columns:-120}"
		done
	} | {
	gawk -v host="$host" -v remote="$remote_log" -v file="$local_file" \
		-v limit="$limit_bytes" '
	# Each value takes the largest unit in which it is still at least 1.0.
	function scaled(value, step, units,   n, unit, i) {
		n = split(units, unit, " ")
		for (i = 1; i < n && (value >= step || -value >= step); i++)
			value /= step
		return sprintf(i == 1 && value == int(value) ? "%d %s" : "%.1f %s", value, unit[i])
	}
	function size(bytes) { return scaled(bytes, 1024, "B KB MB GB TB") }
	function byte_rate(bytes) { return scaled(bytes, 1024, "B/s KB/s MB/s GB/s") }
	function rate(kbps) { return kbps == "" ? "-" : scaled(kbps * 1000, 1000, "bit/s Kbit/s Mbit/s Gbit/s Tbit/s") }
	function delay_text(us) { return us == "" ? "-" : scaled(us, 1000, "µs ms s") }
	# Whole seconds, minutes, hours and days, at most two units.
	function duration(seconds) {
		seconds = int(seconds)
		if (seconds < 60)
			return seconds " s"
		if (seconds < 3600)
			return int(seconds / 60) " min " seconds % 60 " s"
		if (seconds < 86400)
			return int(seconds / 3600) " h " int(seconds % 3600 / 60) " min"
		return int(seconds / 86400) " d " int(seconds % 86400 / 3600) " h"
	}
	function when(t) { return t ? strftime("%Y-%m-%d %H:%M:%S", t) : "-" }
	function ago(t) { return t ? duration(now - t) : "-" }
	# Lines are cut at the terminal width, so the screen never wraps.
	function line(text) { printf "%s\033[K\n", substr(text, 1, columns) }
	# The same, padded to the full width on a yellow background (black text).
	function warning_line(text) {
		printf "\033[30;43m%-*s\033[0m\033[K\n", columns, substr(text, 1, columns)
	}
	# Whether this direction (D or U) had bufferbloat in the last BLOAT_HOLD_S
	# seconds; holding it keeps the row from flickering between redraws.
	function bloated(flag,   i) {
		for (i = bb_last; i >= bb_first && bb_time[i] >= record_time - BLOAT_HOLD_S; i--)
			if (index(bb_flags[i], flag))
				return 1
		return 0
	}
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
	function direction(name, d,   queue, text) {
		queue = queue_valid[d] ? delay_text(queue_us[d]) : "-"
		text = sprintf("%-9s %15s %15s %6s %-12s %10s %8s %10s", name, rate(achieved[d]),
			rate(shaper[d]), load[d] == "" ? "-" : load[d] "%", condition[d],
			delay_text(delay[d]), delayed[d] == "" ? "-" : delayed[d] "/6", queue)
		if (bloated(d == 1 ? "D" : "U"))
			warning_line(text)
		else
			line(text)
	}
	function traffic(d) {
		return achieved[d] == "" ? "-" : rate(achieved[d]) " (" byte_rate(achieved[d] * 1000 / 8) ")"
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
		if (status == "connecting" && replay_bytes != "")
			status_text = status_text ", router replay " size(replay_bytes) " received"
		if (loading != "")
			line(sprintf("%-12s %s ...", "Loading", loading))
		line(sprintf("%-12s %s, reconnects %d", "Connection", status_text, reconnects))
		line(sprintf("%-12s download %s, upload %s", "Throughput", traffic(1), traffic(2)))
		line(sprintf("%-12s %s this run at %s; file %s of %s; %d archived", "Log capture",
			size(captured), byte_rate(capture_rate), size(file_size), size(limit), archives))
		if (version == "")
			line(sprintf("%-12s cake-adapt; its start is not in the captured history", "Router"))
		else
			line(sprintf("%-12s cake-adapt %s, PID %s, started %s (%s ago)%s", "Router",
				version, pid, when(started), ago(started), clock_note()))
		line(sprintf("%-12s %s (%s ago)", "Last record", when(record_time), ago(record_time)))
		line("")
		line(sprintf("%-9s %15s %15s %6s %-12s %10s %8s %10s", "", "achieved", "shaper", "load",
			"condition", "avg delay", "delayed", "TCP queue"))
		direction("Download", 1)
		direction("Upload", 2)
		line("")
		samples = recent_samples()
		line(sprintf("%-12s last 60 s: download %s, upload %s of %d samples; %d shaper changes",
			"Bufferbloat", share(count_recent("D"), samples), share(count_recent("U"), samples),
			samples, shaper_changes()))
		line(sprintf("%-12s RSS %s, peak %s, heap %s; router CPU %s", "Daemon",
			memory_rss == "" ? "-" : size(memory_rss * 1024),
			memory_peak == "" ? "-" : size(memory_peak * 1024),
			memory_anon == "" ? "-" : size(memory_anon * 1024),
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
	# After a restored clock, the start as corrected; otherwise as logged.
	function clock_note() {
		if (clock_state == "restored")
			return ", router clock not set yet"
		if (clock_state == "corrected")
			return ", corrected from " strftime("%H:%M:%S", started_logged)
		return ""
	}
	# The latest capture disconnect after the record at a and up to b, or 0.
	function reboot_between(a, b,   i, found) {
		found = 0
		for (i = 1; i <= disconnects; i++)
			if (disconnect_at[i] > a && disconnect_at[i] <= b)
				found = disconnect_at[i]
		return found
	}
	# Tracks the router clock across starts, for H and L records alike; see
	# the comment above dashboard(). A run is unchecked until its first jump or
	# its first 30 minutes, restored when it started before the previous
	# record, and corrected once its start has moved by the step.
	function clock(t, starting, text,   words, start_pid) {
		if (starting) {
			split(text, words, " ")
			start_pid = words[6]
			sub(/,$/, "", start_pid)
			# The same start again, from the history and then the tail.
			if (start_pid == pid && t == started_logged)
				return
			version = words[3]
			pid = start_pid
			started = started_logged = t
			clock_state = clock_last != "" && t < clock_last ? "restored" : "unchecked"
			clock_last = t
			return
		}
		if ((clock_state == "restored" || clock_state == "unchecked") && clock_last != "" &&
		    t - clock_last > CLOCK_STEP_S) {
			if (clock_state == "restored" || reboot_between(clock_last, t)) {
				started += t - clock_last
				clock_state = "corrected"
			} else {
				clock_state = "set"
			}
		}
		if (clock_state == "unchecked" && t - started_logged > 1800)
			clock_state = "set"
		clock_last = t
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
		CLOCK_STEP_S = 5
		BLOAT_HOLD_S = 2
		status = "starting"
		opened = now = systime()
		columns = 120
		loading = "starting"
		bb_first = 1
		# The screen passes these to its functions before any record arrives.
		# gawk 5.2 crashes ("unexpected parameter type Node_illegal") when a
		# function gets an array element that was only read, never assigned.
		for (d = 1; d <= 2; d++) {
			achieved[d] = shaper[d] = load[d] = condition[d] = ""
			delay[d] = delayed[d] = queue_valid[d] = queue_us[d] = ""
		}
		printf "\033[?25l\033[H\033[2J"
	}
	# A loading step (S columns bytes text; no text when the history is read),
	# drawn at once.
	/^S / {
		split($0, step, " ")
		columns = step[2]
		loading = $0
		sub(/^S [0-9]+ [0-9]+ ?/, "", loading)
		if (loading != "" && step[3] > 0)
			loading = loading " (" size(step[3]) ")"
		now = systime()
		draw()
		next
	}
	# The router replay so far, in bytes.
	/^R / {
		replay_bytes = substr($0, 3)
		next
	}
	/^T / {
		split($0, tick, " ")
		now = tick[2]
		columns = tick[4]
		if (base_size == "")
			base_size = tick[3]
		else if (tick[3] < file_size)
			carried += file_size
		file_size = tick[3]
		captured = carried + file_size - base_size
		history_time[++ticks] = now
		history_bytes[ticks] = captured
		old = ticks > 10 ? ticks - 10 : 1
		capture_rate = now > history_time[old] ?
			(captured - history_bytes[old]) / (now - history_time[old]) : 0
		delete history_time[ticks - 11]
		delete history_bytes[ticks - 11]
		draw()
		next
	}
	/^D / {
		stamp = substr($0, 3)
		gsub(/[-:]/, " ", stamp)
		disconnect_at[++disconnects] = mktime(stamp)
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
			replay_bytes = ""
		} else if (message ~ /^disconnected/) {
			status = "disconnected"
			status_at = t
			disconnect_at[++disconnects] = t
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
		history = substr($0, 1, 1) == "H"
		# The tail starts later than the history ends; no step between them.
		if (!history && !tail_started) {
			tail_started = 1
			clock_last = ""
		}
		if (f[3] ~ /^[0-9]+\.[0-9]+$/)
			clock(f[3] + 0, (type == "SYSLOG" || type == "INFO") && f[4] ~ /^Starting cake-adapt /, f[4] "")
		if (history)
			next
		if (status == "connecting") {
			status = "connected"
			connected_at = now ? now : systime()
			replay_bytes = ""
		}
		if (f[3] ~ /^[0-9]+\.[0-9]+$/)
			record_time = int(f[3])
		# LOAD records come with every traffic sample, so they carry the
		# freshest rates; SUMMARY repeats them when LOAD records are off.
		if (type == "LOAD" && n >= 8) {
			load_records = 1
			achieved[1] = f[5]; achieved[2] = f[6]
			shaper[1] = f[7]; shaper[2] = f[8]
		} else if (type == "SUMMARY" && n >= 13) {
			if (!load_records) {
				achieved[1] = f[4]; achieved[2] = f[5]
				shaper[1] = f[12]; shaper[2] = f[13]
			}
			delayed[1] = f[6]; delayed[2] = f[7]
			delay[1] = f[8]; delay[2] = f[9]
			condition[1] = f[10]; condition[2] = f[11]
			flags = (f[10] ~ /_bb$/ ? "D" : "") (f[11] ~ /_bb$/ ? "U" : "")
			bb_time[++bb_last] = record_time
			bb_flags[bb_last] = flags
			while (bb_first < bb_last && bb_time[bb_first] < record_time - 60) {
				delete bb_time[bb_first]
				delete bb_flags[bb_first]
				bb_first++
			}
		} else if (type == "DATA" && n >= 8) {
			load[1] = f[7]; load[2] = f[8]
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
		}
	}' 2>> "$local_file.screen.log"
	# The screen stopped by itself: say why once, then keep reading its feed,
	# so the capture, which writes into it, goes on instead of dying of a
	# broken pipe. A stop with Ctrl-C ends this whole group first.
	status=$?
	printf '\033[?25h\n'
	printf '%s status screen stopped (gawk exit %d), capture continues; errors in %s\n' \
		"$(date '+%F %T')" "$status" "$local_file.screen.log" | tee -a "$events"
	cat > /dev/null
	}
}

# SSH's own errors, such as a refused connection, go to the screen as events;
# with -l they stay on stderr.
screen_errors() {
	if [ "$print_lines" = 1 ]; then
		cat >&2
	else
		sed -u 's/^/E ssh: /'
	fi
}

# One line a second: the router-side follower's clock and watchdog; it ends
# with the connection.
heartbeat() {
	while echo; do
		sleep 1
	done
}

ssh_pid=
dashboard_pid=
stop() {
	event "stopped"
	[ -n "$ssh_pid" ] && kill "$ssh_pid" 2>/dev/null
	if [ -n "$dashboard_pid" ]; then
		# The screen's reader and ticker run in its own process group.
		kill -- -"$dashboard_pid" 2>/dev/null
		printf '\033[?25h\n' >&4
		rm -f "$feed"
		rmdir "$feed_directory"
	fi
	exit 0
}
trap stop INT TERM
if [ "$print_lines" = 0 ]; then
	if ! command -v gawk > /dev/null; then
		echo "$0: the status screen needs gawk; install it or use -l" >&2
		exit 2
	fi
	touch "$local_file" "$events"
	feed_directory=$(mktemp -d)
	feed=$feed_directory/feed
	mkfifo "$feed"
	set -m
	dashboard &
	dashboard_pid=$!
	set +m
	# Keep the terminal on descriptor 4 and send standard output, and with it
	# every log line and event, into the screen's feed. Opening the FIFO waits
	# until the screen has read the history and opened it.
	exec 4>&1 3> "$feed" 1>&3
fi

event "capturing $host:$remote_log into $local_file, archiving at $limit_mb MB"
# Runs on the router (BusyBox sh) as /tmp/cake-adapt-capture.sh, so that ps
# shows it by name; each session writes it afresh under a temporary name and
# renames it, so a follower still running keeps its own copy. It replays the
# .old copy and the live log, prints the marker, then follows the live log on
# each heartbeat. The log stays open on descriptor 3, so each read continues
# where the last stopped instead of rereading the file. A slow router pays
# mostly for starting processes, so each tick starts only the cat that copies
# the new lines: the position before and after it comes from /proc/self/fdinfo
# with builtins, and only when cat found nothing is the log's size read with
# ls, since that is the only time it can have been rotated. tail -F would lose
# what was written in the second before a rotation, which cake-adapt does by
# copying the log to .old and truncating it in place: when the log shrinks
# below the position, the rest of the old content is read from .old at the
# same offset. The log cannot regrow past that offset within a second, since it
# rotates at 2 MB or after 10 minutes of growth.
#
# The PC sends a heartbeat line every second on the follower's stdin. A
# subshell counts them into a beat file and passes each to the follower through
# a FIFO, with shell builtins only, so the follower waits on a read instead of
# starting sleep; at end of input (the session closed) it kills the whole
# session's process group. Another subshell wakes every 30 s and does the same
# when the count has not moved (the network silently gone), so the follower
# exits within a minute, including a reader blocked writing to a dead
# connection; the count keeps moving during the replay. The follower is POSIX
# sh (read -t is not), and it removes its files however it ends (closed
# session, SIGHUP from the SSH server, or the watchdog).
# The router's shell expands its $ references, so they stay quoted here.
#
# FROZEN: the follower below is final as of fcb9184 (measured in
# profiling/2026-10-09-follower). Do not modify it; make changes on the PC
# side of this script instead.
# shellcheck disable=SC2016
follower='#!/bin/sh
# Written by capture-log.sh for each SSH session; safe to delete.
f=$1
beats=/tmp/cake-adapt-capture.$$.beats
ticks=/tmp/cake-adapt-capture.$$.ticks
echo 0 > "$beats"
mkfifo "$ticks" || exit
trap "rm -f \"$beats\" \"$ticks\"" EXIT
trap exit HUP INT TERM PIPE
exec 5<&0
(
	trap - EXIT HUP INT TERM PIPE
	n=0
	while read -r _; do
		n=$((n + 1))
		echo "$n" > "$beats"
		echo
	done
	kill -TERM 0
) <&5 > "$ticks" &
(
	trap - EXIT HUP INT TERM PIPE
	last=
	while sleep 30; do
		read -r beat < "$beats" || beat=
		[ "$beat" != "$last" ] || kill -TERM 0
		last=$beat
	done
) &
exec 4< "$ticks"
# The position is the first line of fdinfo; the shell reads a byte per system
# call, so only that line is read.
position() {
	read -r _ offset < /proc/$$/fdinfo/3
}
cat "$f.old" 2>/dev/null
exec 3< "$f"
cat <&3
echo "$2"
while read -r _ <&4; do
	position
	start=$offset
	cat <&3
	[ $? -lt 128 ] || exit
	position
	[ "$offset" = "$start" ] || continue
	set -- "$1" "$2" $(ls -ln "$f" 2>/dev/null)
	[ "${7:-0}" -lt "$offset" ] || continue
	tail -c +$((offset + 1)) "$f.old" 2>/dev/null
	[ $? -lt 128 ] || exit
	exec 3< "$f"
done
kill -TERM 0'

while true; do
	event "connecting"
	ssh -o BatchMode=yes -o ConnectTimeout=10 \
		-o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$host" \
		"printf '%s\\n' '$follower' > $remote_script.\$\$ &&
		mv -f $remote_script.\$\$ $remote_script &&
		exec sh $remote_script '$remote_log' '$marker'" \
		< <(heartbeat) > >(write) 2> >(screen_errors) &
	ssh_pid=$!
	wait "$ssh_pid"
	status=$?
	ssh_pid=
	event "disconnected (ssh exit $status), retrying in 10 s"
	sleep 10
done
