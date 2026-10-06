#!/bin/sh
# testbed.sh up|down|rate UL_MBIT DL_MBIT
# An emulated bloated ISP inside the VM, isolated from eth1 and its SQM:
#   [cpe] cwan (CAKE egress = upload; ingress -> ifb4cwan CAKE = download)
#     | veth
#   [isp] netem 10 ms base delay + tbf bottleneck with a 500 ms buffer, each way
#     | veth
#   [inet] iperf3 servers, reflectors 10.99.0.11-16, probe target 10.99.0.20
set -e
X() { ip netns exec "$@"; }

bottleneck() { # NS DEV MBIT
    limit=$(( $3 * 1000000 / 8 / 2 ))   # 500 ms of buffer at the line rate
    X "$1" tc qdisc replace dev "$2" root handle 1: netem delay 10ms limit 100000
    X "$1" tc qdisc replace dev "$2" parent 1:1 handle 10: tbf rate "$3"mbit burst 16k limit "$limit"
}

case "$1" in
up)
    for ns in cpe isp inet; do
        ip netns add "$ns"
        X "$ns" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
        X "$ns" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
        X "$ns" ip link set lo up
    done
    ip link add cwan type veth peer name iwan
    ip link set cwan netns cpe
    ip link set iwan netns isp
    ip link add iinet type veth peer name inet0
    ip link set iinet netns isp
    ip link set inet0 netns inet
    X cpe ip addr add 10.98.0.2/24 dev cwan
    X cpe ip link set cwan up
    X cpe ip route add default via 10.98.0.1
    X isp ip addr add 10.98.0.1/24 dev iwan
    X isp ip addr add 10.99.0.1/24 dev iinet
    X isp ip link set iwan up
    X isp ip link set iinet up
    X isp sysctl -qw net.ipv4.ip_forward=1
    X inet ip addr add 10.99.0.2/24 dev inet0
    for host in 11 12 13 14 15 16 20; do X inet ip addr add 10.99.0.$host/24 dev inet0; done
    X inet ip link set inet0 up
    X inet ip route add default via 10.99.0.1
    # SQM-like CAKE in the cpe namespace.
    X cpe ip link add ifb4cwan type ifb
    X cpe ip link set ifb4cwan up
    X cpe tc qdisc add dev cwan root cake bandwidth 6mbit besteffort nat
    X cpe tc qdisc add dev cwan handle ffff: ingress
    X cpe tc filter add dev cwan parent ffff: matchall action mirred egress redirect dev ifb4cwan
    X cpe tc qdisc add dev ifb4cwan root cake bandwidth 30mbit besteffort nat ingress
    bottleneck isp iinet 8      # upload
    bottleneck isp iwan 40      # download
    X inet iperf3 -s -p 5201 -D
    X inet iperf3 -s -p 5202 -D
    ;;
rate)
    # Change only the tbf rate; the queue already in the bottleneck is kept.
    X isp tc qdisc change dev iinet parent 1:1 handle 10: tbf rate "$2"mbit burst 16k limit $(( $2 * 1000000 / 16 ))
    X isp tc qdisc change dev iwan parent 1:1 handle 10: tbf rate "$3"mbit burst 16k limit $(( $3 * 1000000 / 16 ))
    ;;
down)
    for ns in cpe isp inet; do
        for pid in $(ip netns pids "$ns" 2>/dev/null); do kill "$pid" 2>/dev/null || true; done
    done
    sleep 1
    for ns in cpe isp inet; do ip netns del "$ns" 2>/dev/null || true; done
    ;;
esac
