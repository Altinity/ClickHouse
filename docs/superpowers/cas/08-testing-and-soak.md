---
description: 'Testing and validation for the content-addressed (CAS) MergeTree: the adversarial scenario suite (S01–S35), the 24h soak harness, clickhouse-disks ca-fsck and ca-gc-dryrun introspection, the event and GC audit logs, and the standing findings and backlog pointers.'
sidebar_label: 'Testing and soak'
sidebar_position: 8
slug: /superpowers/cas/testing-and-soak
title: 'CAS — Testing, Soak, and Introspection'
doc_type: 'guide'
---

# CAS — Testing, Soak, and Introspection {#cas-testing-and-soak}

This document covers how the CAS MergeTree feature is empirically validated: the standalone
adversarial scenario suite (S01–S35 in `utils/ca-soak/scenarios/`), the deterministic 24h soak
harness (`utils/ca-soak/`), the independent pool-inspection tooling (`clickhouse-disks ca-fsck` and
`ca-gc-dryrun`), and the two in-server audit log tables.

Related documents: `04-gc-protocol.md` (GC protocol), `07-s3-budget.md` (S3 op budgets).
Spec sources: `specs/2026-06-13-ca-soak-test-design.md`, `specs/2026-06-13-ca-fsck-readonly-design.md`
(paths not found under `docs/superpowers/specs/` as of 2026-07-03; verify location before relying on them).

## 1. Introspection layer {#introspection}

### 1.1 Read-only disk mode {#read-only-mode}

`ContentAddressedMetadataStorage` can be opened in a read-only mode (triggered by the disk's
`readonly` config attribute, or internally by `clickhouse-disks`). In read-only mode:

- The startup capability probe is a check-only read, never a mutating write.
- No background GC scheduler or heartbeat thread is created.
- Every mutating entry point (`writeFile`, `commit`, `moveFile`, `removeRecursive`, GC entry points)
  throws fail-closed with a `READONLY` exception.
- The full read API (`existsFile`, `listDirectory`, `readFile`, `resolveRef`, `readTree`, `locate`,
  `listNamespaces`, `listRefs`) remains available.

Read-only mode is also the basis for a **WORM deployment** (one writer server, multiple
read-only mounters sharing the pool).

### 1.2 `clickhouse-disks ca-fsck` {#ca-fsck}

`clickhouse-disks ca-fsck --disk <ca_disk_name>` (the deprecated alias `fsck` still works, printing
a stderr deprecation note) opens the target CA disk read-only and verifies
pool reachability:

1. **Reachable set**: walks every namespace → every ref → every tree entry, collecting the physical
   object key of every `Blob` via `locate`. Uses the production read API, so a walk bug is also a
   read bug.
2. **All keys**: enumerates the full pool prefix via paginated `backend.list`.
3. **Classifies** each content key:
   - `reachable` — referenced by at least one live ref and present in the pool.
   - `dangling` — referenced by a live ref but **absent** from the pool. This is an INV-NO-LOSS
     violation. The ONLY hard class.
   - present-but-unreferenced blobs are classified through the GC pipeline view (2026-07-02;
     read for LABELING only — reachability never consults GC state). Under the ack-floor two-phase
     pipeline a nonzero, churning unreferenced set is the NORMAL steady state of an active pool, so
     the old single `unreachable` lump read as a leak when it was the pipeline working:
     - `pending-gc` — the present incarnation is in the retired set (condemned at round N, or
       `delete_pending`). Deletion is scheduled ~2-3 rounds out. EXPECTED.
     - `awaiting-gc` — edges still in the GC snapshot (a drop/reclaim not folded yet), or GC has
       not run on the pool at all. EXPECTED.
     - `unaccounted` — outside the whole GC view. Normal only as a transient (created + dropped
       between rounds); a PERSISTENT `unaccounted` object should be impossible under INV-2
       (reachability-before-content, `03-writer-protocol.md`) and is the anomaly signal the soak
       forensics trigger fires on.
   - `unreachable` remains the class of pre-precommit manifest debris rows (labeled
     `reclaimable-/in-flight-pre-precommit`); the summary `unreachable=` counter is the TOTAL of
     all present-but-unreferenced objects, so residual-settling loops keep one monotone number.
4. **Audits each namespace's ref stream by ARITHMETIC** (spec `2026-07-27` §7). fsck may not rest a
   verdict on an enumeration any more than the fold may: ids are dense `1..T` within
   `(namespace, writer_epoch)` (INV-1), so it reads every position by exact key from
   `_ckpt.checkpoint`'s successor upward and never from a listing. An absent expected-next has
   exactly three answers, and the walk reports one verdict per namespace:
   - nothing above it → the namespace's **frontier**. Proven, silent, the healthy case.
   - a **confirmed durable** id of the same epoch above it → `chain-broken`. Contiguity makes this
     impossible without a lost record, so the transactions above the hole are unreachable. The
     witness is confirmed by its own exact `GET` before it may convict — a listing that can omit a
     key can equally well name one that is gone, and this verdict costs an exit code.
   - only later-epoch ids above it → the crossing is **proved** through the next epoch's
     `prev_epoch_seal` back-chain (the same `crossEpochFromSeal` the fold uses), or reported
     `unchecked`.

   `ref_records_walked` counts the positions actually read and proved — what makes "the tail above
   the checkpoint was walked" observable rather than inferred from the absence of a complaint. The
   walk writes nothing: the writer's recovery closes a dead epoch by putting a seal in the slot it
   walked to, and an auditor has no business doing that, so an unclosed epoch reads as unprovable
   rather than being repaired into provable.
5. Reports `dedup_ratio` = `referenced_bytes / physical_bytes` (logical bytes referenced across all
   parts, divided by distinct blob bytes on disk).

**`unchecked` is the honest third answer**, and it is reserved for it: a namespace the audit could not
prove **either way** — an unprovable epoch crossing, an unreadable record, or a namespace whose
examination raised at all. It is COVERAGE, not a finding: it does not make a report unclean and does
not exit nonzero, exactly like `partial`. A healthy pool reports `chain_broken=0 unchecked=0`, so
`unchecked` is never a resting state, and a namespace PROVEN broken is never also counted `unchecked`
("proved broken" and "could not prove" are different answers).

**Record and continue, never wedge.** Every per-namespace step can raise `CORRUPTED_DATA` on a damaged
stream — the replay refuses a non-contiguous tail, the codecs refuse an invalid body. For *recovery*
that throw is the correct fail-close; for a read-only diagnostic it is a bug, because an fsck that dies
on the first bad namespace reports **nothing** about the ones it never reached, including the healthy
ones. So one namespace's failure becomes that namespace's verdict and the sweep goes on. (`--timeout`
is the exception: the deadline is a property of the whole scan and still aborts it, or yields
`partial=1` under `--partial`.)

