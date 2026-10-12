#!/usr/bin/env python3
"""One bounded observation-only MQTT acceptance on the authorized x86 VM."""
import getpass
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import tarfile
import time

root = Path.cwd()
output = root / 'build/sdk-ssl-validation/evidence-vm-tls'
output.mkdir(parents=True, exist_ok=True)
payload_root = root / 'build/sdk-ssl-validation/ssl-root'
remote_root = '/tmp/cake-adapt-mqtt-tls'
ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o',
       'StrictHostKeyChecking=yes']
host = 'root@192.168.56.2'
resources = []
remote_created = False
evidence = {'scope': 'packaged x86 daemon and dependencies in /tmp; verified TLS, observation-only; no installed-service change'}

def remote(script, timeout=10):
    result = subprocess.run(ssh + [host, 'sh -s'], input=script, text=True,
                            capture_output=True, timeout=timeout)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout

def line(process):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(10), 'remote test did not report readiness'
    value = process.stdout.readline().strip()
    assert value.startswith('READY '), value
    return value

def acceptance_expired(signum, frame):
    raise TimeoutError('60-second acceptance deadline exceeded')

signal.signal(signal.SIGALRM, acceptance_expired)

try:
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    def openssl(*options):
        subprocess.run(['openssl', *options], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=10)
    ca = output / 'ca.pem'
    ca_key = output / 'ca.key'
    certificate = output / 'server.pem'
    key = output / 'server.key'
    csr = output / 'server.csr'
    extensions = output / 'server.extensions'
    extensions.write_text('basicConstraints=CA:FALSE\nsubjectAltName=IP:127.0.0.1\n'
                          'extendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n')
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', str(ca_key), '-out', str(ca), '-subj', '/CN=VM MQTT test CA')
    openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
            '-out', str(csr), '-subj', '/CN=127.0.0.1')
    openssl('x509', '-req', '-in', str(csr), '-CA', str(ca), '-CAkey', str(ca_key),
            '-CAcreateserial', '-days', '1', '-out', str(certificate), '-extfile', str(extensions))
    config = output / 'broker.conf'
    config.write_text(f'listener {port} 127.0.0.1\nuser {getpass.getuser()}\nallow_anonymous true\ncertfile {certificate}\nkeyfile {key}\ncafile {ca}\npersistence false\nlog_dest stdout\n')
    broker_log = open(output / 'broker.log', 'w')
    broker = subprocess.Popen([str(root / 'build/mqtt-validation/mosquitto-2.0.22/src/mosquitto'),
                               '-c', str(config)], stdout=broker_log, stderr=subprocess.STDOUT)
    resources.append(broker)
    deadline = time.monotonic() + 3
    while True:
        assert broker.poll() is None, 'broker exited'
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                break
        except OSError:
            assert time.monotonic() < deadline, 'broker readiness timeout'
            time.sleep(.05)
    payload = output / 'payload.tar.gz'
    with tarfile.open(payload, 'w:gz') as archive:
        for name in ('usr/sbin/cake-adapt', 'usr/lib'):
            archive.add(payload_root / name, arcname=name)
        archive.add(ca, arcname='ca.pem')
    remote(f'set -eu\ntest ! -e {remote_root}\nmkdir -m 700 {remote_root}\n')
    remote_created = True
    transfer = subprocess.run(ssh + [host, f'tar -xzf - -C {remote_root}'],
                              input=payload.read_bytes(), capture_output=True, timeout=10)
    assert transfer.returncode == 0, transfer.stderr
    signal.setitimer(signal.ITIMER_REAL, 60)
    names = ['usr/sbin/cake-adapt', 'usr/lib/libmosquitto.so.1', 'usr/lib/libcares.so.2',
             'usr/lib/libssl.so.3', 'usr/lib/libcrypto.so.3']
    evidence['payload_sha256'] = {}
    for name in names:
        evidence['payload_sha256'][name] = hashlib.sha256((payload_root/name).read_bytes()).hexdigest()
    names.append('ca.pem')
    evidence['payload_sha256']['ca.pem'] = hashlib.sha256(ca.read_bytes()).hexdigest()
    hashes = remote(f'cd {remote_root}\nsha256sum ' + ' '.join(names) + '\n')
    for record in hashes.splitlines():
        digest, name = record.split()
        assert digest == evidence['payload_sha256'][name]
    evidence['installed_pids_before'] = remote('pidof cake-adapt || :\n').strip()
    evidence['qdisc_before'] = remote('tc qdisc show\n')
    mqtt_file = open(output / 'mqtt.log', 'w')
    environment = dict(os.environ, LD_LIBRARY_PATH=str(root/'build/mqtt-validation/mosquitto-2.0.22/lib'))
    subscriber = subprocess.Popen([str(root/'build/mqtt-validation/mosquitto-2.0.22/client/mosquitto_sub'),
                                   '-h', '127.0.0.1', '-p', str(port), '-t', 'cake-adapt/vm-tls', '--cafile', str(ca)],
                                  stdout=mqtt_file, stderr=subprocess.PIPE, env=environment)
    resources.append(subscriber)
    script = f'''set -eu
testroot={remote_root}
log=/tmp/sqm-mon-test.log
test ! -e /var/run/cake-adapt/mqtt_tls_test.lock
if [ -e "$log" ]; then
    for fd in /proc/[0-9]*/fd/*; do
        [ "$fd" -ef "$log" ] || continue
        info=${{fd%/fd/*}}/fdinfo/${{fd##*/}}
        flags=$(sed -n 's/^flags:[[:space:]]*//p' "$info")
        case "$flags" in *[1256]) echo "test log has a writer: $fd"; exit 1;; esac
    done
else
    touch "$log"
fi
inode=$(ls -i "$log" | awk '{{print $1}}')
: > "$log"
mkdir "$testroot/logs"
ln "$log" "$testroot/logs/cake-adapt.mqtt_tls_test.log"
cat > "$testroot/test.uci" <<'UCI'
config cake_adapt 'mqtt_tls_test'
    option enabled '1'
    option interface 'eth1'
    option adjust_dl_shaper_rate '0'
    option adjust_ul_shaper_rate '0'
    option enable_sleep_function '0'
    option no_pingers '1'
    list reflectors '127.0.0.1'
    option output_memory_stats '1'
    option mqtt_enabled '1'
    option mqtt_host '127.0.0.1'
    option mqtt_port '{port}'
    option mqtt_topic 'cake-adapt/vm-tls'
    option mqtt_tls '1'
    option mqtt_ca_file '{remote_root}/ca.pem'
    option log_file_path_override '{remote_root}/logs'
UCI
export LD_LIBRARY_PATH="$testroot/usr/lib"
"$testroot/usr/sbin/cake-adapt" -V -C "$testroot/test.uci" -S mqtt_tls_test > "$testroot/validation.log" 2>&1
"$testroot/usr/sbin/cake-adapt" -f -C "$testroot/test.uci" -S mqtt_tls_test > "$testroot/daemon.out" 2>&1 &
pid=$!
echo "$pid" > "$testroot/daemon.pid"
echo "$inode" > "$testroot/log.inode"
echo "READY $pid $inode"
wait "$pid"
echo "EXITED"
test "$(ls -i "$log" | awk '{{print $1}}')" = "$inode"
'''
    (output/'remote-script.sh').write_text(script)
    tunnel = subprocess.Popen(ssh + ['-o', 'ExitOnForwardFailure=yes', '-R',
                                    f'127.0.0.1:{port}:127.0.0.1:{port}', host, 'sh -s'],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, bufsize=1)
    resources.append(tunnel)
    tunnel.stdin.write(script)
    tunnel.stdin.close()
    ready = line(tunnel)
    evidence['ready'] = ready
    pid = ready.split()[1]
    deadline = time.monotonic() + 13
    while not any(record.startswith('MEMORY;') for record in (output/'mqtt.log').read_text().splitlines()):
        assert tunnel.poll() is None and subscriber.poll() is None
        assert time.monotonic() < deadline, 'daemon produced no periodic MEMORY record'
        time.sleep(.1)
    broker.terminate()
    broker.wait(timeout=3)
    time.sleep(1)
    remote(f'kill -0 {pid}\n')
    children = remote(f"awk -v wanted={pid} '/^Pid:/ {{child=$2}} /^PPid:/ && $2==wanted {{print child}}' /proc/[0-9]*/status\n").split()
    evidence['owned_child_pids'] = children
    evidence['alive_during_outage'] = True
    remote(f'kill -TERM {pid}\n')
    tunnel.wait(timeout=5)
    assert tunnel.returncode == 0, tunnel.stderr.read()
    evidence['remote_exit_output'] = tunnel.stdout.read()
    evidence['daemon_output'] = remote(f'cat {remote_root}/daemon.out\n')
    log = remote('cat /tmp/sqm-mon-test.log\n')
    (output/'daemon.log').write_text(log)
    subscriber.terminate()
    subscriber.wait(timeout=3)
    mqtt_file.close()
    received = (output/'mqtt.log').read_text().splitlines()
    assert received and all(record in log.splitlines() for record in received)
    assert any(record.startswith('MEMORY;') for record in received), received
    evidence['received_records'] = len(received)
    evidence['qdisc_after'] = remote('tc qdisc show\n')
    assert evidence['qdisc_after'] == evidence['qdisc_before'], 'qdisc settings changed'
    evidence['installed_pids_after'] = remote('pidof cake-adapt || :\n').strip()
    assert evidence['installed_pids_after'] == evidence['installed_pids_before'], 'installed daemon changed'
    remote('set -eu\n' + '\n'.join(f'test ! -d /proc/{owned}' for owned in [pid] + children) + '\n')
    evidence['clean_shutdown'] = True
    evidence['result'] = 'passed'
    print('PASS: SSL packaged x86 daemon publishes exact MEMORY records through verified TLS, survives outage, stops cleanly; qdiscs and test-log inode unchanged')
