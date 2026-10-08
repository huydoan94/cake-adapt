#!/bin/sh
# IPv6 checks of build B on .3: injection scenarios (checksum offload on and
# off, with an IPv4 control), the rewritten SYNs as the server sees them, and
# client clocks. Results in /root/v6.
T=/tmp/cake-adapt-test
O=/root/v6-csum
BIN=/tmp/b/usr/sbin/cake-adapt
rm -rf "$O"; mkdir -p "$O"
cp -a /lib/bpf/cake-adapt-tcpdelay.o "$O/installed.o"
cp /tmp/b/lib/bpf/cake-adapt-tcpdelay.o /lib/bpf/
cd "$T" || exit 1
fresh() { sh testbed.sh down >/dev/null 2>&1; sh testbed.sh up >/dev/null 2>&1; }
# InCsumErrors by name: the Tcp header line, then its values.
csum_errors() {
    ip netns exec inet awk '/^Tcp:/ { if (!n++) for (i = 2; i <= NF; i++) column[$i] = i; else print $column["InCsumErrors"] }' /proc/net/snmp
}
inject_run() { # NAME SERVER OFFLOAD(on|off)
    fresh
    [ "$3" = off ] && ip netns exec cpe ethtool -K cwan tx off >/dev/null 2>&1
    ip netns exec cpe ethtool -k cwan | grep '^tx-checksumming' > "$O/$1.offload"
    echo "before $(csum_errors)" > "$O/$1.csum"
    ip netns exec inet tcpdump -n -l -vv -i inet0 \
        '(ip and tcp[tcpflags] & tcp-syn != 0 and tcp[tcpflags] & tcp-ack == 0) or (ip6 and ip6[6] == 6 and ip6[53] & 0x12 == 0x02)' \
        > "$O/$1.server-syns" 2>/dev/null &
    DUMP=$!
    SERVER=$2 RESULTS=$1 sh inject.sh "$BIN" > "$O/$1.txt" 2>&1
    kill "$DUMP"
    echo "after $(csum_errors)" >> "$O/$1.csum"
    cp -a "results/$1" "$O/$1"
}
inject_run inject-v6-nooffload fd99::2 off
inject_run inject-v4-nooffload 10.99.0.2 off
sh testbed.sh down >/dev/null 2>&1
cp -a "$O/installed.o" /lib/bpf/cake-adapt-tcpdelay.o
echo "finished; object $(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12)" > "$O/done"