**Exit code** is nonzero when `dangling`, `chain_broken`, `snapshot_oracle_mismatches`,
`corrupted_runs` or `lifeless_keys` is nonzero. That is every term of the report's `clean()` **except
one**: `stale_edge` is a `clean()` term that never exits nonzero, because it is only ever counted under
`--detail`, so a summary run's `stale_edge=0` is structural rather than a finding. The soak harness
gates that class separately (`stale_edge_verdict`), which is what licenses the exception — **a zero
exit code from a summary scan is therefore not by itself proof of a clean pool.** The tool prints de-alarm
`note:` lines when `pending_gc`/`awaiting_gc` are nonzero ("inside the normal GC deletion pipeline —
expected"), a re-run hint when `unaccounted` is nonzero, and a coverage note when `unchecked` is
nonzero — beta testers should never have to interpret raw counters.

`--detail` adds a per-object row (tab-separated `class, key, size[, reachable_from...]`) after the
summary line. `--timeout <seconds>` aborts the scan with a clear error instead of hanging (default
600; 0 = unbounded). There is no `--format json|tsv` option (not implemented as of 2026-07-03) —
output is a plain `key=value` summary line plus optional tab-separated detail rows.

### 1.3 `clickhouse-disks ca-gc-dryrun` {#gc-dryrun}

`clickhouse-disks ca-gc-dryrun --disk <ca_disk_name>` opens the disk read-only and derives the set
of objects the next GC round **would** delete, from the durable `gc/snap` (in-degree graph) and
`gc/state` (round + `retired_refs`) — zero CAS writes and zero deletes.

The key assertion (verified by the soak harness at every quiesced checkpoint):

> **`{preview deletes} ⊆ {ca-fsck unreachable}`** — GC must never plan to delete an object that
> `ca-fsck` can still reach from a live ref.

Note: `ca-gc-dryrun` currently previews `zeroInDegree` only for target shard 0 when
`gc_shards > 1`. This is a known gap (S31 regression guard; see BACKLOG section below).

Both commands reject non-CA disks with a clear message.

### 1.4 `gc/state` disaster recovery runbook {#gc-rebuild-runbook}

**Symptom:** `system.content_addressed_garbage_collection_log` shows the round stuck at
`outcome = 'Error'` with an `error` message containing `CORRUPTED_DATA` — the baseline guard
(`04-gc-protocol.md §gc-rebuild`) has refused every regular round because it found a shard journal
proving trimmed history with no healthy adopted baseline under `gc/state` (e.g. after an operator
`mc rm` of `gc/state`, or backend corruption). GC makes NO further progress and deletes NOTHING while
in this state — the guard is fail-closed by design.

**Remedy — pick one:**

1. **A live replica is up** (preferred): run
   ```sql
   SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] [<disk>]
   ```
   on any server that mounts the affected disk. Omit `<disk>` to rebuild every content-addressed disk
   on that node. The command throws with the refusal text if the rebuild itself refuses (e.g. another
   leader holds the lease, or a committed ref names a missing manifest — real data loss that needs
   `ca-fsck` forensics before reaching for `FORCE`). On success it logs the rebuilt `round`, `generation`,
   `namespaces`, `shards`, `committed_refs`, `live_precommits`, `unowned_alive_manifests`, `edges`, and
   `clamped_shards` counters.

2. **No server is up** (e.g. investigating an offline pool): from a host with `clickhouse-disks`,
   ```bash
   clickhouse-disks --disk <ca_disk_name> ca-gc-rebuild [--force]
   ```
   Requires the disk to be configured `<readonly>true</readonly>` in the `clickhouse-disks` config —
   same rule as `ca-fsck`/`ca-gc-dryrun`: this tool must never claim a live server's mount. It prints the
   report as `key=value` pairs and exits nonzero (with a `refusal=` line) if the rebuild refuses.

**After the rebuild:** regular GC rounds resume; `ca-fsck` converges to `dangling=0` over the next few
rounds as the ack-floor pipeline drains `pending-gc`/`awaiting-gc`. A rebuilt baseline is
conservative — it may over-protect a trimmed-but-live build's manifest (design delta 2 in
`04-gc-protocol.md §gc-rebuild`), so a small, bounded, `ca-fsck`-visible `unaccounted`/leaked set settling
over a rebuild-to-rebuild window is expected, not a regression.

**A rebuild RECLAIMS NOTHING** (`04-gc-protocol.md §gc-rebuild` step 6). It rebuilds cursors and edges
only, so a blob that no surviving manifest names gets no row in the rebuilt baseline and the
incremental pipeline can never reach it. Such blobs are **retained** — they show up as `unaccounted` and
stay there. This is deliberate: the condemnation that used to catch them was derived from a listing,
and a listing that omits a durable key hides a live owner, which made that pass condemn acked data.
Retention until the build/upload registry can enumerate in-flight uploads safely is the named residual
of that removal, and there is no substitute reclamation — an `unaccounted` count that does not drain
after a disaster rebuild is expected, and is not licence to delete from an enumeration.

## 2. Audit log tables {#audit-logs}

### 2.1 `system.content_addressed_garbage_collection_log` {#gc-log}

One `Start` row and one `Finish` row per GC round, emitted by `CasGcScheduler` via an injected
`GcRoundLogger` callback (no `Interpreters` dependency in the disk layer). Key `Finish` columns:

| column | description |
|--------|-------------|
| `event_type` | `'Start'` or `'Finish'` |
| `disk_name` | the CA pool the round ran on |
| `gc_id` | random `u128` hex identifying the scheduler instance (which mounter led) |
| `trigger` | `'Scheduled'` (background tick) or `'Manual'` (`SYSTEM CONTENT ADDRESSED GC RUN`) |
| `round` | monotone round counter |
| `outcome` | `'Success'`, `'NotALeader'`, or `'Error'` |
| `candidates_marked` | refs retired/marked this round |
| `objects_deleted` | objects physically removed |
| `objects_absent` | objects expected but already missing (concurrent delete or bug) |
| `objects_replaced` | 412-spared objects (live incarnation token displaced the condemned one) |
| `objects_spared` | objects not deleted for other reasons (e.g. protected by live build) |
| `manifests_deleted` | owner-removed manifest bodies deleted this round (B11) |
| `entries_condemned` | retired entries newly condemned this round (ack-floor stage 1) |
| `entries_graduated` | entries newly floor-passed, republished `delete_pending` (stage 2; deleted NEXT round) |
| `entries_redeleted` | pending exact-token blob deletes executed this round (stage 3) |
| `fence_outs` | expired mounts fenced out by this round's heartbeat floor |
| `min_ack` | the ack floor latched at round start (`UINT64_MAX` = no counted heartbeats) |
| `anomalies` | fold clamps surfaced (and survived) this round |
| `duration_ms` | wall time for the round |
| `error` | exception message when `outcome = 'Error'`; empty otherwise |
| `ProfileEvents` | `Map(String, UInt64)` delta of GC thread `Cas*` + `S3*` counters over the round |

