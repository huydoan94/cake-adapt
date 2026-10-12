R=/root/rollback/cake-adapt.config.before-restart
mark "F0 restart of a section never configured (tmpcheck)"; P=$(pid)
$I restart tmpcheck; echo "  rc=$?"; sleep 2; state
[ "$(pid)" = "$P" ] && echo "  F0 PASS main untouched" || echo "  F0 FAIL"
since "F0 restart"

mark "F1 section main deleted, restart main"
uci delete cake-adapt.main; uci commit cake-adapt
$I restart main; echo "  rc=$?"; sleep 6; state
[ "$(running)" != true ] && [ ! -f $U ] && echo "  F1 PASS stopped, file removed" || echo "  F1 FAIL"
since "F1 section"

mark "F2 restored, start; then enabled=0, restart main"
cp $R /etc/config/cake-adapt; $I start; sleep 5; state
uci set cake-adapt.main.enabled=0; uci commit cake-adapt
$I restart main; echo "  rc=$?"; sleep 6; state
[ "$(running)" != true ] && [ ! -f $U ] && echo "  F2 PASS stopped, file removed" || echo "  F2 FAIL"
since "F2 restored"

mark "F3 restore configuration, start"
cp $R /etc/config/cake-adapt; $I start; sleep 5; state
cmp /etc/config/cake-adapt $R && echo "  configuration restored"
