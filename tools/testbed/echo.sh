#!/bin/sh
# echo.sh NAME BINARY
# Upload queue estimates when the server delays its echo of our timestamps:
# app-limited downloads, where the server idles between bursts, and
# request-response traffic (tcpthink), where it thinks before answering.
# The shapers stay fixed above the bottleneck (adjustment off), so the
# emulated ISP holds every real queue and the 100 ms backlog samples are the
# truth for queues.py. Phases, each followed by idle:
#   app-download-100ms  one download paced in bursts every 100 ms, upload idle
#   app-download-500ms  the same with bursts every 500 ms
#   bulk-download       real download queue, upload idle
#   bulk-upload         real upload queue
#   upload-app-download real upload queue with a 100 ms bursty download
#   think-0-50ms, think-0-200ms
#                       requests every 100 ms on one connection to a server
#                       that waits a random time in that range before
#                       answering, upload idle
#   upload-think-0-200ms real upload queue with the 0-200 ms requests
# The think phases add a steady 5 Mbit/s download, so the daemon stays awake
# and the estimator chooses among flows, as on a real line. THINK_ONLY=1 runs
# only the think phases, without that download; the daemon then stays awake
# through a 50 kbit/s connection_active_thr_kbps.
# tcpthink, built with the OpenWrt toolchain, must be in /tmp/cake-adapt-test.
# EXTRA_UCI holds extra option lines for the variant under test.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2
: "${UL_CAP:=8}" "${DL_CAP:=40}" "${APP_RATE:=10M}" "${PHASE_S:=40}" "${EXTRA_UCI:=}" "${THINK_ONLY:=0}"
ACTIVE_THR=2000
[ "$THINK_ONLY" = 1 ] && ACTIVE_THR=50
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
mark() { bash -c 'echo "$EPOCHREALTIME '"$1"'"' >> "$R/phases-epoch"; }
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME"
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe ($(ip netns pids cpe | tr '\n' ' ')); not starting"
    exit 1
fi

# Shapers above the bottleneck, so queues form at the emulated ISP only.
X cpe tc qdisc change dev cwan root cake bandwidth 12000kbit
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 60000kbit
sh "$T/testbed.sh" rate "$UL_CAP" "$DL_CAP"
echo "$UL_CAP $DL_CAP $UL_CAP" > "$R/capacity"

cat > "$T/uci-$NAME/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '0'
	option min_dl_shaper_rate_kbps '10000'
	option base_dl_shaper_rate_kbps '60000'
	option max_dl_shaper_rate_kbps '60000'
	option adjust_ul_shaper_rate '0'
	option min_ul_shaper_rate_kbps '2000'
	option base_ul_shaper_rate_kbps '12000'
	option max_ul_shaper_rate_kbps '12000'
	option tcp_delay_attribution '1'
	option connection_active_thr_kbps '$ACTIVE_THR'
	option randomize_reflectors '0'
	option output_processing_stats '1'
	option output_load_stats '1'
	option log_file_max_size_KB '50000'
	option log_file_path_override '$T/logs-$NAME'
$EXTRA_UCI
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
# Bytes queued in each tbf bottleneck with its current rate: upload, download.
ip netns exec isp env TBF='/qdisc tbf/ { for (i = 1; i < NF; i++) if ($i == "rate") rate = $(i + 1); tbf = 1 }
    tbf && /backlog/ { sub(/b$/, "", $2); print rate, $2; exit }' bash -c '
    while true; do
        up=$(tc -s qdisc show dev iinet | awk "$TBF")
        down=$(tc -s qdisc show dev iwan | awk "$TBF")
        echo "$EPOCHREALTIME $up $down"
        read -t 0.1 <> <(:)
    done' > "$R/backlog" &
SAMPLER=$!
# Request-response servers in inet: port, think time, reply bytes.
THINKERS=
for server in "5311 0-50" "5312 0-200"; do
    set -- $server
    ip netns exec inet "$T/tcpthink" -s "$1" "$2" 2000 &
    THINKERS="$THINKERS $!"
done
think() { # PORT, with a steady download alongside unless THINK_ONLY=1
    STEADY=
    if [ "$THINK_ONLY" != 1 ]; then
        ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -R -t "$PHASE_S" -b 5M -J > "$R/think-$1-download.json" &
        STEADY=$!
    fi
    X cpe "$T/tcpthink" -c 10.99.0.2 "$1" "$PHASE_S" 100 200 2000
    [ -z "$STEADY" ] || wait "$STEADY"
}

mark start; sleep 15
if [ "$THINK_ONLY" != 1 ]; then
mark app-download-100ms
X cpe iperf3 -c 10.99.0.2 -p 5202 -R -t "$PHASE_S" -b "$APP_RATE" --pacing-timer 100000 -J > "$R/app-100.json"
mark idle; sleep 10
mark app-download-500ms
X cpe iperf3 -c 10.99.0.2 -p 5202 -R -t "$PHASE_S" -b "$APP_RATE" --pacing-timer 500000 -J > "$R/app-500.json"
mark idle; sleep 10
mark bulk-download
X cpe iperf3 -c 10.99.0.2 -p 5202 -R -t "$PHASE_S" -J > "$R/bulk-download.json"
mark idle; sleep 10
mark bulk-upload
X cpe iperf3 -c 10.99.0.2 -p 5201 -t "$PHASE_S" -J > "$R/bulk-upload.json"
mark idle; sleep 10
mark upload-app-download
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -R -t "$PHASE_S" -b "$APP_RATE" --pacing-timer 100000 -J > "$R/mixed-download.json" &
DOWNLOAD=$!
X cpe iperf3 -c 10.99.0.2 -p 5201 -t "$PHASE_S" -J > "$R/mixed-upload.json"
wait "$DOWNLOAD"
mark idle; sleep 10
fi
mark think-0-50ms;  think 5311 > "$R/think-0-50.txt"
mark idle; sleep 10
mark think-0-200ms; think 5312 > "$R/think-0-200.txt"
mark idle; sleep 10
mark upload-think-0-200ms
ip netns exec cpe "$T/tcpthink" -c 10.99.0.2 5312 "$PHASE_S" 100 200 2000 > "$R/mixed-think.txt" &
THINK=$!
X cpe iperf3 -c 10.99.0.2 -p 5201 -t "$PHASE_S" -J > "$R/think-upload.json"
wait "$THINK"
mark idle; sleep 5
mark end

kill "$SAMPLER" $THINKERS 2>/dev/null
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
