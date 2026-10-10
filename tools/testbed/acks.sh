#!/bin/bash
# acks.sh ACK_MODE [NAME]: CAKE just below a 1 up / 30 down Mbit/s
# bottleneck, so the ISP queue stays empty; measures the ACK share of upload
# and ICMP and VoIP-like UDP latency while downloads (and one upload) run.
# With DAEMON=<cake-adapt binary>, the daemon controls download (5-27 Mbit/s,
# upload fixed at 900 kbit/s) with ul_congest_ack_share ACK_SHARE (default 0).
T=/tmp/cake-adapt-test
MODE=$1                                   # no-ack-filter | ack-filter
NAME=${2:-acks-$MODE}
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
mark() { echo "$EPOCHREALTIME $1" >> "$R/phases"; }
mkdir -p "$R"
if [ -n "$(ip netns pids cpe)" ]; then echo "cpe busy"; exit 1; fi
# The UDP echo for udpping stays up in inet until testbed.sh down.
if ! ip netns exec inet netstat -uln | grep -q ':7001 '; then
    ip netns exec inet setsid "$T/udpping" -s 7001 > /dev/null 2>&1 < /dev/null &
    sleep 1
fi
sh "$T/testbed.sh" rate 1 30
X cpe tc qdisc change dev cwan root cake bandwidth 900kbit "$MODE"
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 27mbit
X cpe tc qdisc show dev cwan > "$R/cwan-qdisc"

TBF='/qdisc tbf/ { for (i = 1; i < NF; i++) if ($i == "rate") rate = $(i + 1); tbf = 1 }
    tbf && /backlog/ { sub(/b$/, "", $2); print rate, $2; exit }'
ip netns exec isp env TBF="$TBF" bash -c '
    while true; do
        up=$(tc -s qdisc show dev iinet | awk "$TBF"); down=$(tc -s qdisc show dev iwan | awk "$TBF")
        echo "$EPOCHREALTIME $up $down"; read -t 0.1 <> <(:)
    done' > "$R/backlog" &
SAMPLER=$!
ip netns exec cpe tcpdump -i cwan -Q out -s 96 -w "$R/out.pcap" 2> "$R/tcpdump.log" &
DUMP=$!
ip netns exec cpe fping --timestamp --loop --period 100 --timeout 3000 10.99.0.20 > "$R/icmp" 2>/dev/null &
PROBE=$!
# VoIP-like: 160 B every 20 ms (64 kbit/s) and 500 B every 20 ms (200 kbit/s).
ip netns exec cpe "$T/udpping" 10.99.0.11 7001 160 20 270 > "$R/udp64" &
U64=$!
ip netns exec cpe "$T/udpping" 10.99.0.12 7001 500 20 270 > "$R/udp200" &
U200=$!
if [ -n "$DAEMON" ]; then
    mkdir -p "$T/uci-$NAME" "$T/logs-$NAME"
    cat > "$T/uci-$NAME/cake-adapt" <<EOC
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '1'
	option min_dl_shaper_rate_kbps '5000'
	option base_dl_shaper_rate_kbps '27000'
	option max_dl_shaper_rate_kbps '27000'
	option adjust_ul_shaper_rate '0'
	option min_ul_shaper_rate_kbps '900'
	option base_ul_shaper_rate_kbps '900'
	option max_ul_shaper_rate_kbps '900'
	option connection_active_thr_kbps '500'
	option ul_congest_ack_share '${ACK_SHARE:-0}'
	option randomize_reflectors '0'
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
EOC
    # The test log: created once if missing, truncated in place, hard-linked.
    [ -e "$LOG" ] || touch "$LOG"
    ls -i "$LOG" > "$R/test-log-inode"
    : > "$LOG"
    ln -f "$LOG" "$T/logs-$NAME/cake-adapt.log"
    "$DAEMON" -C "$T/uci-$NAME/cake-adapt" -S main -V || { echo "$NAME: invalid configuration"; exit 1; }
    ip netns exec cpe "$DAEMON" -C "$T/uci-$NAME/cake-adapt" -S main < /dev/null > /dev/null 2>&1 &
    DAEMON_PID=$!
fi
sleep 2
mark idle; sleep 10
load() { # NAME DOWNLOAD_STREAMS UPLOAD(0|1)
    mark "$1"
    ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -R -P "$2" -t 40 -J > "$R/$1-download.json" &
    local D=$!
    if [ "$3" = 1 ]; then
        ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t 40 -J > "$R/$1-upload.json"
    fi
    wait "$D"
    mark idle; sleep 5
}
load dl1 1 0
load dl4 4 0
load dl8 8 0
load dl4-ul1 4 1
load dl8-ul1 8 1
mark end
if [ -n "$DAEMON" ]; then
    kill -TERM "$DAEMON_PID"
    for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 1; done
    kill -0 "$DAEMON_PID" 2>/dev/null && kill -KILL "$DAEMON_PID"
    wait "$DAEMON_PID"; echo "$NAME: daemon exit $?"
    cp "$LOG" "$R/cake-adapt.log"
    rm -f "$T/logs-$NAME/cake-adapt.log"
    ls -i "$LOG" >> "$R/test-log-inode"
fi
wait "$U64" "$U200"
kill "$PROBE" "$SAMPLER" 2>/dev/null
kill -INT "$DUMP" 2>/dev/null; wait "$DUMP" 2>/dev/null
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
