set -u
echo "== before"; uci show rpcd | grep -c login; cat /etc/config/rpcd > /tmp/rpcd-before.txt
uci add rpcd login >/dev/null; uci set rpcd.@login[-1].username=lucitest; uci set rpcd.@login[-1].password='<throwaway hash>'
uci add_list rpcd.@login[-1].read='luci-app-cake-adapt'; uci add_list rpcd.@login[-1].read='luci-base'
uci add_list rpcd.@login[-1].write='luci-app-cake-adapt'; uci add_list rpcd.@login[-1].write='luci-base'
uci commit rpcd; /etc/init.d/rpcd restart; sleep 2
call() { curl -s -m 10 -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"call\",\"params\":[\"$SID\",\"$1\",\"$2\",$3]}" http://127.0.0.1/ubus; echo; }
SID=$(curl -s -m 10 -d '{"jsonrpc":"2.0","id":1,"method":"call","params":["00000000000000000000000000000000","session","login",{"username":"lucitest","password":"tmp-check-pw-1"}]}' http://127.0.0.1/ubus | sed 's/.*"ubus_rpc_session":"\([0-9a-f]*\)".*/\1/')
echo "sid length ${#SID}"; [ ${#SID} -eq 32 ] || { echo LOGIN FAILED; }
echo "== service list (allowed by the ACL)"; call service list '{"name":"cake-adapt"}'
echo "== rc list (still allowed)"; call rc list '{"name":"cake-adapt"}'
echo "== status"; call cake-adapt.main status '{}' | cut -c1-200
echo "== service set (not granted: expect Access denied)"; call service set '{"name":"x"}'
echo "== service delete (not granted)"; call service delete '{"name":"cake-adapt"}'
echo "== network.interface dump (not granted)"; call network.interface dump '{}'
echo "== service list for a service that does not exist"; call service list '{"name":"no-such-service"}'
echo "== stop the service"; /etc/init.d/cake-adapt stop; sleep 3
echo "-- procd after stop"; call service list '{"name":"cake-adapt"}'
echo "-- rc list after stop"; call rc list '{"name":"cake-adapt"}'
echo "-- status after stop"; call cake-adapt.main status '{}'
echo "== start the service"; /etc/init.d/cake-adapt start; sleep 4
echo "-- procd after start"; call service list '{"name":"cake-adapt"}' | cut -c1-300
echo "== served views"
for f in config status; do echo "$f.js $(curl -s -m 10 -o /tmp/v.js -w '%{http_code}' http://127.0.0.1/luci-static/resources/view/cake-adapt/$f.js) $(sha256sum < /tmp/v.js | cut -c1-64)"; done
rm -f /tmp/v.js
echo "== remove the temporary login"
cp /tmp/rpcd-before.txt /etc/config/rpcd; uci commit rpcd 2>/dev/null; /etc/init.d/rpcd restart; sleep 1
uci show rpcd | grep -c lucitest
echo "== after"; (uci show cake-adapt | cmp - /tmp/uci-before.txt && echo "uci identical"); pidof cake-adapt
