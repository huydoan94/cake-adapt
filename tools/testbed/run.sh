#!/bin/sh
# run.sh NAME BINARY PINGER UP_MS THR_MS DOWN_MS [DRAIN_MS]
# One cake-adapt variant in the cpe namespace against the emulated ISP.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2 PINGER=$3 UP=$4 THR=$5 DOWN=$6 DRAIN=$7
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
now() { cut -d' ' -f1 /proc/uptime; }
mark() { echo "$(now) $1" >> "$R/phases"; }
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME"
# Background processes start through ip netns exec directly, so $! is the real process.
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe ($(ip netns pids cpe | tr '\n' ' ')); not starting"
    exit 1
fi

# Fresh shaper and bottleneck state for every variant.
X cpe tc qdisc change dev cwan root cake bandwidth 6mbit
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 30mbit
sh "$T/testbed.sh" rate 8 40

cat > "$T/uci-$NAME/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '1'
	option min_dl_shaper_rate_kbps '10000'
	option base_dl_shaper_rate_kbps '30000'
	option max_dl_shaper_rate_kbps '60000'
	option adjust_ul_shaper_rate '1'
	option min_ul_shaper_rate_kbps '2000'
	option base_ul_shaper_rate_kbps '6000'
	option max_ul_shaper_rate_kbps '12000'
	option pinger_method '$PINGER'
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
	option log_file_path_override '$T/logs-$NAME'
	list reflectors '10.99.0.11'
	list reflectors '10.99.0.12'
	list reflectors '10.99.0.13'
	list reflectors '10.99.0.14'
	list reflectors '10.99.0.15'
	list reflectors '10.99.0.16'
EOF
[ -n "$DRAIN" ] && printf "\toption queue_drain_period_ms '%s'\n" "$DRAIN" >> "$T/uci-$NAME/cake-adapt"

: > "$LOG"
ln -f "$LOG" "$T/logs-$NAME/cake-adapt.log"
"$BIN" -C "$T/uci-$NAME" -S main -V || { echo "$NAME: invalid configuration"; exit 1; }
ip netns exec cpe "$BIN" -C "$T/uci-$NAME" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!
ip netns exec cpe fping --timestamp --loop --period 100 --timeout 3000 10.99.0.20 > "$R/probe" 2>/dev/null &
PROBE=$!

mark start;          sleep 15
mark upload-steady;  X cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/upload-steady.json"
mark idle;           sleep 10
mark upload-step
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/upload-step.json" &
IPERF=$!
sleep 20; mark capacity-4;  sh "$T/testbed.sh" rate 4 40
sleep 20; mark capacity-8;  sh "$T/testbed.sh" rate 8 40
wait "$IPERF"
mark idle;           sleep 10
mark bidirectional
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -t 60 -R -J > "$R/bidir-download.json" &
DOWNLOAD=$!
X cpe iperf3 -c 10.99.0.2 -p 5201 -t 60 -J > "$R/bidir-upload.json"
wait "$DOWNLOAD"
mark end

kill "$PROBE" 2>/dev/null
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
