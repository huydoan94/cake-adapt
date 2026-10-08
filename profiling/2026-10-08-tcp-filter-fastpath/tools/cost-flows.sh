#!/bin/sh
# Filter and injector cost under 4 downloads, then 4 uploads, from a client
# without timestamps (injection in plain mode). Results go to /root/inject-cost.
T=/tmp/cake-adapt-test; O=/root/inject-cost-$FLOWS
rm -rf $O; mkdir -p $O
mkdir -p $T/uci-inject $T/logs-inject
LOG=/tmp/sqm-mon-test.log
cat > "$T/uci-inject/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '1'
	option min_dl_shaper_rate_kbps '10000'
	option base_dl_shaper_rate_kbps '30000'
	option max_dl_shaper_rate_kbps '60000'
	option adjust_ul_shaper_rate '1'
	option min_ul_shaper_rate_kbps '2000'
	option base_ul_shaper_rate_kbps '6000'
	option max_ul_shaper_rate_kbps '12000'
	option tcp_delay_attribution '1'
	option tcp_timestamp_inject '1'
	option randomize_reflectors '0'
	option output_processing_stats '1'
	option output_load_stats '1'
	option log_file_max_size_KB '50000'
	option log_file_path_override '$T/logs-inject'
	list reflectors '10.99.0.11'
	list reflectors '10.99.0.12'
	list reflectors '10.99.0.13'
	list reflectors '10.99.0.14'
	list reflectors '10.99.0.15'
	list reflectors '10.99.0.16'
EOF
touch "$LOG"; : > "$LOG"
ln -f "$LOG" "$T/logs-inject/cake-adapt.log"
cd $T && (sh testbed.sh down >/dev/null 2>&1; true) && sh testbed.sh up >/dev/null
ip netns exec cpe sysctl -qw net.ipv4.tcp_timestamps=0
sysctl -qw kernel.bpf_stats_enabled=1
ip netns exec cpe /usr/sbin/cake-adapt -C $T/uci-inject -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!
sleep 10
bpftool prog show > $O/before
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -R -t 20 -P $FLOWS -J > $O/download.json
ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t 20 -P $FLOWS -J > $O/upload.json
bpftool prog show > $O/after
sysctl -qw kernel.bpf_stats_enabled=0
kill -TERM $DAEMON; wait $DAEMON; echo "daemon exit $?" > $O/exit
ip netns exec cpe sysctl -qw net.ipv4.tcp_timestamps=1
sh testbed.sh down >/dev/null 2>&1
cp "$LOG" $O/cake-adapt.log
echo done >> $O/exit
