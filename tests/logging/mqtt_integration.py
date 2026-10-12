#!/usr/bin/env python3
"""Bounded loopback acceptance against a locally built Mosquitto, no installs."""
import argparse
import getpass
import hashlib
import json
import os
import pathlib
import selectors
import socket
import ssl
import struct
import subprocess
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=pathlib.Path, required=True)
parser.add_argument('--driver', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
parser.add_argument('--library', type=pathlib.Path, required=True)
parser.add_argument('--tls', action='store_true', help='include certificate-authenticated TLS cases')
parser.add_argument('--tls-only', action='store_true', help='run TLS cases and SSL-capable plain MQTT only')
parser.add_argument('--tls-stalled-only', action='store_true', help='run only the TLS handshake deadline/retry case')
args = parser.parse_args()
args.source = args.source.resolve()
args.library = args.library.resolve()
args.driver = args.driver.resolve()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
evidence = {'driver_sha256': hashlib.sha256(args.driver.read_bytes()).hexdigest(),
            'library_sha256': hashlib.sha256(args.library.read_bytes()).hexdigest(),
            'cases': []}


def reserve_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Driver:
    def __init__(self, port, username='', password='', ca=None, host='localhost'):
        command = [str(args.driver), str(port)]
        if username:
            command += [username, password]
        if ca is not None:
            command += ['--tls', str(ca)]
        command += ['--host', host]
        environment = dict(os.environ, LD_LIBRARY_PATH=str(args.library.parent))
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, bufsize=1, env=environment)
        assert self.read() == 'ready'

    def read(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            assert selector.select(3), 'sender fixture did not respond'
        result = self.process.stdout.readline().strip()
        assert result, 'sender fixture exited unexpectedly'
        return result

    def command(self, command):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()
        return self.read()

    def online(self, expected=True, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (self.command('state') == '1') == expected:
                return
            time.sleep(.05)
        raise AssertionError(f'online did not become {expected}')

    def close(self):
        if self.process.poll() is None:
            elapsed = int(self.command('stop'))
            assert elapsed < 1_300_000, f'shutdown took {elapsed} us'
            self.process.wait(timeout=3)
            assert self.process.returncode == 0
            assert self.process.stderr.read() == '', 'connection failures must be silent'
            return elapsed


class Broker:
    def __init__(self, port, name, auth=False, tls=None):
        self.port = port
        config = [f'listener {port} 127.0.0.1', f'user {getpass.getuser()}',
                  f'allow_anonymous {"false" if auth else "true"}', 'persistence false',
                  'log_dest stdout', 'log_type all']
        if tls is not None:
            certificate, key, ca = tls
            config += [f'certfile {certificate}', f'keyfile {key}', f'cafile {ca}']
        if auth:
            password_file = args.output / 'passwords'
            subprocess.run([str(args.source / 'apps/mosquitto_passwd/mosquitto_passwd'),
                            '-b', '-c', str(password_file), 'fixture', 'fixture-password'], check=True)
            config += [f'password_file {password_file}']
        self.config = args.output / f'{name}.conf'
        self.config.write_text('\n'.join(config) + '\n')
        self.log = open(args.output / f'{name}.log', 'w')
        self.process = subprocess.Popen([str(args.source / 'src/mosquitto'), '-c', str(self.config)],
                                        stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            assert self.process.poll() is None, f'broker failed; see {name}.log'
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.1):
                    return
            except OSError:
                time.sleep(.05)
        raise AssertionError('broker startup timed out')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=3)
        self.log.close()


def field(value):
    value = value.encode()
    return struct.pack('!H', len(value)) + value


def packet(sock, header, payload):
    length = len(payload)
    remaining = bytearray()
    while True:
        value = length % 128
        length //= 128
        remaining.append(value | (128 if length else 0))
        if not length:
            break
    sock.sendall(bytes([header]) + remaining + payload)


def receive(sock):
    def exact(length):
        data = bytearray()
        while len(data) < length:
            chunk = sock.recv(length - len(data))
            assert chunk, 'broker closed subscriber'
            data += chunk
        return bytes(data)
    header = exact(1)[0]
    length = 0
    multiplier = 1
    for _ in range(4):
        value = exact(1)[0]
        length += (value & 127) * multiplier
        if not value & 128:
            return header, exact(length)
        multiplier *= 128
    raise AssertionError('malformed MQTT length')


def subscribe(port, ca=None, username='', password=''):
    sock = socket.create_connection(('127.0.0.1', port), timeout=2)
    if ca is not None:
        context = ssl.create_default_context(cafile=str(ca))
        sock = context.wrap_socket(sock, server_hostname='localhost')
    flags = 2 | (0xc0 if username else 0)
    credentials = field(username) + field(password) if username else b''
    packet(sock, 0x10, field('MQTT') + bytes([4, flags, 0, 30]) + field('acceptance-subscriber') + credentials)
    assert receive(sock) == (0x20, b'\0\0')
    packet(sock, 0x82, b'\0\1' + field('cake-adapt/test') + b'\0')
    assert receive(sock) == (0x90, b'\0\1\0')
    return sock


def certificates():
    def openssl(*options):
        subprocess.run(['openssl', *options], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=10)
    ca = args.output / 'ca.pem'
    ca_key = args.output / 'ca.key'
    wrong_ca = args.output / 'wrong-ca.pem'
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', str(ca_key), '-out', str(ca), '-subj', '/CN=MQTT test CA')
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', str(args.output / 'wrong-ca.key'), '-out', str(wrong_ca),
            '-subj', '/CN=Untrusted MQTT test CA')
    servers = []
    for name, hostname in [('server', 'localhost'), ('wrong-host', 'other.example')]:
        key = args.output / f'{name}.key'
        csr = args.output / f'{name}.csr'
        certificate = args.output / f'{name}.pem'
        extensions = args.output / f'{name}.extensions'
        extensions.write_text(f'basicConstraints=CA:FALSE\nsubjectAltName=DNS:{hostname}\n'
                              'extendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n')
        openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                '-out', str(csr), '-subj', f'/CN={hostname}')
        openssl('x509', '-req', '-in', str(csr), '-CA', str(ca), '-CAkey', str(ca_key),
                '-CAcreateserial', '-days', '1', '-out', str(certificate), '-extfile', str(extensions))
        servers.append((certificate, key, ca))
    return ca, wrong_ca, servers


