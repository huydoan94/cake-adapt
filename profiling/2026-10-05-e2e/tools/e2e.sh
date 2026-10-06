#!/bin/sh
# End-to-end run of one cake-adapt build on the emulated testbed (testbed.sh):
# load phases, a capacity drop, log export/reset, qdisc and IFB lifecycle,
# reflector failures, idle sleep and wake, and shutdown. Every wait is bounded.
# The test log follows the AGENTS.md inode rules; the installed filter object is
# swapped for the build's and restored.
# usage: e2e.sh NAME   (binary bin/NAME, filter obj/NAME.o)
set -u
NAME=$1
DIR=/tmp/e2e
R=$DIR/results/$NAME
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
BIN=$DIR/bin/$NAME
X() { ip netns exec "$@"; }
mark() { printf '%s %s\n' "$(date +%s)" "$*" >> "$R/phases"; }
upload_cake() { X cpe tc qdisc "$1" dev cwan root cake bandwidth "${2:-2250kbit}" besteffort ack-filter ethernet overhead 44 mpu 84; }
download_cake() { X cpe tc qdisc "$1" dev ifb4cwan root cake bandwidth "${2:-27000kbit}" besteffort ethernet overhead 44 mpu 84; }
reflectors() { for host in "$@"; do X inet ip addr "$ACTION" 10.99.0.$host/24 dev inet0; done; }
# The daemon's children by parent PID (its fping; the probe fping is not one), and zombies.
processes() {
	cat /proc/[0-9]*/stat 2>/dev/null | awk -v p="$DAEMON" \
		'$4 == p {c = c $1 ":" $2 " "} $3 == "Z" {z++} END {printf "children=[%s] zombies=%d", c, z}'
}
download() { X cpe iperf3 -c 10.99.0.2 -p 5202 -R -P 4 -t "$1" -J > "$R/$2-download.json" 2>&1; }
upload() { X cpe iperf3 -c 10.99.0.2 -p 5201 -t "$1" -J > "$R/$2-upload.json" 2>&1; }

[ -z "$(ip netns list)" ] || { echo 'existing namespaces; refusing'; exit 1; }
rm -rf "$R"; mkdir -p "$R/logs"
cp "$FILTER" "$R/original-filter.o"
cp "$DIR/obj/$NAME.o" "$FILTER"
touch "$LOG"; ls -i "$LOG" | awk '{print $1}' > "$R/inode-before"
: > "$LOG"
ln "$LOG" "$R/logs/cake-adapt.log"
sed "s|@LOGS@|$R/logs|" "$DIR/uci/cake-adapt" > "$R/cake-adapt"
mkdir -p "$R/uci"; cp "$R/cake-adapt" "$R/uci/cake-adapt"

sh "$DIR/testbed.sh" up
X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 2500kbit burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet
X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate 30000kbit burst 16k limit 1875000 overhead 30 mpu 84 linklayer ethernet
upload_cake change; download_cake change
X inet iperf3 -s -p 5203 -D
X cpe tc -d qdisc show > "$R/qdiscs-start"

ip netns exec cpe fping --timestamp --loop --period 100 --timeout 3000 10.99.0.20 > "$R/probe" 2>/dev/null & PROBE=$!
LD_LIBRARY_PATH="$DIR/lib" ip netns exec cpe "$BIN" -C "$R/uci" -S main > "$R/console" 2>&1 & DAEMON=$!
(
	while kill -0 "$DAEMON" 2>/dev/null; do
		echo "$(date +%s) ul=$(X cpe tc qdisc show dev cwan root | sed -n 's/.*bandwidth \([^ ]*\) .*/\1/p')" \
			"dl=$(X cpe tc qdisc show dev ifb4cwan root 2>/dev/null | sed -n 's/.*bandwidth \([^ ]*\) .*/\1/p')" \
			"ticks=$(awk '{print $14 + $15}' /proc/$DAEMON/stat 2>/dev/null)" \
			"rss=$(awk '/VmRSS/{print $2}' /proc/$DAEMON/status 2>/dev/null)" \
			"hwm=$(awk '/VmHWM/{print $2}' /proc/$DAEMON/status 2>/dev/null)" \
			"fds=$(ls /proc/$DAEMON/fd 2>/dev/null | wc -l)" \
			"$(processes)"
		sleep 1
	done
) > "$R/samples" 2>/dev/null &
SAMPLER=$!

