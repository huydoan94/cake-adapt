#!/bin/sh
set -eu

# Exercise the actual procd entry point without starting a daemon.
PROG=test_program
RUNTIME_DIR=/tmp/cake-adapt-init-test-runtime
. ../files/cake-adapt.init

config_status=0
sections=''
primary_enabled=0
secondary_enabled=0
primary_config=''
secondary_config=''
failed_generation=''
logged=''
instances=0
instance=''
opened=''
parameters=''
generated=''

config_load() { return "$config_status"; }
config_foreach() {
    local callback="$1"
    local section

    for section in $sections; do
        "$callback" "$section"
    done
}
config_get_bool() {
    case "$2" in
        primary) enabled="$primary_enabled" ;;
        secondary) enabled="$secondary_enabled" ;;
        *) enabled=0 ;;
    esac
}
config_get() {
    case "$2" in
        primary) config_file="$primary_config" ;;
        secondary) config_file="$secondary_config" ;;
        *) config_file='' ;;
    esac
}
generate_runtime_config() {
    generated="$generated $1:$2:$3"
    [ "$1" != "$failed_generation" ]
}
mkdir() { :; }
chmod() { :; }
logger() { logged="$logged|$*"; }
procd_open_instance() {
    instance="$1"
    instances=$((instances + 1))
    opened="$opened $1"
}
procd_close_instance() { :; }
procd_set_param() { parameters="$parameters [$instance] $*"; }

if start_service; then exit 1; fi
case "$logged" in
    *daemon.err*'no cake_adapt instances configured'*) ;;
    *) exit 1 ;;
esac

config_status=1
logged=''
if start_service; then exit 1; fi
case "$logged" in
    *daemon.err*'could not be loaded'*) ;;
    *) exit 1 ;;
esac

config_status=0
sections='primary secondary'
logged=''
start_service
[ "$instances" -eq 0 ]
case "$logged" in
    *"instance 'primary' disabled"*"instance 'secondary' disabled"*'no cake-adapt instances are enabled'*) ;;
    *) exit 1 ;;
esac

primary_enabled=1
secondary_enabled=1
primary_config='/etc/cake-adapt/config.primary.sh'
secondary_config=''
logged=''
start_service
[ "$instances" -eq 2 ]
[ "$opened" = ' primary secondary' ]
case "$generated" in
    *"primary:$primary_config:$RUNTIME_DIR/primary"*"secondary::$RUNTIME_DIR/secondary"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[primary] command test_program -C $RUNTIME_DIR/primary -S primary"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[primary] file /etc/config/cake-adapt $primary_config"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[secondary] command test_program -C $RUNTIME_DIR/secondary -S secondary"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[secondary] file /etc/config/cake-adapt"*) ;;
    *) exit 1 ;;
esac

instances=0
opened=''
parameters=''
generated=''
logged=''
failed_generation=primary
start_service
[ "$instances" -eq 1 ]
[ "$opened" = ' secondary' ]
case "$logged" in
    *"not starting instance 'primary'"*) ;;
    *) exit 1 ;;
esac

sections='primary'
instances=0
logged=''
if start_service; then exit 1; fi
[ "$instances" -eq 0 ]

printf '%s\n' 'init service tests passed'
