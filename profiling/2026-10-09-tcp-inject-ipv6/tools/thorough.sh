#!/bin/sh
# Thorough test of the working tree (after) on .2: functional suite, then the
# filter/injector cost of the last commit (before) and after over IPv4 and
# IPv6, alternated B A A B B A A B. Results archived to /root/thorough as
# each run ends.
T=/tmp/cake-adapt-test
O=/root/thorough
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
keep() { tar -C "$T/results" -czf "$O/$1.tar.gz" "$1" 2>/dev/null; rm -rf "$T/results/$1"; echo "$(date +%T) $1" >> "$O/progress"; }
use after
fresh; ( sleep 15; bpftool prog show > "$O/progs.txt" 2>&1 ) &
RESULTS=inject-v6 sh inject.sh "$BIN" > "$O/inject-v6.txt" 2>&1; keep inject-v6
fresh; SERVER=10.99.0.2 RESULTS=inject-v4 sh inject.sh "$BIN" > "$O/inject-v4.txt" 2>&1; keep inject-v4
fresh; sh clock.sh young-v6 "$BIN" 2 > "$O/young-v6.txt" 2>&1; keep young-v6
fresh; sh clock.sh random-v6 "$BIN" 1 > "$O/random-v6.txt" 2>&1; keep random-v6
fresh; OLD_CLOCK=3000000000 sh clock.sh old-v6 "$BIN" 2 > "$O/old-v6.txt" 2>&1; keep old-v6
fresh; PARALLEL=1 OLD_CLOCK=3000000000 sh clock.sh old-v6-parallel "$BIN" 2 > "$O/old-v6-parallel.txt" 2>&1; keep old-v6-parallel
fresh; SERVER=10.99.0.2 OLD_CLOCK=3000000000 sh clock.sh old-v4 "$BIN" 2 > "$O/old-v4.txt" 2>&1; keep old-v4
fresh; TCP_ATTRIBUTION=1 BACKLOG=1 sh run.sh run "$BIN" fping 30 15 60 > "$O/run.txt" 2>&1; keep run
sysctl -qw kernel.bpf_stats_enabled=1
for family in "v4 10.99.0.2" "v6 fd99::2"; do
    set -- $family
    for v in before after after before before after after before; do
        n=$1-$v-$(date +%H%M%S); use $v; fresh
        SERVER=$2 ADJUST=0 INJECT=1 sh bench.sh "$n" "$BIN" /lib/bpf/cake-adapt-tcpdelay.o > "$O/$n.txt" 2>&1
        rm -rf "$T/results/$n"; echo "$(date +%T) $n" >> "$O/progress"
    done
done
sysctl -qw kernel.bpf_stats_enabled=0
sh testbed.sh down >/dev/null 2>&1
cp -a "$O/installed.o" /lib/bpf/cake-adapt-tcpdelay.o
/etc/init.d/cake-adapt start
echo "finished $(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12) $(pidof cake-adapt)" > "$O/done"
