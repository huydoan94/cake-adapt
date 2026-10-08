#!/bin/sh
# remote.sh NAME BINARY
# Congestion beyond the ISP that only some flows cross, as on an overseas
# path: the reflectors and the probe never see it, the TCP estimate of a flow
# that crosses it does. The daemon controls both shapers; the question is
# whether that remote queue makes it blame or cut the wrong direction.
#
# A remote bottleneck in inet (300 ms buffers) carries only ports 5203
# (download, REMOTE_DL Mbit/s) and 5204 (upload, REMOTE_UL Mbit/s); the local
# servers 5201 (upload) and 5202 (download) bypass it. Phases, each followed
# by idle:
#   remote-upload        upload to 5204
#   remote-download      download from 5203
#   local-dl-remote-ul   download from 5202 with upload to 5204
#   local-ul-remote-dl   upload to 5201 with download from 5203
#   local-bidirectional  5202 and 5201, the control
# TCP_ATTRIBUTION=0|1 sets tcp_delay_attribution. The 100 ms samples of every
# bottleneck's backlog (ISP up, ISP down, remote up, remote down) are in
# backlog; remote.py scores a set of runs.
T=/tmp/cake-adapt-test
NAME=$1 BIN=$2
: "${UL_CAP:=8}" "${DL_CAP:=40}" "${REMOTE_UL:=4}" "${REMOTE_DL:=20}" "${PHASE_S:=40}"
: "${TCP_ATTRIBUTION:=1}"
R=$T/results/$NAME
LOG=/tmp/sqm-mon-test.log
X() { ip netns exec "$@"; }
mark() { bash -c 'echo "$EPOCHREALTIME '"$1"'"' >> "$R/phases-epoch"; }
mkdir -p "$R" "$T/uci-$NAME" "$T/logs-$NAME"
if [ -n "$(ip netns pids cpe)" ]; then
    echo "$NAME: processes still running in cpe ($(ip netns pids cpe | tr '\n' ' ')); not starting"
    exit 1
fi

# The remote bottleneck, rebuilt each run so it starts empty: clsact on inet0
# sends port 5204's arriving packets to rem0 and port 5203's leaving packets
# to rem1, each an IFB with a tbf; every other packet passes unshaped.
X inet tc qdisc del dev inet0 clsact 2>/dev/null
for ifb in rem0 rem1; do X inet ip link del "$ifb" 2>/dev/null; done
X inet ip link add rem0 type ifb
X inet ip link add rem1 type ifb
X inet ip link set rem0 up
X inet ip link set rem1 up
X inet tc qdisc add dev rem0 root tbf rate "$REMOTE_UL"mbit burst 16k \
    limit $(( REMOTE_UL * 1000000 / 8 * 3 / 10 ))
X inet tc qdisc add dev rem1 root tbf rate "$REMOTE_DL"mbit burst 16k \
    limit $(( REMOTE_DL * 1000000 / 8 * 3 / 10 ))
X inet tc qdisc add dev inet0 clsact
X inet tc filter add dev inet0 ingress protocol ip u32 match ip dport 5204 0xffff \
    action mirred egress redirect dev rem0
X inet tc filter add dev inet0 egress protocol ip u32 match ip sport 5203 0xffff \
    action mirred egress redirect dev rem1
# A second server on a port already served exits at once.
for port in 5203 5204; do X inet iperf3 -s -p "$port" -D 2>/dev/null; done

X cpe tc qdisc change dev cwan root cake bandwidth 6000kbit
X cpe tc qdisc change dev ifb4cwan root cake bandwidth 30000kbit
sh "$T/testbed.sh" rate "$UL_CAP" "$DL_CAP"
echo "$UL_CAP $DL_CAP $REMOTE_UL $REMOTE_DL" > "$R/capacity"

