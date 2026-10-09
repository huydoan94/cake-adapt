#!/bin/sh
# Profiles bin/NAME on the testbed under seven 40 s workloads (the last two over
# IPv6); every wait is bounded.
# MODE cpu: CPU ticks, RSS and the filter's and injector's BPF statistics,
# nothing attached.
# MODE perf: the same with perf record -g on the daemon.
# MODE strace: strace -c -f on the daemon over 40 s of bidirectional load.
# Test-log rules as in e2e.sh; the installed filter is swapped and restored, and
# kernel.bpf_stats_enabled is restored to its previous value.
# usage: prof.sh NAME MODE RUN
set -u
NAME=$1 MODE=$2 RUN=$3
DIR=/tmp/e2e
R=$DIR/prof/$RUN
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
SECONDS_PER_WORKLOAD=40
X() { ip netns exec "$@"; }
upload_rate() { X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate "$1" burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet; }

idle() { sleep "$1"; }
SERVER=10.99.0.2
download() { X cpe iperf3 -c "$SERVER" -p 5202 -R -P 4 -t "$1" > /dev/null 2>&1; }
upload() { X cpe iperf3 -c "$SERVER" -p 5201 -t "$1" > /dev/null 2>&1; }
bidirectional() { download "$1" & B=$!; upload "$1"; wait "$B"; }
download6() { SERVER=fd99::2 download "$1"; }
bidirectional6() { SERVER=fd99::2 bidirectional "$1"; }
# Both directions while the bottleneck's upload capacity drops and returns every 5 s.
congested() {
	bidirectional "$1" & C=$!
	end=$(( $(date +%s) + $1 ))
	while [ "$(date +%s)" -lt "$end" ]; do
		upload_rate 1500kbit; sleep 5; upload_rate 2500kbit; sleep 5
	done
	wait "$C"
}
ticks() { awk '{print $14 + $15}' "/proc/$DAEMON/stat"; }
bpf() { # run_cnt run_time_ns of the daemon's filter (type 1), then of its injector (type 3)
	for fd in /proc/$DAEMON/fdinfo/*; do
		awk '/^prog_type/{p=$2} /^prog_id/{i=$2} /^run_cnt/{c=$2} /^run_time_ns/{t=$2}
			END{if (p != "") print p, i, c+0, t+0}' "$fd" 2>/dev/null
	done | sort -u | awk '{c[$1]+=$3; t[$1]+=$4} END{print c[1]+0, t[1]+0, c[3]+0, t[3]+0}'
}

[ -z "$(ip netns list)" ] || { echo 'existing namespaces; refusing'; exit 1; }
rm -rf "$R"; mkdir -p "$R/logs" "$R/uci"
cp "$FILTER" "$R/original-filter.o"
cp "$DIR/obj/$NAME.o" "$FILTER"
stats=$(sysctl -n kernel.bpf_stats_enabled)
sysctl -qw kernel.bpf_stats_enabled=1
touch "$LOG"; ls -i "$LOG" | awk '{print $1}' > "$R/inode-before"
: > "$LOG"
ln "$LOG" "$R/logs/cake-adapt.log"
# Profile with the default 60 s idle threshold, so idle keeps probing, and no debug.
sed -e "s|@LOGS@|$R/logs|" -e "/sustained_idle_sleep_thr_s/d" -e "s/option debug '1'/option debug '0'/" \
	-e "s/option output_cpu_stats '1'/option output_cpu_stats '0'/" "$DIR/uci/cake-adapt" > "$R/uci/cake-adapt"
echo "$NAME $MODE $(sha256sum "$DIR/bin/$NAME" | cut -c1-16) $(uname -r)" > "$R/variant"

sh "$DIR/testbed.sh" up
upload_rate 2500kbit
X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate 30000kbit burst 16k limit 1875000 overhead 30 mpu 84 linklayer ethernet
X cpe tc qdisc change dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 27000kbit besteffort ethernet overhead 44 mpu 84
LD_LIBRARY_PATH="$DIR/lib" ip netns exec cpe "$DIR/bin/$NAME" -C "$R/uci" -S main > "$R/console" 2>&1 & DAEMON=$!
sleep 8

if [ "$MODE" = strace ]; then
	strace -c -f -p "$DAEMON" -o "$R/strace.txt" & TRACE=$!
	bidirectional "$SECONDS_PER_WORKLOAD"
	kill -INT "$TRACE"; wait "$TRACE"
else
	for workload in idle download upload bidirectional congested download6 bidirectional6; do
		before=$(ticks); set -- $(bpf); runs=$1 nanoseconds=$2 inject_runs=$3 inject_ns=$4
		if [ "$MODE" = perf ]; then
			perf record -e cpu-clock -F 999 -g -p "$DAEMON" -o "$R/$workload.perf.data" -- sleep "$SECONDS_PER_WORKLOAD" > "$R/$workload.perf.log" 2>&1 & PERF=$!
		fi
		"$workload" "$SECONDS_PER_WORKLOAD"
		[ "$MODE" = perf ] && wait "$PERF"
		after=$(ticks); set -- $(bpf)
		echo "$workload ticks=$((after - before)) seconds=$SECONDS_PER_WORKLOAD" \
			"rss_kb=$(awk '/VmRSS/{print $2}' /proc/$DAEMON/status)" \
			"hwm_kb=$(awk '/VmHWM/{print $2}' /proc/$DAEMON/status)" \
			"fds=$(ls /proc/$DAEMON/fd | wc -l)" \
			"bpf_runs=$(($1 - runs)) bpf_ns=$(($2 - nanoseconds))" \
			"inject_runs=$(($3 - inject_runs)) inject_ns=$(($4 - inject_ns))" >> "$R/cpu.txt"
		if [ "$MODE" = perf ]; then
			perf script -F comm,pid,tid,time,event,ip,sym,dso -i "$R/$workload.perf.data" 2>/dev/null | gzip -9 > "$R/$workload.perf-script.txt.gz"
			rm -f "$R/$workload.perf.data"
		fi
	done
fi

kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$?" > "$R/daemon-exit"
sh "$DIR/testbed.sh" down
rm -f "$R/logs/cake-adapt.log"
gzip -9c "$LOG" > "$R/cake-adapt.log.gz"
cp "$R/original-filter.o" "$FILTER"
sysctl -qw kernel.bpf_stats_enabled="$stats"
ls -i "$LOG" | awk '{print $1}' > "$R/inode-after"
cmp -s "$R/inode-before" "$R/inode-after" && echo unchanged > "$R/inode" || echo CHANGED > "$R/inode"
echo "fping=[$(pidof fping)] namespaces=[$(ip netns list)] stats=$(sysctl -n kernel.bpf_stats_enabled)" > "$R/leftovers"
