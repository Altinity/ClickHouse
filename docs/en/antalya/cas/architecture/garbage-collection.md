---
description: 'The CAS garbage collector: leadership as work de-duplication, the 18-phase round pipeline, condemnation and exact-token deletion, sharding, and observability.'
sidebar_label: 'Garbage collection'
sidebar_position: 8
slug: /antalya/cas/architecture/garbage-collection
title: 'CAS Architecture — Garbage Collection'
doc_type: 'reference'
---

# CAS architecture — garbage collection {#garbage-collection}

## GC model {#gc-model}

`GC` is the only place in `CAS` that ever deletes a blob body or a manifest body. It runs as a
background, lease-paced loop per mount (`Gc::runRegularRound`, `Gc/CasGc.cpp`), folding ref-log
history into blob in-degree, condemning what reaches zero, and deleting only after that
condemnation has survived a full extra round. Each call to `Gc::runRegularRound` is one round
execution. It first processes the `GC` lease by creating, renewing, observing or stealing it, then
either returns as a follower, defers the fold when no destructive decision is due, or folds ref-log
history into a new in-degree snapshot and publishes it.

The [`cas_gc_interval_sec`](/antalya/cas/configuration#disk-settings) disk setting controls the
normal interval between background round executions (60 seconds by default). It is a scheduler
cadence, not a limit on the duration of one round execution. Round duration depends on the amount
of ref-log, manifest, candidate, and cleanup work and on the configured per-round work budgets. A
manual `SYSTEM CAS GC RUN` can request a round execution without waiting for the interval.

This page covers leadership, the round's 18 phases, condemnation and deletion, sharding, pruning,
round cost, and observability. Manifest and ref mechanics that `GC` folds are covered on the
[manifests-and-refs page](/antalya/cas/architecture/manifests-and-refs); the writer-versus-`GC`
race over one blob is covered on the
[blob-protocol page](/antalya/cas/architecture/blob-protocol#writer-gc-race).

## The round {#the-round}

A folding round execution is one pass of 18 named phases ending in exactly one commit `CAS` over
`gc/state` (`Gc::runRegularRound`, `Gc/CasGc.cpp`). Every round execution starts with phase 1, but a
follower or a deferred round execution returns before that commit.

| # | Phase (`GcPhaseTimer` name) | Runs on | What it does |
|---|---|---|---|
| 1 | `lease` | always | Create, renew, observe or steal the lease inside `gc/state`. The only phase a `NotALeader` round emits |
| 2 | `pre_fold_ref_drain` | always | Resolve catalog `Removing` rows whose cleanup evidence the adopted parent already sealed; drop the completed ones before defer or new fold work |
| 3 | `heartbeat_floor` | always | One `LIST` of `gc/server-roots/`, one `GET` per mount slot, fence-out `PUT` for any mount whose write-token has held stable past the threshold |
| 4 | `defer_decision` | always | One full `LIST` of `cas/ns/stream/`, build the catalog-keyed ref walk plan; decide `DEFER` (nothing changed, no graduation due) or continue to a full fold |
| 5 | `parent_seal_read` | fold | Capture the parent fold seal's run references before the fold mutates the in-memory generation/attempt |
| 6 | `fold_ref_group` | fold | Regroup the one `LIST` from phase 4 into per-namespace listings — no I/O, the keys are already in hand |
| 7 | `fold_seal_read` | fold | `GET` and decode the adopted fold seal that anchors this fold's coverage |
| 8 | `fold_ref_intake` | fold | `GET` every new ref-log record and every referenced manifest, extracting blob source edges |
| 9 | `fold_reduce` | fold | Merge prior edges, new deltas and the parent's condemned rows: spare, condemn, graduate or redelete each candidate; compute `suppress_destructive` |
| 10 | `fold_seal_write` | fold | Write the new fold seal once, write-once deterministic, adopting a byte-identical replay instead of rewriting it |
| 11 | `pending_deletes` | fold | The single content-delete site: exact-token delete of every entry a *previous* round marked `delete_pending`, plus the outcome-log writes |
| 12 | `meta_pool_wait` | fold | Drain the bounded pool of async `.meta` condemn-marker writes queued during the fold |
| 13 | `round_commit` | fold | Retention-prune old generations, then publish the single `gc/state` `CAS` that adopts the whole round |
| 14 | `handoff_reclaim` | post-`CAS` | Reclaim a generation a ref moved off during this round, before the ordinary wholesale prune would reach it |
| 15 | `manifest_deletes` | post-`CAS` | Delete manifest bodies whose owner-removal minus-one edge the `CAS` in phase 13 just adopted |
| 16 | `namespace_cleanup` | fold; suppressed on `DEFER` | One bounded page of the perpetual namespace janitor, reclaiming dead-life debris |
| 17 | `ref_object_cleanup` | post-`CAS` | Prune ref logs and snapshots once both fold coverage and a live snapshot make them safe to delete |
| 18 | `orphan_sweep` | post-`CAS` | Exact-token deletion for the [orphan-manifest sweep](/antalya/cas/architecture/manifests-and-refs#orphan-sweep), after phase 13 adopted each candidate's blob-source retirements and the cursor |

Phases 5 through 18 run only when phase 4 decides to fold. A `DEFER` verdict is not a bare no-op:
it still runs one bounded namespace-janitor page with `suppress_destructive = true` — cursor
progress and diagnostics only, no deletes — and then returns, publishing no fold artifact and no
commit `CAS`. Its lease `CAS` may already have created or renewed the lease in phase 1:

```mermaid
flowchart LR
    D4{"4 defer_decision"} -->|"nothing changed, no graduation due"| DEF["DEFER: one suppressed<br/>namespace-janitor page, then return"]
    D4 -->|"changed shards, or graduation due"| FOLD["phases 5 through 18: full fold and round commit"]
```

Orderings that are load-bearing:

- **2 before 4** — a row proved complete by the adopted parent is resolved before `DEFER` or any
  successor plan can publish.
- **15 after 13** — manifest bodies are deleted only after the `CAS` adopted their decrements.
- **13's prune before the `CAS`** — a pre-`CAS` destructive action may rely only on already-
  published state.

**Clamp suppression.** `suppress_destructive = !anomalies.empty() || !carried_holds.empty() ||
!frontier_complete` is computed once and threaded into the merge, current-life ref cleanup and the
perpetual namespace janitor, so they cannot desynchronize. Under suppression there is no
graduation, no redelete, and no ref or namespace deletion; condemnation and sparing continue,
because both are non-destructive.

**Fail-closed aborts.** A throw before the commit `CAS` means nothing is adopted: unapplied transactions,
a cursor/apply mismatch, a missing adopted seal, a table with a snapshot but no surviving log and
no cursor, a non-total condemned summary, and an observed delete marker (bucket versioning is on).

## Phase 1 — lease {#phase-1-lease}

Establishes whether this GC may run the round. There is no separate lease object: the lease lives
inside `gc/state` as `{owner, seq}`, and a round is authorized by winning a token-guarded `CAS` on
that object.

- **Runs on:** always — the only phase a `NotALeader` round emits
- **Reads:** `gc/state`; `gc/hb` (only when another GC owns the lease)
- **Writes / deletes:** one `CAS` on `gc/state` (acquire, renew or steal); no deletes
- **Safety:** the lease is *work de-duplication, not mutual exclusion* — see below
- **Fails the round if:** this `Gc` instance saw `gc/state` before and it has since disappeared
  (`CORRUPTED_DATA`)
- **Observability:** phase row `lease`; metrics `acquired`, `steal_allowed`

```mermaid
%%{init: {"flowchart": {"curve": "linear", "nodeSpacing": 15, "rankSpacing": 20}, "themeVariables": {"lineColor": "#000000"}}}%%
flowchart TD
    READ["GET gc/state"] --> EXISTS{"gc/state exists?"}
    EXISTS -->|yes| OWNER{"lease.owner = gc_id?"}
    EXISTS -->|no| OBSERVED{"Observed before?"}
    OWNER -->|yes| RENEW["Renew"]
    OWNER -->|no| STEALABLE{"Steal allowed and both<br/>lease and heartbeat frozen?"}
    OBSERVED -->|yes| CORRUPT["CORRUPTED_DATA"]
    OBSERVED -->|no| ACQUIRE["Acquire"]
    STEALABLE -->|yes| STEAL["Steal"]
    STEALABLE -->|no| FOLLOWER["Follower"]
```

**Acquire / renew.** If `gc/state` is absent and never seen, this GC creates it with `owner = gc_id`,
`seq = 1`, and fixes `gc_shards`. If `lease.owner` is already this `gc_id`, it increments `seq` with
a token-guarded `CAS`. Non-lease fields are always preserved; a conflict causes a bounded re-read.

**Follower / steal.** If another GC owns the lease, this GC reads `gc/hb` and normally returns
`NotALeader`. The leader advances an advisory `gc/hb` object; the heartbeat proves activity but
grants no authority. A candidate compares `(lease.owner, lease.seq)` and the `gc/hb` writer/sequence
against what it recorded on its *previous* scheduled round. If either signal moved, the owner is
live and the candidate records a fresh observation and backs off. A steal is allowed only when both
signals stayed frozen across two paced observations; the candidate then rewrites `lease.owner`,
increments `seq`, and `CAS`es. A lost steal returns `NotALeader` with no retry. A new process has no
recorded observations, so it can never steal on first sight. A manual `SYSTEM CAS GC RUN` may
acquire or renew but never steals.

**The `CAS` token.** Acquire / renew / steal return the `gc/state` version's backend token; phase 13's
commit `CAS` uses it, so any intervening write to `gc/state` rejects a stale leader's commit. The
resulting `lease.seq` is the folding round's attempt id.

**Safety after leadership changes.** A deposed leader may keep running; correctness does not need
exclusive execution:

1. a folding round is published by exactly one commit `CAS` — a deposed leader's commit fails and
   its candidate generation is never adopted;
2. every fold artifact is written under that leader's own attempt number, invisible to readers and
   reclaimed by generation pruning;
3. destructive pre-`CAS` actions are justified only by previously published durable state, so they
   are replay-idempotent;
4. deletes are exact-token, so a stale leader can never delete a newer incarnation.

## Phase 2 — pre-fold ref drain {#phase-2-pre-fold-ref-drain}

Finishes namespace removals that the last committed fold already proved safe: removes the matching
`Removing` row from the ref catalog. Removal is split across rounds — one fold writes
`cleanup_evidence` into its seal, a *later* round's phase 2 acts on it — so phase 2 never trusts
evidence from the fold running now.

- **Runs on:** always (leader), including on a round that later returns `Deferred`
- **Reads:** the adopted fold seal (`ref_lives` coverage + `cleanup_evidence` only); the ref
  catalog; `gc/state` (leadership re-check around every write)
- **Writes / deletes:** rewrites the ref catalog via `CAS` to drop each eligible row (not a backend
  `DELETE`); no blob, manifest or namespace-object deletes
- **Safety:** a row is dropped only if the adopted parent seal carries `cleanup_evidence` for the
  *same* `incarnation` and that life has no coverage hold; each write is token-guarded and bracketed
  by two `gc/state` re-reads
- **Fails the round if:** `gc/state` points at a missing parent seal (`CORRUPTED_DATA`); leadership
  changes mid-drain (retry-later — a write already sent stays safe, authorized by the parent seal
  and the catalog token)
- **Observability:** phase row `pre_fold_ref_drain`; metric `deleted` (rows removed)

On a fresh pool (`snap_generation = 0`) phase 2 is a logged no-op; it can first remove a row only in
a leader round *after* some earlier fold committed a generation. Phase 3 starts only once every
parent-authorized removal has been resolved — phase 2 is a barrier. Phase 16 later deletes the old
namespace's physical `_log` / `_snap` / `_ckpt` / `_files` objects.

## Phase 3 — heartbeat floor {#phase-3-heartbeat-floor}

Checks each mounted writer for liveness and fences any writer incarnation that stopped renewing its
mount lease. Despite the name it does not touch `gc/hb` (that is phase 1); it watches the backend
token of each `mount` object.

- **Runs on:** always (leader), fold and deferred paths
- **Reads:** `LIST` of `gc/server-roots/`, then a `GET` of each `<server_root_id>/mount`
- **Writes / deletes:** a token-guarded `PUT` per fenced mount (`gc_fenced = true`, `seq + 1`); no
  deletes
- **Safety:** fences only after this leader's *own* monotonic clock has watched the mount's
  write-token hold unchanged for `mount_lease_ttl_ms + 5% + mount_renew_period`; the write is
  guarded by that exact token. A completed fence-out stays valid even if this GC later loses
  leadership. `expires_at_ms` (another host's wall clock) is never trusted.
- **Fails the round if:** nothing — per-mount `PUT` conflicts are re-classified (up to four), then
  treated as `live`
- **Observability:** phase row `heartbeat_floor`; metrics `live`, `terminated`, `fenced_now`,
  `already_fenced`; `GcFenceOut` audit rows in `system.cas_log`

The first observation of any mount is always `live`; a new `Gc` instance starts with an empty
observation map, so it can only delay a fence-out, never do one early. A mount with
`min_active = UINT64_MAX` is a clean farewell (`terminated`). Results feed metrics and events only —
phase 4 takes no input from them.

## Phase 4 — defer decision {#phase-4-defer-decision}

Builds the round's one ref-stream work plan and decides `fold` vs `defer`. `fold` continues to
phase 5 and builds a new in-degree snapshot; `defer` skips generation construction and the commit.

- **Runs on:** always (leader)
- **Reads:** one full `LIST` of `cas/ns/stream/` (key names only); the ref catalog; the adopted fold
  seal (twice, on an adopted generation)
- **Writes / deletes:** none
- **Safety:** a missing / invalid / incomplete adopted seal cannot produce a quiet defer — the
  graduation check refuses to defer and the plan read surfaces the bad state
- **Fails the round if:** the plan-building seal read reports an invalid adopted seal
- **Observability:** phase row `defer_decision`; metrics `changed_shards`, `namespaces_seen`,
  `ref_log_keys_listed`

The plan has one row per admitted catalog life (`Live` or `Removing`; `Creating` excluded), joining
its last folded position, its greatest listed `_log` position, and the keys later phases need. A row
is *changed* when the listed `_log` is newer than the last folded position. Phase 4 chooses `fold`
if any of: changed rows ≥ `gc_fold_threshold` (default 1); an adopted shard has a published pending
delete; an adopted shard has a condemned blob due to graduate
(`oldest_nonpending_condemn_round < round + 1`); or `gc_fold_max_defer_rounds` (default 8)
consecutive defers were reached. On `defer`, one suppressed namespace-janitor page runs (phase 16's
work) and the round returns without a commit. On `fold`, phase 6 reuses this plan and the same
`LIST`.

## Phase 5 — parent seal read {#phase-5-parent-seal-read}

Copies the *previously* adopted fold seal's run references into memory before the new fold can
overwrite them. "Parent" is the prior GC state the new one builds on, not a key-hierarchy parent.

- **Runs on:** fold path only
- **Reads:** the adopted fold seal (its run-reference list only, never the run objects)
- **Writes / deletes:** none
- **Safety:** read-only; a stale leader's saved list only feeds decisions its own commit `CAS` gates
- **Fails the round if:** the seal is unreadable — a seal that vanished after phase 4 yields an
  empty list here, and phase 7 re-checks and reports `CORRUPTED_DATA` before the commit
- **Observability:** phase row `parent_seal_read`

An adopted seal can reference a run stored under an older generation, and the new fold may replace
that run and stop referencing its old generation. Keeping the parent's references lets phase 13
shield those generations from retention pruning if the commit loses, and lets phase 14 reclaim a
generation the parent referenced but the new seal no longer does. Empty on a fresh pool.

## Phase 6 — fold ref group {#phase-6-fold-ref-group}

Regroups phase 4's flat key list into per-namespace listings and freezes the catalog cut. No
backend I/O.

- **Runs on:** fold path only
- **Reads:** nothing (keys already in memory from phase 4)
- **Writes / deletes:** none
- **Safety:** the fold is catalog-authoritative — a namespace exists iff the catalog cut names its
  incarnation; the `LIST` is only a per-namespace hint
- **Fails the round if:** a ref-object key under the stream prefix is unparseable — the round aborts
  the ref walk, produces no ref delta, advances no cursor, records the `ref_folding_aborted`
  anomaly, and forces `suppress_destructive` for the whole round (not re-raised)
- **Observability:** phase row `fold_ref_group`; metrics `ref_keys_listed`, `namespaces_seen`,
  `ref_folding_aborted`

Also computes an empty-universe proof (catalog snapshot has a token and zero entries of any state)
used by phase 9's destructive gate. A malformed key does not skip phase 8's `_ckpt` reads.

## Phase 7 — fold seal read {#phase-7-fold-seal-read}

Reads the adopted fold seal that anchors this fold's coverage and sets up the fold's base state (the
prior coverage view, the mutable successor, the new generation / attempt numbers).

- **Runs on:** fold path only
- **Reads:** the adopted fold seal, twice at the same address — one read anchors coverage, one loads
  the parent run references (see [per-phase backend cost](#per-phase-cost))
- **Writes / deletes:** none
- **Safety:** read-only
- **Fails the round if:** the adopted seal is absent while `snap_generation > 0` — `gc/state` points
  at a missing artifact; the fix is `SYSTEM CAS GC REBUILD` (`CORRUPTED_DATA`)
- **Observability:** phase row `fold_seal_read`; metrics `parent_ref_lives`, `parent_runs`,
  `parent_cleanup_evidence`

The fold's writes land under `attempt = lease.seq` and `new_generation = snap_generation + 1`, while
reads of the parent generation keep using `snap_attempt`. On a fresh pool both reads return nothing
and the fold starts from an empty baseline.

## Phase 8 — fold ref intake {#phase-8-fold-ref-intake}

Reads every new ref-log record of every walkable namespace and the manifests it references,
extracting blob source edges. The heaviest read phase of a folding round.

- **Runs on:** fold path only
- **Reads:** one `_ckpt` per namespace in the universe; `_log` records from each namespace's cursor
  up to its committed ceiling; one manifest body per folded owner edge; extra `_log` reads when the
  walk crosses an epoch seal
- **Writes / deletes:** none (the successor seal's `cleanup_evidence` rows are written between this
  phase's timer and phase 9's)
- **Safety:** per-namespace failures stay per-namespace — a *hold*, never a whole-round abort. A
  concurrent writer appending mid-round changes nothing: the ceiling (`committed_through`) is
  snapshotted once, so the round folds a fixed amount of work. Transactions apply atomically; the
  durable cursor advances once per fully folded record.
- **Fails the round if** (`CORRUPTED_DATA`): a manifest body whose ref / namespace disagrees with its
  key; a table with no sealed cursor whose baseline logs are already gone; a sealed cursor that does
  not close the run the walk produced; a `RemoveNamespace` for a namespace absent from the catalog
  cut; `logs_accounted ≠ logs_applied`
- **Observability:** phase row `fold_ref_intake`; metrics `frontier_namespaces` / `frontier_proven`
  (universe and its proven part), `tables_held`, `logs_accounted` / `logs_applied`; per-cause hold
  reasons are in [GC anomalies](#gc-anomalies)

**The universe** is exactly the `Live` and `Removing` rows of the frozen catalog cut. A namespace
the phase-4 hint omitted, with no carried hold and no `_ckpt`, is walked only while
`gc_frontier_probe_budget` lasts; once spent, the rest ride their cursors verbatim and the round is
suppressed. **The walk** starts at `cursor + 1` (or the checkpoint's genesis position) and stops at
the committed ceiling; a namespace is *proven* only when it reaches the ceiling exactly. Any other
exit — a hold, an unusable checkpoint, the probe budget — leaves it unproven, which feeds phase 9's
gate.

## Phase 9 — fold reduce {#phase-9-fold-reduce}

Recomputes the per-shard in-degree snapshot and computes the round's single destructive gate.

- **Runs on:** fold path only
- **Reads:** streaming `GET` of each referenced parent run segment; one `HEAD` per zero-in-degree
  candidate; one `.meta` `GET` per graduation candidate lacking in-process marker confirmation. When
  orphan-sweep planning runs: a `LIST` page of `cas/manifests/`, a `GET` per candidate, plus
  `gc/state`, the adopted seal, the catalog, and per-namespace `_ckpt` / tail `_log`.
- **Writes / deletes:** one `PUT` per rewritten run segment; schedules the async `.meta` condemn
  markers (drained by phase 12). No deletes.
- **Safety:** `suppress_destructive` is computed once here and read at every destructive site of the
  round; a *pure carry* shard (no delta, no orphan retirement, no parent condemned rows) copies the
  parent's run references with zero run I/O
- **Fails the round if** (`CORRUPTED_DATA`, before the phase 10 write): a folded transaction whose
  deltas reached no shard reducer; a sealed cursor count that disagrees with the walk
- **Observability:** phase row `fold_reduce`; metrics `shards_reduced` / `shards_pure_carry`,
  `condemned`, `graduated`, `spared`, `redelete_pending`, `suppress_destructive`, `frontier_complete`

`suppress_destructive` is true on any recorded anomaly, any hold in the seal about to be made
durable, or an incomplete frontier (`frontier_proven ≠ frontier_namespaces`, or a universe neither
non-empty nor proved empty). Under it the round still condemns, spares and carries, but graduation,
redelete, orphan-sweep planning, retention prune and post-`CAS` deletes do not run. Per candidate
the merge decides one of: `spare`, `condemn`, `supersede`, `graduate` (→ `delete_pending`, deleted
by phase 11 of a later round), `redelete` (→ deleted by phase 11 now), `carry`. `GcRoundWorkBudget`
independently caps graduations and redeletes per round; the overflow is carried unchanged.

## Phase 10 — fold seal write {#phase-10-fold-seal-write}

Validates, encodes and writes the new fold seal with one write-once `PUT`.

- **Runs on:** fold path only
- **Reads:** one byte-compare `GET` only on a deterministic replay
- **Writes / deletes:** one write-once `PUT` of the new `fold_seal`; no `CAS`, no deletes
- **Safety:** the seal is deterministic — the same fold inputs produce byte-identical bytes. A
  byte-equal occupant is this leader's own crash / replay and is adopted with no rewrite; a deposed
  leader writes under its own unadopted attempt and never collides with the adopted seal.
- **Fails the round if:** a divergent-bytes occupant (impossible under correct operation)
  (`CORRUPTED_DATA`)
- **Observability:** phase row `fold_seal_write`; metrics `seal_bytes`, `seal_runs`,
  `seal_ref_lives`, `seal_cleanup_evidence`

The seal's existence marks the fold complete; `snap_generation` / `snap_attempt` are advanced *in
memory* here and made durable only by phase 13's commit `CAS`.

## Phase 11 — pending deletes {#phase-11-pending-deletes}

The round's single content-delete site, before the commit `CAS`: executes the exact-token blob
deletes for entries a *previous* round published as `delete_pending`, and writes the forensic
outcome logs.

- **Runs on:** fold path only
- **Reads:** one `HEAD` only on the token-mismatch path
- **Writes / deletes:** `deleteExact` of each `redelete` blob body; one write-once outcome log per
  shard with settled entries
- **Safety:** an entry is deletable only because a previously committed fold seal published it
  `delete_pending` — durable state from an earlier commit, safe at any leader staleness. Exact token
  means a stale leader cannot delete a fresh incarnation; `NotFound` and `TokenMismatch` are
  tolerated. This round's own commit outcome does not affect the delete's safety.
- **Fails the round if:** a backend delete marker appears in a response — object versioning on a
  mis-provisioned pool (`LOGICAL_ERROR`)
- **Observability:** phase row `pending_deletes`; metrics `deleted`, `absent`, `redeleted`,
  `graduated`, `replaced`, `spared`, `outcome_logs_written`

Under `suppress_destructive` the `redelete` set is empty by construction, so nothing is deleted; the
non-destructive bookkeeping (`spared`, `graduated`, `replaced`) and the outcome logs still run. The
`RoundReport` deletion counters are tallied from the durable outcome logs, not the local decisions.

## Phase 12 — meta pool wait {#phase-12-meta-pool-wait}

A durability barrier: drains the round's batch of async per-hash `.meta` writes (the `Condemned`
markers scheduled by phases 9 and 11, plus phase 11's meta deletes) before the commit `CAS`.

- **Runs on:** fold path only
- **Reads / writes:** none on the GC thread; waits on the bounded `meta_pool`
  (`cas_gc_meta_pool_size`, default 16)
- **Safety:** a writer's meta point-read gate must see this round's condemns durable no later than
  the ledger they pair with. A throwing round's `SCOPE_EXIT` still drains the same pool, so no jobs
  run into the next round.
- **Fails the round if:** a `ThreadPool` framework failure (per-hash operation exceptions are caught
  inside the meta writer)
- **Observability:** phase row `meta_pool_wait`; job counts `jobs_scheduled`,
  `jobs_completed_on_entry`, `jobs_completed` (the `ProfileEvents` map is empty — work runs off the
  GC thread)

## Phase 13 — round commit {#phase-13-round-commit}

The commit boundary: a pre-`CAS` retention prune of old generations, then the single commit `CAS`
over `gc/state`. One phase because the prune is only safe as a pre-`CAS` action.

- **Runs on:** fold path only
- **Reads / writes:** `LIST` + wholesale `DELETE` of pruned generation prefixes; exactly one `CAS`
  on `gc/state`
- **Safety:** the prune skips any generation still referenced by the parent seal (phase 5) or the
  new seal — this is what stops a losing leader's prune from destroying what the winner's seal still
  points at. `snap_pruned_through` still advances past a skipped generation (phase 14 reclaims it
  later). `suppress_destructive` skips the prune entirely. The commit `CAS` uses phase 1's token, so
  a stale leader's commit is rejected.
- **Fails the round if:** the commit `CAS` is not `Committed` — `ABORTED` ("gc/state moved during
  the round"); the round publishes nothing
- **Observability:** phase row `round_commit`; metrics `generations_visited`, `pruned_through`,
  `generations_referenced`, `round`, `generation`

Prune bound: keep the last `cas_gc_snapshot_generations_to_keep` generations (default 3; `0` keeps
everything), at most 64 prefixes a round. After a `Committed` result the round is committed — an
exception in phases 14–18 does not un-commit it. See [the one-pass commit](#gc-state) for the fold
seal's role as the coverage record.

## Phase 14 — handoff reclaim {#phase-14-handoff-reclaim}

First phase of the post-`CAS` tail (phases 14–18 run only after a successful commit). Wholesale-
deletes a generation prefix that phase 13 had to skip (still referenced) but the cursor advanced
past, now that a ref has moved off it this round.

- **Runs on:** post-`CAS` (fold path)
- **Reads / writes:** one `LIST` + wholesale `DELETE` per handed-off generation prefix
- **Safety:** reclaims only when the parent seal referenced the generation, the new seal does not,
  it is already behind `snap_pruned_through`, `suppress_destructive` is false, and the phase's own
  budget (separate from phase 13's) is not exhausted
- **Fails the round if:** nothing — best-effort
- **Observability:** phase row `handoff_reclaim`; metrics `generations_reclaimed`,
  `objects_reclaimed`, `suppressed`

Unlike every other gated site, suppression here *loses* the reclaim rather than postponing it: the
ref moved off this round, nothing revisits, and the prefix is left to `fsck`. A crash in this window
leaks the same way.

## Phase 15 — manifest deletes {#phase-15-manifest-deletes}

Deletes owner-removed manifest bodies, now that phase 13's `CAS` adopted their minus-one decrements.

- **Runs on:** post-`CAS` (fold path)
- **Reads / writes:** `deleteExact` per `(manifest_id, token)` collected by phase 8's fold of `-1`
  owner edges; `NotFound` / `TokenMismatch` tolerated
- **Safety:** each body is unreachable from any live ref (its owner-removal was folded and
  committed) and is never re-derived — the intake cursor that found the `-1` edge is now committed,
  so a folded log is never revisited. Hence the phase is unbudgeted by design and drains the whole
  set each run.
- **Fails the round if:** nothing
- **Observability:** phase row `manifest_deletes`; metrics `attempted`, `deleted`, `suppressed`

Only a crash or `suppress_destructive` leaves an entry — it is then picked up by the orphan-manifest
sweep (phase 18).

## Phase 16 — namespace cleanup {#phase-16-namespace-cleanup}

One bounded page of the perpetual namespace janitor: deletes the physical objects of namespace lives
no longer in the catalog (dead-life debris).

- **Runs on:** fold path here; also on the deferred path right after phase 4 with
  `suppress_destructive` forced on
- **Reads:** the durable `janitor_cursor`; one `LIST` page (≤ 1000 keys) of `cas/ns/`; a fresh
  ref-catalog snapshot; `gc/state` per fence re-check
- **Writes / deletes:** `deleteExact` per dead-life `_log` / `_snap` / `_ckpt` / `_files` object;
  one `CAS` on the maintenance state when the page is decided
- **Safety:** each delete is under a GC fence re-check (`lease.owner` / `lease.seq`) before it and
  once at the end; the incarnation segment in every key makes an old life's objects structurally
  unreachable from a reborn same-name namespace, so a missed key can only leak storage, never expose
  it
- **Fails the round if:** nothing — the whole page is wrapped in a catch-all ("namespace janitor
  skipped this round")
- **Observability:** phase row `namespace_cleanup`; metrics `janitor_pages`, `janitor_keys`,
  `janitor_deleted`, `leaked`

The cursor advances only when the whole page was decided under a held fence and an unambiguous
catalog; under suppression it lists and classifies but deletes nothing and does not advance.

## Phase 17 — ref object cleanup {#phase-17-ref-object-cleanup}

Deletes the `_log` / `_snap` objects of **live** namespace lives once fold coverage and a
checkpoint-named recovery triple make them safe. Distinct from phase 16, which handles lives absent
from the catalog.

- **Runs on:** post-`CAS` (fold path)
- **Reads:** one `HEAD` per candidate; a fresh ref catalog + `gc/state` before *every* delete
  (authority re-validation)
- **Writes / deletes:** `deleteExact` per planned `_log` / `_snap` key; the checkpoint-named
  snapshot is always retained
- **Safety:** before each `deleteExact`, re-validates: ref-catalog token still equals the fold's
  catalog cut, same row and life, unchanged GC fence. The first failure stops the whole pass. A
  per-round `ref_cleanup` cap bounds it; on exhaustion the same candidates are recomputed next
  round.
- **Fails the round if:** nothing — `suppress_destructive` returns immediately (a clamp could leave
  a covered log whose delta is not yet durable)
- **Observability:** phase row `ref_object_cleanup`; metrics `namespaces_planned`, `suppressed`

## Phase 18 — orphan sweep {#phase-18-orphan-sweep}

The last phase: executes the [orphan-manifest sweep](/antalya/cas/architecture/manifests-and-refs#orphan-sweep)
planned in phase 9 and adopted by phase 13's `CAS`.

- **Runs on:** post-`CAS` (fold path)
- **Reads / writes:** `deleteExact` per nomination (planning `LIST` / `GET` cost was paid in
  phase 9); `NotFound` tolerated
- **Safety:** phase 9 exact-read and identity-validated each candidate and computed its source-edge
  retirements; phase 13's `CAS` adopted both those retirements and the sweep cursor, so a post-`CAS`
  body delete cannot orphan a still-reachable edge. A manifest is deletable only once its epoch's
  closing seal is consumed and no tail record above the cursor names it; any uncertainty retains.
- **Fails the round if:** a `TokenMismatch` — an immutable manifest identity must never change token
  (illegal ABA); stricter than every other post-`CAS` delete (`CORRUPTED_DATA`)
- **Observability:** phase row `orphan_sweep`; metrics `deleted`, `skipped`, `undecodable`,
  `cursor_advanced`, and `retained_*` broken down by reason

Under `suppress_destructive` phase 9 planned nothing, so the nomination list is empty and the cursor
does not move.

## GC anomalies {#gc-anomalies}

A GC round records *anomalies* and per-namespace *holds* instead of failing, unless a fail-closed
check fires. Any anomaly or hold in the seal about to be made durable forces `suppress_destructive`
for the whole round (phase 9); condemnation and sparing still run. A hold clears only when a later
walk folds through the offending position. `system.cas_log` carries the audit trail, capped per
round with each row bearing the true total.

| Name | Phase | Meaning | Effect |
|---|---|---|---|
| `ref_folding_aborted` | 6 | a ref-object key under the stream prefix is unparseable | round-wide: no ref delta, no cursor advance, `suppress_destructive` |
| `CheckpointUndecodable` / `CheckpointUnusable` | 8 | a live/removing life's `_ckpt` is undecodable, absent, or lacks `life_epoch` | the namespace folds nothing; held at `cursor + 1` if it has a sealed cursor |
| `CheckpointFrontierEmpty` | 8 | a checkpoint carries no `committed_through` but the namespace has a nonzero sealed cursor | anomaly; namespace unproven |
| `CommittedBelowCursor` | 8 | the sealed cursor is already above the committed ceiling | anomaly; namespace unproven |
| `GapBelowWitness` | 8 | a committed record at or below the ceiling is missing | the namespace is held |
| `UnconsumedSealCrossing` | 8 | an apparent epoch crossing has no consumed `EpochSeal` behind it | the namespace is held |
| `WitnessDisappeared` | 8 | an epoch-crossing chase resolves back to the absent position | the namespace is held |
| `ManifestBodyMissing` | 8 | a folded owner edge's manifest body is absent | the namespace is held below that record; re-read next round |
| `frontier_unprobed_budget` | 8 | `gc_frontier_probe_budget` ran out before every hint-less namespace was walked | round-wide `suppress_destructive` |
| `transactions_unapplied` | 9 | a folded transaction's deltas reached no shard reducer | **fails the round** (`CORRUPTED_DATA`) |

## The one-pass commit {#gc-state}

`<pool_prefix>/gc/state` is the durable safety and round-adoption state: `round`, `gc_shards`,
`snap_generation`, `snap_pruned_through`, `snap_attempt`, `manifest_sweep_cursor`, and the lease. A
folding round publishes it with exactly one commit `CAS` in phase 13, `round_commit`; the fold
itself performs no `CAS` of its own, and phase 1's lease `CAS` over the same object is the only
other writer.

**The fold seal *is* the coverage record**: generation, parent generation, one `ref_lives` row per
catalog-admitted opaque life (coverage plus optional cleanup evidence), references to the
source-edge run segments, and a per-shard condemned summary. It is encoded deterministically, so a
replayed round produces byte-identical bytes and adopts its own output through the
`putDeterministicArtifact` adoption pin (see the [blob-protocol page](/antalya/cas/architecture/blob-protocol#deterministic-artifacts)).
There is **no separate retired-list object** — condemned entries ride the source-edge run as
sentinel rows at `source_id = 0` — and **no run-file list outside the seal**; runs are resolved
*through* the seal's references, never by key construction.

## Finding orphans {#finding-orphans}

In-degree is a set of source edges, not a refcount. A blob becomes a candidate when its edge set
becomes empty and it was touched this pass: one `HEAD` captures the exact incarnation token and
size that a future delete will name. A blob merely carried from the parent run pays no `HEAD`.

**The grace period is measured in rounds, not acks:** an entry graduates once it has survived one
full round (`condemn_round < current_round`). The heartbeat floor is liveness only and **never**
gates graduation.

**The 404 rule.** A body that is present but invalid is `CORRUPTED_DATA`, hard. A body that is
missing is **never** a throw — the fold records and continues, and the caller decides by position:
a precommit activation clamps as a barrier; a committed or removal fold clamps only that table.
Prunes are likewise fail-open on 404.

## Condemnation and deletion {#condemn-delete}

```mermaid
flowchart LR
    A["round n: in-degree hits zero<br/>HEAD -- exact token t"] --> B["write .meta = Condemned round n<br/>async, bounded pool, drained pre-CAS"]
    B --> C["retired with condemn_round = n+1"]
    C --> D{"round n+1: re-verify"}
    D -->|"in-degree recovered"| S["SPARED -- recovery wins, even past the floor"]
    D -->|"still zero, confirmed durable Condemned evidence for hash and t"| G["GRADUATED -- delete_pending"]
    D -->|"still zero, evidence unconfirmed"| C2["carried unchanged, retry the marker, never throw"]
    D -->|"current token not equal to t"| SUP["SUPERSEDED -- a writer resurrected, re-condemn the CURRENT token"]
    G --> E["round n+2, pre-CAS: deleteExact blob, t"]
    E -->|"Deleted or Absent"| F["then drop the .meta"]
    E -->|TokenMismatch| H["nothing deleted -- live at a newer token, leave the .meta alone"]
```

The `.meta` sidecar carries **no token** — it is a per-hash hint. The exact incarnation token lives
in the condemned sentinel row inside the run, together with the condemn round and two flags,
`delete_pending` and `marker_confirmed`. `GC`'s marker is add-only: `Clean → Condemned` yes, the
reverse never, not even when sparing — only a writer that has already displaced the body may clear
it. Minimum two full rounds separate condemnation from deletion, and `delete_pending` is terminal —
an entry is never un-pended.

## Sharding {#sharding}

`cas_gc_shards` is fixed at first lease acquire and immutable; decoders reject `0`. A blob routes by
the **high** 64 bits of its digest, read big-endian.

The role split is worth internalizing: the **coordinator** — the lease holder — owns discovery,
round visibility, the single global fence, and the generation advance, because a publish into
*one* namespace can protect a blob owned by *any* shard, so these span the whole universe and must
not be sharded. **Reducers** own only their disjoint shard; their run-key namespaces never
collide, so two servers could reduce different shards concurrently and reducer work needs no
lease.

A shard with an empty delta bucket, no orphan-sweep retirement routed to it, and no condemned
entries in the parent summary copies the parent's run references verbatim — zero run I/O, a "pure
carry" (see [phase 9](#phase-9-fold-reduce)). A missing parent summary entry on a non-fresh pool is
`CORRUPTED_DATA`, never silently treated as zero.

## Pruning old objects {#pruning}

- **Current-life ref logs and snapshots** (phase 17) — a log is deletable only when covered by
  both durable fold coverage and a durable live snapshot; snapshots strictly older than the newest
  observed one are deletable. There is no batch delete; it is `HEAD` plus `deleteExact` per key.
- **Generations** (phase 13) — keep the last `cas_gc_snapshot_generations_to_keep` (default 3; `0`
  means keep everything, for forensics). Pruning is wholesale: `LIST` the generation prefix and
  delete everything under it, including deposed-leader debris and attempt-scoped outcome sets. A
  generation still referenced by the live seal is skipped, but the cursor still advances past it —
  leak-freedom then rests on the post-`CAS` hand-off reclaim in phase 14.
- **Manifests** — owner-removed bodies delete in phase 15; never-precommitted bodies go through the
  [orphan-manifest sweep](/antalya/cas/architecture/manifests-and-refs#orphan-sweep) in phase 18.

## What a round costs {#round-cost}

Per **folding** round, with `N` live mounts, `S` ref tables and `S_changed` tables carrying new
logs:

| Operation | Count |
|---|---|
| `LIST cas/ns/stream/` | 1 full enumeration |
| `LIST gc/server-roots/` | 1, plus 1 `GET` per mount |
| `GET` the adopted fold seal | 5 on the fold path (phases 4, 5, 7); phase 9 orphan planning adds one more. See [per-phase backend cost](#per-phase-cost) |
| `GET` ref logs | 1 per new log |
| `GET` manifests | 1 per emitted edge — no manifest-body cache within a round |
| `PUT` run segments | 1 per non-pure-carry shard, plus 1 fold seal |
| `HEAD` blobs | 1 per newly condemned |
| `DELETE` | 1 per `redelete` entry — an entry that graduated in an *earlier* round, not the current one |
| Successful lease `CAS gc/state` | 1 |
| Commit `CAS gc/state` | 1 |

The measured `GET` formula is exact: total `GET`s equal ref-log body `GET`s plus manifest body
`GET`s, i.e. `1 + edges_per_log`. An idle folding round is one `LIST` sweep, `N` heartbeat `GET`s,
one successful lease `CAS`, and one commit `CAS`. A deferred round execution is cheaper still: one
`LIST`, three seal `GET`s, the lease `GET`/`PUT` and the heartbeat floor — no commit `CAS` at all.

The round's work is internally self-regulated: anything a pass cannot finish is carried and retried
by the next round's cursors, never dropped. The internal pacing knobs are deliberately not part of
the user-facing configuration surface.

| Setting | Default | Bounds |
|---|---|---|
| `cas_gc_meta_pool_size` | 16 | bounded pool for condemn-marker writes |

## Per-phase backend cost {#per-phase-cost}

Backend requests each phase issues, by key and operation. These tables track the current
implementation and are the expansion of [what a round costs](#round-cost); the request *shapes* are
stable, exact per-round *counts* and any token-conflict retries are not. `N` is the number of items
the phase acts on without conflicts; `P` is the number of paginated `LIST` requests (up to 1000
keys each).

### Phase 1 — lease {#cost-phase-1}

| Result | `gc/state` `GET` | `gc/hb` `GET` | `gc/state` `CAS` |
|---|---:|---:|---:|
| `Acquire` | 1 | 0 | 1 |
| `Renew` | 1 | 0 | 1 |
| `Follower` | 1 | 1 | 0 |
| `Steal` | 1 | 1 | 1 |

A heartbeat pulse runs outside this phase: one `gc/hb` `GET` and one `CAS`.

### Phase 2 — pre-fold ref drain {#cost-phase-2}

No requests when `snap_generation` is `0`. Otherwise, for `N` removed catalog rows:

| Key | Operation | Requests |
|---|---|---:|
| adopted `fold_seal` | `GET` | 1 |
| `<pool_prefix>/cas/ref_catalog` | `GET` | `N + 1` |
| `<pool_prefix>/gc/state` | `GET` | `2N + 1` (two re-reads bracket every write) |
| `<pool_prefix>/cas/ref_catalog` | `CAS` | `N` |

### Phase 3 — heartbeat floor {#cost-phase-3}

`F` successful fence-outs over `M` mounts found by `P` `LIST` requests:

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/gc/server-roots/` | paginated `LIST` | `P` |
| `<server_root_id>/mount` | `GET` | `M` |
| `<server_root_id>/mount` | `CAS` | `F` |

### Phase 4 — defer decision {#cost-phase-4}

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/cas/ns/stream/` | paginated `LIST` | `P` |
| `<pool_prefix>/cas/ref_catalog` | `GET` | 1 |
| adopted `fold_seal` | `GET` | 2 with an adopted generation, otherwise 1 |

No writes.

### Phase 5 — parent seal read {#cost-phase-5}

| Key | Operation | Requests |
|---|---|---:|
| adopted `fold_seal` | `GET` | 1 |

No writes. The `blob_target_runs[].key` run objects are not read here.

### Phase 6 — fold ref group {#cost-phase-6}

No requests. The keys are already in memory from phase 4.

### Phase 7 — fold seal read {#cost-phase-7}

| Key | Operation | Requests |
|---|---|---:|
| adopted `fold_seal` | `GET` | 2 |

No writes. On the fold path phases 4, 5 and 7 read the adopted seal 5 times in total; when phase 9
runs orphan planning it reads the same key once more.

### Phase 8 — fold ref intake {#cost-phase-8}

| Key | Operation | Requests |
|---|---|---:|
| `<life_id>/_ckpt` | `GET` | one per namespace life in the universe |
| `_log` record up to `committed_through` | `GET` | one per record read; none when the cursor already equals the ceiling |
| `_log` record at an epoch start | `GET` | at least two per crossing, plus one per epoch stepped back and one on a failed crossing |
| manifest body | `GET` | one per folded owner edge |

No writes.

### Phase 9 — fold reduce {#cost-phase-9}

| Key | Operation | Requests |
|---|---|---:|
| referenced parent run segments | streaming `GET` | one per referenced run |
| `<pool_prefix>/blobs/...` | `HEAD` | one per zero-in-degree candidate, plus one peek per carried entry that reached zero again |
| blob `.meta` | `GET` | one per graduation candidate with no in-process marker confirmation |
| new run segments | `PUT` | one per written run |
| `<pool_prefix>/cas/manifests/` | `LIST` | one bounded page, only when orphan planning runs |
| manifest candidate body | `GET` | one per candidate on the page (≤ `manifest_sweep_delete_budget_keys`), only when orphan planning runs |
| `gc/state`, adopted `fold_seal`, catalog | `GET` | one each, only when orphan planning runs |
| `_ckpt` and committed-tail `_log` | `GET` | per namespace on the page, only when orphan planning runs |

Also schedules the async `.meta` condemn-marker writes drained by phase 12.

### Phase 10 — fold seal write {#cost-phase-10}

| Key | Operation | Requests |
|---|---|---:|
| new `fold_seal` | `PUT` | 1, or one byte-compare `GET` on a deterministic replay |

No `CAS`.

### Phase 11 — pending deletes {#cost-phase-11}

| Key | Operation | Requests |
|---|---|---:|
| blob body | `DELETE` | one per `redelete` entry |
| per-shard outcome log | `PUT` | one per shard with settled entries |

Under `suppress_destructive`, `redelete` is empty and nothing is deleted; the outcome logs still run.

### Phase 12 — meta pool wait {#cost-phase-12}

No backend request on the GC thread. Waits on the bounded `meta_pool` (`cas_gc_meta_pool_size`,
default 16).

### Phase 13 — round commit {#cost-phase-13}

| Key | Operation | Requests |
|---|---|---:|
| pruned generation prefixes | `LIST` + wholesale `DELETE` | bounded per round |
| `<pool_prefix>/gc/state` | `CAS` | exactly 1 |

### Phase 14 — handoff reclaim {#cost-phase-14}

One `LIST` plus a wholesale `DELETE` per handed-off generation prefix, within the hand-off's own
budget.

### Phase 15 — manifest deletes {#cost-phase-15}

One `DELETE` per `mf_cleanup` entry. No writes under `suppress_destructive`.

### Phase 16 — namespace cleanup {#cost-phase-16}

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/gc/maintenance_state` | `GET` | 1 (durable `janitor_cursor`) |
| `<pool_prefix>/cas/ns/` | `LIST` | one page |
| `<pool_prefix>/cas/ref_catalog` | `GET` | 1 |
| `<pool_prefix>/gc/state` | `GET` | one per fence check |
| dead-life object | `DELETE` | one per object |
| `<pool_prefix>/gc/maintenance_state` | `CAS` | 1 when the page is decided |

### Phase 17 — ref object cleanup {#cost-phase-17}

| Key | Operation | Requests |
|---|---|---:|
| `_log` / `_snap` candidate | `HEAD` | one per candidate |
| `<pool_prefix>/cas/ref_catalog` and `<pool_prefix>/gc/state` | `GET` | one each per delete (authority re-validation) |
| `_log` / `_snap` key | `DELETE` | one per planned key |

### Phase 18 — orphan sweep {#cost-phase-18}

One `DELETE` per nomination. The planning `LIST` and `GET` cost is paid in phase 9.

## Observability {#observability}

`system.cas_gc_log` emits `Start`, `Finish` and per-`Phase` rows, correlated by `round_id` — not
`round`, which is `0` on `Start` and does not exist at all on a not-a-leader round. Phase rows
carry no verb columns by design: per-phase operation counts ride the row's own `ProfileEvents`
delta, so grouping by phase over an S3 event attributes the LIST/GET/PUT/DELETE budget without
inventing schema. `phase_metrics` carries the semantic counts no counter can supply (clamped
tables, dead precommits skipped, pure-carry shards, generations visited). `Deferred` is kept
distinct from `Success` precisely so "folded and found nothing" is distinguishable from "never
folded". Every `GC`-related `ProfileEvent` carries the uppercase `CAS`/`CASGC` prefix — for example
`CASGCRetiredCondemned`, `CASGCRetiredGraduated`, `CASGCRetiredRedeleted`,
`CASGCClampSuppressedPasses`, `CASGCHeartbeatFenceOuts`.

Alongside it, `system.cas_log` carries the audit trail: the condemn chain, fence-outs, anomalies
(capped per round, each carrying the true total), and manifest deletes.

`ca-fsck` distinguishes two classes that are easy to conflate: `dangling` — referenced but missing,
data loss — versus `unreachable`/`awaiting-gc` — present, unreferenced, and
simply waiting for graduation.

## Operational surface {#operational-surface}

| Command | Effect |
|---|---|
| `SYSTEM CAS GC RUN '<disk>'` | One synchronous round on the contacted node; only the lease holder makes progress |
| `SYSTEM CAS GC STOP` / `SYSTEM CAS GC START` | Stop or resume future rounds on the same scheduler, preserving its identity |
| `SYSTEM CAS GC REBUILD` (`clickhouse-disks ca-gc-rebuild`) | Fail-closed disaster-recovery path that every "GC refuses to run" error points at; deliberately over-protects — it prefers bounded leaks over risking an under-count. It cannot delete live data directly: deletions it produces still flow through the normal round's condemn, graduate, exact-token path |
| `clickhouse-disks ca-gc-dryrun` | Opens the disk read-only, constructs a non-leader `GC`, and prints what would be deleted with a reason per entry. Write-free, resolves runs through the seal's references. Documented caveat: it does not fold new owner events, so away from quiescence it can **over-report** — the subset guarantee holds only at quiescence, and its output must never feed a real delete |

`SYSTEM CAS DROP POOL MEMBER '<server_root_id>' FROM DISK '<disk>'` — permanent removal of a dead
replica, distinct from ordinary `GC` — is covered on the
[mounts-and-leases page](/antalya/cas/architecture/mounts-and-leases#mount-lifecycle). `SYSTEM CAS
FSCK` and its `dangling`/`unreachable` vocabulary are a read-only diagnostic pass, not part of the
`GC` protocol itself.
