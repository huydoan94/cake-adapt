#!/bin/sh
# Profiles the debug-symbol build under five 40 s workloads; every wait is bounded.
T=/tmp/cake-adapt-test
R=$T/results/profile-before
BIN=$T/bin/cake-adapt-before
ENDPOINT=10.0.3.2
SECONDS_PER_WORKLOAD=40
mkdir -p "$R"
: > "$R/cpu.txt"
download() {
    wget -q -O /dev/null "http://$ENDPOINT:18080/bytes/20000000000" & D=$!
    sleep "$1"; kill "$D" 2>/dev/null; wait "$D" 2>/dev/null
}
upload() {
    cat /dev/zero | nc "$ENDPOINT" 18081 & U=$!
    sleep "$1"; kill "$U" 2>/dev/null; wait "$U" 2>/dev/null
}
bidirectional() { download "$1" & B=$!; upload "$1"; wait "$B"; }
idle() { sleep "$1"; }
ticks() { awk '{print $14 + $15}' "/proc/$PID/stat"; }
start_daemon() {
    : > /tmp/sqm-mon-test.log
    "$BIN" -C "$T/uci" -S main </dev/null >/dev/null 2>&1 &
    PID=$!
    sleep 5
}
stop_daemon() {
    kill -TERM "$PID"
    for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
    kill -0 "$PID" 2>/dev/null && kill -KILL "$PID"
    wait "$PID"
}
measure() {
    name=$1
    before=$(ticks)
    perf record -e cpu-clock -F 999 -g -p "$PID" -o "$R/$name.perf.data" -- sleep "$SECONDS_PER_WORKLOAD" > "$R/$name.perf.log" 2>&1 &
    PERF=$!
    "$name" "$SECONDS_PER_WORKLOAD"
    wait "$PERF"
    after=$(ticks)
    echo "$name user+system_ticks=$((after - before)) seconds=$SECONDS_PER_WORKLOAD rss_kb=$(awk '/VmRSS/{print $2}' /proc/$PID/status)" | tee -a "$R/cpu.txt"
    perf script -F comm,pid,tid,time,event,ip,sym,dso -i "$R/$name.perf.data" > "$R/$name.perf-script.txt" 2>/dev/null
}

start_daemon
for workload in idle download upload bidirectional; do measure "$workload"; done
stop_daemon

# Latency spikes: scripted RTT episodes (see scripted-fping.sh) under download load.
uci -c "$T/uci" set cake-adapt.main.ping_prefix_string="bash $T/scripted-fping.sh"
uci -c "$T/uci" commit cake-adapt
start_daemon
latency() { sleep 12; download $(( $1 - 12 )); }
measure latency
stop_daemon
uci -c "$T/uci" delete cake-adapt.main.ping_prefix_string
uci -c "$T/uci" commit cake-adapt

echo "leftover fping=[$(pidof fping)] daemon=[$(pidof cake-adapt-before)] scripted=$(ps w | grep -c '[s]cripted-fping')"
perf --version > "$R/tools.txt"; uname -a >> "$R/tools.txt"; busybox 2>&1 | head -1 >> "$R/tools.txt"
ls -la "$R"
