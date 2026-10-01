# Profiling and behavior evidence

Raw evidence from the OpenWrt 25.12 x86 test VM, kept with the code it
measured. Each directory has its own README with the method, the result, and
the scripts that produced it.

| Directory | What it shows |
| --- | --- |
| [`2026-09-30`](2026-09-30/README.md) | **Current.** End-to-end run and CPU profile after the optimization pass: flame graphs for five workloads, CPU before and after, syscall counts, and the qdisc lifecycle, idle, signal and shutdown checks. [Dashboard](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/2026-09-30/index.html). |
| [`cake-autorate-resources`](cake-autorate-resources/README.md) | CPU, memory, process and reaction-time comparison with cake-autorate `ac75f49` under the same workload: about 5× less CPU (7.6× with all records enabled), a tenth of the memory, and a 7× faster median reply-to-decision time. |
| [`controller-comparison`](controller-comparison/README.md) | Side-by-side runs with cake-autorate `ac75f49` and the two traces the controller replays with no mismatching decisions. |
| [`2026-09-26`](2026-09-26/README.md) | Superseded first flame graphs, from before the optimization pass and the source restructure. |

Open `index.html` locally, or use the
[hosted index](https://raw.githack.com/huydoan94/cake-adapt/main/profiling/index.html).
The hosted links read the `main` branch. They use raw.githack because GitHub's
raw-file host disables the JavaScript inside FlameGraph SVGs that drives hover,
search, and click-to-zoom.
