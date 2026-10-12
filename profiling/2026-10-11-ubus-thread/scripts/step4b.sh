U=$(pidof ubusd); D=$(cat /var/run/cake-adapt/main.lock)
trap 'kill -CONT $U 2>/dev/null' EXIT
echo "ubusd=$U daemon=$D"
kill -STOP $U; echo "$(date +%T) ubusd stopped"
s=$(now_ms); /etc/init.d/cake-adapt stop > /tmp/stop.out 2>&1 & P=$!
sleep 8
if kill -0 $P 2>/dev/null; then echo "$(date +%T) init stop still waiting after 8 s (it asks procd over ubus); daemon $D alive: $([ -d /proc/$D ] && echo yes || echo no)"; fi
kill -CONT $U; c=$(now_ms); echo "$(date +%T) ubusd continued"
while [ -d /proc/$D ]; do :; done
echo "daemon $D exited $(( $(now_ms) - c )) ms after CONT ($(( $(now_ms) - s )) ms after the stop command)"
wait $P; echo "init stop rc=$?"; cat /tmp/stop.out; rm -f /tmp/stop.out
grep -E "leaving it behind|shutting down|Stopped cake" $L | tail -3
/etc/init.d/cake-adapt start; sleep 3; echo "started again: $(cat /var/run/cake-adapt/main.lock)"