cat > "$T/uci-$NAME/cake-adapt" <<EOF
config cake_adapt 'main'
	option enabled '1'
	option interface 'cwan'
	option adjust_dl_shaper_rate '1'
	option min_dl_shaper_rate_kbps '10000'
	option base_dl_shaper_rate_kbps '30000'
	option max_dl_shaper_rate_kbps '60000'
	option adjust_ul_shaper_rate '1'
	option min_ul_shaper_rate_kbps '2000'
	option base_ul_shaper_rate_kbps '6000'
	option max_ul_shaper_rate_kbps '12000'
	option tcp_delay_attribution '$TCP_ATTRIBUTION'
	option randomize_reflectors '0'
	option output_processing_stats '1'
	option output_load_stats '1'
	option output_cake_changes '1'
	option log_file_max_size_KB '50000'
	option log_file_path_override '$T/logs-$NAME'
	list reflectors '10.99.0.11'
	list reflectors '10.99.0.12'
	list reflectors '10.99.0.13'
	list reflectors '10.99.0.14'
	list reflectors '10.99.0.15'
	list reflectors '10.99.0.16'
EOF

: > "$LOG"
ln -f "$LOG" "$T/logs-$NAME/cake-adapt.log"
"$BIN" -C "$T/uci-$NAME" -S main -V || { echo "$NAME: invalid configuration"; exit 1; }
ip netns exec cpe "$BIN" -C "$T/uci-$NAME" -S main </dev/null >/dev/null 2>&1 &
DAEMON=$!
ip netns exec cpe fping --timestamp --loop --period 100 --timeout 3000 10.99.0.20 > "$R/probe" 2>/dev/null &
PROBE=$!
# Rate and backlog bytes of a tbf: ISP upload and download, remote upload and download.
TBF='/qdisc tbf/ { for (i = 1; i < NF; i++) if ($i == "rate") rate = $(i + 1); tbf = 1 }
    tbf && /backlog/ { sub(/b$/, "", $2); print rate, $2; exit }'
TBF="$TBF" bash -c '
    while true; do
        isp_up=$(ip netns exec isp tc -s qdisc show dev iinet | awk "$TBF")
        isp_down=$(ip netns exec isp tc -s qdisc show dev iwan | awk "$TBF")
        remote_up=$(ip netns exec inet tc -s qdisc show dev rem0 | awk "$TBF")
        remote_down=$(ip netns exec inet tc -s qdisc show dev rem1 | awk "$TBF")
        echo "$EPOCHREALTIME $isp_up $isp_down $remote_up $remote_down"
        read -t 0.1 <> <(:)
    done' > "$R/backlog" &
SAMPLER=$!
pair() { # NAME DOWNLOAD_PORT UPLOAD_PORT
    ip netns exec cpe iperf3 -c 10.99.0.2 -p "$2" -R -t "$PHASE_S" -J > "$R/$1-download.json" &
    DOWNLOAD=$!
    X cpe iperf3 -c 10.99.0.2 -p "$3" -t "$PHASE_S" -J > "$R/$1-upload.json"
    wait "$DOWNLOAD"
}

mark start; sleep 15
mark remote-upload;       X cpe iperf3 -c 10.99.0.2 -p 5204 -t "$PHASE_S" -J > "$R/remote-upload.json"
mark idle; sleep 10
mark remote-download;     X cpe iperf3 -c 10.99.0.2 -p 5203 -R -t "$PHASE_S" -J > "$R/remote-download.json"
mark idle; sleep 10
mark local-dl-remote-ul;  pair local-dl-remote-ul 5202 5204
mark idle; sleep 10
mark local-ul-remote-dl;  pair local-ul-remote-dl 5203 5201
mark idle; sleep 10
mark local-bidirectional; pair local-bidirectional 5202 5201
mark idle; sleep 5
mark end

kill "$PROBE" "$SAMPLER" 2>/dev/null
kill -TERM "$DAEMON"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$DAEMON" 2>/dev/null || break; sleep 1; done
kill -0 "$DAEMON" 2>/dev/null && kill -KILL "$DAEMON"
wait "$DAEMON"; echo "$NAME: daemon exit $?"
cp "$LOG" "$R/cake-adapt.log"
rm -f "$T/logs-$NAME/cake-adapt.log"
echo "$NAME: leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')]"