A `Start` row with no matching `Finish` marks a round that hung or crashed mid-flight.
`NotALeader` rows on non-leader servers in a shared-pool cluster are expected.
**Hard assertion**: no `'Error'` finish rows in positive scenarios.

Useful diagnostic query — GC round timeline:
```sql
SELECT event_time, event_type, round, outcome, candidates_marked, objects_deleted,
       objects_replaced, objects_spared, duration_ms, error
FROM system.content_addressed_garbage_collection_log
WHERE disk_name = 'ca'
ORDER BY event_time;
```

### 2.2 `system.content_addressed_log` {#event-log}

Per-event CAS audit log (B170). **On by default** since `cbe0ffb7608` — `programs/server/config.xml`
ships a `<content_addressed_log>` section (the CAS disk feature is experimental, so the audit log is the
primary forensic instrument; it costs nothing when no CAS disk is configured). Remove the config section
to disable; when disabled, the injected `CasEventSink *` is `nullptr` — a single branch, no row construction.
Key schema columns:

| column | description |
|--------|-------------|
| `event_type` | taxonomy value (see below) |
| `disk_name` | the CA disk the event occurred on |
| `namespace` | `roots/<ns>` (server/table) |
| `ref_name` | part name or ref identifier |
| `object_kind` | `LowCardinality(String)`: `none`, `blob`, `manifest`, `root`, `snap` |
| `object_hash` | lowercase hex content hash |
| `token` | incarnation token (S3 ETag or equivalent) |
| `round` / `gen` | GC round and snap generation numbers |
| `at_version` | manifest shard version driving the journal record |
| `outcome` | `ok`, `adopt`, `resurrect`, `deleted`, `replaced`, `spared`, `absent`, etc. |
| `reason` | human-readable *why* of the decision (mandatory, never decorative) |
| `thread_id` / `query_id` | thread and query context of the event, when applicable |
| `detail` | `Map(String,String)` structured facts for full reconstruction |

**Event types the soak harness asserts must NOT appear in positive scenarios**:
`read_missing`, `dangling_access`, `corrupt_dangle`, `corrupt_decode`,
`snap_journal_incoherent`, `exception`.

**Writer/mount insight events (2026-07-02)**: `retired_view_advance` — one row per view ADVANCE (never per
beat): `round` = the installed round, `detail.from_round`, `detail.retired_entries` = the size of
the retired list this writer just loaded; answers "when did this writer learn about round N".
`mount_remount` — a self-remount attempt (`outcome` = `ok`/`failed`, `detail.writer_epoch`).
`blob_copy_forward` — a condemned incarnation displaced by verified copy-forward at the promote
gate (`detail.displaced_token`); join by `object_hash` against `blob_retire`/`blob_delete` to trace
the full incarnation history.

Useful for full object lifetime reconstruction:
```sql
SELECT event_time, event_type, object_hash, token, round, gen, outcome, reason, detail
FROM system.content_addressed_log
WHERE disk_name = 'ca' AND object_hash = '<hash>'
ORDER BY event_time;
```

And for ref lifecycle:
```sql
SELECT event_time, event_type, ref_name, round, gen, reason, detail
FROM system.content_addressed_log
WHERE disk_name = 'ca' AND ref_name = '<part_name>'
ORDER BY event_time;
```

### 2.3 GC unit-test suites (ack-floor round) {#gc-unit-suites}

The one-pass ack-floor round (`04 §gc-round`) is exercised by these gtest suites (`src/Disks/tests/`,
`InMemoryBackend` with injected clocks/hooks):

| Suite | File | What it covers |
|---|---|---|
| `CasGcAckFloor` | `gtest_cas_gc_ack_floor.cpp` | The protocol: condemn → `delete_pending` → exact-token delete pipeline; `NoOpRoundDoesNotMutateRefShards`; stale-ack-holds-the-floor; pre-ack publish spares; expired-mount fenced-out-and-excluded; recreated-blob delete is `TokenMismatch`-ok. Ported from `gtest_cas_gc_fence_recheck.cpp` (the fence test dropped, recheck/completion tests ported). |
| `CasHeartbeatFloor` | `gtest_cas_mount.cpp` | `computeHeartbeatFloor` classification (live / terminated / expired) and the token-guarded fence-out (sleeper renewal permanently fails; a concurrent renewal wins ⇒ reclassified live). |
| `CasStoreBeat` | `gtest_cas_store.cpp` | The merged beat: ack advances only after the view load; the drain blocks the ack while a mutation is in flight; a `gc/state` read failure leaves the ack unchanged. |
| `CasThreeCursorMerge` | `gtest_cas_blob_indegree.cpp` | The three-cursor merge rules — spare / graduate→pending / condemn, including the `condemn_round = min_ack − 1` vs `= min_ack` boundary. |
| `CasHeartbeat` | `gtest_cas_mount.cpp` / codecs | The merged keeper body (lease + `min_active` + `observed_gc_round`); `CAWM` watermark object gone. |
| `CasGcReplay` | `gtest_cas_gc_*` | Crash-replay idempotence (renamed from the old resume suite): crash after artifacts but before the CAS ⇒ re-run under a fresh attempt succeeds; already-executed deletes land on `NotFound`. |

**The reclaim-loop pattern (load-bearing for every reclaim assertion).** Because a blob is no longer
deleted in the round that folds its removal, a test that asserts deletion must **run enough rounds
AND advance the ack between them**: the pipeline is condemn at round K → `delete_pending` at the
first pass whose floor `min_ack > K` → physical delete the pass after that (with all acks current:
condemn K → pending K+1 → deleted K+2). Helpers `runRoundsUntilAbsent` / `blobAbsent` /
`currentRetiredSet` drive this; each round calls `store->renewWatermarkOnce()` (runs the beat to keep
the mount watermark fresh — graduation itself paces on GC rounds via `new_round`, not on the removed
`observed_gc_round` ack), and fixpoint loops continue while the current retired list still holds an
in-flight entry. A test failing because a blob is deleted *later* than
the old protocol is expected drift; a test failing because a **referenced** blob is deleted, or an
in-degree double-count appears, is a real bug.

