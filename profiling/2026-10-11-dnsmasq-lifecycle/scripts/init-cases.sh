cp /etc/config/cake-adapt /root/rollback/cake-adapt.config.case
mark "A0 baseline"; state

mark "A1 reload, no change"; P=$(pid); $I reload; sleep 4; state; [ "$(pid)" = "$P" ] && echo "  A1 PASS pid unchanged" || echo "  A1 FAIL"

mark "A2 reload, valid change base_dl 20000->21000"; P=$(pid); S0=$(uci_sum)
uci set cake-adapt.main.base_dl_shaper_rate_kbps=21000; uci commit cake-adapt; $I reload; sleep 5; state
[ "$(pid)" != "$P" ] && [ "$(uci_sum)" != "$S0" ] && echo "  A2 PASS restarted with new file" || echo "  A2 FAIL"
grep -c "base_dl_shaper_rate_kbps '21000'" $U

mark "A3 reload, refused change min_dl 60000 > max 50000"; P=$(pid); S0=$(uci_sum)
uci set cake-adapt.main.min_dl_shaper_rate_kbps=60000; uci commit cake-adapt; $I reload; sleep 5; state
[ "$(pid)" = "$P" ] && [ "$(uci_sum)" = "$S0" ] && [ "$(running)" = true ] && echo "  A3 PASS kept previous pid and file" || echo "  A3 FAIL"
since "A3 reload"

mark "A4 add disabled section second, fix main"; P=$(pid)
uci set cake-adapt.main.min_dl_shaper_rate_kbps=10000
uci set cake-adapt.second=cake_adapt; uci set cake-adapt.second.enabled=0; uci set cake-adapt.second.interface=eth1; uci commit cake-adapt
S1=$(uci_sum); $I reload; sleep 5; state
echo "  (main file back to A2 content: $([ "$(uci_sum)" = "$S1" ] && echo same || echo changed))"
[ "$(pid)" = "$P" ] && echo "  A4 PASS main unchanged, pid kept" || echo "  A4 FAIL"
since "A4 add"

mark "A5 per-instance stop/start/restart"; P=$(pid)
$I stop second; sleep 2; state; [ "$(pid)" = "$P" ] && echo "  A5a PASS stop second leaves main" || echo "  A5a FAIL"
$I stop main; sleep 3; state; ls /var/run/cake-adapt
[ -z "$(pid)" ] || [ "$(running)" != true ] && [ ! -f $U ] && echo "  A5b PASS main stopped, file removed" || echo "  A5b FAIL"
$I start main; sleep 4; state; P=$(pid); [ "$(running)" = true ] && echo "  A5c PASS main started" || echo "  A5c FAIL"
$I restart main; sleep 5; state; [ "$(running)" = true ] && [ "$(pid)" != "$P" ] && echo "  A5d PASS main restarted" || echo "  A5d FAIL"
$I start nosuch; echo "  start nosuch rc=$?"; state
since "A5 per-instance"

mark "A6 disable main, reload"
uci set cake-adapt.main.enabled=0; uci commit cake-adapt; $I reload; sleep 5; state
[ "$(running)" != true ] && [ ! -f $U ] && echo "  A6 PASS disabled instance stopped" || echo "  A6 FAIL"
uci set cake-adapt.main.enabled=1; uci commit cake-adapt; $I reload; sleep 5; state

mark "A7 remove section main, reload"
uci delete cake-adapt.main; uci commit cake-adapt; $I reload; sleep 5; state
[ "$(running)" != true ] && [ ! -f $U ] && echo "  A7 PASS removed instance stopped" || echo "  A7 FAIL"

mark "A8 first start with invalid config"
cp /root/rollback/cake-adapt.config /etc/config/cake-adapt; uci delete cake-adapt.second 2>/dev/null; uci commit cake-adapt
$I stop; sleep 2
uci set cake-adapt.main.min_dl_shaper_rate_kbps=60000; uci commit cake-adapt
$I start; echo "  start rc=$?"; sleep 4; state
[ "$(running)" != true ] && [ ! -f $U ] && echo "  A8 PASS not started" || echo "  A8 FAIL"
since "A8 first"

mark "A9 restore original config, start"
cp /root/rollback/cake-adapt.config /etc/config/cake-adapt; $I start; sleep 5; state
cmp /etc/config/cake-adapt /root/rollback/cake-adapt.config && echo "  config restored"
since "A4 add" | grep -E "procd" | head -5
