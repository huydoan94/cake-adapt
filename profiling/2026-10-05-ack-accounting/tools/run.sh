#!/bin/sh
set -eu
DIR=$1
MODE=$2
NS="cake-ack-$$"
LOG=/tmp/sqm-mon-test.log
CREATED=0
LINK_CREATED=0
snapshot() {
    uname -a > "$DIR/$MODE.$1.kernel"
    ps w > "$DIR/$MODE.$1.process"
    pidof cake-adapt > "$DIR/$MODE.$1.daemon-pids" || true
    ubus call service list '{"name":"cake-adapt"}' > "$DIR/$MODE.$1.service"
    tc -d qdisc show > "$DIR/$MODE.$1.qdiscs"
    tc -d -s qdisc show > "$DIR/$MODE.$1.qdisc-counters"
    ip -d link show > "$DIR/$MODE.$1.links"
    # Elapsed bridge GC time is volatile, not restored interface configuration.
    sed 's/gc_timer *[0-9.]* /gc_timer <elapsed> /g' "$DIR/$MODE.$1.links" > "$DIR/$MODE.$1.links-stable"
    ip route show table all > "$DIR/$MODE.$1.routes"
    sha256sum /etc/config/cake-adapt /etc/init.d/cake-adapt /usr/sbin/cake-adapt > "$DIR/$MODE.$1.files"
    if [ -e /lib/bpf/cake-adapt-tcpdelay.o ]; then sha256sum /lib/bpf/cake-adapt-tcpdelay.o >> "$DIR/$MODE.$1.files"; fi
    if command -v apk >/dev/null; then apk info -v > "$DIR/$MODE.$1.packages"; else opkg list-installed > "$DIR/$MODE.$1.packages"; fi
    ip netns list > "$DIR/$MODE.$1.namespaces"
}
cleanup() {
    status=$?
    trap - EXIT INT TERM HUP
    if [ "$CREATED" = 1 ]; then
        owned_pids=$(ip netns pids "$NS")
        if [ -n "$owned_pids" ]; then
            kill -TERM $owned_pids 2>/dev/null || true
            for attempt in 1 2 3; do
                [ -n "$(ip netns pids "$NS")" ] || break
                sleep 1
            done
            owned_pids=$(ip netns pids "$NS")
            if [ -n "$owned_pids" ]; then
                kill -KILL $owned_pids 2>/dev/null || true
                sleep 1
            fi
            [ -z "$(ip netns pids "$NS")" ] || status=1
        fi
        ip netns del "$NS" || status=1
    fi
    for proc_dir in /proc/[0-9]*; do
        [ "$(readlink "$proc_dir/exe" 2>/dev/null || true)" != "$DIR/probe" ] || status=1
    done
    [ "$LINK_CREATED" = 0 ] || rm -f "$DIR/logs/cake-adapt.log"
    ls -i "$LOG" | awk '{print $1}' > "$DIR/$MODE.inode-after"
    cmp "$DIR/$MODE.inode-before" "$DIR/$MODE.inode-after" || status=1
    snapshot after
    for part in daemon-pids service qdiscs links-stable routes files packages namespaces; do
        cmp "$DIR/$MODE.before.$part" "$DIR/$MODE.after.$part" || status=1
    done
    printf '%s\n' "$status" > "$DIR/$MODE.exit-status"
    exit "$status"
}
case "$MODE" in preflight|jit-retry|batch|batch-retry) ;; *) exit 2 ;; esac
[ ! -e "$DIR/logs/cake-adapt.log" ]
snapshot before
touch "$LOG"
ls -i "$LOG" | awk '{print $1}' > "$DIR/$MODE.inode-before"
for fd in /proc/[0-9]*/fd/*; do
    [ "$(ls -iL "$fd" 2>/dev/null | awk '{print $1}')" = "$(cat "$DIR/$MODE.inode-before")" ] || continue
    flags=$(awk '$1 == "flags:" { print $2 }' "${fd%/fd/*}/fdinfo/${fd##*/}" 2>/dev/null || true)
    [ -n "$flags" ] || continue
    [ "$((flags & 3))" = 0 ] || { echo "Existing test-log writer: $fd"; exit 1; }
done
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
: > "$LOG"
mkdir -p "$DIR/logs" "$DIR/results/$MODE"
ln "$LOG" "$DIR/logs/cake-adapt.log"
LINK_CREATED=1
sha256sum "$DIR/probe" "$DIR/before.o" "$DIR/after.o" > "$DIR/$MODE.artifacts.sha256"
ip netns add "$NS"
CREATED=1
ip netns exec "$NS" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
ip netns exec "$NS" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
ip -n "$NS" link add ca0 type veth peer name ca1
ip -n "$NS" link set ca0 address 02:00:00:00:00:01
ip -n "$NS" link set ca1 address 02:00:00:00:00:02
ip -n "$NS" link set ca0 mtu 1600
ip -n "$NS" link set ca1 mtu 1600
ip -n "$NS" link set ca0 up
ip -n "$NS" link set ca1 up
variants="before after"
if [ "$MODE" = jit-retry ]; then
    cases=raw
    variants=after
elif [ "$MODE" = preflight ]; then
    cases=raw
    ip netns exec "$NS" "$DIR/probe" "$DIR/before.o" after stale > "$DIR/results/$MODE/stale.txt" 2>&1
else
    cases='ether overhead mpu negative atm ptm vlan qinq unknown raw-unknown gso raw-gso disabled'
fi
for case_name in $cases; do
    case "$case_name" in
        raw|raw-unknown|disabled) options='raw overhead 0 mpu 0'; expected='66 / 1514' ;;
        overhead) options='ethernet overhead 44 mpu 0'; expected='96 / 1544' ;;
        mpu) options='ethernet overhead 0 mpu 84'; expected='84 / 1500' ;;
        negative) options='ethernet overhead -20 mpu 0'; expected='32 / 1480' ;;
        atm) options='atm overhead 0 mpu 0'; expected='106 / 1696' ;;
        ptm) options='ptm overhead 0 mpu 0'; expected='53 / 1524' ;;
        raw-gso) options='raw overhead 0 mpu 0 no-split-gso'; expected= ;;
        gso) options='ethernet overhead 0 mpu 0 no-split-gso'; expected= ;;
        vlan|qinq|unknown) options='ethernet overhead 0 mpu 0'; expected= ;;
        ether) options='ethernet overhead 0 mpu 0'; expected='52 / 1500' ;;
    esac
    for variant in $variants; do
        ip netns exec "$NS" tc qdisc del dev ca0 root 2>/dev/null || true
        ip netns exec "$NS" tc qdisc add dev ca0 root handle 1: cake unlimited besteffort ack-filter $options
        # Replacement resets the independent min/max kernel reference each time.
        ip netns exec "$NS" "$DIR/probe" "$DIR/$variant.o" "$variant" "$case_name" > "$DIR/results/$MODE/$case_name.$variant.txt" 2>&1
        ip netns exec "$NS" tc -d -s qdisc show dev ca0 > "$DIR/results/$MODE/$case_name.$variant.qdisc"
        if [ -n "$expected" ]; then
            observed=$(sed -n 's/.*min\/max overhead-adjusted size: *\([0-9]*\) *\/ *\([0-9]*\).*/\1 \/ \2/p' "$DIR/results/$MODE/$case_name.$variant.qdisc")
            [ "$observed" = "$expected" ] || { printf 'kernel expected %s observed %s\n' "$expected" "$observed"; exit 1; }
        fi
    done
    printf 'PASS %s\n' "$case_name" >> "$LOG"
done
printf 'PASS %s\n' "$MODE"
