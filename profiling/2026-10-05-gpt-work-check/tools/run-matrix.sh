#!/bin/sh
# Variant matrix built on profiling/2026-10-05-ack-control/tools/run.sh: the
# same testbed, phases, CAKE/ISP settings, probes and sampler. Differences:
# variants come from the command line as name:binary:on|off; the installed
# service is stopped for the batch and its filter is swapped per variant and
# restored afterwards (the VM is test-only); state snapshots are recorded but
# do not fail the run.
set -eu
DIR=$1
MODE=$2
shift 2
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
OWNED=0
LINK_CREATED=0
SERVICE_STOPPED=0
DAEMON=
SAMPLE=
DUMP=
PROBE=
UDP=
SERVER=
RESULT=
DEADLINE_PID=$(awk '{print $4}' /proc/$$/stat)
X() { ip netns exec "$@"; }
mark() { printf '%s %s\n' "$(date +%s)" "$1" >> "$RESULT/phases"; }
snapshot() {
    uname -a > "$DIR/$MODE.$1.kernel"
    ps w > "$DIR/$MODE.$1.process"
    tc -d qdisc show > "$DIR/$MODE.$1.qdiscs"
    sha256sum /etc/config/cake-adapt /usr/sbin/cake-adapt "$FILTER" > "$DIR/$MODE.$1.files" 2>&1 || true
    ip netns list > "$DIR/$MODE.$1.namespaces"
}

