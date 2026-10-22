#!/bin/sh
set -eu
T=/tmp/cake-adapt-test
R=$T/spike
mkdir -p "$R"
watchdog=
cleanup() {
    [ -z "$watchdog" ] || kill "$watchdog" 2>/dev/null || true
    sh "$T/testbed.sh" down
}
trap cleanup EXIT
trap 'exit 143' TERM HUP INT
(sleep 120; kill -TERM "$$") &
watchdog=$!
uname -a > "$R/environment"
cat /proc/sys/kernel/random/boot_id >> "$R/environment"
sha256sum "$T/icmp-driver" "$T/candidate" /usr/sbin/cake-adapt >> "$R/environment"
sh "$T/testbed.sh" up
ip netns exec cpe ip route get 10.99.0.11 >> "$R/environment"
ip netns exec cpe ip -6 route get fd99::11 >> "$R/environment"
for case in v4 v6 ts; do
    target=10.99.0.11 method=native extra=
    [ "$case" != v6 ] || target=fd99::11
    [ "$case" != ts ] || { method=native-ts; extra=--icmp-timestamp; }
    ip netns exec cpe "$T/icmp-driver" cwan "$method" "$target" 30 100000 > "$R/native-$case.csv" 2> "$R/native-$case.status"
    ip netns exec cpe fping -I cwan --timestamp --count 30 --period 100 --timeout 1000 $extra "$target" > "$R/fping-$case.log" 2> "$R/fping-$case.summary"
done
cp -a "$R" /root/native-icmp-spike
echo 'spike completed; saved /root/native-icmp-spike'
