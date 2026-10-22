#!/bin/bash
set -eu
task_dir=$(cd "$(dirname "$0")" && pwd)
M=$(mktemp -d /tmp/native-gate3-mock.XXXXXX)
trap 'rm -rf "$M"' EXIT
mkdir -p "$M/bin" "$M/task"
cat > "$M/bin/ip" <<'EOF'
#!/bin/bash
set -eu
shift 3
exec "$@"
EOF
cat > "$M/bin/fping" <<'EOF'
#!/bin/bash
set -eu
mkdir "$MOCK_LOCK"
trap 'rmdir "$MOCK_LOCK"' EXIT
case "$0" in
*/fping) echo '[1000.000000] 10.99.0.11 : [0], 64 bytes, 20.000 ms' ;;
*) echo 'serial,receive_realtime_us,rtt_us,kernel_timestamp'; echo '0,1000000000,20000,1' ;;
esac
EOF
cp "$M/bin/fping" "$M/task/icmp-driver"
chmod +x "$M/bin/ip" "$M/bin/fping" "$M/task/icmp-driver"
MOCK_LOCK="$M/probe.lock" PATH="$M/bin:$PATH" T="$M/task" COUNT=2 bash "$task_dir/probes.sh"
python3 "$task_dir/analyze.py" "$M/task/results" 2 >/dev/null
echo 'mock: sequence, serialization, counts and scoring pass'
