#!/usr/bin/env python3
"""Summarize raw diskstats, interrupt, trace, and log captures from run.sh."""
import argparse
import json
import os
import re
import sys
from pathlib import Path


class AuditError(Exception):
    pass


def read(path):
    try:
        return path.read_text(errors="replace")
    except OSError as exc:
        raise AuditError(f"cannot read {path.name}: {exc}") from exc


def diskstats(path):
    rows = {}
    for line in read(path).splitlines():
        fields = line.split()
        if len(fields) < 12:
            continue
        try:
            major, minor = int(fields[0]), int(fields[1])
            values = tuple(int(fields[i]) for i in (3, 5, 7, 9, 11))
        except ValueError as exc:
            raise AuditError(f"malformed diskstats row in {path.name}: {line}") from exc
        rows[(major, minor, fields[2])] = values
    if not rows:
        raise AuditError(f"no device rows in {path.name}")
    return rows


def interrupts(path):
    rows = {}
    cpu_count = 0
    for line in read(path).splitlines():
        if line.lstrip().startswith("CPU"):
            cpu_count = len(line.split())
            continue
        match = re.match(r"^\s*([^:]+):\s*(.*)$", line)
        if not match:
            continue
        fields = match.group(2).split()
        nums = []
        for item in fields:
            if not item.isdigit():
                break
            nums.append(int(item))
        if len(nums) != cpu_count:
            continue
        label = " ".join(fields[len(nums):]) or match.group(1).strip()
        rows[(match.group(1).strip(), label)] = (sum(nums),)
    if cpu_count < 1 or not rows:
        raise AuditError(f"invalid interrupt snapshot {path.name}")
    return cpu_count, rows


def counter_delta(before, after, label):
    if before is None or after is None:
        raise AuditError(f"missing {label} snapshot")
    if set(before) != set(after):
        raise AuditError(f"device/IRQ set changed during {label}")
    result = {}
    for key, old in before.items():
        new = after[key]
        if len(old) != len(new):
            raise AuditError(f"counter shape changed for {key}")
        if len(old) == 1:
            delta = (new[0] - old[0],)
        elif len(old) == 5:
            delta = tuple(n - o for o, n in zip(old[:4], new[:4]))
            delta += (old[4], new[4])
        else:
            raise AuditError(f"unexpected counter shape for {key}")
        monotonic_values = delta[:4] if len(old) == 5 else delta
        if any(value < 0 for value in monotonic_values):
            raise AuditError(f"counter decreased for {key}: {old} -> {new}")
        result[key] = delta
    return result


def uptime(path):
    fields = read(path).split()
    try:
        return float(fields[0])
    except (IndexError, ValueError) as exc:
        raise AuditError(f"invalid uptime in {path.name}") from exc


def whole_device(name, block_devices):
    return name in block_devices


def file_contents(root, name):
    return read(root / name)


def require_same(root, before, after, label):
    if file_contents(root, before) != file_contents(root, after):
        raise AuditError(f"{label} changed during the audit")


def symlinks(path):
    return sorted(re.findall(r"^(\S+) -> (\S+)$", read(path), re.M))


def service_without_pid(value):
    if isinstance(value, dict):
        return {key: service_without_pid(item) for key, item in value.items() if key != "pid"}
    if isinstance(value, list):
        return [service_without_pid(item) for item in value]
    return value


def trace_paths(root, trace):
    links = {}
    for line in file_contents(root, "path-links.txt").splitlines():
        match = re.search(r"(\S+) -> (\S+)$", line)
        if match:
            source, target = match.groups()
            if not os.path.isabs(target):
                target = os.path.normpath(os.path.join(os.path.dirname(source), target))
            links[source] = target
    mounts = []
    for line in file_contents(root, "mounts.txt").splitlines():
        fields = line.split()
        if len(fields) > 2:
            mounts.append((fields[1], fields[2]))
    events = set()
    unresolved = set()
    expression = re.compile(r"\b(openat|open|readv|read|pread64|mmap2|mmap)\(")
    for line in trace.splitlines():
        event_match = expression.search(line)
        if not event_match:
            continue
        event = event_match.group(1)
        found = set(re.findall(r'"(/[^"\\]*)"|<(/[^>]+)>', line))
        paths = {a or b for a, b in found}
        if paths:
            for path in paths:
                resolved = path
                for _ in range(len(links) + 1):
                    for prefix in sorted(links, key=len, reverse=True):
                        if resolved == prefix or resolved.startswith(prefix + "/"):
                            resolved = links[prefix] + resolved[len(prefix):]
                            break
                    else:
                        break
                mount_type = "unknown"
                matching = [(len(target), fs) for target, fs in mounts
                            if resolved == target or resolved.startswith(target.rstrip("/") + "/")]
                if matching:
                    mount_type = max(matching)[1]
                if mount_type not in {"tmpfs", "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2"}:
                    events.add((event, resolved))
        elif event in {"read", "readv", "pread64"}:
            descriptor = re.search(r"\(\s*([^,]+)", line)
            unresolved.add((event, descriptor.group(1).strip() if descriptor else "?"))
    return sorted(events), sorted(unresolved)


