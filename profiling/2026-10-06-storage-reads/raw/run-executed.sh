#!/bin/sh
set -eu
die() { echo "storage audit: $*" >&2; exit 1; }
[ "$#" -eq 1 ] || die "usage: $0 fresh-directory"
DIR=$1
[ -d "$DIR" ] && [ -z "$(ls -A "$DIR")" ] || die "use a fresh directory"
case "$DIR" in /tmp/*) ;; *) die "output directory must be in /tmp" ;; esac
awk '$2 == "/tmp" && $3 == "tmpfs" { found = 1 } END { exit !found }' /proc/mounts || die "/tmp is not tmpfs"
mkdir "$DIR/isolated-log" "$DIR/uci" "$DIR/uci-state" || die "mkdir failed"
LOG=/tmp/sqm-mon-test.log
SERVICE=/etc/init.d/cake-adapt
DAEMON=/usr/sbin/cake-adapt
BASE=$DIR/isolated-log
DAEMON_PID= TRACE_PID= STOPPED_ORIGINAL=0
SERVICE_WAS_RUNNING=0 SERVICE_WAS_ENABLED=0 LOG_INODE=
running_pids() { pidof cake-adapt 2>/dev/null || true; }
wait_gone() {
	wpid=$1
	i=0
	while process_live "$wpid" && [ "$i" -lt 5 ]; do sleep 1; i=$((i + 1)); done
	! process_live "$wpid"
}
process_live() {
	kill -0 "$1" 2>/dev/null || return 1
	[ "$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null || true)" != Z ]
}
restore_service() {
	if [ "$SERVICE_WAS_RUNNING" -eq 1 ]; then
		"$SERVICE" start
		i=0
		while [ -z "$(running_pids)" ] && [ "$i" -lt 5 ]; do sleep 1; i=$((i + 1)); done
		[ -n "$(running_pids)" ]
	fi
}
cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	if [ -n "$TRACE_PID" ] && kill -0 "$TRACE_PID" 2>/dev/null; then
		kill -INT "$TRACE_PID" 2>/dev/null || true
		wait_gone "$TRACE_PID" || kill -KILL "$TRACE_PID" 2>/dev/null || true
		wait "$TRACE_PID" 2>/dev/null || true
	fi
	if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
		kill -TERM "$DAEMON_PID" 2>/dev/null || true
		wait_gone "$DAEMON_PID" || kill -KILL "$DAEMON_PID" 2>/dev/null || true
		wait "$DAEMON_PID" 2>/dev/null || true
	fi
	if [ "$STOPPED_ORIGINAL" -eq 1 ]; then restore_service || status=1; fi
	if [ -n "$LOG_INODE" ]; then
		ls -i "$LOG" > "$DIR/log-inode-final.txt" 2>&1 || status=1
		final_inode=$(awk '{print $1}' "$DIR/log-inode-final.txt")
		[ "$final_inode" = "$LOG_INODE" ] || status=1
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
[ -x "$SERVICE" ] && [ -x "$DAEMON" ] || die "service/daemon unavailable"
[ -r /proc/diskstats ] && [ -r /proc/interrupts ] || die "proc counters unavailable"
command -v strace >/dev/null 2>&1 || die "strace unavailable"
if [ ! -e "$LOG" ]; then touch "$LOG" || die "cannot create log"; fi
LOG_INODE=$(ls -i "$LOG" | awk '{print $1}')
[ -n "$LOG_INODE" ] || die "cannot record log inode"
check_log_writers() {
	for proc in /proc/[0-9]*; do
		[ -d "$proc/fd" ] || continue
		for fd in "$proc"/fd/*; do
			[ -e "$fd" ] || continue
			target=$(readlink "$fd" 2>/dev/null || true)
			case "$target" in /tmp/*)
				fd_inode=$(ls -iL "$fd" 2>/dev/null | awk '{print $1}')
				[ "$fd_inode" = "$LOG_INODE" ] || continue
				flags=$(awk '$1 == "flags:" {print $2}' "$proc/fdinfo/${fd##*/}" 2>/dev/null || true)
				[ -n "$flags" ] || continue
				mode=$((0$flags & 3))
				[ "$mode" -eq 0 ] || die "authorized log has open writer PID ${proc##*/}"
				;;
			esac
		done
	done
}
check_log_writers
: > "$LOG" || die "cannot truncate log in place"
ln "$LOG" "$BASE/cake-adapt.log" || die "cannot hard-link log"
echo "$LOG_INODE" > "$DIR/log-inode-initial.txt"
ubus call service list '{"name":"cake-adapt"}' > "$DIR/service-before.json" 2>&1
if "$SERVICE" enabled >/dev/null 2>&1; then SERVICE_WAS_ENABLED=1; fi
if [ -n "$(running_pids)" ]; then SERVICE_WAS_RUNNING=1; fi
echo "$SERVICE_WAS_RUNNING" > "$DIR/service-running-initial.txt"
echo "$SERVICE_WAS_ENABLED" > "$DIR/service-enabled-initial.txt"
running_pids > "$DIR/pids-before.txt"
ps w > "$DIR/processes-before.txt"
: > "$DIR/daemon-args-before.txt"
for pid in $(running_pids); do tr '\000' ' ' < "/proc/$pid/cmdline" >> "$DIR/daemon-args-before.txt"; echo >> "$DIR/daemon-args-before.txt"; done
apk info -v > "$DIR/packages-before.txt" 2>&1
sha256sum "$DAEMON" /lib/bpf/cake-adapt-tcpdelay.o "$SERVICE" /etc/config/cake-adapt /tmp/cake-adapt-config/main/cake-adapt > "$DIR/installed-sha256.txt" 2>&1
cp /etc/config/cake-adapt "$DIR/uci-installed.txt"
cp /tmp/cake-adapt-config/main/cake-adapt "$DIR/effective-config.txt"
cat /proc/mounts > "$DIR/mounts.txt"
for block in /sys/block/*; do basename "$block"; done > "$DIR/sys-block.txt"
ls -l /etc/TZ /var /var/log > "$DIR/path-links.txt" 2>&1
tc -d qdisc show > "$DIR/qdisc-before.txt" 2>&1
cat /proc/diskstats > "$DIR/diskstats-initial.txt"
cat /proc/interrupts > "$DIR/interrupts-initial.txt"
cat /proc/uptime > "$DIR/uptime-initial.txt"
for pid in $(running_pids); do
	cat "/proc/$pid/io" > "$DIR/taskio-before-$pid.txt" 2> "$DIR/taskio-unavailable-$pid.txt" || true
done
pidof fping > "$DIR/fping-pids-before.txt" 2>&1 || true
for fpid in $(cat "$DIR/fping-pids-before.txt"); do
	awk '{print $4}' "/proc/$fpid/stat" > "$DIR/fping-ppid-before-$fpid.txt" 2>/dev/null || true
done
STOPPED_ORIGINAL=1
"$SERVICE" stop || die "service stop failed"
sleep 1
[ -z "$(running_pids)" ] || die "original daemon remains after stop"
cat > "$DIR/uci/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'eth1'
	option adjust_dl_shaper_rate '0'
	option adjust_ul_shaper_rate '0'
	option min_shaper_rates_enforcement '0'
	option pinger_method 'fping'
	option no_pingers '1'
	option enable_sleep_function '0'
	option debug '0'
	option tcp_delay_attribution '1'
	list reflectors '192.168.56.1'
	option output_load_stats '1'
	option output_processing_stats '1'
	option log_to_file '1'
	option log_file_max_size_KB '2'
	option log_file_path_override '$BASE'
EOF
cp "$DIR/uci/cake-adapt" "$DIR/uci-isolated.txt"
snapshot() {
	name=$1
	cat /proc/diskstats > "$DIR/diskstats-$name.txt"
	cat /proc/interrupts > "$DIR/interrupts-$name.txt"
	cat /proc/uptime > "$DIR/uptime-$name.txt"
}
owned_fping() {
	for fpid in $(pidof fping 2>/dev/null || true); do
		ppid=$(awk '{print $4}' "/proc/$fpid/stat" 2>/dev/null || true)
		[ "$ppid" = "$DAEMON_PID" ] && echo "$fpid"
	done
	return 0
}
phase() {
	name=$1
	seconds=$2
	echo "$name begin $(cat /proc/uptime)" >> "$DIR/phases.txt"
	snapshot "before-$name"
	sleep "$seconds"
	snapshot "after-$name"
	echo "$name end $(cat /proc/uptime)" >> "$DIR/phases.txt"
}
phase control-before 15
cat /proc/diskstats > "$DIR/diskstats-before-startup.txt"
cat /proc/interrupts > "$DIR/interrupts-before-startup.txt"
cat /proc/uptime > "$DIR/uptime-before-startup.txt"
echo "begin $(cat /proc/uptime)" > "$DIR/startup-window.txt"
env UCI_CONFIG_DIR="$DIR/uci" "$DAEMON" -C "$DIR/uci" -S main -f > "$DIR/daemon-console.txt" 2>&1 &
DAEMON_PID=$!
echo "$DAEMON_PID" > "$DIR/daemon-pid.txt"
sleep 3
kill -0 "$DAEMON_PID" 2>/dev/null || die "isolated daemon failed startup"
snapshot startup
echo "end $(cat /proc/uptime)" >> "$DIR/startup-window.txt"
cat "/proc/$DAEMON_PID/io" > "$DIR/taskio-running-start.txt" 2> "$DIR/taskio-unavailable-running-start.txt" || true
owned_fping > "$DIR/fping-pids-running-start.txt"
for fpid in $(cat "$DIR/fping-pids-running-start.txt"); do
	awk '{print $4}' "/proc/$fpid/stat" > "$DIR/fping-ppid-$fpid.txt" 2>/dev/null || true
done
strace -f -ttt -yy -s 256 -e trace=%file,read,readv,pread64,mmap2 -p "$DAEMON_PID" -o "$DIR/strace.txt" > "$DIR/strace-console.txt" 2>&1 &
TRACE_PID=$!
echo "$TRACE_PID" > "$DIR/strace-pid.txt"
sleep 2
kill -0 "$TRACE_PID" 2>/dev/null || die "strace attach failed"
phase running 30
cat "/proc/$DAEMON_PID/io" > "$DIR/taskio-running-end.txt" 2> "$DIR/taskio-unavailable-running-end.txt" || true
owned_fping > "$DIR/fping-pids-running-end.txt"
kill -INT "$TRACE_PID" 2>/dev/null || true
wait_gone "$TRACE_PID" || kill -KILL "$TRACE_PID" 2>/dev/null || true
wait "$TRACE_PID" 2>/dev/null || true
TRACE_PID=
kill -TERM "$DAEMON_PID" 2>/dev/null || true
if ! wait_gone "$DAEMON_PID"; then kill -KILL "$DAEMON_PID" 2>/dev/null || true; fi
wait "$DAEMON_PID" 2>/dev/null || true
for fpid in $(cat "$DIR/fping-pids-running-end.txt"); do
	if [ -r "/proc/$fpid/cmdline" ] && tr '\000' ' ' < "/proc/$fpid/cmdline" | grep -q '[f]ping'; then
		kill -TERM "$fpid" 2>/dev/null || true
	fi
done
sleep 1
for fpid in $(cat "$DIR/fping-pids-running-end.txt"); do
	if [ -r "/proc/$fpid/cmdline" ] && tr '\000' ' ' < "/proc/$fpid/cmdline" | grep -q '[f]ping'; then
		die "fping child remains after shutdown: $fpid"
	fi
done
DAEMON_PID=
phase control-after 15
snapshot before-isolated-stop
pidof fping > "$DIR/fping-pids-after-stop.txt" 2>&1 || true
ps w > "$DIR/processes-after.txt"
cp "$LOG" "$DIR/live-log.txt"
[ ! -e "$BASE/cake-adapt.log.old" ] || cp "$BASE/cake-adapt.log.old" "$DIR/live-log.old.txt"
restore_service || die "could not restore service state"
STOPPED_ORIGINAL=0
tc -d qdisc show > "$DIR/qdisc-after.txt" 2>&1
ubus call service list '{"name":"cake-adapt"}' > "$DIR/service-after.json" 2>&1
apk info -v > "$DIR/packages-after.txt" 2>&1
sha256sum "$DAEMON" /lib/bpf/cake-adapt-tcpdelay.o "$SERVICE" /etc/config/cake-adapt /tmp/cake-adapt-config/main/cake-adapt > "$DIR/installed-sha256-after.txt" 2>&1
cat /proc/mounts > "$DIR/mounts-after.txt"
ls -l /etc/TZ /var /var/log > "$DIR/path-links-after.txt" 2>&1
running_pids > "$DIR/pids-after.txt"
: > "$DIR/daemon-args-after.txt"
for pid in $(running_pids); do tr '\000' ' ' < "/proc/$pid/cmdline" >> "$DIR/daemon-args-after.txt"; echo >> "$DIR/daemon-args-after.txt"; done
if "$SERVICE" enabled >/dev/null 2>&1; then echo 1 > "$DIR/service-enabled-after.txt"; else echo 0 > "$DIR/service-enabled-after.txt"; fi
cat /proc/diskstats > "$DIR/diskstats-after-stop.txt"
cat /proc/interrupts > "$DIR/interrupts-after-stop.txt"
echo "complete $(cat /proc/uptime)" > "$DIR/complete.txt"
