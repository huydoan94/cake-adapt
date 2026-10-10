# TCP timestamp injection on IPv6 only (0.3.15)

`tcp_ts_request` now adds timestamps only to IPv6 SYNs, keyed by client
address (`2a5e3e1`), and counts a handshake as accepted only once the server
acknowledges the client's first data (`0dc6e37`, released as 0.3.15 with only
the version changed). On the 32-bit x86 VM (OpenWrt 25.12) the testbed's
server behaviours end as designed over IPv6, IPv4 is never touched, a client
whose clock looks older than the injected value loses only its first round,
and 43 of 43 lifecycle checks pass with injection on. Filter and injector cost
are unchanged on IPv6 traffic and the injector is cheaper on IPv4.

## Builds

| Name in `raw/` | Build |
| --- | --- |
| `thorough/*-before-*` | `2a5e3e1` (x86 package of that commit) |
| `thorough/` everything else, `*-after-*` | the tree committed as `0dc6e37`; no source changed between this build and the commit |
| `final/` | the tree committed as `2a5e3e1` |
| `cleanup/*-before-*`, `*-after-*` | the IPv6-only rewrite before and after its eBPF cleanup pass, both uncommitted steps toward `2a5e3e1` |
| `lifecycle/` | the installed 0.3.15 package (x86 SHA-256 `95898760…`) |

## Function (`raw/thorough/`, script `tools/thorough.sh`)

`inject.sh` and `clock.sh` are in `tools/testbed/`; their README explains the
scenarios.

- **Server behaviours over IPv6** (`inject-v6`): the `TCP_INJECT` record ended
  at 7 injected, 6 skipped, 4 accepted, 0 stalled. The first connection to a
  server that rejects or resets a timestamped SYN fails once; the second is
  left alone and succeeds. Nothing is rewritten after stop or SIGKILL.
- **Over IPv4** (`inject-v4`): every counter stayed 0 and every connection
  succeeded; IPv4 SYNs are not touched.
- **Client clocks**, two rounds of 20 requests each:

  | Run | Round 1 | Round 2 | Final `TCP_INJECT` (injected; skipped; accepted; stalled) |
  | --- | --- | --- | --- |
  | `young-v6` (uptime clock) | 20 ok | 20 ok | 40; 0; 40; 0 |
  | `old-v6` (34.7-day clock, one at a time) | 19 ok, 1 failed | 20 ok | 40; 0; 39; 1 |
  | `old-v6-parallel` (same, 20 at once) | 20 failed | 20 ok | 40; 0; 20; 20 |
  | `old-v4` (34.7-day clock over IPv4) | 20 ok | 20 ok | 0; 0; 0; 0 |
  | `random-v6` (new random clock per connection) | 12 ok, 8 failed | 7 ok, 13 failed | 19; 0; 11; 8 after the first minute |

  A burst opened before the first stall is seen all carries 1, so a client
  with an old clock can lose that whole first burst; afterwards its learned
  clock is used. `random-v6` has no clock to learn; no real client behaves
  this way, and it is kept as a stress case.
- **Control run with TCP attribution** (`run`): 5,598 `TCP_QUEUE` records,
  4,561 with a valid direction; the daemon exited cleanly.
- Both programs were accepted and JIT-compiled (`progs.txt`: filter 6,768 B
  translated, injector 4,064 B).

`final/` repeats the server behaviours for `2a5e3e1` over IPv6 and IPv4 with
the same outcome.

## Cost (`kernel.bpf_stats_enabled`, ns per run)

Each block alternates the builds in one session (B A A B, repeated in
`thorough`), with `bench.sh` traffic to the IPv4 or IPv6 server. The VM's run-to-run
noise is about ±15%.

| Run | Traffic | Filter before → after | Injector before → after |
| --- | --- | --- | --- |
| `thorough` (`2a5e3e1` → `0dc6e37`) | IPv4 | 1968 → 1578 | 283 → 224 |
| `thorough` | IPv6 | 1522 → 1578 (+4%) | 412 → 423 (+3%) |
| `cleanup` (before → after the cleanup pass) | IPv4 | 1359 → 1503 | 340 → 222 (−35%) |
| `cleanup` | IPv6 | 1546 → 1570 | 420 → 405 |

The individual runs are in each `.txt`. The injector's IPv4 saving comes from
checking the protocol before the settings lookup; the IPv6 changes and the
filter differences are within the noise.

## Lifecycle (`raw/lifecycle/`, script `tools/lifecycle.sh`)

43 of 43 (`summary`) with `tcp_delay_attribution` and `tcp_ts_request` on in
the VM's service. The script is `2026-10-07-openwrt-snapshot`'s 33 checks plus
10 for the injector's tcx link (`bpftool net`): exactly one while running,
after restart, SIGKILL and procd respawn, the download CAKE deleted and
re-created, an SQM restart and restoring the configuration; none after stop
and with a disabled or invalid configuration; and no `TCP measurement
degraded` message at start.

## Limits

- One 32-bit x86 VM; the Filogic build was not run here.
- The testbed clients imitate Windows with nftables; no real Windows client
  was used.
