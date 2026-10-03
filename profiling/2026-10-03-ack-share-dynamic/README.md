# Download ACKs on a slow upload: off vs. `upload_ack_share_min` (2026-10-03)

**Result:** download ACKs took 46–74% of a 900 kbit/s upload, and a 200 kbit/s
UDP stream lost up to 48% of its packets. With `upload_ack_share_min 0.45`,
ACKs used what the other upload traffic left free and gave way only as far
as that traffic needed, down to 45% of upload: the stream lost nothing with
downloads alone and 3.5–5.5% with a bulk upload competing. Download paid
with 16–20 Mbit/s instead of 24 under 4–8 parallel downloads, and 12–17
Mbit/s with a bulk upload.

CAKE already protects sparse traffic by flow; ICMP and a 64 kbit/s stream
lost nothing either way. Traffic above its per-flow fair share, like the
200 kbit/s stream or a bulk upload, competes with every ACK stream.

## The rule

While upload is above `high_load_thr`, the eBPF filter splits what leaves on
upload (after CAKE, over the same 500 ms) into pure ACKs and everything else.
ACKs may use the shaper rate minus the other traffic, minus 5% headroom in
which that traffic's growth shows, but never less than 45% of the shaper
rate. When ACKs exceed that allowance, download is held at
`download achieved × allowance / ACK rate`, never below its minimum. The
minimum is not a reservation: while ACKs need less, other traffic uses the
rest.

## Setup

OpenWrt 25.12 x86 VM, namespace testbed with a
1 up / 30 down Mbit/s bottleneck, plain `ack-filter` on upload, upload fixed
at 900 kbit/s, download controlled between 5 and 27 Mbit/s,
`tools/testbed/acks.sh ack-filter` with `DAEMON=` (1, 4 and 8 downloads,
then 4 and 8 with an upload; ICMP and 64 and 200 kbit/s UDP probes).
Two alternating rounds of `upload_ack_share_min 0` (off) and `0.45`. x86
`cake-adapt` SHA-256 `0b79fccf80c555c1…`, BPF object `ee8c275074df8ae5…`, run
in isolation in `cpe`; the BPF object was in `/lib/bpf` for the test only.
The test log kept inode 433.

## Results

Means of two rounds. UDP is the 200 kbit/s stream: added RTT p50 and loss.
Per-run tables: `R<round>-<cap0|dyn45>.txt` (`cap0` is off, `dyn45` is
`upload_ack_share_min 0.45`).

| Phase | | Off | `upload_ack_share_min 0.45` |
| --- | --- | --- | --- |
| 1 download | download | 22.3 Mbit/s | 22.8 Mbit/s |
| | ACKs | 284 kbit/s | 299 kbit/s |
| 4 downloads | download | 24.0 Mbit/s | 20.4 Mbit/s |
| | ACKs | 538 kbit/s | 496 kbit/s |
| | UDP 200 kbit/s | 12.2 ms, 0.5% | 4.1 ms, 0% |
| 8 downloads | download | 24.4 Mbit/s | 16.3 Mbit/s |
| | ACKs | 662 kbit/s | 499 kbit/s |
| | UDP 200 kbit/s | 41.2 ms, **39%** | 4.0 ms, **0%** |
| 4 downloads + upload | download | 23.6 Mbit/s | 17.4 Mbit/s |
| | upload goodput | 0.16 Mbit/s | 0.21 Mbit/s |
| | UDP 200 kbit/s | 43.5 ms, **15.5%** | 14.1 ms, **3.5%** |
| 8 downloads + upload | download | 24.0 Mbit/s | 12.2 Mbit/s |
| | upload goodput | 0.10 Mbit/s | 0.18–0.21 Mbit/s |
| | UDP 200 kbit/s | 49.7 ms, **48%** | 27.5 ms, **5.5%** |

ICMP and the 64 kbit/s stream lost nothing in any run.

## Findings

1. **The split follows demand.** With downloads alone, the other upload
   traffic (the probes, about 320 kbit/s) left ACKs about 500 kbit/s, 55% of
   upload, so download was held only to 16–20 Mbit/s.
2. **The 200 kbit/s stream is protected:** lossless at a few ms median with
   downloads alone, against up to 39% loss without the rule, and 3.5–5.5%
   loss with a bulk upload, against 15–48%.
3. **Under heavy competition ACKs go down to their minimum.** With a bulk
   upload, the other traffic would take more than 55%, so ACKs were held at
   45%; the bulk upload rose from 0.10–0.16 to 0.18–0.21 Mbit/s. The
   measured ACK rate averaged a little below the minimum (340–375 kbit/s):
   after each hold, the download shaper climbs back gradually.
4. **One download is never held:** its ACKs stayed within the allowance.

## Files

- `raw/R<round>-<cap0|dyn45>/`: upload capture, probes, iperf3 JSON, phase
  marks, ISP backlog, daemon log (`cake-adapt.log.gz`), upload qdisc, test-log
  inode. `raw/cap.log` is the run sequence.

## Reproduce

With `testbed.sh`, `acks.sh`, `udpping` and the binary in
`/tmp/cake-adapt-test/` and the BPF object in `/lib/bpf/` on the VM:

```sh
./testbed.sh up
DAEMON=/tmp/cake-adapt-test/bin/cake-adapt ACK_SHARE=0 bash ./acks.sh ack-filter R1-cap0
DAEMON=/tmp/cake-adapt-test/bin/cake-adapt ACK_SHARE=0.45 bash ./acks.sh ack-filter R1-dyn45
./testbed.sh down
```
- `R<round>-<cap0|dyn45>.txt`: `tools/testbed/acks.py` per run.
