#!/usr/bin/env python3
"""Classify folded cake-adapt stacks: interrupt work versus the daemon's own work.

2026-10-21: perf now also samples the daemon's pinger children, so stacks of
the fping process form their own category, and the native ICMP probes'
raw-socket sends and receives are counted apart from the netlink dump.
"""
import sys

CATEGORIES = (
    ("native ICMP probes (raw sockets)", ("raw_sendmsg", "raw_recvmsg", "rawv6_sendmsg", "rawv6_recvmsg", "__sys_sendto", "__ia32_sys_sendto", "__x64_sys_sendto")),
    ("interrupt/softirq (packet processing charged to the task)", ("handle_softirqs", "irq_exit", "sysvec_", "do_softirq", "__irqentry")),
    ("epoll wait (idle in the event loop)", ("epoll_pwait", "do_epoll_wait")),
    ("BPF ring buffer drain and counters", ("ring_buffer__", "ringbuf_", "__sys_bpf", "map_lookup_elem", "bpf_map_")),
    ("netlink qdisc dump", ("sendmsg", "recvmsg", "netlink_", "tc_dump_qdisc", "rtnetlink")),
    ("pinger pipe read", ("ksys_read", "__ia32_sys_read", "__x64_sys_read", "pipe_read")),
    ("log file write", ("writev", "ksys_write", "vfs_write")),
    ("process lifecycle", ("do_exit", "wait4", "clone", "execve")),
)

def classify(stack):
    # stackcollapse-perf.pl puts the process name first.
    if stack.split(";", 1)[0] == "fping":
        return "fping process (the pinger child)"
    for name, needles in CATEGORIES:
        if any(needle in stack for needle in needles):
            return name
    frames = stack.split(";")
    if any(frame.endswith("_[k]") for frame in frames[1:]):
        return "other kernel"
    return "userspace (cake-adapt, libc, libubox)"

for path in sys.argv[1:]:
    totals = {}
    for line in open(path):
        stack, count = line.rsplit(" ", 1)
        name = classify(stack)
        totals[name] = totals.get(name, 0) + int(count)
    all_samples = sum(totals.values())
    print(f"{path.split('/')[-1].replace('.folded', '')}: {all_samples} samples")
    for name, count in sorted(totals.items(), key=lambda item: -item[1]):
        print(f"  {100 * count / all_samples:5.1f}%  {count:4}  {name}")