def snapshot_path(root, kind, stamp):
    candidate = root / f"{kind}-{stamp}.txt"
    if candidate.exists():
        return candidate
    # Historical phase() reused its global name in snapshot(), producing
    # after-before-<phase> filenames. Leave those raw captures untouched.
    if stamp.startswith("after-"):
        legacy = root / f"{kind}-after-before-{stamp[6:]}.txt"
        if legacy.exists():
            return legacy
    return candidate


def snapshot_pair(root, kind, start, end, parser):
    a = snapshot_path(root, kind, start)
    b = snapshot_path(root, kind, end)
    return parser(a), parser(b)


def show_interval(root, name, start, end, block_devices):
    disks_a, disks_b = snapshot_pair(root, "diskstats", start, end, diskstats)
    irqs_a, irqs_b = snapshot_pair(root, "interrupts", start, end, interrupts)
    if irqs_a[0] != irqs_b[0]:
        raise AuditError(f"CPU column count changed in {name}")
    disk_delta = counter_delta(disks_a, disks_b, name)
    irq_delta = counter_delta(irqs_a[1], irqs_b[1], name)
    t0 = uptime(snapshot_path(root, "uptime", start))
    t1 = uptime(snapshot_path(root, "uptime", end))
    if t1 <= t0:
        raise AuditError(f"invalid time ordering in {name}")
    print(f"\n{name}: {t1 - t0:.1f}s")
    for (major, minor, device), values in disk_delta.items():
        reads, sectors_read, writes, sectors_written = values[:4]
        if whole_device(device, block_devices):
            kind = "whole disk/device"
        elif any(device.startswith(base) and
                 re.fullmatch(r"(?:\d+|p\d+)", device[len(base):])
                 for base in block_devices):
            kind = "partition"
        else:
            kind = "other device"
        print(f"  {device} ({major}:{minor}, {kind}): reads={reads} "
              f"sectors_read={sectors_read} writes={writes} sectors_written={sectors_written}")
        if device == "sda":
            print(f"    I/Os in progress: {values[4]} -> {values[5]} (instantaneous gauge)")
    found_irq = False
    for (number, label), count in irq_delta.items():
        if re.search(r"ahci|ata_piix|sata|nvme|virtio.*(blk|scsi)|scsi|\bide\b", label, re.I):
            print(f"  IRQ {number} {label}: {count[0]}")
            found_irq = True
    if not found_irq:
        print("  storage IRQ labels: none identified")
    return disk_delta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="completed run.sh output directory")
    args = parser.parse_args()
    root = args.directory
    if not root.is_dir():
        raise AuditError("input directory does not exist")
    if not (root / "complete.txt").is_file():
        raise AuditError("capture is incomplete")
    block_devices = set(read(root / "sys-block.txt").split())
    if not block_devices:
        raise AuditError("missing /sys/block device list")

    # Startup is bounded by the pre-launch and post-warmup captures.
    startup = show_interval(root, "startup", "before-startup", "startup", block_devices)
    off_before = show_interval(root, "control-before", "before-control-before",
                               "after-control-before", block_devices)
    running = show_interval(root, "running", "before-running", "after-running", block_devices)
    off_after = show_interval(root, "control-after", "before-control-after",
                              "after-control-after", block_devices)

    console = read(root / "daemon-console.txt")
    load_count = len(re.findall(r"(?m)^LOAD;", console))
    if load_count <= 10:
        raise AuditError(f"expected more than 10 LOAD records, found {load_count}")
    trace_path = root / "strace.txt"
    trace = read(trace_path)
    tz_opens = len(re.findall(r"(?:openat|open)\([^\n]*?/etc/TZ", trace))
    traced_paths = re.findall(r"(?:openat|open|mmap2?|readv?|pread64)\([^\n]*"
                              r"(?:/etc/|/usr/|/lib/|/var/)[^\n]*", trace)

    print(f"\nmonitor LOAD records: {load_count}")
    print(f"traced /etc/TZ opens: {tz_opens}")
    print(f"path-resolved mapped/read/open calls: {len(traced_paths)}")
    persistent, unresolved = trace_paths(root, trace)
    print("distinct traced persistent-path calls:")
    for event, path in persistent:
        print(f"  {event}: {path}")
    print("unresolved-FD read calls:")
    for event, fd in unresolved:
        print(f"  {event}: fd {fd}")
    taskio = root / "taskio-running-start.txt"
    taskio_end = root / "taskio-running-end.txt"
    if taskio.exists() and taskio_end.exists():
        def read_bytes(path):
            for line in read(path).splitlines():
                if line.startswith("read_bytes:"):
                    return int(line.split()[1])
            raise AuditError(f"read_bytes absent in {path.name}")
        try:
            a, b = read_bytes(taskio), read_bytes(taskio_end)
            if b < a:
                raise AuditError("task read_bytes counter decreased")
            print(f"daemon read_bytes delta: {b - a}")
        except AuditError as exc:
            print(f"daemon read_bytes: unavailable ({exc})")
    else:
        print("daemon read_bytes: unavailable (kernel did not expose task I/O accounting)")

    read_activity = any(values[0] or values[1] for values in running.values())
    if not read_activity and load_count > 10 and tz_opens > 1:
        print("classification: bounded no-recurring-device-read observation")
    elif read_activity:
        print("classification: device reads observed; attribution requires correlation")
    else:
        print("classification: inconclusive; required trace/log evidence is incomplete")
    print("IRQ deltas corroborate activity only; control windows and task/path evidence matter.")
    print("VM block counters do not establish physical host media or router NAND behavior.")
    initial_inode = read(root / "log-inode-initial.txt").strip()
    final_inode = read(root / "log-inode-final.txt").split()[0]
    if initial_inode != final_inode:
        raise AuditError("authorized log inode changed")
    print("authorized log inode preserved: yes")
    qdisc_before = read(root / "qdisc-before.txt")
    qdisc_after = read(root / "qdisc-after.txt")
    if qdisc_before != qdisc_after:
        raise AuditError("full qdisc dump changed after service restoration")
    print("restored qdisc dump exactly matches: yes")
    require_same(root, "packages-before.txt", "packages-after.txt", "installed package list")
    require_same(root, "installed-sha256.txt", "installed-sha256-after.txt", "installed file hashes")
    require_same(root, "mounts.txt", "mounts-after.txt", "mount table")
    if symlinks(root / "path-links.txt") != symlinks(root / "path-links-after.txt"):
        raise AuditError("RAM path symlinks changed during the audit")
    require_same(root, "daemon-args-before.txt", "daemon-args-after.txt", "service command arguments")
    was_running = read(root / "service-running-initial.txt").strip() == "1"
    now_running = bool(read(root / "pids-after.txt").strip())
    if was_running != now_running:
        raise AuditError("service running state was not restored")
    if read(root / "service-enabled-initial.txt").strip() != read(root / "service-enabled-after.txt").strip():
        raise AuditError("service enabled state changed")
    try:
        before_service = service_without_pid(json.loads(read(root / "service-before.json")))
        after_service = service_without_pid(json.loads(read(root / "service-after.json")))
    except json.JSONDecodeError as exc:
        raise AuditError(f"service state response is not valid JSON: {exc}") from exc
    if before_service != after_service:
        raise AuditError("service configuration/state differs after restoration")
    print("packages, installed hashes, service settings, mounts, and symlinks preserved: yes")
    # Keep named variables used to make the control windows explicit in output.
    _ = startup, off_before, off_after
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AuditError as exc:
        print(f"storage audit: {exc}", file=sys.stderr)
        sys.exit(2)