### 2.4 Live health-verification playbook {#health-checklist}

When a CAS-backed server is running (a soak, staging, or production) and the question is "is CAS
actually behaving correctly right now", this is the order of checks and what each one tells you —
distilled from a full pass across every relevant `system.*` table during a 5h chaos soak on
`cas-gc-rebuild`. Each step names the table, the query shape, and the gotcha that made an earlier
pass over-claim or under-claim something.

**1. Correctness invariants first, not performance.** Query `system.content_addressed_log`
(`#event-log` above) for the event types the soak harness asserts must never appear:
`read_missing`, `dangling_access`, `corrupt_dangle`, `corrupt_decode`, `snap_journal_incoherent`,
`exception`. Also scan the free-text `reason` column broadly for `anomaly`/`clamp`/`error`/
`corrupt`/`unexpected`/`fail` — not just inside the known-bad event types — since a genuinely new
failure mode may not yet be in that list. Zero hits on both checks is the baseline "nothing is
structurally wrong" signal; do this before looking at any performance number.

**2. `system.errors` / `system.error_log` — read past the raw `value`.** Both tables carry
`last_error_message` and `last_error_trace` columns; a bare error-code count with no further
context tells you almost nothing. `system.text_log` is **not** authoritative on its own for how
often an error fired — `LogSeriesLimiter` suppresses repeated identical log lines after the first
few occurrences (the underlying `system.errors` counter still increments on every occurrence). A
huge `system.errors` count alongside almost no matching `text_log` rows is the expected shape of a
rate-limited, high-frequency error, not evidence the error is rare. Always cross-check
`system.query_log`'s `exception_code != 0` count: if that is zero (or near-zero) despite a large
`system.errors` total, the underlying failures are transient and being fully retried/absorbed
before reaching the client.

**3. Known-benign error codes worth recognizing on sight** (so they don't cost a fresh
investigation every time): `S3_ERROR` / `"...PreconditionFailed..."` is CAS's own conditional-PUT
collision-detection mechanism (`finalizeConditionalWriteInstrumented`,
`CasObjectStorageBackend.cpp`) — every legitimate content-addressing/dedup collision throws (and
thus counts) an `S3Exception` before being caught and reclassified as `PutOutcome::PreconditionFailed`,
a normal outcome, not a real error (see `BACKLOG.md`'s "S3_ERROR inflates" entry for the
observability-fix direction). `CANNOT_PARSE_INPUT_ASSERTION_FAILED` is ClickHouse's own
`ValuesBlockInputFormat` fast-literal/slow-expression parser fallback (`ValuesBlockInputFormat.cpp`)
firing whenever an `INSERT ... VALUES` row contains a function-call expression (e.g.
`toDateTime64(...)`) instead of a bare literal — entirely unrelated to CAS.

**4. `system.trace_log` — three `trace_type`s answer three different questions; don't conflate
them.**
- `CPU`: where the process spends actual processor cycles. Group by the exact `trace` array (not
  just the leaf frame), condense to the top few frames per stack for readability, and remember the
  single most-frequent *identical* stack may still be a small percentage of total CPU samples — it
  is the modal repeated call path, not necessarily the dominant cost center.
- `Real`: where threads spend wall-clock time, *including* blocking waits. Most of the top will be
  healthy idle capacity (the query executor's async-task reactor loop, `BackgroundSchedulePool`
  idle workers, HTTP keep-alive polls, ZooKeeper/S3 RPC round-trips) — the useful signal is a thread
  blocked on a `pthread_cond_wait` **inside your own code's** internal queue/lock (not a generic I/O
  wait), scaling with load.
- `Memory`: samples allocation *events*, not resident memory. The leaf frame is always the
  profiler's own capture routine (`StackTrace::StackTrace()`) — skip the first one or two frames
  before reading the real allocation site. A high sample count here usually just means "this buffer
  type gets constructed often," proportional to write/read volume, not a leak signal by itself.

**5. `system.parts` — a single active-vs-outdated ratio is not a reliable signal; check the
trend.** An extreme-looking ratio (this session saw 690:1) can be entirely normal pipeline depth
within `old_parts_lifetime`'s grace window at the current creation rate. Before calling it a
backlog: (a) re-check the same ratio a few minutes later — is the outdated count growing or
shrinking; (b) check the oldest outdated part's age against the table's `old_parts_lifetime`
setting; (c) cross-check `system.part_log`'s `RemovePart` vs `NewPart`+`MergeParts`+`MutatePart`
event *rates* over the whole run, not a point-in-time count. In this session an earlier claim that
a large outdated-parts ratio was evidence of a cleanup backlog turned out to be wrong once the trend
was checked (the count was falling, and the oldest outdated part was barely past its grace window)
— the claim was explicitly retracted in `BACKLOG.md` rather than left standing.

**6. `system.part_log` — event-type breakdown plus its own `ProfileEvents` column.** Group by
`event_type` (`NewPart`/`MergeParts`/`MutatePart`/`DownloadPart`/`RemovePart`/...) and sum
`ProfileEvents['S3PutObject']` etc. per group to attribute S3/Cas cost to a part-lifecycle stage.
Gotcha: `NewPart`'s `ProfileEvents` are the *same* underlying events as the originating `INSERT`
query's `system.query_log` row (confirmed via `sumMap(ProfileEvents)` cross-checks on both tables)
— summing them together double-counts. `MergeParts`/`MutatePart`/`DownloadPart` genuinely are
additive on top of `query_log`, since the originating `Optimize`/`Alter` query_log rows show *zero*
S3 `ProfileEvents` (their background execution isn't captured by query_log at all, only by
part_log).

**7. `system.query_log` — group `ProfileEvents` by `query_kind` for the synchronous/foreground
view of cost,** and always check `countIf(exception_code != 0)` alongside any scary
`system.errors` total (see step 2).

**8. `system.blob_storage_log` — the write-path audit, with two gotchas.** (a) It does **not** log
`Read` events by default — `enable_blob_storage_log_for_read_operations` defaults to `false`
(requires `enable_blob_storage_log` too) — an all-zero `Read` row count is expected, not a gap to
chase. (b) `data_size` on `Delete`-type rows is a `UInt64` sentinel
(`18446744073709551615` = `UINT64_MAX`), not a real size — filter it out
(`avgIf(data_size, data_size != 18446744073709551615)`) before computing size statistics, or the
aggregate is nonsense. Aggregate by a derived path-prefix category (`blobs` vs
`cas/manifests/<namespace>` vs `cas/refs/<namespace>` vs `gc/server-roots` vs `gc/other`) to get a
per-object-kind cost/error/size/latency breakdown, and to spot cross-replica GC-leader asymmetry: a
replica whose log shows `Delete`-only rows for the *other* replica's namespace, never `Upload`,
confirms it currently holds GC leadership and is reclaiming pool-wide garbage.

