#!/bin/sh
# breakdown.sh CONTROLLER: per-process CPU inside the controller's cgroup,
# separating fping from the controller. Each process's ticks include the
# children it has reaped (cutime + cstime), so short-lived tc/sleep/subshell
# work is charged to the process that started it. Every wait is bounded.
P=/tmp/perfcmp
N=$1
OUT=$P/results/breakdown-$N
CG=/sys/fs/cgroup/perfcmp-$N
ENDPOINT=10.0.3.2
mkdir -p "$OUT"
tc qdisc change root dev eth1 cake bandwidth 20Mbit
tc qdisc change root dev ifb4eth1 cake bandwidth 20Mbit
if [ -n "$(pidof fping cake-adapt)" ] || [ -d "$CG" ]; then
    echo "another controller, pinger or cgroup exists; not starting" >&2
    exit 1
fi
mkdir "$CG"

# phase pid ppid comm ticks
snap() {
    for pid in $(cat "$CG/cgroup.procs"); do
        awk -v phase="$1" '{
            sub(/^[0-9]+ \(/, ""); comm = $0; sub(/\).*/, "", comm)
            sub(/^[^)]*\) /, "")
            # After the comm, field 1 is state; utime..cstime are fields 12..15.
            print phase, '"$pid"', $2, comm, $12 + $13 + $14 + $15
        }' "/proc/$pid/stat" 2>/dev/null
    done >> "$OUT/procs"
    awk -v phase="$1" '$1 == "usage_usec" {print phase, $2}' "$CG/cpu.stat" >> "$OUT/cgroup"
}

case "$N" in
autorate)
    rm -f "$P/autorate/logs/"*
    cp "$P/autorate/config.defaults.sh" "$P/autorate/config.primary.sh"
    sh -c "echo \$\$ > $CG/cgroup.procs; CAKE_AUTORATE_SCRIPT_PREFIX=$P/autorate CAKE_AUTORATE_CONFIG_PREFIX=$P/autorate exec bash $P/autorate/cake-autorate.sh $P/autorate/config.primary.sh" </dev/null >/dev/null 2>&1 &
    ;;
adapt)
    : > /tmp/sqm-mon-test.log
    sh -c "echo \$\$ > $CG/cgroup.procs; exec $P/bin/cake-adapt -C $P/uci-defaults -S main" </dev/null >/dev/null 2>&1 &
    ;;
esac
PID=$!
sleep 15
snap start
sleep 40
snap idle
wget -q -O /dev/null "http://$ENDPOINT:18080/bytes/20000000000" & D=$!
cat /dev/zero | nc "$ENDPOINT" 18081 & U=$!
sleep 45
kill "$D" "$U" 2>/dev/null; wait "$D" "$U" 2>/dev/null
snap bidirectional

kill -TERM "$PID"
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -z "$(cat "$CG/cgroup.procs")" ] && break
    sleep 1
done
[ -n "$(cat "$CG/cgroup.procs")" ] && { echo "cgroup not empty; killing" >&2; echo 1 > "$CG/cgroup.kill"; sleep 1; }
wait "$PID" 2>/dev/null
rmdir "$CG"
echo "leftover fping=[$(pidof fping)] adapt=[$(pidof cake-adapt)] autorate=[$(ps w | grep -c '[c]ake-autorate.sh')]" > "$OUT/leftover"
