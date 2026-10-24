---
description: 'Design for making a CAS GC round cost O(new work) and last minutes regardless of backlog: parallel graduation and redelete (stage A), per-life discovery and cleanup (stage B), a round deadline instead of count budgets (stage C), with the multi-node GC direction as a set of constraints.'
sidebar_label: 'CAS GC rounds in minutes'
sidebar_position: 100
slug: /superpowers/specs/cas-gc-rounds-in-minutes-design
title: 'CAS GC: rounds in minutes, catch-up at S3 speed'
doc_type: 'reference'
---

# CAS GC: rounds in minutes, catch-up at S3 speed {#cas-gc-rounds-in-minutes}

- Date: 2026-09-25
- Status: design approved in conversation; spec for review before planning
- Trigger: Altinity/ClickHouse issue #2429 (otel.demo, 2026-09-24): GC rounds grew from 98 s to 2800 s over eight days, deletes pinned at 5000 per round, ~520k retired-but-undeleted blobs, 4.6M ref-log keys listed per round
- Builds on: PR #2351 (`cas/gc-parallel-delete-blobs`) and its approved rework spec `2026-09-14-cas-gc-round-pool-and-parallel-redelete-design.md` (revision 11)
- Companion measurement: `docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md` (S3 budget of the stand, findings F1-F12 referenced below)
- Backlog items absorbed: `[gc-round-budgets-are-not-backpressure]`, `{#gc-backlog-runaway}` ask 3, `[covered-log-cleanup-aborts-on-catalog-etag]`, CAS-079, `[gc-reduce-confirm-marker-read-ahead]`, `[gc-condemn-head-read-ahead-pinned-window]`, `[gc-pending-deletes-fan-out]`, `[gc-deferred-round-pays-full-list]`, `[gc-namespace-janitor-one-page-per-round-cannot-keep-up]` (steps 1 and 2 of its revised order), CAS-101

## 1. Problem {#problem}

A GC round is linear in what has accumulated, not in what is new. Measured on otel.demo (2 replicas, CAS as the default disk, 94 live namespaces, ~400-475k ref-log keys and ~320k blob retirements per day):

| phase | 09-14 | 09-24 | cost nature | what grows |
|---|---|---|---|---|
| `defer_decision` | 3 s | 1424 s | one LIST of `cas/ns/stream/`, 4.6M keys | every `_log` key in the pool |
| `fold_ref_intake` | 35 s | 903 s | one GET per new log, read-ahead 16 | new logs per round (grows only because rounds are longer) |
| graduation inside `fold_reduce` | seconds | hours | one synchronous GET of `.meta` per condemned entry | condemned backlog; after a restart, all of it |
| `pending_deletes` | 48 ms per entry | same | HEAD + conditional DELETE, serial | delete_pending cohort |
| `ref_object_cleanup` | seconds | seconds | batch delete by 1000 | cheap, but capped at 5000 keys per round |

Three fixed per-round caps (`gc_round_graduation_budget`, `gc_round_redelete_budget`, `gc_round_ref_cleanup_budget`, all 5000) are the only bound on round length. Once arrivals per day exceed `5000 × rounds per day`, keys accumulate, the LIST and the fold get slower, rounds per day fall, and the per-day budget falls with them. The loop does not recover by itself. The caps exist because two paths are serial; they are not backpressure (`[gc-round-budgets-are-not-backpressure]`).

The user-visible cost of the caps is also mis-sized: 5000 write-once keys are 5 batch-delete requests, while the LIST they leave behind costs ~4600 requests per round, each billed as a LIST.

## 2. Goals and non-goals {#goals-and-non-goals}

Goals, in the order the stages deliver them:

1. A round's cost is O(new logs + new condemned + this round's slice of catch-up), never O(keys in the pool) or O(retired entries).
2. A round lasts minutes at any backlog: mandatory work is O(new) and parallel; catch-up work is cut by a wall-clock deadline and carried durably.
3. Catch-up after a long outage runs at the speed of S3 parallelism, spread over back-to-back short rounds, while ordinary rounds keep folding new garbage.
4. A leader restart does not turn the next round into hours.

