#!/bin/sh
# Soak run of bin/NAME on the testbed: CYCLES two-minute cycles of both, download
# and upload load, an idle stretch past the 20 s sleep threshold, and one upload
# CAKE removal and recreation (the TCP filter is reloaded each time). Samples go
# to /root/soak so a VM panic loses only the run in progress. Test-log rules and
# filter handling as in e2e.sh.
# usage: soak.sh NAME CYCLES
set -u
NAME=$1 CYCLES=$2
DIR=/tmp/e2e
R=/root/soak/$NAME
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
X() { ip netns exec "$@"; }
mark() { printf '%s %s\n' "$(date +%s)" "$*" >> "$R/phases"; }
upload_cake() { X cpe tc qdisc "$1" dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84; }
processes() {
	cat /proc/[0-9]*/stat 2>/dev/null | awk -v p="$DAEMON" \
		'$4 == p {c++} $3 == "Z" {z++} END {printf "children=%d zombies=%d", c, z}'
}

[ -z "$(ip netns list)" ] || { echo 'existing namespaces; refusing'; exit 1; }
rm -rf "$R"; mkdir -p "$R" "$DIR/soak-logs" "$DIR/soak-uci"
cp "$FILTER" "$R/original-filter.o"
cp "$DIR/obj/$NAME.o" "$FILTER"
touch "$LOG"; ls -i "$LOG" | awk '{print $1}' > "$R/inode-before"
: > "$LOG"
rm -f "$DIR/soak-logs/"*
ln "$LOG" "$DIR/soak-logs/cake-adapt.log"
# Default 2 MB log limit, so rotation repeats; no debug messages.
sed -e "s|@LOGS@|$DIR/soak-logs|" -e "/log_file_max_size_KB/d" -e "s/option debug '1'/option debug '0'/" \
	"$DIR/uci/cake-adapt" > "$DIR/soak-uci/cake-adapt"
cp "$DIR/soak-uci/cake-adapt" "$R/config"

sh "$DIR/testbed.sh" up
X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 2500kbit burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet
X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate 30000kbit burst 16k limit 1875000 overhead 30 mpu 84 linklayer ethernet
upload_cake change
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 27000kbit besteffort ethernet overhead 44 mpu 84
LD_LIBRARY_PATH="$DIR/lib" ip netns exec cpe "$DIR/bin/$NAME" -C "$DIR/soak-uci" -S main > "$R/console" 2>&1 & DAEMON=$!
(
	while kill -0 "$DAEMON" 2>/dev/null; do
		echo "$(date +%s) ticks=$(awk '{print $14 + $15}' /proc/$DAEMON/stat 2>/dev/null)" \
			"rss=$(awk '/VmRSS/{print $2}' /proc/$DAEMON/status 2>/dev/null)" \
			"vsz=$(awk '/VmSize/{print $2}' /proc/$DAEMON/status 2>/dev/null)" \
			"fds=$(ls /proc/$DAEMON/fd 2>/dev/null | wc -l)" \
			"maps=$(wc -l < /proc/$DAEMON/maps 2>/dev/null)" \
			"$(processes) log=$(wc -c < "$LOG")"
		sleep 10
	done
) > "$R/samples" 2>/dev/null &
SAMPLER=$!

mark start; sleep 10
cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
	mark "cycle $cycle"
	X cpe iperf3 -c 10.99.0.2 -p 5202 -R -P 4 -t 30 > /dev/null 2>&1 & DL=$!
	X cpe iperf3 -c 10.99.0.2 -p 5201 -t 30 > /dev/null 2>&1; wait "$DL"
	X cpe iperf3 -c 10.99.0.2 -p 5202 -R -P 4 -t 20 > /dev/null 2>&1
	X cpe iperf3 -c 10.99.0.2 -p 5201 -t 20 > /dev/null 2>&1
	X cpe tc qdisc del dev cwan root; sleep 3; upload_cake add
	sleep 47
	kill -0 "$DAEMON" 2>/dev/null || { mark "daemon died"; break; }
	cycle=$((cycle + 1))
done

mark shutdown
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && { mark "SIGKILL needed"; kill -KILL "$DAEMON"; }
wait "$DAEMON"; mark "daemon-exit $?"
wait "$SAMPLER"
sh "$DIR/testbed.sh" down
rm -f "$DIR/soak-logs/cake-adapt.log"
gzip -9c "$LOG" > "$R/cake-adapt.log.gz"
cp "$R/original-filter.o" "$FILTER"
ls -i "$LOG" | awk '{print $1}' > "$R/inode-after"
cmp -s "$R/inode-before" "$R/inode-after" && mark inode-unchanged || mark INODE-CHANGED
mark "leftover fping=[$(pidof fping)] namespaces=[$(ip netns list)]"
mark end
