#!/bin/sh
# run.sh CONTROLLER VARIANT ROUND
#   CONTROLLER: autorate | adapt | none (an empty cgroup for the system baseline)
#   VARIANT: defaults (upstream default logging) | stats (all record outputs)
# Runs one controller alone in its own cgroup v2 group, as upstream's
# bench_cpu.sh does, under a fixed workload. Every wait is bounded.
P=/tmp/perfcmp
N=$1
V=$2
R=$3
OUT=$P/results/$V-$N-r$R
CG=/sys/fs/cgroup/perfcmp-$N
ENDPOINT=10.0.3.2
LOG=/tmp/sqm-mon-test.log
mkdir -p "$OUT"

tc qdisc change root dev eth1 cake bandwidth 20Mbit
tc qdisc change root dev ifb4eth1 cake bandwidth 20Mbit
if [ -n "$(pidof fping cake-adapt)" ] || [ -d "$CG" ]; then
    echo "another controller, pinger or cgroup exists; not starting" >&2
    exit 1
fi
mkdir "$CG"

# phase uptime cg_usage cg_user cg_system busy_jiffies idle_jiffies ctxt forks dl_bytes ul_bytes dl_rate ul_rate
snap() {
    set -- "$1" "$(cut -d' ' -f1 /proc/uptime)" \
        "$(awk '{v[$1]=$2} END {print v["usage_usec"], v["user_usec"], v["system_usec"]}' "$CG/cpu.stat")" \
        "$(awk '$1=="cpu" {printf "%d %d ", $2+$3+$4+$7+$8+$9, $5+$6} $1=="ctxt" {printf "%d ", $2} $1=="processes" {printf "%d", $2}' /proc/stat)" \
        "$(tc -s qdisc show dev ifb4eth1 root | awk '/Sent/ {print $2}')" \
        "$(tc -s qdisc show dev eth1 root | awk '/Sent/ {print $2}')" \
        "$(tc qdisc show dev ifb4eth1 root | sed 's/.*bandwidth \([^ ]*\).*/\1/')" \
        "$(tc qdisc show dev eth1 root | sed 's/.*bandwidth \([^ ]*\).*/\1/')"
    echo "$*" >> "$OUT/snaps"
}

# uptime processes rss_kb private_kb, every 5 s, for the processes in the
# cgroup. This kernel has no smaps, so private = resident - file-backed shared
# pages from statm; shared library and bash text is not counted per process.
sample_memory() {
    while [ -d "$CG" ]; do
        t=$(cut -d' ' -f1 /proc/uptime)
        n=0 rss=0 private=0
        for pid in $(cat "$CG/cgroup.procs" 2>/dev/null); do
            set -- $(cat "/proc/$pid/statm" 2>/dev/null)
            [ $# -ge 3 ] || continue
            n=$((n + 1))
            rss=$((rss + $2 * 4))
            private=$((private + ($2 - $3) * 4))
        done
        echo "$t $n $rss $private" >> "$OUT/memory"
        sleep 5
    done
}

download() {
    wget -q -O /dev/null "http://$ENDPOINT:18080/bytes/20000000000" & D=$!
    sleep "$1"; kill "$D" 2>/dev/null; wait "$D" 2>/dev/null
}
upload() {
    cat /dev/zero | nc "$ENDPOINT" 18081 & U=$!
    sleep "$1"; kill "$U" 2>/dev/null; wait "$U" 2>/dev/null
}

case "$N" in
autorate)
    mkdir -p "$P/autorate/logs"
    rm -f "$P/autorate/logs/"*
    cp "$P/autorate/config.$V.sh" "$P/autorate/config.primary.sh"
    snap start
    sh -c "echo \$\$ > $CG/cgroup.procs; CAKE_AUTORATE_SCRIPT_PREFIX=$P/autorate CAKE_AUTORATE_CONFIG_PREFIX=$P/autorate exec bash $P/autorate/cake-autorate.sh $P/autorate/config.primary.sh" </dev/null >"$OUT/stdout" 2>&1 &
    ;;
adapt)
    rm -f "$P/adapt-logs/cake-adapt.log.old" "$P/adapt-logs/"*.gz
    : > "$LOG"
    snap start
    sh -c "echo \$\$ > $CG/cgroup.procs; exec $P/bin/cake-adapt -C $P/uci-$V -S main" </dev/null >"$OUT/stdout" 2>&1 &
    ;;
none)
    snap start
    sh -c "echo \$\$ > $CG/cgroup.procs; exec sleep 100000" </dev/null >/dev/null 2>&1 &
    ;;
esac
PID=$!
sample_memory &
SAMPLER=$!

sleep 15;            snap startup
sleep 40;            snap idle
download 45;         snap download
upload 45;           snap upload
download 45 & B=$!
upload 45; wait "$B"; snap bidirectional
sleep 70;            snap settle
sleep 30;            snap sleep

kill -TERM "$PID"
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -z "$(cat "$CG/cgroup.procs")" ] && break
    sleep 1
done
if [ -n "$(cat "$CG/cgroup.procs")" ]; then
    echo "cgroup not empty 10 s after SIGTERM: $(cat "$CG/cgroup.procs"); killing" >&2
    echo 1 > "$CG/cgroup.kill"
    sleep 1
fi
wait "$PID" 2>/dev/null
echo "exit $?" > "$OUT/exit"
cat "$CG/cpu.stat" > "$OUT/cpu.stat"
rmdir "$CG"
wait "$SAMPLER" 2>/dev/null

case "$N" in
autorate) cp "$P/autorate/logs/"* "$OUT/" 2>/dev/null ;;
adapt) cp "$LOG" "$OUT/cake-adapt.log"; cp "$P/adapt-logs/cake-adapt.log.old" "$OUT/" 2>/dev/null ;;
esac
echo "leftover fping=[$(pidof fping)] adapt=[$(pidof cake-adapt)] autorate=[$(ps w | grep -c '[c]ake-autorate.sh')] runpath=[$(ls /var/run/cake-autorate 2>/dev/null)]" > "$OUT/leftover"