stop_case() {
    if [ -n "$SAMPLE" ]; then kill -TERM "$SAMPLE" 2>/dev/null || true; wait "$SAMPLE" || true; SAMPLE=; fi
    if [ -n "$DAEMON" ]; then
        kill -TERM "$DAEMON" 2>/dev/null || true
        for attempt in 1 2 3 4 5; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
        forced=0
        if kill -0 "$DAEMON" 2>/dev/null; then kill -KILL "$DAEMON" || true; forced=1; fi
        if wait "$DAEMON"; then daemon_status=0; else daemon_status=$?; fi
        printf '%s %s\n' "$daemon_status" "$forced" > "$RESULT/daemon-exit-status"
        DAEMON=
    fi
    if [ -n "$DUMP" ]; then kill -INT "$DUMP" 2>/dev/null || true; wait "$DUMP" || true; DUMP=; fi
    if [ "$OWNED" = 1 ]; then
        for ns in cpe isp inet; do
            owned_pids=$(ip netns pids "$ns" 2>/dev/null || true)
            [ -z "$owned_pids" ] || kill -TERM $owned_pids 2>/dev/null || true
        done
        sleep 1
        for ns in cpe isp inet; do
            owned_pids=$(ip netns pids "$ns" 2>/dev/null || true)
            [ -z "$owned_pids" ] || kill -KILL $owned_pids 2>/dev/null || true
        done
        sleep 1
        for ns in cpe isp inet; do
            if ip netns list | awk '{print $1}' | grep -qx "$ns"; then ip netns del "$ns" || return 1; fi
        done
        OWNED=0
    fi
    for child in "$PROBE" "$UDP" "$SERVER"; do [ -z "$child" ] || wait "$child" || true; done
    PROBE= UDP= SERVER=
    if [ -n "$RESULT" ]; then cp "$LOG" "$RESULT/cake-adapt.log"; fi
    if [ "$LINK_CREATED" = 1 ]; then rm -f "$DIR/logs/cake-adapt.log"; LINK_CREATED=0; fi
}
cleanup() {
    status=$?
    trap - EXIT INT TERM HUP
    stop_case || status=1
    cp "$DIR/original-filter.o" "$FILTER" || status=1
    [ "$SERVICE_STOPPED" = 0 ] || /etc/init.d/cake-adapt start || status=1
    ls -i "$LOG" | awk '{print $1}' > "$DIR/$MODE.inode-after"
    cmp "$DIR/$MODE.inode-before" "$DIR/$MODE.inode-after" || status=1
    snapshot after
    printf '%s\n' "$status" > "$DIR/$MODE.exit-status"
    exit "$status"
}
[ -z "$(ip netns list)" ] || { echo 'Existing namespaces; refusing setup'; exit 1; }
[ ! -e "$DIR/logs/cake-adapt.log" ]
[ -e "$DIR/original-filter.o" ] || cp "$FILTER" "$DIR/original-filter.o"
snapshot before
touch "$LOG"
ls -i "$LOG" | awk '{print $1}' > "$DIR/$MODE.inode-before"
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
/etc/init.d/cake-adapt stop
SERVICE_STOPPED=1
: > "$LOG"
mkdir -p "$DIR/logs" "$DIR/results/$MODE"
sha256sum "$DIR"/bin/* "$DIR"/obj/* "$DIR/sample" "$DIR/udpping" "$DIR/lib/libbpf.so.1" "$DIR/lib/libelf.so.1" > "$DIR/$MODE.artifacts.sha256"
grep '^Cpus_allowed_list:' /proc/$$/status > "$DIR/$MODE.affinity"
[ "$(awk '{print $2}' "$DIR/$MODE.affinity")" = 0 ]
for spec in "$@"; do
    variant=${spec%%:*}
    rest=${spec#*:}
    build=${rest%%:*}
    extensions=${rest#*:}
    RESULT="$DIR/results/$MODE/$variant"
    mkdir -p "$RESULT" "$DIR/uci"
    : > "$LOG"
    ln "$LOG" "$DIR/logs/cake-adapt.log"
    LINK_CREATED=1
    BINARY="$DIR/bin/$build"
    cp "$DIR/obj/$build.o" "$FILTER"
    attribution=1 share=0.45
    [ "$extensions" = on ] || { attribution=0; share=0; }
    printf '%s %s %s\n' "$variant" "$build" "$extensions" > "$RESULT/variant"
    cat > "$DIR/uci/cake-adapt" <<EOF
config cake_adapt 'main'
    option enabled '1'
    option interface 'cwan'
    option adjust_dl_shaper_rate '1'
    option min_dl_shaper_rate_kbps '5000'
    option base_dl_shaper_rate_kbps '27000'
    option max_dl_shaper_rate_kbps '33000'
    option adjust_ul_shaper_rate '1'
    option min_ul_shaper_rate_kbps '1000'
    option base_ul_shaper_rate_kbps '2250'
    option max_ul_shaper_rate_kbps '3000'
    option connection_active_thr_kbps '500'
    option tcp_delay_attribution '$attribution'
    option OPTION_ACK_SHARE '$share'
    option randomize_reflectors '0'
    option output_processing_stats '1'
    option output_load_stats '1'
    option output_cake_changes '1'
    option output_cpu_stats '1'
    option log_file_max_size_KB '50000'
    option log_file_path_override '$DIR/logs'
    list reflectors '10.99.0.11'
    list reflectors '10.99.0.12'
    list reflectors '10.99.0.13'
    list reflectors '10.99.0.14'
    list reflectors '10.99.0.15'
    list reflectors '10.99.0.16'
EOF
    # The ACK option was renamed on 2026-10-04; each build reads its own name.
    ack_option=$("$BINARY" -L | grep -E '^(ul_congest_ack_share|upload_ack_congested_share|upload_ack_share_min)$' | head -1)
    sed -i "s/OPTION_ACK_SHARE/$ack_option/" "$DIR/uci/cake-adapt"
    cp "$DIR/uci/cake-adapt" "$RESULT/config"
    "$BINARY" -C "$DIR/uci" -S main -V > "$RESULT/config-validation" 2>&1
    OWNED=1
    sh "$DIR/testbed.sh" up
    X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 2500kbit burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet
    X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate 30000kbit burst 16k limit 1875000 overhead 30 mpu 84 linklayer ethernet
    X cpe tc qdisc change dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84
    X cpe tc qdisc change dev ifb4cwan root cake bandwidth 27000kbit besteffort ethernet overhead 44 mpu 84
    X cpe ip route get 10.99.0.2 > "$RESULT/route"
    grep -q 'dev cwan' "$RESULT/route"
    X cpe tc -d -s qdisc show > "$RESULT/cpe-qdiscs-start"
    X isp tc -d -s qdisc show > "$RESULT/isp-qdiscs-start"
    ip netns exec inet "$DIR/udpping" -s 7001 > "$RESULT/udp-server" 2>&1 & SERVER=$!
    ip netns exec cpe tcpdump -i cwan -Q out -s 128 -w "$RESULT/out.pcap" 2> "$RESULT/tcpdump.log" & DUMP=$!
    ip netns exec cpe fping --timestamp --loop --period 100 --timeout 3000 10.99.0.20 > "$RESULT/icmp" 2> "$RESULT/fping-console" & PROBE=$!
    ip netns exec cpe "$BINARY" -C "$DIR/uci" -S main > "$RESULT/console" 2>&1 & DAEMON=$!
    sleep 1
    kill -0 "$DAEMON"
    grep '^Cpus_allowed_list:' /proc/$DAEMON/status > "$RESULT/daemon-affinity"
    program_id=0
    for fd in /proc/$DAEMON/fdinfo/*; do
        if grep -q '^prog_type' "$fd"; then
            cp "$fd" "$RESULT/bpf-fdinfo"
            program_id=$(awk '$1 == "prog_id:" {print $2}' "$fd")
        fi
    done
    if [ "$extensions" = off ]; then [ "$program_id" = 0 ];
    else [ "$program_id" -gt 0 ]; grep -q 'TCP measurement started' "$LOG"; fi
    ip netns exec cpe "$DIR/sample" "$DAEMON" "$program_id" 110 > "$RESULT/samples" 2> "$RESULT/sample-console" & SAMPLE=$!
    mark udp-start
    ip netns exec cpe "$DIR/udpping" 10.99.0.20 7001 160 20 100 > "$RESULT/udp64" 2> "$RESULT/udp-console" & UDP=$!
    mark idle
    sleep 8
    load() {
        phase=$1 seconds=$2 direction=$3
        mark "$phase"
        if [ "$direction" != upload ]; then
            ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 --connect-timeout 5000 -R -P 4 -t "$seconds" -J > "$RESULT/$phase-download.json" & DOWNLOAD=$!
        fi
        if [ "$direction" != download ]; then
            ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 --connect-timeout 5000 -t "$seconds" -J > "$RESULT/$phase-upload.json" & UPLOAD=$!
        fi
        if [ "$direction" != upload ]; then wait "$DOWNLOAD"; fi
        if [ "$direction" != download ]; then wait "$UPLOAD"; fi
        kill -0 "$DAEMON" "$SAMPLE" "$DUMP" "$PROBE" "$UDP"
    }
    load upload 15 upload
    mark quiet; sleep 3
    load download 20 download
    mark quiet; sleep 3
    load mixed 25 mixed
    mark recovery; sleep 8
    mark end
    X cpe tc -d -s qdisc show > "$RESULT/cpe-qdiscs-end"
    X isp tc -d -s qdisc show > "$RESULT/isp-qdiscs-end"
    kill -TERM "$SAMPLE"; wait "$SAMPLE"; SAMPLE=
    [ "$extensions" = off ] || grep -q '^TCP_QUEUE;' "$LOG"
    kill -TERM "$UDP"; wait "$UDP"; UDP=
    stop_case
    [ "$(cat "$RESULT/daemon-exit-status")" = '0 0' ]
    echo "COLLECTED $MODE $variant"
done
