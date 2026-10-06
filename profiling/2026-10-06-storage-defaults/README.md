# Storage writes with the default log location (2026-10-06)

## Result

With the shipped defaults, cake-adapt writes nothing to persistent storage.
In two runs of 10.5 minutes, every block device showed 0 reads and 0 writes,
and so did a stopped window of 60 s before each run and 90 s after it. The
kernel's dirty and writeback memory stayed at 0 kB throughout.

The runs covered:
- load in each direction and both;
- a log export and reset;
- removal and recreation of the upload CAKE;
- idle sleep and wake;
- a time-based log rotation.

Every file the daemon opened for writing is in `/tmp/log`, which is RAM.
`/var/log` is the default log directory, and `/var` links to `/tmp`. The
second run added TCP measurement, the ACK share and the per-reply records,
and its log write path was the same.

Reads from persistent storage happen only when a program starts:
- shared libraries, when the daemon, `ip` and each new fping are launched;
- the TCP filter object, when the capture opens or is recreated.

They are served from the page cache; the device read counter did not move.

This complements the
[`2026-10-06-storage-reads`](../2026-10-06-storage-reads/README.md) audit:
- **Write tracing:** that audit's trace left out `write` and `writev`. These
  runs trace them.
- **Log path:** it put the log under `/tmp` by configuration. These runs use
  the default `/var/log`.
- **Length:** these runs last 10.5 minutes with load and lifecycle events,
  and the stopped window after each run is longer than the writeback delay.

## Setup

- x86 VM `192.168.56.2`, OpenWrt 25.12, with an ext4 root on `sda2`. `/tmp` is
  tmpfs and `/var -> tmp`, the same layout as the Filogic router. The router
  shows `/var -> tmp`, `/tmp` on tmpfs and `/overlay` on UBIFS, with no
  `log_file_path_override` and no syslog file (checked by the user).
- The daemon was the installed `05eb6b3` package, `/usr/sbin/cake-adapt`. It
  ran on the emulated testbed (`tools/testbed/testbed.sh`) with a 2.5 / 30
  Mbit/s bottleneck, so that the load stayed inside the VM.
- **Configs:** each is the shipped
  [`files/cake-adapt.config`](../../files/cake-adapt.config) with only these
  changes:
  - the section enabled;
  - the testbed interface;
  - rate adjustment on, with testbed rates;
  - `connection_active_thr_kbps 500`, below the 1,000 kbit/s upload minimum;
  - six testbed reflectors.

  All logging options keep their defaults: `log_to_file` 1, `debug` 1, the
  detailed records off, 2 MB and 10 minutes rotation, `/var/log`.
  - [`config-defaults`](tools/config-defaults): TCP measurement off, as shipped.
  - [`config-tcp`](tools/config-tcp): adds `tcp_delay_attribution 1`,
    `ul_congest_ack_share 0.45` and `output_processing_stats 1`.
- [`tools/stor.sh`](tools/stor.sh):
  - stops the installed service and runs 60 s stopped;
  - starts the daemon under
    `strace -f -yy` from exec, tracing file calls, reads, writes, maps,
    truncates and syncs;
  - runs 627 s of phases (`raw/<variant>/phases`), stops it, and waits 90 s;
  - samples `/proc/diskstats`, `Dirty`/`Writeback` and the storage IRQs every
    10 s;
  - restarts the service.

  The kernel writes dirty pages back after 30 s, checking every 5 s, so the
  90 s tail would catch late writes.
- This run logged to the default path on purpose. It did not use
  `/tmp/sqm-mon-test.log`.

## Block devices

Both variants, from [`defaults-summary.txt`](defaults-summary.txt) and
[`tcp-summary.txt`](tcp-summary.txt):

| Window | Seconds | Devices | Reads | Sectors read | Writes | Sectors written | Storage IRQs |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| stopped, before | 60 | sda, sda1, sda2 | 0 | 0 | 0 | 0 | 0 |
| running | 627 | sda, sda1, sda2 | 0 | 0 | 0 | 0 | 0 |
| stopped, after | 90 | sda, sda1, sda2 | 0 | 0 | 0 | 0 | 0 |

All 82 samples of each run are identical (`raw/<variant>/disk`).

Between the two runs, the service restart and the testbed's teardown and
setup caused two flush requests with no sectors written. That was outside
both runs.

## File operations

[`tools/stor.py`](tools/stor.py) classified every traced call by where its
file lives:

| Operation (defaults / tcp) | RAM (tmpfs) | Persistent storage | Kernel pseudo-files | Pipes and sockets |
| --- | ---: | ---: | ---: | ---: |
| write | 151 / 514 | **0 / 0** | 0 / 0 | 7,393 / 7,386 |
| open for writing | 3 / 3 | **0 / 0** | 3 / 3 (`/dev/null`) | – |
| read | 146 / 439 | 14 / 34 | 2 / 3 | 19,844 / 19,864 |

Files opened for writing:
- `/tmp/log/cake-adapt.log`;
- its `.old` copy;
- the SIGUSR1 export, `/tmp/log/cake-adapt_<time>.log.gz`;
- `/dev/null`, the pingers' stderr.

The writes to pipes are fping writing its replies to the daemon.

Reads and maps of persistent paths:
- the shared libraries of `cake-adapt`, `ip` and fping, at each exec;
- `/lib/bpf/cake-adapt-tcpdelay.o`, in the `tcp` variant only, read when the
  capture opened and when the upload CAKE was recreated.

`/etc/TZ` was opened 7,581 times (defaults) and 7,863 times (`tcp`). Each
open resolves to `/tmp/TZ` in RAM:
- fping accounted for 7,453 and 7,470 of them, opening it for each reply it
  prints with a timestamp. cake-autorate runs the same fping.
- cake-adapt opened it 128 times (defaults) and 393 times (with per-reply
  records) in 10.5 minutes, when formatting log timestamps.

## Log volume (RAM)

| Variant | Active log at the end | Export |
| --- | ---: | ---: |
| defaults | 12 kB | 62 kB |
| tcp (with per-reply records) | 935 kB | 278 kB |

The active log is limited to 2 MB plus a 2 MB `.old`. SIGUSR1 exports stay in
`/var/log` until reboot.

## Limits

- **Devices:** an x86 VM's ext4 disk, not the router's NAND. The paths and
  mounts that decide where writes go match the router; the flash itself was
  not measured.
- **Sources:** writes by other system components (syslog, the package
  manager, UCI commits) were not part of this test. A
  `log_file_path_override` on persistent storage would write there by design.

Collected by Claude directly at the user's request, outside the designated
agents of COLLABORATION.md.
