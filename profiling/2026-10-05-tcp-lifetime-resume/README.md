# Tuple-lifetime resumption: synchronization remains blocked

Finding 2 was resumed on 2026-10-05. The saved implementation was inspected,
not restored. A helper-mediated nonblocking lock was considered as a bounded
replacement for its unsupported i386 BPF atomics. Source review found that
this replacement can discard the very handshake needed to invalidate an old
lifetime. It therefore fails the finding's acceptance criteria.

The user's existing stop condition applies: stop if finishing requires a larger
redesign, and revert the attempted tuple implementation while preserving policy
and evidence. No production changes were made in this attempt, so no rollback
was necessary. No VM was accessed and no package was built or installed.

## Kernel constraints checked

- The saved [JIT diagnosis](../2026-10-05-tcp-lifetime-deferred/kernel-jit-excerpt.txt)
  establishes that the tested Linux 6.12.108 i386 JIT rejects BPF atomic
  instructions. The previous candidate's four compare-and-swap sites cover
  both allocation and lifecycle publication.
- Linux v6.12's [`check_map_prog_compatibility()`](https://github.com/torvalds/linux/blob/v6.12/kernel/bpf/verifier.c)
  explicitly rejects maps containing a BPF spin lock for socket-filter programs.
  A spin-lock helper is therefore not a drop-in replacement. This is a source
  check, not a new probe of the installed kernel.
- Linux v6.12's [hash-map implementation](https://github.com/torvalds/linux/blob/v6.12/kernel/bpf/hashtab.c)
  checks `BPF_NOEXIST` and inserts under the bucket lock. A non-LRU map entry
  can consequently act as an owner-only acquisition marker, provided only a
  successful acquirer deletes it. Failed acquisition must not release it;
  failed deletion leaves it occupied. This does not protect other LRU maps
  against unrelated-flow eviction.

Checked official raw-source SHA-256 values:

```text
verifier.c  dc7faf153f033e5ffcd3653a6f2ca74b086302c5f168d1ca713ff61978cea90c
hashtab.c   feb077e198b9613fcacc505db4b8bd171cf79b653eca7d330219382ac39a8cdf
```

## Counterexample to dropping contended timestamp work

This sequence follows the actual `handle_synack()`, `packet_generation()` and
`generation_is_current()` rules in the
[saved patch](../2026-10-05-tcp-lifetime-deferred/implementation.patch).
It is source analysis, not an executed kernel reproduction.

1. Tuple F has active generation G, confirmed ISNs A/B, and pending sides A/B.
2. A replacement local SYN carries ISN C, different from A. It loses lock
   acquisition and is discarded without publishing C or invalidating G.
3. The replacement SYN/ACK carries ISN D and ACK C+1. It acquires the lock,
   but fails validation against the still-pending A+1. It returns generation
   zero without changing the old pending sides or active generation.
4. Replacement data acquires the lock. G's identity still matches pending A/B,
   so the data is assigned G. A TSval larger than the predecessor's passes
   the estimator's timestamp progression check and retains its old calibration.
   A colliding echoed timestamp can also match a predecessor departure under G.

Counting acquisition failures does not invalidate this calibration. A single
global acquisition marker has the same missed-SYN failure. A scoped implementation
agent independently checked the sequence and rejected the proposed replacement.

## Boundary for another attempt

A future design must define how losing reset evidence makes measurement
unavailable, including in-flight records, recovery ordering and bounded memory.
A capture-wide fail-stop protocol or an ordered lifecycle stream are possible
directions, not accepted designs. Neither was implemented. The existing LRU
pointer and eviction assumptions also need explicit review before restoration.

The previous candidate and its historical tests remain preserved in the
[deferral record](../2026-10-05-tcp-lifetime-deferred/README.md). Finding 2 stays
open; neither that candidate nor this analysis establishes deployment readiness.
