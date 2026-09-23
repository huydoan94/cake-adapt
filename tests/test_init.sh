#!/bin/sh
set -eu

# Exercise the actual procd entry point without starting a daemon.
PROG=test_program
RUNTIME_DIR=/tmp/cake-adapt-init-test-runtime
RUNTIME_CONFIG=$RUNTIME_DIR/cake-adapt
. ../files/cake-adapt.init

config_status=0
generate_status=0
test_enabled=0
test_cake_autorate_config=''
logged=''
instances=0
parameters=''
generated=''

config_load() { return "$config_status"; }
config_get_bool() { enabled="$test_enabled"; }
config_get() { cake_autorate_config="$test_cake_autorate_config"; }
generate_runtime_config() { generated="$*"; return "$generate_status"; }
mkdir() { :; }
chmod() { :; }
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
test_cake_autorate_config='/root/cake-autorate/config.primary.sh'
logged=''
start_service
[ "$instances" -eq 1 ]
[ -z "$logged" ]
[ "$generated" = "$test_cake_autorate_config" ]
case "$parameters" in
    *"command test_program -C $RUNTIME_DIR"*'respawn'*'file /etc/config/cake-adapt /root/cake-autorate/config.primary.sh /root/cake-autorate/defaults.sh'*) ;;
    *) exit 1 ;;
esac

generate_status=1
logged=''
if start_service; then exit 1; fi
[ "$instances" -eq 1 ]
case "$logged" in
    *daemon.err*"could not generate $RUNTIME_CONFIG"*) ;;
    *) exit 1 ;;
esac
printf '%s\n' 'init service tests passed'
