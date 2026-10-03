#!/bin/sh
# bench.sh NAME BINARY OBJECT
# The TCP filter's own cost under load: one cake-adapt in cpe with
# tcp_delay_attribution and upload_ack_share_min, OBJECT installed as the
# filter, 10 s of warm-up, then 60 s of 4 downloads and 1 upload. The kernel's
# BPF statistics (kernel.bpf_stats_enabled) give the filter's run count and
# run time over the loaded minute, and /proc the daemon's CPU time.
# ADJUST=0 keeps both shapers at their base rate, so filter variants see the
# same traffic.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2 OBJECT=$3
: "${ADJUST:=1}"
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME" /lib/bpf
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe ($(ip netns pids cpe | tr '\n' ' ')); not starting"
    exit 1
fi
cp "$OBJECT" /lib/bpf/cake-adapt-tcpdelay.o
X cpe tc qdisc change dev cwan root cake bandwidth 6000kbit
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 30000kbit
sh "$T/testbed.sh" rate 8 40

cat > "$T/uci-$NAME/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '$ADJUST'
	option min_dl_shaper_rate_kbps '10000'
	option base_dl_shaper_rate_kbps '30000'
	option max_dl_shaper_rate_kbps '60000'
	option adjust_ul_shaper_rate '$ADJUST'
	option min_ul_shaper_rate_kbps '2000'
	option base_ul_shaper_rate_kbps '6000'
	option max_ul_shaper_rate_kbps '12000'
	option tcp_delay_attribution '1'
	option upload_ack_share_min '0.45'
	option connection_active_thr_kbps '2000'
	option randomize_reflectors '0'
	option log_file_max_size_KB '50000'
	option log_file_path_override '$T/logs-$NAME'
	list reflectors '10.99.0.11'
	list reflectors '10.99.0.12'
	list reflectors '10.99.0.13'
	list reflectors '10.99.0.14'
	list reflectors '10.99.0.15'
	list reflectors '10.99.0.16'
EOF

: > "$LOG"
ln -f "$LOG" "$T/logs-$NAME/cake-adapt.log"
"$BIN" -C "$T/uci-$NAME" -S main -V || { echo "$NAME: invalid configuration"; exit 1; }
ip netns exec cpe "$BIN" -C "$T/uci-$NAME" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!

# run_cnt and run_time_ns of the daemon's one BPF program, and its CPU ticks.
sample() {
    for f in /proc/$DAEMON/fdinfo/*; do
        grep -q '^prog_type' "$f" 2>/dev/null || continue
        awk '/^run_cnt/ { c = $2 } /^run_time_ns/ { t = $2 } END { printf "%s %s", c, t }' "$f"
    done
    awk '{ print "", $14 + $15 }' /proc/$DAEMON/stat
}

sleep 10
sample > "$R/start"
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -t 60 -P 4 -R -J > "$R/download.json" &
DOWNLOAD=$!
X cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/upload.json"
wait "$DOWNLOAD"
sample > "$R/end"

kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
grep -q 'TCP measurement started' "$R/cake-adapt.log" || echo "$NAME: capture did not start"
read -r c0 t0 k0 < "$R/start"
read -r c1 t1 k1 < "$R/end"
echo "$NAME: runs=$((c1 - c0)) ns_per_run=$(( (t1 - t0) / (c1 - c0) )) daemon_ticks=$((k1 - k0))" | tee "$R/summary"
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
