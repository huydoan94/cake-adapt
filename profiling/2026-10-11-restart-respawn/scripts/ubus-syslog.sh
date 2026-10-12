L=/var/log/cake-adapt.log
D=$(cat /var/run/cake-adapt/main.lock); B=$(pidof ubusd)
mark "U1 ubusd killed; daemon=$D ubusd=$B"
kill $B
i=0; while [ $i -lt 40 ]; do since "U1 ubusd" | grep -q 'connected to ubus' && break; sleep 1; i=$((i + 1)); done
echo "  waited ${i} s; ubusd now $(pidof ubusd); daemon $(cat /var/run/cake-adapt/main.lock) alive: $([ -d /proc/$D ] && echo yes || echo no)"
echo "-- logread (with priority) since U1:"
logread | awk -v m="== U1" 'index($0, m) {on=1; next} on' | grep 'cake-adapt\['
echo "-- daemon log file:"
grep -E 'ubus' $L | grep -vE '^(DATA|LOAD|SHAPER|REFLECTOR|SUMMARY|CPU|MEMORY|TCP_)' | tail -n 6 | cut -c1-160
ubus call cake-adapt.main status | head -4
