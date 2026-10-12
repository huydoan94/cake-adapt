# Optional non-SSL MQTT log forwarding

Current revision, 2026-10-12: the user chose `libmosquitto-nossl` to remove the
MQTT OpenSSL dependency. TLS and CA-file options are removed from UCI, LuCI and
the sender. Username/password authentication remains supported over plain TCP;
traffic and credentials are unencrypted. MQTT remains disabled by default.

The logging path still uses a try-lock and a bounded 32-record queue. Sender and
network threads own broker operations. Offline, contended, full and oversized
submissions are silently dropped. QoS 0, no retention, exact log schemas and
10-second retries without a retry limit are unchanged.

## Current validation

- Config, LuCI, logging, defaults and queue checks passed. Queue tests also passed
  AddressSanitizer/UndefinedBehaviorSanitizer outside the ptrace-restricted
  sandbox. The x86 editor command resolves the staged headers and passes strict
  syntax checks.
- Actual non-SSL library/broker acceptance passed exact topic/payload delivery,
  independent logging with file output disabled, silent offline loss, no replay,
  a peer withholding CONNACK, recovery, credentials and bounded shutdown.
  Connections to the silent peer occurred at 0.003, 10.018 and 20.033 seconds;
  recovery took 10.024 seconds. Offline 10,000-record bursts returned in 16–29 µs
  on this host; this measures submission only. Maximum observed shutdown was
  about 102 ms. These are not router resource-cost claims.
- The driver's actual process mappings verified the exact hashed non-SSL library.
  Its native dynamic dependency is libc; the SDK library depends on c-ares,
  libgcc and libc. Neither library links OpenSSL.
- The daemon and LuCI packages built in the x86 SDK at 0.3.17-r1, alongside
  `libmosquitto-nossl` 2.0.22 and c-ares 1.34.6. All builds used `NO_DEPS=1` with
  cached source archives; no kernel targets or new downloads were needed. APK
  metadata names `libmosquitto-nossl` and has no MQTT CA/OpenSSL dependency.
- The SDK build helper requires dependencies already staged and uses `NO_DEPS=1`
  for both packages, including its diagnostic retry command. It rejected the
  actual older SSL library in an isolated preflight before any build or copy;
  this prevents an SDK's selected SSL variants from silently reintroducing the
  earlier recursive OpenSSL/kernel work.
- On the authorized OpenWrt 25.12 x86 VM, the daemon and libraries extracted from
  those APKs ran from an isolated `/tmp` directory without replacing the installed
  service. Both rate-adjustment flags were off. Ten MQTT records matched complete
  file-log lines, including a periodic `MEMORY` record; the daemon stayed alive
  during broker outage. Its process and owned fping child exited, test files and
  lock were removed, the original service remained running, both CAKE settings
  stayed unchanged and `/tmp/sqm-mon-test.log` retained inode 179.
- A separate bounded reviewer checked the non-SSL source/configuration transition
  and the VM harness. Reported harness cleanup issues were fixed and rechecked.
  All 16 flowcharts were regenerated with the existing approved renderer.

The VM's local reflector did not respond, so the instance could enter STALL.
The first harness attempt received exact DEBUG records but incorrectly required
CPU records, which intentionally pause outside RUNNING. After local review, the
bounded rerun required the activity-independent periodic MEMORY record instead.
The initial attempt and missing-`stat` preflight are retained under ignored
`build/mqtt-validation/`; no production behavior was changed to satisfy a check.

## Evidence and limits

[metadata.json](metadata.json) records current source/archive/package identities,
APK sizes and dynamic dependencies. [nossl-results.json](nossl-results.json),
[nossl.log](nossl.log), [nossl-sanitizers.log](nossl-sanitizers.log) and
[nossl-loaded-library.json](nossl-loaded-library.json) record native checks.
[nossl-vm-results.json](nossl-vm-results.json), [MQTT payloads](nossl-vm-mqtt.log),
[daemon log](nossl-vm-daemon.log), [cleanup state](nossl-vm-cleanup.log) and
[VM outcome](nossl-vm.log) record the bounded VM batch.

No packages were copied into repository `bin/`, versions changed, production
routers accessed or release published. The approved validation sources/builds
and caches remain in ignored `build/`; SDK sources/staging/packages remain in
the x86 SDK. The earlier Filogic and TLS outputs remain separate.

Field interoperability, frequent-record overhead and resource usage while
controlling a real link remain unmeasured. The observation-only VM run validates
transport and cleanup; it is not a CAKE control or performance comparison.

The superseded TLS-capable prototype's original report and tested identities
are preserved in [README-tls.md](README-tls.md) and
[metadata-tls.json](metadata-tls.json). Its TLS/Filogic results do not validate the
current non-SSL sources.

## Reproduction with existing approved local builds

```sh
export LD_LIBRARY_PATH="$PWD/build/mqtt-validation/nossl/lib"
make -C tests check-mqtt ../build/tests/bin/mqtt_driver \
  MQTT_CFLAGS=-I../build/mqtt-validation/nossl/include \
  MQTT_LIBS=../build/mqtt-validation/nossl/lib/libmosquitto.so.1
python3 tests/logging/mqtt_integration.py \
  --source build/mqtt-validation/mosquitto-2.0.22 \
  --library build/mqtt-validation/nossl/lib/libmosquitto.so.1 \
  --driver build/tests/bin/mqtt_driver \
  --output build/mqtt-validation/evidence-nossl
```

`--source` selects the existing broker and password-tool binaries; the broker
uses a plain listener even though those older fixture binaries support TLS.
`--library` selects and hashes the new non-SSL client library.
