---
description: 'Why the MSan CAS-S3 stateless shards exceed the CI budget (issue #2298): the CI evidence, eight local runs, the RustFS blocking-thread ratchet in the shared cgroup, the CAS GC runaway, the runner stack collection that froze the server, the DROP TABLE head-of-line wait, hypotheses with verdicts, and proposals.'
sidebar_label: 'MSan CAS-S3 shard budget RCA (2026-09-16)'
sidebar_position: 7
slug: /superpowers/cas/msan-cas-s3-shard-budget-rca-2026-09-16
title: 'MSan CAS-S3 stateless lanes: why the shards exceed the CI budget (RCA, 2026-09-14 to 2026-09-16)'
doc_type: 'reference'
---

# MSan CAS-S3 stateless lanes: why the shards exceed the CI budget {#msan-cas-s3-shard-budget-rca}

Consolidated report of the T4 investigation, 2026-09-14 to 2026-09-16, for issue #2298.

Audience: someone who did not follow the runs. Sections 1 and 2 are facts with a source path for every
number; section 3 labels each hypothesis confirmed / refuted / unresolved; 4 and 5 are proposals. Paths are
relative to the lane-g worktree unless absolute. `RIG` = `tmp/investigation/t4/msan_local`.

> Data paths in this report (`RIG/...`, `tmp/pr2300-cicd-watch/...`) point at run artifacts on the investigator's machine that are not in the repository. The published rendering is https://claude.ai/code/artifact/ca2f9064-8508-463a-a000-5129766eb3b8.

# 1. Problem statement and what CI shows {#problem-statement-and-what-ci-shows}

## 1.1 The lanes and the budget {#the-lanes-and-the-budget}

The MasterCI / PR pipeline runs the stateless suite under MSan twice: an ordinary lane
(`Stateless tests (amd_msan, WasmEdge, parallel 1-4/4` + `sequential 1-2/2`) on a local-disk MergeTree, and a
content-addressed lane (`Stateless tests (amd_msan, cas s3 storage, parallel, 1-3/3`). The CAS lane's shards
1/3 and 3/3 were cancelled at the 6 h job limit in both attempts of run 10; only 2/3 produced a
`result_*.json` and is the only shard with usable data (`msan/REPORT.md` §1).

Shard 2/3 ran 3 h 34 min wall (`tmp/pr2300-cicd-watch/run10/job_msan_cas_2of3.log`, 05:35:35 to 09:09:36),
of which the test step was 10,467 s.

## 1.2 The shard's own timeline {#the-shard-s-own-timeline}

From `msan/msan_cas_2of3_timeline.tsv`, 10-minute buckets of test completion (`msan/REPORT.md` §7,
`RIG/analysis/REPORT_run1.md` §1):

| elapsed | tests done | median s | share >= 150 s |
|---|---|---|---|
| 0-10 min | 109 | 8.80 | 2% |
| 50-60 min | 83 | 16.09 | 6% |
| 80-90 min | 41 | 26.69 | 20% |
| 130-140 min | 37 | 41.55 | 22% |
| 150-160 min | 28 | 80.36 | 36% |
| 160-170 min | 9 | 295.75 | **67%** |

Worker parallelism is flat at 5-6 all run and the median test-number prefix per bucket has no trend, so
neither queue draining nor test composition explains it (`msan/REPORT.md` §7). Using each test's local
duration as its own baseline to control for identity, the CI/local ratio ramps
(`RIG/analysis/REPORT_run1.md` §2, `RIG/analysis/local_vs_ci.tsv`):

| CI elapsed | n | median CI/local ratio |
|---|---|---|
| 0-30 min | 301 | 4.7 |
| 60-90 min | 142 | 8.2 |
| 120-150 min | 123 | 9.9 |
| 150-180 min | 41 | **20.7** |

The same work costs 4.4x more at the end of the run than at the start.

## 1.3 The CI server log {#the-ci-server-log}

`tmp/pr2300-cicd-watch/run10/msan_cas_2of3_server.log.zst`, 257 MB decompressed, 1,313,213 lines, 07:44:23
to 10:47:09. Extraction in `RIG/analysis/ci_timeline.tsv`, analysis in `RIG/analysis/CI_SERVER_LOG_TIMELINE.md`.

| signal | 10-20 min | 160-180 min | growth | local run 3 control (3 h 22 min) |
|---|---|---|---|---|
| `Moving: Execution took` median | 613 ms | 9,169 then 12,748 ms | **21x** | 298-426 ms, flat |
| small-query median (< 10k rows) | 0.0236 s | 0.1997 s | **8.5x** | 0.0186 -> 0.0365 s, saturates at min 60 |
| `DiskLocalCheckThread` median | 260 ms | 653 then 953 ms | 3.7x | crosses its log threshold once in 3.5 h |
| `DatabaseCatalogDropTableTask` median | 2,454 ms | 20,322 then 202,240 ms | 82x | 922-2,254 ms, flat |
| `JoinOrderOptimizer` (pure CPU) | 1 ms | 1-3 ms | **flat** | flat |
| `Attempt k/501 failed` (S3 retry) | 2 | 0 | — | — |
| `SlowDown`, `EADDRNOTAVAIL`, errno 99 | 0 | 0 | — | — |
| CAS contention / deadline / `retrying later` | 0 | 0 | — | — |
| `Too many parts`, `MemoryTracker: Correcting` | 0 | 0 | — | — |

98% of queries read under 10,000 rows, so the 8.5x is per-query fixed overhead, not data volume. Pearson
correlation with the share of tests >= 150 s: small-query median 0.853, `DiskLocalCheckThread` 0.712. Pure
computation does not slow; every path that touches the filesystem slows 4x to 82x.

## 1.4 The failed system-table scrape {#the-failed-system-table-scrape}

The job could not collect `system.metric_log`. The server did not stop, because it had been frozen by
the runner's own stack collection 14 minutes earlier (§1.6):

```
09:05:52 Failed to stop ClickHouse process 2215 gracefully - send TRAP signal to generate core file
09:06:08 Code: 76. DB::Exception: Cannot lock file .../ci/tmp/run_r0/status. Another server instance ...
         (5 times, 09:06:08 to 09:07:05)
09:07:05 ... --query "select * from system.metric_log into outfile '.../system_tables/metric_log.tsv' ..."
09:08:12 Fail [Server died]
```

(`job_msan_cas_2of3.log`.) The scrape runs after the stop and takes the `status` flock. The frozen server
still held that flock (it is a kernel lock, released only when the process dies), so every `clickhouse local`
call failed instantly with `Code: 76` until the container was torn down at 09:08:19; the tables were written,
just unreadable. The shard's artifacts hold only server / azurite / minio / rustfs /
dmesg logs, no memory or thread series (`msan/REPORT.md` §7). This is why eight local runs were needed.

