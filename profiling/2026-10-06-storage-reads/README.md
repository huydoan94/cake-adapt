# Bounded storage-read audit

This one-batch audit checks whether the installed baseline daemon causes
recurring virtual block-device reads during active monitoring and logging. It
uses stopped-control (15 s), running observation (30 s after a 3 s startup
warmup), then stopped-control (15 s), with a path-resolved syscall trace
attached during the running window. Startup counters are reported separately.
There is no traffic generator or soak test.

The harness is `tools/run.sh`; `tools/analyze.py` summarizes the collected raw
captures locally. Run the harness as root on the authorized x86 test VM only.
Transfer `tools/run.sh` to the VM, create a new empty directory under `/tmp`,
and run:

```sh
mkdir /tmp/storage-read-run
/tmp/run.sh /tmp/storage-read-run
```

The script refuses a nonempty directory. It records the original service
state, stops only the installed service, starts the installed executable with
an isolated observation-only UCI configuration, then restores the original
running state. It does not install or replace files. Upload and download
adjustment and minimum-rate enforcement are disabled; it uses `fping` against
the authorized `192.168.56.1` reflector and enables detailed logging with a
2 KiB rotation limit. TCP delay attribution is enabled to exercise the
available passive measurement path. Existing CAKE qdiscs are captured before
and after restoration for comparison.

The authorized `/tmp/sqm-mon-test.log` is created only when absent, truncated
in place, and hard-linked as the isolated instance log. Its inode is captured
and checked at exit. The harness checks open descriptors by access mode so a
reader such as `tail -f` is allowed while a writer prevents the run. All other
captures remain in the output directory on tmpfs. The trace attaches to the
normally launched daemon, is detached with `SIGINT`, and has a bounded cleanup
path. The daemon and recorded direct `fping` children are stopped before the
final control interval.

Copy the completed directory off the VM, then run the analyzer locally:

```sh
python3 tools/analyze.py /path/to/storage-read-run
```

It reports per-window read requests, sectors read, writes, sectors written,
storage IRQ deltas, and the instantaneous `sda` I/Os-in-progress gauge. Whole
disks and partitions are listed separately; their counts are never added
together. IRQs corroborate activity but cannot attribute it to the daemon.
Task `read_bytes` is included when `/proc/PID/io` exists; `rchar` and `syscr`
are not storage-byte measures. If task I/O accounting is unavailable, the
analyzer reports that limitation and continues.

Zero device read and sector-read deltas in the running window, with more
than ten `LOAD` records and repeated traced `/etc/TZ` opens, support only a
bounded no-recurring-device-read observation. Nonzero deltas show device
activity but do not by themselves establish daemon causation. Correlate startup
and stopped controls, task accounting, traced paths, and storage IRQ changes.
Do not interpret missing sampled calls as proof that no reads occurred.

The result describes virtual block-device requests in this VM. It does not
establish physical host-media or router NAND behavior, and says nothing about
sustained traffic load or a real router. Do not run this concurrently with
another controller of the same qdiscs.

## Completed x86 VM batch

One batch ran against the already-installed baseline package on
`192.168.56.2`; no SDK build or package replacement was involved. The command
was:

```sh
timeout 150 ssh root@192.168.56.2 'sh /tmp/cake-adapt-storage-20261006-run.sh /tmp/cake-adapt-storage-20261006-data'
```

The measured windows were 3 s startup, 15 s stopped control, 30 s running,
and 15 s stopped control. `result.txt` contains the analyzer output. All
captured devices, including `sda`, `sda1`, `sda2`, the loop devices, and `sr0`,
had zero completed reads, sectors read, writes, and sectors written in each
window. The `sda` in-progress gauge remained 0. Storage IRQ deltas were zero.
The run logged 175 `LOAD;` records and traced 32 `/etc/TZ` opens; path
resolution placed those accesses at `/tmp/TZ` on tmpfs. No persistent-path
open, read, or mmap was resolved. The only unassociated reads shown were on
timerfd and pipe descriptors. The kernel did not provide `read_bytes` in the
daemon's `/proc/PID/io` record, so that process-level metric is unavailable.

The before/after qdisc dumps, installed file hashes, package list, service
settings and command arguments, mount table, symlink entries, and authorized
log inode all matched. The captured phase filenames expose a harness bug in
the executed copy: `snapshot()` reused `phase()`'s global `name`, so the three
phase end captures were named `after-before-<phase>`. The analyzer maps those
historical names without changing the raw captures. The corrected harness now
uses distinct `snapshot_name` and `phase_name` variables. `raw/run-executed.sh`
preserves the exact pre-fix script used for the batch.

Remote cleanup confirmed the installed service restored as PID 12141 with its
`fping` child PID 12173, no audit-owned daemon or pinger remaining, authorized
log inode 288 unchanged, and only audit-owned temporary files removed. The
unrelated profiling directory was stashed before the audit and is to be
restored as uncommitted user work after this finding is committed.

Source inspection indicates that recurring netlink counters, BPF ring/map
accesses, fping pipes, and timerfds use kernel memory; optional CPU statistics
read `/proc/stat`, and UCI configuration is loaded once from the service's
generated `/tmp` copy. The BPF object is read when capture opens or recreates,
while executable and shared-library loading can read storage at daemon or
pinger startup/restart. No recurring flash-file reads were identified during
stable operation. Filogic is expected to have the same steady-state result
given its matching RAM-backed timezone and log layout; this is an extrapolation,
not a physical NAND measurement.

This is a bounded idle-monitoring and log-rotation observation. It includes no
traffic generation and does not establish physical host-media or router NAND
behavior.
