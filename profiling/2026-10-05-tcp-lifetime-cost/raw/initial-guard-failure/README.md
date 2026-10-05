Initial short-pair diagnostic. The runner correctly stopped on a candidate
count mismatch (804 filter runs for 803 injected packets). A manual post-run
check confirmed namespace cleanup, log inode 183, daemon PID 2646, and the
original qdisc hash. The likely extra event was removed as a confound by
turning off IPv6 only in the private test namespace; see `corrected-three-pairs/`.