Non-goals:

- Any change to the object layout, key shapes, object kinds, or the wire formats of `gc/state`, the fold seal, ref logs, checkpoints or snapshots.
- Any change to the LIST trust model (settled 2026-08-03; LIST stays a hint, proof stays the exact GET).
- Any change to exact-token blob deletes, the DELETE-SITE INVARIANT, or the HEAD-before-PUT write protocol.
- Multi-node GC. Section 7 records the direction only as constraints on this work.
- Cutting `fold_ref_intake` by time. An unproven namespace frontier suppresses all destruction for the round (`CasGc.cpp` frontier proof), so a partial intake is never productive.

## 3. Stage A: parallelism, no change of decisions {#stage-a}

Base: PR #2351 brought to the shape of its approved rework spec (round-scoped `GcRound` with `io_pool` and `meta_writer`, one disk setting `cas_gc_concurrency` default 16, `pending_deletes` fan-out on by default, every worker failure logged and counted as `redelete_failed` / `CASGCRetiredRedeleteFailed`). Stage A adds four changes on top, in this order. Each one moves only the moment of a read or memoizes an immutable body, never a decision; the rule that `GcReadAhead` is worth trusting for ("only the moment of the fetch moves") stays the design rule.

### A0. Persist the marker confirmation on carry {#a0-persist-marker-confirmed}

`settleEntry` (`CasBlobInDegree.cpp`) carries a condemned entry unchanged when `confirm_condemned_marker` succeeded but the graduation budget was exhausted, so the run keeps `marker_confirmed = false` for every carried row and only the in-process memo remembers the confirmation. After a restart or a leadership change every carried row pays the synchronous GET again (audit F3: 200k GETs, four hours, round 1379 on otel.demo). Fix: when the gate confirmed the marker, carry a copy with `marker_confirmed = true`; the run is rewritten anyway. The gate's semantics do not change; only rows condemned since the last successful run still need the re-check. Under stage C the count budget disappears, but the deadline carries rows the same way, so A0 stays necessary.

### A1. Graduation gate through the read-ahead {#a1-graduation-gate}

`confirm_condemned_marker` (`Gc::fold`) issues one synchronous `loadMeta` GET per carried condemned entry without an in-process confirmation. Candidates are the condemned rows of the adopted run, which the merge streams in hash order, so the hint site is a lookahead on the run cursor: while merging shard `s`, keep the next `window()` condemned rows' meta keys hinted (`reads.hintRead(layout.metaKey(ref))`), and let `confirm_condemned_marker` take the hinted read (`reads.takeRead`) instead of `op.readUnder`. The in-process memo `condemnMarkerConfirmedInProcess` stays as a pure shortcut: consulted first, never required. A missing, unreadable or non-`Condemned` meta is handled exactly as today: `CASGCCondemnMarkerUnconfirmedCarry`, retry the marker write, refuse graduation for this round.

Why the memo must stay optional: a leadership change or restart empties it, and in a multi-node round (section 7) the executor that wrote the marker is not the one that graduates. The read-ahead path is the normal path.

### A2. Free the pinned HEAD window {#a2-pinned-head-window}

`[gc-condemn-head-read-ahead-pinned-window]` as written: before `topUpHeadHints`, and on any `takeHead` miss with `pending() == window()`, discard every pending hint whose key sorts before the key being taken (counted as `CASGCReadAheadWasted`), then top up.

### A3. `pending_deletes` fan-out {#a3-pending-deletes-fan-out}

As in the rework spec 3.3: `redeleteBlobs` over the round's `io_pool`, submission window `2 × concurrency`, outcomes applied on the round thread in index order within a shard, the four safety comments re-attached.

### A4. Manifest bodies read once per round {#a4-manifest-body-memo}