mark start; sleep 12
mark download;       download 25 download
mark recover;        sleep 8
mark upload;         upload 20 upload
mark recover;        sleep 8
mark bidirectional;  download 25 bidirectional & DL=$!; upload 25 bidirectional; wait "$DL"
mark recover;        sleep 8
mark capacity-drop;  upload 30 capacity & UL=$!; sleep 10
	X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 1500kbit burst 16k limit 93750 overhead 30 mpu 84 linklayer ethernet
	mark capacity-1500; sleep 10
	X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 2500kbit burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet
	mark capacity-2500; wait "$UL"
mark recover;        sleep 8

# A light upload keeps the link active, so the idle timer does not end these phases.
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5203 -b 800k -t 400 > /dev/null 2>&1 & KEEP=$!
mark log-export;     kill -USR1 "$DAEMON"; sleep 2; ls -l "$R/logs" >> "$R/phases"
mark "log-reset size=$(wc -c < "$LOG")"; kill -USR2 "$DAEMON"; sleep 2
mark "log-reset-done size=$(wc -c < "$LOG") inode=$(ls -i "$LOG" | awk '{print $1}')"
mark upload-cake-removed;    X cpe tc qdisc del dev cwan root; sleep 4
mark upload-cake-recreated;  upload_cake add; sleep 6
mark download-cake-removed;  X cpe tc qdisc del dev ifb4cwan root; sleep 4
mark download-cake-recreated; download_cake add; sleep 6
mark upload-bandwidth-external; X cpe tc qdisc change dev cwan root cake bandwidth 2000kbit; sleep 4
mark ifb-removed;    X cpe ip link del ifb4cwan; sleep 4
mark ifb-recreated
	X cpe ip link add ifb4cwan type ifb; X cpe ip link set ifb4cwan up; download_cake add
	X cpe tc filter replace dev cwan parent ffff: matchall action mirred egress redirect dev ifb4cwan
	sleep 6
mark ifb-download;   download 10 ifb
mark one-reflector-down; ACTION=del reflectors 11; sleep 15
mark one-reflector-up;   ACTION=add reflectors 11; sleep 5
kill "$KEEP"; wait "$KEEP" 2>/dev/null
mark all-reflectors-down; ACTION=del reflectors 11 12 13 14 15 16; sleep 20
mark all-reflectors-up;   ACTION=add reflectors 11 12 13 14 15 16; sleep 10
mark idle-wait;      sleep 35
mark "idle $(processes)"
mark wake-download;  download 12 wake
mark "awake $(processes)"
sleep 3

mark shutdown
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && { mark "daemon did not stop; SIGKILL"; kill -KILL "$DAEMON"; }
wait "$DAEMON"; mark "daemon-exit $?"
wait "$SAMPLER"
kill "$PROBE"; wait "$PROBE" 2>/dev/null
X cpe tc -d -s qdisc show > "$R/qdiscs-end"
mark "leftover $(processes)"
sh "$DIR/testbed.sh" down
rm -f "$R/logs/cake-adapt.log"
cp "$LOG" "$R/cake-adapt.log"
cp "$R/original-filter.o" "$FILTER"
ls -i "$LOG" | awk '{print $1}' > "$R/inode-after"
cmp -s "$R/inode-before" "$R/inode-after" && mark inode-unchanged || mark INODE-CHANGED
mark "namespaces=[$(ip netns list)] fping=[$(pidof fping)] iperf3=[$(pidof iperf3)]"
mark end