def check_delivery(driver, subscriber, record):
    driver.command('record ' + record)
    header, payload = receive(subscriber)
    assert header == 0x30, 'publish must be QoS 0 and non-retained'
    topic_size = struct.unpack('!H', payload[:2])[0]
    assert payload[2:2 + topic_size] == b'cake-adapt/test'
    assert payload[2 + topic_size:] == record.encode(), 'TLS changed the exact log record'


def run_tls_cases(resources):
    ca, wrong_ca, servers = certificates()
    port = reserve_port()
    broker = Broker(port, 'tls-trusted', auth=True, tls=servers[0])
    resources.append(broker)
    subscriber = subscribe(port, ca, 'fixture', 'fixture-password')
    resources.append(subscriber)
    good = Driver(port, 'fixture', 'fixture-password', ca=ca)
    resources.append(good)
    good.online()
    check_delivery(good, subscriber, 'TLS; exact payload')
    subscriber.close()
    evidence['cases'].append({'name': 'tls-trusted-auth-delivery', 'stop_us': good.close()})
    bad = Driver(port, 'fixture', 'fixture-password', ca=wrong_ca)
    resources.append(bad)
    time.sleep(1)
    assert bad.command('state') == '0', 'untrusted broker certificate accepted'
    evidence['cases'].append({'name': 'tls-wrong-ca', 'stop_us': bad.close()})
    broker.close()

    port = reserve_port()
    broker = Broker(port, 'tls-wrong-host', tls=servers[1])
    resources.append(broker)
    bad = Driver(port, ca=ca)
    resources.append(bad)
    time.sleep(1)
    assert bad.command('state') == '0', 'wrong broker hostname accepted'
    evidence['cases'].append({'name': 'tls-wrong-host', 'stop_us': bad.close()})
    broker.close()

    # A TLS-only client cannot silently downgrade when given a plain broker.
    port = reserve_port()
    broker = Broker(port, 'ssl-capable-plain')
    resources.append(broker)
    subscriber = subscribe(port)
    resources.append(subscriber)
    bad = Driver(port, ca=ca)
    resources.append(bad)
    time.sleep(1)
    assert bad.command('state') == '0', 'TLS client fell back to plaintext'
    bad.command('record MUST-NOT-DOWNGRADE')
    subscriber.settimeout(.3)
    try:
        receive(subscriber)
        raise AssertionError('TLS failure emitted a plaintext publish')
    except socket.timeout:
        pass
    evidence['cases'].append({'name': 'tls-no-plaintext-fallback', 'stop_us': bad.close()})
    good = Driver(port)
    resources.append(good)
    good.online()
    check_delivery(good, subscriber, 'PLAIN; SSL-capable build')
    evidence['cases'].append({'name': 'ssl-capable-plain-delivery', 'stop_us': good.close()})
    subscriber.close()
    broker.close()

    run_tls_stalled(resources, ca)


