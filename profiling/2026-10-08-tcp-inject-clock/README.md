# TCP timestamp injection against client clocks (2026-10-08)

**Result:** with the injected TSval `0xca4ead01`, a Windows-like client whose
own timestamp clock looked older than that value had its connections hang
silently, while the counters reported success. The fix (`146a751`: inject 1,
detect stalled handshakes, pause injection) turns that into counted stalls
and a 24-hour pause after the first minute's check.

**What this is not:** a Windows test. The client is Linux made to behave like
Windows; the Windows clock itself comes from the field data below.

## Why

A client that adopts the server's timestamp then sends its own clock, and a
server drops segments whose TSval is behind the last one it saw (PAWS),
comparing 32-bit values by their signed difference. Windows sends
milliseconds since boot: the round-3 capture of
`2026-10-07-tcp-timestamp-injection` saw TSval 35,439,554 at 02:40:12 UTC on
2026-10-08, which dates the boot to 10:49:33 CST on 2026-10-07; Task Manager
on that PC gave 10:50:15 CST. Against `0xca4ead01`, uptimes from 14.4 to 39.3
days look older; against 1, every uptime up to 24.8 days follows it.

## Method (`tools/testbed/clock.sh`)

The client in `cpe` keeps timestamps on, but nftables blanks the timestamp
option of its SYNs (so the injector adds one) and zeroes the echo in arriving
SYN-ACKs (so Linux accepts an echo it never sent, as Windows does). Its clock
is `net.ipv4.tcp_timestamps`: `2` counts since boot (the VM was up about 14
hours, a young Windows-like clock), `1` adds a random offset per connection,
so about half look older than any fixed value. Two rounds of one request to
each of 20 servers (`tcpthink`), each killed 5 s after it should end.
Baseline: the installed build (`3e23b04` code); fix: `146a751`, run as a
separate binary with its BPF object swapped in for the test.

## Results (`raw/*/connections`, `raw/*/cake-adapt.log.gz`)

| Clock | Build | Round 1 | Round 2 | Counters at the end |
| --- | --- | --- | --- | --- |
| young (2) | baseline | 20 ok | 20 ok | injected 40, accepted 40 |
| young (2) | fix | 20 ok | 20 ok | injected 40, accepted 40, stalled 0 |
| random (1) | baseline | 10 failed | 9 failed | injected 40, accepted 40, nothing rejected or skipped |
| random (1) | fix | 9 failed | 0 failed | injected 15, accepted 15, stalled 8, then paused |

With the fix, the first minute's check found 8 stalls and paused injection
for 24 hours with the warning; every later connection went out without
injection and worked. The breaker acts once a minute, so a client with an
old clock can lose that minute's new connections once a day.

## Limitation

The injector runs on the WAN after masquerading, so it cannot tell clients
apart or learn each one's clock; the pause applies to all of them. See
`docs/configuration.md`.