## 1.5 The runner and the process layout {#the-runner-and-the-process-layout}

From `job_msan_cas_2of3.log`: `Architecture: x86_64`, **`CPUs: 16`**, **`Total Memory: 30.6GiB`**,
`Name: github-hetzner-runner-standby-...`, `INSTANCE_TYPE='altinity-self-hosted'`. The container is started
as `docker run --init --oom-score-adj=1000 --rm ...` with **no `--memory` flag**.

`ci/jobs/scripts/clickhouse_proc.py` starts MinIO (`setup_minio.sh`, line 149), azurite, and RustFS
(downloaded at lines 170-190) as subprocesses of the job. **The ClickHouse server, MinIO, azurite, RustFS
and the six `clickhouse-test` workers all live in one container cgroup.**

`MemoryWorker` runs with the Cgroups source; `CgroupsV2Reader::readMemoryUsage`
(`src/Common/MemoryWorker.cpp:166-190`) sums `anon + sock + kernel(...)` from the whole cgroup's
`memory.stat`. With no `--memory`, the server derives `max_server_memory_usage` = 0.9 x 30.6 GiB =
**27.54 GiB** from the host.

The CI shard shows **zero** total-memory-limit errors. Its five `Code: 241` lines are one test asserting its
own 19.07 MiB per-query cap, and the job's `OOM in dmesg` check passed (`job_msan_cas_2of3.log`).

The harness also passes `--memory-limit 10737418240`, which builds a cgroup per `clickhouse-test` worker:
that caps the test client, not the server.

## 1.6 The runner's stack collection froze the server {#the-runner-s-stack-collection-froze-the-server}

The "shutdown hang" in this shard is not a server shutdown hang. The server clock runs 2 h ahead of the
job log (`Starting ClickHouse` at 07:44:29 server time vs `Start ClickHouse Server` at 05:44:11 UTC), so the
server log's last line, `Child process was stopped by signal 19` at 10:47:09, is 08:47:09 UTC. The job log
around that time (`job_msan_cas_2of3.log`, UTC):

| UTC | event |
|---|---|
| 06:10 – 08:43 | 13 tests die on the per-test `--timeout` (600 s, `Reason: Timeout!`, marked `BROKEN`): the first at 06:10, roughly one per 10 min from 07:13, three at 08:43 (the 160-170 min bucket of §1.2). None is a CAS test; the set is summing trees, joins, sorts, `01516_drop_table_stress_long`, part restore. Eight of them cost 80 worker-minutes of waiting on six workers |
| 08:45:18 | `timeout_handler` runs `print_sql_stacktraces`: the `system.stack_trace` query with `demangle(addressToSymbol(...))` times out after 30 s |
| 08:45:32 | `_ensure_lldb_installed` runs `apt-get install lldb` on the fly (the msan image ships without it) |
| 08:46:08 – 08:46:38 | `lldb --batch -p 2211` (the watchdog) is killed on the 30 s ceiling of `get_stacktraces_from_lldb` |
| 08:46:39 – 08:47:09.104 | `lldb --batch -p 2215` (the server) is killed on the same ceiling, mid-attach |
| **08:47:09.140** | watchdog logs `Child process was stopped by signal 19`. **Last line of the server log** |
| 08:48:50 | more `Test execution timed out`; four more lldb attempts on client and bash pids, two time out |
| 08:50:32 | `clickhouse-test` terminates itself (`stop_tests` → `os.killpg(SIGTERM)`), exit 143; 1035 / 3799 tests done |
| 08:50:32 – 09:00:51 | `system flush logs` and `SYSTEM FLUSH ASYNC INSERT QUEUE`, 300 s socket timeout each |
| 09:00:51 – 09:05:52 | `clickhouse stop --max-tries 300 --do-not-kill`: no `Received termination signal` in the server log |
| 09:05:52 | `Failed to stop ClickHouse process 2215 gracefully - send TRAP signal`; `proc.wait(10)`; `proc.kill()` |
| 09:06:08 – 09:08:12 | nine `clickhouse local` scrapes, every one `Code: 76` on the `status` flock (§1.4) |
| 09:08:19 | container torn down; the process finally dies |

**Mechanism.** `lldb -p` attaches with `PTRACE_ATTACH`, which stops the tracee with `SIGSTOP`, and then
loads symbols. The inner MSan ELF is 7.8 GB (`RIG/bin/clickhouse`), so symbol loading takes far longer than
the 30 s ceiling in `tests/clickhouse-test:1128` (`get_stacktraces_from_lldb`). `shell_get_output` kills
lldb on timeout. When a tracer dies while the tracee is in a ptrace-stop, the kernel detaches it and leaves
it in group-stop; the real parent then observes `WIFSTOPPED`, which is exactly the watchdog line at
08:47:09.140, 36 ms after lldb was killed (`src/Daemon/BaseDaemon.cpp:706-709`). From that moment the server
is in state `T`: no thread runs, no signal handler runs, no log line is written. The comment in
`get_stacktraces_from_lldb` ("Killing lldb leaves the server process untouched") is wrong.

Six lldb invocations ran in this job; none produced a stack (all six hit the 30 s ceiling, two of them
on the server and watchdog pids). `print_c_stacktraces` already skips ASan builds because a debugger
attach disables LeakSanitizer; MSan and TSan are not skipped.

**The TRAP never reached the server.** `stop_server` in `ci/jobs/scripts/clickhouse_proc.py:944-976`
sends `SIGTRAP` and then `SIGKILL` to `self.proc`, the `Popen` object of the launched process. The launched
process is the **watchdog** (pid 2211; `Will watch for the process with pid 2215`), and the watchdog
forwards only `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGTERM` (`src/Daemon/BaseDaemon.cpp:669-670`). So even on a
healthy server this path kills the watchdog and orphans the server; it never produces the server's stack or
core. Here it did not matter, because a stopped process cannot run the fatal-signal handler anyway, but the
`proc.kill()` aimed at pid 2215 would have released the `status` flock and let the scrape run.

**Is this the cause of the job overrunning its budget?** No. By 08:45 UTC, before the first lldb call,
the shard had completed 1035 of 3799 tests in 2 h 51 min and per-test timeouts had been landing since 06:10; the
runner would not have finished within the 3 h job budget (`ci/defs/job_configs.py:425`) regardless. The
freeze is a **consequence** of the same degradation: tests time out, the runner tries to collect stacks, the
attach on a 7.8 GB MSan binary cannot finish in 30 s, and the kill converts a slow server into a dead one.
What the freeze does cost: ~21 minutes of tail (two 300 s flushes, a 300 s stop, the scrape loop), the
loss of every system table (§1.4), and the loss of the stacks that were the point of the exercise. It also
explains why earlier reads of this job as "shutdown hang" and "server died" were wrong: the server was
alive and frozen.

