---
description: "Canonical reference for the content-addressed (CAS) MergeTree garbage-collection protocol: leader election, lease + advisory heartbeat, the full round (fold → retire → fence → recheck → trim/reclaim), orphan removal, ref removal, shard-object reclaim, incarnation, registry removal (D1), attempt-scoped generations, snap prune, and concurrent-leader safety."
sidebar_label: "GC protocol"
sidebar_position: 4
slug: /superpowers/cas/gc-protocol
title: "CAS MergeTree — GC Protocol"
doc_type: reference
---

# CAS MergeTree — GC Protocol {#gc-protocol}

**Status summary:** see per-section stamps. The core round (fold → retire → fence → recheck → trim) is **DONE** and soak-validated. Attempt-scoped generations (concurrent-leader safety) and the source-edge-set in-degree (H1b fix) are **DONE** (2026-07-01). Shard incarnation + registry removal (D1) is **TLA+ gate GREEN, implementation TODO**. Snap prune and advisory heartbeat are **DONE**.

Cross-links: `06-tla-models.md` for formal proofs · `07-s3-budget.md` for per-operation cost.

---

## 1. Overview {#overview}

The GC subsystem is the only entity authorized to **delete** content-addressed objects. It is the symmetric counterpart of the write path: writers add reference edges; GC detects and collects objects whose edge count reaches zero. The central guarantee is:

> **No committed reference to content ever becomes unreadable**, because no object is deleted while any session pin or committed reference names it. (`INV_NO_LOSS`, `INV_NO_DANGLE` — proved in `CaGcRootLocalPartManifestCore.tla`, cross-link `06-tla-models.md §root-local-manifest`.)

GC is **not** on the write critical path. It runs as a background lease-holder loop (`CasGcScheduler`) and is *work-dedup only* — every round step is idempotent and split-brain-safe; a stale leader can only duplicate work, never mis-delete. The safety proof (`CaGcRootLocalPartManifestCore.tla`) explicitly makes no leadership uniqueness assumption.

### Object graph {#object-graph}

```
dynamic root namespace (per-table)
  └─ root shard (RootShard, mutable CAS object)
       └─ part manifest (PartManifest, immutable, root-local, single-owner)
            └─ blob (content-addressed, deduplicated across all tables)
```

- **Blobs** are content-addressed under `blobs/<aa>/<blob_hash>`. Only blobs are deduplicated across namespaces.
- **Part manifests** are immutable root-local objects under `roots/<ns>/_manifests/<writer_instance_id>/<build_seq>/<aa>/<manifest_instance_id>.proto`. Each manifest has at most one structural owner at any time.
- **Root shards** (`cas/refs/<ns>/<shard>`) are mutable CAS objects. Each carries a `RootOwnerEvent` journal, committed `RefRecord`s, and fence state.
- **Precommit owners** are journal entries naming a manifest before its committed publish. They protect blobs during the upload window.

---

## 2. Leader election, lease, and advisory heartbeat {#leader-election}

**Status: DONE** (B160 advisory heartbeat landed and soak-validated; `CaGcLeaseCore.tla` proves safety — see `06-tla-models.md §lease-core`).

### 2.1 Lease structure {#lease-structure}

GC leader election is a **clock-free, observation-window steal** over the durable `gc/state` object. The `GcState` carries:

```
GcState {
    lease { owner: UInt128,  seq: uint64 }   // current leader identity + monotone sequence
    fence_seq: uint64                         // per-round fence epoch
    round: uint64                             // monotone GC round counter
    snap_generation: uint64                   // pointer to the authoritative in-degree run set
    snap_attempt: uint64                      // attempt id that produced snap_generation (NEW — attempt-scoped gen)
    snap_pruned_through: uint64               // retention cursor for old gc/gen generations
    fence_version: { round → { "ns/shard" → shard_version } }
    gc_shards: uint64                         // immutable creation-time blob-target shard count
}
```

The steal is a single **atomic CAS** on `gc/state` that bumps `lease.owner`, `lease.seq`, and `fence_seq` together. Safety of every round step is independent of who holds the lease; the CAS-steal merely prevents redundant work.

A follower steals only when it has observed the incumbent's `(owner, seq)` **unchanged** across its observation window AND the heartbeat counter is also frozen (see §2.2). This prevents a false steal when a leader is alive but mid-round.

### 2.2 Advisory heartbeat (B160) {#advisory-heartbeat}

**Problem solved:** a GC round (`fold → retire → fence → recheck → cascade`) can take many seconds on a large pool, longer than the follower's tick interval. A follower that sees `gc/state.seq` frozen (the leader only bumps it once per round) falsely concludes the leader is dead and steals — causing ~70–80% of GC rounds to abort in two-replica soaks.

**Mechanism:** a separate lightweight heartbeat thread writes `gc/hb` (a small `{owner, hb_seq}` object) every `H` seconds independently of round progress. The steal decision is extended:

```
incumbent_alive =
    gc/state.(owner,seq) changed since last observation       // existing
 OR ( gc/hb.owner == gc/state.owner                          // not a stale ex-leader
      AND gc/hb.hb_seq advanced since last observation )     // new heartbeat pulse
steal ONLY IF NOT incumbent_alive
```

A live-but-slow leader's heartbeat advances within the observation window → no false steal. A dead leader's pulse also freezes → steal proceeds via the same atomic `gc/state` CAS.

**Fallbacks (never-worse):** if `gc/hb` is absent (fresh pool) or has a different owner (displaced ex-leader still pulsing), the protocol falls back to the existing `gc/state.seq` observation — exactly today's behavior. No degradation.

**TLA+ reference:** `CaGcLeaseCore.tla` + `_sab_noheartbeat.cfg` (produces a false-steal counterexample without the heartbeat) + `_heartbeat.cfg` (proves no false steal with heartbeat). See `06-tla-models.md §lease-core`.

