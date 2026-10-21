# Native timestamps with jittered capacity drops (2026-10-21)

**Result:** with the capacity drops at random points of the controller's
sawtooth, native-ts no longer loses on upload-step p95; it is better (median
131 against 153 ms). The gate still fails, now on three different metrics,
each by a small margin. Together with the fixed-drop batch, which failed on a
different metric, native-ts looks equivalent to fping-ts within the testbed's
noise. fping stays the default; accepting native-ts is the user's decision.

Why this run: in the fixed-drop batch (`../2026-10-20-native-icmp-gate/`),
native-ts failed only upload-step p95. `drop-analysis.md` there traced that to
where each sawtooth happened to be at the single, fixed drop instant. This
rerun keeps the binary, workloads and phases, and changes only the upload-step
phase (`run.sh`, a copy of Codex's harness with this one change):

- three 6-s drops to 4 Mbit/s instead of one 20-s drop;
- each drop after a random 5–8.5 s at full rate, longer than the sawtooth's
  ~3.2-s period;
- the waits drawn per case, kept in each case's `drop-waits`.

3 timestamp pairs on VM .2, alternating which method goes first, all on one
boot (no panic). Binary SHA256
`cc140526769d2ea5a3295557a80081e764591bcf7abd44c1c5a9a5992e93e52b`
(`cde7c67`/`a3c13a6`). Scored with `../2026-10-20-native-icmp-gate/gate.py
--pairs ts`; per-drop view from [`drops.py`](drops.py).

## Gate

| Metric | fping-ts | native-ts | Median Δ | fping range | |
| --- | --- | --- | ---: | ---: | --- |
| Upload-step added RTT p95 (ms) | 153.1, 152.3, 202.3 | 144.8, 122.8, 131.3 | −21.8 | 50.0 | pass |
| Upload-step added RTT p50 (ms) | 2.8, 2.5, 3.8 | 5.4, 4.9, 3.3 | +2.1 | 1.3 | **fail** |
| Bidirectional >15 ms (% of probes) | 70.9, 69.6, 72.8 | 62.6, 74.6, 76.1 | +3.65 | 3.2 | **fail** |
| Bidirectional upload goodput (Mbit/s) | 6.66, 6.66, 6.66 | 6.64, 6.58, 6.62 | −0.04 | 0.01 | **fail** |

The other 17 metrics pass. The goodput failure is 0.6%, against an fping range
of 0.01 Mbit/s: three equal values make the rule unforgiving. The full table
is in [`gate.json`](gate.json).

Combined daemon + pinger CPU, % of one core: fping-ts 1.09/1.58/1.00,
native-ts 1.02/1.81/0.77. Both rose in round 2, and native-ts's saving is
smaller than in the fixed-drop batch (0.60–0.69 against 0.89–1.07); this run
does not explain why.

## Per drop

RTT p95 of the independent probe in the 3 s after each drop, 9 drops per
method:

| | Median | Range | Median s to first bufferbloat | Median s to shaper ≤ 4000 kbit/s |
| --- | ---: | --- | ---: | ---: |
| fping-ts | 247 ms | 195–305 | 0.37 | 1.02 |
| native-ts | 212 ms | 161–509 | 0.32 | 0.98 |

The shaper rate at the drop does not predict the peak (correlation 0.04 over
the 18 drops), so the sawtooth explanation in `drop-analysis.md` is at most
part of the story. Two native-ts drops peaked high (400 and 509 ms) although
their detection and cuts were normal; their one-way upload delay reached 397
and 507 ms, meaning a larger queue in the emulated bottleneck at the moment of
the drop. Nine drops per method cannot tell whether native-ts has a heavier
tail.

## Runs and limits

A first attempt failed after about a minute per case, before any load, because
BusyBox `sleep` takes whole seconds; the waits now use `bash read -t`. Its
logs are not kept here. The test log kept its inode in every case. Owned
namespaces and payloads were removed afterwards, and the installed controller
was unchanged.

Re-score with:

```sh
tar -xJf raw.tar.xz -C /tmp/jitter
python3 drops.py /tmp/jitter
python3 ../2026-10-20-native-icmp-gate/gate.py /tmp/jitter --pairs ts
```
