L=/var/log/cake-adapt.log
now_ms() { awk '{printf "%d", $1*1000}' /proc/uptime; }
ubus_lines() { grep -E 'ubus' $L | grep -vE '^(DATA|LOAD|SHAPER|REFLECTOR|SUMMARY|CPU|MEMORY|TCP_)' | cut -c1-160; }
trap 'kill -CONT $(pidof ubusd) 2>/dev/null' EXIT
D=$(cat /var/run/cake-adapt/main.lock); U=$(pidof ubusd); uptime; dmesg | grep -c "rcu: INFO"
echo "daemon=$D ubusd=$U"
echo "-- ubus lines since the daemon started (first-try connection is not logged):"
ubus_lines
N0=$(ubus_lines | wc -l)

echo "== B1 ubusd killed (cases 1, 2, 3)"
kill $U; t=$(now_ms)
i=0; while [ $i -lt 30 ]; do ubus_lines | tail -n +$((N0 + 1)) | grep -q 'connected to ubus' && break; sleep 1; i=$((i + 1)); done
echo "after $(( $(now_ms) - t )) ms; ubusd now $(pidof ubusd)"
ubus_lines | tail -n +$((N0 + 1))
ubus call cake-adapt.main status | head -4
echo "daemon $(cat /var/run/cake-adapt/main.lock) alive: $([ -d /proc/$D ] && echo yes || echo no)"
logread | grep -E 'cake-adapt\[' | grep -i ubus | tail -4 | cut -c12-

echo "== B2 ubusd stopped 15 s (frozen, not lost)"
N1=$(ubus_lines | wc -l); U=$(pidof ubusd)
d0=$(grep -c '^DATA;' $L); kill -STOP $U; sleep 15; d1=$(grep -c '^DATA;' $L); kill -CONT $U
echo "DATA records during stop: $((d1 - d0))"
sleep 2
ubus call cake-adapt.main status | head -4
echo "new ubus lines:"; ubus_lines | tail -n +$((N1 + 1))
echo "daemon $(cat /var/run/cake-adapt/main.lock) alive: $([ -d /proc/$D ] && echo yes || echo no)"
echo "kernel RCU stall reports: $(dmesg | grep -c "rcu: INFO")"
