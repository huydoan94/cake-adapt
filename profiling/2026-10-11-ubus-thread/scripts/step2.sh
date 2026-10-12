U=$(pidof ubusd); D=$(pidof cake-adapt)
trap 'kill -CONT $U 2>/dev/null' EXIT
echo "ubusd=$U daemon=$D"
( end=$(( $(date +%s) + 80 )); while [ $(date +%s) -lt $end ]; do wget -q -O /dev/null -T 10 "https://speed.cloudflare.com/__down?bytes=25000000"; done ) & G=$!
sleep 8; counts
kill -STOP $U; echo "$(date +%T) ubusd stopped: $(grep State /proc/$U/status)"
for t in 15 30 45 60; do sleep 15; counts; grep '^LOAD;' $L | tail -1; done
status_call 3
kill -CONT $U; echo "$(date +%T) ubusd continued"
status_call 10; cat /tmp/st.json; rm -f /tmp/st.json
kill $G 2>/dev/null; killall wget 2>/dev/null
echo "daemon pid now $(pidof cake-adapt)"
grep -n -E "ubus|ERROR|WARNING" $L | tail -5
logread | grep cake-adapt | tail -3
