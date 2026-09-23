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
    enabled interface ul_if dl_if cake_autorate_config adjust_dl_shaper_rate \
    pinger_method ping_extra_args
EOF
chmod +x "$temporary_directory/options"

cat > "$temporary_directory/uci" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$CALL_LOG"
EOF
chmod +x "$temporary_directory/uci"

cat > "$temporary_directory/source/defaults.sh" <<'EOF'
ul_if=wan
dl_if=ifb4wan
adjust_dl_shaper_rate=0
pinger_method=fping
ping_extra_args=""
reflectors=("1.1.1.1" "8.8.8.8")
EOF

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
    "$temporary_directory/options"

grep -Fq 'set cake-adapt.main.ul_if=eth1' "$temporary_directory/calls"
grep -Fq 'set cake-adapt.main.dl_if=download' "$temporary_directory/calls"
if grep -Fq 'set cake-adapt.main.interface=' "$temporary_directory/calls"; then
    exit 1
fi
grep -Fq 'set cake-adapt.main.adjust_dl_shaper_rate=1' "$temporary_directory/calls"
grep -Fq 'set cake-adapt.main.ping_extra_args=-I eth1' "$temporary_directory/calls"
grep -Fq 'add_list cake-adapt.main.reflectors=9.9.9.9' "$temporary_directory/calls"
grep -Fq 'add_list cake-adapt.main.reflectors=149.112.112.112' "$temporary_directory/calls"
grep -Fq 'commit cake-adapt' "$temporary_directory/calls"

cat > "$temporary_directory/options" <<'EOF'
#!/bin/sh
exit 1
EOF
if CALL_LOG="$temporary_directory/calls" \
    UCI="$temporary_directory/uci" \
    bash ../files/cake-adapt.import \
        "$temporary_directory/source/config.primary.sh" \
        "$temporary_directory/cake" \
        "$temporary_directory/options" 2>/dev/null; then
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
        "$temporary_directory/options" 2>/dev/null; then
    exit 1
fi

printf '%s\n' 'cake-autorate import tests passed'
