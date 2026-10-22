#!/bin/sh
set -eu

temporary_directory="$(mktemp -d /tmp/cake-adapt-import-test.XXXXXX)"
trap 'rm -rf "$temporary_directory"' EXIT INT TERM

mkdir -p "$temporary_directory/stage/.uci" "$temporary_directory/source"
: > "$temporary_directory/stage/cake-adapt"
: > "$temporary_directory/calls"

cat > "$temporary_directory/options" <<'EOF'
#!/bin/sh
printf '%s\n' \
    enabled interface ul_if dl_if config_file adjust_dl_shaper_rate \
    pinger_method ping_extra_args ping_prefix_string log_file_path_override
EOF
chmod +x "$temporary_directory/options"

# A stand-in uci. Calls with -c write the runtime configuration and are
# logged; others read the system UCI, where "get" answers from UCI_VALUES
# ("name=value" entries separated by ";"), as if the section set them; an
# "exec.name=value" entry answers for /etc/cake-adapt/exec, which must be read
# from the exec directory (-c), and is logged.
cat > "$temporary_directory/uci" <<'EOF'
#!/bin/sh
case " $* " in
*" -c $EXEC_DIRECTORY "*)
    printf 'exec %s\n' "$*" >> "$CALL_LOG"
    ;;
*" -t "*)
    printf '%s\n' "$*" >> "$CALL_LOG"
    exit 0
    ;;
esac
case " $* " in *" show "*) exit "${SHOW_STATUS:-0}" ;; esac
for argument in "$@"; do last=$argument; done
name=${last##*.}
case $last in
exec.*)
    case " $* " in *" -c $EXEC_DIRECTORY "*) ;; *) exit 1 ;; esac
    name=exec.$name
    ;;
esac
IFS=';'
for entry in ${UCI_VALUES:-}; do
    if [ "${entry%%=*}" = "$name" ]; then
        printf '%s\n' "${entry#*=}"
        exit 0
    fi
done
exit 1
EOF
chmod +x "$temporary_directory/uci"

# A stand-in logger that records what would reach syslog.
cat > "$temporary_directory/logger" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$SYSLOG"
EOF
chmod +x "$temporary_directory/logger"
: > "$temporary_directory/syslog"

# Plain sh, as on a router without Bash; Bash runs only to read a file.
# run_import SECTION [CONFIG_FILE]: the file as the exec file's config_file.
run_import() {
    values=${UCI_VALUES:-}
    [ "$#" -lt 2 ] || values="$values;exec.config_file=$2"
    EXEC_DIRECTORY="$temporary_directory/exec-directory" \
        UCI_VALUES=$values CALL_LOG="$temporary_directory/calls" UCI="$temporary_directory/uci" \
        LOGGER="$temporary_directory/logger" SYSLOG="$temporary_directory/syslog" \
        BASH_READER=../files/cake-adapt.import-bash \
        sh ../files/cake-adapt.import \
        "$temporary_directory/stage" "$temporary_directory/options" "$1"
}
called() { grep -Fq -- "$1" "$temporary_directory/calls"; }

# UCI only: the section and every option it sets, its reflectors as a list.
UCI_VALUES='enabled=1;interface=eth1;adjust_dl_shaper_rate=1;reflectors=1.1.1.1 8.8.8.8' \
    run_import primary
called 'set cake-adapt.primary=cake_adapt'
called 'set cake-adapt.primary.enabled=1'
called 'set cake-adapt.primary.interface=eth1'
called 'set cake-adapt.primary.adjust_dl_shaper_rate=1'
called 'add_list cake-adapt.primary.reflectors=1.1.1.1'
called 'add_list cake-adapt.primary.reflectors=8.8.8.8'
called 'commit cake-adapt'
if called 'set cake-adapt.primary.pinger_method' ||
    called 'set cake-adapt.primary.ping_extra_args'; then
    exit 1
fi

# An exec file uci cannot parse is refused rather than read as empty; one
# that parses, or none at all, is fine.
mkdir "$temporary_directory/exec-directory"
: > "$temporary_directory/exec-directory/exec"
if SHOW_STATUS=1 UCI_VALUES='enabled=1' run_import primary 2> /dev/null; then exit 1; fi
grep -Fq "could not parse $temporary_directory/exec-directory/exec" "$temporary_directory/syslog"
UCI_VALUES='enabled=1' run_import primary
rm "$temporary_directory/exec-directory/exec"
SHOW_STATUS=1 UCI_VALUES='enabled=1' run_import primary

# The options that run something, or choose its arguments, come only from
# the exec file, and the runtime configuration keeps them; any of them in
# /etc/config/cake-adapt is refused, with the reason in syslog.
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1;exec.ping_prefix_string=mwan3 use wan;exec.ping_extra_args=-S 192.0.2.1' \
    run_import primary
called 'set cake-adapt.primary.ping_prefix_string=mwan3 use wan'
called 'set cake-adapt.primary.ping_extra_args=-S 192.0.2.1'
called "exec -q -c $temporary_directory/exec-directory get exec.primary.ping_extra_args"
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1;exec.log_file_path_override=/var/log/main' run_import primary
called 'set cake-adapt.primary.log_file_path_override=/var/log/main'
for name in config_file ping_prefix_string ping_extra_args log_file_path_override; do
    : > "$temporary_directory/calls"
    if UCI_VALUES="enabled=1;$name=/usr/bin/wrapper" run_import primary 2> /dev/null; then
        exit 1
    fi
    grep -Fq "option '$name' is refused in /etc/config/cake-adapt, which LuCI can change; move it to" \
        "$temporary_directory/syslog"
    if called 'commit cake-adapt'; then exit 1; fi
