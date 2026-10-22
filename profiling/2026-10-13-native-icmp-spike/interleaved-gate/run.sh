#!/bin/bash
set -eu
T=/tmp/native-icmp-gate3
RUN=${RUN:-acceptance}
case "$RUN" in smoke | smoke-fixed | acceptance) ;; *) exit 2 ;; esac
R=$T/$RUN/results
PERSIST=/root/native-icmp-gate3/$RUN
if [ -e "$R" ] || [ -e "$PERSIST" ]; then
    echo 'run destination exists; refusing to mix evidence' >&2
    exit 2
fi
mkdir -p "$R" "$PERSIST"
owned=0
worker=
watchdog=
cleanup() {
    if [ -n "$worker" ]; then
        kill -TERM "$worker" 2>/dev/null || true
        wait "$worker" 2>/dev/null || true
    fi
    if [ -n "$watchdog" ]; then
        # Stop both the timeout sleeper and its shell, not an unrelated sleep.
        for child in $(cat "/proc/$watchdog/task/$watchdog/children" 2>/dev/null); do
            kill "$child" 2>/dev/null || true
        done
        kill "$watchdog" 2>/dev/null || true
        wait "$watchdog" 2>/dev/null || true
    fi
    if [ "$owned" -eq 1 ]; then sh "$T/testbed.sh" down; fi
    { echo namespaces; ip netns list; echo controllers; pidof cake-adapt fping || true;
      sha256sum /usr/sbin/cake-adapt; cat /proc/sys/kernel/random/boot_id; } > "$R/cleanup"
    cp -a "$R" "$PERSIST/final"
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
for ns in cpe isp inet; do
    if ip netns list | awk '{print $1}' | grep -qx "$ns"; then
        echo "namespace $ns already exists; refusing" >&2
        exit 2
    fi
done
{
    uname -a
    cat /proc/sys/kernel/random/boot_id
    cat /proc/loadavg
    grep Cpus_allowed_list /proc/self/status
    cat /sys/devices/system/clocksource/clocksource0/current_clocksource
    sha256sum "$T/icmp-driver" /usr/bin/fping /usr/sbin/cake-adapt "$T/probes.sh" "$T/testbed.sh"
    echo "${COUNT:-300} per cohort; fping-before, interleaved(native/fping), fping-after"
    echo 'backend interval 200ms; interleaved turns100ms; /proc/uptime scheduling resolution10ms'
    echo 'gate: abs(interleaved native median - interleaved fping median) <= abs(fping-before median - fping-after median)'
} > "$R/environment"
owned=1
sh "$T/testbed.sh" up
ip netns exec cpe ip route get 10.99.0.11 >> "$R/environment"
ip netns exec cpe tc -s qdisc show dev cwan > "$R/qdisc-upload"
ip netns exec cpe tc -s qdisc show dev ifb4cwan > "$R/qdisc-download"
(sleep 360; kill -TERM "$$") &
watchdog=$!
R="$R" PERSIST="$PERSIST" bash "$T/probes.sh" &
worker=$!
status=0
wait "$worker" || status=$?
worker=
exit "$status"
