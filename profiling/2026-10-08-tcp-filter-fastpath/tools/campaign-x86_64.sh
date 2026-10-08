#!/bin/sh
# Swaps only the filter object; the VM's service stays stopped during runs.
# Order ABBA per (timestamps, flows). Results in /root/userfp.
O=/root/userfp-x64/runs; mkdir -p $O
cp -a /lib/bpf/cake-adapt-tcpdelay.o $O/installed.o
/etc/init.d/cake-adapt stop; sleep 2
run() { # VARIANT TS FLOWS STEP
    cp /root/userfp-$1.o /lib/bpf/cake-adapt-tcpdelay.o
    sed -e "s/tcp_timestamps=0/tcp_timestamps=$2/" /root/cost-flows.sh > $O/cost.sh
    FLOWS=$3 sh $O/cost.sh >/dev/null 2>&1
    mkdir -p $O/$4 && cp -a /root/inject-cost-$3/. $O/$4/
    echo "$4 $1 ts=$2 flows=$3 sha=$(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12) service=$(pidof cake-adapt)" >> $O/progress
}
for ts in 1; do for flows in 4 1; do
    run base $ts $flows ts$ts-f$flows-base-a
    run new  $ts $flows ts$ts-f$flows-new-a
    run new  $ts $flows ts$ts-f$flows-new-b
    run base $ts $flows ts$ts-f$flows-base-b
done; done
cp -a $O/installed.o /lib/bpf/cake-adapt-tcpdelay.o
/etc/init.d/cake-adapt start
echo "finished restored=$(sha256sum /lib/bpf/cake-adapt-tcpdelay.o | cut -c1-12)" >> $O/progress
