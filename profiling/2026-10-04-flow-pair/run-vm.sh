#!/bin/sh
# Targeted integration check, not a throughput or directional-accuracy benchmark.
# Usage: run-vm.sh WORKDIR BINARY OBJECT
# Needs the existing tools/testbed/testbed.sh copied into WORKDIR.
set -eu
DIR=$1
BIN=$2
OBJECT=$3
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
OWNED=0
DAEMON=
FILTER_CREATED=0
BPF_DIR_CREATED=0
X() { ip netns exec "$@"; }
mark() { printf '%s %s\n' "$(date +%s)" "$1" >> "$DIR/results/phases"; }

# Abort rather than overwrite existing testbed state or a production filter.
[ -z "$(ip netns list)" ] || { echo 'Existing namespaces; refusing setup'; exit 1; }
[ ! -e "$FILTER" ] || { echo 'Existing BPF object; refusing overwrite'; exit 1; }
mkdir -p "$DIR/results" "$DIR/uci" "$DIR/logs"
for fd in /proc/[0-9]*/fd/*; do
    if [ "$(readlink "$fd" 2>/dev/null || true)" = "$LOG" ]; then
        flags=$(awk '$1 == "flags:" { print $2 }' "${fd%/fd/*}/fdinfo/${fd##*/}")
        if [ "$((flags & 3))" != 0 ]; then
            echo "Existing writer of test log at $fd; inspect before testing"
            exit 1
        fi
    fi
done
touch "$LOG"
ls -i "$LOG" | awk '{ print $1 }' > "$DIR/results/inode-before"
: > "$LOG"
ln "$LOG" "$DIR/logs/cake-adapt.log"

cleanup() {
    trap - EXIT INT TERM HUP
    if [ -n "$DAEMON" ] && kill -0 "$DAEMON" 2>/dev/null; then
        kill -TERM "$DAEMON"
        for n in 1 2 3 4 5; do
            kill -0 "$DAEMON" 2>/dev/null || break
            sleep 1
        done
        kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
        wait "$DAEMON" || true
    fi
    if [ -n "$DAEMON" ]; then
        cp "$LOG" "$DIR/results/cake-adapt.log"
    fi
    [ "$OWNED" = 0 ] || sh "$DIR/testbed.sh" down
    [ "$FILTER_CREATED" = 0 ] || rm -f "$FILTER"
    [ "$BPF_DIR_CREATED" = 0 ] || rmdir /lib/bpf
    rm -f "$DIR/logs/cake-adapt.log"
    ls -i "$LOG" | awk '{ print $1 }' > "$DIR/results/inode-after"
    cmp "$DIR/results/inode-before" "$DIR/results/inode-after"
    ip netns list > "$DIR/results/namespaces-after"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
if [ ! -d /lib/bpf ]; then
    mkdir /lib/bpf
    BPF_DIR_CREATED=1
fi
cp "$OBJECT" "$FILTER"
FILTER_CREATED=1
sha256sum "$BIN" "$FILTER" > "$DIR/results/artifacts.sha256"
OWNED=1
sh "$DIR/testbed.sh" up
sh "$DIR/testbed.sh" rate 20 120
X cpe tc qdisc change dev cwan root cake bandwidth 2mbit
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 80mbit
X inet iperf3 -s -p 5203 -D
X cpe ip route get 10.99.0.2 > "$DIR/results/route"
"$BIN" -C "$DIR/uci" -S main -V > "$DIR/results/config-validation"
ip netns exec cpe "$BIN" -C "$DIR/uci" -S main > "$DIR/results/console" 2>&1 &
DAEMON=$!
sleep 1
kill -0 "$DAEMON"
for fd in /proc/$DAEMON/fdinfo/*; do
    if grep -q '^prog_type' "$fd"; then
        cp "$fd" "$DIR/results/bpf-fdinfo"
    fi
done
mark established-start
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 --connect-timeout 5000 -t 22 -J > "$DIR/results/upload.json" &
UPLOAD=$!
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 --connect-timeout 5000 -t 22 -R -J > "$DIR/results/established-download.json" &
DOWNLOAD=$!
sleep 5
mark add-path-delay
X isp tc qdisc change dev iwan root handle 1: netem delay 90ms limit 100000
X isp tc qdisc change dev iinet root handle 1: netem delay 30ms limit 100000
X isp tc -s qdisc show dev iwan > "$DIR/results/isp-download-qdisc"
X isp tc -s qdisc show dev iinet > "$DIR/results/isp-upload-qdisc"
sleep 4
mark newcomer-start
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5203 --connect-timeout 5000 -t 8 -R -J > "$DIR/results/new-download.json" &
NEWCOMER=$!
wait "$NEWCOMER"
mark newcomer-end
wait "$UPLOAD"
wait "$DOWNLOAD"
mark established-end
X cpe tc -s qdisc show dev cwan > "$DIR/results/cpe-upload-qdisc"
X cpe tc -s qdisc show dev ifb4cwan > "$DIR/results/cpe-download-qdisc"
grep -q 'TCP measurement started' "$LOG"
grep -q '^TCP_QUEUE;' "$LOG"
test -s "$DIR/results/bpf-fdinfo"
