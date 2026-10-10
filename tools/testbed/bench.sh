#!/bin/sh
# bench.sh NAME BINARY OBJECT
# The TCP filter's own cost under load: one cake-adapt in cpe with
# tcp_delay_attribution and ul_congest_ack_share, OBJECT installed as the
# filter, 10 s of warm-up, then 60 s of 4 downloads and 1 upload. The kernel's
# BPF statistics (kernel.bpf_stats_enabled) give the filter's run count and
# run time over the loaded minute, and /proc the daemon's CPU time.
# ADJUST=0 keeps both shapers at their base rate, so filter variants see the
# same traffic. INJECT=1 also turns on tcp_ts_request (builds that have
# it; earlier builds read its old name, tcp_timestamp_inject, also written),
# whose injector program is then measured too: summary has one line per
# BPF program (socket filter, then the injector). SERVER is the iperf3
# servers' address: 10.99.0.2, or fd99::2 for IPv6, which the injector reads.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2 OBJECT=$3
: "${ADJUST:=1}" "${INJECT:=0}" "${SERVER:=10.99.0.2}"
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
	option ul_congest_ack_share '0.45'
$([ "$INJECT" = 1 ] && printf "\toption tcp_ts_request '1'\n\toption tcp_timestamp_inject '1'")
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
"$BIN" -C "$T/uci-$NAME/cake-adapt" -S main -V || { echo "$NAME: invalid configuration"; exit 1; }
ip netns exec cpe "$BIN" -C "$T/uci-$NAME/cake-adapt" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!

# Per BPF program of the daemon: type, run_cnt and run_time_ns, one line each,
# then a line with its CPU ticks.
sample() {
    for f in /proc/$DAEMON/fdinfo/*; do
        grep -q '^prog_type' "$f" 2>/dev/null || continue
        awk '/^prog_type/ { p = $2 } /^run_cnt/ { c = $2 } /^run_time_ns/ { t = $2 }
            END { print "program", p, c, t }' "$f"
    done | sort -u
    awk '{ print "ticks", $14 + $15 }' /proc/$DAEMON/stat
}

sleep 10
sample > "$R/start"
ip netns exec cpe iperf3 -c "$SERVER" -p 5202 -t 60 -P 4 -R -J > "$R/download.json" &
DOWNLOAD=$!
X cpe iperf3 -c "$SERVER" -p 5201 -t 60 -J > "$R/upload.json"
wait "$DOWNLOAD"
sample > "$R/end"

kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
grep -q 'TCP measurement started' "$R/cake-adapt.log" || echo "$NAME: capture did not start"
# Programs by type: 1 is the socket filter, 3 the injector (tcx).
awk -v name="$NAME" '
    NR == FNR && $1 == "program" { c[$2] = $3; t[$2] = $4; next }
    NR == FNR && $1 == "ticks" { k = $2; next }
    $1 == "program" && $3 > c[$2] {
        printf "%s: type=%s runs=%d ns_per_run=%d\n", name, $2, $3 - c[$2], ($4 - t[$2]) / ($3 - c[$2])
    }
    $1 == "ticks" { printf "%s: daemon_ticks=%d\n", name, $2 - k }
' "$R/start" "$R/end" | tee "$R/summary"
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
