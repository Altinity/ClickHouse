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

| # | Phase (`GcPhaseTimer` name) | What it does |
|---|---|---|
| 1 | `lease` | Create, renew, observe or steal the lease inside `gc/state`. The only phase a `NotALeader` round execution emits |
| 2 | `pre_fold_ref_drain` | Resolve catalog `Removing` rows whose cleanup evidence the adopted parent already sealed; exact-CAS-delete the completed ones before defer or new fold work |
| 3 | `heartbeat_floor` | One `LIST` of `gc/server-roots/`, one `GET` per mount slot, fence-out `PUT` for any mount whose write-token has held stable past the threshold |
| 4 | `defer_decision` | One full `LIST` of `cas/ns/stream/`, build the catalog-keyed ref walk plan; decide `DEFER` (nothing changed, no graduation due) or continue to a full fold. A `DEFER` verdict still runs one namespace-janitor page — the same work phase 16 does on a folding round — with its deletes suppressed |
| 5 | `parent_seal_read` | Capture the parent fold seal's run references before the fold mutates the in-memory generation/attempt, to detect a ref that moved off an already-pruned generation |
| 6 | `fold_ref_group` | Regroup the one `LIST` from phase 4 into per-table listings — no I/O, the keys are already in hand |
| 7 | `fold_seal_read` | `GET` and decode the adopted fold seal that anchors this fold's coverage |
| 8 | `fold_ref_intake` | `GET` every new ref-log record and every referenced manifest, extracting blob source edges |
| 9 | `fold_reduce` | The three-cursor merge over prior edges, new deltas and the parent's condemned rows: spare, condemn, graduate or redelete each candidate |
| 10 | `fold_seal_write` | Write the new fold seal once, write-once deterministic, adopting a byte-identical replay instead of rewriting it |
| 11 | `pending_deletes` | The single content-delete site: exact-token `deleteExact` of every entry the *previous* round marked `delete_pending`, plus the forensic outcome-log writes |
| 12 | `meta_pool_wait` | Drain the bounded pool of async `.meta` condemn-marker writes queued during the fold |
| 13 | `round_commit` | Retention-prune old generations, then publish the single `gc/state` `CAS` that adopts the whole round |
| 14 | `handoff_reclaim` | Post-`CAS`: reclaim any generation that a ref moved off during this very round, before the ordinary wholesale prune would reach it |
| 15 | `manifest_deletes` | Delete manifest bodies whose owner-removal minus-one edge the `CAS` in phase 13 just adopted |
| 16 | `namespace_cleanup` | One bounded page of the perpetual namespace janitor, reclaiming dead-life debris |
| 17 | `ref_object_cleanup` | Prune ref logs and snapshots once both fold coverage and a live snapshot make them safe to delete |
| 18 | `orphan_sweep` | Post-`CAS` exact-token deletion for the [orphan-manifest sweep](/antalya/cas/architecture/manifests-and-refs#orphan-sweep), after phase 13 adopted each candidate's exact blob-source retirements and the cursor. Planning retains, counts, logs, and advances past undecodable bodies without wedging later candidates; a decoded identity mismatch during planning or token ABA during deletion still fails the round with `CORRUPTED_DATA` |

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

## Phase 1: lease {#phase-1-lease}

There is **no separate `GC` lease object**. The lease lives inside `gc/state` itself as
`{owner, seq}`.

After decoding, the object has this structure:

```text
<pool_prefix>/gc/state
├── header
│   ├── type = cas_gc_state
│   └── format_version
└── body
    ├── round
    ├── gc_shards
    ├── snap_generation
    ├── snap_pruned_through
    ├── snap_attempt
    ├── manifest_sweep_cursor
    └── lease
        ├── owner
        └── seq
```

Phase 1 changes only `lease` during renew or steal; the other body fields are preserved. The
backend token returned with `gc/state` identifies its exact object version and is not a field in
this structure.

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

### 1.1 Acquire {#phase-1-acquire}

If `gc/state` does not exist and this `Gc` instance has never seen it, phase 1 creates the object.
The new lease has `owner = gc_id` and `seq = 1`; `gc_shards` is also fixed at this point. A
successful create lets the round execution continue. A create conflict causes a bounded re-read.

If the same `Gc` instance has already observed `gc/state` and the object later disappears, phase 1
throws `CORRUPTED_DATA` instead of creating a new history.

### 1.2 Renew {#phase-1-renew}

If `lease.owner` already equals this instance's `gc_id`, phase 1 increments `lease.seq` and replaces
`gc/state` with a token-guarded `CAS`. All non-lease fields stay unchanged. A successful renew lets
the round execution continue; a conflict causes a bounded re-read.

### 1.3 Follower {#phase-1-follower}

If another `GC` owns the lease, phase 1 reads `gc/hb` and normally returns `NotALeader`. When a
scheduled round execution backs off because this is the first observation or because either signal
moved, it records the observed lease and heartbeat in the local `Gc` instance. This observation is
kept only in memory. A new process starts without it, so the first sight of a foreign lease never
causes a steal.

A manual `SYSTEM CAS GC RUN` may acquire a missing lease or renew its own lease. It never steals a
foreign lease and does not record a foreign observation for the background loop.

### 1.4 Steal {#phase-1-steal}

The current leader writes a small `gc/hb` object. It sends one heartbeat immediately after a
successful acquire, renew or steal, before the long work starts. A separate heartbeat loop keeps
advancing it while the scheduler considers itself the leader. The heartbeat is advisory: it shows
activity but does not grant authority to commit a round.

A `Gc` instance that is a candidate to become the leader compares two signals with the values it
recorded during its previous scheduled round execution:

- the `(lease.owner, lease.seq)` pair in `gc/state`;
- the writer and sequence stored in `gc/hb`.

If either signal changed, the current lease owner is treated as active. The candidate records a new
observation and returns as a follower. The heartbeat writer does not have to equal `lease.owner`:
during a handoff, an old heartbeat thread may still write for a short time, and any moving
heartbeat must prevent another steal.

Steal is allowed only when both signals stayed unchanged across two observations made by the paced
background loop. The candidate then replaces `lease.owner` with its own `gc_id`, increments
`lease.seq`, and performs a token-guarded `CAS`. A successful `CAS` lets the round execution
continue. A lost steal `CAS` causes one re-read and returns `NotALeader`; there is no second steal
attempt in the same round execution.

### 1.5 Successful result {#phase-1-result}

Acquire, renew and steal all return the committed `GcState` and the backend token (for example, an
S3 `ETag`) of that exact `gc/state` version. The later `round_commit` uses this token as its expected
token. Any intervening update of `gc/state` therefore rejects the stale leader's commit. The
resulting `lease.seq` also becomes the folding round's attempt id.

### 1.6 Safety after leadership changes {#phase-1-safety}

A deposed leader may keep running, but correctness does not rely on exclusive execution:

1. A folding round is published by exactly **one commit `CAS`** over `gc/state`; a deposed leader's
   commit fails and its candidate generation is not adopted.
2. Every fold artifact is written under that leader's own attempt number, invisible to every
   reader, and reclaimed later by wholesale generation pruning.
3. Destructive pre-`CAS` actions are justified only by previously published durable state, so they
   are replay-idempotent.
4. Deletes are exact-token, so a stale leader can never delete a newer incarnation.

The lease is therefore **work de-duplication, not mutual exclusion**.

### 1.7 Phase costs {#phase-1-costs}

Without token conflicts, phase 1 sends:

| Result | `<pool_prefix>/gc/state` `GET` | `<pool_prefix>/gc/hb` `GET` | `<pool_prefix>/gc/state` conditional `PUT` (`CAS`) | Total |
|---|---:|---:|---:|---:|
| `Acquire` | 1 | 0 | 1 | 2 |
| `Renew` | 1 | 0 | 1 | 2 |
| `Follower` | 1 | 1 | 0 | 2 |
| `Steal` | 1 | 1 | 1 | 3 |

Each acquire or renew token conflict (the conditional `PUT` loses because the `gc/state` token
changed) adds two requests. A lost `Steal` adds one final `gc/state` `GET`, raising its total to four
requests. A heartbeat pulse runs outside this phase and costs one `gc/hb` `GET` and one conditional
`PUT`.

## Phase 2: pre-fold ref drain {#phase-2-pre-fold-ref-drain}

Phase 2 completes namespace removals that were proved safe by the last committed folding round. It
also runs on a round execution that later returns as `Deferred`.

### 2.1 Removal protocol {#phase-2-removal-protocol}

Namespace removal is split across round executions:

During the first, folding round execution:

1. The fold reads a ref-log transaction containing `RemoveNamespace` and writes
   `cleanup_evidence` into its fold seal.
2. `round_commit` makes that fold seal the adopted parent.

During a later leader round execution:

3. Phase 2 uses the adopted evidence to remove the matching `Removing` row from
   `<pool_prefix>/cas/ref_catalog`.
4. Phase 16, `namespace_cleanup`, may delete the old namespace's physical `_log`, `_snap`, `_ckpt`,
   and `_files` objects. Its cleanup is bounded, so some objects may remain for a subsequent round.

The fold that creates `cleanup_evidence` cannot use it immediately. Phase 2 accepts evidence only
from a fold seal already published by an earlier commit `CAS`.

### 2.2 Inputs {#phase-2-inputs}

Phase 1 supplies `snap_generation` and `snap_attempt` from `<pool_prefix>/gc/state`. Together they
identify the adopted parent fold seal.

#### 2.2.1 Adopted parent fold seal {#phase-2-parent-fold-seal}

`<pool_prefix>/gc/gen/<snap_generation>/attempt/<snap_attempt>/fold_seal` describes the last GC
generation published by `round_commit`. It is immutable after publication. Phase 2 reads only the
following part:

```text
<pool_prefix>/gc/gen/<snap_generation>/attempt/<snap_attempt>/fold_seal
└── ref_lives[<life_id>]
    ├── coverage
    │   └── hold                    [optional]
    └── cleanup_evidence            [optional]
        └── remove_txn_id
```

- `ref_lives[<life_id>]` contains the fold state for one namespace incarnation. Its map key is
  compared with `incarnation` in the catalog.
- `coverage.hold`, when present, means the fold could not prove complete coverage. Phase 2 must keep
  the catalog entry.
- `cleanup_evidence` means the fold processed the terminal `RemoveNamespace` transaction for this
  life.
- `cleanup_evidence.remove_txn_id` identifies that terminal ref-log transaction. Phase 2 does not
  read the ref-log again because the adopted seal is the durable evidence.

If `<pool_prefix>/gc/state` points to a missing parent fold seal, phase 2 throws `CORRUPTED_DATA`.

#### 2.2.2 Namespace catalog {#phase-2-ref-catalog}

`<pool_prefix>/cas/ref_catalog` is the durable source of truth for the current incarnation and
lifecycle state of every namespace in the pool. Phase 2 uses these fields:

```text
<pool_prefix>/cas/ref_catalog
└── entries[]
    ├── ns
    ├── state
    ├── incarnation
    └── removal_started_round       [only for Removing]
```

- `ns` is the logical namespace name.
- `state` is `Creating`, `Live`, or `Removing`. Phase 2 considers only `Removing` entries.
- `incarnation` is the unique `life_id` of this namespace life. Recreating the same `ns` produces a
  new value, so evidence for an old life cannot remove the new entry.
- `removal_started_round` records when the entry moved to `Removing` and is required in that state.

An entry is eligible only when it is `Removing`, the adopted parent has `cleanup_evidence` for the
same `incarnation`, and that life has no coverage `hold`. `Creating` and `Live` entries are never
changed by this phase. The catalog's backend token is returned alongside the object; it is not a
catalog field and is used as the precondition when phase 2 replaces the catalog.

### 2.3 No adopted generation {#phase-2-no-adopted-generation}

If the `snap_generation` field of `<pool_prefix>/gc/state` is `0`, no folding round has been
committed yet and there is no adopted parent fold seal. Phase 2 returns successfully without
reading the fold seal or the catalog and without removing anything.

In a new pool, phase 2 is still executed and logged during the first leader round execution, but it
is a no-op. It can remove catalog entries for the first time only during a later leader round
execution after an earlier folding round has committed a generation. This is normally the second
leader round execution, but not necessarily the second call to `Gc::runRegularRound`: a
`NotALeader`, `Deferred`, or failed execution does not create an adopted generation.

### 2.4 Drain {#phase-2-drain}

When an adopted generation exists, phase 2 reads its fold seal and repeatedly processes eligible
catalog entries:

1. Read a complete `<pool_prefix>/cas/ref_catalog` snapshot and its backend token.
2. Select one eligible `Removing` entry.
3. Re-read `<pool_prefix>/gc/state` and verify that `lease.owner` and `lease.seq` still match the
   values obtained in phase 1. A mismatch here skips the write and ends the drain as a lost-authority
   failure.
4. Write a new catalog without that exact entry, using the catalog token as the `CAS` precondition.
5. Re-read the catalog to determine the durable result, and re-read `<pool_prefix>/gc/state` once
   more to confirm leadership was still held across the write. Continue until no eligible entry
   remains; after the eligible set empties, re-read `<pool_prefix>/gc/state` a final time before
   declaring the drain complete.

The two `<pool_prefix>/gc/state` re-reads around every write are why phase 2 issues `2N + 1` state
`GET`s for `N` removed rows (see the [phase costs](#phase-2-costs) below). The catalog row is
removed by replacing the whole catalog with `CAS`; this is not a backend `DELETE`. The mandatory
catalog re-read distinguishes a completed removal, replacement by a new incarnation, and a token
conflict that still left the old entry present.

### 2.5 Result {#phase-2-result}

On success, the final catalog snapshot contains no `Removing` entry that the adopted parent fold
seal already proved complete. Phase 2 invalidates any local runtime for each confirmed old
`(ns, incarnation)` and records the number of catalog rows it removed.

If leadership changes during the drain, phase 2 stops and the round execution fails with a
retry-later exception. A catalog update already sent by the old leader is still safe: it was
authorized by the previously adopted fold seal and guarded by the exact catalog token.

Phase 2 does not change `<pool_prefix>/gc/state` or the parent fold seal, create a new generation,
read ref-log bodies, or physically delete namespace data, manifests, or blobs. Its result is a
barrier: phase 3 starts only after all catalog removals already authorized by the parent have been
resolved.

### 2.6 Phase costs {#phase-2-costs}

If `snap_generation` is `0`, phase 2 sends no requests to the storage backend.

Otherwise, let `N` be the number of eligible catalog rows removed without conflicts. Phase 2 sends:

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/gc/gen/<generation>/attempt/<attempt>/fold_seal` | `GET` | 1 |
| `<pool_prefix>/cas/ref_catalog` | `GET` | `N + 1` |
| `<pool_prefix>/gc/state` | `GET` | `2N + 1` |
| `<pool_prefix>/cas/ref_catalog` | conditional `PUT` (`CAS`) | `N` |

The total is `4N + 3` backend requests: `3N + 3` reads and `N` writes. Each catalog token conflict
(the conditional `PUT` loses because the catalog token changed, but the old row remains) adds four
requests.

## Phase 3: heartbeat floor {#phase-3-heartbeat-floor}

Phase 3 checks whether each mounted writer is still active and fences writer incarnations that
stopped renewing their mount lease. It runs on both the deferred and folding paths.

Despite its name, this phase does not read or write `<pool_prefix>/gc/hb`. That object tracks the
GC leader and belongs to phase 1. Phase 3 observes the backend token of each writer's `mount`
object.

### 3.1 Mount objects {#phase-3-mount-objects}

Each server root has three control-plane objects:

```text
<pool_prefix>/gc/server-roots/
└── <server_root_id>/
    ├── owner
    ├── epoch
    └── mount
```

Phase 3 lists the whole `server-roots` subtree, but reads only keys ending in `/mount`. It does not
read or change `owner` or `epoch`.

The decoded mount object has this structure:

```text
<pool_prefix>/gc/server-roots/<server_root_id>/mount
├── header
│   ├── type = cas_mount_lease
│   └── format_version
└── body
    ├── server_uuid
    ├── writer_epoch
    ├── hostname
    ├── pid
    ├── started_at_ms
    ├── seq
    ├── expires_at_ms
    ├── min_active
    ├── gc_fenced
    └── write_attempt_id
```

The phase uses the backend token to detect renewals, `gc_fenced` to detect an earlier fence-out,
`min_active = UINT64_MAX` to detect a clean farewell, and `seq` to write `seq + 1` during a
fence-out. All other body fields are preserved. In particular, `expires_at_ms` is not evidence that
the writer stopped: it was produced from another process's wall clock.

### 3.2 Observation window {#phase-3-observation-window}

The `Gc` instance keeps an in-memory observation for each `server_root_id`:

```text
server_root_id -> (last backend token, first observation time)
```

The time comes from the local monotonic clock. A new token starts a new observation window. A new
`Gc` instance starts with an empty map, so it can delay a fence-out but cannot perform one too early.
Observations for terminal or no longer listed mounts are removed.

The token must stay unchanged for this threshold:

```text
mount_lease_ttl_ms + 5% of mount_lease_ttl_ms + mount_renew_period
```

### 3.3 Classification {#phase-3-classification}

Each listed mount has one result:

| Condition | Result |
|---|---|
| The object disappeared after `LIST` | Skip it |
| `gc_fenced = true` | `already_fenced` |
| `min_active = UINT64_MAX` | `terminated` |
| The token is new or changed | `live`; start a new observation window |
| The token is unchanged for less than the threshold | `live` |
| The token is unchanged for the full threshold | Try to fence it out |

The first observation is always `live`, even when `expires_at_ms` is in the past. These results are
local counters; they do not change the defer decision or allow blob graduation.

### 3.4 Fence-out {#phase-3-fence-out}

For an eligible mount, phase 3 preserves its body, sets `gc_fenced = true`, increments `seq`, and
performs a token-guarded conditional `PUT`. A successful write changes the backend token, so the
old writer cannot renew with its previous token. It must remount with a new writer epoch before it
can write again.

If the conditional `PUT` loses because the writer renewed the mount, phase 3 re-reads the object and
classifies it again. A changed token starts a new observation window and produces `live`. Retries
are limited to four conflicts; continued token movement is treated as `live`.

The phase does not recheck the GC lease while scanning. This is safe because the decision uses a
full local token-stability window and the write is guarded by that exact token. A completed
fence-out remains valid even if this GC later loses leadership or does not commit a folding round.

### 3.5 Result {#phase-3-result}

The result records `live`, `terminated`, `fenced_now`, and `already_fenced`, plus the
`server_root_id` of each mount fenced by this execution. These values are used for metrics and
events; phase 4 receives no direct input from them.

Phase 3 does not delete mount objects or user data. It may write `gc_fenced = true` even when the
round later returns as `Deferred` or destructive GC work is suppressed.

### 3.6 Phase costs {#phase-3-costs}

The phase enumerates the subtree to find the `/mount` object of every known `server_root_id`. Let
`P` be the number of backend `LIST` requests used by this enumeration, `M` the number of `/mount`
keys found, `F` the number of successful fence-outs, and `C` the number of fence-out token
conflicts. Each `LIST` response contains up to 1000 keys. A normal server root has three keys
(`owner`, `epoch`, and `mount`), so with `R` complete server roots, `P` is usually
`ceil(3R / 1000)`, while `M = R`. A completed phase sends:

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/gc/server-roots/` | paginated `LIST` | `P` |
| `<pool_prefix>/gc/server-roots/<server_root_id>/mount` | `GET` | `M + C` |
| `<pool_prefix>/gc/server-roots/<server_root_id>/mount` | conditional `PUT` | `F + C` |

The total is `P + M + F + 2C` backend requests. Each token conflict (the writer changes the mount
token before the fence-out `PUT`) adds one `PUT` and one re-read.

## Phase 4: defer decision {#phase-4-defer-decision}

Phase 4 builds one in-memory plan of ref-stream work and decides whether this execution needs a
full fold:

- `defer` skips generation construction and the round commit;
- `fold` continues with phase 5 and builds a new in-degree snapshot.

The phase only reads the backend. It does not create a generation, change `gc/state`, or delete
objects.

### 4.1 Inputs {#phase-4-inputs}

#### 4.1.1 Ref stream {#phase-4-ref-stream}

Phase 4 fully enumerates this subtree:

```text
<pool_prefix>/cas/ns/stream/
└── <life_id>/
    ├── _log/
    │   └── <writer_epoch>-<ref_sequence>.zst
    └── _snap/
        └── <writer_epoch>-<ref_sequence>.zst
```

It reads only key names, not the bodies of `_log` or `_snap` objects. For each `<life_id>`, it finds
the greatest transaction id in `_log`. A `_snap` key helps describe the listed namespace life, but
does not by itself count as new fold work.

#### 4.1.2 Namespace catalog {#phase-4-ref-catalog}

After the full `LIST`, phase 4 reads `<pool_prefix>/cas/ref_catalog`. `Live` and `Removing` rows are
included in the plan; `Creating` rows are excluded. A listed `<life_id>` that is absent from this
later catalog snapshot is dead-life debris and does not force a fold.

#### 4.1.3 Adopted fold seal {#phase-4-fold-seal}

With an adopted generation, phase 4 reads its `fold_seal` twice:

```text
<pool_prefix>/gc/gen/<snap_generation>/attempt/<snap_attempt>/fold_seal
```

The first read checks `condemned_summary` to decide whether graduation work is due. The second read
loads `ref_lives`, including the last folded ref position for each namespace life. The two reads are
not shared.

Phase 4 also reads the local `rounds_since_last_fold_` counter and the `gc_fold_threshold` and
`gc_fold_max_defer_rounds` settings. The `gc_stuck_removal_rounds` setting controls only a warning
for a `Removing` row that has not produced cleanup evidence; it does not affect the decision.

### 4.2 Walk plan {#phase-4-walk-plan}

The in-memory plan has one row for each admitted catalog life. Each row joins:

- its `Live` or `Removing` catalog entry;
- its last folded ref position from the adopted seal;
- its greatest listed `_log` position;
- the listed `_log` and `_snap` keys needed by later phases.

A row is changed when its greatest listed `_log` position is newer than its last folded position.
The `changed_shards` metric counts these changed plan rows, not GC shards and not individual log
objects.

On the folding path, phase 6 reuses this plan and the same `LIST` result. It does not enumerate the
ref stream again.

### 4.3 Decision {#phase-4-decision}

Phase 4 chooses `fold` when any of these conditions is true:

| Condition | Why a fold is needed |
|---|---|
| Changed plan rows are at least `gc_fold_threshold` | Apply accumulated ref-log work |
| An adopted shard has `pending_total > 0` | Let phase 11 process a published deletion |
| An adopted shard has `oldest_nonpending_condemn_round < state.round + 1` | Advance a condemned blob to `delete_pending` |
| `rounds_since_last_fold_ >= gc_fold_max_defer_rounds` | Bound consecutive deferred executions |

Only when none of these conditions is true does it choose `defer`. The defaults are
`gc_fold_threshold = 1` and `gc_fold_max_defer_rounds = 8`. A missing, invalid, or incomplete
adopted seal cannot produce a quiet defer: the graduation check refuses to defer, and the
plan-building read reports the invalid state.

### 4.4 Result {#phase-4-result}

On `defer`, the phase increments `rounds_since_last_fold_`. After the phase timer ends, one
suppressed page of phase 16, `namespace_cleanup`, runs and the execution returns. It creates no new
generation and performs no commit `CAS`; the already adopted round stays unchanged.

On `fold`, the phase resets `rounds_since_last_fold_` and keeps the walk plan for later phases. A
failure after this decision does not restore the local counter.

Phase 4 does not recheck the GC lease. If leadership changed, a stale plan remains local and a
later round commit is rejected by the `gc/state` token.

### 4.5 Phase costs {#phase-4-costs}

Let `P` be the number of backend `LIST` requests needed to enumerate
`<pool_prefix>/cas/ns/stream/`. Each response contains up to 1000 keys.

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/cas/ns/stream/` | paginated `LIST` | `P` |
| `<pool_prefix>/cas/ref_catalog` | `GET` | 1 |
| Adopted `fold_seal` | `GET` | 2 with an adopted generation; otherwise 1 |

The total is `P + 3` backend requests with an adopted generation and `P + 2` on a fresh pool. The
plan builder still probes the generation-zero `fold_seal` key on a fresh pool and accepts its
absence; the separate graduation check skips that read. The phase sends no backend writes.

## Phase 5: parent seal read {#phase-5-parent-seal-read}

Phase 5 reads the fold seal adopted before the current fold and copies its `blob_target_runs` into
memory. It runs only when phase 4 selected `fold`.

Here, parent means the previously adopted GC state on which the new state is based. It is not a
parent object in the backend key hierarchy. Deferred executions may have happened since this seal
was committed.

### 5.1 Input {#phase-5-input}

The exact seal is selected by `snap_generation` and `snap_attempt` from the `gc/state` version read
in phase 1:

```text
<pool_prefix>/gc/gen/<snap_generation>/attempt/<snap_attempt>/fold_seal
└── body
    └── blob_target_runs[]
        ├── key
        ├── checksum
        ├── shard
        └── generation
```

Each entry names one run segment containing part of the adopted in-degree snapshot. Its `key`
normally has this form:

```text
<pool_prefix>/gc/gen/<generation>/attempt/<attempt>/blob_target/<shard>/<seq>
```

Phase 5 copies the four reference fields but does not read or validate the run objects themselves.
It performs its own seal `GET`; it does not reuse either seal read from phase 4.

### 5.2 Why it runs before the fold {#phase-5-before-fold}

An adopted seal can reference a run stored under an older generation. When a shard has no new
work, a later fold can reuse that run without rewriting it. The adopted seal generation and the
physical generation of its runs therefore do not have to match.

The current fold may replace such a run and stop referencing its old generation. Reading only the
new seal would then lose two facts: which generation still belongs to the adopted state if the
commit fails, and which generation became unused if the commit succeeds. Phase 5 preserves the old
side of that comparison before the new seal is built.

### 5.3 Later use {#phase-5-later-use}

The saved list is used in two later phases:

1. Before the commit `CAS`, phase 13 protects generations referenced by either the parent seal or
   the new seal from retention cleanup. If the commit loses, the old adopted state therefore still
   has all its runs.
2. After a successful commit, phase 14 compares the parent and new references. It can reclaim an
   old generation when the parent referenced it, the new seal no longer does, and the generation is
   already behind `snap_pruned_through`, subject to destructive suppression and the phase 14 budget.

Phases 6 through 12 do not use this list. Phase 5 itself does not prune or delete anything.

### 5.4 Result {#phase-5-result}

The result is the local `parent_seal_runs` list. On a fresh pool, the generation-zero seal is absent
and the list is empty.

An invalid seal or a backend read failure stops the execution. If an adopted seal disappears after
phase 4, phase 5 gets an empty list; phase 7 checks the adopted seal again and reports
`CORRUPTED_DATA` before the commit.

Phase 5 does not recheck the GC lease. A stale leader can keep the list in memory, but its later
commit is still guarded by the phase 1 `gc/state` token.

### 5.5 Phase costs {#phase-5-costs}

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/gc/gen/<snap_generation>/attempt/<snap_attempt>/fold_seal` | `GET` | 1 |

The phase sends exactly one backend read and no writes. It does not send a `GET` for any
`blob_target_runs[].key`.

## Phase 6: fold ref group {#phase-6-fold-ref-group}

Phases 6 through 10 build the new generation. They run inside `Gc::fold` and only on the folding
path.

Phase 6 does no backend I/O. It regroups the single `LIST` from phase 4 into per-life listings and
records the catalog cut and its derived facts on the fold result.

### 6.1 Regrouping {#phase-6-regrouping}

`groupRefKeys` turns the flat key list into physical tables keyed by `<life_id>`. Each table is
resolved against the phase 4 catalog snapshot:

- a `<life_id>` absent from the snapshot is dead-life debris and is dropped;
- a table is kept only when the catalog's admitted incarnation for that namespace equals its
  `<life_id>`.

The result is `ref_tables`: one `RefTableListing` of `_log` and `_snap` keys per admitted
namespace. This is what makes the fold catalog-authoritative — the catalog decides which namespaces
exist, and the `LIST` is only a per-namespace hint.

### 6.2 Catalog cut {#phase-6-catalog-cut}

The phase stores the full catalog snapshot on the result and computes `catalog_cut_proved_empty`:
true only when the snapshot carries a backend token and holds no entries of any lifecycle state,
including `Creating`. This is a positive empty-universe proof used by the phase 9 destructive gate.

### 6.3 Malformed keys {#phase-6-malformed-keys}

A ref-object key under the stream prefix whose shape `groupRefKeys` cannot parse — a missing
life/kind/id segment, an unrecognized kind directory, a missing `.zst` suffix, or trailing garbage —
makes it throw. Phase 6 catches it, sets
`ref_folding_aborted`, and records an anomaly. It is not re-raised: the round then produces no ref
delta, advances no cursor, and authorizes no destructive work. The anomaly turns on
`suppress_destructive` for the whole round.

The abort does not stop phase 8 from reading every `_ckpt`; only the walk itself is skipped.

### 6.4 Result {#phase-6-result}

The outputs are `ref_tables`, `root_shards` (one per admitted namespace), the stored `catalog_cut`,
`catalog_cut_proved_empty`, and `ref_folding_aborted`. Metrics: `ref_keys_listed`,
`namespaces_seen`, `ref_folding_aborted`.

### 6.5 Phase costs {#phase-6-costs}

The phase sends no backend requests. The keys are already in memory from phase 4.

## Phase 7: fold seal read {#phase-7-fold-seal-read}

Phase 7 reads the adopted fold seal that anchors this fold's coverage and prepares the fold's base
inputs.

### 7.1 Two reads of one key {#phase-7-two-reads}

The adopted seal is read twice at the same address:

```text
<pool_prefix>/gc/gen/<snap_generation>/attempt/<snap_attempt>/fold_seal
```

The first read anchors coverage. If the object is absent while `snap_generation > 0`, the phase
reports `CORRUPTED_DATA`: `gc/state` points at a missing adopted artifact, and the fix is
`SYSTEM CAS GC REBUILD`. The second read loads the parent run references. It returns the same
generation, the same attempt, and the same bytes; nothing between the two reads touches the backend.
On a folding round this is the fourth and fifth `GET` of this one key — phase 4 reads it twice and
phase 5 once. When orphan-sweep planning runs in phase 9, it reads the same key a sixth time. The
redundancy is measured, not yet removed.

### 7.2 Base inputs {#phase-7-base-inputs}

The phase builds, in memory:

- `parent_ref_lives` — a constant copy of the coverage and holds from the walk plan, the
  unchanged prior view while the successor seal earns changes later in the fold;
- the mutable successor `ref_lives` the fold will extend;
- `condemn_round = state.round + 1`;
- `new_generation = snap_generation + 1`;
- `attempt = lease.seq` — every fold artifact write below lands under this attempt, while the
  parent-generation reads keep using `snap_attempt`.

It also defines three functions that phase 9 calls: one `HEAD`-observe per new zero-in-degree
candidate (records the exact incarnation token, schedules the `.meta` condemn marker), a
side-effect-free `HEAD` peek, and the graduation-gate marker check.

### 7.3 Result {#phase-7-result}

Metrics: `seal_reads` (2), `redundant_reads` (1), `parent_ref_lives`, `dropped_parent_ref_lives`,
`parent_runs`, `parent_cleanup_evidence`.

### 7.4 Phase costs {#phase-7-costs}

| Key | Operation | Requests |
|---|---|---:|
| Adopted `fold_seal` | `GET` | 2 |

The phase sends no writes. On a fresh pool both reads return nothing and the fold starts from an
empty baseline.

## Phase 8: fold ref intake {#phase-8-fold-ref-intake}

Phase 8 reads the new ref-log records of every walkable namespace life and the manifests they
reference, extracting blob source edges. It is the heaviest read phase of a folding round.

### 8.1 Checkpoints {#phase-8-checkpoints}

`readCheckpointWitnesses` reads one checkpoint per namespace life in the round's universe:

```text
<pool_prefix>/cas/ns/state/<life_id>/_ckpt
```

It supplies `committed_through` — an inclusive ceiling, snapshotted once and never re-read within
the round — plus `life_epoch`, the genesis position for a never-folded life, and the set of
undecodable checkpoints. An absent `_ckpt` is normal and is not a witness.

The checkpoint is then grounded through `chooseRecoveryGrounding`. A `Live` or `Removing` life
whose `_ckpt` is undecodable, absent, or lacks `life_epoch` has no usable grounding: the namespace
folds nothing this round. If it has a sealed cursor, it is held at `cursor + 1` with reason
`CheckpointUndecodable`; if it has none, only an anomaly is recorded. Either way the namespace is
unproven (`CheckpointUnusable`).

### 8.2 The round's universe {#phase-8-universe}

The universe is every `Live` or `Removing` row of the frozen catalog cut, and nothing else. The
`LIST` from phase 4 is a hint. It cannot shrink the universe — a namespace the store goes quiet
about keeps its catalog row and its obligation — and it cannot grow it — a physical id the catalog
does not name is inert debris.

A namespace the hint does not mention, with no carried hold and no `_ckpt` on record, is walked
only while `gc_frontier_probe_budget` lasts. Once the budget is spent, the remaining such
namespaces are not walked at all: their cursors ride verbatim, they count toward
`frontier_namespaces` but not toward `frontier_proven`, and the round is suppressed
(`frontier_unprobed_budget`). Held namespaces are always walked; the `_ckpt` read of phase 8.1 is
paid for every namespace regardless of the budget.

### 8.3 The walk {#phase-8-walk}

For each namespace, the first position is arithmetic: `cursor + 1` when a sealed cursor exists,
otherwise `{life_epoch, 1}` from the checkpoint. The hint never chooses a start. Phase 8 then reads
the exact key:

```text
<pool_prefix>/cas/ns/stream/<life_id>/_log/<writer_epoch>-<ref_sequence>.zst
```

`committed_through` is both the work-set ceiling and the frontier proof. Before every read the walk
compares the expected position with the ceiling: a position above it is never read, so the round
folds a fixed amount of work whatever a concurrent writer appends meanwhile. The namespace is
proven when the walk stops exactly at the ceiling — the cursor already equals `committed_through`
(no read at all), or the last folded record is `committed_through`. A checkpoint with no
`committed_through` proves a namespace with no sealed cursor for free; the same checkpoint with a
nonzero cursor is an anomaly (`CheckpointFrontierEmpty`). A cursor that is already above the
ceiling is an anomaly too (`CommittedBelowCursor`).

An absent record at or below the ceiling is never a frontier: the record is committed and owes an
answer, so the namespace is held (`GapBelowWitness`). The witness set — the hint, the checkpoint,
`committed_through`, and a carried hold's position — only decides whether the absence is a same-epoch
gap or an epoch crossing. An epoch is crossed only over a consumed `EpochSeal`: the walk chains
backward from the witness through `prev_epoch_seal`, one `GET` per epoch stepped, until it lands on
the seal below the sealed cursor or gives up. A proven crossing's epoch-start record is then read
again, at its normal position in the walk, to actually fold it — so the cheapest crossing still pays
two `GET`s of that one record. A crossing with no consumed seal behind it holds
(`UnconsumedSealCrossing`), and one that resolves back to the absent position holds
(`WitnessDisappeared`).

Every exit other than reaching the ceiling leaves the namespace unproven: a hold this round, a
carried hold, an unusable checkpoint, or the probe budget.

Two shapes fail the round closed with `CORRUPTED_DATA` inside this phase: a table with no sealed
cursor whose logs at or below its newest `_snap` are already gone (a baseline lost after cleanup),
and a sealed cursor that does not close the contiguous run the walk produced.

### 8.4 Manifests {#phase-8-manifests}

Each record's explicit owner changes fold through `foldManifestEdges`, one `GET` per edge:

```text
<pool_prefix>/cas/manifests/<ns>/<epoch-hex>-<build-seq-hex>/<NNNNNN>.zst
```

A body whose ref or namespace does not match its key is `CORRUPTED_DATA`. Each blob entry appends a
`BlobDelta` to the round buffer. A `-1` (owner-removed) edge also records `(manifest_id, token)`
for the phase 15 exact-token body delete. A missing body holds the namespace below that record
(`ManifestBodyMissing`) and the record is re-read next round, whether the owner was committed or a
precommit. Two exceptions: a `-1` precommit edge whose body never existed emits nothing and is
skipped, and a `+1` precommit whose build is provably dead by the watermark floor is skipped as a
non-activating edge (`dead_precommits_skipped`).

### 8.5 Atomicity and cursor {#phase-8-atomicity}

A transaction applies atomically. Each record's blob deltas and owner-removed cleanup are staged in
per-record buffers and merged into the round buffers only when the whole record folds. A mid-record
hold discards the staged buffers and leaves the cursor below that record. The durable cursor
advances at one site, once per fully folded record.

A hold clears by exactly one event: a later walk folding through the offending position. A hold
this round replaces the carried one; a walk that stops below a carried hold's position re-seals it
verbatim with `retry_count + 1`. A held namespace is unproven by definition.

Per-namespace failures stay per-namespace — a hold, never a whole-round abort. The only
whole-round abort is phase 6's malformed key.

### 8.6 Result {#phase-8-result}

The outputs are the `deltas` buffer, the updated successor `ref_lives` coverage and holds, the
`mf_cleanup` map for phase 15, the `checkpoints` map for phase 17's delete boundaries, and
`new_removals` — fully folded `RemoveNamespace` transactions whose cleanup evidence is written into
the seal rows between this phase's timer and phase 9's. A removal whose namespace has no row in
the catalog cut or no admitted ref-life row is `CORRUPTED_DATA`.

The phase records many metrics. The load-bearing ones are `frontier_namespaces` and
`frontier_proven` (the universe and the proven part of it), `tables_held`, and the pair
`logs_accounted` / `logs_applied` — a control-flow identity that fails the round closed on a
mismatch.

### 8.7 Phase costs {#phase-8-costs}

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/cas/ns/state/<life_id>/_ckpt` | `GET` | one per namespace life in the universe, budget or not |
| `_log` record from the first position up to `committed_through` | `GET` | one per record read; none for a namespace whose cursor already equals the ceiling |
| `_log` record at an epoch start | `GET` | at least two per crossing (chain validation, then the ordinary fold read), plus one per epoch stepped back and one on a failed crossing |
| Manifest body | `GET` | one per folded owner edge |

An absent read (`absent_probes`) is a hold, not a routine per-namespace probe. The phase sends no
writes.

## Phase 9: fold reduce {#phase-9-fold-reduce}

Phase 9 recomputes the in-degree snapshot per shard and computes the round's single destructive
gate.

### 9.1 The destructive gate {#phase-9-gate}

`suppress_destructive` is computed once, here, from three independent terms:

- a recorded anomaly;
- any hold in the seal the phase is about to make durable, detected this round or carried;
- an incomplete frontier — under an authoritative universe policy, `frontier_proven` must equal
  `frontier_namespaces`, and the universe must be either non-empty or proved empty by
  `catalog_cut_proved_empty`.

It is read at every destructive site of the round. Under suppression the round still condemns,
spares, and carries; graduation, blob redelete, orphan-sweep planning, retention prune, and
post-`CAS` deletes do not run.

Independently of the gate, `GcRoundWorkBudget` caps how many entries graduate and how many
redeletes are handed to phase 11 in one round. An entry past the cap is carried unchanged and
re-evaluated next round.

### 9.2 The merge {#phase-9-merge}

For each shard, phase 9 folds four inputs: the prior source edges streamed from the parent
generation's run segments, the new `BlobDelta`s, the parent's condemned rows (which ride the prior
run as sentinel rows), and the source-edge retirements nominated by orphan-sweep planning (9.4). A
shard is a pure carry when three of them are absent: no delta in its bucket, no orphan-sweep
retirement in its bucket, and the parent's `condemned_summary.condemned_total == 0` — the prior
run's surviving edges are then copied without being read. The new seal copies the parent's run
references and summary verbatim, with no run I/O. Otherwise the merge runs, with one `HEAD` per
zero-in-degree candidate, and writes a new run segment:

```text
<pool_prefix>/gc/gen/<new_generation>/attempt/<attempt>/blob_target/<shard>/<seq>
```

through `putDeterministicArtifact`. Each shard's `condemned_summary` is distilled from its surviving
rows so the next round's decisions read only the seal.

### 9.3 Candidate outcomes {#phase-9-outcomes}

| Outcome | Meaning |
|---|---|
| `spare` | In-degree recovered; the entry is dropped from the retired set |
| `condemn` | New zero-in-degree candidate; the `HEAD` confirmed the body still exists |
| `supersede` | A carried entry whose current token differs from the retired one: republication replaced the incarnation, and a fresh condemn of the current token replaces the stale entry (`blob_retire_replaced`) |
| `graduate` | A prior-condemned entry passed the round-paced floor and has confirmed `Condemned` marker evidence; published `delete_pending`, deleted by phase 11 of a later round. Skipped under suppression or once the graduation budget is spent |
| `redelete` | Already `delete_pending` in the prior list; phase 11 deletes it before this round's `CAS`. Carried unchanged under suppression or once the redelete budget is spent |
| `carry` | Everything else: not yet past the floor, or no marker evidence |

### 9.4 Orphan-sweep planning {#phase-9-orphan-planning}

When the round is not suppressed and `manifest_sweep_list_budget_keys > 0`, phase 9 also plans one
bounded page of the orphan-manifest sweep from `state.manifest_sweep_cursor`. The deletes belong to
phase 18; the plan, the exact source-edge retirements, and the cursor are adopted by phase 13's
`CAS`.

`planManifestCursorPage` is not a single `LIST`. It freezes every candidate's bytes with one `GET`
each, up to `manifest_sweep_delete_budget_keys`, re-reads `gc/state` and the adopted fold seal,
takes its own catalog cut, and for every namespace on the page reads that namespace's `_ckpt` and
walks its committed tail through `activeManifestKeys`. See 9.7.

### 9.5 Fail-closed checks {#phase-9-fail-closed}

Two conditions abort the round with `CORRUPTED_DATA`. They are thrown after the phase 9 timer
closes and before the phase 10 seal write, and therefore long before the commit `CAS`:

- a folded transaction whose deltas never reached a shard reducer — the round lost a durable
  record it had already read;
- a sealed cursor count that disagrees with the walk — a cursor advanced past a record the round
  never applied.

### 9.6 Result {#phase-9-result}

The outputs are the new `blob_target_runs`, `condemned_summary`, `retired_merge` (with `redelete`
and `graduated` for phase 11), `suppress_destructive`, `frontier_complete`, and the planned
`orphan_sweep`. Metrics: `shards_total`, `shards_pure_carry`, `shards_reduced`, `deltas_in`,
`runs_written`, `condemned`, `graduated`, `spared`, `redelete_pending`, `unmatched_removes`,
`suppress_destructive`, `frontier_complete`, `transactions_unapplied`.

### 9.7 Phase costs {#phase-9-costs}

| Key | Operation | Requests |
|---|---|---:|
| Referenced parent run segments | streaming `GET` | one per referenced run |
| `<pool_prefix>/blobs/...` | `HEAD` | one per zero-in-degree candidate, plus one peek per carried entry that reached zero again |
| Blob `.meta` | `GET` | one per graduation candidate with no in-process marker confirmation |
| New run segments | `PUT` | one per written run |
| `<pool_prefix>/cas/manifests/` | `LIST` | one bounded page, only when orphan planning runs |
| Manifest candidate body | `GET` | one per candidate on the page, up to `manifest_sweep_delete_budget_keys` |
| `gc/state`, adopted `fold_seal`, catalog | `GET` | one each, only when orphan planning runs |
| `_ckpt` and committed-tail `_log` records | `GET` | per namespace on the page, only when orphan planning runs |

The phase also schedules the round's async `.meta` condemn-marker writes — one per new condemn,
and one retry per graduation candidate whose marker is not confirmed; phase 12 drains them.

## Phase 10: fold seal write {#phase-10-fold-seal-write}

Phase 10 validates, encodes, and writes the new fold seal with one write-once `PUT`:

```text
<pool_prefix>/gc/gen/<new_generation>/attempt/<attempt>/fold_seal
```

The seal is deterministic — the same fold inputs produce byte-identical bytes — so it goes through
`putDeterministicArtifact`. A byte-equal occupant is this leader's own crash or replay and is
adopted with no rewrite; divergent bytes are impossible under correct operation and fail closed
with `CORRUPTED_DATA`. A deposed leader writes under its own unadopted attempt, so it never
collides with the adopted seal.

The seal's existence marks the fold complete, but the fold performs no `CAS` of its own. After the
phase timer ends, `snap_generation` and `snap_attempt` are set in memory to `new_generation` and
`attempt`; phase 13's commit `CAS` is what makes that durable.

### 10.1 Result {#phase-10-result}

Metrics: `seal_bytes`, `seal_runs`, `seal_ref_lives`, `seal_cleanup_evidence`.

### 10.2 Phase costs {#phase-10-costs}

| Key | Operation | Requests |
|---|---|---:|
| New `fold_seal` | `PUT` | 1, or one byte-compare `GET` on a deterministic replay |

The phase sends no `CAS`.

## Phase 11: pending deletes {#phase-11-pending-deletes}

Phase 11 is the round's single content-delete site. It runs before the commit `CAS`. It executes
the exact-token blob deletes for entries a previous round published as `delete_pending`, and writes
the forensic outcome logs.

### 11.1 What it deletes {#phase-11-what-it-deletes}

For each shard, the `redelete` entries from phase 9 are deleted by exact incarnation token:

```text
deleteExact(<pool_prefix>/blobs/<algo>/<hex-prefix>/<hex>, token)
```

`NotFound` and `TokenMismatch` are tolerated — a `TokenMismatch` means a writer recreated the
incarnation, which is a live object. A backend delete marker in the response is a `LOGICAL_ERROR`:
object versioning is enabled on a mis-provisioned pool.

### 11.2 Non-destructive bookkeeping {#phase-11-bookkeeping}

The same per-shard loop also settles the other merge outcomes, which are not destructive and run
even under suppression: `spared` (in-degree recovered, entry dropped), `graduated` (published
`delete_pending`, deleted by phase 11 of a later round), and `replaced` (a republication superseded
a stale entry and re-condemned the current token).

### 11.3 Outcome logs {#phase-11-outcome-logs}

Per shard, one write-once outcome log:

```text
<pool_prefix>/gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>.zst
```

written with `putIfAbsent` and byte-adopt. The `RoundReport` deletion counters are tallied from the
final durable logs, not from the local decisions.

### 11.4 Authorization {#phase-11-authorization}

An entry is deletable only because a previously committed fold seal published it `delete_pending`.
That is durable state from an earlier commit, so the delete is safe at any leader staleness: the
exact token means a stale leader cannot delete a fresh incarnation. The outcome of this round's own
commit `CAS` does not matter to the delete's safety.

### 11.5 Result {#phase-11-result}

Metrics: `redeleted`, `graduated`, `deleted`, `absent`, `replaced`, `spared`,
`outcome_logs_written`.

### 11.6 Phase costs {#phase-11-costs}

| Key | Operation | Requests |
|---|---|---:|
| Blob body | `DELETE` | one per `redelete` entry, plus one `HEAD` on the token-mismatch quirk path |
| Per-shard outcome log | `PUT` | one per shard with settled entries, plus one `GET` on a write-once conflict |

Under `suppress_destructive`, `redelete` is empty by construction and the phase deletes nothing; the
bookkeeping and outcome logs still run.

## Phase 12: meta pool wait {#phase-12-meta-pool-wait}

Phase 12 is a durability barrier. It drains the round's batch of async per-hash `.meta` writes
before the retired-list publish and the commit `CAS`.

### 12.1 The jobs {#phase-12-jobs}

The batch holds the `Condemned` marker writes scheduled by phase 9 — one per new zero-in-degree
candidate — and by phase 11 — one per `replaced` — plus the per-hash meta deletes phase 11 queues
for confirmed-deleted or absent bodies. The jobs run on the `meta_pool` threads, so this phase
row's `ProfileEvents` map is empty by construction. The row carries job counts instead:
`jobs_scheduled`, `jobs_completed_on_entry` (sampled before the wait), and `jobs_completed`.

### 12.2 Why it is a barrier {#phase-12-why-barrier}

A writer's meta point-read gate must see this round's condemns durable no later than the ledger
they are paired with. A per-hash operation exception is caught inside `GcMetaWriter`; a `ThreadPool`
framework failure propagates here and prevents the round commit.

On an exception path that skips this barrier, a non-throwing drain in `runRegularRound`'s
`SCOPE_EXIT` still waits for the same pool, so a throwing round never leaves its jobs running into
the next round, where their confirmations would reach a graduation gate that never scheduled them.

### 12.3 Phase costs {#phase-12-costs}

The phase sends no backend request on the GC thread. It waits on the bounded `meta_pool`
(`cas_gc_meta_pool_size`, default 16).

## Phase 13: round commit {#phase-13-round-commit}

Phase 13 is the round's commit boundary. It does two things in one phase: a pre-`CAS` retention
prune of old generations, then the single commit `CAS` over `<pool_prefix>/gc/state`. They are one
phase because the prune's writes are only safe as a pre-`CAS` action; splitting them would suggest
they are independently retryable.

### 13.1 Retention prune {#phase-13-prune}

`pruneSupersededGenerations` wholesale-deletes the prefix `<pool_prefix>/gc/gen/<g>/` for
generations older than `adopted_generation - cas_gc_snapshot_generations_to_keep`, bounded to 64
generations a round and by the shared prefix-wholesale budget. A generation still referenced by
either the parent seal (`parent_seal_runs` from phase 5) or the new seal is skipped, but
`snap_pruned_through` still advances past it. `suppress_destructive` skips the prune entirely and
leaves the cursor where it is.

The prune is a pre-`CAS` action, so it may rely only on already-published state. Protecting phase
5's parent references here is what keeps a losing leader's prune from destroying what the winning
leader's adopted seal still points at.

### 13.2 The commit `CAS` {#phase-13-commit-cas}

```text
casPut(<pool_prefix>/gc/state, encodeGcState(next), state_token)
```

`state_token` is the token from phase 1's lease `CAS`. `next` carries `round = new_round`, the
in-memory `snap_generation` and `snap_attempt` from phase 10, the new `snap_pruned_through`, and —
unless suppressed — the orphan-sweep `next_cursor` in `manifest_sweep_cursor`. A non-`Committed`
result is `ABORTED` ("gc/state moved during the round"): another leader advanced the object, and
this round publishes nothing.

### 13.3 After the commit {#phase-13-after-commit}

`RoundReport::round` is set, and `report.pending_*` are tallied from the adopted seal's
`condemned_summary`. From here the round is committed: an exception in phases 14 through 18 does not
un-commit it. See [the one-pass commit](#gc-state) for the fold seal's role as the coverage record.

### 13.4 Result {#phase-13-result}

Metrics: `generations_visited`, `pruned_through`, `generations_referenced`, `round`, `generation`.

### 13.5 Phase costs {#phase-13-costs}

| Key | Operation | Requests |
|---|---|---:|
| Pruned generation prefixes | `LIST` + wholesale `DELETE` | bounded per round |
| `<pool_prefix>/gc/state` | `CAS` | exactly 1 |

## Phase 14: handoff reclaim {#phase-14-handoff-reclaim}

Phases 14 through 18 are the post-`CAS` tail. They run only after a successful commit `CAS`.

Phase 14 is the first destructive site of the tail. Phase 13's prune skips a generation the live
seal still references but advances `snap_pruned_through` past it, and the wholesale prune never
revisits a generation behind the cursor. When a ref moves off such a generation during this round,
phase 14 wholesale-deletes that generation's whole prefix:

```text
<pool_prefix>/gc/gen/<old_generation>/
```

It is the exact reclaimer the ordinary prune would have used, deferred until the ref finally moved.

### 14.1 Conditions {#phase-14-conditions}

A generation is reclaimed only when the parent seal referenced it, the new adopted seal does not,
it is already behind `snap_pruned_through`, `suppress_destructive` is false, and the phase's own
prefix-wholesale budget — separate from phase 13's — is not exhausted.

### 14.2 One-shot {#phase-14-one-shot}

The reclaim is best-effort. A crash in this window leaks the prefix to `fsck`: the cursor already
advanced, so a plain retry does not re-attempt it, and the parent/new seal difference that triggers
the hand-off does not recur once the ref has moved. Unlike every other gated site, suppression here
loses the reclaim rather than postponing it — the ref moved off this round, nothing revisits, and
the prefix is left to `fsck`.

### 14.3 Result {#phase-14-result}

Metrics: `generations_reclaimed`, `objects_reclaimed`, `suppressed`.

### 14.4 Phase costs {#phase-14-costs}

One `LIST` plus a wholesale `DELETE` per handed-off generation prefix, bounded by the hand-off's own
budget.

## Phase 15: manifest deletes {#phase-15-manifest-deletes}

Phase 15 deletes owner-removed manifest bodies, now that phase 13's `CAS` adopted their minus-one
decrements.

### 15.1 Input {#phase-15-input}

`mf_cleanup` from phase 8: one `(manifest_id, token)` per folded `-1` owner edge. Each is deleted by
exact token:

```text
deleteExact(<pool_prefix>/cas/manifests/<ns>/<epoch-hex>-<build-seq-hex>/<NNNNNN>.zst, token)
```

`NotFound` and `TokenMismatch` are tolerated.

### 15.2 Unbudgeted by design {#phase-15-unbudgeted}

A cap would strand a declined entry: it is unreachable from any live ref and is never re-derived by
this pipeline, because the intake cursor that found the `-1` edge was committed by this round's
`CAS`, so a folded log is never revisited. The phase drains the whole `mf_cleanup` map every round
it runs; only a crash or `suppress_destructive` leaves an entry for the orphan-manifest sweep.

### 15.3 Result {#phase-15-result}

Metrics: `attempted`, `deleted`, `suppressed`.

### 15.4 Phase costs {#phase-15-costs}

One `DELETE` per `mf_cleanup` entry. No writes under `suppress_destructive`.

## Phase 16: namespace cleanup {#phase-16-namespace-cleanup}

Phase 16 is one bounded page of the perpetual namespace janitor. It runs on the folding path here,
and on the deferred path right after phase 4 with `suppress_destructive` forced on.

### 16.1 The page {#phase-16-page}

`LIST` up to 1000 keys of `<pool_prefix>/cas/ns/` from the durable `janitor_cursor`, read a fresh
`<pool_prefix>/cas/ref_catalog` snapshot, and classify each key by its `<life_id>`. A `<life_id>`
the catalog still resolves is live and is skipped. The rest is dead-life debris: `_log`, `_snap`,
`_ckpt`, and `_files/` objects of lives no longer in the catalog.

### 16.2 Deletion and fence {#phase-16-deletion}

Dead-life objects are deleted by exact token, under a GC fence re-check — `lease.owner` and
`lease.seq` in `<pool_prefix>/gc/state` — before each delete and once more at the end. The
incarnation segment in the key makes an old life's objects structurally unreachable from a reborn
same-name namespace, so a `LIST` that missed one can only leak storage, never expose it.

### 16.3 Cursor {#phase-16-cursor}

`janitor_cursor` advances only when the whole page was decided under a held fence and an
unambiguous catalog. Under suppression — always on the deferred path — the page lists and
classifies but deletes nothing and does not advance the cursor. The whole page is wrapped in a
catch-all: any exception is logged as "namespace janitor skipped this round" and is not re-raised.

### 16.4 Result {#phase-16-result}

Metrics: `evidence_rows`, `janitor_pages`, `janitor_keys`, `janitor_deleted`, `leaked`.

### 16.5 Phase costs {#phase-16-costs}

| Key | Operation | Requests |
|---|---|---:|
| `<pool_prefix>/gc/maintenance_state` | `GET` | 1, for the durable `janitor_cursor` |
| `<pool_prefix>/cas/ns/` | `LIST` | one page (up to 1000 keys) |
| `<pool_prefix>/cas/ref_catalog` | `GET` | 1 |
| `<pool_prefix>/gc/state` | `GET` | one per fence check |
| Dead-life object | `DELETE` | one per object |
| `<pool_prefix>/gc/maintenance_state` | `CAS` | 1 when the page is decided, plus 1 to reset a corrupt state |

## Phase 17: ref object cleanup {#phase-17-ref-object-cleanup}

Phase 17 deletes the `_log` and `_snap` objects of **live** namespace lives once fold coverage and
a checkpoint-named recovery triple make them safe.

It is distinct from phase 16: phase 16 removes objects of lives absent from the catalog, phase 17
removes objects of live lives whose fold has moved past them. So every delete is re-licensed by the
same complete catalog observation and GC lease that adopted the fold.

### 17.1 Plan and delete {#phase-17-plan}

Per namespace in `ref_tables`, resolved against the same `catalog_cut` (never re-resolved): read the
checkpoint recovery triple, run
`planRefCleanup(listing, durable_cursor, checkpoint_snapshot_id, retained_log_proof)`, and delete
each planned `_log` and `_snap` key. The checkpoint-named snapshot is always retained.

### 17.2 Authority re-validation {#phase-17-authority}

Before every `deleteExact`, `deleteRefObject` re-validates authority: a fresh
`<pool_prefix>/cas/ref_catalog` read whose token still equals `catalog_cut`'s, the same row and
life, and an unchanged GC fence in `<pool_prefix>/gc/state`. The first failure stops the whole
cleanup pass rather than falling back to another key.

### 17.3 Suppression and budget {#phase-17-suppression}

`suppress_destructive` returns immediately: a clamp could leave landed-before-cut edges unfolded, so
a covered-log delete could remove a log whose delta is not yet durable. `trim_enabled` is a test
seam; production always trims, and the phase row is emitted even when the pass is fully skipped. A
cumulative per-round `ref_cleanup` cap bounds the pass; on exhaustion `planRefCleanup` recomputes
the same remaining candidates next round.

### 17.4 Result {#phase-17-result}

Metrics: `suppressed`, `trim_enabled`, `namespaces_planned`.

### 17.5 Phase costs {#phase-17-costs}

| Key | Operation | Requests |
|---|---|---:|
| `_log` / `_snap` candidate | `HEAD` | one per candidate |
| `<pool_prefix>/cas/ref_catalog` and `<pool_prefix>/gc/state` | `GET` | one each per delete (authority re-validation) |
| `_log` / `_snap` key | `DELETE` | one per planned key |

## Phase 18: orphan sweep {#phase-18-orphan-sweep}

Phase 18 is the last phase. It executes the orphan-manifest sweep planned in phase 9 and adopted by
phase 13's `CAS`.

### 18.1 Execution {#phase-18-execution}

For each nomination in `orphan_sweep.nominations`, `deleteExact(<manifest key>, token)`. `NotFound`
is tolerated and counted as skipped. A `TokenMismatch` is `CORRUPTED_DATA` — an immutable manifest
identity must never change token (illegal ABA). This is stricter than every other post-`CAS` delete.

### 18.2 Authorization {#phase-18-authorization}

Phase 9 exact-read and identity-validated each candidate and computed its `source_retirements`.
Phase 13's `CAS` adopted both those retirements into the new seal's runs and the sweep cursor, so a
post-`CAS` body delete cannot orphan a still-reachable edge.

### 18.3 The deletion premise {#phase-18-premise}

A manifest of an epoch-`E` build is deletable only when the namespace cursor has consumed epoch
`E`'s closing seal and no unconsumed tail record above the cursor names it as a removal target. Any
uncertainty retains. The `retained_*` metrics break the retained count down by reason class; on
Stage A, retention is the normal outcome. Under suppression, phase 9 planned nothing, so the
nomination list is empty and the cursor did not move.

### 18.4 Result {#phase-18-result}

Metrics: `cursor_advanced`, `list_budget_keys`, `suppressed`, `listed`, `deleted`, `skipped`,
`undecodable`, `retained_no_coverage`, `retained_hold`, `retained_unconsumed_seal`,
`retained_tail_removal`.

### 18.5 Phase costs {#phase-18-costs}

One `DELETE` per nomination. The planning `LIST` and `GET` cost was paid in phase 9.

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
carry" (see [phase 9](#phase-9-merge)). A missing parent summary entry on a non-fresh pool is
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
| `GET` the adopted fold seal | 5, explicitly instrumented |
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
