#!/bin/sh
# run.sh NAME BINARY PINGER UP_MS THR_MS DOWN_MS
# One cake-adapt variant in the cpe namespace against the emulated ISP.
# The line comes from the environment: bottleneck capacities in Mbit/s
# (UL_STEP is upload during the capacity drop), shaper bounds in kbit/s,
# TCP_ATTRIBUTION=1 for tcp_delay_attribution, ACK_FILTER=1 for CAKE's
# ack-filter on upload, ACTIVE_THR for connection_active_thr_kbps (which may
# not exceed either minimum shaper rate), BACKLOG=1 to sample the emulated
# ISP's real queues every 100 ms.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2 PINGER=$3 UP=$4 THR=$5 DOWN=$6
: "${UL_CAP:=8}" "${DL_CAP:=40}" "${UL_STEP:=4}"
: "${UL_MIN:=2000}" "${UL_BASE:=6000}" "${UL_MAX:=12000}"
: "${DL_MIN:=10000}" "${DL_BASE:=30000}" "${DL_MAX:=60000}"
: "${TCP_ATTRIBUTION:=0}" "${ACK_FILTER:=0}" "${ACTIVE_THR:=2000}" "${BACKLOG:=0}"
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
now() { cut -d' ' -f1 /proc/uptime; }
mark() {
    echo "$(now) $1" >> "$R/phases"
    # Wall-clock marks line up with the daemon's log and the backlog samples.
    bash -c 'echo "$EPOCHREALTIME '"$1"'"' >> "$R/phases-epoch"
}
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME"
# Background processes start through ip netns exec directly, so $! is the real process.
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe ($(ip netns pids cpe | tr '\n' ' ')); not starting"
    exit 1
fi

# Fresh shaper and bottleneck state for every variant.
ACK=no-ack-filter
[ "$ACK_FILTER" = 1 ] && ACK=ack-filter
X cpe tc qdisc change dev cwan root cake bandwidth "$UL_BASE"kbit "$ACK"
X cpe tc qdisc change dev ifb4cwan root cake bandwidth "$DL_BASE"kbit
sh "$T/testbed.sh" rate "$UL_CAP" "$DL_CAP"
echo "$UL_CAP $DL_CAP $UL_STEP" > "$R/capacity"

cat > "$T/uci-$NAME/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '1'
	option min_dl_shaper_rate_kbps '$DL_MIN'
	option base_dl_shaper_rate_kbps '$DL_BASE'
	option max_dl_shaper_rate_kbps '$DL_MAX'
	option adjust_ul_shaper_rate '1'
	option min_ul_shaper_rate_kbps '$UL_MIN'
	option base_ul_shaper_rate_kbps '$UL_BASE'
	option max_ul_shaper_rate_kbps '$UL_MAX'
	option pinger_method '$PINGER'
	option tcp_delay_attribution '$TCP_ATTRIBUTION'
	option connection_active_thr_kbps '$ACTIVE_THR'
	option randomize_reflectors '0'
	option dl_avg_owd_delta_max_adjust_up_thr_ms '$UP'
	option ul_avg_owd_delta_max_adjust_up_thr_ms '$UP'
	option dl_owd_delta_delay_thr_ms '$THR'
	option ul_owd_delta_delay_thr_ms '$THR'
	option dl_avg_owd_delta_max_adjust_down_thr_ms '$DOWN'
	option ul_avg_owd_delta_max_adjust_down_thr_ms '$DOWN'
	option output_processing_stats '1'
	option output_load_stats '1'
	option output_cake_changes '1'
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
ip netns exec cpe fping --timestamp --loop --period 100 --timeout 3000 10.99.0.20 > "$R/probe" 2>/dev/null &
PROBE=$!
SAMPLER=
if [ "$BACKLOG" = 1 ]; then
    # Bytes queued in each tbf bottleneck with its current rate: upload, download.
    # The tbf qdisc'"'"'s rate and backlog bytes; netem'"'"'s 10 ms delay line is not queueing.
    ip netns exec isp env TBF='/qdisc tbf/ { for (i = 1; i < NF; i++) if ($i == "rate") rate = $(i + 1); tbf = 1 }
        tbf && /backlog/ { sub(/b$/, "", $2); print rate, $2; exit }' bash -c '
        while true; do
            up=$(tc -s qdisc show dev iinet | awk "$TBF")
            down=$(tc -s qdisc show dev iwan | awk "$TBF")
            echo "$EPOCHREALTIME $up $down"
            read -t 0.1 <> <(:)
        done' > "$R/backlog" &
    SAMPLER=$!
fi

mark start;          sleep 15
mark upload-steady;  X cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/upload-steady.json"
mark idle;           sleep 10
mark upload-step
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/upload-step.json" &
IPERF=$!
sleep 20; mark capacity-low;  sh "$T/testbed.sh" rate "$UL_STEP" "$DL_CAP"
sleep 20; mark capacity-back; sh "$T/testbed.sh" rate "$UL_CAP" "$DL_CAP"
wait "$IPERF"
mark idle;           sleep 10
mark download-steady; X cpe iperf3 -c 10.99.0.2 -p 5202 -t 60 -R -J > "$R/download-steady.json"
mark idle;           sleep 10
mark bidirectional
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -t 60 -R -J > "$R/bidir-download.json" &
DOWNLOAD=$!
X cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/bidir-upload.json"
wait "$DOWNLOAD"
mark end

kill "$PROBE" $SAMPLER 2>/dev/null
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
X cpe tc qdisc show dev cwan | grep -o "ack-filter[a-z-]*\|no-ack-filter" > "$R/ack-filter"
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
