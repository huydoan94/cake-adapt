# TCP lifetime kernel runner

This tool loads the supplied current `tcpdelay.bpf.o` in an isolated network
namespace, attaches the production capture consumer, and sends synthetic IPv4
TCP frames over a veth pair. It does not start cake-adapt or fping. Use it only
on a disposable test kernel with BPF loading privileges.

Build for the x86 SDK with:

```sh
make -C tools/tcp-lifetime \
  CC=../../../openwrt-sdk-x86/staging_dir/toolchain-i386_pentium4_gcc-14.3.0_musl/bin/i486-openwrt-linux-musl-gcc \
  STAGING_DIR=../../../openwrt-sdk-x86/staging_dir/target-i386_pentium4_musl
```

Set `CC` and `STAGING_DIR` to the corresponding aarch64 SDK paths for an
arm64 runner. Output is written to `build/tcp-lifetime/runner`. Strict C11
warnings are enabled by default. `LIBBPF_CFLAGS`, `LIBBPF_LIBS`, `CFLAGS`,
`LDFLAGS`, and `LDLIBS` can be overridden for toolchain-specific needs.

The target wrapper accepts an object and a result directory:

```sh
tools/tcp-lifetime/run.sh [--preflight] OBJECT RESULT_DIR
```

`RESULT_DIR` must not already exist. The wrapper creates it and appends an
`ownership.manifest` journal before each namespace, log, and runner mutation;
the journal retains planned paths and observed PIDs if the hard deadline
terminates the wrapper.

It snapshots package/service/process state, configuration and installed file
hashes, host links/routes/qdiscs, and namespaces before creating its uniquely
named namespace and `tl0`/`tl1` veth. The runner has a 120-second alarm and the
wrapper has a 160-second hard deadline, with cleanup starting at 150 seconds.
The wrapper retains logs and
before/after snapshots in `RESULT_DIR`, records hashes for the object, runner,
native consumer/ABI inputs and test wrapper, and verifies the test-log
device/inode at cleanup. It refuses to truncate the authorized test log while
any process has that inode open for writing, including through an alias.
Read-only holders are allowed. Cleanup terminates and reaps the runner, removes
its namespace and hardlink, then compares snapshots even after setup or
execution failure. Compare snapshots for operational state; packet and qdisc
byte counters may change during traffic.

The process snapshot records cake-adapt and pinger PIDs. A separate snapshot
records BPF JIT sysctl values. The tool-owned logging shim forwards libbpf
messages to stderr so loader and verifier diagnostics remain in captured
runner output.

The runner reports the object path, versioned record ABI, current map bounds,
program id and JIT length. A zero JIT length fails the assertion. `--preflight`
checks current map schemas and program identity, then verifies that a
non-timestamp SYN reaches the production consumer and native pending-SYN state.
The normal mode additionally checks SYN retransmission, active and passive
handshake pairing, a repeated local ISN with a different peer ISN, generation
creation across 32-bit sequence wrap, and capture timing availability. These
are functional assertions only; the normal mode does not claim calibration
accuracy, queue accuracy, or performance.
The wrapper uses the runner's `--file-identity PATH` mode to read device/inode
through `stat(2)`; this avoids requiring a separate `stat` utility on OpenWrt.

The earlier profiling runner exercised a reverted generation-in-BPF ABI and
cannot provide before/after results for this native lifetime consumer. This
runner intentionally makes no such comparison. Remaining TL-05 cases are not
implemented here: simultaneous-open behavior, timestamp wrap and cadence
calibration, reuse/quarantine permutations, ring
loss and epoch recovery, helper publication failure, native table/history
capacity, generation/epoch exhaustion, and bounded cross-CPU schedules. No
runtime result is established by local compilation or shell syntax checks.

✓ Local wrapper checks cover hardlink writer refusal, failed-veth setup cleanup,
read-only log holders and a missing recorded log. Run them with
`python3 tools/tcp-lifetime/check-wrapper.py`. They copy the wrapper into a
temporary fixture, redirect its log/release paths, and replace network commands
and runner execution with mocks; no VM, BPF load or real network mutation occurs.
They do not establish the real kernel deadline or packet semantics.

See [PREFLIGHT.md](PREFLIGHT.md) for the accepted bounded preflight plan, input
identity, required baseline and stopping/cleanup rules. Full TL-05 coverage
remains open.