def run_tls_stalled(resources, ca):
    # A peer withholding ServerHello exercises TLS handshake timeout and retries.
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(.2)
    accepted = []
    peers = []
    closed = []
    received = []
    finished = threading.Event()
    started = time.monotonic()
    def accept_connections():
        while not finished.is_set():
            try:
                peer, _ = listener.accept()
                peer.setblocking(False)
                peers.append(peer)
                closed.append(None)
                received.append(0)
                accepted.append(time.monotonic() - started)
            except socket.timeout:
                pass
            for index, peer in enumerate(peers):
                if closed[index] is not None:
                    continue
                try:
                    data = peer.recv(4096)
                    received[index] += len(data)
                    if not data:
                        closed[index] = time.monotonic() - started
                except BlockingIOError:
                    pass
    thread = threading.Thread(target=accept_connections, daemon=True)
    thread.start()
    try:
        stalled = Driver(listener.getsockname()[1], ca=ca)
        resources.append(stalled)
        burst_us = int(stalled.command('burst'))
        assert burst_us < 200_000, 'TLS handshake blocked submission'
        deadline = time.monotonic() + 12
        while len(accepted) < 2 and time.monotonic() < deadline:
            time.sleep(.05)
        assert len(accepted) == 2, accepted
        interval = accepted[1] - accepted[0]
        assert 9.5 <= interval <= 10.8, interval
        assert stalled.command('state') == '0'
        assert closed[0] is not None, 'first TLS handshake socket was not closed'
        first_socket_lifetime = closed[0] - accepted[0]
        assert 4.5 <= first_socket_lifetime <= 5.8, first_socket_lifetime
        assert received[0] > 0, 'stalled peer did not receive a TLS handshake'
        evidence['cases'].append({'name': 'tls-stalled-handshake', 'connection_times_sec': accepted,
                                  'first_socket_close_sec': closed[0],
                                  'first_socket_lifetime_sec': first_socket_lifetime,
                                  'first_socket_handshake_bytes': received[0],
                                  'offline_burst_us': burst_us, 'stop_us': stalled.close()})
    finally:
        finished.set()
        thread.join(timeout=1)
        listener.close()
        for peer in peers:
            peer.close()


