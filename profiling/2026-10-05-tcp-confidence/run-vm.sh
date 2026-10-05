#!/bin/sh
# Baseline retention integration only; no throughput-control or queue-accuracy claim.
# DIR contains before, after, filter.o, testbed.sh, cake-adapt.config, and target tests.
set -eu
DIR=$1
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
DAEMON=
OWNED=0
FILTER_CREATED=0
BPF_DIR_CREATED=0
RESULT=
LOG_LINK=
X() { ip netns exec "$@"; }
mark() { printf '%s %s\n' "$(date +%s)" "$1" >> "$RESULT/phases"; }
snapshot() {
    uname -a > "$DIR/$1.kernel"
    ps w > "$DIR/$1.process"
    pidof cake-adapt > "$DIR/$1.daemon-pids" || true
    ubus call service list '{"name":"cake-adapt"}' > "$DIR/$1.service"
    tc -d qdisc show > "$DIR/$1.qdiscs"
    tc -d -s qdisc show > "$DIR/$1.qdisc-counters"
    ip -d link show > "$DIR/$1.links"
    ip route show table all > "$DIR/$1.routes"
    sha256sum /etc/config/cake-adapt /etc/init.d/cake-adapt /usr/sbin/cake-adapt > "$DIR/$1.files"
    if command -v apk >/dev/null; then apk info -v > "$DIR/$1.packages";
    else opkg list-installed > "$DIR/$1.packages"; fi
    ip netns list > "$DIR/$1.namespaces"
}
stop_case() {
    if [ -n "$DAEMON" ]; then
        kill -TERM "$DAEMON" 2>/dev/null || true
        for n in 1 2 3 4 5; do
            kill -0 "$DAEMON" 2>/dev/null || break
            sleep 1
        done
        kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON" || true
        wait "$DAEMON" || true
        DAEMON=
        cp "$LOG" "$RESULT/cake-adapt.log"
    fi
    if [ "$OWNED" = 1 ]; then sh "$DIR/testbed.sh" down; OWNED=0; fi
    if [ -n "$LOG_LINK" ]; then rm -f "$LOG_LINK"; LOG_LINK=; fi
}
cleanup() {
    trap - EXIT INT TERM HUP
    stop_case
    [ "$FILTER_CREATED" = 0 ] || rm -f "$FILTER"
    [ "$BPF_DIR_CREATED" = 0 ] || rmdir /lib/bpf
    ls -i "$LOG" | awk '{ print $1 }' > "$DIR/inode-after"
    cmp "$DIR/inode-before" "$DIR/inode-after"
    snapshot after
}

[ -z "$(ip netns list)" ] || { echo 'Existing namespaces; refusing setup'; exit 1; }
[ ! -e "$FILTER" ] || { echo 'Existing filter; refusing overwrite'; exit 1; }
mkdir -p "$DIR/results"
snapshot before
touch "$LOG"
ls -i "$LOG" | awk '{ print $1 }' > "$DIR/inode-before"
for fd in /proc/[0-9]*/fd/*; do
    [ "$(ls -iL "$fd" 2>/dev/null | awk '{ print $1 }')" = "$(cat "$DIR/inode-before")" ] || continue
    flags=$(awk '$1 == "flags:" { print $2 }' "${fd%/fd/*}/fdinfo/${fd##*/}" 2>/dev/null || true)
    [ -n "$flags" ] || continue
    [ "$((flags & 3))" = 0 ] || { echo "Existing test-log writer: $fd"; exit 1; }
done
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
"$DIR/test_estimator" > "$DIR/results/test-estimator.txt"
"$DIR/test_controller" > "$DIR/results/test-controller.txt"
sha256sum "$DIR/before" "$DIR/after" "$DIR/filter.o" "$DIR/test_estimator" "$DIR/test_controller" > "$DIR/artifacts.sha256"
if [ ! -d /lib/bpf ]; then mkdir /lib/bpf; BPF_DIR_CREATED=1; fi
cp "$DIR/filter.o" "$FILTER"
FILTER_CREATED=1

for variant in before after; do
    CASE=$DIR/$variant-run
    RESULT=$DIR/results/$variant
    mkdir -p "$CASE/uci" "$CASE/logs" "$RESULT"
    : > "$LOG"
    LOG_LINK=$CASE/logs/cake-adapt.log
    ln "$LOG" "$LOG_LINK"
    cp "$DIR/cake-adapt.config" "$CASE/uci/cake-adapt"
    uci -c "$CASE/uci" set cake-adapt.main.log_file_path_override="$CASE/logs"
    uci -c "$CASE/uci" commit cake-adapt
    cp "$CASE/uci/cake-adapt" "$RESULT/config"
    OWNED=1
    sh "$DIR/testbed.sh" up
    sh "$DIR/testbed.sh" rate 20 120
    X cpe tc qdisc change dev cwan root cake bandwidth 2mbit
    X cpe tc qdisc change dev ifb4cwan root cake bandwidth 80mbit
    X cpe ip route get 10.99.0.2 > "$RESULT/route"
    ip netns exec cpe "$DIR/$variant" -C "$CASE/uci" -S main > "$RESULT/console" 2>&1 &
    DAEMON=$!
    sleep 1
    kill -0 "$DAEMON"
    for fd in /proc/$DAEMON/fdinfo/*; do
        if grep -q '^prog_type' "$fd"; then cp "$fd" "$RESULT/bpf-fdinfo"; fi
    done
    mark warmup
    ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 --connect-timeout 5000 -t 84 -J > "$RESULT/upload.json" &
    UPLOAD=$!
    ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 --connect-timeout 5000 -t 84 -R -J > "$RESULT/download.json" &
    DOWNLOAD=$!
    sleep 5
    mark add-delay
    X isp tc qdisc change dev iwan root handle 1: netem delay 90ms limit 100000
    X isp tc qdisc change dev iinet root handle 1: netem delay 30ms limit 100000
    sleep 70
    mark clear-delay
    X isp tc qdisc change dev iwan root handle 1: netem delay 10ms limit 100000
    X isp tc qdisc change dev iinet root handle 1: netem delay 10ms limit 100000
    sleep 8
    wait "$UPLOAD"
    wait "$DOWNLOAD"
    mark end
    X cpe tc -d -s qdisc show > "$RESULT/cpe-qdiscs"
    X isp tc -d -s qdisc show > "$RESULT/isp-qdiscs"
    grep -q 'TCP measurement started' "$LOG"
    grep -q '^TCP_QUEUE;' "$LOG"
    test -s "$RESULT/bpf-fdinfo"
    stop_case
done
