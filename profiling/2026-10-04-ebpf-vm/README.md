# VM run audit: eBPF evidence not established (2026-10-04)

This directory preserves a VM run attempted to validate
[`EBPF_REVIEW.md`](../../EBPF_REVIEW.md). Auditing the raw records showed that
the run does not establish any live eBPF estimator result. The earlier version
of this README incorrectly labeled fping RTT/2 results as TCP queue estimates.
That conclusion is withdrawn.

## What the raw data actually contains

In [`raw/vm-ebpf/cake-adapt.log`](raw/vm-ebpf/cake-adapt.log):

| Record | Count |
| --- | ---: |
| `TCP_QUEUE` | 0 |
| `DATA` | 5,574 |
| `SHAPER` | 1,119 |

The copied `queues.py` has separate paths for `TCP_QUEUE` and `DATA`.
When there are no TCP records, it prints only the table labeled
`fping per-direction delta (RTT/2, DATA records)`. The earlier report
misidentified that table as the output of the TCP estimator.

The previously reported correlations of 0.96 and 0.91 and download
false-alarm rate of 73.9% therefore do not demonstrate eBPF accuracy or
aggregation failure. Giving each direction RTT/2 can itself explain apparent
download delay during upload congestion.

The [phase marks](raw/vm-ebpf/phases-epoch) also contain repeated starts and
overlapping phases from interrupted attempts. They cannot be treated as one
clean sequence of independent phases. Per-phase statistics require a new run
or explicitly justified filtering; no corrected per-phase claim is made here.

## Run setup and verification gaps

The attempted workload used the isolated `cpe`, `isp`, and `inet` namespaces
on an OpenWrt 25.12 x86 VM with Linux 6.12.108. Requested settings were:

- 4 Mbit/s upload and 65 Mbit/s download bottlenecks;
- an upload capacity step from 4 to 2 Mbit/s and back;
- 3 Mbit/s upload and 20 Mbit/s download starting shaper rates;
- 1--5 Mbit/s upload and 10--50 Mbit/s download bounds;
- `TCP_ATTRIBUTION=1`, `BACKLOG=1`, and processing/load/shaper records.

The binary was copied from the VM's installed executable, without verifying
that it matched the reviewed source. An eBPF object was copied into the test
directory, but attachment at the daemon's runtime object path was not verified.
Copying an object and setting a configuration flag do not prove the filter ran.

The runner completed with daemon exit 0 and no reported remaining processes in
`cpe`; the namespaces were subsequently removed. The cleanup inode command
printed `missing`. There was no recorded inode before the run and no successful
inode comparison, so continuity of the authorized test log was not verified.

These gaps prevent this run from satisfying the requested live evidence check.
The userspace estimator reproductions in `EBPF_REVIEW.md` remain separate
evidence; this directory neither confirms nor disproves them.

## Preserved material

[`raw/vm-ebpf/`](raw/vm-ebpf/) contains the original daemon log, backlog
samples, probe output, phase marks, capacity settings, and iperf3 JSON.
[`raw/SHA256SUMS`](raw/SHA256SUMS) covers those copied files. The analysis
scripts are retained as used in the attempted run. No raw data was changed
during this correction.

From the repository root:

```sh
awk -F';' '
  { gsub(/^[ \t]+|[ \t]+$/, "", $1); counts[$1]++ }
  END {
    print "TCP_QUEUE:", counts["TCP_QUEUE"] + 0
    print "DATA:", counts["DATA"] + 0
    print "SHAPER:", counts["SHAPER"] + 0
  }
' profiling/2026-10-04-ebpf-vm/raw/vm-ebpf/cake-adapt.log

python3 profiling/2026-10-04-ebpf-vm/queues.py \
    profiling/2026-10-04-ebpf-vm/raw/vm-ebpf

sha256sum -c profiling/2026-10-04-ebpf-vm/raw/SHA256SUMS
```

## Requirements for a valid follow-up

Use a verified binary and object from the reviewed source, establish successful
filter attachment, and require actual `TCP_QUEUE` records before scoring TCP
estimates. Use unique run directories, bounded processes, clean phase marks,
and a recorded test-log inode before and after the run.

The intended deployment is approximately 80 Mbit/s download and 2 Mbit/s upload.
A relevant run must test download alone, upload alone, both under competition,
and recovery, with ACK and non-ACK rates measured separately. The 45% setting
must be evaluated as a dynamic allowance under competition, including the
current 5% headroom and utilization trigger, rather than as a permanent cap.

The new-flow, tuple-reuse, persistent-floor, receiver-delay, and CAKE byte
accounting claims require their own targeted live checks. A generic sustained
load run does not establish those cases.
