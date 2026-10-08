# TCP timestamp injection on the testbed (2026-10-07)

**Result:** `tcp_timestamp_inject` (`3e23b04`) behaves as designed on OpenWrt
25.12 (x86 VM `192.168.56.2`, kernel 6.12.108, 32-bit). Against a client
without TCP timestamps, the four server behaviors end as intended, the
counters match the connections exactly, nothing is rewritten after the daemon
stops or is killed, SQM's download redirect keeps working, and all 33
lifecycle checks pass with injection on. Merging the injector into the TCP
filter's object removed the separate ingress program, so incoming packets are
read once.

**What this is not:** a router or accuracy result. Costs come from a slow
32-bit VirtualBox CPU with the kernel's BPF statistics on, which add clock
reads to every run; real routers run with them off. The Windows behavior
(adopting timestamps) is from `2026-10-07-tcp-timestamp-injection`, not this
testbed, whose Linux client ignores them.

## Scenarios (`raw/scenarios/`, `tools/testbed/inject.sh`)

The client in `cpe` runs with `net.ipv4.tcp_timestamps=0`. Each server gets
two connections:

| Server | First connection | Second connection |
| --- | --- | --- |
| 5202 accepts timestamps | works | works |
| 5201, client drops timestamped SYN-ACKs | fails: resends its SYN, rejection counted | works, not injected (skipped) |
| 5204 resets timestamped SYNs | fails: reset, rejection counted | works, skipped |
| 5203 drops timestamped SYNs | works after one resend without the timestamp | works, skipped |

The daemon's line at stop: `injected=7 server_accepted=5 server_declined=0
client_rejected=1 server_rejected=2 skipped=7 failed=0`, one per connection
as listed. SYNs after a clean stop and after SIGKILL left without a timestamp
(`raw/scenarios/handshakes`), so the kernel removed the program both times.

## Cost (`raw/cost-*`, `tools/cost.sh`)

`bpftool prog show` run time per run under 20 s of 4 downloads, then 20 s of
4 uploads, from the timestamp-less client (injection on):

| Program | Separate programs | Merged object |
| --- | --- | --- |
| TCP filter (both directions) | 1,135 ns | 1,065 ns |
| injector egress (tcx) | 587 ns | 466 ns |
| injector ingress (tcx) | 505 ns | removed |

An empty tcx program measures 234 ns (egress) and 215 ns (ingress) under the
same load (`raw/cost-floor-empty-program.txt`, `tools/empty.bpf.c`): that much
is the hook and the statistics' own clock reads. The merged egress injector
checks every packet in place and only SYNs and resets go further; the
read-only observation of SYN-ACKs and resets moved into the filter, which
already parses every incoming packet.

## Lifecycle (`raw/lifecycle.txt`)

33 of 33 with `tcp_delay_attribution` and `tcp_timestamp_inject` on in the
VM's service, including the download CAKE deleted and re-created, an SQM
restart, signals and rotation.

## Notes

- One measurement was lost to the VM's known i386 panic
  (`raw/vm2-serial-panic.log`: a page fault in `handle_exception` inside
  `<ENTRY_TRAMPOLINE>`, after a clocksource readout warning, during `iperf3`;
  no BPF frame) and repeated.
- Two problems found here were fixed before `3e23b04`: LLVM compiled a loop to
  a `__bpf_trap` kfunc call, which needs kernel BTF that OpenWrt lacks (the
  build now fails on it), and a two-step option read made the egress program
  too complex for the verifier (`E2BIG`).
