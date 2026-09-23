#!/bin/sh
set -eu

temporary_directory="$(mktemp -d /tmp/cake-adapt-import-test.XXXXXX)"
trap 'rm -rf "$temporary_directory"' EXIT INT TERM

mkdir -p "$temporary_directory/cake/.uci" "$temporary_directory/source"
: > "$temporary_directory/cake/cake-adapt"
: > "$temporary_directory/calls"

cat > "$temporary_directory/options" <<'EOF'
#!/bin/sh
printf '%s\n' \
    enabled interface ul_if dl_if config_file adjust_dl_shaper_rate \
    pinger_method ping_extra_args
EOF
chmod +x "$temporary_directory/options"

cat > "$temporary_directory/uci" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$CALL_LOG"
EOF
chmod +x "$temporary_directory/uci"

cat > "$temporary_directory/source/config.primary.sh" <<'EOF'
ul_if=eth1
dl_if=download
adjust_dl_shaper_rate=1
ping_extra_args="-I eth1"
reflectors=("9.9.9.9" "149.112.112.112")
EOF

CALL_LOG="$temporary_directory/calls" \
UCI="$temporary_directory/uci" \
bash ../files/cake-adapt.import \
    "$temporary_directory/source/config.primary.sh" \
    "$temporary_directory/cake" \
    "$temporary_directory/options" \
    primary

grep -Fq 'set cake-adapt.primary.ul_if=eth1' "$temporary_directory/calls"
grep -Fq 'set cake-adapt.primary.dl_if=download' "$temporary_directory/calls"
if grep -Fq 'set cake-adapt.primary.interface=' "$temporary_directory/calls"; then
    exit 1
fi
grep -Fq 'set cake-adapt.primary.adjust_dl_shaper_rate=1' "$temporary_directory/calls"
grep -Fq 'set cake-adapt.primary.ping_extra_args=-I eth1' "$temporary_directory/calls"
grep -Fq 'add_list cake-adapt.primary.reflectors=9.9.9.9' "$temporary_directory/calls"
grep -Fq 'add_list cake-adapt.primary.reflectors=149.112.112.112' "$temporary_directory/calls"
grep -Fq 'commit cake-adapt' "$temporary_directory/calls"

: > "$temporary_directory/calls"
cat > "$temporary_directory/source/config.partial.sh" <<'EOF'
adjust_dl_shaper_rate=0
EOF
CALL_LOG="$temporary_directory/calls" \
UCI="$temporary_directory/uci" \
bash ../files/cake-adapt.import \
    "$temporary_directory/source/config.partial.sh" \
    "$temporary_directory/cake" \
    "$temporary_directory/options" \
    primary
grep -Fq 'set cake-adapt.primary.adjust_dl_shaper_rate=0' \
    "$temporary_directory/calls"
if grep -Fq '.reflectors' "$temporary_directory/calls"; then
    exit 1
fi

if CALL_LOG="$temporary_directory/calls" \
    UCI="$temporary_directory/uci" \
    bash ../files/cake-adapt.import \
        "$temporary_directory/source/config.primary.sh" \
        "$temporary_directory/cake" \
        "$temporary_directory/options" \
        'bad/name' 2>/dev/null; then
    exit 1
fi

cat > "$temporary_directory/options" <<'EOF'
#!/bin/sh
exit 1
EOF
if CALL_LOG="$temporary_directory/calls" \
    UCI="$temporary_directory/uci" \
    bash ../files/cake-adapt.import \
        "$temporary_directory/source/config.primary.sh" \
        "$temporary_directory/cake" \
        "$temporary_directory/options" \
        primary 2>/dev/null; then
    exit 1
fi

cat > "$temporary_directory/options" <<'EOF'
#!/bin/sh
printf '%s\n' adjust_dl_shaper_rate
EOF

printf '%s\n' 'adjust_dl_shaper_rate=(' > \
    "$temporary_directory/source/config.primary.sh"
if CALL_LOG="$temporary_directory/calls" \
    UCI="$temporary_directory/uci" \
    bash ../files/cake-adapt.import \
        "$temporary_directory/source/config.primary.sh" \
        "$temporary_directory/cake" \
        "$temporary_directory/options" \
        primary 2>/dev/null; then
    exit 1
fi

printf '%s\n' 'standalone configuration import tests passed'
