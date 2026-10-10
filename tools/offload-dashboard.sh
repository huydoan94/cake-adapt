#!/bin/bash
# offload-dashboard.sh [-i SECONDS] [-t SECONDS] HOST [WAN]
#
# Shows a router's software flow offloading on this machine, updated in place
# every -i SECONDS (default 2, at most 10): the flowtable's devices, how many
# connections are offloaded by protocol, the share of active TCP offloaded,
# how full the conntrack table is, and each CPU's utilization (total, user,
# system, irq and softirq). The connection counts come from walking the whole
# conntrack table, which costs the router most, so they are refreshed only
# every -t SECONDS (default 10, rounded up to a whole number of samples) and
# shown with their age; the CPU table follows every sample. WAN defaults to the device of the router's wan
# interface; when that device does not exist on the router, it says so and
# exits with status 3. It reconnects after SSH drops and router reboots. Stop
# with Ctrl-C. It needs gawk here; HOST needs key authentication.
#
# How to read it: an offloaded connection appears in conntrack without its
# timeout and state, marked [OFFLOAD] (or [HW_OFFLOAD]). Active TCP counts
# established and offloaded TCP; the rest of it is usually idle (a flow idle for
# about 30 s leaves the flowtable until its next packet) or the router's own
# connections, which are never offloaded.
[ -n "${BASH_VERSION:-}" ] || exec bash "$0" "$@"
set -u