done

# UCI and a file: UCI wins, the file fills the rest; the file's interface is
# ignored, and UCI's reflectors replace the file's.
cat > "$temporary_directory/source/config.primary.sh" <<'EOF'
interface=wrong
ul_if=eth1
dl_if=download
adjust_dl_shaper_rate=0
ping_extra_args="-I eth1"
reflectors=("9.9.9.9" "149.112.112.112")
EOF
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1;adjust_dl_shaper_rate=1;reflectors=1.1.1.1' \
    run_import primary "$temporary_directory/source/config.primary.sh"
called 'set cake-adapt.primary.adjust_dl_shaper_rate=1'
called 'set cake-adapt.primary.ul_if=eth1'
called 'set cake-adapt.primary.dl_if=download'
called 'set cake-adapt.primary.ping_extra_args=-I eth1'
called 'add_list cake-adapt.primary.reflectors=1.1.1.1'
called "set cake-adapt.primary.config_file=$temporary_directory/source/config.primary.sh"
if called 'adjust_dl_shaper_rate=0' || called 'interface=wrong' ||
    called '9.9.9.9' || called '149.112.112.112'; then
    exit 1
fi

# The file's reflectors when UCI has none.
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1' run_import primary "$temporary_directory/source/config.primary.sh"
called 'add_list cake-adapt.primary.reflectors=9.9.9.9'
called 'add_list cake-adapt.primary.reflectors=149.112.112.112'

# The root-only file may set a prefix; the exec file's prefix wins over it.
printf '%s\n' 'ul_if=eth1' 'ping_prefix_string="ip vrf exec wan"' > "$temporary_directory/source/prefix.sh"
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1' run_import primary "$temporary_directory/source/prefix.sh"
called 'set cake-adapt.primary.ping_prefix_string=ip vrf exec wan'
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1;exec.ping_prefix_string=mwan3 use wan' \
    run_import primary "$temporary_directory/source/prefix.sh"
called 'set cake-adapt.primary.ping_prefix_string=mwan3 use wan'
if called 'ip vrf'; then exit 1; fi

# What the file prints is never read as values, even enabled.
cat > "$temporary_directory/source/noisy.sh" <<'EOF'
echo "s dl_if=injected"
echo "s enabled=1"
ul_if=eth1
EOF
: > "$temporary_directory/calls"
UCI_VALUES='reflectors=1.1.1.1' run_import primary "$temporary_directory/source/noisy.sh" 2> /dev/null
called 'set cake-adapt.primary.ul_if=eth1'
if called 'injected' || called 'enabled=1'; then exit 1; fi

# A file that exits early is refused, not imported without its values.
printf '%s\n' 'ul_if=eth1' 'exit 0' > "$temporary_directory/source/exits.sh"
: > "$temporary_directory/calls"
if run_import primary "$temporary_directory/source/exits.sh" 2> /dev/null; then exit 1; fi
if called 'commit cake-adapt'; then exit 1; fi

# The reader's reason for refusing a file reaches syslog.
printf '%s\n' 'reflectors=()' > "$temporary_directory/source/empty.sh"
if UCI_VALUES='enabled=1' run_import primary "$temporary_directory/source/empty.sh" 2> /dev/null; then
    exit 1
fi
grep -Fq 'reflectors is empty' "$temporary_directory/syslog"

# A file without Bash installed: refused, with the reason in syslog.
: > "$temporary_directory/calls"
if BASH_PROGRAM=/nonexistent/bash UCI_VALUES='enabled=1' \
    run_import primary "$temporary_directory/source/config.primary.sh" 2> /dev/null; then
    exit 1
fi
grep -Fq "cannot read Bash file $temporary_directory/source/config.primary.sh: bash is not installed" \
    "$temporary_directory/syslog"
if called 'commit cake-adapt'; then exit 1; fi

# Without a file, Bash is never needed.
: > "$temporary_directory/calls"
BASH_PROGRAM=/nonexistent/bash UCI_VALUES='enabled=1;reflectors=1.1.1.1' run_import primary
called 'add_list cake-adapt.primary.reflectors=1.1.1.1'
called 'commit cake-adapt'

# Refused: a bad section name, a relative or unreadable file, invalid syntax,
# an array for a scalar, and a cake-adapt that cannot list its options.
if run_import 'bad/name' 2> /dev/null; then exit 1; fi
if run_import primary config.primary.sh 2> /dev/null; then exit 1; fi
if run_import primary /nonexistent.sh 2> /dev/null; then exit 1; fi
printf '%s\n' 'adjust_dl_shaper_rate=(' > "$temporary_directory/source/bad.sh"
if run_import primary "$temporary_directory/source/bad.sh" 2> /dev/null; then exit 1; fi
printf '%s\n' 'ping_extra_args=(a b)' > "$temporary_directory/source/array.sh"
if run_import primary "$temporary_directory/source/array.sh" 2> /dev/null; then exit 1; fi
cat > "$temporary_directory/options" <<'EOF'
#!/bin/sh
exit 1
EOF
if run_import primary 2> /dev/null; then exit 1; fi

printf '%s\n' 'standalone configuration import tests passed'
