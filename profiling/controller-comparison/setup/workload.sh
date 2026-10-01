#!/bin/sh
# Identical two-minute workload for both controllers; every transfer is bounded.
ENDPOINT=10.0.3.2
note() { echo "$(date +%s.%N | cut -c1-17) $*" >> "$1.phases"; }
download() {
    wget -q -O /dev/null "http://$ENDPOINT:18080/bytes/20000000000" & D=$!
    sleep "$1"; kill "$D" 2>/dev/null; wait "$D" 2>/dev/null
}
upload() {
    cat /dev/zero | nc "$ENDPOINT" 18081 & U=$!
    sleep "$1"; kill "$U" 2>/dev/null; wait "$U" 2>/dev/null
}
OUT=$1
: > "$OUT.phases"
note "$OUT" idle;     sleep 10
note "$OUT" download; download 40
note "$OUT" idle;     sleep 20
note "$OUT" upload;   upload 30
note "$OUT" idle;     sleep 20
note "$OUT" end
