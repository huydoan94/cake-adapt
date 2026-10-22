#!/bin/sh
# LuCI app checks that need no router: JSON files parse, views parse as
# JavaScript, the views' pure rules hold, and the Configuration view names exactly the daemon's options.
set -eu
app=../luci-app-cake-adapt
for json in "$app"/root/usr/share/luci/menu.d/*.json "$app"/root/usr/share/rpcd/acl.d/*.json; do
	python3 -I -m json.tool "$json" > /dev/null
done
for view in "$app"/htdocs/luci-static/resources/view/cake-adapt/*.js; do
	node --check "$view"
done
node ./luci_status.js "$app/htdocs/luci-static/resources/view/cake-adapt/status.js"
# Once per package's features file: cake-adapt (also with MQTT left on in an
# instance), cake-adapt-mqtt and cake-adapt-mqtt-ssl.
config=$app/htdocs/luci-static/resources/view/cake-adapt/config.js
node ./luci_config.js "$config" ''
node ./luci_config.js "$config" '' stored
node ./luci_config.js "$config" 'mqtt
'
node ./luci_config.js "$config" 'mqtt
tls
'
# Every daemon option is in the Configuration view, and nothing else: the
# constants named in the options[] table, plus reflectors, which config.c reads
# outside the table, minus config_file, log_file_path_override, ping_extra_args
# and ping_prefix_string, which only root sets, in /etc/cake-adapt/exec. The view names each option on the line of its
# taboption call, on the instance section (s) or on the nested Advanced
# section (a); names starting with _ (the nested section's holder) are not
# options.
daemon=$(sed -n '/^static const struct option_binding options\[\]/,/^};/p' ../src/config/config.c |
	grep -ow 'OPTION_[A-Z0-9_]*' |
	while read -r constant; do
		sed -n "s/^#define $constant \"\(.*\)\"/\1/p" ../src/common/constants.h
	done | grep -v -e '^config_file$' -e '^log_file_path_override$' -e '^ping_extra_args$' \
		-e '^ping_prefix_string$' | sort)
view=$(sed -n "s/.*[sa]\.taboption([^,]*, *form\.[A-Za-z]*, *'\([a-zA-Z0-9_]*\)'.*/\1/p" \
	"$app/htdocs/luci-static/resources/view/cake-adapt/config.js" |
	grep -v -e '^reflectors$' -e '^_' | sort -u)
[ "$daemon" = "$view" ] || {
	printf 'Configuration view and daemon options differ:\n' >&2
	printf '%s\n' "$daemon" > "${TMPDIR:-/tmp}/cake-adapt-daemon-options"
	printf '%s\n' "$view" > "${TMPDIR:-/tmp}/cake-adapt-view-options"
	diff "${TMPDIR:-/tmp}/cake-adapt-daemon-options" "${TMPDIR:-/tmp}/cake-adapt-view-options" >&2 || true
	exit 1
}
printf '%s\n' 'LuCI app checks passed'
