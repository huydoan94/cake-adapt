# Superseded TLS-capable MQTT prototype

This report records the earlier implementation and its original tested hashes in
[metadata-tls.json](metadata-tls.json). The current code uses non-SSL MQTT; these
TLS and Filogic results do not validate the current revision. Historical commands
require the fixture identified by that metadata. See [current evidence](README.md).

# Optional MQTT log forwarding

This is native loopback acceptance of the production logging and MQTT modules,
with Mosquitto 2.0.22 built locally. It is not a router deployment or a CAKE
performance comparison. MQTT is disabled by default.

The user chose silent loss instead of offline buffering. The main logging path
uses a try-lock and a bounded queue; it never resolves a host, opens a broker
connection, waits for queue space or publishes over the network. Sender and
network threads own those operations. Output flags and log schemas are preserved.

`tests/logging/test_mqtt.c` checks disabled/offline submission, lock contention,
queue overflow, exact payloads, length limits and send failure.
`tests/logging/mqtt_integration.py` drives the production sender against the
actual library and broker. `results.json` contains the observed timings:

- A TCP peer that never sends CONNACK received connections at approximately
  0.003, 10.019 and 20.034 seconds. The sender remained offline and dropped logs.
- A 10,000-record offline burst returned in 18–24 microseconds on this host.
  This measures submission only, not general daemon or router CPU cost.
- After a broker outage, reconnection took 10.028 seconds. The first delivered
  record was the fresh one; offline records were not replayed.
- QoS 0, non-retained delivery preserved the topic and exact record payload.
  The ordinary INFO logging path published with file logging disabled.
- Correct credentials and a trusted matching TLS certificate connected. Wrong
  credentials, an untrusted certificate and a trusted certificate for the wrong
  hostname remained offline without stderr diagnostics.
- Shutdown took at most 102 ms in these cases. The production main-thread join
  is capped at about one second if a worker remains stuck.

AddressSanitizer and UndefinedBehaviorSanitizer passed the queue tests and the
TLS connection/teardown cases; `sanitizer-results.json` and `sanitizer.log` retain
those results. The logging, configuration, init/import, LuCI, replay and monitor
host checks passed. The netlink checks require running outside the restricted
sandbox and passed there.

`metadata-tls.json` identifies the source archive, native library, fixture and tested
source files. The approved third-party source, binaries, broker logs and temporary
test certificates remain in ignored `build/mqtt-validation/`; there was no system
installation. The integration fixture always stops its broker and driver.

On 2026-10-12 both packages built successfully in the existing Filogic SDK at
0.3.17-r1, with no deployment or artifact copy into the repository's `bin/`.
The packaged daemon is 118,793 bytes; its APK is 73,517 bytes and the LuCI APK is
16,620 bytes. The APK metadata names `libmosquitto-ssl` and `ca-bundle` as required
dependencies, and the executable links `libmosquitto.so.1`. Package checksums,
installed sizes and the dependency-source checksums are in `metadata-tls.json`.

Required sources were downloaded into the SDK's `dl/`: Mosquitto 2.0.22,
OpenSSL 3.5.7, c-ares 1.34.6 and ca-certificates 20260601. The initial recursive
SDK build also began repackaging selected kernel modules, so it was stopped.
Final library and package builds used `NO_DEPS=1` and excluded unneeded broker,
client and OpenSSL-engine packages. No feeds were updated or SDK downloaded.
The Graphviz renderer (`@hpcc-js/wasm-graphviz` 1.29.2) and its npm cache stay in
ignored `build/`; all 16 flowcharts were regenerated with Graphviz 16.1.0.

Reproduce after building the library and broker in that directory:

```sh
export LD_LIBRARY_PATH="$PWD/build/mqtt-validation/mosquitto-2.0.22/lib"
make -C tests check-mqtt ../build/tests/bin/mqtt_driver \
  MQTT_CFLAGS=-I../build/mqtt-validation/mosquitto-2.0.22/include \
  MQTT_LIBS=../build/mqtt-validation/mosquitto-2.0.22/lib/libmosquitto.so.1
python3 tests/logging/mqtt_integration.py \
  --source build/mqtt-validation/mosquitto-2.0.22 \
  --driver build/tests/bin/mqtt_driver \
  --output build/mqtt-validation/evidence
```

This establishes native transport behavior and a non-waiting submission path.
Router interoperability, field resource usage and the impact of enabling frequent
records while controlling a real link still require on-target observation.
