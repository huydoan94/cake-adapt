#!/bin/sh
# Final build: inject.sh over IPv6 and IPv4 with bpftool sizes. Results in /root/final.
T=/tmp/cake-adapt-test
O=/root/final
rm -rf "$O" /tmp/after; mkdir -p "$O" /tmp/after
apk extract --allow-untrusted --destination /tmp/after /tmp/after.apk >/dev/null
BIN=/tmp/after/usr/sbin/cake-adapt
cp -a /lib/bpf/cake-adapt-tcpdelay.o "$O/installed.o"
cp /tmp/after/lib/bpf/cake-adapt-tcpdelay.o /lib/bpf/
/etc/init.d/cake-adapt stop; sleep 2
cd "$T" || exit 1
for f in "v6 fd99::2" "v4 10.99.0.2"; do
    set -- $f
    sh testbed.sh down >/dev/null 2>&1; sh testbed.sh up >/dev/null 2>&1
    ( sleep 15; bpftool prog show > "$O/progs-$1.txt" 2>&1 ) &
    SERVER=$2 RESULTS=inject-$1 sh inject.sh "$BIN" > "$O/inject-$1.txt" 2>&1
    cp "$T/results/inject-$1/cake-adapt.log" "$O/inject-$1.log" 2>/dev/null; rm -rf "$T/results/inject-$1"
done
sh testbed.sh down >/dev/null 2>&1
cp -a "$O/installed.o" /lib/bpf/cake-adapt-tcpdelay.o
/etc/init.d/cake-adapt start
echo "finished $(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12) $(pidof cake-adapt)" > "$O/done"