**Fixes, all on the failure path only** (ordered by value / risk; none touches server code):

1. `get_stacktraces_from_lldb`: on timeout, kill lldb's whole process group and send `SIGCONT` to the
   target pid. `SIGCONT` is a no-op for a running server. Fix the comment.
2. `stop_server`: address the server pid from the pid file for `SIGCONT`, `SIGTRAP` and the final kill,
   not `self.proc` (the watchdog). Then a real shutdown hang yields the fatal-handler stack and a core.
3. Skip `print_c_stacktraces` under MSan and TSan as it is under ASan, instead of raising the ceiling:
   every hung test would pay minutes for an attach that today yields nothing.
4. Collect `system.stack_trace` **without** `demangle(addressToSymbol(...))` and symbolize offline
   against the CI binary (the same procedure as `trace_log`); the raw query returns in seconds under MSan.

## 1.7 What the timed-out tests were waiting on {#what-the-timed-out-tests-were-waiting-on}

Per-test analysis in `tmp/pr2300-cicd-watch/run10/timeouts_summary.txt` (runner side) and
`timeouts_queries.txt` (server side, query lifecycle matched by `query_id` between `executeQuery: (from` and
`TCPHandler: Processed in`).

**No stacks exist for any of them.** On a per-test timeout `clickhouse-test` sends `SIGTSTP` to the test's
process group; only the client's fatal handler answers, with a raw-address trace whose one symbolized frame is
`__poll` (the client waiting for the server). Server stacks were never collected: `system.stack_trace` timed out
and lldb froze the server (§1.6).

**None of the 13 was stuck on one query.** Each ran its whole script slowly and died on the 600 s budget:

| test | queries | sum of query time | slowest query |
|---|---|---|---|
| `00148_summing_merge_tree_aggregate_function` | 85 | 663 s | `drop table` 60 s |
| `03802_summing_merge_tree_tuple_element` | 108 | 667 s | `DROP TABLE` 112 s |
| `02346_text_index_creation` | 71 | 661 s | `DROP TABLE tab` 149 s |
| `04033_tpc_ds_q24` | 4 | 162 s (+ one query still open) | TPC-DS q24 162 s, second run open at kill |
| `00376_shard_group_uniq_array_of_int_array` | 14 | 910 s | `CREATE TABLE … AS SELECT` 353 s, `remote()` 222 s |
| `00926_adaptive_index_granularity_merge_tree` | 135 | 666 s | `DROP TABLE` 88 s |
| `01516_drop_table_stress_long` | 67 | 407 s | `DROP TABLE` 53 s |
| `01721_join_implicit_cast_long` | 398 | 582 s | `DROP TABLE` 25 s |
| `03822_attach_with_unknown_projection` | 40 | 596 s | `DROP TABLE … SYNC` 113 s |
| `01453_fixsed_string_sort` | 18 | 635 s | `drop table` 379 s |
| `02899_restore_parts_replicated_merge_tree` | 26 | 469 s | `OPTIMIZE FINAL` 50 s, `ALTER DELETE` 49 s |
| `00718_low_cardinaliry_alter` | 19 | 799 s | `drop table` 564 s |
| `02661_read_from_archive_7z` | 25 | 551 s | `DROP TABLE` (a `File` engine table) 330 s |

**`DROP TABLE` dominates, and it is two effects stacked.** Traced for `tab_00718` (564 s), `badFixedStringSort`
(379 s) and `02661_archive_table` (330 s) on the server log:

1. *Head-of-line wait in the catalog drop task.* The CI users config sets
   `database_atomic_wait_for_drop_and_detach_synchronously = 1` (`tests/config/users.d/database_atomic_drop_detach_sync.xml`),
   so every `DROP TABLE` blocks in `DatabaseCatalog: Waiting for table … to be finally dropped`.
   `dropTableDataTask` (`src/Interpreters/DatabaseCatalog.cpp:1649`) takes the current batch, runs
   `dropTablesParallel`, **waits for the whole batch**, and only then reschedules itself. A table enqueued while
   a batch is running waits for the batch's slowest drop, whatever `database_catalog_drop_table_concurrency`
   (256 in CI) allows. `tab_00718` was enqueued at 10:36:27 and reached `dropAllData` at 10:42:29, exactly when
   the previous batch's `badFixedStringSort` finished. The `File` table of `02661` had nothing to delete and still
   waited 5 min 27 s. Same code upstream; the CAS lane only makes the batches long.
2. *Part removal on the CAS disk got 30-100x slower over the run.* `dropAllData` removes parts serially; each part
   is a `delete_tmp_*` repoint through `CachedPartFolderAccess` (a ledger commit). Per part, from the drop-task
   thread's own timestamps:

   | server hour | drops | median s / part | p90 | max |
   |---|---|---|---|---|
   | 07 | 56 | 0.83 | 2.45 | 3.7 |
   | 08 | 419 | 2.38 | 5.20 | 11.4 |
   | 09 | 227 | 6.66 | 8.81 | 17.1 |
   | 10 | 74 | 10.73 | 23.19 | 37.1 |

   At 07:53 a 6-part drop took 0.35 s per part; at 10:42 the 7-part `tab_00718` took 24-37 s per part, 3 min 5 s
   in all. The gap between `Removing N parts from filesystem (serially)` and the first `Repointed committed ref`
   is silent at this log level. The drop queue itself never backed up (`Have N tables in drop queue`: mean
   1.2-2.0, max 25), which is consistent with (1): the task only ever holds one batch.

   What the other evidence says about the gap (2026-09-16 follow-up):
   - *Where the removing thread waits.* The ref ledger is one per pool (`CasPool.h:1245`) with a single
     leader-flush queue (`CasRefLedger::appendRefOpsOnRuntime`). The 5-minute `system.stack_trace` samples of local
     runs 7 and 8 (`RIG/run{7,8}/samples/stacks_*.tsv`) show part-removal threads in exactly two places: queued in
     that lane (one run-7 sample has 18 threads at once: `IMergeTreeDataPart::remove → moveDirectory →
     republishRef → precommitAdd → appendRefOps → appendRefOpsOnRuntime`), or as the leader inside the
     conditional PUT of the `_log` chunk or `_ckpt` (`commitRefChunk` / `publishCkpt` → `finalizeConditionalWrite`
     → `TaskTracker::waitAll`). Run 8 totals: 262,468 `CASRefBatchFlushes` for 291,366 `CASRefBatchedMutations`,
     about 1.1 mutations per flush, so the combiner almost never fires and the lane runs one PUT per mutation;
     `CASRefQueueWaitMicroseconds` summed to 19,889 s.
   - *It is not raw PUT latency.* The sampled `WriteBufferFromS3` Create→Close for `_log` / `_ckpt` keys in the CI
     log grows from 22 ms to 81 ms median between hours 07 and 10 (p90 58 → 175 ms, one 12 s outlier). A 3.7x
     growth in PUT does not make a 30-100x growth in repoint unless the queue ahead of the PUT is deep or tail
     PUTs dominate.
   - *Unmeasured.* The split between queue wait and PUT tail: run 8's `query_log` and `cas_log` were empty, and
     the stack samples are too sparse to weigh it. This is the open question that decides whether eliding the
     repoint removes the cost or halves it; backlog entry `[drop-path-head-of-line-and-repoint-ramp]` in
     `docs/superpowers/cas/BACKLOG.md` names the measurement.

