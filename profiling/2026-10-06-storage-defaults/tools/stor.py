#!/usr/bin/env python3
"""stor.py RUN_DIR: block-device activity per window, and every traced file
operation of the daemon and its children classified by where its file lives.

Windows come from the `@ <time> <label>` blocks in `disk`: control-before
(daemon stopped, 60 s), running (from the end of control-before to the start
of control-after) and control-after (daemon stopped, 90 s, longer than the
kernel's 30 s dirty expiry plus its 5 s writeback interval).
"""
import collections
import gzip
import re
import sys
from pathlib import Path

run = Path(sys.argv[1])

# --- block devices ---------------------------------------------------------
snapshots = []  # (time, label, {device: (reads, sectors_read, writes, sectors_written)}, dirty, writeback, irqs)
current = None
for line in (run / "disk").read_text().splitlines():
    if line.startswith("@ "):
        _, stamp, label = line.split(maxsplit=2)
        current = [int(stamp), label, {}, None, None, {}]
        snapshots.append(current)
        continue
    fields = line.split()
    if line.startswith("Dirty:"):
        current[3] = int(fields[1])
    elif line.startswith("Writeback:"):
        current[4] = int(fields[1])
    elif ":" in fields[0]:
        # One IRQ line: its per-CPU counts precede the controller name.
        current[5][fields[0]] = sum(int(f) for f in fields[1:] if f.isdigit())
    elif len(fields) >= 14:
        current[2][fields[2]] = tuple(int(fields[i]) for i in (3, 5, 7, 9))


def snapshot(label):
    return next(s for s in snapshots if s[1] == label)


def delta(first, second):
    return {d: tuple(b - a for a, b in zip(first[2][d], second[2][d])) for d in first[2] if d in second[2]}


def irq_delta(first, second):
    return sum(second[5][i] - first[5][i] for i in first[5] if i in second[5])


windows = [
    ("control-before (stopped)", snapshot("control-before-start"), snapshot("control-before-end")),
    ("running", snapshot("control-before-end"), snapshot("control-after-start")),
    ("control-after (stopped)", snapshot("control-after-start"), snapshot("control-after-end")),
]
print("## Block devices (reads, sectors read, writes, sectors written)\n")
print("| Window | Seconds | Device | Reads | Sectors read | Writes | Sectors written | Storage IRQs |")
print("|---|---:|---|---:|---:|---:|---:|---:|")
for name, first, second in windows:
    for device, values in sorted(delta(first, second).items()):
        print(f"| {name} | {second[0] - first[0]} | {device} | " + " | ".join(map(str, values)) + f" | {irq_delta(first, second)} |")
dirty = [s[3] for s in snapshots if s[3] is not None]
writeback = [s[4] for s in snapshots if s[4] is not None]
print(f"\nDirty kB max {max(dirty)}, Writeback kB max {max(writeback)} over {len(snapshots)} samples")
active = []
for first, second in zip(snapshots, snapshots[1:]):
    for device, values in delta(first, second).items():
        if any(values):
            active.append(f"{second[0] - snapshots[0][0]:>4} s {first[1]}->{second[1]} {device} {values}")
print("10 s intervals with any device activity:", *(active or ["none"]), sep="\n  ")

# --- traced file operations ------------------------------------------------
CALLS = {
    "write": ("write", "writev", "pwrite64", "pwritev", "truncate", "ftruncate", "fsync", "fdatasync", "sync", "syncfs", "sync_file_range"),
    "read": ("read", "readv", "pread64"),
    "map": ("mmap2",),
}
line_pattern = re.compile(r"^(\d+) +([\d.]+) (\w+)\((.*)\) += (-?\d+|\?)(?:<([^>]*)>)?")
programs = {}
start = None
table = collections.Counter()
persistent = collections.defaultdict(lambda: [0, None, None, set()])
opened_for_write = collections.Counter()


def place(path):
    if path is None:
        return "unknown"
    if path.startswith(("pipe:", "socket:", "anon_inode:", "UNIX", "NETLINK", "TCP", "UDP")) or "socket" in path:
        return "pipe/socket/anon"
    if path.startswith(("/tmp", "/var", "/dev/shm", "/run")):
        return "RAM (tmpfs)"
    if path.startswith(("/proc", "/sys", "/dev")):
        return "kernel pseudo-file"
    if path.startswith("/"):
        return "persistent storage"
    return "unknown"


opener = gzip.open if (run / "trace.gz").exists() else open
with opener(run / ("trace.gz" if (run / "trace.gz").exists() else "trace"), "rt") as trace:
    for line in trace:
        m = line_pattern.match(line)
        if not m:
            continue
        pid, stamp, call, args, result, resolved = m.groups()
        stamp = float(stamp)
        start = start if start is not None else stamp
        if call == "execve" and result == "0":
            programs[pid] = re.match(r'"([^"]+)"', args).group(1).rsplit("/", 1)[-1]
        program = programs.get(pid, "?")
        fd_path = re.match(r"-?\d+<([^>]*)>", args)
        quoted = re.match(r'(?:AT_FDCWD|\d+<[^>]*>)?,? ?"([^"]*)"', args)
        if call in ("open", "openat"):
            path = resolved or (quoted.group(1) if quoted else None)
            kind = "open"
            if re.search(r"O_(WRONLY|RDWR|CREAT|TRUNC|APPEND)", args):
                kind = "open for writing"
                opened_for_write[(program, path)] += 1
        elif call in CALLS["write"] or call in CALLS["read"] or call in CALLS["map"]:
            kind = next(k for k, v in CALLS.items() if call in v)
            if call == "mmap2":
                fd_path = re.search(r", (-?\d+)<([^>]*)>, ", args)
                path = fd_path.group(2) if fd_path else None
                if path is None:
                    continue  # anonymous memory
            else:
                path = fd_path.group(1) if fd_path else None
        else:
            kind = "metadata (stat, access, exec, ...)"
            path = resolved or (quoted.group(1) if quoted else None)
        where = place(path)
        table[(kind, where)] += 1
        if where == "persistent storage":
            entry = persistent[(kind, path)]
            entry[0] += 1
            entry[1] = stamp - start if entry[1] is None else entry[1]
            entry[2] = stamp - start
            entry[3].add(program)

print("\n## Traced file operations by where the file lives\n")
places = ["RAM (tmpfs)", "persistent storage", "kernel pseudo-file", "pipe/socket/anon", "unknown"]
kinds = ["write", "open for writing", "read", "map", "open", "metadata (stat, access, exec, ...)"]
print("| Operation | " + " | ".join(places) + " |")
print("|---|" + "---:|" * len(places))
for kind in kinds:
    print(f"| {kind} | " + " | ".join(str(table[(kind, p)]) for p in places) + " |")
print("\nFiles opened for writing:", *(f"{n} x {p} ({prog})" for (prog, p), n in sorted(opened_for_write.items())) or ["none"], sep="\n  ")
print("\nPersistent-storage paths (count, first and last seconds after exec, programs):")
for (kind, path), (count, first, last, progs) in sorted(persistent.items(), key=lambda i: (i[0][0], -i[1][0])):
    print(f"  {kind:<36} {count:>5}  {first:7.1f}-{last:7.1f}s  {path}  [{', '.join(sorted(progs))}]")
