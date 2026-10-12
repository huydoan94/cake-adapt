U=$(pidof ubusd); D=$(cat /var/run/cake-adapt/main.lock)
trap 'kill -CONT $U 2>/dev/null' EXIT
daemon_pid() { cat /var/run/cake-adapt/main.lock 2>/dev/null; }
wait_new_pid() { # OLD: waits up to 20 s for procd to start another daemon that took the lock
	i=0; while [ $i -lt 20 ]; do n=$(daemon_pid); [ -n "$n" ] && [ "$n" != "$1" ] && kill -0 "$n" 2>/dev/null && { echo $n; return; }; sleep 1; i=$((i+1)); done
}
echo "ubusd=$U daemon=$D"
kill -STOP $U; echo "$(date +%T) ubusd stopped"
kill -KILL $D; echo "$(date +%T) daemon $D killed; waiting for procd to respawn it"
N=$(wait_new_pid $D); echo "$(date +%T) respawned as $N"
sleep 15
counts
echo "-- log lines about start, ubus or warnings:"
grep -E "Starting cake-adapt|ubus|WARNING|ERROR" $L | tail -4
echo "-- threads of $N:"
for t in /proc/$N/task/*; do echo "$t wchan=$(cat $t/wchan) $(grep State $t/status)"; done
ps w | grep -E "cake-adapt|fping" | grep -v grep
echo "== step 4: SIGTERM (what procd sends) to $N while ubusd is stopped"
s=$(now_ms); kill -TERM $N
while [ -d /proc/$N ]; do :; done
echo "$(date +%T) daemon $N exited $(( $(now_ms) - s )) ms after SIGTERM"
grep -E "leaving it behind|shutting down|Stopped" $L | tail -3
N2=$(wait_new_pid $N); echo "$(date +%T) procd respawned it as $N2"
sleep 12
echo "10 s warnings logged by $N2: $(grep -c "status not on ubus yet" $L) (file holds both runs if not rotated)"
echo "== CONT ubusd"
kill -CONT $U; echo "$(date +%T) ubusd continued"
i=0; while [ $i -lt 10 ] && [ "$(grep -c "status published on ubus" $L)" -eq 0 ]; do sleep 1; i=$((i+1)); done
echo "published logged within ~$i s of CONT:"; grep -E "status published|status not on ubus" $L | tail -3
status_call 10; grep -E '"pid"|"state"' /tmp/st.json; rm -f /tmp/st.json
echo "pid now $(daemon_pid) / $(pidof cake-adapt)"
logread | grep cake-adapt | tail -6