finally:
    signal.setitimer(signal.ITIMER_REAL, 0)
    try:
        if remote_created:
            remote(f'''set -eu
if [ -f {remote_root}/daemon.pid ]; then
pid=$(cat {remote_root}/daemon.pid)
if [ -f /proc/$pid/cmdline ]; then
case "$(tr '\\000' ' ' < /proc/$pid/cmdline)" in
'{remote_root}/usr/sbin/cake-adapt '*)
kill -TERM "$pid" 2>/dev/null || :
for attempt in 1 2 3 4 5; do
    [ -d /proc/$pid ] || break
    sleep 1
done
if [ -d /proc/$pid ]; then
    children=$(awk -v wanted="$pid" '/^Pid:/ {{child=$2}} /^PPid:/ && $2==wanted {{print child}}' /proc/[0-9]*/status)
    kill -KILL "$pid" $children 2>/dev/null || :
    sleep 1
    test ! -d /proc/$pid
    for child in $children; do test ! -d /proc/$child; done
fi
;;
esac
fi
rm -f /var/run/cake-adapt/mqtt_tls_test.lock
fi
rm -rf {remote_root}
''')
    except Exception as error:
        evidence['cleanup_error'] = str(error)
        evidence['result'] = 'failed-cleanup'
    for process in reversed(resources):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
    (output/'results.json').write_text(json.dumps(evidence, indent=2)+'\n')
    if 'cleanup_error' in evidence:
        raise RuntimeError(evidence['cleanup_error'])
