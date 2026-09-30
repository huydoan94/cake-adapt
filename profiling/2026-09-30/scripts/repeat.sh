#!/bin/sh
# Repeat the bidirectional CPU measurement, alternating builds; bounded waits only.
T=/tmp/cake-adapt-test
ENDPOINT=10.0.3.2
download() { wget -q -O /dev/null "http://$ENDPOINT:18080/bytes/20000000000" & D=$!; sleep "$1"; kill "$D" 2>/dev/null; wait "$D" 2>/dev/null; }
upload() { cat /dev/zero | nc "$ENDPOINT" 18081 & U=$!; sleep "$1"; kill "$U" 2>/dev/null; wait "$U" 2>/dev/null; }
for round in 1 2; do
    for build in before profile; do
        : > /tmp/sqm-mon-test.log
        "$T/bin/cake-adapt-$build" -C "$T/uci" -S main </dev/null >/dev/null 2>&1 &
        PID=$!
        sleep 5
        before=$(awk '{print $14 + $15}' /proc/$PID/stat)
        perf record -e cpu-clock -F 999 -g -p "$PID" -o /tmp/cake-adapt-test/repeat.data -- sleep 40 >/dev/null 2>&1 &
        PERF=$!
        download 40 & B=$!; upload 40; wait "$B"; wait "$PERF"
        after=$(awk '{print $14 + $15}' /proc/$PID/stat)
        echo "round=$round build=$build bidirectional user+system_ticks=$((after - before)) seconds=40 records=$(grep -c '^DATA;' /tmp/sqm-mon-test.log)"
        kill -TERM "$PID"; for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 1; done; wait "$PID"
        rm -f /tmp/cake-adapt-test/repeat.data*
    done
done | tee "$T/results/profile/bidirectional-repeats.txt"
echo "leftover fping=[$(pidof fping)]"
