U=$(pidof ubusd); D=$(cat /var/run/cake-adapt/main.lock)
echo "ubusd=$U daemon=$D"
grep -E "status published" $L | tail -1
kill $U; k=$(now_ms); echo "$(date +%T) ubusd $U killed (SIGTERM)"
i=0; while [ $i -lt 20 ]; do n=$(pidof ubusd); [ -n "$n" ] && [ "$n" != "$U" ] && break; sleep 1; i=$((i+1)); done
echo "$(date +%T) procd respawned ubusd as $(pidof ubusd)"
i=0; while [ $i -lt 15 ]; do grep -q "status published" $L && [ "$(grep -E 'status published|lost the ubus' $L | tail -1 | grep -c published)" = 1 ] && [ "$(grep -c 'lost the ubus' $L)" -gt 0 ] && break; sleep 1; i=$((i+1)); done
grep -E "status published|lost the ubus|could not publish" $L | tail -3
status_call 10; cat /tmp/st.json; rm -f /tmp/st.json
echo "daemon still $(cat /var/run/cake-adapt/main.lock), alive: $([ -d /proc/$D ] && echo yes || echo no)"
logread | grep -E "cake-adapt" | tail -4