**9. `system.content_addressed_garbage_collection_log` — round duration by `outcome`, normalized
by actual work done.** `outcome = 'Success'` vs `'NotALeader'` tells you which replica currently
holds GC leadership — this can change mid-run (e.g. during a chaos-stage kill/restart), and that is
expected, not a bug. For duration, normalize by
`objects_deleted + candidates_marked + entries_graduated + entries_redeleted`: a big round taking a
long time is fine if the per-item cost stays roughly constant across rounds; a round taking a long
time while doing **zero** work is the real anomaly worth tracing. In this session, two such
zero-work spikes both turned out to have a clean explanation once traced — an early cold-cache
warmup at rounds 1–4 of the run, and a later spike landing almost exactly at the chaos-stage
transition (correlating with a GC leadership handoff) — neither was dismissed without checking, but
neither needed a new backlog entry either.

**10. `system.events` — the ground-truth cumulative totals, and a second attribution axis.**
Cross-reference the generic S3-level counters (`S3PutObject`/`S3GetObject`/`S3HeadObject`/
`S3ListObjects`/`DiskS3DeleteObjects`) against CAS's own semantic per-object-kind counters
(`Cas{Blob,Gc,Manifest,Meta,Root,Other}{Put,Get,Head,Delete,List}`, incremented at the S3-call site
itself, independent of `ThreadGroup` attribution). In this session's soak, the Cas-level counters
covered `PUT`/`GET`/`LIST` far better than the query_log/part_log/GC-log axis (77–96% vs 4–58%)
because they don't depend on the calling thread being attributed back to a query or part-log event
— while `HEAD` stayed persistently under-attributed on both axes (~56%), a genuine open question
left for a follow-up rather than force-explained. See `BACKLOG.md`'s "S3 cost/capacity attribution"
entry for the full worked comparison and `07-s3-budget.md` for the design-level S3 budget model.

**11. Host/container-level context for elevated S3 error rates.** When `system.errors`/
`system.events` show elevated S3 transport errors (`Broken pipe`, `Timeout`), check the *object
storage backend's own* container/host CPU and memory (e.g. `docker stats`), not just the
ClickHouse server's. A saturated local test-stand backend (a single `rustfs` container pinned above
200% CPU was the case in this session) fully explains transient transport errors that are then
silently absorbed by ClickHouse's own S3-client retry logic — confirmed via `system.query_log`
showing zero real query failures. This is a test-stand characteristic, not a product bug, but it
must be *confirmed* (backend CPU + zero query failures), not assumed.

**General methodology notes.** Build a small "budget table" (cost broken down by source) whenever
a single cumulative counter looks surprising — the counter tells you *that* something happened, not
*where* it came from; cross-referencing two or three tables against the same-moment `system.events`
snapshot tells you the shape of the cost. Never assert a finding from a single point-in-time
snapshot ratio — check the trend over a few minutes before calling something a backlog item or a
regression. When a later, more careful check contradicts an earlier claim, retract it explicitly in
whatever document holds it, rather than let a wrong "corroborating evidence" note stand next to the
real one. And distinguish "confirmed root cause" from "plausible, not fully proven" in the writeup
itself — several findings in this session's `BACKLOG.md` entries carry that distinction
deliberately.

## 3. Scenario suite (S01–S35) {#scenario-suite}

Located at `utils/ca-soak/scenarios/`. Each scenario is an independent, focused run against a
fresh pool prefix. The common run contract, hard assertions, and recommended observations are in
`utils/ca-soak/scenarios/README.md`.

### 3.1 Common hard assertions {#common-assertions}

Every positive scenario must satisfy:

- **SQL correctness**: all replicas return the same aggregates as the scenario oracle.
- **Storage correctness**: `clickhouse-disks ca-fsck --detail` reports `dangling = 0`.
- **GC safety**: `ca-gc-dryrun` delete candidates are a subset of the `ca-fsck` unreachable set at
  quiescence.
- **Event audit**: `system.content_addressed_log` contains no `read_missing`, `dangling_access`,
  `corrupt_dangle`, `corrupt_decode`, `snap_journal_incoherent`, or `exception` rows.
- **GC rounds**: `system.content_addressed_garbage_collection_log` has no `Failed` finish rows.
  `NotALeader` rows are expected on non-leader servers.
- **No unbounded leftovers**: after forced GC, `unreachable = 0` (unless the scenario deliberately
  abandons writes, in which case the residual must be classified and proved bounded).
- **No excessive resource growth**: `MemoryResident`, scratch-dir bytes, and pool bytes return to
  baseline or stay within the scenario budget.

### 3.2 Scenario table {#scenario-table}

