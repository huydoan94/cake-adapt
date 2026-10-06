#!/bin/sh
# Lifecycle check of the refactor build on the emulated testbed: both CAKE
# qdiscs are removed and recreated while the daemon runs. The test log follows
# the AGENTS.md inode protocol; the installed filter is swapped and restored.
set -u
DIR=/tmp/eval
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
R=$DIR/lifecycle
X() { ip netns exec "$@"; }
upload_cake() { X cpe tc qdisc "$1" dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84; }
download_cake() { X cpe tc qdisc "$1" dev ifb4cwan root cake bandwidth 27000kbit besteffort ethernet overhead 44 mpu 84; }
mark() { printf '%s %s\n' "$(date +%s)" "$1" >> "$R/phases"; }
[ -z "$(ip netns list)" ] || { echo 'Existing namespaces; refusing setup'; exit 1; }
rm -rf "$R"; mkdir -p "$R/logs" "$R/uci"
cp "$FILTER" "$R/original-filter.o"
cp "$DIR/obj/refactor.o" "$FILTER"
touch "$LOG"; ls -i "$LOG" | awk '{print $1}' > "$R/inode-before"
: > "$LOG"
ln "$LOG" "$R/logs/cake-adapt.log"
sed "s|option log_file_path_override .*|option log_file_path_override '$R/logs'|" \
    "$DIR/results/conf6/refactor/config" > "$R/uci/cake-adapt"
sh "$DIR/testbed.sh" up
upload_cake change; download_cake change
X cpe tc qdisc show > "$R/qdiscs-start"
LD_LIBRARY_PATH=$DIR/lib X cpe "$DIR/bin/refactor" -C "$R/uci" -S main > "$R/console" 2>&1 & D=$!
sleep 6; mark started
X cpe iperf3 -c 10.99.0.2 -p 5201 -t 30 > /dev/null 2>&1 & U=$!
sleep 4; mark upload-removed;   X cpe tc qdisc del dev cwan root
sleep 4; mark upload-recreated; upload_cake add
sleep 6; mark download-removed;   X cpe tc qdisc del dev ifb4cwan root
sleep 4; mark download-recreated; download_cake add
sleep 6; mark bandwidth-changed-externally
X cpe tc qdisc change dev cwan root cake bandwidth 2000kbit
sleep 4; mark stop
kill -0 "$D" && echo daemon-alive-before-stop > "$R/alive"
kill -TERM "$D"; wait "$D"; echo "exit $?" > "$R/daemon-exit"
wait "$U" 2>/dev/null
X cpe tc qdisc show > "$R/qdiscs-end"
sh "$DIR/testbed.sh" down
rm -f "$R/logs/cake-adapt.log"
cp "$LOG" "$R/cake-adapt.log"
cp "$R/original-filter.o" "$FILTER"
ls -i "$LOG" | awk '{print $1}' > "$R/inode-after"
cmp "$R/inode-before" "$R/inode-after" && echo inode-unchanged
pidof refactor fping iperf3 || echo no-test-processes
