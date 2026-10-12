# Helpers for the restart and respawn checks on the x86 VM (prepended to each step).
I=/etc/init.d/cake-adapt
U=/var/run/cake-adapt/main.uci
svc() { ubus call service list '{"name":"cake-adapt"}'; }
pid() { svc | jsonfilter -q -e '@["cake-adapt"].instances.main.pid'; }
running() { svc | jsonfilter -q -e '@["cake-adapt"].instances.main.running'; }
uci_sum() { [ -f $U ] && sha256sum $U | cut -c1-16 || echo none; }
state() { echo "  pid=$(pid) running=$(running) main.uci=$(uci_sum) lock=$(cat /var/run/cake-adapt/main.lock 2>/dev/null)"; }
mark() { echo "== $(date +%T) $*"; logger -t restart-check "== $*"; }
since() { logread | awk -v m="== $1" 'index($0, m) {on=1; next} on' | grep -E 'cake-adapt|procd' | cut -c12-; }
