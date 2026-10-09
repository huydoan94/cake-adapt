#!/bin/sh
# Cleanup A/B on .2: filter and injector cost of the pre-cleanup (B) and
# cleaned (A) builds over IPv4 and IPv6 traffic, alternated B A A B in one
# session; then inject.sh over IPv6 with A. Results in /root/cleanup.
T=/tmp/cake-adapt-test
O=/root/cleanup
rm -rf "$O"; mkdir -p "$O"
for v in before after; do
    rm -rf /tmp/$v; mkdir -p /tmp/$v
    apk extract --allow-untrusted --destination /tmp/$v /tmp/$v.apk >/dev/null
done
cp -a /lib/bpf/cake-adapt-tcpdelay.o "$O/installed.o"
/etc/init.d/cake-adapt stop; sleep 2
cd "$T" || exit 1
fresh() { sh testbed.sh down >/dev/null 2>&1; sh testbed.sh up >/dev/null 2>&1; }
use() { BIN=/tmp/$1/usr/sbin/cake-adapt; cp /tmp/$1/lib/bpf/cake-adapt-tcpdelay.o /lib/bpf/; }
sysctl -qw kernel.bpf_stats_enabled=1
for family in "v4 10.99.0.2" "v6 fd99::2"; do
    set -- $family
    for v in before after after before; do
        n=$1-$v-$(date +%H%M%S); use $v; fresh
        ( sleep 30; bpftool prog show > "$O/$n.progs" 2>&1 ) &
        SERVER=$2 ADJUST=0 INJECT=1 sh bench.sh "$n" "$BIN" /lib/bpf/cake-adapt-tcpdelay.o > "$O/$n.txt" 2>&1
        rm -rf "$T/results/$n"; echo "$(date +%T) $n" >> "$O/progress"
    done
done
sysctl -qw kernel.bpf_stats_enabled=0
use after; fresh
RESULTS=inject-v6 sh inject.sh "$BIN" > "$O/inject-v6.txt" 2>&1
cp "$T/results/inject-v6/cake-adapt.log" "$O/inject-v6.log" 2>/dev/null; rm -rf "$T/results/inject-v6"
sh testbed.sh down >/dev/null 2>&1
cp -a "$O/installed.o" /lib/bpf/cake-adapt-tcpdelay.o
/etc/init.d/cake-adapt start
echo "finished $(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12) $(pidof cake-adapt)" > "$O/done"
