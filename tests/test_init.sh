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
    *"primary:$primary_config:$RUNTIME_DIR/primary.uci"*"secondary::$RUNTIME_DIR/secondary.uci"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[primary] command test_program -C $RUNTIME_DIR/primary.uci -S primary"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[primary] file $RUNTIME_DIR/primary.uci"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[secondary] command test_program -C $RUNTIME_DIR/secondary.uci -S secondary"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[secondary] file $RUNTIME_DIR/secondary.uci"*) ;;
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

# A refused configuration keeps the runtime file the instance runs from, so
# procd sees the same registration and leaves it running.
command mkdir -p "$RUNTIME_DIR"
: > "$RUNTIME_DIR/primary.uci"
: > "$RUNTIME_DIR/removed.uci"
sections='primary secondary'
failed_generation=primary
instances=0
opened=''
parameters=''
logged=''
start_service
[ "$opened" = ' primary secondary' ]
case "$logged" in
    *"daemon.err configuration of instance 'primary' refused"*"instance 'primary' keeps its previous configuration $RUNTIME_DIR/primary.uci"*) ;;
    *) exit 1 ;;
esac
case "$parameters" in
    *"[primary] command test_program -C $RUNTIME_DIR/primary.uci -S primary"*"[primary] file $RUNTIME_DIR/primary.uci"*) ;;
    *) exit 1 ;;
esac
# A section no longer configured loses its runtime file.
[ -f "$RUNTIME_DIR/primary.uci" ]
[ ! -e "$RUNTIME_DIR/removed.uci" ]

# A disabled section loses it too.
primary_enabled=0
logged=''
start_service
[ ! -e "$RUNTIME_DIR/primary.uci" ]
primary_enabled=1

# Unreadable /etc/config/cake-adapt: instances with a runtime file keep it.
: > "$RUNTIME_DIR/primary.uci"
config_status=1
opened=''
logged=''
if start_service; then exit 1; fi
[ "$opened" = ' primary' ]
case "$logged" in
    *'could not be loaded'*"instance 'primary' keeps its previous configuration"*) ;;
    *) exit 1 ;;
esac
config_status=0

# A runtime directory that cannot be prepared: instances with a file keep it.
chmod() { return 1; }
opened=''
logged=''
if start_service; then exit 1; fi
[ "$opened" = ' primary' ]
case "$logged" in
    *'could not prepare runtime configuration directory'*"instance 'primary' keeps its previous configuration"*) ;;
    *) exit 1 ;;
esac
chmod() { :; }

# A symbolic link is not read through: nothing is kept.
command mv "$RUNTIME_DIR" "$RUNTIME_DIR.real"
command ln -s "$RUNTIME_DIR.real" "$RUNTIME_DIR"
opened=''
logged=''
if start_service; then exit 1; fi
[ -z "$opened" ]
case "$logged" in
    *'is a symbolic link; not starting, running instances stop'*) ;;
    *) exit 1 ;;
esac
command rm -f "$RUNTIME_DIR"
command mv "$RUNTIME_DIR.real" "$RUNTIME_DIR"

# A named instance: only that one is registered, and files of others stay.
: > "$RUNTIME_DIR/secondary.uci"
failed_generation=''
opened=''
generated=''
logged=''
start_service secondary
[ "$opened" = ' secondary' ]
case "$generated" in
    *primary*) exit 1 ;;
esac
[ -f "$RUNTIME_DIR/primary.uci" ]
opened=''
logged=''
if start_service unknown; then exit 1; fi
[ -z "$opened" ]
case "$logged" in
    *"no cake_adapt instance 'unknown' configured"*) ;;
    *) exit 1 ;;
esac

# Stopping removes the runtime file of the stopped instance, or of all.
stop_service secondary
[ -f "$RUNTIME_DIR/primary.uci" ]
[ ! -e "$RUNTIME_DIR/secondary.uci" ]
: > "$RUNTIME_DIR/secondary.uci"
stop_service
[ ! -e "$RUNTIME_DIR/primary.uci" ]
[ ! -e "$RUNTIME_DIR/secondary.uci" ]
command rm -rf "$RUNTIME_DIR"
sections='primary'

