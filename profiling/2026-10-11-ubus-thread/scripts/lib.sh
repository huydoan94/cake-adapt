L=/var/log/cake-adapt.log
now_ms() { awk '{printf "%d", $1*1000}' /proc/uptime; }
# status_call SECONDS: one ubus status call, killed after SECONDS; prints rc and duration.
status_call() {
	s=$(now_ms)
	ubus call cake-adapt.main status > /tmp/st.json 2>&1 &
	p=$!
	( sleep "$1"; kill $p 2>/dev/null ) &
	w=$!
	wait $p; rc=$?
	kill $w 2>/dev/null; wait $w 2>/dev/null
	echo "status call rc=$rc after $(( $(now_ms) - s )) ms (killed by watchdog: rc>=128)"
}
counts() { echo "$(date +%T) lines=$(wc -l < $L) DATA=$(grep -c '^DATA;' $L) LOAD=$(grep -c '^LOAD;' $L) last=$(tail -1 $L | cut -c1-40)"; }
