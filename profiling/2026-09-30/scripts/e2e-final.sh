#!/bin/sh
# Final end-to-end run of the isolated production build; every wait is bounded.
T=/tmp/cake-adapt-test
R=$T/results/final
BIN=$T/bin/cake-adapt-final
LOG=/tmp/sqm-mon-test.log
ENDPOINT=10.0.3.2
mkdir -p "$R"
note() { echo "$(date +%s) $(date +%T) PHASE $*" | tee -a "$R/phases.txt"; }
rate() { tc qdisc show dev "$1" root 2>/dev/null | sed -n 's/.*cake \([0-9a-f]*\): .* bandwidth \([^ ]*\) .*/\1:\2/p'; }
zombies() { ps | awk '$4 ~ /^Z/ {n++} END {print n + 0}'; }
download() {
    wget -q -O /dev/null "http://$ENDPOINT:18080/bytes/20000000000" & D=$!
    sleep "$1"; kill "$D" 2>/dev/null; wait "$D" 2>/dev/null
}
upload() {
    cat /dev/zero | nc "$ENDPOINT" 18081 & U=$!
    sleep "$1"; kill "$U" 2>/dev/null; wait "$U" 2>/dev/null
}

: > "$LOG"
note "start inode=$(ls -i $LOG | awk '{print $1}')"
"$BIN" -C "$T/uci" -S main </dev/null >/dev/null 2>&1 &
PID=$!
(
    while kill -0 "$PID" 2>/dev/null; do
        echo "$(date +%s) ul=$(rate eth1) dl=$(rate ifb4eth1) rss=$(awk '/VmRSS/{print $2}' /proc/$PID/status 2>/dev/null) fping=$(pidof fping) zombies=$(zombies)"
        sleep 2
    done
) > "$R/samples.txt" &
SAMPLER=$!

note idle-probing-15s;      sleep 15
note download-40s;          download 40
note recover-20s;           sleep 20
note upload-30s;            upload 30
note recover-15s;           sleep 15
note bidirectional-30s;     download 30 & DL=$!; upload 30; wait "$DL"
note recover-15s;           sleep 15
note "log-export-sigusr1";  kill -USR1 "$PID"; sleep 2; ls "$T/logs" | tee -a "$R/phases.txt"
note "log-reset-sigusr2 size-before=$(wc -c < $LOG)"; kill -USR2 "$PID"; sleep 1
note "log-reset size-after=$(wc -c < $LOG) inode=$(ls -i $LOG | awk '{print $1}')"
note eth1-qdisc-delete;     tc qdisc del dev eth1 root; sleep 5
note eth1-qdisc-restore;    tc qdisc add dev eth1 root handle 800d: cake bandwidth 20Mbit diffserv3 triple-isolate nat nowash no-ack-filter split-gso rtt 100ms raw overhead 0; sleep 6
note ifb-qdisc-delete;      tc qdisc del dev ifb4eth1 root; sleep 5
note ifb-qdisc-restore;     tc qdisc add dev ifb4eth1 root handle 800e: cake bandwidth 20Mbit besteffort triple-isolate nat wash no-ack-filter split-gso rtt 100ms raw overhead 0; sleep 6
note "idle-75s fping=$(pidof fping)"; sleep 75
note "wake-download-12s fping=[$(pidof fping)]"; download 12
note "after-wake fping=$(pidof fping)"
note shutdown
kill -TERM "$PID"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
if kill -0 "$PID" 2>/dev/null; then note "daemon did not stop; killing"; kill -KILL "$PID"; fi
wait "$PID"; note "daemon exit status $?"
wait "$SAMPLER"
note "leftover fping=[$(pidof fping)] daemon=[$(pidof cake-adapt-final)] zombies=$(zombies) inode=$(ls -i $LOG | awk '{print $1}')"
cp "$LOG" "$R/test.log"
