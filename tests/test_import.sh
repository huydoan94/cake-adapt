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
    pinger_method ping_extra_args
EOF
chmod +x "$temporary_directory/options"

# A stand-in uci. Calls with -c write the runtime configuration and are
# logged; others read the system UCI, where "get" answers from UCI_VALUES
# ("name=value" entries separated by ";"), as if the section set them.
cat > "$temporary_directory/uci" <<'EOF'
#!/bin/sh
case " $* " in
*" -c "*)
    printf '%s\n' "$*" >> "$CALL_LOG"
    exit 0
    ;;
esac
for argument in "$@"; do last=$argument; done
name=${last##*.}
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

run_import() {
    CALL_LOG="$temporary_directory/calls" UCI="$temporary_directory/uci" \
        bash ../files/cake-adapt.import \
        "$temporary_directory/stage" "$temporary_directory/options" "$@"
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
if called 'pinger_method' || called 'ping_extra_args'; then exit 1; fi

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
if called 'adjust_dl_shaper_rate=0' || called 'interface=wrong' ||
    called '9.9.9.9' || called '149.112.112.112'; then
    exit 1
fi

# The file's reflectors when UCI has none.
: > "$temporary_directory/calls"
UCI_VALUES='enabled=1' run_import primary "$temporary_directory/source/config.primary.sh"
called 'add_list cake-adapt.primary.reflectors=9.9.9.9'
called 'add_list cake-adapt.primary.reflectors=149.112.112.112'

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