# A directory where the instance's runtime file belongs stops that instance.
command mkdir -p "$RUNTIME_DIR/primary.uci"
failed_generation=''
instances=0
logged=''
generated=''
if start_service; then exit 1; fi
command rm -rf "$RUNTIME_DIR"
[ "$instances" -eq 0 ]
[ -z "$generated" ]
case "$logged" in
    *"primary.uci is not a regular file; not starting instance 'primary'"*) ;;
    *) exit 1 ;;
esac

# Respawn never gives up: retry 0 is unlimited.
case "$parameters" in
    *"[primary] respawn 3600 5 0"*) ;;
    *) exit 1 ;;
esac

# Restart: each requested instance whose configuration passes is stopped and
# registered again, even when unchanged.
initscript=/etc/init.d/cake-adapt
# The ubus stub records the instance of each delete request.
killed=''
ubus_status=0
ubus() {
    local instance="${4##*\"instance\":\"}"

    [ "$1 $2 $3" = 'call service delete' ] || exit 1
    case "$4" in
        '{"name":"cake-adapt",'*) ;;
        *) exit 1 ;;
    esac
    killed="$killed ${instance%%\"*}"
    return "$ubus_status"
}
procd_lock() { :; }
rc_procd() { "$@"; }
command mkdir -p "$RUNTIME_DIR"
sections='primary secondary'
primary_enabled=1
secondary_enabled=1
failed_generation=''
opened=''
logged=''
restart
[ "$killed" = ' primary secondary' ]
[ "$opened" = ' primary secondary' ]

# A refused configuration with a previous runtime file keeps the instance
# running it: not stopped, registered unchanged.
: > "$RUNTIME_DIR/primary.uci"
failed_generation=primary
killed=''
opened=''
logged=''
restart
[ "$killed" = ' secondary' ]
[ "$opened" = ' primary secondary' ]
[ -f "$RUNTIME_DIR/primary.uci" ]
case "$logged" in
    *"configuration of instance 'primary' refused"*"instance 'primary' keeps its previous configuration"*) ;;
    *) exit 1 ;;
esac

# Without a previous file the refused instance does not start.
command rm -f "$RUNTIME_DIR/primary.uci"
killed=''
opened=''
logged=''
restart
[ "$killed" = ' secondary' ]
[ "$opened" = ' secondary' ]
case "$logged" in
    *"could not generate $RUNTIME_DIR/primary.uci; not starting instance 'primary'"*) ;;
    *) exit 1 ;;
esac

# A named restart touches only that instance.
: > "$RUNTIME_DIR/primary.uci"
failed_generation=''
killed=''
opened=''
generated=''
restart secondary
[ "$killed" = ' secondary' ]
[ "$opened" = ' secondary' ]
case "$generated" in
    *primary*) exit 1 ;;
esac
[ -f "$RUNTIME_DIR/primary.uci" ]

# A disabled instance stops on a named restart and loses its file.
primary_enabled=0
killed=''
opened=''
restart primary
[ "$killed" = ' primary' ]
[ -z "$opened" ]
[ ! -e "$RUNTIME_DIR/primary.uci" ]
primary_enabled=1

# So does a section no longer configured.
: > "$RUNTIME_DIR/removed.uci"
killed=''
opened=''
logged=''
restart removed || :
[ "$killed" = ' removed' ]
[ -z "$opened" ]
[ ! -e "$RUNTIME_DIR/removed.uci" ]

# An instance procd does not run (Not found, exit status 252) is no failure.
ubus_status=252
killed=''
logged=''
restart primary
[ "$killed" = ' primary' ]
case "$logged" in
    *'could not stop'*) exit 1 ;;
esac

# Any other ubus failure is logged, and the restart fails.
ubus_status=249
killed=''
opened=''
logged=''
if restart primary; then exit 1; fi
[ "$opened" = ' primary' ]
case "$logged" in
    *"daemon.err could not stop instance 'primary' for restart (ubus status 249); it keeps running"*) ;;
    *) exit 1 ;;
esac
ubus_status=0
command rm -rf "$RUNTIME_DIR"

printf '%s\n' 'init service tests passed'
