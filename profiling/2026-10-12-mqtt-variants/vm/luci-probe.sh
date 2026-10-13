# Runs on the VM: log in as the limited test user through uhttpd and do what config.js does.
rpc() { curl -s http://127.0.0.1/ubus -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"call\",\"params\":[\"$1\",\"$2\",\"$3\",$4]}"; }
L=$(rpc 00000000000000000000000000000000 session login '{"username":"variantstest","password":"<redacted>"}')
S=$(echo "$L" | jsonfilter -e '@.result[1].ubus_rpc_session')
echo "login: ${S:+ok}"
echo "file.read features: $(rpc "$S" file read '{"path":"/usr/share/cake-adapt/features"}')"
echo "file.read other (must be denied): $(rpc "$S" file read '{"path":"/etc/shadow"}')"
echo "uci get mqtt_enabled: $(rpc "$S" uci get '{"config":"cake-adapt","section":"main","option":"mqtt_enabled"}')"
echo "installed config.js sha256: $(sha256sum /www/luci-static/resources/view/cake-adapt/config.js | cut -d' ' -f1)"
echo "luci getFeatures as anonymous: $(rpc 00000000000000000000000000000000 luci getFeatures '{}' | head -c 120)"
