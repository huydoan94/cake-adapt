mark "R0 baseline"; state

mark "R1 restart, configuration unchanged"; P=$(pid); S0=$(uci_sum)
$I restart; echo "  rc=$?"; sleep 6; state
[ "$(running)" = true ] && [ "$(pid)" != "$P" ] && [ "$(uci_sum)" = "$S0" ] && echo "  R1 PASS new pid, same file" || echo "  R1 FAIL"
since "R1 restart"

mark "R2 refused change min_dl 60000 > max 50000, restart"; P=$(pid); S0=$(uci_sum)
uci set cake-adapt.main.min_dl_shaper_rate_kbps=60000; uci commit cake-adapt
$I restart; echo "  rc=$?"; sleep 6; state
[ "$(running)" = true ] && [ "$(pid)" = "$P" ] && [ "$(uci_sum)" = "$S0" ] && echo "  R2 PASS pid and file kept" || echo "  R2 FAIL"
since "R2 refused"

mark "R3 refused, restart main"; P=$(pid)
$I restart main; echo "  rc=$?"; sleep 6; state
[ "$(running)" = true ] && [ "$(pid)" = "$P" ] && echo "  R3 PASS pid kept" || echo "  R3 FAIL"
since "R3 refused"

mark "R4 fixed, restart main"; P=$(pid)
cp /root/rollback/cake-adapt.config.before-restart /etc/config/cake-adapt
$I restart main; echo "  rc=$?"; sleep 6; state
[ "$(running)" = true ] && [ "$(pid)" != "$P" ] && echo "  R4 PASS new pid" || echo "  R4 FAIL"
since "R4 fixed"

mark "R5 refused with no previous file: stop, then restart"
$I stop; sleep 3
uci set cake-adapt.main.min_dl_shaper_rate_kbps=60000; uci commit cake-adapt
$I restart; echo "  rc=$?"; sleep 4; state
[ "$(running)" != true ] && [ ! -f $U ] && echo "  R5 PASS not started" || echo "  R5 FAIL"
since "R5 refused"

mark "R6 restore configuration, start"
cp /root/rollback/cake-adapt.config.before-restart /etc/config/cake-adapt
$I start; sleep 5; state
cmp /etc/config/cake-adapt /root/rollback/cake-adapt.config.before-restart && echo "  configuration restored"
echo "  respawn: $(svc | jsonfilter -e '@["cake-adapt"].instances.main.respawn')"
