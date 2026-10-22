#!/bin/bash
set -eu
T=/tmp/native-icmp-phase3
. "$T/children.sh"
test ! -e "$T/results"
test ! -e /root/native-icmp-phase3
mkdir "$T/results"
owned=0
worker=
watchdog=
CAP_A= CAP_B=
stop_captures() {
    local stopped=0
    for capture_pid in $CAP_A $CAP_B; do kill -INT "$capture_pid" 2>/dev/null || true; done
    if [ -n "$CAP_A$CAP_B" ]; then sleep 1; fi
    for capture_pid in $CAP_A $CAP_B; do
        if kill -0 "$capture_pid" 2>/dev/null; then kill -KILL "$capture_pid" 2>/dev/null || true; fi
    done
    if [ -n "$CAP_A" ]; then wait "$CAP_A" || stopped=1; CAP_A=; fi
    if [ -n "$CAP_B" ]; then wait "$CAP_B" || stopped=1; CAP_B=; fi
    return "$stopped"
}
cleanup() {
    status=$?
    set +e
    trap - EXIT HUP INT TERM
    if [ -n "$watchdog" ]; then
        kill "$watchdog" 2>/dev/null || true
        wait "$watchdog" 2>/dev/null || true
    fi
    if [ -n "$worker" ]; then kill -TERM "$worker" 2>/dev/null || true; fi
    stop_captures || status=1
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
    sha256sum "$T/cake-adapt" /usr/sbin/cake-adapt /usr/bin/fping "$T/run.sh" "$T/testbed.sh" "$T/batch.sh" "$T/cadence-batch.sh" /usr/bin/tcpdump "$T/children.sh" "$T/log-inode.sh"
} > "$T/results/environment"
cat /proc/sys/kernel/random/boot_id > "$T/results/boot-before"
cat /proc/sys/net/core/bpf_jit_enable > "$T/results/bpf-jit-enable"
tcpdump --version > "$T/results/tcpdump-version" 2>&1
owned=1
sh "$T/testbed.sh" up
(read -r -t 480 ignored <> <(:) || true; kill -TERM "$$") &
watchdog=$!
export SEND_CAPTURE_ONLY=1
for method in fping-ts native-ts; do
    C=$T/results/capture-$method
    mkdir "$C"
    ip netns exec isp tcpdump -n -i iwan -s 96 -U -w "$C/isp-ingress.pcap" 'icmp and icmp[0] = 13 and dst net 10.99.0.0/24' >"$C/isp-stdout" 2>"$C/isp-stderr" &
    CAP_A=$!
    ip netns exec inet tcpdump -n -i inet0 -s 96 -U -w "$C/reflector-ingress.pcap" 'icmp and icmp[0] = 13 and dst net 10.99.0.0/24' >"$C/reflector-stdout" 2>"$C/reflector-stderr" &
    CAP_B=$!
    sleep 1
    kill -0 "$CAP_A"; kill -0 "$CAP_B"
    grep -q 'listening on' "$C/isp-stderr"
    grep -q 'listening on' "$C/reflector-stderr"
    cat "/proc/$CAP_A/stat" > "$C/isp-cpu-before"
    cat "/proc/$CAP_B/stat" > "$C/reflector-cpu-before"
    cut -d' ' -f1 /proc/uptime > "$C/cpu-before.time"
    sh "$T/run.sh" "$method" "$T/cake-adapt" "$method" 10 30 60 &
    worker=$!
    wait "$worker"
    worker=
    sleep 2 # Drain in-flight requests before closing equivalent captures.
    cat "/proc/$CAP_A/stat" > "$C/isp-cpu-after"
    cat "/proc/$CAP_B/stat" > "$C/reflector-cpu-after"
    cut -d' ' -f1 /proc/uptime > "$C/cpu-after.time"
    stop_captures
    cat /proc/sys/kernel/random/boot_id > "$T/results/boot-after"
    cmp "$T/results/boot-before" "$T/results/boot-after"
    test -z "$(ip netns pids cpe)"
    cp -a "$C" /root/native-icmp-phase3/
done
