#!/bin/sh
# One reflector (10.99.0.13) answers 50 ms late for 1 s in every 5 while light
# TCP runs both ways; each build runs observation-only for 100 s. Bounded.
T=/tmp/cake-adapt-test
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
spikes() {
	i=0
	while [ $i -lt 20 ]; do
		sleep 4
		X inet tc qdisc change dev inet0 parent 1:3 handle 30: netem delay 50ms
		sleep 1
		X inet tc qdisc change dev inet0 parent 1:3 handle 30: netem delay 0ms
		i=$((i + 1))
	done
}
sh $T/testbed.sh up > $T/up.log 2>&1
# ICMP from 10.99.0.13 goes to band 3 (ets, as sch_prio is not built), whose netem delay the spikes switch.
X inet tc qdisc add dev inet0 root handle 1: ets bands 3 strict 3 priomap 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 || exit 1
X inet tc qdisc add dev inet0 parent 1:3 handle 30: netem delay 0ms || exit 1
X inet tc filter add dev inet0 parent 1: protocol ip u32 match ip src 10.99.0.13/32 match ip protocol 1 0xff flowid 1:3 || exit 1
mkdir -p $T/logs $T/uci $T/results
for build in prev fix; do
	cp $T/$build.o /lib/bpf/cake-adapt-tcpdelay.o
	cat > $T/uci/cake-adapt <<EOC
config cake_adapt main
	option enabled 1
	option interface cwan
	option adjust_dl_shaper_rate 0
	option adjust_ul_shaper_rate 0
	option tcp_delay_attribution 1
	option output_processing_stats 1
	option connection_active_thr_kbps 100
	option log_file_path_override $T/logs
	list reflectors 10.99.0.11
	list reflectors 10.99.0.12
	list reflectors 10.99.0.13
	list reflectors 10.99.0.14
	list reflectors 10.99.0.15
	list reflectors 10.99.0.16
EOC
	: > $LOG
	ln $LOG $T/logs/cake-adapt.log
	ip netns exec cpe $T/$build -C $T/uci -S main > /dev/null 2>&1 & D=$!
	sleep 10
	ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -b 300k -t 100 > /dev/null 2>&1 & U=$!
	ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -R -b 3M -t 100 > /dev/null 2>&1 & W=$!
	spikes
	wait $U $W
	kill -TERM $D; wait $D; echo "$build daemon exit $?"
	cp $LOG $T/results/$build.log
	rm -f $T/logs/cake-adapt.log
done
cp $T/orig.o /lib/bpf/cake-adapt-tcpdelay.o
sh $T/testbed.sh down > /dev/null 2>&1
ip netns list
ls -i $LOG
