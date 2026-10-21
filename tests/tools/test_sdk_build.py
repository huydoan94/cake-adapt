#!/usr/bin/env python3
"""Exercise SDK preflight and ordered staging with an isolated fake SDK."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2]
# Literal expectations independent of the shell staging table.
CONTRACTS = {
    'lua': ('liblua', 'base', 'utils/lua', 'lua', ['usr/include/lua.h', 'usr/include/lauxlib.h', 'usr/lib/liblua.so']),
    'libjson-c': ('libjson-c', 'base', 'libs/libjson-c', 'libjson-c', ['usr/include/json-c/json.h', 'usr/lib/pkgconfig/json-c.pc', 'usr/lib/libjson-c.so']),
    'libubox': ('libubox', 'base', 'libs/libubox', 'libubox', ['usr/include/libubox/uloop.h', 'usr/include/libubox/ustream.h', 'usr/include/libubox/utils.h', 'usr/include/libubox/blobmsg.h', 'usr/include/libubox/blobmsg_json.h', 'usr/lib/libubox.so', 'usr/lib/libblobmsg_json.so']),
    'uci': ('libuci', 'base', 'system/uci', 'uci', ['usr/include/uci.h', 'usr/lib/libuci.so']),
    'ubus': ('libubus', 'base', 'system/ubus', 'ubus', ['usr/include/libubus.h', 'usr/lib/libubus.so']),
    'libnl-tiny': ('libnl-tiny', 'base', 'libs/libnl-tiny', 'libnl-tiny', ['usr/include/libnl-tiny/netlink/netlink.h', 'usr/include/libnl-tiny/netlink/attr.h', 'usr/include/libnl-tiny/netlink/errno.h', 'usr/include/libnl-tiny/netlink/handlers.h', 'usr/include/libnl-tiny/netlink/msg.h', 'usr/include/libnl-tiny/netlink/socket.h', 'usr/lib/libnl-tiny.so']),
    'zlib': ('zlib', 'base', 'libs/zlib', 'zlib', ['usr/include/zlib.h', 'usr/include/zconf.h', 'usr/lib/pkgconfig/zlib.pc', 'usr/lib/libz.so']),
    'argp-standalone': ('argp-standalone', 'base', 'libs/argp-standalone', 'argp-standalone', ['usr/include/argp.h', 'usr/lib/libargp.a']),
    'musl-fts': ('musl-fts', 'base', 'libs/musl-fts', 'musl-fts', ['usr/include/fts.h', 'usr/lib/libfts.so']),
    'elfutils': ('libelf', 'base', 'libs/elfutils', 'elfutils', ['usr/include/libelf.h', 'usr/include/gelf.h', 'usr/lib/pkgconfig/libelf.pc', 'usr/lib/libelf.so']),
    'libbpf': ('libbpf', 'base', 'libs/libbpf', 'libbpf', ['usr/include/bpf/libbpf.h', 'usr/include/bpf/bpf.h', 'usr/include/bpf/bpf_helpers.h', 'usr/include/bpf/bpf_endian.h', 'usr/lib/libbpf.so']),
    'c-ares': ('libcares', 'packages', 'libs/c-ares', 'c-ares', ['usr/include/ares.h', 'usr/lib/libcares.so']),
    'mosquitto': ('libmosquitto-nossl', 'packages', 'net/mosquitto', 'mosquitto.nossl', ['usr/include/mosquitto.h', 'usr/lib/libmosquitto.so', 'usr/lib/libmosquitto.so.1']),
}
OPENSSL = ('libopenssl', 'base', 'libs/openssl', 'openssl', [
    'usr/include/openssl/ssl.h', 'usr/include/openssl/crypto.h',
    'usr/lib/libssl.so', 'usr/lib/libcrypto.so', 'usr/lib/pkgconfig/openssl.pc',
    'usr/lib/pkgconfig/libssl.pc', 'usr/lib/pkgconfig/libcrypto.pc'])
FAKE_TOOL = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
kind = Path(sys.argv[0]).name
sdk = Path(os.environ['TEST_SDK'])
stage = sdk / 'staging_dir/target-test'
contracts = json.loads(os.environ['TEST_CONTRACTS'])
if kind == 'pkg-config' and os.environ.get('TEST_REQUIRE_SDK_ENV'):
    expected = {'STAGING_DIR': str(stage), 'STAGING_DIR_HOST': str(sdk / 'staging_dir/host'),
                'STAGING_DIR_HOSTPKG': str(sdk / 'staging_dir/hostpkg'), 'STAGING_PREFIX': str(stage / 'usr'),
                'PKG_CONFIG_LIBDIR': str(stage / 'usr/lib/pkgconfig')}
    if any(os.environ.get(name) != value for name, value in expected.items()):
        sys.exit(1)
if kind in ('clang', 'clang-13', 'clang-12'):
    if os.environ.get('TEST_BAD_CLANG'):
        sys.exit(1)
    print('#define __clang_major__ 19')
    sys.exit(0)
if kind in ('llvm-strip', 'llvm-strip-13', 'llvm-strip-12', 'cmake', 'ninja', 'csstidy', 'luasrcdiet', 'jsmin', 'autoreconf', 'autoconf', 'automake', 'aclocal', 'autoheader', 'libtoolize', 'autom4te', 'm4', 'pkg-config', 'perl') or kind.endswith('-gcc'):
    sys.exit(1 if os.environ.get('TEST_BAD_TOOL') == kind else 0)
if kind == 'po2lmo':
    sys.exit(1)
if kind == 'readelf':
    state = Path(args[-1]).read_text()
    if state == 'broken':
        sys.exit(1)
    if '-h' in args:
        print('  Class: ELF32\n  Data: little endian\n  Type: DYN (Shared object file)')
        print('  Machine: foreign' if state == 'foreign' else '  Machine: test-target')
    else:
        if state in ('ssl', 'ssl-only'):
            print('Shared library: [libssl.so.3]')
        if state in ('ssl', 'crypto-only'):
            print('Shared library: [libcrypto.so.3]')
        print('Shared library: [libcares.so.2]')
    sys.exit(0)
with (sdk / 'calls').open('a') as output:
    output.write(json.dumps([kind, *args]) + '\n')
if kind == 'feeds':
    if os.environ.get('TEST_FAIL') == 'feeds':
        sys.exit(1)
    for source, (dependency, feed, local, stamp, artifacts) in contracts.items():
        if dependency == args[-1] or source == 'mosquitto' and args[-1] == 'libmosquitto-ssl':
            directory = sdk / 'package/feeds' / feed / source
            directory.mkdir(parents=True, exist_ok=True)
            (directory / 'Makefile').touch()
    sys.exit(0)
target = args[2]
print(f'fake-make stdout {target}')
print(f'fake-make stderr {target}', file=sys.stderr)
if os.environ.get('TEST_FAIL') and os.environ['TEST_FAIL'] in target:
    sys.exit(1)
if target.endswith('/compile'):
    source = target.split('/')[-2]
    if source in contracts:
        if os.environ.get('TEST_BAD_STAGE') == source:
            sys.exit(0)
        dependency, feed, local, stamp, artifacts = contracts[source]
        state = 'nossl'
        if source == 'mosquitto':
            state = next(value.split('=', 1)[1] for value in args if value.startswith('BUILD_VARIANT='))
            stamp = 'mosquitto.' + state
            if os.environ.get('TEST_BAD_REPAIR'):
                state = 'nossl' if state == 'ssl' else 'ssl'
        if os.environ.get('TEST_BAD_ELF') == source:
            state = 'foreign'
        for artifact in [f'stamp/.{stamp}_installed', *artifacts]:
            destination = stage / artifact
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(state)
    else:
        variant = next((value.split('=', 1)[1] for value in args if value.startswith('BUILD_VARIANT=')), None)
        package = f'{source}-{variant}' if source == 'cake-adapt' and variant != 'base' else source
        (sdk / f'bin/packages/{package}-0.3.17-r1.apk').write_text(package)
'''


class SDKBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cake-sdk-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / 'repo with spaces'
        self.sdk = self.root / 'openwrt-sdk-test'
        self.stage = self.sdk / 'staging_dir/target-test'
        self.toolchain = self.sdk / 'staging_dir/toolchain-test_musl'
        (self.repo / 'tools').mkdir(parents=True)
        for name in ('sdk-build.sh', 'sdk-resolve.sh', 'sdk-dependencies.sh'):
            shutil.copyfile(SOURCE / 'tools' / name, self.repo / 'tools' / name)
        shutil.copyfile(SOURCE / 'Makefile', self.repo / 'Makefile')
        (self.sdk / 'package').mkdir(parents=True)
        (self.sdk / 'package/cake-adapt').symlink_to(self.repo)
        for path in ('include/bpf.mk', 'rules.mk', 'calls'):
            self.write(self.sdk / path)
        self.write(self.sdk / '.config', 'CONFIG_ALL=y\nCONFIG_PACKAGE_libmosquitto-ssl=m\nCONFIG_USE_LLVM_BUILD=y\nCONFIG_USE_MUSL=y\nCONFIG_LUCI_JSMIN=y\nCONFIG_LUCI_CSSTIDY=y\nCONFIG_LUCI_SRCDIET=y\n')
        self.original_config = (self.sdk / '.config').read_bytes()
        (self.sdk / 'bin/packages').mkdir(parents=True)
        self.commands = self.root / 'commands'
        self.commands.mkdir()
        for path in (self.commands / 'make', self.commands / 'readelf', self.sdk / 'scripts/feeds',
                     self.toolchain / 'bin/test-gcc',
                     self.sdk / 'staging_dir/host/llvm-bpf/bin/clang',
                     self.sdk / 'staging_dir/host/llvm-bpf/bin/llvm-strip',
                     self.sdk / 'staging_dir/host/bin/cmake', self.sdk / 'staging_dir/host/bin/ninja'):
            self.tool(path)
        for name in ('autoreconf', 'autoconf', 'automake', 'aclocal', 'autoheader', 'libtoolize', 'autom4te', 'm4', 'pkg-config', 'perl'):
            self.tool(self.sdk / 'staging_dir/host/bin' / name)
        self.write(self.toolchain / 'lib/libc.so', 'nossl')
        for header in ('stdio.h', 'linux/types.h', 'linux/bpf.h', 'asm/types.h'):
            self.write(self.toolchain / 'include' / header)
        self.env = {**os.environ, 'PATH': f'{self.commands}:{os.environ["PATH"]}',
                    'TEST_SDK': str(self.sdk), 'TEST_CONTRACTS': json.dumps({**CONTRACTS, 'openssl': OPENSSL})}
        # The tests choose the output mode themselves.
        self.env.pop('VERBOSE', None)
        for source, (dependency, feed, local, stamp, artifacts) in CONTRACTS.items():
            self.write(self.sdk / 'feeds' / feed / local / 'Makefile')
            self.write(self.sdk / 'package/feeds' / feed / source / 'Makefile')
            self.ready(source)

    def write(self, path, content=''):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def tool(self, path):
        self.write(path, FAKE_TOOL)
        path.chmod(0o755)

    def ready(self, source, state='nossl'):
        dependency, feed, local, stamp, artifacts = OPENSSL if source == 'openssl' else CONTRACTS[source]
        if source == 'mosquitto' and state == 'ssl':
            self.write(self.stage / 'stamp/.mosquitto.ssl_installed')
        for path in [f'stamp/.{stamp}_installed', *artifacts]:
            self.write(self.stage / path, state)

    def ssl_feed(self):
        self.write(self.sdk / 'feeds/base/libs/openssl/Makefile')
        self.write(self.sdk / 'package/feeds/base/openssl/Makefile')

    def reset_calls(self):
        (self.sdk / 'calls').write_text('')

    def run_build(self, expected=0, prepare=False, variants=('mqtt',), arguments=None):
        """Build or prepare the variants; () builds the helper's default, all three."""
        args = ['sh', str(self.repo / 'tools/sdk-build.sh')]
        if prepare:
            args.append('--prepare-only')
        args.extend(f'variant={variant}' for variant in variants)
        args.extend(arguments if arguments is not None else [str(self.sdk)])
        result = subprocess.run(args, env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, expected, result.stdout)
        self.assertEqual((self.sdk / '.config').read_bytes(), self.original_config)
        self.output = result.stdout
        calls = [json.loads(line) for line in (self.sdk / 'calls').read_text().splitlines()]
        for call in calls:
            if call[0] == 'make':
                self.assertIn('NO_DEPS=1', call)
                self.assertNotIn('kernel', call[3])
        return calls

    def targets(self, calls):
        return [call[3] for call in calls if call[0] == 'make']

    def app_calls(self, calls):
        return [call for call in calls if call[0] == 'make' and call[3].startswith('package/cake-adapt/')]

    def test_ready_dependencies_skip_preparation(self):
        calls = self.run_build()
        self.assertEqual(self.targets(calls), ['package/cake-adapt/clean', 'package/cake-adapt/compile'])
        for call in calls:
            self.assertIn('BUILD_VARIANT=mqtt', call)
            self.assertIn('CONFIG_PACKAGE_cake-adapt-mqtt=m', call)
        self.assertEqual((self.repo / 'bin/cake-adapt-mqtt-0.3.17-r1-test.apk').read_text(), 'cake-adapt-mqtt')
        self.assertTrue((self.repo / 'build/sdk-test-mqtt.log').is_file())

    def test_default_builds_every_variant_in_order_and_luci_once(self):
        self.ssl_feed()
        self.ready('openssl')
        self.link_luci()
        calls = self.run_build(variants=())
        self.assertEqual(self.targets(calls), [
            'package/cake-adapt/clean', 'package/cake-adapt/compile',
            'package/cake-adapt/clean', 'package/cake-adapt/compile',
            'package/feeds/packages/mosquitto/clean', 'package/feeds/packages/mosquitto/compile',
            'package/cake-adapt/clean', 'package/cake-adapt/compile',
            'package/luci-app-cake-adapt/clean', 'package/luci-app-cake-adapt/compile'])
        apps = self.app_calls(calls)
        for calls_of_variant, (variant, package) in zip((apps[0:2], apps[2:4], apps[4:6]), (
                ('base', 'cake-adapt'), ('mqtt', 'cake-adapt-mqtt'), ('mqtt-ssl', 'cake-adapt-mqtt-ssl'))):
            for call in calls_of_variant:
                self.assertIn(f'BUILD_VARIANT={variant}', call)
                self.assertIn(f'CONFIG_PACKAGE_{package}=m', call)
            self.assertEqual((self.repo / f'bin/{package}-0.3.17-r1-test.apk').read_text(), package)
            self.assertTrue((self.repo / f'build/sdk-test-{variant}.log').is_file())
        self.assertIn('BUILD_VARIANT=ssl', calls[4])
        for call in calls[-2:]:
            self.assertFalse(any(value.startswith('BUILD_VARIANT=') for value in call))
        self.assertEqual((self.repo / 'bin/luci-app-cake-adapt-0.3.17-r1.apk').read_text(), 'luci-app-cake-adapt')
        self.assertTrue((self.repo / 'build/sdk-test-luci.log').is_file())

    def test_variant_all_is_the_default(self):
        self.ssl_feed()
        self.ready('openssl')
        # Dependency staging depends on what the previous build left, so compare
        # only the package builds: the same variants, in the same order.
        def built(calls):
            return [value for call in self.app_calls(calls) for value in call if value.startswith('BUILD_VARIANT=')]
        default = built(self.run_build(variants=()))
        self.reset_calls()
        self.assertEqual(built(self.run_build(variants=('all',))), default)
        self.assertEqual(default[::2], ['BUILD_VARIANT=base', 'BUILD_VARIANT=mqtt', 'BUILD_VARIANT=mqtt-ssl'])

    def test_base_variant_needs_no_mqtt_library_or_feed(self):
        for source in ('c-ares', 'mosquitto'):
            _, feed, local, stamp, artifacts = CONTRACTS[source]
            shutil.rmtree(self.sdk / 'package/feeds' / feed / source)
            shutil.rmtree(self.sdk / 'feeds' / feed / local)
            for artifact in [f'stamp/.{stamp}_installed', *artifacts]:
                (self.stage / artifact).unlink()
        calls = self.run_build(variants=('base',))
        self.assertEqual(self.targets(calls), ['package/cake-adapt/clean', 'package/cake-adapt/compile'])
        for call in calls:
            self.assertIn('BUILD_VARIANT=base', call)
            self.assertIn('CONFIG_PACKAGE_cake-adapt=m', call)
        self.assertEqual((self.repo / 'bin/cake-adapt-0.3.17-r1-test.apk').read_text(), 'cake-adapt')
        self.assertNotIn('libmosquitto', (self.repo / 'build/sdk-test-base.contracts').read_text())
        self.reset_calls()
        self.assertEqual(self.run_build(expected=3, prepare=True, variants=('mqtt',)), [])

    def test_failed_variant_stops_the_later_ones(self):
        self.env['TEST_FAIL'] = 'cake-adapt/compile'
        calls = self.run_build(expected=1, variants=())
        self.assertEqual(self.targets(calls), ['package/cake-adapt/clean', 'package/cake-adapt/compile'])
        self.assertIn('BUILD_VARIANT=base', calls[-1])
        self.assertFalse((self.repo / 'bin').exists())

    def test_quiet_default_keeps_make_output_in_the_log(self):
        (self.stage / 'usr/include/ares.h').unlink()
        self.link_luci()
        self.run_build()
        for line in ('libcares: stage', 'Preflight complete', 'Staging libcares', 'BUILT (test, cake-adapt-mqtt)',
                     'Building luci-app-cake-adapt'):
            self.assertIn(line, self.output)
        self.assertNotIn('fake-make', self.output)
        log = (self.repo / 'build/sdk-test-mqtt.log').read_text()
        for target in ('package/feeds/packages/c-ares/compile', 'package/cake-adapt/compile'):
            self.assertIn(f'fake-make stdout {target}', log)
            self.assertIn(f'fake-make stderr {target}', log)
        self.assertIn('Preflight complete', log)
        self.assertIn('fake-make stdout package/luci-app-cake-adapt/compile',
                      (self.repo / 'build/sdk-test-luci.log').read_text())

    def test_verbose_shows_make_output(self):
        self.env['VERBOSE'] = '1'
        self.run_build()
        self.assertIn('fake-make stdout package/cake-adapt/compile', self.output)
        self.assertIn('fake-make stderr package/cake-adapt/compile', self.output)
        self.assertIn('fake-make stdout package/cake-adapt/compile',
                      (self.repo / 'build/sdk-test-mqtt.log').read_text())

    def test_failed_make_shows_the_end_of_the_log(self):
        self.env['TEST_FAIL'] = 'cake-adapt/compile'
        self.run_build(expected=1)
        self.assertIn('Last 30 lines of', self.output)
        self.assertIn('fake-make stdout package/cake-adapt/compile', self.output)
        self.assertIn('the log shows no compiler error', self.output)
        self.env['VERBOSE'] = '1'
        self.run_build(expected=1)
        self.assertNotIn('Last 30 lines of', self.output)

    def test_declared_mqtt_dependency_must_match_the_variant(self):
        p = self.repo / 'Makefile'
        original = p.read_text()
        for variant, broken in (('mqtt', original.replace('+libmosquitto-nossl', '+libmosquitto-ssl')),
                                ('mqtt-ssl', original.replace(' +ca-bundle', '')),
                                ('base', original.replace('+fping', '+libmosquitto-nossl +fping'))):
            with self.subTest(variant=variant):
                p.write_text(broken)
                self.assertEqual(self.run_build(expected=3, prepare=True, variants=(variant,)), [])
                self.assertIn('but its variant needs', self.output)
        p.write_text(original)

    def test_cli_options_in_either_order(self):
        self.ssl_feed()
        self.ready('openssl')
        self.ready('mosquitto', 'ssl')
        for arguments in ([str(self.sdk), 'variant=mqtt-ssl', '--prepare-only'],
                          ['--prepare-only', 'variant=mqtt-ssl', str(self.sdk)],
                          ['variant=mqtt-ssl', str(self.sdk), '--prepare-only']):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.run_build(variants=(), arguments=arguments), [])
                self.assertIn('READY (test, cake-adapt-mqtt-ssl)', self.output)
        self.run_build(variants=(), arguments=['variant=mqtt-ssl', 'variant=base', '--prepare-only', str(self.sdk)])
        self.assertLess(self.output.index('READY (test, cake-adapt)'), self.output.index('READY (test, cake-adapt-mqtt-ssl)'))

    def test_invalid_or_duplicate_cli_options_do_not_mutate_sdk(self):
        for arguments in ([], ['ssl=yes', str(self.sdk)], ['ssl=no', str(self.sdk)], ['variant=', str(self.sdk)],
                          ['variant=ssl', str(self.sdk)], ['variant=all', 'variant=mqtt', str(self.sdk)],
                          ['variant=mqtt', 'variant=all', str(self.sdk)], ['variant=all', 'variant=all', str(self.sdk)],
                          ['variant=mqtt', 'variant=mqtt', str(self.sdk)],
                          ['--prepare-only', '--prepare-only', str(self.sdk)],
                          ['--variant', str(self.sdk)], ['other=yes', str(self.sdk)],
                          [str(self.sdk), str(self.sdk)]):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.run_build(expected=1, variants=(), arguments=arguments), [])
                self.assertFalse((self.repo / 'build').exists())

    def test_base_and_mqtt_need_no_openssl_feed(self):
        self.assertFalse((self.sdk / 'feeds/base/libs/openssl').exists())
        for variants in (('base',), ('mqtt',), ('base', 'mqtt')):
            with self.subTest(variants=variants):
                self.assertEqual(self.run_build(prepare=True, variants=variants), [])

    def test_ssl_ready_stage_skips_variant_repair_and_labels_copy(self):
        self.ssl_feed()
        self.ready('openssl')
        self.ready('mosquitto', 'ssl')
        self.link_luci()
        calls = self.run_build(variants=('mqtt-ssl',))
        self.assertEqual(self.targets(calls), ['package/cake-adapt/clean', 'package/cake-adapt/compile',
                         'package/luci-app-cake-adapt/clean', 'package/luci-app-cake-adapt/compile'])
        self.assertTrue(all('BUILD_VARIANT=mqtt-ssl' in call for call in calls[:2]))
        self.assertTrue((self.repo / 'bin/cake-adapt-mqtt-ssl-0.3.17-r1-test.apk').is_file())
        self.assertTrue((self.repo / 'bin/luci-app-cake-adapt-0.3.17-r1.apk').is_file())
        self.assertTrue((self.repo / 'build/sdk-test-mqtt-ssl.log').is_file())

    def test_native_make_rebuilds_when_mqtt_capability_changes(self):
        native = self.root / 'native'
        self.write(native / 'logging/log.c', '''#if MQTT_SUPPORTED
int mqtt_enabled_build;
#endif
#if MQTT_TLS_SUPPORTED
int tls_enabled;
#else
int tls_disabled;
#endif
''')
        shutil.copyfile(SOURCE / 'src/Makefile', native / 'Makefile')
        object_dir = native / 'build'
        obj = object_dir / 'logging/log.o'
        command = ['make', '--no-print-directory', '-C', str(native), str(obj),
                   f'OBJECT_DIR={object_dir}', 'NETLINK_CFLAGS=', 'LIBBPF_CFLAGS=']
        for mqtt, tls in (('1', '0'), ('1', '1'), ('0', '0'), ('1', '0')):
            result = subprocess.run([*command, f'MQTT_SUPPORTED={mqtt}', f'MQTT_TLS_SUPPORTED={tls}'],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.check_output(['nm', str(obj)], text=True)
            self.assertEqual('mqtt_enabled_build' in symbols, mqtt == '1')
            self.assertEqual('tls_enabled' in symbols, tls == '1')
            self.assertEqual('tls_disabled' in symbols, tls == '0')
        unchanged = obj.stat().st_mtime_ns
        subprocess.run([*command, 'MQTT_SUPPORTED=1', 'MQTT_TLS_SUPPORTED=0'], check=True, capture_output=True)
        self.assertEqual(obj.stat().st_mtime_ns, unchanged)

    def test_native_make_links_libmosquitto_only_with_mqtt(self):
        native = SOURCE / 'src'
        dry = ['make', '--no-print-directory', '-n', '-B', '-C', str(native), 'cake-adapt',
               f'OBJECT_DIR={self.root / "native-build"}', 'NETLINK_CFLAGS=', 'LIBBPF_CFLAGS=']
        for mqtt, tls, source in (('0', '0', 'mqtt_none.c'), ('1', '0', 'mqtt.c'), ('1', '1', 'mqtt.c')):
            with self.subTest(mqtt=mqtt, tls=tls):
                result = subprocess.run([*dry, f'MQTT_SUPPORTED={mqtt}', f'MQTT_TLS_SUPPORTED={tls}'],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                compiled = [line for line in result.stdout.splitlines() if ' -c -o ' in line and '/logging/mqtt' in line]
                self.assertEqual(len(compiled), 1)
                self.assertTrue(compiled[0].endswith('logging/' + source))
                self.assertIn(f'-DMQTT_SUPPORTED={mqtt} -DMQTT_TLS_SUPPORTED={tls}', compiled[0])
                link = [line for line in result.stdout.splitlines() if ' -o cake-adapt ' in line]
                self.assertEqual(len(link), 1)
                self.assertEqual('-lmosquitto' in link[0], mqtt == '1')
        for mqtt, tls in (('0', '1'), ('2', '0'), ('1', ''), ('yes', 'no')):
            with self.subTest(mqtt=mqtt, tls=tls):
                result = subprocess.run([*dry, f'MQTT_SUPPORTED={mqtt}', f'MQTT_TLS_SUPPORTED={tls}'],
                                        text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('must be 0/0, 1/0 or 1/1', result.stderr)

    def test_luci_package_reads_capabilities_at_run_time(self):
        recipe = (SOURCE / 'luci-app-cake-adapt/Makefile').read_text()
        self.assertNotIn('ssl', recipe)
        self.assertNotIn('Prepare', recipe)
        self.assertIn('+cake-adapt', recipe)
        acl = json.loads((SOURCE / 'luci-app-cake-adapt/root/usr/share/rpcd/acl.d/luci-app-cake-adapt.json').read_text())
        read = acl['luci-app-cake-adapt']['read']
        self.assertEqual(read['file'], {'/usr/share/cake-adapt/features': ['read']})
        self.assertIn('read', read['ubus']['file'])

    def test_ssl_stages_openssl_before_mosquitto_and_switches_back(self):
        self.ssl_feed()
        calls = self.run_build(variants=('mqtt-ssl',), prepare=True)
        self.assertEqual(self.targets(calls), ['package/feeds/base/openssl/clean',
                         'package/feeds/base/openssl/compile', 'package/feeds/packages/mosquitto/clean',
                         'package/feeds/packages/mosquitto/compile'])
        for call in calls[:2]:
            self.assertIn('CONFIG_PACKAGE_libopenssl=m', call)
            for name in ('libopenssl-afalg', 'libopenssl-devcrypto', 'libopenssl-padlock',
                         'libopenssl-legacy', 'libopenssl-conf', 'openssl-util'):
                self.assertIn(f'CONFIG_PACKAGE_{name}=', call)
            for engine in ('AFALG', 'DEVCRYPTO', 'PADLOCK'):
                self.assertIn(f'CONFIG_OPENSSL_ENGINE_BUILTIN_{engine}=', call)
            self.assertIn('CONFIG_OPENSSL_ENGINE_BUILTIN=', call)
            self.assertIn('CONFIG_ALL=', call)
        for call in calls[2:]:
            self.assertIn('BUILD_VARIANT=ssl', call)
            self.assertIn('CONFIG_PACKAGE_libmosquitto-ssl=m', call)
            self.assertIn('CONFIG_PACKAGE_libmosquitto-nossl=', call)
        self.reset_calls()
        self.assertEqual(self.targets(self.run_build(variants=('mqtt',), prepare=True)),
                         ['package/feeds/packages/mosquitto/clean', 'package/feeds/packages/mosquitto/compile'])

    def test_ssl_requires_both_crypto_and_ssl_dynamic_dependencies(self):
        self.ssl_feed()
        self.ready('openssl')
        for state in ('nossl', 'ssl-only', 'crypto-only', 'broken', 'foreign'):
            with self.subTest(state=state):
                self.reset_calls()
                self.ready('mosquitto', 'ssl')
                self.write(self.stage / 'usr/lib/libmosquitto.so.1', state)
                self.assertEqual(self.targets(self.run_build(variants=('mqtt-ssl',), prepare=True)),
                                 ['package/feeds/packages/mosquitto/clean', 'package/feeds/packages/mosquitto/compile'])

    def test_ssl_openssl_artifacts_and_stamp_are_repaired(self):
        self.ssl_feed()
        self.ready('openssl')
        self.ready('mosquitto', 'ssl')
        for artifact in ['stamp/.openssl_installed', *OPENSSL[-1]]:
            with self.subTest(artifact=artifact):
                self.reset_calls()
                (self.stage / artifact).unlink()
                self.assertEqual(self.targets(self.run_build(variants=('mqtt-ssl',), prepare=True)),
                                 ['package/feeds/base/openssl/clean', 'package/feeds/base/openssl/compile'])

    def test_ssl_failed_openssl_or_variant_repair_blocks_app(self):
        self.ssl_feed()
        for failure in ('openssl-header', 'openssl-elf', 'mosquitto-variant'):
            with self.subTest(failure=failure):
                self.reset_calls()
                self.ready('openssl')
                self.ready('mosquitto')
                self.env.pop('TEST_BAD_STAGE', None)
                self.env.pop('TEST_BAD_ELF', None)
                self.env.pop('TEST_BAD_REPAIR', None)
                if failure.startswith('openssl'):
                    (self.stage / 'usr/include/openssl/ssl.h').unlink()
                    self.env['TEST_BAD_STAGE' if failure.endswith('header') else 'TEST_BAD_ELF'] = 'openssl'
                else:
                    self.env['TEST_BAD_REPAIR'] = '1'
                calls = self.run_build(expected=3, variants=('mqtt-ssl',))
                self.assertFalse(any('/cake-adapt/' in target for target in self.targets(calls)))
                self.assertFalse((self.repo / 'bin').exists())

    def test_ssl_late_missing_feed_or_unusable_perl_blocks_all_preparation(self):
        (self.stage / 'usr/include/lua.h').unlink()
        self.assertEqual(self.run_build(expected=3, variants=('mqtt-ssl',)), [])
        self.ssl_feed()
        self.env['TEST_BAD_TOOL'] = 'perl'
        self.assertEqual(self.run_build(expected=2, variants=('mqtt-ssl',)), [])

    def test_sdk_tool_wrappers_receive_package_staging_environment(self):
        self.ssl_feed()
        self.env['TEST_REQUIRE_SDK_ENV'] = '1'
        calls = self.run_build(variants=('mqtt-ssl',), prepare=True)
        self.assertEqual(self.targets(calls), ['package/feeds/base/openssl/clean',
                         'package/feeds/base/openssl/compile', 'package/feeds/packages/mosquitto/clean',
                         'package/feeds/packages/mosquitto/compile'])

    def test_ssl_libatomic_required_only_for_affected_architecture(self):
        self.ssl_feed()
        self.ready('openssl')
        self.ready('mosquitto', 'ssl')
        config = self.sdk / '.config'
        config.write_text(config.read_text() + 'CONFIG_arm=y\n')
        self.original_config = config.read_bytes()
        self.assertEqual(self.run_build(expected=2, variants=('mqtt-ssl',), prepare=True), [])
        self.write(self.toolchain / 'lib/libatomic.so.1', 'foreign')
        self.assertEqual(self.run_build(expected=2, variants=('mqtt-ssl',), prepare=True), [])
        self.write(self.toolchain / 'lib/libatomic.so.1', 'nossl')
        self.assertEqual(self.run_build(variants=('mqtt-ssl',), prepare=True), [])

    def test_package_variants_declare_their_dependencies_and_capabilities(self):
        self.write(self.sdk / 'rules.mk', 'INCLUDE_DIR:=$(TOPDIR)/include\n')
        # Each BuildPackage records what its Package/<name> sets, as package-defaults.mk resets it first.
        self.write(self.sdk / 'include/package.mk', '''define BuildPackage
$(eval VARIANT:=)$(eval PROVIDES:=)$(eval DEFAULT_VARIANT:=)$(eval $(Package/$(1)))
INFO += $(1)|$(VARIANT)|$(DEFAULT_VARIANT)|$(strip $(PROVIDES))|$(strip $(DEPENDS));
endef
.PHONY: print
print:
\t@printf '%s\\n' '$(INFO)' | tr ';' '\\n' | sed 's/^ *//'
\t@printf 'build=%s/%s features=%s\\n' '$(MQTT_SUPPORTED)' '$(MQTT_TLS_SUPPORTED)' '$(strip $(FEATURES))'
''')
        common = '+libuci +libubox +libubus +libnl-tiny +libbpf +zlib +fping'
        expected = [f'cake-adapt|base|1||{common}',
                    f'cake-adapt-mqtt|mqtt||cake-adapt|{common} +libmosquitto-nossl',
                    f'cake-adapt-mqtt-ssl|mqtt-ssl||cake-adapt|{common} +libmosquitto-ssl +ca-bundle']
        for variant, build, features in (('base', '0/0', ''), ('mqtt', '1/0', 'mqtt'), ('mqtt-ssl', '1/1', 'mqtt tls')):
            with self.subTest(variant=variant):
                result = subprocess.run(['make', '--no-print-directory', '-f', str(self.repo / 'Makefile'),
                                         f'TOPDIR={self.sdk}', 'print', f'BUILD_VARIANT={variant}'],
                                        cwd=self.repo, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = [line for line in result.stdout.splitlines() if line]
                self.assertEqual(lines[:3], expected)
                self.assertEqual(lines[3], f'build={build} features={features}')

    def test_each_direct_library_stamp_header_and_linker_repaired(self):
        for source in ('uci', 'libubox', 'ubus', 'libnl-tiny', 'libbpf', 'zlib', 'mosquitto'):
            dependency, feed, local, stamp, artifacts = CONTRACTS[source]
            for artifact in [f'stamp/.{stamp}_installed', artifacts[0], artifacts[-1]]:
                with self.subTest(source=source, artifact=artifact):
                    self.reset_calls()
                    (self.stage / artifact).unlink()
                    self.assertEqual(self.targets(self.run_build(prepare=True)),
                                     [f'package/feeds/{feed}/{source}/clean', f'package/feeds/{feed}/{source}/compile'])

    def test_all_repairs_follow_transitive_order(self):
        for source, (_, _, _, stamp, _) in CONTRACTS.items():
            (self.stage / f'stamp/.{stamp}_installed').unlink()
        calls = self.run_build(prepare=True)
        self.assertEqual(self.targets(calls), [f'package/feeds/{feed}/{source}/{action}'
                         for source, (_, feed, _, _, _) in CONTRACTS.items()
                         for action in ('clean', 'compile')])
        for call in calls:
            self.assertIn('CONFIG_ALL=', call)
        self.assertFalse((self.repo / 'bin').exists())

    def test_missing_required_pkg_config_metadata_is_repaired(self):
        for source, metadata in (('libjson-c', 'json-c.pc'), ('zlib', 'zlib.pc'), ('elfutils', 'libelf.pc')):
            with self.subTest(source=source):
                self.reset_calls()
                (self.stage / 'usr/lib/pkgconfig' / metadata).unlink()
                self.assertEqual(self.targets(self.run_build(prepare=True)),
                                 [f'package/feeds/base/{source}/clean', f'package/feeds/base/{source}/compile'])

    def test_invalid_or_foreign_staged_shared_library_is_repaired(self):
        for source in ('uci', 'libubox', 'ubus', 'libnl-tiny', 'libbpf', 'zlib', 'c-ares'):
            _, feed, _, _, artifacts = CONTRACTS[source]
            for state in ('broken', 'foreign'):
                with self.subTest(source=source, state=state):
                    self.reset_calls()
                    self.write(self.stage / artifacts[-1], state)
                    self.assertEqual(self.targets(self.run_build(prepare=True)),
                                     [f'package/feeds/{feed}/{source}/clean', f'package/feeds/{feed}/{source}/compile'])

    def test_late_missing_autotools_or_pkg_config_prevents_early_repair(self):
        for name, late_source in (('autoreconf', 'elfutils'), ('autom4te', 'musl-fts'), ('pkg-config', 'libbpf')):
            with self.subTest(name=name):
                self.reset_calls()
                (self.stage / 'usr/include/lua.h').unlink(missing_ok=True)
                (self.stage / CONTRACTS[late_source][-1][0]).unlink(missing_ok=True)
                tool = self.sdk / 'staging_dir/host/bin' / name
                tool.unlink()
                try:
                    self.assertEqual(self.run_build(expected=2), [])
                finally:
                    self.tool(tool)
                    self.ready(late_source)
                    self.ready('lua')

    def test_ssl_overwrite_with_existing_stamp_is_repaired_first(self):
        self.ready('mosquitto', 'ssl')
        calls = self.run_build()
        self.assertEqual(self.targets(calls), ['package/feeds/packages/mosquitto/clean',
                         'package/feeds/packages/mosquitto/compile',
                         'package/cake-adapt/clean', 'package/cake-adapt/compile'])
        for call in calls[:2]:
            for selection in ('BUILD_VARIANT=nossl', 'CONFIG_PACKAGE_libmosquitto-nossl=m',
                              'CONFIG_PACKAGE_libmosquitto-ssl=', 'CONFIG_PACKAGE_libmosquittopp=', 'CONFIG_MOSQUITTO_LWS='):
                self.assertIn(selection, call)

    def test_missing_feed_links_registered_locally(self):
        shutil.rmtree(self.sdk / 'package/feeds')
        calls = self.run_build(prepare=True)
        self.assertEqual(calls, [['feeds', 'install', '-p', feed, dependency]
                         for dependency, feed, _, _, _ in CONTRACTS.values()])
        self.assertFalse((self.repo / 'bin').exists())

    def test_registered_root_package_target_is_used(self):
        shutil.move(self.sdk / 'package/feeds/base/uci', self.sdk / 'package/uci')
        (self.stage / 'usr/include/uci.h').unlink()
        self.assertEqual(self.targets(self.run_build(prepare=True)), ['package/uci/clean', 'package/uci/compile'])

    def test_late_missing_feed_stops_before_any_make_or_registration(self):
        (self.stage / 'usr/include/lua.h').unlink()
        shutil.rmtree(self.sdk / 'package/feeds')
        shutil.rmtree(self.sdk / 'feeds/packages/net/mosquitto')
        self.assertEqual(self.run_build(expected=3), [])

    def test_unknown_direct_dependency_stops_before_any_make(self):
        p = self.repo / 'Makefile'
        p.write_text(p.read_text().replace('+fping', '+newlibrary +fping'))
        self.assertEqual(self.run_build(expected=3), [])
        self.assertIn('unrepresented cake-adapt dependency: +newlibrary', self.output)

    def test_unknown_build_dependency_stops_before_any_make(self):
        p = self.repo / 'Makefile'
        p.write_text(p.read_text() + '\nPKG_BUILD_DEPENDS += newlibrary/host\n')
        self.assertEqual(self.run_build(expected=3), [])
        self.assertIn('unrepresented cake-adapt build dependency: newlibrary/host', self.output)

    def test_represented_build_dependency_uses_existing_contract(self):
        p = self.repo / 'Makefile'
        p.write_text(p.read_text() + '\nPKG_BUILD_DEPENDS := libubox\n')
        self.assertEqual(self.run_build(prepare=True), [])

    def test_missing_or_unusable_tools_stop_before_any_make(self):
        for path in (self.toolchain / 'bin/test-gcc',
                     self.sdk / 'staging_dir/host/llvm-bpf/bin/clang',
                     self.sdk / 'staging_dir/host/llvm-bpf/bin/llvm-strip',
                     self.toolchain / 'include/linux/bpf.h',
                     self.sdk / 'staging_dir/host/bin/cmake',
                     self.sdk / 'staging_dir/host/bin/ninja'):
            with self.subTest(path=path):
                self.reset_calls()
                (self.stage / 'usr/include/mosquitto.h').unlink(missing_ok=True)
                saved = path.read_bytes()
                path.unlink()
                try:
                    self.assertEqual(self.run_build(expected=2), [])
                finally:
                    path.write_bytes(saved)
                    path.chmod(0o755)
        self.env['TEST_BAD_CLANG'] = '1'
        self.assertEqual(self.run_build(expected=2), [])

    def test_gcc_aliases_of_one_binary_are_accepted(self):
        (self.toolchain / 'bin/other-gcc').symlink_to('test-gcc')
        self.assertEqual(self.run_build(prepare=True), [])

    def test_prebuilt_and_host_llvm_selection(self):
        config = self.sdk / '.config'
        self.tool(self.sdk / 'llvm-bpf/bin/clang')
        self.tool(self.sdk / 'llvm-bpf/bin/llvm-strip')
        config.write_text(config.read_text().replace('CONFIG_USE_LLVM_BUILD=y', 'CONFIG_USE_LLVM_PREBUILT=y'))
        self.original_config = config.read_bytes()
        self.assertEqual(self.run_build(prepare=True), [])
        llvm_host = self.root / 'host llvm'
        self.tool(llvm_host / 'bin/clang')
        self.tool(llvm_host / 'bin/llvm-strip')
        config.write_text(config.read_text().replace('CONFIG_USE_LLVM_PREBUILT=y',
                          f'CONFIG_USE_LLVM_HOST=y\nCONFIG_BPF_TOOLCHAIN_HOST_PATH="{llvm_host}"'))
        self.original_config = config.read_bytes()
        self.assertEqual(self.run_build(prepare=True), [])

    def test_dependency_failure_prevents_app_build_and_copy(self):
        (self.stage / 'usr/include/ares.h').unlink()
        self.env['TEST_FAIL'] = 'c-ares'
        self.assertEqual(self.targets(self.run_build(expected=3)), ['package/feeds/packages/c-ares/clean'])
        self.assertFalse((self.repo / 'bin').exists())

    def test_failed_or_incorrect_mosquitto_repair_prevents_app_build(self):
        for failure in ('mosquitto', 'bad-library'):
            with self.subTest(failure=failure):
                self.ready('mosquitto', 'broken')
                self.reset_calls()
                self.env.pop('TEST_FAIL', None)
                self.env.pop('TEST_BAD_REPAIR', None)
                self.env['TEST_FAIL' if failure == 'mosquitto' else 'TEST_BAD_REPAIR'] = failure
                self.assertTrue(all('/mosquitto/' in target for target in self.targets(self.run_build(expected=3))))
                self.assertFalse((self.repo / 'bin').exists())

    def test_successful_make_without_staging_stops_preparation(self):
        (self.stage / 'usr/include/uci.h').unlink()
        self.env['TEST_BAD_STAGE'] = 'uci'
        self.assertEqual(self.targets(self.run_build(expected=3, prepare=True)),
                         ['package/feeds/base/uci/clean', 'package/feeds/base/uci/compile'])

    def test_feed_registration_failure_stops_preparation(self):
        shutil.rmtree(self.sdk / 'package/feeds')
        self.env['TEST_FAIL'] = 'feeds'
        self.assertEqual(self.targets(self.run_build(expected=3)), [])

    def link_luci(self):
        (self.repo / 'luci-app-cake-adapt').mkdir()
        (self.sdk / 'package/luci-app-cake-adapt').symlink_to(self.repo / 'luci-app-cake-adapt')
        self.write(self.sdk / 'feeds/luci/luci.mk', 'LUCI_LANG.fr=French\n')

    def test_luci_unused_host_tools_not_required(self):
        self.link_luci()
        self.assertEqual(self.targets(self.run_build()), ['package/cake-adapt/clean', 'package/cake-adapt/compile',
                         'package/luci-app-cake-adapt/clean', 'package/luci-app-cake-adapt/compile'])

    def test_luci_optional_minifiers_warn_for_applicable_assets_only(self):
        self.link_luci()
        for asset, tool in (('htdocs/app.js', 'jsmin'), ('htdocs/app.css', 'csstidy'), ('luasrc/app.lua', 'luasrcdiet')):
            with self.subTest(asset=asset):
                self.reset_calls()
                self.write(self.repo / 'luci-app-cake-adapt' / asset)
                self.run_build(prepare=True)
                self.assertIn(f'LuCI {tool} is unavailable', self.output)
                self.tool(self.sdk / 'staging_dir/hostpkg/bin' / tool)
                self.run_build(prepare=True)
                self.assertNotIn(f'LuCI {tool} is unavailable', self.output)

    def test_luci_translations_require_real_po2lmo_before_any_make(self):
        self.link_luci()
        self.write(self.repo / 'luci-app-cake-adapt/po/fr/app.po')
        self.assertEqual(self.run_build(expected=2), [])
        self.assertIn('package/feeds/luci/luci-base/host/compile', self.output)
        self.tool(self.sdk / 'staging_dir/hostpkg/bin/po2lmo')
        self.assertEqual(self.run_build(prepare=True), [])

    def test_luci_unselected_translations_need_no_host_tool(self):
        self.link_luci()
        self.write(self.repo / 'luci-app-cake-adapt/po/fr/app.po')
        config = self.sdk / '.config'
        config.write_text(config.read_text().replace('CONFIG_ALL=y', '# CONFIG_ALL is not set'))
        self.original_config = config.read_bytes()
        self.assertEqual(self.run_build(prepare=True), [])

    def test_glibc_does_not_require_musl_only_libraries(self):
        for source in ('argp-standalone', 'musl-fts'):
            _, feed, local, _, _ = CONTRACTS[source]
            shutil.rmtree(self.sdk / 'package/feeds' / feed / source)
            shutil.rmtree(self.sdk / 'feeds' / feed / local)
        renamed = self.toolchain.with_name('toolchain-test_glibc')
        self.toolchain.rename(renamed)
        config = self.sdk / '.config'
        config.write_text(config.read_text().replace('CONFIG_USE_MUSL=y', 'CONFIG_USE_GLIBC=y'))
        self.original_config = config.read_bytes()
        self.assertEqual(self.run_build(prepare=True), [])


if __name__ == '__main__':
    unittest.main()
