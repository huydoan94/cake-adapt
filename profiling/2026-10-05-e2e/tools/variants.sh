#!/bin/sh
# Short variant runs of bin/NAME on the testbed: fping-ts as the pinger, and the
# TCP filter object missing. Test-log inode rules as in e2e.sh; the installed
# filter is saved first and restored at the end.
# usage: variants.sh NAME
set -u
NAME=$1
DIR=/tmp/e2e
LOG=/tmp/sqm-mon-test.log
FILTER=/lib/bpf/cake-adapt-tcpdelay.o
X() { ip netns exec "$@"; }

run_case() { # CASE SED-EXPRESSION
	R=$DIR/results/$NAME-$1
	rm -rf "$R"; mkdir -p "$R/logs" "$R/uci"
	touch "$LOG"; ls -i "$LOG" | awk '{print $1}' > "$R/inode-before"
	: > "$LOG"
	ln "$LOG" "$R/logs/cake-adapt.log"
	sed -e "s|@LOGS@|$R/logs|" -e "$2" "$DIR/uci/cake-adapt" > "$R/uci/cake-adapt"
	sh "$DIR/testbed.sh" up
	X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate 2500kbit burst 16k limit 156250 overhead 30 mpu 84 linklayer ethernet
	X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate 30000kbit burst 16k limit 1875000 overhead 30 mpu 84 linklayer ethernet
	X cpe tc qdisc change dev cwan root cake bandwidth 2250kbit besteffort ack-filter ethernet overhead 44 mpu 84
	X cpe tc qdisc change dev ifb4cwan root cake bandwidth 27000kbit besteffort ethernet overhead 44 mpu 84
	LD_LIBRARY_PATH="$DIR/lib" ip netns exec cpe "$DIR/bin/$NAME" -C "$R/uci" -S main > "$R/console" 2>&1 & DAEMON=$!
	sleep 8
	X cpe iperf3 -c 10.99.0.2 -p 5202 -R -P 4 -t 25 -J > "$R/download.json" 2>&1 & DL=$!
	X cpe iperf3 -c 10.99.0.2 -p 5201 -t 25 -J > "$R/upload.json" 2>&1
	wait "$DL"
	sleep 3
	kill -TERM "$DAEMON"
	for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
	kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
	wait "$DAEMON"; echo "$?" > "$R/daemon-exit"
	sh "$DIR/testbed.sh" down
	rm -f "$R/logs/cake-adapt.log"
	cp "$LOG" "$R/cake-adapt.log"
	ls -i "$LOG" | awk '{print $1}' > "$R/inode-after"
	cmp -s "$R/inode-before" "$R/inode-after" && echo unchanged > "$R/inode" || echo CHANGED > "$R/inode"
	echo "fping=[$(pidof fping)] namespaces=[$(ip netns list)]" > "$R/leftovers"
}

[ -z "$(ip netns list)" ] || { echo 'existing namespaces; refusing'; exit 1; }
cp "$FILTER" "$DIR/results/original-filter.o"
cp "$DIR/obj/$NAME.o" "$FILTER"
# fping-ts reports one-way delays itself, so TCP attribution (fping only) is off.
run_case fping-ts "s/option tcp_delay_attribution '1'/option tcp_delay_attribution '0'\n\toption pinger_method 'fping-ts'/"
rm -f "$FILTER"
run_case no-filter "s/^x//"
cp "$DIR/results/original-filter.o" "$FILTER"
sha256sum "$FILTER" "$DIR/results/original-filter.o"
