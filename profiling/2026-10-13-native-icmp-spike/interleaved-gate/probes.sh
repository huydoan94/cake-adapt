#!/bin/bash
# Single-packet probes, serialized across both backends. No two live requests.
set -eu
T=${T:-/tmp/native-icmp-gate3}
R=${R:-$T/results}
COUNT=${COUNT:-300}
PERSIST=${PERSIST:-$T/persist}
if [ -e "$R/timeline.csv" ]; then
    echo 'probe output exists; refusing to append to another run' >&2
    exit 2
fi
mkdir -p "$R" "$PERSIST"
active=
cleanup() {
    if [ -n "$active" ]; then
        kill -TERM "$active" 2>/dev/null || true
        wait "$active" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
now_cs() {
    local uptime idle whole fraction
    read -r uptime idle < /proc/uptime
    whole=${uptime%.*}; fraction=${uptime#*.}
    current_cs=$((10#$whole * 100 + 10#$fraction))
}
probe() {
    local batch=$1 index=$2 backend=$3 interval_cs=$4 start_cs end_cs status delay_cs
    now_cs; start_cs=$current_cs
    if [ "$backend" = native ]; then
        ip netns exec cpe "$T/icmp-driver" cwan native 10.99.0.11 1 100000 >> "$R/$batch.native" 2>> "$R/$batch.native.status" &
    else
        ip netns exec cpe fping -I cwan --timestamp --count 1 --period 100 --timeout 1000 10.99.0.11 >> "$R/$batch.fping" 2>> "$R/$batch.fping.status" &
    fi
    active=$!
    status=0
    wait "$active" || status=$?
    active=
    now_cs; end_cs=$current_cs
    printf '%s,%s,%s,%s,%s,%s\n' "$batch" "$index" "$backend" "$start_cs" "$end_cs" "$status" >> "$R/timeline.csv"
    [ "$status" -eq 0 ] || { echo "$batch: $backend probe $index failed ($status)" >&2; return 1; }
    delay_cs=$((start_cs + interval_cs - end_cs))
    if [ "$delay_cs" -gt 0 ]; then
        # The OpenWrt BusyBox sleep has no fractional-duration support.
        # A read/write pipe stays open until Bash's timed read expires.
        read -r -t "$(printf '%d.%02d' "$((delay_cs / 100))" "$((delay_cs % 100))")" unused <> <(:) || true
    fi
}
printf 'batch,index,backend,start_cs,end_cs,status\n' > "$R/timeline.csv"
for batch in fping-before interleaved fping-after; do
    for ((index=0; index<COUNT; index++)); do
        if [ "$batch" = interleaved ]; then
            probe "$batch" "$index" native 10
            probe "$batch" "$index" fping 10
        else
            probe "$batch" "$index" fping 20
        fi
    done
    cp -a "$R" "$PERSIST/$batch"
    echo "completed $batch"
done
