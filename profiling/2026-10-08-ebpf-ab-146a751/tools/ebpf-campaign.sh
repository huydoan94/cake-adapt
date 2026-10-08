#!/bin/sh
# ebpf-campaign.sh: A (f2e68e1 build, as installed) against B (146a751 build)
# on every eBPF behaviour the testbed covers. Each variant runs its own daemon
# with its own filter object; blocks alternate A B B A. The VM's service is
# stopped throughout; results are kept in /root/ebpf after each run.
T=/tmp/cake-adapt-test
O=/root/ebpf
mkdir -p "$O"
cp -a /lib/bpf/cake-adapt-tcpdelay.o "$O/installed.o"
/etc/init.d/cake-adapt stop
sleep 2
progress() { echo "$(date +%T) $*" >> "$O/progress"; }
use() { # VARIANT: sets BIN and installs the variant's filter object
    case $1 in
    A) BIN=/tmp/a/usr/sbin/cake-adapt; cp /tmp/a/lib/bpf/cake-adapt-tcpdelay.o /lib/bpf/ ;;
    B) BIN=/tmp/b-x86/usr/sbin/cake-adapt; cp /tmp/b-x86/lib/bpf/cake-adapt-tcpdelay.o /lib/bpf/ ;;
    esac
}
keep() { # NAME: moves a result out of the testbed directory
    rm -rf "$O/$1"; cp -a "$T/results/$1" "$O/$1"; rm -rf "$T/results/$1"
}
# The injector and filter cost with injection on: 4 downloads, then 4 uploads,
# from a client without timestamps, so every SYN is injected.
cost_inject() { # NAME
    R=$T/results/$1; mkdir -p "$R" "$T/uci-$1" "$T/logs-$1"
    cat > "$T/uci-$1/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '0'
	option min_dl_shaper_rate_kbps '10000'
	option base_dl_shaper_rate_kbps '30000'
	option max_dl_shaper_rate_kbps '60000'
	option adjust_ul_shaper_rate '0'
	option min_ul_shaper_rate_kbps '2000'
	option base_ul_shaper_rate_kbps '6000'
	option max_ul_shaper_rate_kbps '12000'
	option tcp_delay_attribution '1'
	option tcp_timestamp_inject '1'
	option randomize_reflectors '0'
	option output_processing_stats '1'
	option log_file_max_size_KB '50000'
	option log_file_path_override '$T/logs-$1'
	list reflectors '10.99.0.11'
	list reflectors '10.99.0.12'
	list reflectors '10.99.0.13'
	list reflectors '10.99.0.14'
	list reflectors '10.99.0.15'
	list reflectors '10.99.0.16'
EOF
    : > /tmp/sqm-mon-test.log
    ln -f /tmp/sqm-mon-test.log "$T/logs-$1/cake-adapt.log"
    ip netns exec cpe tc qdisc change dev cwan root cake bandwidth 6000kbit
    ip netns exec cpe tc qdisc change dev ifb4cwan root cake bandwidth 30000kbit
    ip netns exec cpe sysctl -qw net.ipv4.tcp_timestamps=0
    sysctl -qw kernel.bpf_stats_enabled=1
    ip netns exec cpe "$BIN" -C "$T/uci-$1" -S main </dev/null >/dev/null 2>&1 &
    DAEMON=$!
    sleep 10
    bpftool prog show > "$R/before"
    ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -R -t 20 -P 4 -J > "$R/download.json"
    ip netns exec cpe iperf3 -c 10.99.0.2 -p 5201 -t 20 -P 4 -J > "$R/upload.json"
    bpftool prog show > "$R/after"
    sysctl -qw kernel.bpf_stats_enabled=0
    kill -TERM "$DAEMON"; wait "$DAEMON"; echo "daemon exit $?" > "$R/exit"
    ip netns exec cpe sysctl -qw net.ipv4.tcp_timestamps=1
    cp /tmp/sqm-mon-test.log "$R/cake-adapt.log"
    rm -f "$T/logs-$1/cake-adapt.log"
}

cd "$T" || exit 1
sh testbed.sh down >/dev/null 2>&1
sh testbed.sh up >/dev/null || { progress "testbed failed"; exit 1; }
progress "start $(sha256sum /tmp/a/lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12) $(sha256sum /tmp/b-x86/lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12)"

# 1. Filter cost with injection off, shapers fixed.
sysctl -qw kernel.bpf_stats_enabled=1
for v in A B B A; do
    n=bench-$v-$(date +%H%M%S); use $v
    ADJUST=0 sh bench.sh "$n" "$BIN" /lib/bpf/cake-adapt-tcpdelay.o > /dev/null 2>&1
    keep "$n"; progress "$n done"
done
sysctl -qw kernel.bpf_stats_enabled=0

# 2. Filter and injector cost with injection on.
for v in A B B A; do
    n=cost-$v-$(date +%H%M%S); use $v
    cost_inject "$n"; keep "$n"; progress "$n done"
done

# 3. Control and TCP queue accuracy: the full run with attribution and backlog.
for v in A B B A; do
    n=run-$v-$(date +%H%M%S); use $v
    TCP_ATTRIBUTION=1 BACKLOG=1 sh run.sh "$n" "$BIN" fping 30 15 60 > /dev/null 2>&1
    keep "$n"; progress "$n done"
done

# 4. ACK accounting: download held for upload ACKs (ul_congest_ack_share 0.45).
for v in A B B A; do
    n=acks-$v-$(date +%H%M%S); use $v
    DAEMON=$BIN ACK_SHARE=0.45 bash acks.sh ack-filter "$n" > /dev/null 2>&1
    keep "$n"; progress "$n done"
done
sh testbed.sh rate 8 40

# 5. Injection behaviour against the four server kinds, stop and kill.
for v in A B; do
    use $v
    sh inject.sh "$BIN" > "$O/inject-$v.txt" 2>&1
    rm -rf "$O/inject-$v"; cp -a "$T/results/inject" "$O/inject-$v"
    progress "inject-$v done"
    sh testbed.sh down >/dev/null 2>&1; sh testbed.sh up >/dev/null
done

# 6. Client clocks, young and random (B; A was measured before).
use B
for m in 2 1; do
    n=clock-B-ts$m
    sh clock.sh "$n" "$BIN" "$m" > "$O/$n.txt" 2>&1
    keep "$n"; progress "$n done"
done

cp -a "$O/installed.o" /lib/bpf/cake-adapt-tcpdelay.o
sh testbed.sh down >/dev/null 2>&1
/etc/init.d/cake-adapt start
progress "finished; object $(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12), service $(pidof cake-adapt)"
