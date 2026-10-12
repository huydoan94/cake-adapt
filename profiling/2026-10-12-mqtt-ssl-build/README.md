# Optional SSL builds and verified MQTT TLS

`ssl=no` remains the default. `sh tools/sdk-build.sh x86 ssl=yes` stages the
SSL library and builds matching daemon and LuCI packages. SSL builds expose
`mqtt_tls` and `mqtt_ca_file` in LuCI's Advanced Logging tab. TLS defaults off;
when enabled, trust and hostname verification are mandatory. There is no
insecure mode or plaintext fallback. The no-SSL daemon rejects active TLS
configuration. Saved TLS and custom CA settings survive disabling and
re-enabling MQTT in LuCI.

Both variants retain the existing nonblocking submission, silent offline/full
queue drops, QoS 0, no retention and indefinite 10-second reconnect cadence.
This report covers transport and build behavior, without a control-performance
or production-router claim. [metadata.json](metadata.json) identifies the
uncommitted tested source by SHA-256; the earlier MQTT report covers older hashes.

## Checks and evidence

- All 39 build-helper fixture methods pass ([output](helper-tests.log)). They
  cover dependency readiness and ordering, headers/libraries/stamps, ELF target
  compatibility, pkg-config metadata and wrapper environment, build tools,
  unavailable feeds and undeclared dependencies, variant repair, invalid flags,
  package dependency selection, LuCI capability preparation, source rebuilds
  when switching capability, and unchanged-build behavior.
- Native config and queue/setup tests pass with both flags. LuCI checks pass
  both modes, including inactive-field save/reload/enable retention. Changed
  C and headers remain clang-format stable; shell syntax and diff checks pass.
- The real cached Mosquitto 2.0.22 library/broker fixture passes six cases:
  trusted TLS with authentication and exact payload delivery, wrong CA rejection,
  wrong hostname rejection, no TLS-to-plaintext fallback, plaintext transport
  in an SSL build, and a stalled TLS handshake. See [native results](native-results.json).
- The same six cases pass ASan/UBSan with `ASAN_OPTIONS=detect_leaks=1` outside
  the ptrace-restricted sandbox ([results](sanitizer-results.json)). Config and
  queue tests also pass sanitizers. Earlier sandbox runs disabled leak detection;
  the separate unrestricted fixture supplied the leak check.
- A focused follow-up checks the stalled peer's first socket EOF, rather than
  inferring its deadline from retries. It sees 311 ClientHello bytes, closure
  after 5.208 seconds, retry after 10.026 seconds, a 32 µs offline submission
  burst, and 101,409 µs shutdown ([results](stalled-results.json)). These host
  timings are functional bounds, not router overhead measurements.
- Both daemon and LuCI packages build in the OpenWrt 25.12 x86 SDK with
  `NO_DEPS=1`, first `ssl=yes`, then `ssl=no`. SSL APK dependencies contain
  `libmosquitto-ssl` and `ca-bundle`; default dependencies contain
  `libmosquitto-nossl` with neither CA bundle nor SSL library dependency. Actual
  staged ELF dependencies and daemon TLS symbols agree, and packaged LuCI
  embeds the corresponding capability. Package checksums and full metadata are
  saved for [SSL](ssl-package-metadata.json) and [default](nossl-package-metadata.json).
  Both daemon packages report 178,338 installed bytes; the extracted SSL daemon
  is 118,789 bytes. Optional OpenSSL adds 1,063,972 bytes for libssl and 4,674,760
  bytes for libcrypto in this SDK, excluding other dependency files.

## Bounded x86 VM check

The [reviewed plan](vm-plan.md) and [harness](x86-tls-vm.py) use an isolated `/tmp`
payload extracted from the actual SSL APKs, a test CA and an SSH reverse tunnel
into the local cached TLS broker. Both rate-adjustment flags are off; no TCP
capture/injection or installed-service changes occur. The VM clock and local
clock agree, and the certificate has a loopback IP SAN.

One read-only preflight passes. The first batch stops before daemon launch
because the native TLS subscriber executable is missing; cleanup succeeds
([failure log](vm-harness-failure.log)). Building only that client from cached
source fixes the harness. The affected rerun passes under its aggregate
60-second acceptance deadline with separate bounded cleanup:

- Nine received records match complete file-log lines, including periodic
  `MEMORY`. The daemon remains alive during broker outage and stops on TERM.
- The test daemon and its fping child exit; scoped payload and lock are removed.
  The installed daemon remains PID 3389, qdisc settings match before/after,
  and `/tmp/sqm-mon-test.log` retains inode 179.

[VM results](vm-results.json), [MQTT records](vm-mqtt.log),
[daemon records](vm-daemon.log), [broker log](vm-broker.log) and the
[public test CA](vm-ca.pem) retain identities and raw transport evidence.
Private test keys and payload archives remain only in ignored `build/`.
This is not browser acceptance, internet-broker interoperability, or a
measurement of frequent-record overhead while controlling CAKE.

## Reproduction and environment changes

Use `make -C tests check-sdk-build` for helper fixtures and `(cd tests && sh
 test_luci.sh)` for both LuCI capability modes. Build focused config/MQTT targets
with `ssl=yes` or `ssl=no`, x86 staged UCI headers and the corresponding cached
native Mosquitto library. Run the TLS fixture with:

```sh
python3 tests/logging/mqtt_integration.py --tls-only \
  --source build/mqtt-validation/mosquitto-2.0.22 \
  --driver build/tls-settings/ssl/bin/mqtt_driver \
  --library build/mqtt-validation/mosquitto-2.0.22/lib/libmosquitto.so.1 \
  --output build/tls-settings/evidence
```

Use `--tls-stalled-only` for the socket-deadline case. The VM harness expects
SSL package contents under `build/sdk-ssl-validation/ssl-root`, native cached
TLS broker/subscriber executables, and the authorized existing VM. Do not run
it against a production router. SDK validation uses `--prepare-only x86
ssl=yes|no`, followed by focused daemon/LuCI clean and compile targets with
`NO_DEPS=1` and the matching flag; it avoids the helper's release-copy step.

No new download or system installation occurred. The approved OpenSSL 3.5.7
archive was reused from the existing Filogic `dl/` cache and copied into x86
`dl/`; Filogic was not built or staged. x86 gained its OpenSSL feed link,
package/build outputs and staged OpenSSL libraries. Mosquitto staging was
restored to no-SSL after validation. Existing SDK configuration selections,
including explicit disabled values, were unchanged. SDK configuration
normalization added 34 previously absent symbols (23 selected, 11 disabled);
see [delta](sdk-config-delta.json). This includes optional engine defaults,
which the scoped dependency commands override off. No kernel targets ran.
Artifacts remain in the SDK and ignored validation directory; nothing was
copied to repository `bin/`, installed on the VM, versioned, committed or released.
The 16 repository flowcharts were regenerated using the existing approved
cached Graphviz renderer and their source hashes were checked.