The other slow shapes, `CREATE … AS SELECT` 353 s and TPC-DS q24 162 s, are the same 8-20x per-query slowdown
of §1.2 applied to heavier queries; nothing in them touches drop or CAS-specific paths.

**One false lead, closed.** `AWSClient: If the signature check failed … Attempting to adjust the signer`
appears 68,533 times in the server log (6/s at the end of the run). It is not a retry storm: the SDK logs it in
`AdjustClockSkew` for *every* failed attempt before `ShouldRetry` is consulted
(`contrib/aws/src/aws-cpp-sdk-core/source/client/AWSClient.cpp:355`), and the matching
`Request failed, now waiting` line never appears. These are the expected `HEAD` 404s of the write path
(`Expect404ResponseScope`), i.e. log noise proportional to write traffic.

**Consequence for the shard.** With synchronous drops, one slow multi-part drop stalls every other test's
`DROP TABLE` for minutes, and almost every stateless test ends with one. This is how the per-part CAS removal
latency turns into whole-shard test timeouts, and why the timed-out set looks random. Two cheap levers:
`database_catalog_drop_table_concurrency` does nothing here; the batch-wait in `dropTableDataTask` does, and
the per-part removal cost is the CAS-side item tracked as `[PART-REMOVAL-REPOINT]`; both findings are filed together as `[drop-path-head-of-line-and-repoint-ramp]` in `docs/superpowers/cas/BACKLOG.md`.

# 2. What was measured locally {#what-was-measured-locally}

Rig: `RIG`, documented in `RIG/RUNBOOK.md` §1 — the CI `Build (amd_msan)` artifact of tag
`v26.6.4.20001.altinityantalya`, sha `2073b1f88de`, inner ELF 7.8 GB, BuildID `2d17c136590b06bc`; the same
1016-name list as CI shard 2/3, `-j 6`, `--cas-s3-storage`; host 32 logical CPUs, 91 GiB RAM, NVMe.

## 2.1 Run index {#run-index}

