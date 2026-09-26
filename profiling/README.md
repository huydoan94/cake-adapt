# Raw VM flame graphs

[View the rendered profiling dashboard](https://html-preview.github.io/?url=https://github.com/huydoan94/cake-adapt/blob/main/profiling/index.html),
or open `index.html` locally. The hosted link reads the `main` branch and
therefore reflects the latest version pushed to GitHub.

These are standard interactive flame graphs generated directly from `perf
script` stacks. Frame width is proportional to sampled `cpu-clock` time. Hover
over a frame for its value and click it to zoom.

- [Userspace CPU flame graph](flamegraph-user.svg): 180-second `cpu-clock:u`
  capture, 64 samples.
- [Mixed user/kernel flame graph](flamegraph-mixed.svg): 60-second `cpu-clock`
  capture, 408 samples.
- [Conclusion graph](conclusion.svg): interpretation derived from the two raw
  captures, kept separate from the flame graphs.

No interpretation or manually assigned call relationships are added to either
graph.

## Raw inputs

- `raw/user.perf-script.txt`: unmodified userspace `perf script` output.
- `raw/mixed.perf-script.txt`: unmodified mixed user/kernel `perf script`
  output.
- `raw/user.folded`: userspace stacks collapsed by `stackcollapse-perf.pl`.
- `raw/mixed.folded`: mixed stacks collapsed by `stackcollapse-perf.pl`.

The capture used the normal optimized OpenWrt x86 build flags plus `-g3
-gdwarf-4 -fno-omit-frame-pointer` so application frames could be resolved. It
ran under repeated validated downloads. The installed package was not replaced.

The graphs were generated with the official
[FlameGraph](https://github.com/brendangregg/FlameGraph) scripts:

```sh
stackcollapse-perf.pl --all raw/user.perf-script.txt > raw/user.folded
flamegraph.pl --countname nanoseconds --colors hot raw/user.folded \
    > flamegraph-user.svg

stackcollapse-perf.pl --all raw/mixed.perf-script.txt > raw/mixed.folded
flamegraph.pl --countname nanoseconds --colors hot raw/mixed.folded \
    > flamegraph-mixed.svg
```
