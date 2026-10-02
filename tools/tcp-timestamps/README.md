# Per-direction queueing delay from TCP timestamps

Passive estimates of the upstream and downstream ISP queues from TCP flows seen
at the router, to tell which direction is bloated when `fping` reports only
RTT/2 and ICMP timestamps (`fping-ts`) are filtered.

For each flow with TCP timestamps, relative to the flow's minimum:

- **downstream:** arrival time − remote TSval × remote tick changes only with
  the downstream one-way delay;
- **upstream:** when an arriving packet echoes (TSecr) a TSval we sent, remote
  TSval × tick − our departure time of that TSval changes only with the
  upstream one-way delay. Delayed ACKs only add, so each 100 ms window keeps its
  minimum.

The remote tick is fitted per flow and snapped to standard clock rates (1, 4,
10, 100 ms) when within 5%. A growing queue stretches arrival times and biases
the raw fit; without snapping, a 2% error drifts by about 400 ms over 20 s.

```sh
python3 estimate.py DIR   # DIR holds cwan.pcap, backlog and phases from tools/testbed/capture.sh
```

## Validation (2026-10-02, `validation-2026-10-02/`)

The emulated ISP's real backlog was the ground truth. 170,484 TCP packets were
captured with none dropped.

| Phase | Upstream true / estimated | Downstream true / estimated | Attribution correct |
| --- | --- | --- | --- |
| upstream bloated | 392 / 390 ms | 0 / 0.2 ms | 100% |
| downstream bloated | 0 / 0.2 ms | 48 / 50 ms | 97% |
| both shapers too high | 274 / 273 ms | 0 / 0.4 ms | 88% |
| nothing bloated | 0 / 0.2 ms | 0 / 0.2 ms | 100% |

In the third phase the downstream queue never built: upload bloat delayed
download's ACKs. The estimate saw that, so a controller using it would cut only
upload.

The raw 20 MB capture is not kept; `result.txt` is the estimator's output on
it. Not yet tested: remote clocks other than Linux's 1 ms, clock drift, flows
without TCP timestamps, and middleboxes that rewrite them.
