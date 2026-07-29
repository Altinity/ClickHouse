---
description: 'Performance audit of the CAS GC on the 2026-07-29 T14 soak specimen (6 workers, 40 GB budget, ~29 GiB pool). The GC leader completed zero folding rounds in 41.5 minutes; the run itself died of a 178.9 s startup against a 180 s health gate; and the dominant cost on the specimen was not GC at all but 248,400 relink-refusal exceptions in 32 minutes. Written from a post-mortem log salvage after the live cluster was destroyed before it could be sampled.'
sidebar_label: 'GC perf audit — T14 soak specimen'
sidebar_position: 21
slug: /superpowers/reports/gc-perf-audit-soak
title: 'CAS GC performance audit — the 6/40 T14 soak specimen (2026-07-29)'
doc_type: 'reference'
---

# CAS GC performance audit — the 6/40 T14 soak specimen {#title}

## Scope, and the evidence rule {#scope}

This audit was commissioned as a live sampling of the T14 soak cluster: `system.trace_log`
aggregates, the per-round per-phase rows of `system.content_addressed_garbage_collection_log`,
`system.events`, `system.errors`. **None of that was obtained** — the cluster was destroyed before
the first query could run (see [below](#specimen-lost)). What follows is built from a salvage of the
artifacts that outlived the containers.

Every figure below names the file it came from, and every named file is in
`tmp/t14/gc_audit/` (scratch, deliberately not committed). Where a question cannot be answered from
those files, this report says so instead of estimating. Three of the commissioned questions are in
that category and are marked **UNANSWERABLE** in place.

Two systematic cautions apply to every number derived from the server text logs:

- **Trace-level S3 lines are rate-limited.** `WriteBufferFromS3` Trace lines carry a
  `LogSeriesLimiter` annotation, so any S3 operation count derived by counting log lines is a
  **lower bound**, not a measurement. Counts sourced from ProfileEvents (the `gc_phases` row) are
  exact; counts sourced from log lines are labelled as bounds.
- **Two log streams, two time spans.** `clickhouse-server.log` on ch1 had rotated and covers
  07:37:56–08:31:29 UTC; `clickhouse-server.err.log` was not rotated and covers the whole run from
  07:24:44. Figures are labelled with the stream they come from.

All times are UTC. Run origin `t+0` = 07:23:43 (`base_time=1785309823`,
`tmp/t14/gc_audit/t14_soak.log`).

## The specimen was destroyed before it could be sampled {#specimen-lost}

At the first command of this audit there were no `ca-soak-*` containers in `docker ps -a` — not
running, not exited — `docker compose ls -a` was empty, and ports 8123/8124 refused connections. The
host-mounted server logs stop at 08:31:29, so teardown happened roughly 15 minutes before the audit
began.

The loss is structural, not accidental. `utils/ca-soak/docker-compose.yml` mounts only the binary,
the config fragments and `./logs/chN` into each server (`ch1` volumes at lines 73-88). There is no
top-level `volumes:` section and no `/var/lib/clickhouse` mount, so every system table — `trace_log`,
`content_addressed_garbage_collection_log`, `events`, `errors`, `text_log` — lived in the container
writable layer and died with it. No ca-soak data volume survives in `docker volume ls`.

**Any future soak intended as a performance specimen must dump its system tables to
`utils/ca-soak/logs/` before `compose down`, or persist `/var/lib/clickhouse` on a named volume.**
This is the single highest-value change suggested by this audit, because it is what separated a
four-hour instrumented run from an answerable question set.

What did survive is the host-mounted text logs, and those were preserved deliberately: the re-run
was asked to archive `logs/ch1` and `logs/ch2` rather than delete them, so the specimen's raw logs
are at `utils/ca-soak/logs/{ch1,ch2}_pre_t14b_20260729T110907/` and every extraction below can be
re-run against them.

## What survived, and what it can answer {#what-survived}

| Artifact | File | What it carries |
|---|---|---|
| Soak driver log | `t14_soak.log` | stage timeline, throttle, checkpoint verdicts, the GC-phase summary line |
| Failure record | `failure_t14.json` | the checkpoint failure, last fault, per-node convergence |
| Driver metrics DB | `soak_t14_stagea.db` → `metrics.tsv`, `gc_phases.tsv`, `gc_phases_events_json.json` | 72 per-tick rows (parts, rows, pool bytes, RSS, 16 CAS signals) + one GC per-phase row with exact ProfileEvents |
| fsck cost probe | `t14_fsck_cost.log` | pool size and a timed product `ca-fsck` |
| Server logs, both nodes | `ch1_*.txt`, `ch2_*.txt` | GC round outcomes, exception shapes and rates, stack-frame histograms, restart timeline |

The driver metrics DB is the reason this audit has exact GC numbers at all: it captured a
`gc_phases` row with a full ProfileEvents delta before the cluster died.

## 1. The headline: a folding round that never ended {#headline}

**On ch1, the GC leader completed zero rounds in 41 minutes 32 seconds, and was still inside the
same round when the process was killed.**

The evidence is the absence of a log line that the code emits unconditionally.
`CasGcScheduler.cpp:306-312` logs every completed round at Debug — either
`CA GC round {}: deferred (skip-unchanged; ...)` or
`CA GC round {}: candidates={} deleted={} ...` — and `CasGcScheduler.cpp:334` logs every *failed*
round via `tryLogCurrentException(log, "CA GC round failed (will retry next tick)")`. A round
therefore cannot finish, in any outcome, without leaving a line.

`tmp/t14/gc_audit/ch1_gc_all.txt` contains **four** GC lines for the entire run, across every log
file including the rotated ones:

```
07:24:18.286  CA GC round 0: deferred (skip-unchanged; no changed shard reached the fold threshold ...)
07:24:28.305  CA GC round 0: deferred (skip-unchanged; ...)
07:24:38.313  CA GC round 0: deferred (skip-unchanged; ...)
08:08:08.658  CA GC: lease held by another mounter (tick 1)      <- after the restart
```

Three deferred rounds on the 10 s cadence, then nothing. The fourth round began at **07:24:49.502**
(`tmp/t14/gc_audit/ch1_thread723.txt`, the first S3 activity on the scheduler thread after the third
deferral) and never produced an outcome line. The old process's last log line is at **08:06:21.406**,
immediately before the successor's `Application: Starting ClickHouse` at 08:07:18.073 with no
shutdown sequence — a hard kill. The round was therefore in flight for at least **41 min 32 s**.

That the round was *working*, not blocked, is shown by the only lines the scheduler thread did emit
during those 41 minutes — seven clusters of S3 retry warnings, each naming the key being read
(`tmp/t14/gc_audit/ch1_thread723.txt`):

```
07:29:17.951  ... soak_pool/cas/refs/ca_soak_ch1/store/679/...
07:40:27.839  ... soak_pool/cas/manifests/ca_soak_ch1/store/679/...@cas@/0000000000000001-0000000000020df4/000001.zst
07:42:03.769  ... soak_pool/cas/manifests/ca_soak_ch1/store/679/...
07:56:59.846  ... soak_pool/cas/manifests/ca_soak_ch2/store/ffe/...@cas@/0000000000000001-00000000000185a4/000001.zst
07:57:03.881  ... soak_pool/cas/refs/ca_soak_ch2/store/ffe/...
```

These are the ~1-in-N requests that hit a transient rustfs timeout and were retried; the successful
majority are not logged at all. They prove the round was still fetching manifest bodies and ref
objects 32 minutes in, across both server namespaces — i.e. it was in the pool-wide fold, not stuck
on a lock.

Corroboration from the other node: `tmp/t14/gc_audit/gc_phases.tsv` records ch2 at **248 rounds,
`acquired=0`, `steal_allowed=248`** — 248 consecutive rounds in which ch2 was allowed to steal the
lease and never could, because ch1 held it continuously without completing a round.

Note also that the heartbeat did **not** starve, which rules out the tempting explanation that a long
round blocks the lease beat. `tmp/t14/gc_audit/ch1_derived_facts.txt` records 100 `soak_pool/gc/hb`
and 35 `soak_pool/gc/server-roots/ca_soak_ch1/mount` writes between 07:30:00 and 08:06:30 — surviving
Trace lines only, so a lower bound — emitted by threads other than the scheduler's (724, 722, 2874),
and sampled at a clean 10 s cadence in the 08:04-08:06 window right up to the kill. That is exactly
what `CasGcScheduler::heartbeatLoop` (`CasGcScheduler.cpp:339-375`) is built to do; its own comment
states the reason: "A long round updates the durable lease only when it completes, so without these
pulses a follower could mistake a live leader for a dead one." The advisory heartbeat therefore
masked the stall from followers, and only the missing round-outcome lines reveal it.

### Cost structure: what is measurable and what is not {#fold-cost-structure}

The commissioned breakdown — GETs per log, GETs per manifest edge, HEAD-vs-GET ratio, serial RTT
estimate — is only partly recoverable.

**Measurable, from code.** `Gc::foldManifestEdges` (`Gc/CasGc.cpp:1007-1018`) performs, per manifest
edge, exactly one `backend.head(key)` followed by one `backend.get(key)`:

```cpp
const HeadResult head = backend.head(key);
if (!head.exists)
    return false;
const auto got = backend.get(key);
if (!got)
    return false;
ProfileEvents::increment(ProfileEvents::CasRefManifestBodyFoldGets);
```

So the structural HEAD:GET ratio is **1:1 by construction**, two serial round trips per manifest
edge, and `CasRefManifestBodyFoldGets` counts exactly the GET half. The HEAD is pure overhead in the
common case where the body exists — it exists only to distinguish "absent body" from "raced delete",
and the `get` already returns falsy on a 404 (the very next branch). This is the redundancy Task 15
removes.

**UNANSWERABLE: the absolute GET volume and rate.** The controller's live observation of
`CasRefManifestBodyFoldGets` exceeding 1M at 313-950 gets/s, and the ~2.14 manifest-body GETs per
log, came from `system.events` queries against the running server. `system.events` is gone, and no
surviving file contains those counter values — `grep -r CasRefManifestBodyFoldGets tmp/t14/gc_audit/`
returns only the *text of the queries* the controller ran, echoed in
`tmp/t14/gc_audit/ch1_cas_lines.txt` (`SELECT value FROM system.events WHERE
event='CasRefManifestBod...'`), never a result. Those figures are recorded here as the controller's
live observation and are **not independently verified by this audit**. The same applies to any
per-log or per-edge ratio.

**Measurable, as a lower bound on serial RTT.** The one exact S3 timing that survives is ch2's lease
phase (`tmp/t14/gc_audit/gc_phases_events_json.json`): 496 `S3GetObject` + 496 `S3HeadObject` =
992 `S3ReadRequestsCount` costing `S3ReadMicroseconds=1515499`, i.e. **~1.53 ms per S3 read round
trip** against in-container rustfs. At that RTT, the 1:1 HEAD:GET pairing in `foldManifestEdges`
costs ~3.05 ms of serial latency per manifest edge, and a fold over N edges cannot beat ~3.05 ms × N
however fast the storage is, because the two calls are strictly sequential and the loop is serial.
Removing the HEAD halves that floor.

### Where this lands in the fix landscape {#fix-landscape}

The stall is the O(pool) one-pass fold meeting a ~29 GiB hot pool, which is precisely what
`docs/superpowers/cas/BACKLOG.md` already tracks:

- **Task 15 (bounded rounds / frozen tail)** is the load-bearing fix: a round that always terminates
  turns every downstream symptom below into a non-event. `{#fsck-scale-timeout}` in the BACKLOG
  already names it as the backlog-drain dependency.
- **`[Lever B]` incremental point-updatable in-degree** carries the scaling data this specimen
  confirms qualitatively: "GC round is O(pool objects) — 87 ms@400 parts → 93 s@10k tables →
  398 s@100k parts". The specimen is the next point on that curve: unbounded at ~29 GiB.
- **`[T1]` delta-runs** removes the O(edges) snapshot rewrite that a hot pool pays every pass.
- **`[PROBE-A-CADENCE-UNIT]`** is the already-recorded consequence: sampling on `round % period == 0`
  never fires when rounds do not complete. This audit's timestamps confirm the premise exactly —
  zero completed rounds, so `CasGcProbeADue`/`Performed` could not leave zero.

## 2. The five watch-items {#watch-items}

### (a) `pthread_mutex_lock` wall samples — UNANSWERABLE {#watch-mutex}

The ~57k query-side wall samples were read from `system.trace_log`, which did not survive. Nothing in
the salvaged files attributes wall time to a mutex, and there is no way to identify the mutex from
deeper frames without the stacks. This question needs the next specimen.

### (b) Local-file churn (`open64`/`unlink`/`mkdir`) — UNANSWERABLE as a share {#watch-file-churn}

Quantifying its share of CPU/wall requires `trace_log`. What the salvage does show is that the churn
is real and that its largest single source is not the CA staging path: `tmp/t14/gc_audit/ch1_rollup.txt`
records `CachedPartFolderAccess` as the 6th-hottest logger at 54,586 lines and `FakeDiskTransaction`
at 5,556, against 1,155,235 for the table's own logger. A share-of-CPU figure cannot be derived from
log-line counts and is not offered.

### (c) libunwind frames — mechanism identified and quantified, CPU share UNANSWERABLE {#watch-libunwind}

The CPU share needs `trace_log`, but the *cause* of libunwind in a CPU profile is now unambiguous,
and it is large enough that it would dominate any profile taken on this specimen.

`tmp/t14/gc_audit/ch1_rollup.txt` counts **622,148 stack-frame lines** and 27,240 Error-level lines in
ch1's 53-minute log window. `tmp/t14/gc_audit/ch1_stack_frames.txt` shows those frames are one
exception shape: **27,166** occurrences of
`DataPartsExchange::Fetcher::relinkPartToDisk` (`DataPartsExchange.cpp:1549`) under
`fetchSelectedPart` → `executeFetch` → `executeLogEntry` → `processQueueEntry`, at ~22.9 symbolized
frames per trace.

Over the whole run the count is far larger. `tmp/t14/gc_audit/ch1_error_codes.txt` and
`ch2_error_codes.txt` give **123,907 on ch1 and 124,493 on ch2 — 248,400 cluster-wide** instances of:

```
Code: 210. DB::Exception: Source <other node> did not prove it still holds the manifest it offered
for part <part> by relink; the relink is abandoned and the fetch will be retried later. (NETWORK_ERROR)
```

Each node is failing to relink from the other (`680e574ae569` = ch1, `e836b305d40a` = ch2, per the
node column of `tmp/t14/gc_audit/metrics.tsv` and ch1's own startup line `hostname=680e57...`).
`tmp/t14/gc_audit/ch1_relink_per_min.txt` gives the shape: the storm runs 07:24–07:56 and peaks at
**9,219 per minute at 07:26** (~154/s on one node), decaying to 2 by 07:56.

This is the already-tracked `[RELINK-CONFIRM-BUSY-LANE]` finding in `BACKLOG.md:175`, and this audit
sharpens it: the BACKLOG records "~1000-3700/min, ~106k rows per 20 soak minutes", where the measured
peak is 2.5× the top of that range and the per-node total is ~124k over 33 minutes. It is fail-closed
and correct — `failure_t14.json` shows both replicas converged to identical `count`, `sum_fp` and
`sum_version` — so the cost is CPU, log volume and lost dedup, not correctness.

**This is the most important caveat in the audit: a CPU profile taken on this specimen would be
measuring the relink storm, not the GC.** 248,400 exceptions with ~23 symbolized frames each, at
ERROR severity, in ~32 minutes, is the explanation for libunwind in the CPU top, and it is unrelated
to garbage collection.

### (d) `CANNOT_PARSE_INPUT_ASSERTION_FAILED` — UNCONFIRMABLE, and absent from the logs {#watch-parse-errors}

The reported 28k pre-restart occurrences came from `system.errors`, which did not survive.
Independently: `grep -c CANNOT_PARSE_INPUT_ASSERTION_FAILED` over ch1's full `err.log` returns **0**,
and the string appears nowhere in `tmp/t14/gc_audit/ch2_error_codes.txt` either. The complete list of
exception shapes seen on either node is in `ch1_error_codes.txt`/`ch2_error_codes.txt`: the relink
refusal, then `Broken pipe` (16), `Write buffer has been canceled` (10), `Cannot execute query in
readonly mode` (10), and a handful of syntax errors from the driver's own introspection queries.

So the counter was incremented by a path that does not log at Warning or above — most plausibly a
caught-and-counted parse failure — and its provenance **cannot be determined from the salvage**. It
needs `system.errors` plus `query_log` on the next specimen.

### (e) GC lease locality on ch2 — ANSWERED, and the cost is small {#watch-lease-locality}

This one the salvage answers exactly, from ProfileEvents rather than logs.

`tmp/t14/gc_audit/gc_phases.tsv`: ch2 ran **248 rounds, `phase=lease` only**, `total_us=1678490`,
`max_us=82603`, with `metrics_json` (`gc_phases_metrics_json.json`) showing `acquired=0`,
`steal_allowed=248`. `detector: (no fold phase ran)` in `t14_soak.log` confirms ch2 never folded.
`tmp/t14/gc_audit/ch2_gc_all.txt` shows the corresponding 390 log lines,
`CA GC: lease held by another mounter (tick N)` every 10 s, escalating to a single Information line
at tick 140 (`... for 140 consecutive ticks (normal for a follower; investigate if no m...)`).

The per-round cost of learning "I am not the leader"
(`tmp/t14/gc_audit/gc_phases_events_json.json`):

| Quantity | Value | Per round |
|---|---|---|
| Rounds | 248 | — |
| Lease-phase wall | 1,678,490 µs | 6.77 ms |
| `S3GetObject` | 496 | 2 |
| `S3HeadObject` | 496 | 2 |
| `S3ReadMicroseconds` | 1,515,499 | 6.11 ms (90.3% of the phase) |
| `ReadBufferFromS3Bytes` | 53,795 | 108 B per GET |

So an idle follower pays **4 S3 requests and 6.77 ms per round**, of which 90% is S3 read latency,
to move 108 bytes. At the 10 s cadence that is 0.4 requests/s/node — real but small, and worth
recording precisely so it is not mistaken for a cost driver. It matters only in the shape it would
take at scale (N followers × cadence), not on this specimen. The `max_us=82603` outlier (82.6 ms in
one round) is the same S3 tail latency visible in the fold's retry clusters.

## 3. Part churn, and what the restart actually cost {#part-churn-restart}

The commissioned datum was "part generations reached 74k+ in ~35 min; MergeTree restart took >3 min
loading Outdated parts". The first half is confirmed; **the second half is wrong in an instructive
way.**

**Part generations: confirmed.** The maximum block number over ch1's log window is **74,672**
(`max_max_block` from the `20260729_<min>_<max>_<level>` part names in ch1's current
`clickhouse-server.log`). `tmp/t14/gc_audit/metrics.tsv` gives the concurrent picture: `parts_inactive`
peaks at **32,762** on ch1 at t+656 s (32,392 on ch2), against `parts_active` in the single and low
double digits throughout — a merge/mutation churn profile, with rows peaking at 5,426,012 (t+870 s)
and TTL/mutations draining it to 1,393,021. Pool bytes end at **31,143,704,824 (29.0 GiB)**, matching
the independent `du -sb` of 31,147,968,714 in `tmp/t14/gc_audit/t14_fsck_cost.log`.

**The restart was not slow because of Outdated parts on the user table.** From
`tmp/t14/gc_audit/ch1_timeline.txt` and the startup-window rollup:

| Time | Event | Δ |
|---|---|---|
| 08:07:18.073 | `Application: Starting ClickHouse` | — |
| 08:07:18.632 | `CasPool`: stale-looking mount lease held by predecessor; `CasMountLease` waiting ~36500 ms | +0.6 s |
| 08:07:58.645 | first mount write — the lease wait ends | **+40.0 s** |
| 08:07:58.852 | `Application: Ready for connections` (this is what answers `/ping`) | +40.8 s |
| 08:10:16.982 | `ca_soak.ca_stress: Loading data parts` | **+138.1 s** |
| 08:10:16.997 | `Loaded data parts (1 items) took 0.015369321 seconds` | +0.015 s |
| 08:10:17.016 | `Loaded 6 outdated data parts asynchronously` | — |

The user table loaded **1 active part in 15 ms** with 6 outdated parts. The 178.9 s from process
start to the table being usable decomposes into two costs, neither of which is user-table part
loading:

1. **40.0 s — the CAS mount-lease token-stability wait.** After an unclean kill the successor cannot
   distinguish a dead predecessor from a live one, so it waits out the token window
   (`Attempting to mount content-addressed server root ca_soak_ch1 after node change or hard restart;
   waiting ~36500 ms`). This is designed safety, not a bug, and it is not a consequence of the stuck
   round — the heartbeat was current until 08:06:20.
2. **138.1 s — reloading the *system log* tables' Outdated parts.** The startup-window message
   rollup in `tmp/t14/gc_audit/ch1_derived_facts.txt` is dominated by
   `system.content_addressed_log` (**299** `Loading Outdated part` lines, 310
   `Finished loading Outdated part`, 310 `Loaded metadata version 0`), then
   `system.asynchronous_metric_log` (114), with `system.part_log`, `system.blob_storage_log`,
   `system.trace_log`, `system.metric_log` and `system.text_log` behind them. Activity is uniform at
   143-306 log lines per 10 s across the whole window — steady work, no stall.

**This is what killed the run.** `tmp/t14/gc_audit/failure_t14.json` records the verdict
`(ping, table_loaded) states={'Node(localhost:8123)': (True, False), 'Node(localhost:8124)': (True, False)}`
— pingable, tables not loaded, exactly the state the server was in between 08:07:58 and 08:10:17. The
harness declared failure shortly after its metrics tick at 08:08:40 (`t14_soak.log`, tick #33,
ts=1785312520), i.e. ~96 s before the table finished attaching.

The CA angle is direct and, as far as this audit can tell, **not currently tracked**: the
instrumentation that makes a soak a specimen — `system.content_addressed_log`, mounted via
`configs/ca_event_log.xml` — is itself the largest single contributor to restart time, at 299
Outdated parts on a run that lasted 43 minutes. `ch1_rollup.txt` shows 11,421 log lines from the
`system.content_addressed_log` logger during normal operation.

What the salvage **cannot** show is what the churn did to CA specifically: manifest counts and
repoint traffic per unit of merge churn need `system.content_addressed_log` and
`content_addressed_garbage_collection_log` rows, both of which died with the containers. The
merge-churn-to-CA-traffic ratio remains an open measurement.

## 4. Ranked optimization opportunities {#opportunities}

Ranked by expected win on evidence in this report. Per the protocol-untouchable rule, anything that
changes a protocol step or an on-disk format is marked **USER-GATED** — including cases where the
change looks like a pure optimization.

### 1. Make rounds terminate (Task 15 bounded rounds) {#opp-bounded-rounds}

**Expected win:** the largest available, and a precondition for most of the others. Converts an
unbounded round into a bounded one, which restores round-cadence semantics for everything keyed on
them.
**Evidence:** zero completed rounds in 41 min 32 s (`ch1_gc_all.txt` + the round-outcome logging
contract at `CasGcScheduler.cpp:306-312,334`); ch2 blocked from leadership for 248 consecutive rounds
(`gc_phases.tsv`, `acquired=0`).
**Maps to:** Task 15, with `[Lever B]` and `[T1]` in `BACKLOG.md {#gc-scalability}` as the structural
follow-ups.
**Risk class:** internal scheduling if the tail is frozen without changing durable layout;
**USER-GATED** if delivered via `[T1]` delta-runs or `[Lever B]`, both of which change what GC
writes and reads on the pool.

### 2. Stop paying ERROR-severity exceptions for an expected outcome {#opp-relink-noise}

**Expected win:** removes ~248,400 exception constructions with ~23 symbolized frames each per
32 minutes of busy two-node operation (`ch1_error_codes.txt`, `ch2_error_codes.txt`,
`ch1_stack_frames.txt`), and with them the single largest distortion in any profile taken on a busy
CA cluster. Also removes ~600k stack-frame lines per node per hour from the logs
(`ch1_rollup.txt`: 622,148 frame lines in 53 min).
**Evidence:** peak 9,219 refusals/min on one node (`ch1_relink_per_min.txt`); zero correctness
consequence (`failure_t14.json`, replicas converged identically).
**Maps to:** `[RELINK-CONFIRM-BUSY-LANE]`, `BACKLOG.md:175`, sub-items (b) and (c) — demote severity
and add a `ProfileEvents` proven/refused pair so the rate is a metric rather than log spam.
**Risk class:** the severity demotion and the counter pair are non-protocol and low risk. The
underlying availability fix — refining `confirmExactRef` rule 3 so a pending mutation does not refuse
every ref — is **USER-GATED** (it changes what the handoff protocol will assert).

### 3. Drop the HEAD in `foldManifestEdges` {#opp-fold-head}

**Expected win:** halves the serial round trips in the fold's inner loop — from ~3.05 ms to
~1.53 ms per manifest edge at the measured 1.53 ms RTT (`gc_phases_events_json.json`). On a fold
whose cost is dominated by a serial per-edge loop, this is a ~2× reduction in the latency floor and
a 50% cut in that request class.
**Evidence:** `Gc/CasGc.cpp:1007-1018` performs `head` then `get` per edge, and the `get` already
returns falsy on a raced delete, making the HEAD redundant on the hit path; measured per-request S3
read latency of 1.53 ms from ch2's lease phase.
**Maps to:** Task 15's scope as described in the commission (already in flight).
**Risk class:** non-protocol — it removes a request, changes no object and no step ordering. The one
behaviour to preserve is the absent-vs-raced-delete distinction the HEAD currently draws; the
`!got` branch must keep fail-closing for committed bodies.
**Caveat:** the absolute win cannot be sized without the GET counts, which did not survive.

### 4. Stop the CA audit log from dominating restart {#opp-ca-log-restart}

**Expected win:** up to ~138 s off cold start on a run of this shape — the difference between passing
and failing a 180 s health gate, which is exactly what this run failed.
**Evidence:** `system.content_addressed_log` contributed 299 of the Outdated parts reloaded during
the 138.1 s pre-user-table window (startup rollup), against 1 active part and 15 ms for the user
table (`ch1_timeline.txt`).
**Maps to:** NEW — not found in `BACKLOG.md` during this audit.
**Risk class:** low and non-protocol; it is flush cadence and part-size policy for a system log
(`configs/ca_event_log.xml`), plus possibly a lighter retention. Worth confirming on the next
specimen that the same profile appears without the soak's instrumentation config, so the fix targets
the product default rather than the harness.

### 5. Back off the follower's lease poll {#opp-follower-poll}

**Expected win:** small, and stated as such: 4 S3 requests and 6.77 ms per round per idle follower,
0.4 requests/s at the 10 s cadence (`gc_phases.tsv`, `gc_phases_events_json.json`). It earns a place
in the list only because it is pure overhead and scales linearly with follower count and cadence.
**Evidence:** 248 rounds, `acquired=0`, 90.3% of the phase in `S3ReadMicroseconds` to move 108 bytes
per GET.
**Maps to:** NEW; adjacent to `[GC-EMPTY-SHARD-PROBES]` in `BACKLOG.md {#gc-scalability}`, which
covers the analogous constant-probe floor.
**Risk class:** **USER-GATED** — backing off the poll changes how quickly a follower notices a dead
leader, which is lease-protocol timing, not a local optimization.

**Deliberately not ranked:** `[FSCK-SCALE-TIMEOUT]` (`t14_fsck_cost.log`: `FSCK_SECONDS=731.1`,
`FSCK_EXIT=159`, i.e. the product `ca-fsck` returned nothing at all on the 29.0 GiB pool) is a real
and severe finding, but it was measured and filed by the controller before this audit and is
GC-adjacent rather than GC. It belongs to the same root cause as item 1: an unbounded round leaves an
unbounded backlog for fsck to audit.

## Evidence index {#evidence-index}

All paths relative to `tmp/t14/gc_audit/`; line counts in `MANIFEST.txt`.

| File | Lines | Provenance |
|---|---|---|
| `t14_soak.log` | 148 | copy of `build/t14_soak.log`, soak driver stdout |
| `failure_t14.json` | 52 | copy of `utils/ca-soak/failure_t14.json` |
| `soak_t14_stagea.db` | — | copy of `utils/ca-soak/soak_t14_stagea.db` (driver metrics DB) |
| `metrics.tsv` | 73 | dump of the DB's `metrics` table, 72 per-tick rows |
| `gc_phases.tsv`, `gc_phases_metrics_json.json`, `gc_phases_events_json.json` | 2 / 3 / 24 | the single GC per-phase row and its embedded maps |
| `t14_fsck_cost.log` | 8 | copy of `build/t14_fsck_cost.log` |
| `ch1_gc_all.txt`, `ch2_gc_all.txt` | 4 / 390 | all `ContentAddressedGC` lines, every log file incl. rotated |
| `ch1_thread723.txt` | 153 | every line emitted by ch1's GC scheduler thread |
| `ch1_error_codes.txt`, `ch2_error_codes.txt` | 10 / 8 | exception-shape histogram over the full `err.log` |
| `ch1_relink_per_min.txt`, `ch2_relink_per_min.txt` | 33 / 33 | relink-refusal counts per minute |
| `ch1_stack_frames.txt`, `ch2_stack_frames.txt` | 80 / 80 | stack-frame function histogram |
| `ch1_rollup.txt`, `ch2_rollup.txt` | 18899 / 19005 | level and logger rollup of the main log |
| `ch1_err_shapes.txt`, `ch2_err_shapes.txt` | 2652 / 2641 | Warning+ message-shape rollup |
| `ch1_timeline.txt`, `ch2_timeline.txt` | 49 / 66 | startup/shutdown/part-loading markers |
| `ch1_derived_facts.txt` | 73 | figures computed inline: max block number, kill/restart boundary, user-table load, mount-lease wait, startup rollup, heartbeat writes, parse-error count |
| `extract.sh`, `extract_gc.sh`, `derive.sh` | 70 / 45 / 60 | the extraction scripts, re-runnable against any log dir |

**The raw specimen logs were preserved** and the whole extraction is reproducible: at this audit's
request they were archived rather than deleted when the re-run started, and now live at
`utils/ca-soak/logs/ch1_pre_t14b_20260729T110907/` and `.../ch2_pre_t14b_20260729T110907/`. Every
script above was re-run against that archive to confirm it reproduces
(`docker run --rm -v <archive>:/l1:ro -v tmp/t14/gc_audit:/o alpine:3.20 sh /o/derive.sh /l1`).

The logs are `syslog:syslog 0640` and were read through a root container, the host account having no
passwordless sudo — hence the `docker run` wrapper on every extraction rather than a plain `grep`.

## What the next specimen needs {#next-specimen}

To answer the three UNANSWERABLE items and size opportunity 3, the next soak must, **before**
teardown, dump to `utils/ca-soak/logs/`:

1. `system.content_addressed_garbage_collection_log` in full — the per-round per-phase rows are the
   only exact source of fold cost, and on this run ch1 never wrote one because the round never ended.
   A round that never completes writes no row: **the GC log cannot observe the pathology it most
   needs to report**, which is itself an argument for a periodic in-round progress row.
2. `system.trace_log` aggregates (top stacks by count, split query-side/background, symbolized).
3. `system.events` and `system.errors` full dumps, per node, before and after the fault window.

Items (a), (b) and (d) of the watch-list are unanswerable for exactly this reason and carry forward
unchanged.
