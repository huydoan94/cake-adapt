#!/bin/sh
set -eu
T=/tmp/cake-adapt-test
R=$T/paired-sendtime
mkdir -p "$R"
watchdog=
cleanup() {
    [ -z "$watchdog" ] || kill "$watchdog" 2>/dev/null || true
    sh "$T/testbed.sh" down
}
trap cleanup EXIT
trap 'exit 143' TERM HUP INT
(sleep 90; kill -TERM "$$") &
watchdog=$!
sh "$T/testbed.sh" up
for case in network loopback timestamp; do
    target=127.0.0.1 interface=lo method=native extra=
    [ "$case" != network ] || { target=10.99.0.11; interface=cwan; }
    [ "$case" != timestamp ] || { method=native-ts; extra=--icmp-timestamp; }
    ip netns exec cpe "$T/icmp-driver" "$interface" "$method" "$target" 30 100000 > "$R/native-$case.csv" 2> "$R/native-$case.status" &
    native=$!
    ip netns exec cpe fping -I "$interface" --timestamp --count 30 --period 100 --timeout 1000 $extra "$target" > "$R/fping-$case.log" 2> "$R/fping-$case.summary" &
    fping=$!
    wait "$native"
    wait "$fping"
done
cp -a "$R" /root/native-icmp-spike-sendtime
echo 'paired diagnosis complete'
