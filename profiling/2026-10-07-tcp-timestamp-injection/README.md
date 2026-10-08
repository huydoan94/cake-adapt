# TCP timestamps injected for Windows clients (2026-10-07)

**Result:** adding the timestamp option to a Windows client's SYNs on the
router is enough to turn TCP timestamps on for Windows connections, in both
directions. A `tc` BPF program on the LAN rewrote every Windows SYN (775 over
about 30 minutes of the user's real use), and 715 servers enabled timestamps.

- **Rounds 1 and 2,** with a second program that removed the timestamps from
  packets back to Windows: no connection failed, no checksum was wrong, no
  timestamp reached Windows, and cake-adapt gained download estimates from
  Windows connections, which give it nothing without timestamps.
- **Round 3,** without that second program: Windows took the server's
  timestamp in the SYN-ACK as agreement and put its own on 98.7% of its
  packets. No handshake failed, and during a sustained download both the
  download and the upload estimate were valid in nearly every record. The
  strip is unnecessary for Windows, and dropping it gains the upload
  direction.

The user saw normal, smooth browsing and transfers in all three rounds.

**What this is not:** an accuracy measurement, a cost measurement, or a
product change. The program is an experiment in `tools/`, loaded by hand with
`tc`; cake-adapt is unchanged. Rewriting packets goes beyond what cake-adapt
does today (observe, and control CAKE), so adopting it needs a user decision.

## Why

Windows does not offer TCP timestamps on connections it opens, so the TCP
filter's estimator cannot measure Windows downloads (see
[`docs/design/L4S.md`](../../docs/design/L4S.md), "TCP timestamp availability
and alternatives"). Timestamps are agreed in the SYN, and the download
estimate needs only the server's TSval and our arrival times, not the
client's echo.

## Method

[`tools/tsinject.bpf.c`](tools/tsinject.bpf.c) (object SHA-256 in
`tools/tsinject.o.sha256`, built with the snapshot SDK's LLVM 22):

- `br-lan` ingress (`inject`): a SYN without data and without a timestamp
  gets `NOP, NOP, TS(TSval 0xca4ead01, TSecr 0)` appended; the IP and TCP
  checksums are updated incrementally and the stack re-verifies them.
- `br-lan` egress (`strip`): a packet whose TSecr is that marker belongs to
  such a connection (the client never sends a newer TSval, so the server keeps
  echoing it); its timestamp option is overwritten with NOPs. No state is
  shared between the two programs.

The VM was `192.168.56.3`: OpenWrt snapshot r36953, x86/64, kernel 6.18.55,
`kmod-sched-bpf` installed, its WAN bridged to the user's network
(`192.168.2.70`), SQM CAKE on the WAN and cake-adapt 0.3.7-r1 (`b28b1ee`)
with `tcp_delay_attribution 1`. The user's Windows PC routed all IPv4 traffic
through the VM. Attached with:

```sh
tc qdisc add dev br-lan clsact
tc filter add dev br-lan ingress pref 1 bpf da obj tsinject.o sec tc/ingress
tc filter add dev br-lan egress pref 1 bpf da obj tsinject.o sec tc/egress
```

A watcher recorded SYNs and SYN-ACKs on both sides (`tcpdump -v` on `eth1`
and `br-lan`) and cake-adapt's `TCP_QUEUE` records. Round 2 also sampled all
packets to Windows for 3 s every 20 s. `tools/tally.py` produces the tallies.

## Results

| | Round 1 (02:07-02:15 UTC) | Round 2 (02:15-02:18 UTC) |
| --- | --- | --- |
| Windows SYNs without timestamps, rewritten | 144 of 144 | 72 of 72 |
| servers that enabled timestamps | 133 | 64 |
| servers that answered without them | 11 | 8 |
| SYNs left unanswered | 0 | 0 |
| SYN-ACKs to Windows still carrying the marker | 0 of 167 | 0 of 96 |
| bad checksums | 0 of 666 | 0 of 374 |
| `TCP_QUEUE` records with a valid download estimate | 4,314 of 7,406 | 1,386 of 2,400 |

- Every server that declined is Microsoft's (13.107, 20.190, 40.126, 52.104,
  52.123, 150.171 and 172.172 addresses); they answered without timestamps
  and the connections worked.
- SYNs that already carried a timestamp (22 and 19, from another client
  behind the same address) were left untouched.
- Round 2's samples (`raw/round2-strip-samples.txt`) caught 346 packets to
  Windows; none carried the marker. The 164 with timestamps belong to
  connections whose client asked for them.
- During the round 1 download (up to 38 Mbit/s) almost every record had a
  valid download estimate, beside fping's RTT/2 download delta:

| 10 s from (UTC) | TCP estimate mean / max | fping mean / max |
| --- | --- | --- |
| 02:09:20 | 7.7 / 54 ms | 5.1 / 47 ms |
| 02:09:30 | 15.3 / 92 ms | 11.6 / 62 ms |
| 02:09:40 | 1.0 / 10 ms | 3.0 / 32 ms |
| 02:09:50 | 16.4 / 107 ms | 13.8 / 103 ms |
| 02:10:00 | 8.6 / 72 ms | 9.2 / 71 ms |
| idle | about 0.5 ms | about 0.6 ms |

### Round 3: without the strip (02:39-03:00 UTC)

Only `inject` was attached (`.3` had rebooted after round 2, cause unknown;
no crash record survives on it). The watcher also recorded RSTs, and Windows'
own packets (TTL 128) were sampled for 3 s every 20 s
(`raw/round3-windows-samples.txt`).

| | Round 3 |
| --- | --- |
| Windows SYNs rewritten | 559 (one more SYN fell at the watcher's start) |
| servers that enabled timestamps | 518 |
| servers that answered without them | 41 |
| SYNs left unanswered, or sent again | 0 |
| bad checksums | 0 of 2,787 |
| sampled Windows packets carrying Windows' own timestamp | 53,853 of 54,564 (98.7%) |
| connections ended by a Windows RST, SYN-ACK with a timestamp | 68 of 518 (13%) |
| connections ended by a Windows RST, server declined timestamps | 20 of 41 (49%) |
| `TCP_QUEUE` records with a valid download / upload estimate | 6,791 / 5,618 of 15,516 |

- Windows answers a timestamped SYN-ACK as a client that asked for
  timestamps, for example `TS val 35457248 ecr 3460381709`: its own clock and
  the server's echoed.
- The RSTs are not caused by the timestamps: they are more frequent on the
  connections the experiment does not change (the server declined), and most
  go to a few hosting networks (123.30.x, 14.225.x, 42.112.x), whose sites
  close connections that way.
- A sustained download (02:48:00-02:50:50, 30-45 Mbit/s) had a valid estimate
  in both directions in nearly every record: download 2-10 ms and upload
  0.5-2.5 ms per 10 s mean, against fping's RTT/2 of 8-17 ms
  (`raw/round3-download-window.txt`). fping cannot split directions; the TCP
  estimate puts the delay on download, the loaded direction. Its download
  values are lower than fping's full round-trip delta would suggest (16-34
  ms if all of it were download queue); fping's replies also cross the VM's
  own download CAKE, which the tap does not see, but this run cannot tell the
  causes apart.
- Upload bursts of up to 50 Mbit/s (the VM's upload maximum) had a valid
  upload estimate in 60-80% of records, with small queues (TCP 0.3-1 ms,
  fping RTT/2 1-3 ms): the user's real upstream is faster than the VM's
  shaper.

## Limits

- The estimator is bounded by fping's delay, so agreement in the maxima is
  partly by construction; the means and idle values are the more independent
  comparison. This is not an accuracy result against a known queue.
- The estimator follows one flow at a time and does not log which; the
  Windows connections carried nearly all of the traffic during the download,
  so they very likely supplied it.
- IPv4 only (the VM had no IPv6 route for the PC, and part of the user's
  traffic left over IPv6 around it). No server that drops segments lacking
  timestamps showed up in rounds 1 and 2 (216 connections), but such servers
  may exist. Filter cost was not measured.
- Round 3's behavior is this Windows version's; another client that checks
  the echoed TSecr against its own clock, as Linux does, would reject the
  SYN-ACK. The inject program skips SYNs that already offer timestamps, so it
  never meets Linux, Android or macOS clients.
- Rounds 1 and 2 did not record RSTs, so their RST counts are 0 by
  omission.
- The first 10 s of round 1 (`02:07:30`) are the daemon's start after a VM
  reboot, before its baseline settled.

## Files

- `raw/round1.log`, `raw/round2.log`, `raw/round3.log`: the watcher output
  (WAN/LAN handshakes, and RSTs in round 3; `EST` = `TCP_QUEUE` records).
- `raw/round*-tally.txt`: `tools/tally.py` output. Round 2 has no fping
  comparison because the DATA extract was taken before it; round 3's starts
  at 02:51:38, where the daemon's retained log began.
- `raw/round2-strip-samples.txt`: packets to Windows per 3 s sample.
- `raw/round3-windows-samples.txt`: Windows' own packets per 3 s sample, with
  examples.
- `raw/round3-download-window.txt`: per 10 s TCP estimates, peak rates and
  fping, recorded live during round 3's download.
- `raw/fping-download-delta.txt`, `raw/round3-fping-delta.txt`: fields from
  the daemon's DATA records (columns named in each file's first line).
