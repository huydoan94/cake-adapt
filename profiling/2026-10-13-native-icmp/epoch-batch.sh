#!/bin/bash
set -eu
T=/tmp/native-icmp-phase3
. "$T/children.sh"
MODE=${MODE:-acceptance}
case "$MODE" in smoke|acceptance) ;; *) exit 2 ;; esac
test ! -e "$T/results"
test ! -e /root/native-icmp-phase3
mkdir "$T/results"
owned=0
worker=
watchdog=
cleanup() {
    status=$?
    set +e
    trap - EXIT HUP INT TERM
    if [ -n "$watchdog" ]; then
        kill "$watchdog" 2>/dev/null || true
        wait "$watchdog" 2>/dev/null || true
    fi
    if [ -n "$worker" ]; then kill -TERM "$worker" 2>/dev/null || true; fi
    if [ "$owned" = 1 ]; then
        sh "$T/testbed.sh" down || status=1
        if [ -n "$worker" ]; then wait "$worker" 2>/dev/null || true; fi
    fi
    { echo namespaces; ip netns list; echo controllers; pidof cake-adapt fping || true;
      sha256sum /usr/sbin/cake-adapt; cat /proc/sys/kernel/random/boot_id;
      sh "$T/log-inode.sh"; } > "$T/results/cleanup"
    mkdir -p /root/native-icmp-phase3
    cp -a "$T/results" /root/native-icmp-phase3/final || status=1
    exit "$status"
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
for ns in cpe isp inet; do
    if ip netns list | awk '{print $1}' | grep -qx "$ns"; then echo "namespace $ns exists" >&2; exit 2; fi
done
# Ignore readers (including the user's tail), but refuse an existing writer.
if [ -e /tmp/sqm-mon-test.log ]; then
    log_identity=$(sh "$T/log-inode.sh")
    for info in /proc/[0-9]*/fdinfo/*; do
        mount_id=$(awk '/^mnt_id:/ {print $2}' "$info" 2>/dev/null) || continue
        inode=$(awk '/^ino:/ {print $2}' "$info" 2>/dev/null) || continue
        device=$(awk -v id="$mount_id" '$1 == id {print $3; exit}' "${info%/fdinfo/*}/mountinfo" 2>/dev/null) || continue
        identity=$device:$inode
        if [ "$identity" = "$log_identity" ]; then
            flags=$(awk '/^flags:/ {print $2}' "$info" 2>/dev/null) || continue
            if (( (8#$flags & 3) != 0 )); then echo "existing test-log writer: $info" >&2; exit 2; fi
        fi
    done
fi
{
    uname -a; cat /proc/sys/kernel/random/boot_id; cat /proc/loadavg
    grep Cpus_allowed_list /proc/self/status
    cat /sys/devices/system/clocksource/clocksource0/current_clocksource
    echo 'USER_HZ=100 (Linux i386 procfs ABI, not CONFIG_HZ)'
    pidof cake-adapt || true
    sha256sum "$T/cake-adapt" /usr/sbin/cake-adapt /usr/bin/fping "$T/run.sh" "$T/testbed.sh" "$T/batch.sh" "$T/children.sh" "$T/log-inode.sh"
} > "$T/results/environment"
owned=1
sh "$T/testbed.sh" up
(read -r -t 900 ignored <> <(:) || true; kill -TERM "$$") &
watchdog=$!
methods=${METHODS:-'fping native fping-ts native-ts'}
for method in $methods; do
    case "$method" in fping|native|fping-ts|native-ts) ;; *) exit 2 ;; esac
done
if [ "$MODE" = smoke ]; then
    methods=native
    export DURATION=3 STEP_DURATION=1 IDLE_START=2 IDLE_GAP=1
fi
for method in $methods; do
    sh "$T/run.sh" "$method" "$T/cake-adapt" "$method" 10 30 60 &
    worker=$!
    wait "$worker"
    worker=
    test -z "$(ip netns pids cpe)"
done
