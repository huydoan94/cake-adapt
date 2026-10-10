#!/bin/sh
# cake_mq check on the x86 VM: one controlled run with CAKE_MQ=1 and one with
# plain cake, the new build from an isolated directory; results to /root/mq.
T=/tmp/cake-adapt-test
O=/root/mq
rm -rf "$O" /tmp/mq; mkdir -p "$O" /tmp/mq
apk extract --allow-untrusted --destination /tmp/mq /tmp/mq.apk > /dev/null
BIN=/tmp/mq/usr/sbin/cake-adapt
/etc/init.d/cake-adapt stop; sleep 2
cd "$T" || exit 1
for variant in mq cake; do
    [ "$variant" = mq ] && export CAKE_MQ=1 || export CAKE_MQ=0
    sh testbed.sh down > /dev/null 2>&1; sh testbed.sh up > "$O/$variant-up.txt" 2>&1
    ip netns exec cpe tc qdisc show > "$O/$variant-qdiscs-start.txt"
    ( for i in $(seq 1 14); do sleep 20; echo "== $(date +%T)"; ip netns exec cpe tc qdisc show | grep -E "^qdisc cake"; done ) > "$O/$variant-qdiscs-during.txt" 2>&1 &
    snap=$!
    TCP_ATTRIBUTION=1 sh run.sh "$variant" "$BIN" fping 30 15 60 > "$O/$variant-run.txt" 2>&1
    kill "$snap" 2>/dev/null
    ip netns exec cpe tc -s qdisc show > "$O/$variant-qdiscs-end.txt"
    cp "$T/results/$variant/cake-adapt.log" "$O/$variant.log" 2>/dev/null
    tar -C "$T/results" -czf "$O/$variant.tar.gz" "$variant" 2>/dev/null; rm -rf "$T/results/$variant"
    echo "$(date +%T) done $variant" >> "$O/progress"
done
sh testbed.sh down > /dev/null 2>&1
/etc/init.d/cake-adapt start
echo finished >> "$O/progress"
