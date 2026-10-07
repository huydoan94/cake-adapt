# Bufferbloat simulator

A closed-loop, deterministic simulator for comparing controller variants. It
links the real `src/controller/controller.c` and `src/latency/tracker.c`, so it
measures the code the daemon runs.

```sh
make -C tools/simulator run                   # summary, RTT/2 and one-way delays
build/tools/simulator/simulate --owd --trace steady > steady.csv
```

## Model

- **Bottleneck:** each direction has an ISP FIFO whose buffer holds 500 ms at
  the scenario's peak capacity, draining at a capacity that varies over time.
- **Shaping:** CAKE limits the flow to the controller's rate. Greedy TCP fills
  the shaper while the bottleneck buffer has room. Once the buffer overflows,
  losses hold TCP to the capacity, leaving a standing queue.
- **Shaper position:** upload CAKE sits before the bottleneck, so its achieved
  rate is what enters it. Download CAKE sits on the IFB after the bottleneck,
  so its achieved rate is what the bottleneck delivers.
- **Pings:** 6 reflectors with base delays of 5–25 ms and up to 2 ms of jitter,
  one reply every 50 ms, each going through the real tracker and controller.
  `--owd` gives separate one-way delays (`fping-ts`, IRTT). The default reports
  RTT/2 in both directions, as `fping` does. Delays reach the tracker in
  whole microseconds, rounded to the nearest, as the daemon's parsers deliver
  them.
- **Achieved rate:** sampled every 200 ms, as in the daemon.

The first four scenarios have download bounds of 20/60/100 Mbit/s and upload
bounds of 5/12/20 Mbit/s (minimum/base/maximum). The asymmetric ones, where
upload is the narrow, bloated direction, have 50/150/250 down and 3/8/20 up:

| Scenario | Capacity |
| --- | --- |
| `steady` | 70 down / 15 up; download loaded from 10 s, upload from 50 s |
| `step-down` | download 80 → 35 at 60 s → 80 at 120 s; upload idle |
| `sine` | 60 ± 25 down (60 s period), 12 ± 4 up (45 s period), both loaded |
| `random-walk` | 30–90 down, 6–18 up, changing every 100 ms; 5 seeds |
| `asym-steady` | 200 down / 10 up, both loaded |
| `upload-step` | upload 15 → 6 at 60 s → 15 at 120 s; download idle |
| `asym-walk` | 150–250 down, 4–12 up, changing every 100 ms; 5 seeds |

## Metrics

Only loaded periods count:

- **Delay percentiles:** p50/p95/p99 and maximum of the bottleneck queueing
  delay, which is the delay added by bufferbloat.
- **Time above thresholds:** share of time above 15 ms and above 30 ms.
- **Capacity used:** delivered bits over available bits.

## Limits

TCP is a fluid approximation: it fills whatever the shaper allows, and backs
off only when the bottleneck buffer is full. There is no slow start, no
per-flow behavior, and no CAKE-internal queueing. Results compare controller
variants against each other; they are not predictions for a real line. A
single-seed tail such as `step-down`'s p99 is sensitive to small input
changes: rounding instead of truncating the simulated delays moves it between
about 670 and 860 ms, because a cut lands at a different moment. A
promising variant still needs confirmation on the VM with an emulated bloated
link.
