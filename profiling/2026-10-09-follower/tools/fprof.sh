#!/bin/sh
# fprof.sh up|sample NAME SECONDS|strace SECONDS|down
# The capture follower's cost on the x86 VM: the installed daemon runs on the
# testbed with router-like log output (debug, CPU and memory records, 2 MB
# rotation) under a light steady load, so it logs continuously; capture-log.sh
# on the PC follows that log. Test-log rules as in prof.sh.
set -u
DIR=/tmp/fprof
OUT=/root/fprof
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
busy() { awk '/^cpu /{print $2 + $3 + $4 + $7 + $8, $5 + $6}' /proc/stat; }
ticks() { awk '{print $14 + $15 + $16 + $17}' "/proc/$1/stat" 2>/dev/null || echo 0; }
follower() { ps w | awk '/sh \/tmp\/cake-adapt-capture.sh/ && !/awk/ {print $1; exit}'; }
daemon() { ps w | awk -v c="cake-adapt -C $DIR/uci" 'index($0, c) && !/awk/ {print $1; exit}'; }
# BusyBox here has no timeout applet: strace runs in the background and is
# stopped with SIGINT, after which it writes its output.
trace() { # SECONDS strace-arguments...
	seconds=$1; shift
	strace "$@" 2> /dev/null & t=$!
	sleep "$seconds"; kill -INT "$t"; wait "$t"
}
parent() { awk '{print $4}' "/proc/$1/stat"; }

case $1 in
up)
	[ -z "$(ip netns list)" ] || { echo 'existing namespaces; refusing'; exit 1; }
	rm -rf "$DIR"; mkdir -p "$DIR/logs" "$DIR/uci" "$OUT"
	touch "$LOG"; ls -i "$LOG" | awk '{print $1}' > "$OUT/inode-before"
	: > "$LOG"
	ln "$LOG" "$DIR/logs/cake-adapt.log"
	sed -e "s|@LOGS@|$DIR/logs|" -e "/sustained_idle_sleep_thr_s/d" \
		-e "s/option log_file_max_size_KB .*/option log_file_max_size_KB '2048'/" \
		/tmp/fprof-template > "$DIR/uci/cake-adapt"
	echo "	option output_memory_stats '1'" >> "$DIR/uci/cake-adapt"
	sh /tmp/e2e/testbed.sh up > /dev/null 2>&1
	X cpe /usr/sbin/cake-adapt -C "$DIR/uci" -S main > "$DIR/console" 2>&1 &
	# A steady 3 Mbit/s download keeps the line active, so records keep coming.
	X cpe iperf3 -c 10.99.0.2 -p 5202 -R -b 3M -t 3600 > /dev/null 2>&1 &
	echo $! > "$DIR/load"
	;;
sample) # NAME SECONDS: system, daemon, follower and its dropbear session
	name=$2 seconds=$3 daemon=$(daemon)
	f=$(follower); d=${f:+$(parent "$f")}
	set -- $(busy); b0=$1 i0=$2
	t0=$(ticks "$daemon"); f0=$(ticks "${f:-0}"); s0=$(ticks "${d:-0}")
	l0=$(wc -c < "$LOG"); r0=$(cat "$LOG".old 2>/dev/null | wc -c)
	sleep "$seconds"
	set -- $(busy)
	echo "$name seconds=$seconds busy_ticks=$(($1 - b0)) idle_ticks=$(($2 - i0))" \
		"daemon_ticks=$(($(ticks "$daemon") - t0)) follower=${f:-none}" \
		"follower_ticks=$(($(ticks "${f:-0}") - f0)) dropbear=${d:-none}" \
		"dropbear_ticks=$(($(ticks "${d:-0}") - s0))" \
		"log_bytes_now=$(wc -c < "$LOG") log_bytes_start=$l0" >> "$OUT/samples"
	;;
strace) # SECONDS: every process the follower starts and its system calls
	f=$(follower)
	[ -n "$f" ] || { echo 'no follower'; exit 1; }
	trace "$2" -f -tt -o "$OUT/strace.txt" -p "$f"
	trace "$2" -f -c -o "$OUT/strace-summary.txt" -p "$f"
	;;
down)
	kill "$(cat "$DIR/load")" 2> /dev/null
	daemon=$(daemon)
	kill -TERM "$daemon"
	for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$daemon" 2> /dev/null || break; sleep 1; done
	sh /tmp/e2e/testbed.sh down > /dev/null 2>&1
	rm -f "$DIR/logs/cake-adapt.log"
	cp "$DIR/console" "$OUT/"
	ls -i "$LOG" | awk '{print $1}' > "$OUT/inode-after"
	cmp -s "$OUT/inode-before" "$OUT/inode-after" && echo unchanged > "$OUT/inode" || echo CHANGED > "$OUT/inode"
	echo "fping=[$(pidof fping)] namespaces=[$(ip netns list)] follower=[$(follower)]" > "$OUT/leftovers"
	;;
esac
