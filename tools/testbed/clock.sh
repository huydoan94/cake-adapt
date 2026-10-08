#!/bin/sh
# clock.sh NAME BINARY MODE
# tcp_timestamp_inject against a Windows-like client whose own timestamp clock
# may be "older" than the injected TSval, which servers then drop (PAWS).
# The client in cpe sends SYNs without a timestamp yet adopts the server's,
# as Windows does: nftables blanks the timestamp option of its SYNs before
# the injector sees them, and zeroes the TSecr of arriving SYN-ACKs so Linux
# accepts an echo it did not send. Its clock comes from
# net.ipv4.tcp_timestamps=MODE: 2 is milliseconds since boot (Windows-like,
# young), 1 adds a random offset per connection (about half fall in any
# half of the clock). Two rounds of one request to each of 20 servers
# (tcpthink on 5320-5339, timestamps on), each ended by tcpthink's own alarm
# 5 s after it should finish: the second round shows what the injector learned.
# Needs testbed.sh up, nftables and tcpthink.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2 MODE=$3
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME"
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe; not starting"
    exit 1
fi

SERVERS=
for port in $(seq 5320 5339); do
    ip netns exec inet "$T/tcpthink" -s "$port" 0 2000 &
    SERVERS="$SERVERS $!"
done
X cpe nft -f - <<'EOF'
table inet clock_test {
    chain out {
        type filter hook output priority 0;
        tcp flags & (syn | ack) == syn reset tcp option timestamp
    }
    chain pre {
        type filter hook prerouting priority -300;
        tcp flags & (syn | ack) == syn | ack tcp option timestamp tsecr set 0
    }
}
EOF
X cpe sysctl -qw net.ipv4.tcp_timestamps="$MODE"

cat > "$T/uci-$NAME/cake-adapt" <<EOF
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
	option log_file_path_override '$T/logs-$NAME'
	list reflectors '10.99.0.11'
	list reflectors '10.99.0.12'
	list reflectors '10.99.0.13'
	list reflectors '10.99.0.14'
	list reflectors '10.99.0.15'
	list reflectors '10.99.0.16'
EOF
"$BIN" -C "$T/uci-$NAME" -S main -V || { echo "$NAME: invalid configuration"; exit 1; }

: > "$LOG"
ln -f "$LOG" "$T/logs-$NAME/cake-adapt.log"
ip netns exec cpe "$BIN" -C "$T/uci-$NAME" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!
sleep 20
for round in 1 2; do
    for port in $(seq 5320 5339); do
        if X cpe "$T/tcpthink" -c 10.99.0.2 "$port" 0.1 0 200 2000 >/dev/null 2>&1; then
            result=ok
        else
            result=failed
        fi
        echo "$round $port $result" | tee -a "$R/connections"
    done
done
sleep 65

kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
kill $SERVERS 2>/dev/null
X cpe nft delete table inet clock_test
X cpe sysctl -qw net.ipv4.tcp_timestamps=1
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
grep -E 'TCP_INJECT|injection' "$R/cake-adapt.log" | tail -3
awk '{n[$1" "$3]++} END {for (k in n) print "round " k ": " n[k]}' "$R/connections" | sort
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