| ID | Priority | What it stresses | Key risk targeted | D2 status |
|----|----------|-----------------|-------------------|-----------|
| S01 | P0 | Huge single blob upload | Memory materialization in `Build::putBlob` (now **FIXED** — streaming from staged temp file; confirmed <2x peak) | INCONCLUSIVE at dev scale; needs `--scale ci/full` |
| S02 | P0 | Huge duplicate blob | Dedup skips body upload for existing large blobs | PASS |
| S03 | P0 | Million-live-object idle GC | GC memory and LIST cost at scale; `CasBlobList = 0` for journal-driven rounds | INCONCLUSIVE (dev scale) |
| S04 | P0 | Million-object orphan drain | GC reclaim throughput and memory at scale | FAIL (GC Error rows — pre-existing, now resolved by attempt-scoped generation) |
| S05 | P0 | 10 000 sparse tables | GC does O(changed shards), not O(all tables), per round | FAIL (same GC Error rows) |
| S06 | P0 | 10 000-column wide part | Manifest limits, memory, S3 op count for very wide parts | INCONCLUSIVE (timestamp parse bug in harness, now fixed) |
| S07 | P0 | Manifest cap fail-closed | `LIMIT_EXCEEDED` before any ref is published; no orphans | INCONCLUSIVE (dev scale cannot trip the cap limit) |
| S08 | P0 | Thousands of parts created quickly | CAS contention (`CasRootCasConflict`); root-shard size | INCONCLUSIVE |
| S09 | P0 | Mutation carry-forward | Only changed columns are re-uploaded; identity updates produce zero large blob growth | INCONCLUSIVE |
| S10 | P1 | Patch parts and lightweight deletes | No dangling refs during patch-part create/merge/remove | FAIL (harness scale/timing issue, not a product bug) |
| S11 | P0 | Heavy `ALTER TABLE ... DELETE` | Mutation latency, queue depth, GC reclaim bounded after deletions | FAIL (GC Error rows — pre-existing) |
| S12 | P1 | Ten replicas, shared pool, parallel inserts | Leader election, dedup across replicas, no data-size amplification | NOT RUN (`docker-compose-10replicas.yml` now exists (ch1..ch10); gap is `soak/cluster.py`'s `Cluster` class, hardcoded to 2 nodes) |
| S13 | P0 | Process loss during write and GC | Abandoned precommits are safe; stale GC leaders cannot over-delete | FAIL (chaos kill/restart connection refused — harness issue) |
| S14 | P0 | Restart with many refs | Startup time scales with table metadata, not blob count | FAIL (GC Error rows — pre-existing) |
| S15 | P1 | GC target shard comparison (`gc_shards` 1/2/8) | Correctness is identical across shard counts; per-round memory decreases | not shown in D2 |
| S16 | P1 | Hot content cycle with GC | Resurrection uses a fresh re-upload; no condemned-token adoption | FAIL (GC Error rows pre-existing; single forensic `dangling=1` was transient FP, not a safety bug) |
| S17 | P1 | Detached, attach, and drop detached | Detached refs stay rooted; dropped detached content reclaimed by GC | PASS |
| S18 | P1 | Freeze and unfreeze shadows | Shadow namespaces keep blobs alive; unfreeze releases shadow refs | INCONCLUSIVE (`SYSTEM UNFREEZE` disabled by default config) |
| S19 | P1 | Clone and partition movement | Clone republishes refs, not blobs; gated paths fail closed | FAIL (missing `SYNC REPLICA` before agreement check — harness bug) |
| S20 | P1 | Replicated fetch and relink | Followers reuse blobs; no byte amplification per replica | FAIL (counter not scoped per node — harness issue) |
| S21 | P1 | Read-heavy many-ref workload | Read-path caching; column-subset queries fetch only required blobs | FAIL (`FINAL` on plain `ReplicatedMergeTree` → `ILLEGAL_FINAL` — harness bug) |
| S22 | P1 | Object-store throttling and retry budget | Retryable errors visible; successful statements remain correct | NOT RUN (needs fault-injecting S3 proxy) |
| S23 | P2 | Idle shared pool baseline | Background GC op count is minimal; memory flat; non-leaders quiet | FAIL (metric bug: `s3_ops` summed `*Microseconds` not op counts; real ops are tiny) |
| S24 | P2 | Small dedup-cache capacity | Cache miss changes cost, not correctness; cache memory bounded | RUNNABLE (`docker-compose-small_dedup_cache.yml` wired as the `smalldedupcache` compose variant; `needs_infra` unset on the `S24` class) |
| S25 | P2 | Non-`Atomic` database paths | Path parsing correct outside `store/<uuid>` layout | FAIL (test setup bug — DB created on one node only) |
| S26 | P2 | Table-level verbatim file churn | Verbatim files not accidentally content-addressed; regular GC does not need to scan them | FAIL (missing `SYNC REPLICA` — harness bug) |
| S27 | P2 | Backend list pagination ambiguity | Ambiguous keys treated as changed and re-read; correctness preserved | NOT RUN (needs instrumented S3 proxy) |
| S28 | P0 | Concurrent wide/large insert scratch pressure | Scratch approaches sum of all active staged part payloads under concurrent inserts | PASS |
| S29 | P0 | Large non-direct-blob file memory spike | `CaInlineWriteBuffer` path; files outside `.bin`/mark/`primary.idx` buffer until `INLINE_CAP` | INCONCLUSIVE |
| S30 | P0 | Repeated create/drop namespace churn | Monotone namespace registry; GC fanout must not grow without bound after D1 | PASS (D1 regression guard; D1 fixes monotone fanout) |
| S31 | P1 | `ca-gc-dryrun` under `gc_shards > 1` | `previewDeletes` coverage across all target shards | FAIL (cluster boot failure — infra issue) |
| S32 | P1 | TTL expiry reclaim | TTL-expired rows disappear; content reclaimed by GC | PASS |
| S33 | P1 | Concurrent explicit GC leaders — reclaim-leak guard | Non-leader explicit rounds must not orphan owner-removal events permanently | PASS (attempt-scoped generations fix landed; S33 now a real regression guard) |
| S34 | P0 | Create/drop churn (D1 namespace-reclaim win) | Per-round GC fanout does not grow across create/drop iterations after D1 | PASS |
| S35 | P0 | Rapid same-name rotation (D1 corner case) | Shard incarnation handles same-namespace repeated create/drop safely | PASS |

**D2 summary (2026-07-02, post-D1 sweep, seed 20260702)**: 8 PASS, 8 INCONCLUSIVE, 14 FAIL, 3 NOT RUN,
1 RUNNABLE (S24, since wired to the `smalldedupcache` compose variant), S15 not shown in D2.
Every FAIL is a pre-existing harness/infra/scale issue — zero D1 regressions.
**2026-07-03 night re-triage**: the D2 FAILs were re-classified against the fixed stack — all of them
resolved, superseded, or card bugs (the one real-looking S13 'fail' was the card comparing replicas
before `SYSTEM SYNC REPLICA`; card fixed, re-run PASS 11/11). Current standing: 8 PASS, ZERO real
fails; remaining inconclusives are honest scale gates (rerun at ci/full) and infra gates
(S12/S22/S27). See `ROADMAP.md §area-testing` and `utils/ca-soak/scenarios/BACKLOG.md`.

### 3.3 Running a scenario {#running-scenarios}

```bash
# List available scenarios
cd utils/ca-soak && python3 -m scenarios.run --list

# Run one scenario (default 15 min measurement window, dev scale)
python3 -m scenarios.run --scenario S01 --seed 42 --duration 15m

# Larger scale (CI/full)
python3 -m scenarios.run --scenario S01 --seed 42 --scale ci/full
```

The run emits `report.md`, `report.json`, `metrics.sqlite`, raw system-table extracts,
pool-size samples, and container resource samples in `runs/<run_id>/`.
A scenario may be marked `inconclusive`, but never silently converted to `pass`.

## 4. Soak harness {#soak-harness}

Spec: `specs/2026-06-13-ca-soak-test-design.md`.
Location: `utils/ca-soak/` (Python + docker-compose; nothing ships in the server binary).

### 4.1 Topology {#topology}

Two `ReplicatedMergeTree` replicas (`ch1`, `ch2`) share one CA pool on `rustfs1`, coordinated via
`keeper1`. RustFS runs with `RUSTFS_SCANNER_ENABLED=false RUSTFS_HEAL_ENABLED=false` (stability
fix from B93). `clickhouse-disks ca-fsck` runs read-only against the same pool from a node
container at quiesced checkpoints.

`docker-compose.yml` is the default RustFS-backed topology. A second compose,
`docker-compose-awss3.yml`, targets a live AWS S3 bucket instead of RustFS for the release-gate
real-S3 GC validation; it uses named Docker volumes for state and reads bucket credentials from
the git-ignored `configs/aws.env`. The live-AWS variant was validated 2026-07-03.
The live-GCS variant (`docker-compose-gcs.yml`, `storage_conf_gcs_*.xml` with
`<http_client>gcs_hmac</http_client>`, HMAC pair in git-ignored `configs/gcs.env`) was validated
the same day: probe + replication + two-phase reclaim + DROP-to-zero + ca-fsck all green. NOTE: the
read-only ca-fsck disk lives in the standalone `configs/fsck_only_gcs.xml` passed only to
`clickhouse-disks -C` — a `ca_ro` disk in the SERVER config breaks table load on restart
(`UNKNOWN_DISK`; ROADMAP prod-gate row).

### 4.2 Workload and oracle {#workload-oracle}

Table `ca_stress` (forced Wide parts: `min_bytes_for_wide_part = 0`, `min_rows_for_wide_part = 0`)
with columns `(op_id, writer, bucket, k, ts, version, v, payload, row_fp)`, TTL on `ts`.
`payload = det_blob(seed, bucket, k % SHARED)` so identical content recurs across parts and
replicas (real CA dedup).

The **ledger** produces a fully seeded, reproducible stream of operations: `insert`, `update`,
`delete`, `optimize`, `truncate`, `drop_partition`. The **oracle** (`model.py`) is an authoritative
per-key `(bucket, k)` map; it applies each operation and knows the expected live row set at any
quiesced checkpoint. Aggregates compared: `count()`, `sum(row_fp)`, `uniqExact((bucket,k))`,
`sum(v)`, `min(op_id)`, `max(op_id)`. These are integer aggregates — exactly reproducible in Python
and SQL with no hash-serialization matching.

### 4.3 Quiescence protocol {#quiescence}

Before any checkpoint assertion:
1. Pause all workers.
2. `SYSTEM SYNC REPLICA` on both nodes; wait `system.replication_queue` empty on both.
3. Wait every `system.mutations` row `is_done`; no active `system.merges`.
4. `OPTIMIZE TABLE ca_stress FINAL` + `ALTER TABLE ca_stress MATERIALIZE TTL`.
5. Drive CA GC to **fixpoint** — poll until pool object set and `ca-fsck`'s `unreachable` field stop changing
   across successive GC rounds (bounded retries, fail loudly on timeout).

TTL boundary handling: checkpoint timing is arranged so no row sits within ±ε of its TTL boundary.
If the ambiguous band is non-empty, the checkpoint fails (a scheduling bug, not tolerated).

### 4.4 Checkpoint assertions {#checkpoint-assertions}

At every quiesced checkpoint:
- SQL oracle match on **both** replicas (`count`, `sum(row_fp)`, `uniqExact`, `sum(v)`, `min/max(op_id)`).
- `clickhouse-disks ca-fsck --detail`: `dangling = 0` (INV-NO-LOSS, hard fail).
- `clickhouse-disks ca-fsck`: `unreachable = 0` (GC drained to fixpoint).
- `clickhouse-disks ca-gc-dryrun`: `{preview} ⊆ {ca-fsck unreachable}` (GC safety direction).
- `system.content_addressed_log`: no error-class event types.
- `system.content_addressed_garbage_collection_log`: no `'Error'` finish rows.

### 4.5 Chaos {#chaos}

`chaos.py` applies seeded faults: `{t_offset, target ∈ {ch1,ch2,both,rustfs}, action ∈ {kill-9,
restart, pause/unpause}, duration}`. After every fault window → a recovery checkpoint (full
quiescence + all assertions). Faults are bounded so a quiescent checkpoint is always reachable
between them; a server that does not recover within the bound is a correctness failure.

### 4.6 Known soak limitations {#soak-limitations}

- **4h active-workload chaos soak** at `WORKERS=6` fills the disk in ~60–90 min. ROOT CAUSE
  identified 2026-07-03: [rustfs#3231](https://github.com/rustfs/rustfs/issues/3231) (open) —
  overwriting a >128 KiB object in an UN-VERSIONED bucket leaks the previous incarnation's data
  directory. Every `casPut` of a `cas/refs/<...>/<shard>` body leaks one uuid dir (observed: 1600+
  dirs on one busy shard key; `cas/refs` = 39 GB of a 45 GB pool vs 7.2 GB of real blob data), and
  the metacache walk over those dirs is what produces the `list_merged err Io(timeout)` /
  `walk_dir timeout` LIST storms (reproduce at just ~600k files). Re-enabling the RustFS scanner
  was tested for one run and showed ZERO reclaim effect (the leak is not versions; the old
  bring-up failure with the scanner was the unpinned `mc`, since pinned) — the scanner stays off.
  Mitigations: `WORKERS=2`; durable fix = lower ref-shard `casPut` rate (journal batching, B157
  family) and/or shard bodies under the 128 KiB inline threshold, or the upstream fix.
- **Large-pool ca-fsck timeout**: `ca-fsck` at ~150 GB pool times out (>180 s) because
  `ObjectStorageBackend::list` re-enumerates the whole `roots/` prefix per page (O(N²) in the
  manifest backlog). Fix: paginate at source; separate shard objects into a dedicated prefix.
  Tracked in the backlog below.
- **TTL-band oracle ambiguity**: a row sitting within ±10 s of its TTL boundary cannot be predicted
  exactly. A fault that delays TTL materialization can leave the band non-empty. Mitigation: disable
  TTL in the soak table for long chaos runs, or widen the ambiguous-band wait.

### 4.7 `lazy_load_tables` for CAS databases {#lazy-load-cas-databases}

Host CAS tables in a database created with `lazy_load_tables = 1`:

```sql
CREATE DATABASE ca_soak ENGINE = Atomic SETTINGS lazy_load_tables = 1;
```

Why: without it, if the object store is briefly unreachable while a CAS table's async startup runs
its ref-table recovery, the recovery seal `PUT` throws `NETWORK_ERROR`, and ClickHouse's
`AsyncLoader` records the table's `load table` job as `FAILED` **terminally**. Every later touch
(`SELECT`, `ATTACH`, even `DETACH`) then rethrows the cached error, and the only recovery is a full
server restart — a permanent outage from a transient blip. With `lazy_load_tables = 1` the table
attaches as a lightweight proxy and its real storage is built on first access, so a failed startup
surfaces as a per-query error and is retried on the next access instead of being cached `FAILED`.

Caveat: a lazily-loaded table does not start its background activity (replication queue, merges)
until its first access. This pairs with the CAS-side bounded recovery retry
(`cas_request_budget.recovery_retry_budget_ms`, default 120 s) which rides out short blips within a
single startup; `lazy_load_tables` is the backstop for a blip that outlasts that budget. The soak
harness creates its `ca_stress` table in a `ca_soak` lazy database for exactly this reason. See
`docs/superpowers/specs/2026-07-20-cas-table-load-stuck-asyncloader-design.md` and the BACKLOG entry
"a transient S3-backend NETWORK_ERROR during CAS table-startup recovery permanently strands the
table".

## 5. Standing findings and backlog {#backlog}

The authoritative finding log is `utils/ca-soak/scenarios/BACKLOG.md` (newest at bottom).

### 5.1 Confirmed fixed {#fixed}

- **S01 `Build::putBlob` memory materialization** (suspected-bug, HIGH): peak memory grew linearly
  with blob size (~6.5x for a 1 GiB blob) because `putBlob` materialized the staged `BlobSource`
  into a `String` before `putIfAbsentStream`. **FIXED** (`uploadFromSource` signature changed to
  stream from the staged temp file). Verified: 2 GiB INSERT peak 13 GiB → 4.33 GiB (~2x, matching
  local-disk baseline). Residual ~2x is generic `max_block_size` buffering, not CA overhead.
- **GC-CONCURRENT-LEADER-LEAK** (suspected-bug, HIGH): explicit `SYSTEM CONTENT ADDRESSED GARBAGE
  COLLECTION` on >1 replica concurrently permanently orphaned blobs (reclaim leak). Root cause: the
  fold-seal and `gc/state` CAS were not atomic, so a deposed leader could write a final-key seal
  before its lease CAS failed, orphaning owner-removal events from ever being folded. **FIXED** by
  attempt-scoped generations: every per-round artifact is keyed by `(gen, attempt = lease.seq)`;
  a deposed leader's artifacts land under its own unadopted attempt and are invisible to every
  decision path. TLA+ gate A green; S33 liveness verdict flipped to a real regression guard.
- **CA-S3 stateless lane — system logs on CA + opt-in cache** (2026-07-10): the June **B86** workaround
  that pinned every system log to the local `default` policy on the CA-S3 lane is REMOVED — the rewritten
  CA write path flushes fast (`trace_log` 7087 rows in 77 ms; no 180 s timeouts), so system logs now live
  on `content_addressed_s3` like user tables. An opt-in `content_addressed_s3_cache` disk (`<type>cache>`
  over the CA disk; integration test `test_cas_file_cache`) is validated warm ≈ local (0 S3 GETs) but is
  NOT the lane default — it adds cold-populate cost to single-pass reads/writes, and the stateless suite is
  mostly single-pass. Read-side companion: CA-S3 reads are cacheless (~3× local warm; see
  `09-read-protocol.md`). Lane point-fixes: `04286` (directory-safe HEAD), `05008` (ack-floor
  `entries_redeleted >= objects_deleted`), `05009` (default-ON log + `disk_name` filter), `01271`, `03829`
  (`max_memory_usage` 150M→170M for the ~9 MiB CA write buffer). Remaining reds are conclusively non-CAS
  (arch x86-vs-arm FP `01854_s2`/`02224_s2`/`03233_dynamic`; no-MySQL/IPv6 infra `02479`/`01880`/`02784`;
  `04033_tpc_ds_*` on the `web` disk).

### 5.2 Open backlog items {#open-backlog}

- **GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS** (HIGH, scalability): `listRootShardTokens` calls
  `backend.list` with `max_keys = 0`, which re-enumerates the entire `roots/` prefix (including
  all `_manifests`) on every page — O(N²/1000) S3 LIST round-trips. Fix: real paginated list at
  the backend, or a dedicated shard prefix (IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST: relocate all
  shard objects into one flat prefix so GC discovery is a single LIST).
- **S31 `ca-gc-dryrun` under `gc_shards > 1`**: `previewDeletes` previews `zeroInDegree` only
  for target shard 0, so the dry-run oracle can be blind to candidates in other shards.
- **S12 / 10-replica test**: `docker-compose-10replicas.yml` (`ch1`..`ch10`) exists and is wired
  as the `tenreplicas` compose variant; the remaining gap is `soak/cluster.py`'s `Cluster` class,
  which is hardcoded to 2 nodes and has no mechanism to address `ch3`..`ch10` (see `BACKLOG.md`
  entry `NEEDS-INFRA-S12`).
- **S22 / throttling**: requires a fault-injecting S3 proxy (not in current compose).
- **S27 / list pagination ambiguity**: requires an instrumented object-store proxy.
- **S23 idle RSS +82 MiB**: above the 64 MiB budget; confirm it does not grow unbounded in the 4h
  soak.
- **GC run-file streaming** (scalability): the O(buffer) reader debt is **DONE** (T2/T0, 2026-07-02) —
  `RunFileReader` streams over true ranged reads in `CasObjectStorageBackend::get` + the `getStream`
  seam; the whole-run `full` member is gone. The remaining byte-volume work (delta-runs + compaction,
  **T1**) stays DESIRABLE — see [`BACKLOG.md`](BACKLOG.md) §2.
- **S10, S19, S20, S21 harness bugs**: replica-agreement race (missing `SYNC REPLICA`), `FINAL` on
  wrong engine type, per-node counter scoping — to be fixed in the harness (not product bugs).
- **Ack-floor round soak validation** (TODO): the one-pass ack-floor round (`04 §gc-round`) is
  implemented and unit/TLA+-covered but not yet soak-validated. Needed: hard-KILL a writer
  mid-commit-burst and verify the next rounds spare-then-recondemn correctly (no dangle in `ca-fsck`);
  a paused (SIGSTOP) writer holds the floor, then resumes, acks, and the floor advances; a scenario
  asserting per-round request counts stay O(delta)+O(servers) — a regression guard against
  reintroducing a universe sweep of GET/PUTs. Deletion latency is now condemn → pending (first
  floor-pass) → delete (next pass), so soak fixpoint loops must advance acks between rounds.
