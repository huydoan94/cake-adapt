#!/bin/sh
# run.sh NAME BINARY PINGER UP_MS THR_MS DOWN_MS
# One cake-adapt variant in the cpe namespace against the emulated ISP.
# The line comes from the environment: bottleneck capacities in Mbit/s
# (UL_STEP is upload during the capacity drop), shaper bounds in kbit/s,
# TCP_ATTRIBUTION=1 for tcp_delay_attribution, ACK_FILTER=1 for CAKE's
# ack-filter on upload, ACTIVE_THR for connection_active_thr_kbps (which may
# not exceed either minimum shaper rate), BACKLOG=1 to sample the emulated
# ISP's real queues every 100 ms. CAKE_MQ=1 as for testbed.sh (cake_mq).
set -eu
T=/tmp/native-icmp-phase3
NAME=$1 BIN=$2 PINGER=$3 UP=$4 THR=$5 DOWN=$6
SECTION=nativecheck_$(printf '%s' "$NAME" | tr '-' '_')
. "$T/children.sh"
: "${UL_CAP:=8}" "${DL_CAP:=40}" "${UL_STEP:=4}"
: "${UL_MIN:=2000}" "${UL_BASE:=6000}" "${UL_MAX:=12000}"
: "${DL_MIN:=10000}" "${DL_BASE:=30000}" "${DL_MAX:=60000}"
: "${TCP_ATTRIBUTION:=0}" "${ACK_FILTER:=0}" "${ACTIVE_THR:=2000}" "${BACKLOG:=0}"
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
now() { cut -d' ' -f1 /proc/uptime; }
cpu() {
    # Parent waited-child counters and live child identities must stay stable.
    for attempt in 1 2 3; do
        children_before=$(children "$DAEMON")
        cat "/proc/$DAEMON/stat" > "$R/cpu-parent-before"
        : > "$R/cpu-children"
        stable=1
        for pid in $children_before; do
            cat "/proc/$pid/stat" >> "$R/cpu-children" 2>/dev/null || stable=0
        done
        cat "/proc/$DAEMON/stat" > "$R/cpu-parent-after"
        children_after=$(children "$DAEMON")
        waited_before=$(awk '{print $16, $17}' "$R/cpu-parent-before")
        waited_after=$(awk '{print $16, $17}' "$R/cpu-parent-after")
        if [ "$stable" = 1 ] && [ "$children_before" = "$children_after" ] && [ "$waited_before" = "$waited_after" ]; then
            now > "$R/cpu-$1.time"
            cat "$R/cpu-parent-after" "$R/cpu-children" > "$R/cpu-$1.stat"
            rm "$R/cpu-parent-before" "$R/cpu-parent-after" "$R/cpu-children"
            return
        fi
    done
    echo 'CPU snapshot unstable after three attempts' >&2
    return 1
}
mark() {
    echo "$(now) $1" >> "$R/phases"
    # Wall-clock marks line up with the daemon's log and the backlog samples.
    bash -c 'echo "$EPOCHREALTIME '"$1"'"' >> "$R/phases-epoch"
}
test ! -e "$R"
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME"
DAEMON= PROBE= SAMPLER= IPERF= DOWNLOAD=
clsact_owned=0
lock_owned=0
test ! -e "/var/run/cake-adapt/$SECTION.lock"
cleanup() {
    status=$?
    set +e
    trap - EXIT HUP INT TERM
    if [ "$clsact_owned" = 1 ]; then
        ip netns exec inet tc qdisc del dev inet0 clsact 2>/dev/null || true
    fi
    for pid in $PROBE $SAMPLER $IPERF $DOWNLOAD; do kill -TERM "$pid" 2>/dev/null || true; done
    if [ -n "$DAEMON" ]; then
        kill -TERM "$DAEMON" 2>/dev/null || true
        for i in 1 2 3 4 5; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
        kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON" || true
        wait "$DAEMON" 2>/dev/null || true
    fi
    for pid in $PROBE $SAMPLER $IPERF $DOWNLOAD; do wait "$pid" 2>/dev/null || true; done
    if [ -e "$LOG" ]; then
        cp "$LOG" "$R/cake-adapt.log" || status=1
        sh "$T/log-inode.sh" > "$R/log-inode-after" || status=1
        if [ -e "$R/log-inode-before" ]; then
            cmp "$R/log-inode-before" "$R/log-inode-after" || status=1
        fi
    fi
    rm -f "$T/logs-$NAME/cake-adapt.$SECTION.log"
    mkdir -p /root/native-icmp-phase3
    cp -a "$R" /root/native-icmp-phase3/ || status=1
    if [ "$lock_owned" = 1 ]; then rm -f "/var/run/cake-adapt/$SECTION.lock"; fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
# Background processes start through ip netns exec directly, so $! is the real process.
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe ($(ip netns pids cpe | tr '\n' ' ')); not starting"
    exit 1
fi

# Fresh shaper and bottleneck state for every variant.
ACK=no-ack-filter
[ "$ACK_FILTER" = 1 ] && ACK=ack-filter
# The kind must match: changing a cake_mq as cake would replace it.
KIND=cake
[ "${CAKE_MQ:-0}" = 1 ] && KIND=cake_mq
X cpe tc qdisc change dev cwan root "$KIND" bandwidth "$UL_BASE"kbit "$ACK"
X cpe tc qdisc change dev ifb4cwan root "$KIND" bandwidth "$DL_BASE"kbit
sh "$T/testbed.sh" rate "$UL_CAP" "$DL_CAP"
echo "$UL_CAP $DL_CAP $UL_STEP" > "$R/capacity"

cat > "$T/uci-$NAME/cake-adapt" <<EOF
config cake_adapt '$SECTION'
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

touch "$LOG"
sh "$T/log-inode.sh" > "$R/log-inode-before"
: > "$LOG"
ln -f "$LOG" "$T/logs-$NAME/cake-adapt.$SECTION.log"
"$BIN" -C "$T/uci-$NAME/cake-adapt" -S "$SECTION" -V || { echo "$NAME: invalid configuration"; exit 1; }
lock_owned=1
ip netns exec cpe "$BIN" -f -C "$T/uci-$NAME/cake-adapt" -S "$SECTION" </dev/null >"$R/daemon-output" 2>&1 &
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

cp "$T/uci-$NAME/cake-adapt" "$R/config"
ip netns exec cpe ip route get 10.99.0.11 > "$R/route"
mark start;          sleep "${IDLE_START:-15}"
kill -0 "$DAEMON"
cpu before
mark upload-steady;  X cpe iperf3 -c 10.99.0.2 -p 5201 -t "${DURATION:-60}" -J > "$R/upload-steady.json"
mark idle;           sleep "${IDLE_GAP:-10}"
mark upload-step
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t "${DURATION:-60}" -J > "$R/upload-step.json" &
IPERF=$!
sleep "${STEP_DURATION:-20}"; mark capacity-low;  sh "$T/testbed.sh" rate "$UL_STEP" "$DL_CAP"
sleep "${STEP_DURATION:-20}"; mark capacity-back; sh "$T/testbed.sh" rate "$UL_CAP" "$DL_CAP"
wait "$IPERF"
IPERF=
if [ "${SEND_CAPTURE_ONLY:-0}" = 1 ]; then
    mark end
    cpu after
    kill -0 "$DAEMON"
    ip netns exec cpe tc -s qdisc show dev cwan > "$R/qdisc-upload"
    ip netns exec cpe tc -s qdisc show dev ifb4cwan > "$R/qdisc-download"
    echo "$NAME: upload capture workload completed"
    exit 0
fi
mark idle;           sleep "${IDLE_GAP:-10}"
mark download-steady; X cpe iperf3 -c 10.99.0.2 -p 5202 -t "${DURATION:-60}" -R -J > "$R/download-steady.json"
mark idle;           sleep "${IDLE_GAP:-10}"
mark bidirectional
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -t "${DURATION:-60}" -R -J > "$R/bidir-download.json" &
DOWNLOAD=$!
X cpe iperf3 -c 10.99.0.2 -p 5201 -t "${DURATION:-60}" -J > "$R/bidir-upload.json"
wait "$DOWNLOAD"
DOWNLOAD=
mark end
cpu after
kill -0 "$DAEMON"
ip netns exec cpe tc -s qdisc show dev cwan > "$R/qdisc-upload"
ip netns exec cpe tc -s qdisc show dev ifb4cwan > "$R/qdisc-download"
# Keep active TCP traffic so health/stall timers run throughout fault injection.
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t 70 -J > "$R/health-upload.json" &
IPERF=$!
ip netns exec inet tc qdisc add dev inet0 clsact
clsact_owned=1
mark loss-one
ip netns exec inet tc filter add dev inet0 egress protocol ip pref 1 u32 match ip protocol 1 0xff match ip src 10.99.0.11/32 action drop
sleep 15
ip netns exec inet tc -s filter show dev inet0 egress > "$R/loss-one-filter"
ip netns exec inet tc filter del dev inet0 egress pref 1
mark recovery-one; sleep 12
mark loss-all
ip netns exec inet tc filter add dev inet0 egress protocol ip pref 1 u32 match ip protocol 1 0xff match ip src 10.99.0.0/24 action drop
sleep 25
ip netns exec inet tc -s filter show dev inet0 egress > "$R/loss-all-filter"
ip netns exec inet tc filter del dev inet0 egress pref 1
mark recovery-all; sleep 12
mark health-end
kill -0 "$DAEMON"
wait "$IPERF"
IPERF=
echo "$NAME: completed"