---

## 3. The GC round {#gc-round}

**Status: DONE** (core round implemented and soak-validated; `CaGcRootLocalPartManifestCore.tla` proves safety at all stages — see `06-tla-models.md §root-local-manifest`).

The round skeleton (unchanged from initial design through all refinements):

```
discover → fold → retire → fence → recheck → exact-token delete → trim
```

No cascade step exists in the current implementation (the old tree-expansion cascade is gone with the part-manifest redesign; blob decrements are emitted directly from manifest fold).

### 3.1 Discovery {#discovery}

The GC leader discovers the live namespace universe by `LIST(cas/refs/)` — the set of `(ns, shard)` root-shard objects that physically exist. Each object is self-identifying (carries its `incarnation` — see §6.1).

**Registry removal (D1, TODO):** historically discovery read from `gc/registry`, an append-only authority. D1 (§7) replaces this with the LIST-based discovery and deletes the registry. The token-diff optimization (skip body re-read when the listed shard token matches the previously folded token) is retained as a read accelerator.

### 3.2 Fold {#fold}

Fold reads the incremental diff from each root shard's `RootOwnerEvent` journal and emits blob-edge deltas into the per-shard in-degree runs.

**Input:** each shard's journal window `(folded_cursor, current_shard_version]`. Events outside this window are skipped.

**Event semantics** (from the single ordered `RootOwnerEvent` stream):

| Event | Blob delta | Part-manifest cleanup |
|-------|------------|----------------------|
| `new_binding` only (first publish or precommit) | `+1` per blob entry (if manifest body present) | none |
| `old_binding` only, true removal | `−1` per blob entry | record `ManifestId` for cleanup |
| `old_binding == new_binding` (promote precommit, pure owner move) | **Δ = 0** | none |
| `old_binding ≠ new_binding` (repoint) | `−1` old + `+1` new | cleanup for old `ManifestId` |

**Fold barrier (missing-body precommit):** GC does NOT advance the fold cursor past a `RootOwnerEvent` that leaves a live precommit whose manifest body is absent or fails `RefMatchesBody`/`ManifestNamespaceMatches` validation. The cursor advances only when the body activates (emitting its `+1` deltas) or the precommit is abandoned (emitting no deltas). This is the fold barrier that makes promotion always of an activated manifest and keeps Δ = 0 correct for promotions. A stuck missing-body precommit is bounded by the writer-epoch watermark-based precommit reclaim path.

**Output artifacts** (all under the attempt namespace — see §5):

```
gc/gen/<G_f>/attempt/<a>/fold_seal
gc/gen/<G_f>/attempt/<a>/blob_target/<shard>/<run>
gc/gen/<G_f>/attempt/<a>/part_manifest_cleanup/<shard>/<run>
```

The `FoldSeal` records per-`(ns, shard)` coverage: `folded_token`, `folded_cursor`, and the produced run `RunRef`s (key + footer checksum). It is written **write-once** before the fold-adopt CAS; it is a deterministic artifact and any colliding write must be byte-equal or `CORRUPTED_DATA`.

**Cursor advance:** the `gc/state` CAS that advances `snap_generation` (the fold-adopt — see §5.2) also advances `folded_cursor` per shard. These two updates are atomic: a crash before the CAS leaves the prior generation authoritative and the new fold attempt as invisible garbage.

### 3.3 In-degree representation: source-edge set {#indegree-source-edge-set}

**Status: DONE** (H1b fix, 2026-07-01; `CaGcIndegRefoldCore.tla` validated the fix — see `06-tla-models.md §indeg-refold`).

**The persisted per-`(generation, shard)` artifact is `(blob_hash → set of active source edges)`, not an integer refcount.** A source edge is identified by `(ManifestId, path)`; its id is `sourceEdgeId(ManifestId, path)` (a stable deterministic 16-byte id). The run key is `(blob_hash BE-16 ++ source_id BE-16)` — lexicographic order is `(blob_hash, source_id)`.

Fold is **idempotent**: activating a source edge inserts it into the set (union); removing one deletes it from the set (set-difference). Re-folding either operation is a no-op. This structural idempotency eliminates the entire class of integer-refcount underflow bugs (H1b, H2 — see below under REJECTED).

`in_degree(blob) = |edge_set(blob)|`, computed transiently within a round from the run; never persisted as a separate counter.

`zeroInDegree` = blobs whose edge set became empty **this generation** (an explicit zero-transition marker row is written; prior-generation zero markers are dropped). Only blobs with an explicit zero-marker are GC candidates; blobs that never appeared as edge targets are never candidates (`INV_JOURNAL_COVERAGE` — a blob is only a candidate if GC ever saw it as a reference target).

**REJECTED: integer refcount.** The implementation previously persisted `(blob_hash → int64 count)` and folded as `prior_count + Σ ±1`. This is **not** idempotent: re-folding a removal (H1b — a fence-window removal whose cursor was not advanced past the fence) drives the count to −1, triggering a fail-closed `CORRUPTED_DATA` throw and wedging GC. The model `CaGcRootLocalPartManifestCore.tla` never caught this because it uses set-based idempotent in-degree — the implementation diverged from the model. The source-edge-set design is what the big model actually proves; the change makes the implementation faithful to it. See `06-tla-models.md §indeg-refold` for the focused model that validated the fix.

### 3.4 Retire {#retire}

Retire scans the blob-target shards for blobs whose source-edge set became empty this generation (`zeroInDegree`), issues a `HEAD` for each candidate to observe its current backend token, and writes a **retired set** (durable, write-once, append-by-unique-path under the attempt namespace).

The retired set at `gc/gen/<G_f>/attempt/<a>/retired/<round>/<shard>` carries `{kind, hash, observed_token, logical_size}` per entry. It is an **observation-bearing artifact**: the token is the value observed by this specific `HEAD`; two leaders may observe different tokens if a re-incarnation happened between their HEADs. The first durable write wins (read-if-present semantics, not byte-equal-or-`CORRUPTED_DATA`).

