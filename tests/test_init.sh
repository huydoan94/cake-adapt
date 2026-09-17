#!/bin/sh
set -eu

# Exercise the actual procd entry point without starting a daemon.
. ../files/cake-adapt.init

config_status=0
test_enabled=0
logged=''
instances=0
parameters=''

config_load() { return "$config_status"; }
config_get_bool() { enabled="$test_enabled"; }
logger() { logged="$*"; }
procd_open_instance() { instances=$((instances + 1)); }
procd_close_instance() { :; }
procd_set_param() { parameters="$parameters $*"; }

start_service
[ "$instances" -eq 0 ]
case "$logged" in
    *daemon.notice*'disabled by configuration'*) ;;
    *) exit 1 ;;
esac

config_status=1
logged=''
if start_service; then exit 1; fi
[ "$instances" -eq 0 ]
case "$logged" in
    *daemon.err*'could not be loaded'*) ;;
    *) exit 1 ;;
esac

config_status=0
test_enabled=1
logged=''
start_service
[ "$instances" -eq 1 ]
[ -z "$logged" ]
case "$parameters" in
    *'command /usr/sbin/cake-adapt'*'respawn'*) ;;
    *) exit 1 ;;
esac
printf '%s\n' 'init service tests passed'