resources = []
try:
    if not (args.tls_only or args.tls_stalled_only):
        # A TCP peer that never sends CONNACK must not hold up logging or shutdown.
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(.2)
        accepted = []
        peers = []
        finished = threading.Event()
        started = time.monotonic()
        def accept_connections():
            while not finished.is_set():
                try:
                    peer, _ = listener.accept()
                    peers.append(peer)
                    accepted.append(time.monotonic() - started)
                except socket.timeout:
                    pass
        thread = threading.Thread(target=accept_connections, daemon=True)
        thread.start()
        driver = Driver(listener.getsockname()[1])
        resources.append(driver)
        burst_us = int(driver.command('burst'))
        assert burst_us < 200_000, 'offline submission blocked'
        deadline = time.monotonic() + 23
        while len(accepted) < 3 and time.monotonic() < deadline:
            time.sleep(.05)
        assert len(accepted) == 3, accepted
        intervals = [accepted[i + 1] - accepted[i] for i in range(2)]
        assert all(9.5 <= interval <= 10.8 for interval in intervals), intervals
        assert driver.command('state') == '0'
        stop_us = driver.close()
        finished.set()
        thread.join(timeout=1)
        listener.close()
        for peer in peers:
            peer.close()
        evidence['cases'].append({'name': 'silent-peer', 'connection_times_sec': accepted,
                                  'offline_burst_us': burst_us, 'stop_us': stop_us})

        # Broker outage, recovery, exact payloads, and independent logging without a file.
        port = reserve_port()
        broker = Broker(port, 'plain')
        resources.append(broker)
        subscriber = subscribe(port)
        driver = Driver(port)
        resources.append(driver)
        driver.online()
        driver.command('record DATA; exact payload')
        header, payload = receive(subscriber)
        assert header == 0x30, 'publish must be QoS 0 and non-retained'
        topic_size = struct.unpack('!H', payload[:2])[0]
        assert payload[2:2 + topic_size] == b'cake-adapt/test'
        assert payload[2 + topic_size:] == b'DATA; exact payload'
        driver.command('message MQTT-only logging')
        _, payload = receive(subscriber)
        assert b'INFO; ' in payload and payload.endswith(b'; MQTT-only logging')
        subscriber.close()
        broker.close()
        driver.online(False)
        driver.command('record MUST-NOT-REPLAY')
        burst_us = int(driver.command('burst'))
        assert burst_us < 200_000
        restart_time = time.monotonic()
        broker = Broker(port, 'recovery')
        resources.append(broker)
        subscriber = subscribe(port)
        driver.online(timeout=12)
        recovery_sec = time.monotonic() - restart_time
        assert 8.5 <= recovery_sec <= 11.5, recovery_sec
        driver.command('record RECOVERED')
        _, payload = receive(subscriber)
        assert payload.endswith(b'RECOVERED'), 'offline logs were replayed'
        subscriber.close()
        evidence['cases'].append({'name': 'delivery-and-recovery', 'recovery_sec': recovery_sec,
                                  'offline_burst_us': burst_us, 'stop_us': driver.close()})
        broker.close()

        # Authentication succeeds with the right password; failures remain silent.
        port = reserve_port()
        broker = Broker(port, 'authentication', auth=True)
        resources.append(broker)
        good = Driver(port, 'fixture', 'fixture-password')
        resources.append(good)
        good.online()
        good.close()
        bad = Driver(port, 'fixture', 'wrong-password')
        resources.append(bad)
        time.sleep(1)
        assert bad.command('state') == '0'
        evidence['cases'].append({'name': 'authentication', 'stop_us': bad.close()})
        broker.close()

    if args.tls_stalled_only:
        ca, _, _ = certificates()
        run_tls_stalled(resources, ca)
    elif args.tls or args.tls_only:
        run_tls_cases(resources)
    print('PASS: selected real MQTT and TLS acceptance cases; see results.json')
finally:
    for resource in reversed(resources):
        try:
            resource.close()
        except Exception:
            if isinstance(resource, Driver) and resource.process.poll() is None:
                resource.process.kill()
                resource.process.wait(timeout=3)
    (args.output / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