interval=2
table_interval=10
while getopts i:t: option; do
	case $option in
	i) interval=$OPTARG ;;
	t) table_interval=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || [ $# -gt 2 ] || ! [ "$interval" -ge 1 ] 2>/dev/null || [ "$interval" -gt 10 ] ||
	! [ "$table_interval" -ge 1 ] 2>/dev/null; then
	echo "usage: $0 [-i SECONDS (1-10)] [-t SECONDS] HOST [WAN]" >&2
	exit 2
fi
# The table is walked on every Nth sample.
table_every=$(((table_interval + interval - 1) / interval))
host=$1
wan=${2:-}
command -v gawk > /dev/null || { echo "$0: needs gawk" >&2; exit 2; }
remote_script=/tmp/offload-dashboard.sh

# Runs on the router (BusyBox sh) as /tmp/offload-dashboard.sh, written afresh
# for each SSH session, like capture-log.sh's follower and for the same reason:
# a slow router pays mostly for starting processes. The PC's heartbeat, one
# line every SECONDS, is its clock: a subshell counts the beats into a file
# and passes each through a FIFO with shell builtins only, and each beat starts
# one awk that reads /proc and prints a few counters, not the conntrack table;
# its program is read into a variable once per session, so the router holds a
# single file.
# Another subshell wakes every 30 s and kills the session's process group when
# the count has not moved (the network silently gone); end of input (the
# session closed) does the same. It removes its files however it ends. A WAN
# device that does not exist ends it at once with status 3, and the dashboard
# with it. The flowtable list comes from nft once per session. The script
# may not contain a single quote: it travels inside one.
follower=$(cat << 'EOF'
#!/bin/sh
# Written by offload-dashboard.sh for each SSH session; safe to delete.
every=$1
wan=${2:-$(uci -q get network.wan.device)}
if [ -z "$wan" ] || [ ! -d "/sys/class/net/$wan" ]; then
	echo "X WAN device not found: ${wan:-none given, and no device for the wan interface}"
	exit 3
fi
beats=/tmp/offload-dashboard.$$.beats
ticks=/tmp/offload-dashboard.$$.ticks
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
# Per sample: C cpu user nice system idle iowait irq softirq steal (jiffies),
# then a line with a single dot. On every EVERYth sample the table too, after
# the CPU lines: M conntrack maximum; P protocol entries offloaded hardware; A
# active-TCP offloaded. Per conntrack entry it looks only at the protocol, the
# state field and whether the line holds OFFLOAD].
program=$(cat << "AWK"
FILENAME == "/proc/stat" {
	if ($1 ~ /^cpu[0-9]/)
		print "C", $1, $2, $3, $4, $5, $6, $7, $8, $9
	next
}
FILENAME ~ /nf_conntrack_max$/ { print "M", $1; next }
{
	protocol = $3
	entries[protocol]++
	offloaded = index($0, "OFFLOAD]")
	if (offloaded) {
		off[protocol]++
		if (index($0, "[HW_OFFLOAD]"))
			hw[protocol]++
	}
	if (protocol == "tcp" && (offloaded || $6 == "ESTABLISHED")) {
		active++
		if (offloaded)
			active_off++
	}
}
END {
	if (ARGC > 2) {
		for (p in entries)
			print "P", p, entries[p], off[p] + 0, hw[p] + 0
		print "A", active + 0, active_off + 0
	}
	print "."
}
AWK
)
echo "N wan $wan"
nft list flowtables 2> /dev/null | sed -n "s/^/N /p"
n=0
while read -r _ <&4; do
	if [ "$n" -eq 0 ]; then
		awk "$program" /proc/stat /proc/sys/net/netfilter/nf_conntrack_max /proc/net/nf_conntrack
	else
		awk "$program" /proc/stat
	fi
	[ $? -lt 128 ] || exit
	n=$(((n + 1) % every))
done
kill -TERM 0
EOF
)

case "$follower" in
*\'*) echo "$0: the router script must not contain a single quote" >&2; exit 2 ;;
esac

heartbeat() {
	while echo; do
		sleep "$interval"
	done
}

# Connection events go to the screen as E lines with their time, SSH's own
# errors too (stamped when the screen reads them).
session() {
	local status

	while true; do
		echo "E $(date +%s) connecting"
		ssh -T -o BatchMode=yes -o ConnectTimeout=10 \
			-o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$host" \
			"printf '%s\\n' '$follower' > $remote_script.\$\$ &&
			mv -f $remote_script.\$\$ $remote_script &&
			exec sh $remote_script '$table_every' '$wan'" \
			< <(heartbeat) 2> >(sed -u "s/^/E - ssh: /")
		status=$?
		[ "$status" = 3 ] && return
		echo "E $(date +%s) disconnected (ssh exit $status), retrying in 5 s"
		sleep 5
	done
}

trap 'printf "\033[?25h\n"' EXIT
trap 'exit 130' INT TERM
printf '\033[?25l\033[2J'
session | gawk -v host="$host" -v interval="$interval" -v table_interval="$((table_every * interval))" '
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
function share(part, whole) { return whole ? sprintf("%d%%", 100 * part / whole) : "-" }
# Lines are cut at the terminal width, so the screen never wraps, and stop one
# row short of its height, so it never scrolls: each draw then overwrites the
# last in place.
function line(text) {
	if (++drawn < rows)
		printf "%s\033[K\n", substr(text, 1, columns)
}
function terminal_size(   command, size_line, parts) {
	command = "stty size < /dev/tty 2> /dev/null"
	if ((command | getline size_line) > 0)
		split(size_line, parts, " ")
	close(command)
	rows = parts[1] > 5 ? parts[1] : 24
	columns = parts[2] > 20 ? parts[2] : 120
}
# A share of one CPU over the last sample, from its jiffy counters.
function cpu_part(c, field) {
	return cpu_ready[c] ? sprintf("%d%%", 100 * (cpu[c, field] - previous_cpu[c, field]) / cpu_spent[c] + 0.5) : "-"
}
function draw(   i, p, c, order, n, ft) {
	terminal_size()
	drawn = 0
	now = systime()
	printf "\033[H"
	line(sprintf("Flow offloading on %s, WAN %s    %s", host, wan == "" ? "-" : wan, strftime("%Y-%m-%d %H:%M:%S", now)))
	if (connected)
		line(sprintf("Connected for %s, a sample every %d s, last %s ago", duration(now - connected_at), interval, sample_at ? duration(now - sample_at) : "-"))
	else
		line(sprintf("Not connected: %s", last_event))
	if (connected)
		line("Last event: " last_event)
	ft = ""
	for (i = 1; i <= flowtable_count; i++)
		ft = ft (i > 1 ? "; " : "") flowtable[i]
	line("Flowtables: " (ft == "" ? (connected ? "none (flow_offloading off?)" : "-") : ft))
	line("")
	line(sprintf("  %-12s %8s %10s %7s   counted every %d s, %s", "Connections", "total", "offloaded", "share",
		table_interval, table_at ? "last " duration(now - table_at) " ago" : "not yet"))
	n = split("tcp udp", order, " ")
	for (p in entries)
		if (p != "tcp" && p != "udp")
			order[++n] = p
	for (i = 1; i <= n; i++) {
		p = order[i]
		line(sprintf("  %-12s %8d %10d %7s%s", toupper(p), entries[p], offloaded[p], share(offloaded[p], entries[p]),
			hardware[p] ? sprintf("   (%d in hardware)", hardware[p]) : ""))
	}
	line(sprintf("  %-12s %8d %10d %7s   established or offloaded", "active TCP", active, active_off, share(active_off, active)))
	line(sprintf("  %-12s %8d of %d", "conntrack", total_entries, max_entries))
	line("")
	line(sprintf("  %-12s %8s %7s %7s %7s %8s", "CPU", "total", "user", "system", "irq", "softirq"))
	for (i = 1; i <= cpu_count; i++) {
		c = cpu_name[i]
		line(sprintf("  %-12s %8s %7s %7s %7s %8s", c, cpu_part(c, "busy"), cpu_part(c, "user"),
			cpu_part(c, "system"), cpu_part(c, "irq"), cpu_part(c, "softirq")))
	}
	printf "\033[J"
	fflush()
}
BEGIN { rows = 24; columns = 120 }
$1 == "E" {
	text = $0
	sub(/^E [^ ]+ /, "", text)
	last_event = strftime("%H:%M:%S", $2 == "-" ? systime() : $2) " " text
	if (text == "connecting") {
		connected = 0
	} else if (text ~ /^disconnected/) {
		connected = 0
		sample_at = 0
		delete previous_cpu
	}
	draw()
	next
}
$1 == "X" {
	fatal = substr($0, 3)
	next
}
$1 == "N" {
	if (!connected) {
		connected = 1
		connected_at = systime()
		flowtable_count = 0
		last_event = strftime("%H:%M:%S", connected_at) " connected"
	}
	if ($2 == "wan")
		wan = $3
	else if ($2 == "flowtable")
		flowtable_name = $3
	else if ($0 ~ /devices = \{/) {
		text = $0
		sub(/.*devices = \{ */, "", text)
		sub(/ *\}.*/, "", text)
		gsub(/"/, "", text)
		gsub(/, /, " ", text)
		flowtable[++flowtable_count] = flowtable_name ": " text
	}
	next
}
# The CPU counters: user and nice are user time; busy is everything but idle
# and iowait.
$1 == "C" {
	if (!sample_open) {
		sample_open = 1
		cpu_count = 0
	}
	c = $2
	cpu_name[++cpu_count] = c
	cpu[c, "user"] = $3 + $4
	cpu[c, "system"] = $5
	cpu[c, "irq"] = $8
	cpu[c, "softirq"] = $9
	cpu[c, "all"] = $3 + $4 + $5 + $6 + $7 + $8 + $9 + $10
	cpu[c, "busy"] = cpu[c, "all"] - $6 - $7
	next
}
# A table sample starts with M; its counts replace the last ones.
$1 == "M" {
	max_entries = $2
	total_entries = 0
	delete entries
	delete offloaded
	delete hardware
	table_at = systime()
	next
}
$1 == "P" { entries[$2] = $3; offloaded[$2] = $4; hardware[$2] = $5; total_entries += $3; next }
$1 == "A" { active = $2; active_off = $3; next }
$1 == "." {
	sample_open = 0
	if (!connected) {
		connected = 1
		connected_at = systime()
	}
	sample_at = systime()
	for (i = 1; i <= cpu_count; i++) {
		c = cpu_name[i]
		cpu_spent[c] = cpu[c, "all"] - previous_cpu[c, "all"]
		cpu_ready[c] = ((c, "all") in previous_cpu) && cpu_spent[c] > 0
	}
	draw()
	# This sample becomes the previous one.
	for (i = 1; i <= cpu_count; i++) {
		c = cpu_name[i]
		split("all busy user system irq softirq", field, " ")
		for (f in field)
			previous_cpu[c, field[f]] = cpu[c, field[f]]
	}
	next
}
END {
	if (fatal != "") {
		printf "\033[2J\033[H%s\n", fatal
		exit 3
	}
}'
exit "${PIPESTATUS[1]}"