Once the retired set is written, the `RetireView` (the writer-facing barrier) reflects it — writers refreshing their retire view will see the condemned tokens and re-upload rather than referencing a condemned generation.

**Retire fail-closed (absent-at-retire):** a candidate whose object is already absent when its
`HEAD` returns 404 (for example a prior crashed round already deleted it) records **nothing** —
there is no token to condemn — and it flows to recheck as `Absent`. Retire must **never fabricate a
token** on a 404. A synthetic token would let a stale exact-token delete match a future, unrelated
incarnation that reuses the same hash. Fail-closed on the absent path is what preserves
`INV_NO_RETURN`.

**Snap prune (delete-time and retire-404):** when a candidate's `HEAD` returns 404, the node is pruned from the in-degree snapshot (`GcSnap::forget`, §4.1). When GC confirms a delete, the node is also pruned. This prevents the 404-HEAD storm where cumulative deleted nodes are re-`HEAD`ed every round. See §4.

### 3.5 Fence {#fence}

The fence is a CAS write to every discovered root shard that advances `fence_round` (monotone, `max` semantics). It records the committed `shard_version` into `GcState.fence_version[round]["ns/shard"]`.

**Purpose:** the fence_round in a root shard is the write-ordering barrier. A writer observing `fence_round = R` in a shard must not reference a blob condemned as of round `R` without first refreshing its retire view to `≥ R`. This closes the create-ordering race: a publish after the fence lands at a `shard_version > fence_version[R][shard]`; the recheck fold includes it; the blob is spared.

**Registry fence (D1 removes):** previously there was also a registry-fence sub-step that minted fence-only manifests for absent shards. D1 (§7) removes this: only discovered (physically present) shards are fenced; birth ordering is now handled by the precommit gate.

### 3.6 Recheck and exact-token delete {#recheck}

**This is the only deletion site.** Four gates must all pass before `deleteExact` fires:

1. **Durable retired set entry** — the token was observed and persisted.
2. **All-shard fence** — every shard was fenced at ≥ round `R`.
3. **Fold-through-fence** — the recheck re-folds the fence window `(folded_cursor, fence_version]` into the completion generation. Only blobs whose source-edge set is still empty after this re-fold are deletion candidates.
4. **Exact observed token** — `deleteExact(key, observed_token)` uses `If-Match`; a 412 (token mismatch) means the object was re-incarnated between retire and delete → outcome `Replaced`, no delete.

The completion generation artifacts:

```
gc/gen/<G_c>/attempt/<a>/blob_target/<shard>/<run>    // completion in-degree runs
gc/gen/<G_c>/attempt/<a>/outcomes/<round>/<shard>     // durable delete outcomes
gc/gen/<G_c>/attempt/<a>/completion_seal              // written after full outcome coverage
```

