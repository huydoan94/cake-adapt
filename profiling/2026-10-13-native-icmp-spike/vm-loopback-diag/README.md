# VM loopback offset diagnostic (2026-10-13, primary agent)

One bounded run on VM .2 (OpenWrt 25.12 x86, i686 kernel 6.12.108, clock source
`tsc`; `kvm-clock` and `acpi_pm` also available), idle (load 0.00), probes run
one after another on `lo`, 100 requests at 100 ms each. The driver
(`tests/latency/icmp_driver.c` with three added timing columns, checksum in
`environment`) now prints, per reply, the send time, the kernel receive time and
the time it read the reply.

| Probe | Kernel-receive RTT median | User-read RTT median | Receive to read |
| --- | ---: | ---: | ---: |
| native echo | 34 µs | 48 µs | 14 µs |
| native timestamp | 32 µs | 45 µs | 13 µs |
| fping | 31 µs | — | — |

Native agrees with fping within 3 µs. The +75–104 µs offset of the earlier
spike batches does not reproduce. Those batches ran native and fping at the same
time on the VM's CPU (the "concurrent paired" cases), so the offset was most
likely contention between the two probes, not the code or the VM's clock. The
kernel receive timestamp is 13–14 µs earlier than the program's read, which is
the scheduling delay native's timestamps remove.

The service, ubusd and the test log were not touched; the driver and results
were removed from the VM after collection (`no-driver-left`).