`foldManifestEdges` reads a manifest body once per emitted edge (`CASRefManifestBodyFoldGets == CASRefEmittedEdges`, audit F4). Bodies are immutable at write-once keys, and within one round the same manifest is read for its `+1` and later its `-1` (publish, repoint, drop of one part fall into one round whenever the round is longer than the part's life). Memoize the decoded edge list per `ManifestId` for the duration of the round, bounded by a total edge count; past the bound, evict and re-read. No decision moves: the fold applies the same edges in the same order. Expected on otel.demo: manifest GETs in `fold_ref_intake` halve; together with the `delete_tmp` repoint elision (a writer change outside this spec, audit F2) they quarter.

### A5. Acceptance {#a5-acceptance}

On a stand with ≥100k condemned entries and a fresh leader (empty memo):

- After A0, a restart with N carried condemned rows issues at most (rows condemned in the last round) meta GETs, not N.
- After A4, `CASRefManifestBodyFoldGets` per round is at most the number of distinct manifests folded, not the number of edges.

- `fold_reduce` and `pending_deletes` wall time scale with `cas_gc_concurrency`; `CASGCReadAheadMiss` on a mass-removal round is within one window of zero.
- `objects_deleted`, `objects_spared`, `objects_replaced`, `entries_graduated` are identical to a `cas_gc_concurrency = 1` run on the same input.
- Expected on otel.demo figures (70 ms per request): 200k graduations ≈ 15 min instead of 4 h; 200k redeletes ≈ 10 min instead of 2.7 h.

## 4. Stage B: discovery and cleanup cost O(new) {#stage-b}

### B1. Discovery without the global LIST {#b1-discovery}

`listRefPrefix` today is one LIST of `<prefix>/cas/ns/stream/`, O(all `_log` keys). Its consumers (see `docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit/gc-cost-map.md`, question 1) are the DEFER signal (`changedRows`), `fold_ref_group` (the per-life `ref_tables`), `cleanupRefObjects` (below-floor keys) and the `dead_life_debris` counter. `fold_ref_intake` does not use it: it walks by exact key from the cursor, forward only.

Replacement, per round:

1. The catalog cut (already one GET) gives the `Live` and `Removing` lives.
2. For every such life, one exact GET of `refLogKey(life, last_folded + 1)` on the round's `io_pool`: 404 means "nothing new", 200 means "this life folds". This is the same frontier probe the intake already makes (`frontier_proven`, `absent_probes`), moved into `defer_decision`; the intake keeps its own proof unchanged. LIST trust is untouched because LIST is not consulted for the verdict.
3. For every life that folds, one LIST of the life's stream prefix with `start-after` = the key of the covered floor (`refLogKey(life, last_folded_ref_id)`). Lexicographic key order equals `(writer_epoch, ref_sequence)` order (fixed-width hex), so the page holds the `_log` tail above the floor and every `_snap` key, which is what `groupRefKeys` needs to build this life's `RefTableListing`. `ObjectStorageBackend::listUnder` already accepts a cursor for `start-after`.
4. `shouldDeferRound` is evaluated from the probe results, before any LIST. A quiet pool pays one GET per live life and nothing else.

Cost: a quiet life is 1 GET; a hot life is 1 GET + 1 LIST page. On otel.demo (94 lives) about one second at 16 workers instead of 1575 s. At 10k tables about 30 s at 16 workers, independent of backlog, and it scales with `cas_gc_concurrency` where the global LIST is serial by continuation token.

`dead_life_debris` moves to the janitor (B4); `defer_decision` no longer reports it.

### B2. Cleanup lists its own range {#b2-cleanup-range}

`cleanupRefObjects` stops depending on the fold-time listing. For every `Live`/`Removing` life whose checkpoint names a valid recovery triple (unchanged gate, `readCheckpointSnapshotBase`):

1. LIST `<life>/_log/` from the beginning; keep keys that satisfy the existing rule `L < checkpoint && L <= durable_cursor`; stop listing at the first key above the bound.
2. LIST `<life>/_snap/`; keep snapshot ids `< checkpoint` (unchanged rule, `planRefCleanup`).
3. Delete in chunks of `gc_bulk_delete_chunk_keys` through `removeChunkWriteOnceOrOneByOne`, under the per-chunk `authorityHolds` (licence in B3).

No cursor is needed: deleted keys vanish, so a later LIST from the beginning returns the oldest remaining keys. The phase stays post-CAS at its current position. The per-round bound is the count cap in stage B and the deadline in stage C.

### B3. Cleanup licence per row, adjudicated before code {#b3-cleanup-licence}

Today `authorityHolds` requires `current_catalog.etag == folded.catalog_cut->etag`, whole-pool stillness between the fold's cut and the post-CAS cleanup, and one refusal `return`s out of the whole pass (`[covered-log-cleanup-aborts-on-catalog-etag]`, CAS-079). Under CREATE/DROP churn 40 of 85 rounds deleted nothing; under a longer round the window only widens. After B2 the cleanup's inputs no longer include anything from the fold-time listing: they are a catalog cut, the `gc/state` this round just adopted, and the life's own checkpoint.

Design hypothesis, to be adjudicated and not assumed: the cleanup takes its own fresh catalog cut at the start of the phase; each chunk is licensed against that cut by row value, life resolution (`throwIfAmbiguous`) and `gc/state` owner + `seq`; a refusal is scoped to its namespace and the pass continues with the next life. This is candidates (a)+(b) of the backlog plan. It is also the only licence shape that survives multi-node execution (section 7), where an executor has no "moment of the fold".

Order, as the backlog prescribes and this spec keeps: (0) estimate from run-8 data; (1) one local run with the stop reason split into ProfileEvents (`etag only, row and life unchanged` / `row or life changed` / `gc/state absent or moved`) plus the planned-but-undeleted count per round; (2) if etag-only dominates, ca-arch adjudication with the code and the author's "same complete observation" comment, then codex review capped at three rounds; (3) code. Relaxing the etag comparison directly is not allowed at any step.

### B4. Dead-life debris belongs to the janitor {#b4-janitor}

The janitor already LISTs `namespaceRootPrefix()` itself, one page per round, and removes dead-life objects one HEAD + one exact `remove` at a time. Two changes, both from the janitor brainstorm's revised order:

1. Dead-life `_log`/`_snap` keys are write-once and never reborn at the same key: delete them through `removeChunkWriteOnceOrOneByOne` (a page becomes one or two batch requests) instead of per-key HEAD + `remove`. No licence change.
2. `gc_janitor_pages_per_round` (default 1) and `gc_janitor_page_keys` (default 1000) become settings. The phase keeps its position: moving it before `defer_decision` is not invariant-preserving (`suppress_destructive` comes from the fold verdict).

`[targeted-drain-of-retired-lives]` stays frozen and is not part of this work.

### B5. Acceptance {#b5-acceptance}

- `defer_decision` wall time does not change when `_log` keys are added below the floor of quiet lives; it changes only with the number of live lives.
- `ref_object_cleanup` deletes on every round in which some life has covered keys; after B3, refusals whose row and life were unchanged are zero.
- After a full drain, a one-off global LIST of `cas/ns/stream/` returns on the order of `live lives × retained tail` keys.
- A pool with a large dead-life debris drains at `pages_per_round × page_keys` keys per round with one or two delete requests per page.

## 5. Stage C: a deadline instead of count budgets {#stage-c}

### C1. One round deadline, families cut by the remainder {#c1-deadline}

`cas_gc_round_deadline_sec` (default 300), measured from the round start. Never cut: the probes, the intake of every live life, the reduce merge, the seal, the `gc/state` CAS, `pruneSupersededGenerations`' own cursor logic, and the hand-off reclaim (one-shot, backlog class C). Cut by `remaining()` before scheduling the next window or chunk, because their remainder already lives in durable state:

| family | what carries | where |
|---|---|---|
| graduation | condemned rows carried unchanged | the run (as today under the cap) |
| redelete | entries stay `delete_pending` | the run |
| `ref_object_cleanup` | nothing to carry; recomputed from the checkpoint | durable checkpoint and cursor |
| janitor, orphan sweep | cursor | `gc/state` |

The phase order is unchanged, so when time runs out the post-CAS families are the first to be starved; they are the cheapest to repeat.

### C2. Count budgets of class A and cleanup are removed {#c2-budgets-removed}

Removed: `cas_gc_round_graduation_budget`, `cas_gc_round_redelete_budget`, `cas_gc_round_ref_cleanup_budget`. The on-S3 format is frozen since `26.6.4` and is not touched here; for the config keys, an old key is accepted for one release as a deprecated no-op with a Warning at load (the existing unknown-`cas_` diagnostic would otherwise fail a config that worked yesterday), and the docs carry a migration note. Kept: `cas_gc_round_sweep_namespace_budget`, `cas_gc_round_sweep_recovery_op_budget`, `cas_gc_round_prefix_wholesale_budget`, `cas_gc_round_handoff_prefix_wholesale_budget` (cursor-paced or one-shot, backlog classes B and C), `rebuild_edge_budget` (memory), `cas_gc_round_outcome_entry_budget` (audit size). CAS-101 is closed in the same change: `report.deleted` / `absent` / `replaced` / `spared` are tallied from the in-memory decisions, not from the capped outcome logs. `gc_frontier_probe_budget` is out of scope and stays documented as an off switch.

### C3. Round pacing {#c3-pacing}

If a round ends with `deadline_hit` and a non-empty carry, the scheduler starts the next round immediately (`requestRoundSoon`); `gc_interval_sec` applies only when the carry is empty. Every round still performs discovery, intake and the seal, so new garbage keeps flowing while old garbage is drained.

### C4. Not in stage C {#c4-not-in-c}

- A graduation cursor in the seal. The carry costs one streaming GET of the run (~26 MB for 520k entries); the expensive per-entry GET is removed by A1. A cursor is a seal-format change and stays a follow-up gated on a measurement showing the run read dominating.
- Cutting the intake (section 2).
- Resuming an interrupted round. The round is one-pass: intake and reduce leave their runs under the attempt prefix, but the only durable record of a completed phase is the fold seal, written after reduce, so a fresh attempt cannot tell a complete set of intake runs from an interrupted one and starts from the parent seal. On otel.demo two restarts during intake or reduce cost four hours each (2026-09-25). Stage C makes that loss small (a round is minutes) but does not resume it; resume needs per-phase completion markers, which are the per-life work units of section 7.

### C5. Observability {#c5-observability}

- Phase rows: `deadline_hit`, `carried` on graduation, redelete, cleanup, janitor, sweep.
- Round row: `deadline_hit`, `carry_total`.
- `system.cas_mounts.pending_reclaim` is computed from the adopted seal's `CondemnedSummary` (durable, restart-safe) instead of the process-local counter.
- Metrics are attached to phases and families, not to the round, for section 7.

### C6. Default and acceptance {#c6-acceptance}

300 s leaves about four minutes of catch-up per round after a one-minute mandatory part on a pool of a hundred tables: at 16 workers and 70 ms per request, roughly 3-4k redeletes plus as many graduations plus tens of thousands of `_log` keys per round. On otel.demo's 320k retirements per day that is ~100 five-minute rounds per day. The number is tuned on the stand.

- With a 500k-blob backlog, round duration stays at the deadline and does not grow with the backlog.
- The backlog decreases linearly at about `concurrency / latency`.
- After the drain, rounds take under a minute and run with the `gc_interval_sec` pause.

## 6. Settings summary {#settings}

| key | change |
|---|---|
| `cas_gc_concurrency` | from the rework spec; governs every fan-out including A1-A3 and B1 |
| `cas_gc_round_deadline_sec` | new, default 300 (C1) |
| `cas_gc_janitor_pages_per_round`, `cas_gc_janitor_page_keys` | new, defaults 1 and 1000 (B4) |
| `cas_gc_round_graduation_budget`, `cas_gc_round_redelete_budget`, `cas_gc_round_ref_cleanup_budget` | removed (C2) |

## 7. Multi-node GC: direction and the constraints it imposes {#multi-node-direction}

Not implemented here. Recorded so that A, B and C do not have to be undone.

Target shape: one logical round, one leader as coordinator, one seal and one `gc/state` CAS. Work splits into three classes with natural keys: per life (frontier probe, intake of one life into per-shard delta runs, cleanup of that life's covered keys), per blob-hash shard (`gc_shards`: reduce, graduation, redelete, outcome logs, condemn markers), and coordinator-only (catalog cut, lease, barriers, seal, CAS, prune, hand-off). Two barriers, "all lives taken in" and "all shards reduced"; the shuffle between them is the existing per-shard delta runs in the attempt prefix. Executors claim work units and report completion inside the attempt prefix, so a failed attempt's debris is never adopted, as today. The claim and done objects are new object kinds and are explicitly outside this spec.

Resume of an interrupted round is the same change as multi-node execution, not a separate one: a per-life completion marker in the attempt prefix ("intake of this life reached frontier X") is both the executor's `done` record and what lets a fresh attempt adopt a predecessor's finished phases instead of restarting from the parent seal. Reduce already has such a record, the fold seal; intake has none. These markers are new write-once object kinds inside the attempt prefix, invisible to older readers, and remain outside this spec by the owner's rule on new object kinds.

Constraints on this spec, all satisfied by the sections above:

1. No in-process state on the decision path (A1: the memo is a shortcut).
2. The unit of discovery and cleanup is a life (B1, B2).
3. The cleanup licence is local to a life plus `gc/state` owner and seq (B3 hypothesis); whole-catalog stillness across nodes is not achievable.
4. The deadline belongs to a work family, and the remainder lives in durable carry, not in `RoundReport` (C1).
5. Outcome application order is deterministic within a shard only (A3).
6. Pools are per round, not per disk (rework spec 3.1).
7. Phase rows carry `server_root_id` and `round_id` already; new metrics go on phases (C5).

## 8. Tests, failing-first per stage {#tests}

Suite names match the `CAS*` gate filter. Under `DEBUG_OR_SANITIZER_BUILD` any test that expects a `LOGICAL_ERROR` is a death test. The gate runs under ASan and TSan on the exact tree pushed.

Stage A:

- A1: fresh `Gc` (empty memo) over a run with N condemned rows; instrumented backend asserts meta reads overlap (in-flight > 1) at concurrency 4 and peak 1 at concurrency 1; outcomes equal the sequential run's; an unreadable meta yields `CASGCCondemnMarkerUnconfirmedCarry` and a marker rewrite, never a graduation.
- A2: candidate superset with gaps; assert hits/misses/wasted per round against the sequential oracle; `Miss` within one window of zero.
- A3: the rework spec's tests 5 to 10, 9a, 9b (overlap witness, worker-thread fault, multi-fault batch all logged and counted, `Replaced` inside a batch, apply-phase fault).

Stage B:

- B1: a pool with K live lives and M `_log` keys below every floor; assert `defer_decision` issues exactly K exact GETs and no LIST when nothing changed, and one LIST per hot life otherwise; assert the DEFER verdict and `ref_tables` equal the global-LIST implementation's for the same state (oracle kept in the test only).
- B1: a `_log` key present above the floor but omitted by an injected LIST omission still folds (proof is the exact GET), and a key omitted below the floor is never touched.
- B2: keys below the bound are deleted in batches; a key equal to the checkpoint or above the durable cursor is never deleted; a second round with nothing below the bound issues one LIST per life and no delete.
- B3 (after adjudication): a CREATE of an unrelated table between the fold and the cleanup no longer refuses the pass; a changed row or life of the cleaned namespace refuses that namespace only; a moved `gc/state` owner or seq refuses everything.
- B4: a dead life with P keys drains in ⌈P/1000⌉ delete requests; pages per round setting honored.

Stage C:

- C1: a fake clock; a round with a large carry stops each family at the deadline, commits, and the next round continues from the durable carry; the intake and the seal are never skipped.
- C2: the removed keys fail config load with the unknown-key diagnostic; `objects_deleted + objects_absent + objects_replaced == entries_redeleted` past the old cap.
- C3: `deadline_hit` with carry schedules the next round without the interval; empty carry waits the interval.
- C5: `pending_reclaim` equals the seal's condemned minus delete_pending totals and survives a `Gc` re-creation.

## 9. Documentation {#documentation}

`docs/en/antalya/cas/architecture/garbage-collection.md` (phase list, discovery without the global LIST, the deadline), `docs/en/antalya/cas/configuration.md` (settings table, migration note for the removed keys), `docs/en/operations/system-tables/cas_gc_log.md` (new phase metrics and round columns), `docs/en/operations/system-tables/cas_mounts.md` (`pending_reclaim` semantics). Changelog: one Performance Improvement entry per stage.

## 10. Open questions for the owner {#open-questions}

1. B3 is adjudicated, not decided (section 4). Confirmed in conversation.
2. AWS conditional `DELETE` with `If-Match` on the blob key would make the `pending_deletes` HEAD redundant (412 and 404 give the same classification). This is a protocol-step change under the user's veto on such optimizations for PUT; it is listed here only as a question, not as work.
3. The default of `cas_gc_round_deadline_sec` (300) is a starting point to be tuned on the stand.
4. Audit F5: the ref lane overwrites `_ckpt` on every flush (477k PUT/day, 21% of PUTs, one of the two PUTs on the flush's critical path). A checkpoint every N flushes or T seconds bounds the recovery walk by N logs. Protocol-semantics decision for the owner; not part of this spec.
5. Audit F7: batching the unconditional `.meta` deletes after a successful blob delete through the 1000-key batch path. Free on AWS, saves requests and threads; a protocol-step change for the owner.
6. Audit F2: the `delete_tmp` repoint elision is the largest writer-side lever (191k repoints/day on the stand, a third of GC intake). It is a writer change outside this spec and should be scheduled next to stage A.

## 11. Verification items for the plan {#verification-items}

- The current state of PR #2351 against the rework spec (last code commit 2026-09-18 carries `cas_gc_io_concurrency` and a per-`Gc` `io_pool`, not the round-scoped shape); stage A's first task is to close that gap on the PR branch.
- `groupRefKeys` and `RefTableListing` behaviour when given a per-life listing that starts at the floor (B1 step 3): confirm nothing below the floor is required for the grouping's validation.
- The exact key the intake probes for `frontier_proven`, to reuse it verbatim in B1 step 2.
- `listUnder` `start-after` semantics on GCS and the local backend (the code notes some backends ignore it and the filter keeps the contract).
- Audit F6: the global LIST costs ~9.3 S3 requests per 1000-key logical page (`S3ListObjects` 14.4k per round against
  `CASRefGlobalListPages` 1543; the `defer_decision` phase, not the walk alone). The request-cuts spec
  (`2026-09-29-cas-gc-request-cuts-design.md`, section 6) measures and attributes them; the fix belongs here, under three
  constraints: a resumed page bounded to `limit` truncates walks on stores that ignore `start_after`; `max_keys` sets the
  request's `MaxKeys` and does not stop `S3IteratorAsync`'s prefetch, so a transport-level request-count test is needed;
  a trailing empty page moves the janitor cursor, the orphan sweep's `wrapped` and `deletePrefixWholesale` at exactly its
  budget. One request per 1000 keys stays the target.
- Audit F8: ~200k/day manifest PUTs on the stand are unattributed (2 per part publish); attribute before any writer-side change.
- Where `CondemnedSummary` totals are available to `cas_mounts` without a new read (C5).
