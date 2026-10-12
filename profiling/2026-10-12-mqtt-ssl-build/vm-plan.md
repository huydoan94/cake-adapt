# Bounded optional-TLS acceptance, x86

Question: does the SSL-built packaged daemon validate TLS configuration and
publish generated logs through a certificate-verified TLS broker on the authorized
x86 VM, while observation and shutdown remain responsive?

Scope: one read-only preflight (10-second deadline) on 192.168.56.2, then at most
one planned acceptance batch after local harness review. No production/Windows
access, rate adjustment, installed-service restart, package installation,
version bump or repo/bin copy. Native fixtures separately prove rejection of
untrusted certificates and wrong hostnames; do not repeat long native cases here.

Payload: exact SSL daemon/library package contents extracted locally into ignored
build/, including Mosquitto, c-ares, OpenSSL and a test-only CA certificate. Transfer
to a new /tmp/cake-adapt-mqtt-tls directory on VM, verify individual hashes.
Local cached TLS broker through a bounded SSH reverse tunnel; certificate SAN
includes 127.0.0.1 so VM loopback hostname verification succeeds. Unique UCI
instance mqtt_tls_test: existing eth1, both rate adjustment flags off, no TCP
capture/injection, no sleep, local reflector, MQTT+TLS on with explicit CA and
local port/topic, output_memory_stats on. Preserve /tmp/sqm-mon-test.log using
one hard-linked isolated instance log name, check writers before in-place
truncation, record inode; no interaction with user's tail.

Acceptance batch <=60seconds after transfer: -V config succeeds, daemon starts,
receive at least one periodic MEMORY record over TLS and verify every delivered
line matches file log, broker outage leaves daemon alive, TERM stops test daemon
and fping child. Existing qdisc settings, installed daemon PID and test-log inode
must remain unchanged. One batch suffices; no controller or performance claim.

Cleanup always: terminate owned broker/subscriber/tunnel, only test instance and
children; remove test-created /tmp directory/lock, preserve test log inode.
Record raw broker/subscriber/daemon logs, checksums, before/after service and
qdisc state. A harness failure gets local diagnosis and only affected rerun;
a second environment/harness failure stops VM work for reassessment. No broad
follow-up campaigns, no arbitrary waits or dependencies installed.

Affected rerun: first batch stopped before daemon launch because the cached native
TLS subscriber binary had not been compiled. Scoped cleanup succeeded. Built only
client/mosquitto_sub from the existing approved Mosquitto source with WITH_TLS=yes,
WITH_CJSON=no and WITH_EDITLINE=no. Rerun the same acceptance once; preserve first
failure evidence in evidence-vm-tls-harness-failure and vm-harness-failure.log.
