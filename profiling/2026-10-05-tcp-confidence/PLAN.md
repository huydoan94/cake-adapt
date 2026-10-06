# Bounded acceptance plan
Question: does the actual daemon retain an established TCP floor beyond its old 30-60s expiry while fresh fping still sees added delay, and recover after clearing the path? Also run cross-compiled focused estimator/controller assertions, including the ambiguous ACK-pair fallback, on the 32-bit target.

One read-only preflight: reachable VM, existing namespaces, processes, installed package/config/executable/filter hashes, complete original qdiscs, test-log inode and writers, available iperf3/fping/ip/tc. Stop if unavailable or conflicting state; no cleanup of unrelated state.

One batch: unchanged before daemon from current HEAD 518df81 and proposed after daemon, sequential isolated namespaces. Rate adjustment OFF: 5s warmup, 70s fixed added 80ms download/20ms upload path delay, 8s restored path. One repetition per variant. This is baseline-retention integration, not a real ISP backlog or throughput-control benchmark. Run focused target tests once. Before daemon is compiled from a git archive of HEAD into ignored build/queue-confidence; same unchanged filter object for both. Runtime libraries isolated; installed package unchanged.

Evidence: source/binary/filter hashes, uname, program fdinfo/JIT, raw TCP_QUEUE and DATA/LOAD records, phase times, receiver JSON, routing to local endpoint, full original and test qdiscs, configuration, process/package/file state before/after, log inode before/after. No CPU-cost claim.

Accept: warm baseline fit, before late-delay estimate collapses, after late-delay remains at least 50ms download/10ms upload, after recovery download <10ms with fresh samples; receiver bytes positive. Focused assertions pass. Original services, packages, config, qdiscs and processes restored; namespaces/test children absent; protected log inode unchanged.

Deadline: <=240s foreground SSH for batch, connect/transfer <=20s each. Kill only owned processes; trap HUP/INT/TERM/EXIT; preserve /tmp/sqm-mon-test.log inode. Refuse an existing production filter rather than replace it; original historical wrapper has this contract. Preserve all raw failed-run output. At most two harness/environment failures before stop and reassess; no new soak or performance runs. If baseline retention assertion fails because fping itself absorbs the path change, diagnose locally rather than add repetitions.
