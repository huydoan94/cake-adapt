# TCP-delay review map (2026-10-04)

[`index.html`](https://raw.githack.com/huydoan94/cake-adapt/further-integration/profiling/2026-10-04-review-map/index.html) is a one-page visual summary of
[`EBPF_REVIEW.md`](../../EBPF_REVIEW.md), checked against the working tree on
2026-10-04 (Fix 1 committed, phase 1 for the tuple-reuse finding uncommitted).
Open it in a browser; it needs no build step.

It has three parts:

- **Measurement path:** each finding pinned where it happens, numbered by the
  review's sections (2-6). N1-N4 are points the review does not make.
- **On the wire:** one router/server ladder per case, comparing what is really
  on the link with what cake-adapt reads. The millisecond values are the
  review's synthetic examples; findings 3-5 were rerun with the review's own
  harness against this working tree and still reproduce.
- **N1 chart:** the download shaper rate from [`probe_split.c`](probe_split.c).

## N1: a wrong split raises the congested direction

The probe drives `controller_update()` from today's source. Download is loaded
at 81% of an 8 Mbit/s shaper and fping reports 50 ms of added delay each way,
as a download queue would. It runs twice, 40 samples 0.5 s apart:

| TCP estimate | Download shaper after 20 s |
| --- | ---: |
| none (delivery-rate heuristic) | 5,831 kbit/s |
| download 0 ms, upload 20 ms (wrong) | 8,706 kbit/s |

With the wrong estimate, download gets none of the delay, its congestion
clears, and the high-load increase path, which does not check attribution,
raises it.

Reproduce from the repository root:

```sh
cc -std=c11 -O2 -I. -Isrc -Itests -Wl,--wrap=calloc \
  profiling/2026-10-04-review-map/probe_split.c \
  src/controller/controller.c src/common/helpers.c -o /tmp/probe_split
/tmp/probe_split
```

It prints the download shaper rate after each sample for both runs, then the
two final values.
