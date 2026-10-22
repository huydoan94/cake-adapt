# Loopback check on the development machine (2026-10-13, primary agent)

Same driver as the VM spike (`build/tests/bin/icmp_driver`, checksum in
`environment.txt`), run in a private network namespace (`unshare -rn`, loopback
up) on the WSL2 x86_64 kernel, 30 requests at 100 ms each, then iputils `ping`
in the same namespace.

| Probe | Median RTT (µs) |
| --- | ---: |
| native echo | 31 (an earlier identical run: 23) |
| native timestamp | 26 |
| `ping` | 28 (earlier run: 29) |

Native matches `ping` within a few microseconds in both directions; the
VM's consistent +75–104 µs loopback offset does not appear here, so it is
specific to the 32-bit VirtualBox VM (clock source, timestamp path or
scheduling), not to the code's send or receive timing. Raw output:
`native.csv`, `native-ts.csv`, `ping.txt`.