The completion in-degree runs must be durable **before** any `deleteExact` of that round (so a crash mid-delete can re-derive the candidate set from durable state). Outcome logs are written **after** each delete. The completion advance (CAS #2) requires a complete outcome log for every retired entry.

**Outcomes:**
- `Deleted` — `deleteExact` succeeded; object removed.
- `Absent` — the object was already gone before `deleteExact` (a prior crashed round deleted it). No error.
- `Replaced` — 412, token mismatch; the object was re-incarnated; not deleted, no data loss.
- `Spared` — in-degree > 0 after the fold-through-fence recheck; not a candidate after all.

**Recheck 404 policy (context-specific, fail-closed — no blanket "missing body ⇒ spare"):**
recheck applies the **same context-specific missing-body policy as fold**, keyed on the binding
kind:

- A missing or invalid **committed-or-promoted new-binding** manifest body inside the fence window
  **clamps/aborts the affected delete** (fail-closed, **not** spare-by-default) and is surfaced to
  `fsck` — a committed ref must never resolve to a missing manifest, so this is a corruption signal,
  not a benign absence.
- A missing **precommit** body is **non-activating** (it contributed no blob edges), not corruption.
- An **old-binding removal** uses the blob edges **already sealed at fold** (computed while the old
  body was still required present). **Recheck must never read a deleted manifest body** to compute
  decrements — the edges were sealed earlier, closing the delete-then-recreate race.

**`created_delete_marker → LOGICAL_ERROR` (per-delete versioning guard):** if a `deleteExact`
`DeleteOutcome` reports `created_delete_marker = true`, the backend created a versioning tombstone
instead of a real delete — i.e. the bucket is versioning-enabled, which the startup probe (`01
§backend-contract`) should already have rejected. GC throws `LOGICAL_ERROR` fail-closed: the pool is
mis-provisioned, and a delete-marker regime would let an exact-token-deleted object be resurrected
(`INV_NO_RETURN` violation). This is a per-delete backstop for the startup probe.

### 3.7 Part-manifest cleanup {#manifest-cleanup}

Part manifests (`_manifests/...`) are deleted when their owning reference is removed. The part-manifest cleanup bundle records `ManifestId`s whose owner was removed and whose blob decrements were sealed into the generation. The delete is an exact-token `deleteExact` issued during the recheck/completion phase, gated by the same fence + fold ordering as blob deletes.

A part manifest has at most one structural owner at any time (`SingleManifestOwner`). An owner removal that leaves no successor is a true removal → the manifest body is debris after its blob decrements are sealed.

**Orphan part-manifest sweep** (pre-precommit debris): manifest bodies written before `PrecommitAdd` that were never activated have no owner. A bounded background sweep (one namespace, one eligible build prefix per round) enumerates and deletes them by exact token after confirming they are absent from the namespace's sealed owner view. Eligible build prefixes are those whose `writer_instance_id`/`build_sequence` the watermark confirms can no longer write (e.g. `min_active > build_sequence`). See `CaGc.cpp::orphanPartManifestSweep`.

### 3.8 Trim {#trim}

After the completion seal is durable, `trim` removes journal records from root shards that are at or below the committed `folded_cursor`. This is `INV_JOURNAL_COVERAGE`: only records durably represented in the committed snap generation may be trimmed.

**B140-dangle HISTORY:** the original implementation stored snap edges and `folded_cursor` as two separately-durable objects with no enforced coherence. A lease-steal between the snap write and the `gc/state` CAS could leave `folded_cursor` ahead of the actual snap coverage → `trim` over-trimmed → a live part's edge was permanently lost → in-degree undercount → data loss (`fsck dangling`). The fix (v2, `2026-06-18`) embeds the fold cursor inside the snap codec so the two are always co-durable. The `CaB140DangleMerge.tla` model proved the fix closes the dangle. See `06-tla-models.md §b140-dangle`.

### 3.9 Resident-snap incremental GC and the durable-vs-resident cursor {#resident-snap-checkpoint}

**Status: DONE** (incremental-GC checkpoint; source `specs/2026-06-14-ca-reduce-s3-op-count-design.md`).

To avoid a per-round `loadSnap` + full snap PUT on a large pool, the decoded `GcSnap` is kept
**resident** in the long-lived per-leader GC object. Each round folds only the new journal records
(changed root-shard bodies past `folded_cursor`) into the resident snap — no per-round snap I/O. The
whole snap plus its `folded_cursor` are persisted **only at a checkpoint**, triggered when either:

- `gc_checkpoint_records = 4096` — at least this many journal records have been folded since the
  last checkpoint (caps the recovery re-fold cost), or
- `gc_checkpoint_rounds = 64` — at least this many rounds have elapsed (caps staleness even under
  zero churn).

Between checkpoints there is **zero snap I/O**. A recovering or newly-elected leader loads the last
checkpoint's snap and `folded_cursor`, then re-folds the journal delta from that cursor to the live
`shard_version` (bounded by `gc_checkpoint_records`), so handoff recovery is seconds, not minutes.

**Durable-vs-resident cursor trim invariant:** there are two cursors — the **resident** cursor
(advances every round as records are folded into the in-memory snap) and the **durable**
`folded_cursor` (advances only at a checkpoint). `trim` (§3.8) may remove journal records **only at
or below the durable `folded_cursor`**, never the resident cursor. Records between the durable
cursor and the live `shard_version` are unpersisted; a recovery leader re-folds them from the
durable cursor, so trimming them would lose folded edges on the next recovery — the exact B140-class
undercount. The `gc/state.folded_cursor` therefore always equals the last-checkpoint cursor, and the
incremental path must not advance it except at a checkpoint.

---

## 4. Snap prune {#snap-prune}

**Status: DONE** (B174 snap prune, `gc_snap_generations_to_keep`, soak-validated; `CaGcRootLocalPartManifestCore.tla` is unaffected — prune is pure space reclamation).

### 4.1 Delete-time and retire-404 node pruning {#node-pruning}

**Status: DONE** (P9, TLA+ `GForget` action added to `CaGcCore.tla`).

**Problem:** after GC deletes an object, its node stays in the `known` set (candidate eligibility) forever. Every subsequent round re-derives it as a zero-in-degree candidate and issues a `HEAD` → genuine 404 → `continue`. Measured: ~46k re-`HEAD`s per round even when no objects were deleted this round — ~98% of GC op-count.

**Fix:** `GcSnap::forget(kind, hash)` removes a node from `known` the instant GC confirms it is gone:

- **Delete-time prune** (primary): after a `deleteExact` outcome of `Deleted` or `Absent-while-held`, call `snap.forget(kind, hash)`. Rides the existing cascade persist — durable by the time the round's retired sets are dropped.
- **Retire-404 prune** (defensive): when a candidate `HEAD` returns 404 in the retire loop, call `snap.forget(kind, hash)` and continue. Self-heals the split-brain case where a live leader already deleted a node the follower still observes in its snap.

A forgotten node that is later re-referenced is re-added to `known` by the ordinary fold (`GFold` re-inserts into `known`). Forgetting is idempotent and can never cause a delete (it only removes a node from candidacy).

**TLA+ reference:** `CaGcCore.tla` adds a `GForget(l, h)` action gated on `~present[h] ∧ h ∈ everEdged ∧ InDeg(h) = 0`. All invariants (`INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_RETURN`, `INV_OVER_COUNT_ONLY`) hold under `GForget`. See `06-tla-models.md §gc-core`.

### 4.2 Generation retention {#generation-retention}

**Status: DONE** (B174).

GC writes one `gc/gen/<generation>/` tree per round (the full folded in-degree run set + seals) and historically never deleted old ones. Measured at 12h soak: `gc/` was 82% of pool storage (2822 generations retained, ~3.7 MiB/shard/generation), dwarfing actual data 8:1.

**Mechanism:** `PoolConfig::gc_snap_generations_to_keep` (default 3; 0 = unlimited for forensics). The prune step runs inside each round's cascade, before the `gc/state` CAS. It advances a `GcState::snap_pruned_through` cursor forward in bounded batches (`MAX_PRUNE_GENERATIONS_PER_ROUND = 64`):

```
prune_floor = adopted_generation - keep
for g in (snap_pruned_through, prune_floor]:
    for each shard in [0, gc_shards):
        deleteExact(gcGenKey(g, shard), head_token)   // Absent/Replaced tolerated (idempotent)
```

**Safety:** pruning runs before the `gc/state` CAS, when `gc/state` still names the prior generation. `prune_floor = adopted_generation - keep` is strictly below the committed generation and `keep−1` above it. If the subsequent CAS fails (lease lost), the already-issued deletes are harmless (the winning leader's floor is even higher). The cursor is not durably advanced on CAS failure, so the next round re-attempts idempotently.

**Scope:** prunes only GC's internal `gc/gen/` bookkeeping. Does NOT touch data objects (blobs, manifests, refs).

---

## 5. Attempt-scoped generations (concurrent-leader safety) {#attempt-scoped-generations}

**Status: DONE** (concurrent-leader-leak fix, 2026-07-01; `CaGcRootLocalPartManifestCore.tla` `_sab_deposedleaderwritesfinalgen.cfg` is the negative-control that proves the old design was unsafe).

### 5.1 Problem: orphaned-seal wedge and divergent-run corruption {#concurrent-leader-problem}

Before this fix, a GC leader deposed mid-round would still finish its fold and write `fold_seal(N+1)` via `putIfAbsent` to a **final** `gc/gen/<N+1>/fold_seal` key — which succeeds because the write-once slot is empty — then fail the lease-guarded `gc/state` CAS. `snap_generation` stays at `N`, but `fold_seal(N+1)` is now durable. Every later round recomputes `N+1`, re-folds, hits the orphaned seal's divergent bytes, and throws `ABORTED "concurrent leader"` — **forever** (GC wedges).

Worse: if a divergent leader's blob in-degree run also landed at the final key, the live leader's seal records a checksum of different bytes → wrong in-degree → either over-pin (leak) or over-delete (dangle, `INV_NO_DANGLE` violation).

Root cause: **an irreversible, reader-visible artifact was published at a final generation key before the leader re-confirmed its authority.**

### 5.2 Fix: accepted-attempt generation {#attempt-scoped-design}

Each round mints exactly one **attempt** `a = lease.seq`. All of that round's artifacts live under an attempt-scoped prefix:

```
gc/gen/<G_f>/attempt/<a>/fold_seal
gc/gen/<G_f>/attempt/<a>/blob_target/<shard>/<run>
gc/gen/<G_f>/attempt/<a>/part_manifest_cleanup/<shard>/<run>
gc/gen/<G_f>/attempt/<a>/retired/<round>/<shard>
gc/gen/<G_c>/attempt/<a>/blob_target/<shard>/<run>
gc/gen/<G_c>/attempt/<a>/outcomes/<round>/<shard>
gc/gen/<G_c>/attempt/<a>/completion_seal
```

No artifact is written to a final `gc/gen/<g>/…` key outside an `attempt/` prefix before its adopt CAS. Two competing leaders for the same generation write to **different** attempt prefixes (`lease.seq` is unique per steal). Divergent-run and divergent-seal collisions are structurally impossible.

**Round lifecycle:**

1. **Fold (hidden).** Leader writes fold artifacts under `gc/gen/<G_f>/attempt/<a>/`. Nothing is reader-visible. Competing leaders fold under their own `a`.
2. **Fold adopt (CAS #1 — selects the attempt).** A lease-token `gc/state` CAS sets `snap_generation = G_f, snap_attempt = a`. Commits iff the leader still holds the lease. A deposed leader's adopt fails; its entire attempt is unadopted garbage.
3. **Tail.** `retire` / `fence` / `recheck` write durable artifacts under the **accepted** `(snap_generation, snap_attempt)`.
4. **Completion advance (CAS #2 — inherits the attempt).** Once the completion seal + full outcome coverage are durable, a lease-token CAS advances `snap_generation = G_c`, keeping `snap_attempt = a`. A deposed leader's CAS #2 fails.

**Artifact-class rule** (how concurrent writes to the same attempt key are reconciled):

- **Deterministic artifacts** (in-degree runs, fold seal, completion seal): byte-reproducible from their inputs. Any producer finding one present must find it **byte-equal → adopt** (deterministic replay) or **divergent → `CORRUPTED_DATA`** (fail-closed, impossible under correct operation). Replaces the old outcome-ignoring `putIfAbsent` in `foldDeltasIntoGeneration`.
- **Observation-bearing artifacts** (retired sets, outcome logs): carry HEAD-observed tokens that two observers may legitimately differ on (a re-incarnation between their HEADs). **First-durable-write wins** (read-if-present). A later producer reads the present artifact and uses it as authority; it never recomputes-and-compares.

**`ViewableRound` round-advance rule (sharded subset-safety):** `gc/state.round` advances only
after **every** blob-target shard's retired set **and** every part-manifest cleanup bundle for the
round are durable. This preserves `ViewableRound`. In sharded mode the blob-target reducers run in
parallel over disjoint shards; a writer refreshing its retire view to round `R` must observe the
**complete** retired-token set, not a partial snapshot from the faster reducers. Advancing the round
before all shards have written would open a window where a writer publishes against a stale token
view and dangles. The invariant name is `INV_ONLY_ADOPTED_VIEWABLE` / `ViewableRound` in the model;
this is its operational statement.

**Resume** (`tryResumeIncompleteRound`): derives the stopping point from which accepted-attempt artifacts are durable — no stored phase marker. Crash before fold adopt: hidden attempt is garbage; next round folds fresh. Crash after fold adopt but before completion advance: resume the tail, reading present artifacts as authority, producing only the missing ones.

**Pruning** (space reclamation, not safety-load-bearing):

1. *Generation retention* (§4.2) deletes `gc/gen/<g>/` wholesale when `g < retention_floor` — all attempts of obsolete generations.
2. *Attempt orphan sweep* looks at `gc/gen/<snap_generation>/attempt/*`, skips `snap_attempt`, deletes attempts with `seq < min_live_lease_seq`. Bounded per round, fail-open, exact-token.

**TLA+ reference:** `CaGcRootLocalPartManifestCore.tla` `_sab_deposedleaderwritesfinalgen.cfg` — a deposed leader writes artifacts to a final gen key; this **must** produce a counterexample (`INV_ONLY_ADOPTED_VIEWABLE` or an R0 safety violation). The design config holds all R0 invariants. See `06-tla-models.md §root-local-manifest`.

---

## 6. Orphan removal {#orphan-removal}

### 6.1 Blob orphan removal {#blob-orphan-removal}

**Status: DONE** (via the normal round — fold detects zero-in-degree blobs; retire/recheck deletes them).

A blob becomes an orphan when all manifests referencing it are removed (their edges are folded out). The fold emits explicit zero-transition markers for such blobs; `zeroInDegree` streams them; `retire` observes their token; `recheck` deletes after the fence + fold-through-fence confirmation.

The only path to blob deletion is through this pipeline. There is no direct delete bypassing the four gates.

### 6.2 Part-manifest orphan removal {#manifest-orphan-removal}

**Status: DONE** (owner-driven cleanup in part-manifest cleanup bundle; orphan sweep for pre-precommit debris is DONE).

A part manifest is orphaned when its owner (committed ref or precommit) is removed. The fold emits its `ManifestId` into the part-manifest cleanup bundle; the recheck/completion phase issues `deleteExact` for it after the fence confirms no concurrent re-attachment.

Pre-precommit manifest bodies (written before `PrecommitAdd`) have no owner. The orphan sweep handles them (§3.7).

### 6.3 In-degree via source-edge set, not integer refcount {#source-edge-not-refcount}

**REJECTED: integer refcount.** See §3.3. The key invariant is that **in-degree is never persisted as authority** — it is a transient quantity derived from the active source-edge set within a round. Persisting it as a separate counter and carrying it forward as `prior_count + Σ±1` is fragile (H1b: fence-window removal re-folded next round → underflow; H2: drop-then-repoint double-counts a −1). The source-edge set is idempotent by construction.

The `CasBlobInDegree.h` API reflects this: `foldDeltasIntoGeneration` takes `std::vector<BlobDelta>` where each `BlobDelta{blob_hash, source_id, remove}` is an edge activation or removal, and the fold is an idempotent set merge.

### 6.4 Additional reachability roots {#reachability-roots}

Beyond committed part refs and precommits, two integration paths contribute reachability roots the
MARK walk must honor (source `specs/content_addressed_shared_mergetree_design.md`):

**Patch parts / lightweight deletes are first-class roots.** A patch is its own ref class: a **patch
manifest** that references the base part's data columns (the columns it patches) plus its own delta
blobs. A patch ref is a first-class reachability root, exactly like a regular part ref. While a patch
is active, its ref keeps alive **both** its own delta blobs **and** the base blobs it references
(transitive reachability through the patch manifest) — a base part's blobs cannot be swept while a
live patch references them, even if the base part is otherwise superseded, provided the MARK walk
treats patch manifests as roots and follows their references into the base. When a patch is
materialized into the base (a merge), the patch ref is released like any other ref, and its
uniquely-owned delta blobs become sweepable. (Residual-risk note: patch-on-content-addressed-base is
flagged as not yet fully model-checked in the v3 design's residual-risk list.)

**Stateless / ref-less reader GC fence.** The per-process `isSharedPtrUnique` (`use_count() == 1`)
fence is a valid cross-node fence only for replicas that hold a `/parts` ref. It does **not** cover a
stateless compute node that attaches by reading the catalog and registers no `/parts` ref — such a
reader is invisible to the MARK union, and the owning replicas can release their refs after
`old_parts_lifetime` (480 s) mid-SELECT, making the manifest/blobs unreachable and swept out from
under the read. The fix is **ephemeral reader state included in the MARK union**: an ephemeral Keeper
node (or equivalent) created at query start naming the parts/snapshot it reads, **auto-released on
session end / crash** so a crashed reader cannot leak the pin (grain: one pin per part, or a single
per-snapshot pin for huge scans). A coordinator-free deployment may instead size grace ≥ the longest
ref-less SELECT, but the self-bounding ephemeral pin is preferred. This is **distinct** from the
**lost-replica timeout** (a crashed-but-not-dropped replica's `/parts` refs outlive its process and
pin blobs forever — a genuine leak needing a separate long-dead-replica reaping rule).

> Note: under the current per-server-owned-namespace model (`01 §shared-blobs-per-server-trees`) each
> server owns its own ref namespace, which narrows but does not eliminate the ref-less-reader window;
> the ephemeral-pin design remains the documented mechanism for the shared-pool cross-node read case.

---

## 7. Ref removal and shard-object reclaim (D1) {#d1-registry-removal}

**Status: TLA+ gate GREEN (2026-07-01); implementation phases 1–5 TODO.** `CaGcShardIncarnationCore.tla` `_design.cfg` holds `INV_NO_DANGLING` + `INV_NO_ORPHAN_EDGE` across 724,944 distinct states. See `06-tla-models.md §shard-incarnation`.

### 7.1 Problem: `dropNamespace` never deregisters {#drop-namespace-problem}

`dropNamespace` (`CasStore.cpp:942-995`) tombstones each touched shard (appends removal `RootOwnerEvent`s, clears refs via `mutateShard`), but leaves the namespace in `gc/registry`. GC discovers from `registry.namespaces × shardsToVisit` and fences the full cartesian product — so per-round GC cost is proportional to **every table ever created**, not the live ones. The empty shard objects and registry entries accumulate without bound (S30 scenario). A scalability defect, not a correctness bug.

### 7.2 Rejected fixes {#d1-rejected}

- **Writer deregisters at `dropNamespace`:** unsafe. The removal `RootOwnerEvent`s carry the `−1` blob-in-degree edges; GC folds them only in the recheck window `(folded_cursor, fence_version]`. If the namespace vanishes from discovery before GC folds that window, the `−1`s are lost → blobs keep phantom in-degree → permanent leak.
- **GC infers "empty ⇒ retire":** a freshly-created table with no inserts yet is indistinguishable from a dropped empty one.
- **Path-keyed cursor + delete-and-recreate:** ABA hazard. A recreated shard resets `shard_version` to 0; the old sealed `folded_cursor = K` silently skips events at versions `1..≤K` → lost edges → dangle or leak.

### 7.3 Design: incarnation + LIST-based discovery {#d1-design}

Two orthogonal coordinates replace all five overlapping counters:

**Coordinate 1 — `incarnation`.** A durable, monotone, never-reused value stamped into a `RootShard` at its (re)creation and immutable for that object's life. Source: `(writer_epoch, build_sequence)` of the build that first creates the shard, both already durable-monotone-never-reused. On delete + recreate, the new create stamps a strictly-greater coordinate. **`INC-MONO`**: for a fixed `(ns, shard)`, every successive materialization carries a strictly greater incarnation.

**Coordinate 2 — GC `round`.** The pool-global clock (unchanged). The writer's retire-view gate floor: no writer may durably reference a blob condemned as of round `R` without refreshing its retire view to `≥ R`. This is pool-global and cannot be per-`server_root`.

The fold cursor is keyed by `(ns, shard, incarnation)` instead of `(ns, shard)`. A recreated shard draws a strictly-greater incarnation → the old sealed cursor never matches → fold always processes a new incarnation from zero. ABA is closed by construction.

**Discovery replaces the registry:** `discoverUniverse` becomes `LIST(cas/refs/)`. The registry (`gc/registry`, `RootsRegistry`, `rootsRegistryKey`) is deleted entirely. `listNamespaces` (used for FREEZE shadow-tree enumeration) migrates to a `LIST` over `cas/refs/`.

**Newborn namespace ordering (no separate object):** the registry's irreducible role was ordering a first publish that dedup-references an existing blob against a concurrent last-drop-and-GC. D1 replaces this with the existing **precommit machinery**: a first publish creates the ref-shard object at its canonical path carrying a precommit binding (with the fresh incarnation). The shard is LIST-discoverable immediately. The precommit gate (fold barrier + watermark reclaim) provides create-ordering without a separate pending-newborns object. **THM-NO-RETURN (central):** with the registry removed, for a first publish that dedup-references blob `B`, either (a) the newborn shard is in GC's LIST universe and GC folds its precommit `+1` before condemning `B`, or (b) the writer observed a retire-view floor `≥ R` and re-uploads rather than referencing a condemned blob. No interleaving can dangle a live ref. This is the theorem the TLA+ gate proved.

**Shard-object reclaim:** `dropNamespace` appends, as its last journal event, an explicit **tombstone** marker (`RootOwnerEvent` variant meaning "namespace dropped, no owner"). GC, during fold, when a shard is empty (no refs), its last journal event is the tombstone, and its journal has been folded past a completed fence, issues `deleteExact(rootShardKey(ns, shard), token)`. Any writer append (revive) changes the token → `deleteExact` returns `TokenMismatch` → the delete is refused; the object survives with new content. An idle-but-live shard (all parts dropped, table alive) has no tombstone → not reclaimed → still discovered. The tombstone is the drop signal.

**TLA+ validation:** `CaGcShardIncarnationCore.tla` `_design.cfg`. Three negative controls confirm irreducibility: `SabotageNewbornNoFloor` (round self-floor is irreducible), `SabotagePathKeyedCursor` (incarnation is irreducible — ABA without it), `SabotageDeleteBeforeFold` (fold-before-reclaim ordering is irreducible). **The one-vs-two coordinates question is answered: two** — neither coordinate alone suffices.

---

## 8. Safety invariants {#safety-invariants}

The following invariants are the formal safety obligations. Sources: `CaGcRootLocalPartManifestCore.tla` (primary big model), supplemented by focused models for specific sub-problems.

| Invariant | Meaning | Proved in |
|-----------|---------|-----------|
| `INV_NO_LOSS` | No committed ref to content becomes unreadable | `CaGcRootLocalPartManifestCore.tla` |
| `INV_NO_DANGLE` | No live edge addresses a deleted object | `CaGcRootLocalPartManifestCore.tla` |
| `INV_NO_RETURN` | A dead token (exact-token-deleted object) never returns | `CaGcRootLocalPartManifestCore.tla` |
| `INV_JOURNAL_COVERAGE` | Trim only removes records at or below the committed `folded_cursor` | `CaGcRootLocalPartManifestCore.tla` |
| `INV_OVER_COUNT_ONLY` | GC state may over-count, never under-count | `CaGcRootLocalPartManifestCore.tla` |
| `INV_ONLY_ADOPTED_VIEWABLE` | Only artifacts under `(snap_generation, snap_attempt)` are consulted by any decision path | `CaGcRootLocalPartManifestCore.tla` (attempt-scoped gen config) |
| `INV_NO_DANGLING` (D1) | No live ref addresses a deleted shard object | `CaGcShardIncarnationCore.tla` |
| `INV_NO_ABA` (D1) | Incarnation-keyed cursors never skip a new incarnation's events | `CaGcShardIncarnationCore.tla` |
| `INV_INDEG_NONNEG` | Blob in-degree never goes negative (source-edge set cannot) | `CaGcIndegRefoldCore.tla` `_fix.cfg` |

**Ordering bias:** all ordering choices (write `+` before ref; write ref before session removal; remove ref before writing `−`) are biased to **over-count** on crash, never under-count. An over-count delays reclamation; an under-count could delete a live object. This is `INV_OVER_COUNT_ONLY`.

---

## 9. Concurrent-leader safety summary {#concurrent-leader-summary}

Three mechanisms together make the round split-brain-safe with no additional locking:

1. **Attempt-scoped generations (§5):** unadopted artifacts are invisible to every decision path. Two concurrent leaders for the same generation write to different attempt prefixes. Only the fold-adopt CAS #1 selects the authoritative attempt; any leader whose adopt fails has its entire fold attempt as inert garbage.

2. **Exact-token delete:** `deleteExact(key, observed_token)` uses `If-Match`. A re-incarnation between `retire` and `deleteExact` changes the token → 412 → `Replaced` outcome → no delete. A deposed leader's delete is idempotent: the winner observes `NotFound`/token-mismatch and records the outcome.

3. **GC lease (work-dedup only):** the lease prevents redundant parallel work and reduces S3 churn, but is not a safety primitive. The TLA+ proof makes no leadership uniqueness assumption. Any number of concurrent GC leaders produce correct outcomes.

**REJECTED: `gc_lock` (in-process mutex).** The original PoC serialized every commit against the whole sweep with a shared in-process mutex. This blocked writers and limited GC to one process. The current design replaces it with the above three mechanisms; the mutex has been removed.

---

## 10. S3 budget {#s3-budget}

GC is the dominant S3 cost center on a pool with steady ingest/drop churn. Detailed per-phase breakdown is in `07-s3-budget.md`. Key figures:

| Phase | Dominant cost |
|-------|--------------|
| Discover | 1 LIST per root (token-diff: skip if token unchanged) |
| Fold | 1 GET per changed root shard; 1 PUT per blob-target-shard in-degree run; 1 PUT fold seal |
| Retire | 1 HEAD per zero-in-degree candidate; 1 PUT retired set per shard |
| Fence | 1 CAS per live root shard (bumps `fence_round`) |
| Recheck | 1 GET per completion in-degree run; `deleteExact` per confirmed deletion |
| Snap prune | `snap_shards × (HEAD + DELETE)` per pruned generation (steady state: 1 generation/round) |
| Heartbeat | 1 small CAS per `H` seconds |

**P9 snap-prune impact (DONE):** before snap-prune and the node-forgetting mechanism, GC issued ~46k `HEAD`s per idle round (re-`HEAD`ing every previously deleted candidate). Both are now eliminated: the retire-404 path prunes the node on first 404; the delete-time path prunes immediately on deletion. The `gc/` prefix went from 82% of pool storage to bounded sawtooth.

---

## 11. Testing {#testing}

**Status: comprehensive gtest suite exists; soak S04/S33/S03/S11 all drain to `fsck unreachable=0, dangling=0, gc_residual=0` with the current binary.**

Key test files:
- `src/Disks/tests/gtest_cas_gc_round.cpp` — per-step unit tests (fold/retire/fence/recheck/delete/trim/lease)
- `src/Disks/tests/gtest_cas_gc_scenarios.cpp` — fault-injection scenario battery (split-brain, crash-replay, spared/replaced/absent outcomes)
- `src/Disks/tests/gtest_cas_gc_undercount_repro.cpp` — H1b regression (fence-window removal re-folded next round); GREEN after source-edge-set fix
- `src/Disks/tests/gtest_cas_gc_snap.cpp` — node-forgetting / snap-prune unit tests
- `utils/ca-soak/scenarios/` — adversarial scenario suite (S01–S35); S30 tests D1 registry growth; S33 tests concurrent GC leaders

**Known blind spot:** tests running with `gc_shards=1` (the default) do not exercise sharded bugs. All new fold/discovery tests added for D1 must also run with `gc_shards > 1`.

---

## 12. DONE / TODO / REJECTED / DESIRABLE {#status}

| Area | Status | Note |
|------|--------|------|
| Core round (fold → retire → fence → recheck → trim) | **DONE** | Soak-validated |
| Exact-token delete, four-gate recheck | **DONE** | |
| Source-edge-set in-degree (H1b fix) | **DONE** | 2026-07-01; faithful to big TLA+ model |
| Attempt-scoped generations (concurrent-leader safety) | **DONE** | 2026-07-01; `_sab_deposedleaderwritesfinalgen` confirmed |
| Advisory heartbeat (B160, false-steal fix) | **DONE** | `CaGcLeaseCore.tla` proved |
| Snap prune — node forgetting (P9) | **DONE** | `GcSnap::forget`; `GForget` added to model |
| Snap prune — generation retention (B174) | **DONE** | 3 gen default; sawtooth gc/ storage |
| Part-manifest cleanup (owner-driven) | **DONE** | |
| Orphan part-manifest sweep (pre-precommit debris) | **DONE** | Bounded per round |
| Shard incarnation (D1 phase 1) | **TODO** | TLA+ gate GREEN 2026-07-01 |
| LIST-based discovery + registry deletion (D1 phase 2) | **TODO** | Depends on phase 1 |
| Newborn = precommit-shard (D1 phase 3) | **TODO** | Depends on phase 2 |
| Shard tombstone reclaim (D1 phase 4) | **TODO** | Depends on phase 3 |
| S30 soak validation (D1 phase 5) | **TODO** | Depends on phase 4 |
| Distributed `gc_shards > 1` parallel GC | **DESIRABLE** | Attempt-scoped gen is its prerequisite; shard claim/scheduler not yet built |
| Integer refcount persisted artifact | **REJECTED** | H1b underflow; replaced by source-edge set |
| `gc_lock` in-process mutex | **REJECTED** | Blocks writers; replaced by exact-token + attempt-scoped gen |
| TLA+ leadership uniqueness assumption | **REJECTED** | Safety proof makes no leadership assumption; see `INV_ONLY_ADOPTED_VIEWABLE` |
| Keeper as durable state for GC | **REJECTED** | Keeper is optional accelerator only; all GC durable state is in S3 |
| Registry as authority for namespace universe | **REJECTED** → D1 | Monotone growth; being replaced by LIST-based discovery |
| "empty ⇒ deregister" inference | **REJECTED** | Indistinguishable from idle-but-live namespace |
| Path-keyed fold cursor without incarnation | **REJECTED** | ABA hazard on drop+recreate |
