#!/bin/sh
# Runtime congestion check: scripted latency episodes under the A/B workload.
T=/tmp/cake-adapt-test
R=$T/results/congestion
LOG=/tmp/sqm-mon-test.log
mkdir -p "$R"
uci -c "$T/uci" set cake-adapt.main.ping_prefix_string="bash $T/scripted-fping.sh"
uci -c "$T/uci" commit cake-adapt
: > "$LOG"
"$T/bin/cake-adapt-final" -C "$T/uci" -S main </dev/null >/dev/null 2>&1 &
PID=$!
sleep 2
sh "$T/workload.sh" "$R/workload"
kill -TERM "$PID"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
kill -0 "$PID" 2>/dev/null && { echo "daemon did not stop; killing"; kill -KILL "$PID"; }
wait "$PID"; echo "daemon exit status $?"
echo "leftover fping-or-script=[$(pidof fping) $(ps w | grep -c '[s]cripted-fping')] daemon=[$(pidof cake-adapt-final)]"
uci -c "$T/uci" delete cake-adapt.main.ping_prefix_string
uci -c "$T/uci" commit cake-adapt
cp "$LOG" "$R/test.log"
