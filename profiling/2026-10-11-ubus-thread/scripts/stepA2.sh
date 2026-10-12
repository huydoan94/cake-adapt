D=$(cat /var/run/cake-adapt/main.lock); echo "service daemon $D ($(pidof cake-adapt))"
ls -la /var/run/cake-adapt/
s=$(now_ms); /usr/sbin/cake-adapt -C /var/run/cake-adapt/main.uci -S main; rc=$?
echo "second daemon by hand: rc=$rc after $(( $(now_ms) - s )) ms"
logread | grep cake-adapt | tail -2
echo "service daemon after: $(cat /var/run/cake-adapt/main.lock) ($(pidof cake-adapt)); qdiscs:"; tc qdisc show | grep cake
/usr/sbin/cake-adapt -C /var/run/cake-adapt/main.uci -S main -V; echo "validation (-V) while running: rc=$?"
/usr/sbin/cake-adapt -L > /dev/null; echo "listing (-L): rc=$?"
/etc/init.d/cake-adapt restart; sleep 3
N=$(cat /var/run/cake-adapt/main.lock); echo "after restart: $N ($(pidof cake-adapt)), was $D"
logread | grep cake-adapt | tail -3
ubus call cake-adapt.main status | grep '"pid"'
