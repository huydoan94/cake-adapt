#!/bin/bash
# capture.sh: TCP header capture on cwan while known ISP queues fill, with the
# emulated ISP's real backlog sampled every 100 ms as ground truth.
#   A  upload shaper above the 8 Mbit/s bottleneck  -> upstream queue only
#   B  download shaper above the 40 Mbit/s bottleneck -> downstream queue only
#   C  both above, both loaded                       -> both queues
#   D  both below capacity, both loaded              -> no ISP queue
T=/tmp/cake-adapt-test
R=$T/results/capture
mkdir -p "$R"
X() { ip netns exec "$@"; }
mark() { echo "$EPOCHREALTIME $1" >> "$R/phases"; }
shape() { # UL_MBIT DL_MBIT
    X cpe tc qdisc change dev cwan root cake bandwidth "$1"mbit
    X cpe tc qdisc change dev ifb4cwan root cake bandwidth "$2"mbit
}

# Ground truth: bytes queued in each tbf bottleneck and its rate.
ip netns exec isp bash -c '
    while true; do
        up=$(tc -s qdisc show dev iinet | sed -n "s/.*backlog \([0-9]*\)b.*/\1/p" | tail -1)
        down=$(tc -s qdisc show dev iwan | sed -n "s/.*backlog \([0-9]*\)b.*/\1/p" | tail -1)
        echo "$EPOCHREALTIME $up $down"
        read -t 0.1 <> <(:)
    done' > "$R/backlog" &
SAMPLER=$!
ip netns exec cpe tcpdump -i cwan -s 128 -w "$R/cwan.pcap" -U tcp 2>"$R/tcpdump.log" &
DUMP=$!
sleep 2

run() { # PHASE UL DL MODE
    shape "$2" "$3"
    mark "$1"
    case $4 in
    up)   X cpe iperf3 -c 10.99.0.2 -p 5201 -t 20 > /dev/null ;;
    down) X cpe iperf3 -c 10.99.0.2 -p 5202 -t 20 -R > /dev/null ;;
    both) ip netns exec cpe iperf3 -c 10.99.0.2 -p 5202 -t 20 -R > /dev/null &
          D=$!
          X cpe iperf3 -c 10.99.0.2 -p 5201 -t 20 > /dev/null
          wait "$D" ;;
    esac
    mark idle
    sleep 6
}
run A-upstream   12 30 up
run B-downstream  6 60 down
run C-both       12 60 both
run D-none        6 30 both
mark end
sleep 1
kill "$DUMP" "$SAMPLER" 2>/dev/null
wait 2>/dev/null
shape 6 30
ls -la "$R"
echo "leftover in cpe=[$(ip netns pids cpe | tr '\n' ' ')] isp=[$(ip netns pids isp | tr '\n' ' ')]"