| run | layout change | duration | outcome | key numbers |
|---|---|---|---|---|
| 1 | system logs on **local** disk, one pass | 25 min | no ramp | median 1.79 -> 3.45 s over 3 buckets; RSS plateau 14.6 GB by min 16; TERM -> exit 10 s |
| 2 | logs on `cas_s3` (CI layout), 6 passes, **everything in one shared terminal cgroup** | 52 min | **cliff** at +49 min | 115,533 `MEMORY_LIMIT_EXCEEDED`/min at peak; server RSS 18.3 GB, cgroup 25.01 GiB vs cap 24; per-test median flat 2.4-3.0 s until the cliff; exit 10 s |
| 3 | server and both RustFS in **separate systemd scopes** | 3 h 22 min | no cliff, no ramp | paired iteration 6/1 median **1.18**; per-pass RSS delta +9.96, +2.25, +1.40, +2.36, +0.69, +0.61 GB; `MemoryTracking` flat 4.6-4.9 GB vs RSS 24 GB; named caches 298 MB total; exit 15 s |
| 4 | everything in ONE scope, `MemoryMax=30G`, no swap, cap 24 GiB | 51 min | **cliff** at +39-47 min | scope anon 25.5 GB (server 16.9 + RustFS 8.5); `pgsteal` 0 until the jump; 85 `241`; exit 20 s |
| 5 | one scope sized 30.60 GiB (server derives CI's 27.54 GiB cap), `CPUQuota=1600%` | 90 min | **cliff** at +79 min | passes 1-2 full; medians 1.94-3.56 s, no ramp; `refault` 151 before the jump; 135 `241`; exit **80 s** |
| 6 | run 5 layout, **FUSE latency injected** per object-store operation | 51 min | **ramp reproduced** | see 2.5 |
| 7 | run 3 layout + RustFS instrumented (`/proc` + admin metrics) | 3 h 25 min | no cliff, no ramp | paired 6/1 **1.17**; RustFS anon 0.09 -> 13.78 GB, threads 69 -> 1074; exit 15 s |
| 8 | run 7 + RustFS runtime and allocator knobs (A/B) | 143 min, 6 passes | complete, see 2.7 | threads pinned 130, anon max 2.55 GB, `rename_data` 0.5-0.8 ms, 30% less wall time than run 7 |

## 2.2 The cliff class and its mechanism {#the-cliff-class-and-its-mechanism}

Runs 2, 4 and 5 all ended the same way, and none of them is the CI ramp. In each, the object store's memory
is charged to the server because `MemoryWorker` sums the whole cgroup (`MemoryWorker.cpp:166-190`). In run 2
the server's own RSS was 18.3 GB while the enforced reading was 25.01 GiB against a 24 GiB cap, the
difference being the CAS RustFS at 8.93 GiB plus the minio one at 0.22 GiB (`RIG/analysis/REPORT_run2.md`
§4-7, `RIG/run2/cliff/status.txt`).

The trigger is always RustFS's second-pass jump. Run 4, minute 40: RustFS 2.43 -> 8.53 GB in one 30 s
sample, scope anon 16.33 -> 25.50 GB. Run 5, minute 80: 3.53 -> 9.32 GB, scope anon 21.64 -> 28.39. In both,
`pgsteal` was **exactly 0** and `workingset_refault_file` in the hundreds until that moment
(`RIG/analysis/REPORT_run45.md` §3): no memory pressure preceded it.

The CI msan shard is one pass of 1044 tests, leaving roughly 2 GB in the object store next to a ~13 GB
server inside a 27.54 GiB cap: it never trips it, and its log confirms that. The cliff is the shape of the
**ASan** cas 2/2 failures (808 x `241` with RSS at the cap), not of the msan ramp (`msan/REPORT.md` §9).

## 2.3 MSan memory multiplication {#msan-memory-multiplication}

Run 3 at TERM: `VmRSS` 23.52 GB, `VmHWM` 30.28 GB, `MemoryResidentMax` 31.00 GB, `MemoryTracking` 4.78 GB,
1438 threads (`RIG/analysis/REPORT_run3.md` §2). All named caches summed are **298 MB**, the largest being
`CASManifestDecodeCacheBytes` at 134.1 MB, pinned at its ceiling from minute 10.

Of the 23.5 GB: ~2.5 GB file-backed binary, ~9.6 GB MSan shadow and origin at 2x the tracked heap (the
mappings at `0x110000000000` and `0x2ffffffff000` measured 3.01 and 2.99 GB in run 2,
`RIG/run2/cliff/big_mappings.txt`), 0.3 GB named caches, and **~11 GB unattributed** allocator and shadow
retention that no server metric exposes. A query at 8 GiB tracked spiked anon to 24.8 GiB.

**The ClickHouse process alone peaked at 31.00 GB, above the entire RAM of the CI runner.**

## 2.4 RustFS: the blocking-thread ratchet {#rustfs-the-blocking-thread-ratchet}

Run 7, CAS instance over 204 min (`RIG/analysis/REPORT_run7.md`, `RIG/run7/samples/rustfs_proc_cas.tsv`):
anon 0.09 -> 13.78 GB (peak 14.51), threads 69 -> 1074, fds 14 -> 1901 (peak 3859). MinIO control on the same
host and disk: anon 84 -> 242 MB, threads 69 -> 88, fds 14 -> 15.

| correlate of anon | Pearson r |
|---|---|
| threads | **0.949** |
| elapsed time | 0.752 |
| fds | 0.368 |
| cumulative requests / object count | none |

Per-thread cost, two measurements: **1.05 MB** from the first-difference regression over the 13 thread-count
changes, and **13.83 MB** from the peak ratio. The first is the 1 MiB stack
(`crates/config/src/constants/runtime.rs:51`); the other ~12.8 MB accumulates afterwards. After the
1074-thread ceiling at minute 136 anon still grew **+1.64 GB over 69 minutes at constant thread count**, and
all four drops over 1 GB happened with zero thread loss.

Source (`rustfs_src/analysis/RUSTFS_MEMORY.md`, rc.3 @ 1aae680): threads are tokio blocking threads with
keep-alive **60 s** instead of tokio's 10 s (`rustfs/src/server/runtime.rs:130-132`), re-fed by **4 or more
`spawn_blocking` per small PUT** under `Strict` durability (`crates/ecstore/src/disk/local.rs:9620-9803`:
staged `xl.meta` fdatasync, commit rename, destination-dir fsync, one per ancestor dir). mimalloc is the
allocator with **no tuning at all** (`rustfs/src/main.rs:54`, zero `mi_option_set` in the tree), and
`mi_collect` runs only in a default-off loop (`rustfs/src/allocator_reclaim.rs:378`). The object data cache
is disabled by default and cannot be the cause.

The ratchet is driven by server connection storms: fds 1794 against 1789 held connections at minute 38,
2432 against 2415 at minute 86. After the fd peak of 3859, fds fell 46% in 11 minutes while threads did not
move by one.

## 2.5 RustFS latency, and the FUSE emulation {#rustfs-latency-and-the-fuse-emulation}

Run 7, avg ms per op per 10-minute bucket (`RIG/analysis/run7_rustfs_ops.tsv`):

| op | 0-10 min | 80-90 | 130-140 | 200-210 | trend |
|---|---|---|---|---|---|
| `rename_data` | 44.0 | 59.1 | 69.0 | 64.6 | **flat**, a constant per-PUT fsync tax |
| `delete` | 0.4 | 10.4 | 17.7 | **44.4** | 110x, grows with store size |
| `delete_version` | 0.4 | 2.3 | 2.5 | 3.8 | 10x |
| `read_version` | 0.0 | 0.1 | 0.1 | 0.1 | flat over 11.45 M calls |

Store at the end: 7.4 G apparent / 4.41 GiB actual in **409,536 files** and 456,434 directories, ~67
objects per test (`RIG/run7/shutdown/rustfs_cas_du.txt`, `rustfs_cas_filecount.txt`); run 3 measured 74.

**Run 6** put the object store's data behind a FUSE mount injecting a fixed latency per operation
(`RUNBOOK.md` "Run 6"; knob history in `RIG/run6/throttle_applied_at`, corroborated by `slowfs_latency_ms`
in `RIG/run6/samples/summary.tsv`). Single pass, 991 completed tests:

| phase | injected latency | n | median s | p90 s | >= 100 s | >= 150 s |
|---|---|---|---|---|---|---|
| A | 0 ms | 610 | **2.55** | 18.0 | 2 | 1 |
| B | 1 ms | 294 | **4.78** | 65.8 | 17 | 8 |
| C | 3 ms | 21 | **24.37** | 137.2 | 4 | 1 |
| D | 0 ms (reset) | 66 | 5.51 | 58.9 | 4 | 2 |

1 ms per operation costs 1.9x, 3 ms costs 9.6x, with zero `Code: 241`. Phase C is small (n=21) because at
3 ms only 12-14 tests completed per 10 minutes, which is itself the measurement. This is the only local
configuration that reproduced the CI ramp's magnitude.

## 2.6 CAS GC runs away {#cas-gc-runs-away}

Run 7 (`RIG/analysis/REPORT_run7.md` §4; written up as `{#gc-backlog-runaway}` in
`/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/BACKLOG.md`):

| bucket (min) | 0-10 | 50-60 | 100-110 | 140-150 | 200-210 |
|---|---|---|---|---|---|
| GC round duration, median s | **24.3** | 108.7 | 265.8 | 458.7 | **596.8** |
| `CASGCPendingReclaim_cas_s3` | 81 | 3,077 | 9,620 | 25,043 | **35,351** |
| rounds completed per 10 min | 23 | 5 | 3 | 1 | 1 |

97 rounds in 3 h 25 min against a 20 s interval. Round length tracks the **backlog** (r = 0.920), not
RustFS delete latency (r = 0.572) or `rename_data` (0.445): the loop is inside CAS.

Phase breakdown from run 8's live `system.cas_gc_log` (BACKLOG entry), 0-5 min versus 15-20 min windows:
`defer_decision` 0.2 -> 10.9 s, `fold_ref_intake` 0.5 -> 9.7 s, `pending_deletes` 0.1 -> 8.4 s. The
`defer_decision` LIST walks `ref_log_keys_listed` 2,308 -> 19,415 across `namespaces_seen` 105 -> 434, of
which `dead_life_debris` is 51 -> 403: **93-95% of what the LIST walks is dropped tables' debris**. The
janitor that removes it runs once per round over one page of 1,000 keys (size hard-coded at
`CasGc.cpp:363`) and runs *after* the LIST, so it removes what the same round already listed
(`CasGc.cpp:352-386`, `CasNamespaceJanitor.cpp`). Its throughput is page x rounds/min, and rounds/min
collapses as debris grows: a positive feedback loop.

The CI msan shard's log shows GC round 27 at +61 min: ~135 s per round already in hour one.

## 2.7 Shutdown, and run 8 final {#shutdown-and-run-8-final}

Local shutdown never hung: 10, 10, 15, 20, **80**, -, 15, 20 s for runs 1-5, 7 and 8. Run 5's 80 s is the
slowest and the only one taken while over the cap under reclaim. CI's "hang" was not a hang (§1.6).

**Run 8 is complete** (143 min, six passes, same workload as run 7; `RIG/run8/`, dumps in `RIG/run8/shutdown/`).
Knobs applied to the CAS RustFS only (`RUNBOOK.md` "Run 8"): `RUSTFS_RUNTIME_MAX_BLOCKING_THREADS=64`,
`RUSTFS_RUNTIME_THREAD_KEEP_ALIVE=5`, `RUSTFS_ALLOCATOR_RECLAIM_ENABLED=true`,
`RUSTFS_ALLOCATOR_RECLAIM_INTERVAL_SECS=30`, `RUSTFS_DURABILITY_MODE=relaxed`. The MinIO stand-in (second
RustFS, untreated) is the in-run control.

| metric | run 7 control | run 8 treated |
|---|---|---|
| wall time for the same six passes | 205 min | **143 min** |
| tests OK / FAIL | 5082 / 870 | 5065 / 887 (same known-red set) |
| RustFS CA threads, max / final | 1074 / 1074 | **130 / 130** (pinned from pass 2) |
| RustFS CA anon, max / final | 15.2 GB / 14.4 GB | **2.55 GB / 2.45 GB** (1.24 GB after pass 1, still creeping) |
| untreated RustFS (control) at the end | — | 99 threads, 0.33 GB |
| RustFS CA file descriptors, final | 1,901 | 5,160 (unexplained, see below) |
| `rename_data` per op | 44-69 ms | **0.5-0.8 ms** |
| `Code: 241` in test output | 12 | 4 |
| test median / p90, sixths of the run | 2.90/17.0 → 3.36/21.1 → 3.72/21.5 → 3.42/21.5 → 3.61/22.1 → 3.77/23.2 s | 1.85/9.7 → 2.46/12.1 → 2.56/12.3 → 2.66/12.4 → 2.66/13.1 → 2.61/12.9 s |
| pool size at the end | 7.4 GB in 409,536 files | 4.5 GB in 273,740 files |
| `CASGCPendingReclaim` at the end (monitor) | 9,620 at +100 min | 9,293, age 372 s |

Same work, 30% less wall time, per-test median 1.4x lower and p90 1.7x lower across the whole run, and the
shape changes: run 7 keeps drifting up to the last sixth, run 8 plateaus after the first sixth. The five knobs
hold the CA RustFS to 130 threads and 2.5 GB across six passes against 1074 threads and 15 GB, a 5.9x cut in
anonymous memory. Memory still creeps (1.24 → 2.45 GB over five more passes), so the cap bounds the ratchet
without removing it; the descriptor count ended higher than run 7 (5,160 vs 1,901), which is not analysed.

Two caveats. The knobs cannot be attributed individually: four bound threads and the allocator,
`RUSTFS_DURABILITY_MODE=relaxed` trades a durability guarantee for speed, and one run cannot separate them.
And the GC loop is untouched, as expected: round duration in run 8 still grows, median 16.9 s in the first
30 min, 90 s, 138 s, 256 s, then 319 s in the last bucket (`RIG/run8/samples/cas_gc_log.tsv`, `Finish` rows,
82 rounds), and the pending backlog ends where run 7's did. That is §2.6, not this section.

# 3. Hypotheses {#hypotheses}

| # | hypothesis | status |
|---|---|---|
| H1 | Server lifetime / CAS state growth inside the server causes the ramp | **refuted** |
| H2 | The memory-limit cliff is the CI ramp | **refuted** |
| H3 | Host memory pressure on the 30.6 GiB runner causes the ramp | **refuted as sufficient** |
| H4 | CPU starvation on 16 cores causes the ramp | **refuted** |
| H5 | S3 connection churn / port exhaustion causes the ramp | **refuted** |
| H6 | Runner local-disk latency growth causes the ramp | **confirmed in shape, unproven on the runner** |
| H7 | CAS GC is an amplifier | **unresolved, plausible** |
| H8 | RustFS thread/memory growth is an amplifier via the shared cgroup | **confirmed as a mechanism, not as the msan cause** |
| H9 | The CI shutdown hang | **explained: not a shutdown hang** (§1.6) |

**H1 — refuted.** Runs 3 and 7 each ran 6 passes on one server, 3 h 22 min and 3 h 25 min. Paired
iteration-6-vs-1 medians 1.18 and 1.17 against CI's 4.4x, with 0-2 tests at or above 150 s in every bucket
of both. Run 3 accumulated more parts, tables and objects than CI; its `Moving` task stayed at 298-426 ms.

**H2 — refuted.** The CI shard has zero total-memory-limit errors across 3 h, a passing dmesg OOM check, and
five `Code: 241` lines all belonging to one test's own 19.07 MiB cap. Locally the cliff is a 4,000x step in
one minute after 45 minutes of flat durations; CI is a smooth 4.4x rise with no errors. Different shapes.
The cliff is real and worth fixing, but it is the ASan lane's failure, not the msan ramp.

**H3 — refuted as sufficient.** Run 5 gave the server CI's exact 27.54 GiB cap inside a 30.60 GiB scope with
no swap. For 80 minutes the per-test median stayed between 1.94 and 3.56 s, `Moving` at 279-357 ms and
`DataProcessing` at 298-444 ms, all flat, with `pgsteal` at zero. This leaves the runner's *metadata*-cache
pressure open: on 30.6 GiB the dentry and inode cache competes with a 13 GB server, which is a route into
H6 rather than an independent cause.

**H4 — refuted.** The pure-CPU probe in the CI log, `JoinOrderOptimizer: Optimized join order in N ms`, is
1 ms at minute 10 and 1-3 ms at minute 170. Run 5 additionally imposed `CPUQuota=1600%` and showed no ramp.

**H5 — refuted.** CI shows 2 S3 retry lines in the first bucket and 0 in the last, zero `SlowDown`, zero
`EADDRNOTAVAIL`/errno 99, and `WriteBufferFromS3` flat at ~500 per bucket. The 56 `Connection refused`
events noted in `msan/REPORT.md` §7 are flat over time, so not the ramp's mechanism.

**H6 — confirmed in shape, unproven on the runner.** Supporting: in CI every filesystem-touching operation
slows 4x to 82x while pure CPU is flat (§1.3); the local control is flat on the same probes for longer; and
run 6 reproduced the ramp's magnitude by injecting 1 and 3 ms per object-store operation, with nothing else
changed (§2.5). The flat-then-break shape at minute 150 matches a cloud volume exhausting burst credits
under the CAS lane's small-object write rate. What is missing: no `iostat`, `io.stat` or device model from
the runner, so burst-credit exhaustion is inferred from the server's own latency, not observed. This is the
single gap that a CI-side artifact would close.

**H7 — unresolved, plausible.** GC round duration grows 24x and the backlog 436x locally (§2.6), and GC is
about a third of all object-store operations. On NVMe the tests do not feel it (1.17), but the CI shard's
log shows ~135 s rounds in hour one; if per-operation latency there is 10-100x local, the same loop would
be felt. Unproven, because the shard exports no `cas_gc_log`.

**H8 — confirmed as a mechanism, not as the msan cause.** RustFS memory growth is real, understood at source
level, reproduced, and then removed by knobs (§2.4, §2.7); in the CI container layout it is charged to
`max_server_memory_usage` (§1.5). It causes the local cliffs and the ASan lane's `241` storms, but not the
msan ramp: that shard's single pass never reaches the jump.

**H9 — explained, and it is not a shutdown hang.** Local shutdown never exceeded 80 s across seven runs,
and the CI server never started a shutdown: it was left in group-stop (state `T`) at 08:47:09 UTC by the
runner's killed lldb attach, 14 minutes before `clickhouse stop` (§1.6). `SIGTERM` and `SIGTRAP` were sent to
a stopped process and to the watchdog respectively; neither could have produced a log line, a stack or a core.
K6 still has value for a real hang, but the first fix is C8: never leave the target stopped, and signal the
server pid rather than the watchdog.

# 4. Proposals {#proposals}

## 4.1 CI/CD {#ci-cd}

| # | change | expected effect | cost |
|---|---|---|---|
| C1 | Start MinIO, azurite and RustFS in their own cgroup inside the job container (`ci/jobs/scripts/clickhouse_proc.py`), or give the container `--memory` sized for server + stores | Removes the cliff class entirely: the object store stops being charged to `max_server_memory_usage`. Fixes the ASan lane's 808 x `241` | Medium: `clickhouse_proc.py` plus a cgroup delegation in the container |
| C2 | Scrape `system.metric_log`, `asynchronous_metric_log`, `cas_log`, `cas_gc_log`, `blob_storage_log` **before** the stop, or dump them from the live server every N minutes | Would have answered this in one CI run. Today the scrape runs after the stop and dies on the `status` flock with `Code: 76` (§1.4) | Small: reorder the praktika step |
| C3 | Sample `/proc/<pid>/{status,smaps_rollup}` for every sidecar and the server every 30 s, and the RustFS admin API (`/rustfs/admin/v3/metrics`) every 60 s | Makes §2.4 and §2.5 visible per job instead of per local rig | Small |
| C4 | Sample cgroup `memory.stat` and `io.stat`, plus `iostat -x` and `dmesg`, per job | **Closes H6.** This is the missing evidence | Small |
| C5 | Provisioned-IOPS volume, or tmpfs for the object store, on the CAS sanitizer lanes | If H6 holds, removes the ramp | Medium, cost implications |
| C6 | Reshard `msan cas s3` 3 -> 5 | Shorter server lifetime and smaller per-shard footprint; buys budget without fixing the cause | Small |
| C9 | Make `dropTableDataTask` reschedule while a batch is still running (or drop per-table from `enqueueDroppedTableCleanup` when `ignore_delay`), so a synchronous `DROP` never waits for an unrelated table's batch; and measure the `delete_tmp_*` repoint cost per part from run 8's `cas_log` (§1.7) | Removes the head-of-line minutes from every `DROP TABLE` on the CAS lane; the per-part cost is the CAS item | Small (catalog task) + the existing backlog item |
| C8 | `get_stacktraces_from_lldb`: kill lldb's process group and `SIGCONT` the target on timeout; `stop_server`: signal the server pid from the pid file, not `self.proc` (the watchdog); skip lldb under MSan/TSan as under ASan; scrape `system.stack_trace` unsymbolized (§1.6) | A hung test no longer freezes the server; a real shutdown hang yields a stack and a core; system tables survive | Small: `tests/clickhouse-test` and `clickhouse_proc.py`, failure path only |
| C7 | Size the runner for the sum of footprints: server up to 31 GB (§2.3) plus object stores, against today's 30.6 GiB | Removes H3's residual metadata-cache pressure | Cost |

## 4.2 Configuration {#configuration}

| # | change | expected effect | cost |
|---|---|---|---|
| G1 | `RUSTFS_RUNTIME_MAX_BLOCKING_THREADS=64` and `RUSTFS_RUNTIME_THREAD_KEEP_ALIVE=5` on CI object stores | Thread ceiling 1074 -> 130, anon 15.2 GB -> 2.55 GB max, wall time -30% (run 8 final, §2.7) | Two env vars |
| G2 | `RUSTFS_DURABILITY_MODE=relaxed`, **CI only** | `rename_data` 44-69 ms -> 0.5-0.8 ms (run 8 final). Not separable from G1 in one run; isolate before recommending outside CI. Must be set together with G1: the thread cap also shrinks the fsync permit pool 512 -> 32 (`crates/ecstore/src/disk/os.rs:1049-1061`) | One env var; no fsync on commit, acceptable for CI |
| G3 | `RUSTFS_ALLOCATOR_RECLAIM_ENABLED=true`, `_INTERVAL_SECS=30` | Lets `mi_collect` run; smaller retained heaps. Caveat: it refuses to run while request/scanner activity is visible (`allocator_reclaim.rs:28-31`), so under continuous CI load it may never fire | One env var |
| G4 | System logs of the CAS lanes on a plain disk, not the `cas_s3` policy | Run 1 vs run 2: RSS plateau at 14.6 GB versus 0.21 GB/min unbounded growth, and the S3 connection pool 137 versus 2575 held sockets. Also shrinks the RustFS object count | One config override; loses CAS coverage of the system-log write path |
| G5 | Explicit `max_server_memory_usage` for msan/tsan CAS lanes with 3x headroom for shadow and origin | Turns silent thrash into a clear error; prevents the cgroup-derived value from being wrong | One setting |
| G6 | Raise `cas_gc_interval_sec` on CI lanes so rounds do not stack | Palliative for H7 while the janitor is fixed | One setting |
| G7 | Bound the server's S3 connection pool toward the object store | The 2575-socket storms drive the RustFS thread ratchet (§2.4) | One setting |

## 4.3 Code {#code}

| # | change | expected effect | cost |
|---|---|---|---|
| K1 | GC dead-life janitor: make pages per round and page size settings (`gc_round_janitor_pages`, `gc_janitor_page_keys`), run pages while candidates and a time budget remain, and move `namespace_cleanup` **before** `defer_decision` so a round does not list what it is about to delete (`CasGc.cpp:352-386,363`) | Directly attacks the 93-95% debris in the LIST. Smallest measurable first step; A/B on the rig with `pages=10` | Small |
| K2 | Batch the janitor's deletes through the existing bulk-delete path (`gc_bulk_delete_chunk_keys`), skipping the per-key HEAD where the LIST already carries the etag | Removes the per-object HEAD + remove cost at ~7 ms each | Medium |
| K3 | Replace the global hint enumeration (`enumerateRefPrefix`, `CasGc.cpp:3955`) with the catalog cut plus one frontier probe per **live** life and a per-life LIST only for lives that fold (`CasGc.cpp:4003`) | The real O(debris) fix: 20 LIST pages / 19,415 keys at 93% debris would become ~30-50 small requests with zero debris sensitivity | Large |
| K4 | ~~Merge the fold read-ahead branch~~ **Correction 2026-09-16:** the fold read-ahead (`GcReadAhead`, `cas_gc_read_concurrency`, default 16) has been in `antalya-26.6` since `8f6cd74a3f5` (2026-09-05) and is in v26.6.4, i.e. in the CI binary of this shard and of run 8. The `fold_ref_intake` growth measured here is WITH the read-ahead on; its remaining serial step is one manifest GET per ref log (`[gc-intake-manifest-edge-serial-chain]`) | Not a lever any more; the intake fix is the manifest-edge speculation | Backlog entry exists |
| K5 | Document the `MemoryWorker` cgroup-sum caveat at `MemoryWorker.cpp:166-190`, or add a per-process fallback when the cgroup holds foreign processes | Prevents the next person from reading `current RSS: 25.01 GiB` as the server's own | Small |
| K6 | Shutdown watchdog: when `clickhouse stop` exceeds N seconds, dump `system.stack_trace` over TCP and `/proc/<pid>/status` before the TRAP | Diagnoses a real shutdown hang, should one occur after C8 | Small, CI-side script |
| K7 | Populate the `round` column on `Phase` rows of `cas_gc_log` (today 0; `round_id` must be used instead) | Makes the phase table of §2.6 a one-liner | Small |
| K8 | The MSan/TSan branch of PR #2349 (tracker snap) | Accounting only, not demand; removes a confound from future measurements | Small |

# 5. Debuggability wishlist {#debuggability-wishlist}

What would have answered this in one CI run instead of eight local runs, in priority order.

1. **System tables scraped before the stop** — `metric_log`, `asynchronous_metric_log`, `cas_log`,
   `cas_gc_log`, `blob_storage_log`, `part_log`; today a hung server loses all of them (§1.4). Move the
   scrape before the stop, or dump from the live server:
   `clickhouse-client --query "SELECT * FROM system.metric_log WHERE event_time > now() - 600 FORMAT TSVWithNamesAndTypes"`.
2. **Per-process `/proc` samples for everything in the container**, every 30 s:
   `for p in $(pgrep -P 1); do awk '/VmRSS|VmHWM|Threads/' /proc/$p/status; done` — shows the object store's
   memory immediately.
3. **cgroup accounting**, every 30 s: `cat /sys/fs/cgroup/{memory.stat,io.stat}`. `anon`, `pgsteal`,
   `workingset_refault_file` and `memory.events` separate the cliff from reclaim at a glance.
4. **Device latency**: `iostat -x 30` plus the volume type and burst-credit metric. Settles H6.
5. **RustFS admin metrics**: `/rustfs/admin/v3/metrics` with SigV4 every 60 s, per-op counts and
   `acc_time_ns` — how §2.5 was measured.
6. **A background-task latency probe at Information level.** `Moving`, `DiskLocalCheckThread` and
   `DatabaseCatalogDropTableTask` carry `Execution took N ms` and were the decisive signal, but are emitted
   only above a threshold; a low-rate unconditional sample would be better.
7. **Shutdown diagnostics before the TRAP** (K6), and a TRAP that reaches the server rather than the
   watchdog (§1.6).
8. **A stack collector that cannot freeze the target**: `SIGCONT` after every debugger timeout, and no
   ptrace attach on binaries whose symbols cannot load within the ceiling (§1.6).

# Appendix {#appendix}

**Run index.** `RIG/run{0..8}/`, each with `clickhouse-test.log` (epoch-prefixed), `samples/` (30 s metric
sampler; 60 s RustFS `/proc` and admin samples; 5 min thread and stack histograms), `server_logs/` and
`shutdown/`. Per-run reports `RIG/analysis/REPORT_run{1,2,3,45,7}.md` plus `CI_SERVER_LOG_TIMELINE.md`, with
derived TSVs alongside (`ci_timeline.tsv`, `local_vs_ci.tsv`, `run7_rustfs_ops.tsv`, `run7_rustfs_series.tsv`,
`run{2,3,4,5,7}_tests.tsv`).

**Scripts.** `RIG/`: `start_rustfs.sh`, `start_server.sh`, `sampler.sh`, `rustfs_sampler.sh`,
`cache_sampler.sh`, `run_tests.sh`, `stop_all.sh`, `start_all.sh` (runs 4+), `apply_throttle.sh` (run 6).
**Reproducing the rig:** `RIG/RUNBOOK.md` — §1 binary provenance, §2 configs and the two deliberate
divergences from CI, §3 ports, §6 test invocation, §7 start/stop, then one section per run from "Run 2".

**Other sources.** CI: `tmp/pr2300-cicd-watch/run10/{job_msan_cas_2of3.log,msan_cas_2of3_server.log.zst}`,
`masterci_v26.6.4/`, `msan/{REPORT.md,cas.tsv,plain.tsv,joined.tsv,msan_cas_2of3_timeline.tsv}`. RustFS
source: `rustfs_src/analysis/RUSTFS_MEMORY.md` (rc.3 @ 1aae680). GC:
`/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/BACKLOG.md` `{#gc-backlog-runaway}`.
