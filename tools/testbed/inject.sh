#!/bin/sh
# inject.sh [BINARY]: checks tcp_timestamp_inject against the emulated ISP.
# The client in cpe sends no TCP timestamps (net.ipv4.tcp_timestamps=0, as
# Windows), and the servers in inet behave four ways:
#   5202 accepts timestamps;
#   5201 with a cpe rule dropping SYN-ACKs that carry one (a client rejecting);
#   5204 resets SYNs that carry one; 5203 drops them silently.
# Each server gets two connections: the first meets the server's behavior, the
# second shows what the injector learned (a rejected server is skipped, so its
# second connection is not rewritten and works). Then the daemon is stopped, and
# restarted and killed, and a SYN shows whether anything still rewrites it.
# Needs testbed.sh up, nftables, and the test-log rules of run.sh.
T=/tmp/cake-adapt-test
R=$T/results/inject
BIN=${1:-/usr/sbin/cake-adapt}
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
mark() { echo "$(date -u +%T) $1" | tee -a "$R/phases"; }

rm -rf "$R" "$T/uci-inject" "$T/logs-inject"
mkdir -p "$R" "$T/uci-inject" "$T/logs-inject"
if [ -n "$(ip netns pids cpe)" ]; then
    echo "inject: processes still running in cpe; not starting"
    exit 1
fi

X inet iperf3 -s -p 5203 -D
X inet iperf3 -s -p 5204 -D
X inet nft -f - <<'EOF'
table inet inject_test {
    chain input {
        type filter hook input priority 0;
        tcp dport 5203 tcp flags & (syn | ack) == syn tcp option timestamp exists drop
        tcp dport 5204 tcp flags & (syn | ack) == syn tcp option timestamp exists reject with tcp reset
    }
}
EOF
X cpe sysctl -qw net.ipv4.tcp_timestamps=0

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

touch "$LOG"
ls -i "$LOG" | cut -d' ' -f1 > "$R/inode-before"
: > "$LOG"
ln -f "$LOG" "$T/logs-inject/cake-adapt.log"
"$BIN" -C "$T/uci-inject" -S main -V || { echo "inject: invalid configuration"; exit 1; }

# Handshakes and resets on the WAN side of cpe, as the injector sees them.
# Background processes start through ip netns exec directly, so $! is the real process.
ip netns exec cpe tcpdump -n -l -v -i cwan 'tcp[tcpflags] & (tcp-syn|tcp-rst) != 0' > "$R/handshakes" 2>/dev/null &
DUMP=$!
X cpe tc -s qdisc show dev ifb4cwan > "$R/ifb-before"
ip netns exec cpe "$BIN" -C "$T/uci-inject" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!
sleep 10

attempt() { # NAME PORT SECONDS [extra iperf3 arguments]
    name=$1 port=$2 seconds=$3
    shift 3
    start=$(date +%s)
    X cpe iperf3 -c 10.99.0.2 -p "$port" -t "$seconds" --connect-timeout 4000 "$@" -J > "$R/$name.json" 2> "$R/$name.err"
    result=$?
    echo "$name port=$port exit=$result seconds=$(( $(date +%s) - start ))" | tee -a "$R/attempts"
}

mark accept;         attempt accept-1 5202 20 -R
                     attempt accept-2 5202 5 -R
mark client-reject
X cpe nft -f - <<'EOF'
table inet inject_test {
    chain input {
        type filter hook input priority 0;
        ip saddr 10.99.0.2 tcp sport 5201 tcp flags & (syn | ack) == syn | ack tcp option timestamp exists drop
    }
}
EOF
                     attempt client-reject-1 5201 5
                     attempt client-reject-2 5201 15 -R
X cpe nft delete table inet inject_test
mark server-reset;   attempt server-reset-1 5204 5
                     attempt server-reset-2 5204 5
mark server-drop;    attempt server-drop-1 5203 5
                     attempt server-drop-2 5203 5
mark stop
X cpe tc -s qdisc show dev ifb4cwan > "$R/ifb-after"
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
wait "$DAEMON"; echo "daemon exit $?" | tee -a "$R/attempts"
mark after-stop;     attempt after-stop 5202 2 -R

# A daemon killed without warning must not leave its programs behind.
ip netns exec cpe "$BIN" -C "$T/uci-inject" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!
sleep 8
kill -KILL "$DAEMON"; wait "$DAEMON" 2>/dev/null
# Its fping child may outlive it; stop only that one, in cpe.
for pid in $(ip netns pids cpe); do
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = fping ] && kill "$pid"
done
mark after-kill;     attempt after-kill 5202 2 -R
sleep 1
kill "$DUMP"

X cpe sysctl -qw net.ipv4.tcp_timestamps=1
X inet nft delete table inet inject_test
for port in 5203 5204; do
    for pid in $(ip netns pids inet); do
        grep -q -- "-p.$port" "/proc/$pid/cmdline" 2>/dev/null && kill "$pid"
    done
done
cp "$LOG" "$R/cake-adapt.log"
ls -i "$LOG" | cut -d' ' -f1 > "$R/inode-after"
cmp -s "$R/inode-before" "$R/inode-after" && echo "test log inode unchanged" | tee -a "$R/attempts"
