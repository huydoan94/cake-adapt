#!/bin/sh
# run.sh autorate|adapt: reset rates, run one controller alone under the workload, stop it.
A=/tmp/ab
WHICH=$1
OUT=$A/results/$WHICH
mkdir -p "$A/results"
tc qdisc change root dev eth1 cake bandwidth 20Mbit
tc qdisc change root dev ifb4eth1 cake bandwidth 20Mbit
[ -z "$(pidof cake-adapt fping)" ] || { echo "another controller or pinger is running"; exit 1; }
if [ "$WHICH" = autorate ]; then
    mkdir -p "$A/autorate/logs"; : > "$A/autorate/logs/cake-autorate.primary.log"
    CAKE_AUTORATE_SCRIPT_PREFIX=$A/autorate CAKE_AUTORATE_CONFIG_PREFIX=$A/autorate \
        bash "$A/autorate/cake-autorate.sh" "$A/autorate/config.primary.sh" </dev/null >"$OUT.stdout" 2>&1 &
else
    : > /tmp/sqm-mon-test.log
    "$A/adapt/bin/cake-adapt" -C "$A/adapt/uci" -S main </dev/null >"$OUT.stdout" 2>&1 &
fi
PID=$!
sleep 2
sh "$A/workload.sh" "$OUT"
kill -TERM "$PID"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
kill -0 "$PID" 2>/dev/null && { echo "controller did not stop; killing"; kill -KILL "$PID"; }
wait "$PID" 2>/dev/null
sleep 1
echo "leftover: fping=[$(pidof fping)] bash-autorate=[$(ps w | grep -c '[c]ake-autorate.sh')] adapt=[$(pidof cake-adapt)] runpath=[$(ls /var/run/cake-autorate 2>/dev/null)]"
if [ "$WHICH" = autorate ]; then cp "$A/autorate/logs/cake-autorate.primary.log" "$OUT.log"; else cp /tmp/sqm-mon-test.log "$OUT.log"; fi
tc qdisc show dev eth1 root; tc qdisc show dev ifb4eth1 root
