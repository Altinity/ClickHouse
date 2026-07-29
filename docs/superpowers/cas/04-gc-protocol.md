---
description: "Canonical reference for the content-addressed (CAS) MergeTree garbage-collection protocol: leader election, lease + advisory heartbeat, the one-pass ack-floor round (heartbeat floor → three-cursor merge → two-phase graduation → single gc/state CAS), orphan removal, ref removal, shard-object reclaim, incarnation, registry removal (D1), attempt-scoped generations, snap prune, and concurrent-leader safety."
sidebar_label: "GC protocol"
sidebar_position: 4
slug: /superpowers/cas/gc-protocol
title: "CAS MergeTree — GC Protocol"
doc_type: reference
---

# CAS MergeTree — GC Protocol {#gc-protocol}

**Status summary:** see per-section stamps. The regular round is a **single pass**: a heartbeat ack floor, one three-cursor merge that verifies/graduates/condemns in the same fold, two-phase graduation of deletions, and one `gc/state` CAS. It is **DONE** (implemented on branch `cas-gc-ack-floor-fence`; soak-validated, including a live-AWS S3 run, 2026-07-03 — the night soak that ran under this round found and fixed the clamp-suppression gap below). The ack floor replaces the former per-round all-shard fence and fold-through-fence recheck (see the History note in §3). Attempt-scoped generations (concurrent-leader safety) and the source-edge-set in-degree (H1b fix) are **DONE** (2026-07-01). Shard incarnation + registry removal (D1) is **DONE** (TLA+ gate GREEN 2026-07-01, all five implementation phases landed). Snap prune and advisory heartbeat are **DONE**.

Cross-links: `06-tla-models.md` for formal proofs · `07-s3-budget.md` for per-operation cost.

> **⚠️ Narrative-vs-code drift (retired-in-snapshot, 2026-07-11).** The prose below still describes the
> pre-refactor **three-cursor** merge and a separate **retired-list run** published via
> `gc/state.retired_refs`. As of the retired-in-snapshot refactor
> (`specs/2026-07-10-cas-retired-in-snapshot-design.md`, DONE) this is superseded: condemned state now
> rides the **source-edge run** as `kCondemned` sentinel rows (at the zero-sentinel key), the merge is
> **two-cursor** (prior run + deltas), and the fold seal carries a per-gc-shard `condemned_summary`
> (`condemned_total`, `pending_total`, `oldest_nonpending_condemn_round`, total over `gc_shards`) that
> `graduationDue` reads zero-I/O. `RetiredSet`/`retiredKey`/`retired_refs`/magic `CART` are deleted.
> Graduation is round-paced (writer condemned-detection is a per-hash `.meta` point-read), not ack-floor
> `min_ack`. The section-by-section prose refresh is tracked in `ROADMAP.md` (doc-debt TODO); read the
> code (`Core/CasGc.cpp`, `Core/CasBlobInDegree.cpp`) and the spec as authoritative until then.

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
- **Root shards** (`cas/refs/<ns>/<shard>`) are mutable CAS objects. Each carries a `RootOwnerEvent` journal, committed `RefRecord`s, and a `fence_round` birth floor (stamped once at shard creation — see §7; it is no longer bumped per round).
- **Precommit owners** are journal entries naming a manifest before its committed publish. They protect blobs during the upload window.

---

## 2. Leader election, lease, and advisory heartbeat {#leader-election}

**Status: DONE** (B160 advisory heartbeat landed and soak-validated; `CaGcLeaseCore.tla` proves safety — see `06-tla-models.md §lease-core`).

### 2.1 Lease structure {#lease-structure}

GC leader election is a **clock-free, observation-window steal** over the durable `gc/state` object. The `GcState` carries:

```
GcState {
    lease { owner: UInt128,  seq: uint64 }   // current leader identity + monotone sequence
    round: uint64                             // monotone GC round counter
    snap_generation: uint64                   // pointer to the authoritative in-degree run set
    snap_attempt: uint64                      // attempt id that produced snap_generation (attempt-scoped gen)
    snap_pruned_through: uint64               // retention cursor for old gc/gen generations
    retired_refs: { gc_shard → retired_list_object_key }   // current retired-list runs (ack-floor)
    gc_shards: uint64                         // immutable creation-time blob-target shard count
}
```

`retired_refs` maps each blob-target gc-shard to the object key of the **current** retired-list run (the outstanding-candidate set); it is published in the same `gc/state` CAS that advances `round`, so a reader that observes round K can always load the retired list of version K (the publish-order invariant, §3.6). The former per-round `fence_seq` epoch and the `fence_version` map are **removed** with the fence phase (see the History note below).

The steal is a single **atomic CAS** on `gc/state` that bumps `lease.owner` and `lease.seq` together. Safety of every round step is independent of who holds the lease; the CAS-steal merely prevents redundant work.

A follower steals only when it has observed the incumbent's `(owner, seq)` **unchanged** across its observation window AND the heartbeat counter is also frozen (see §2.2). This prevents a false steal when a leader is alive but mid-round.

### 2.2 Advisory heartbeat (B160) {#advisory-heartbeat}

**Problem solved:** a GC round can take longer than the follower's tick interval. A follower that sees `gc/state.seq` frozen (the leader only bumps it once per round) falsely concludes the leader is dead and steals — causing ~70–80% of GC rounds to abort in two-replica soaks. (The one-pass ack-floor round is far cheaper than the old fence/recheck round, but the false-steal risk is structural, not cost-driven, so the heartbeat remains load-bearing.)

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

**Status: DONE** (one-pass ack-floor round implemented on `cas-gc-ack-floor-fence`; soak-validated live on AWS S3, 2026-07-03. `CaGcAckFloorCore.tla` + `CaGcAckFloorZombie.tla` prove the ack-floor safety obligations — see `06-tla-models.md §ackfloor-core`; the fold/manifest machinery it reuses stays proved by `CaGcRootLocalPartManifestCore.tla`).

The regular round is **one pass**:

```
heartbeat ack floor → discover → fold (three-cursor merge) → pre-CAS deletes of previously-published pending → outcome logs → retired-list publish → single gc/state CAS → post-CAS cleanup
```

There is **no fence phase, no recheck phase, and no crash-resume step**. The three-cursor merge (§3.4) verifies old candidates, graduates the safe ones, and condemns new ones in a single streaming fold; the ack floor (§3.2) is what makes a delete safe without an all-shard write fence. Physical deletion is two-phase (§3.5): condemnation → `delete_pending` at the first pass whose floor passes the entry → exact-token delete the next pass. No cascade step exists (blob decrements are emitted directly from manifest fold).

> **History (superseded fence/recheck round).** Through 2026-07-02 the round was
> `discover → fold → retire → fence → recheck → exact-token delete → trim`, with a per-round
> **fence** (a CAS write to every present root shard bumping `fence_round`, recorded in
> `GcState.fence_version`) and a **recheck** (re-folding the `(folded_cursor, fence_version]` window
> per fenced shard). Both phases were O(universe) GET+CAS-PUT every round — ~2.4 M requests at
> 100k tables × 8 root shards (`07 §gc-budget`) — and the recheck's per-candidate
> `inDegreeInGeneration` re-reads were the quadratic hot spot that started the investigation. They
> were replaced by the causal ack floor + three-cursor merge. See
> `specs/2026-07-02-cas-gc-ack-floor-fence-redesign.md` for the full rationale.

### 3.1 Discovery {#discovery}

The GC leader discovers the live namespace universe by `LIST(cas/refs/)` — the set of `(ns, shard)` root-shard objects that physically exist. Each object is self-identifying (carries its `incarnation` — see §6.1).

**Registry removal (D1, DONE):** historically discovery read from `gc/registry`, an append-only authority. D1 (§7) replaces this with the LIST-based discovery and deletes the registry (`discoverUniverse` now LISTs `cas/refs/` directly). The token-diff optimization (skip body re-read when the listed shard token matches the previously folded token) is retained as a read accelerator.

This one LIST sweep is the round's only universe-proportional operation (O(universe/1000) LIST requests); everything else is O(delta) + O(servers). Discovery runs **after** the heartbeat floor (§3.1a), so any fenced-out writer's last commits are durable before the sweep enumerates them.

### 3.1a Heartbeat fence pass {#heartbeat-floor}

Before discovery, GC runs one **heartbeat fence pass** (`computeHeartbeatFloor`, `CasServerRoot.cpp`) so a superseded (expired) writer's last commits are made durable-or-fenced before the sweep enumerates them. It is the round's first step and its only use of a wall clock:

1. **Enumerate:** LIST `gc/server-roots/` + GET each mount (O(servers), single-digit counts).
2. **Classify** each heartbeat, using the injected `now_ms_fn` and `skew_margin_ms = mount_lease_ttl_ms / 2` (`live | terminated | expired`):
   - *terminated* (graceful-shutdown stamp) → excluded: its own final write is causal proof no further mutations exist.
   - *live* (`expires_at_ms + skew_margin > now`) → left alone.
   - *expired* (`now > expires_at_ms + skew_margin`, no terminated stamp) → **fence-out**: one token-guarded `putOverwrite` that preserves the body, sets `gc_fenced`, and bumps `seq`. Success ⇒ the sleeper's next renewal permanently fails (`tripMountLost`; only a full re-open with a fresh view can resume writing). `PreconditionFailed` ⇒ it renewed concurrently ⇒ re-GET and reclassify as live.

The result is a `HeartbeatFloor{live, terminated, fenced_now, already_fenced, fenced_srids}` (counts + one `GcFenceOut` audit event per fenced srid). Fence-outs must complete before discovery, so every fenced writer's last commits are durable before the sweep.

#### Fence vs decommission — a dead member's footprint outlives the fence {#fence-vs-decommission}

The fence pass settles **liveness only**: a fenced member can no longer write, but its footprint —
frozen watermark authority, stale precommits, staging debris, roots objects, and the mount/owner/epoch
slot itself — stays in the pool indefinitely (a long-absent member pinning shared floors is the safe
default). Erasing that footprint is a **deliberate operator action**:
`SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '<server_id>' FROM DISK '<disk>'` (also
`clickhouse-disks ca-drop-member`), specified in
[`specs/2026-07-13-cas-pool-member-decommission-design.md`](../specs/2026-07-13-cas-pool-member-decommission-design.md).
Decommission impersonates the dead member as a **WRITER** (it claims the victim's mount via
`Store::openForDecommission` and refuses a live one) — `GC` still never invents a ref transition.
Drain-phase failures are per-object tolerated (warned + continued) but any warning keeps the slot
**terminated, not deleted**, so a re-run re-drains; only a fully clean run retires the slot
(farewell stamp, then `epoch → owner → mount` deletion, mount last for crash-safe resume).
Known limitation (fail-closed): a mid-retirement crash on a victim that still has namespace debris
(`ref-log`/`_snap` objects not yet physically reclaimed by GC) is refused on re-run by the
`serverRootSubtreeEmpty` gate until GC's namespace-cleanup catches up — an availability narrowing,
never data loss.

**SUPERSEDED (v3 freshness-meta):** this pass used to ALSO compute a writer-ack floor `min_ack = min(observed_gc_round)` over live heartbeats, and graduation was gated on it. The `observed_gc_round` field was removed (`cas_format.proto` reserved 10 — the writer-side retired-view ack floor is gone). **Graduation now paces on GC rounds** — a retired entry graduates one round after it is condemned (via `new_round`), not on heartbeat acks — and the writer's condemned-detection is a per-hash `.meta` point-read (§3.4, `03 §merged-heartbeat`). `CaGcAckFloorZombie.tla` (`06 §area-11`) models the historical ack-floor ordering; only its GC-round-pipeline half stays current.

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

In the ack-floor round the fold does **no internal CAS**: it sets `snap_generation` / `snap_attempt` in memory and the single round CAS (§3.6) commits them. The fold also loads the prior retired list from `gc/state.retired_refs` (GET each gc-shard run; a referenced-but-missing run is `CORRUPTED_DATA` — the integrity of destructive bookkeeping, not a data-plane 404) and feeds it as the third cursor of the merge (§3.4).

**Output artifacts** (all under the attempt namespace — see §5):

```
gc/gen/<G_f>/attempt/<a>/fold_seal
gc/gen/<G_f>/attempt/<a>/blob_target/<shard>/<run>            // new in-degree snapshot run
gc/gen/<G_f>/attempt/<a>/part_manifest_cleanup/<shard>/<run>
gc/gen/<G_f>/attempt/<a>/retired/<round>/<shard>             // current retired list (published via retired_refs)
gc/gen/<G_f>/attempt/<a>/outcomes/<round>/<shard>            // per-entry delete/spare outcomes
```

The `FoldSeal` records per-`(ns, shard)` coverage: `folded_cursor` and the produced run `RunRef`s (key + footer checksum). It is written **write-once** before the round CAS; it is a deterministic artifact and any colliding write must be byte-equal or `CORRUPTED_DATA`. Completion seals are a retired concept — the fold seal alone resolves cursors now.

**Cursor advance:** the single `gc/state` CAS that advances `snap_generation` (§3.6) also advances `folded_cursor` per shard and publishes `retired_refs` and `round` — all atomically. A crash before the CAS leaves the prior generation and the prior retired list authoritative and the new attempt as invisible garbage; the next pass re-runs under a fresh attempt (§3.6 crash-resume).

### 3.2a Snapshot-run reads: streaming + reference-parent runs {#snapshot-run-reads}

**Status: DONE** (T2 streaming reads + T0 reference-parent runs, 2026-07-02; `specs/2026-07-02-cas-gc-snapshot-streaming-design.md`). The in-degree snapshot run (the per-`(generation, shard)` `RunKind::SourceEdge` artifact, §3.3) is read at **O(block) resident memory**, and an empty-delta shard reads nothing at all.

**Streaming reads (T2).** Every run consumer — the prior cursor of the three-cursor merge and the preview helpers `zeroInDegree` / `inDegreeInGeneration` — reads a run through `RunFileReader` in **streaming mode**, never materializing the whole run. Opening a run is a fixed request profile:

1. `head(key)` — the object size.
2. one ranged `get` of the tail suffix (`min(object_size, kRunHardCapBlockSize + 64 KiB)`) — carries the footer (the `footer_len` trailer + the CRC'd sparse block index).
3. `getStream(key, {header_end, data_end})` — a forward-only body stream over a **write-once** object, positioned at the first block; the 13-byte header is drained from its front so it needs no extra request.

So a linear scan is **exactly three requests** (`head` + tail `get` + body `getStream`), plus **one exact-footer ranged `get`** for a run whose footer exceeds the tail probe — a large run is four requests. Resident state is the footer index plus **one** current block (`cur_block.size() <= kRunHardCapBlockSize`); the whole-run member is gone. A `seek` after open costs one extra ranged `get` per touched block (the sparse index locates it); after a `seek`, every subsequent block comes via ranged `get` — the pure-linear fold path never seeks. The block-by-block ranged-`get` alternative for full scans was rejected (an 8 GB run is ~32 000 blocks = a per-shard request storm); it survives only as the `seek` implementation.

`getStream` is contractually for **write-once objects only** (runs, seals): their bytes cannot change under an open stream. Mutable objects (root shards, `gc/state`, mounts) stay on `get`. Fail-closed is unchanged: per-block CRC and the footer CRC verify exactly as the borrowed-memory path does; a short/truncated stream read or any CRC failure is `CORRUPTED_DATA`, never a partial parse. `RunFileWriter` is untouched, so output runs are byte-identical and `putDeterministicArtifact` semantics are unaffected.

**Reference-parent runs (T0).** When a gc-shard's delta bucket is empty for a pass, the fold **neither reads nor writes that shard's run**: the new `fold_seal` carries the parent generation's `RunRef` for that shard verbatim (key + checksum + shard + generation). `RunRef` gains explicit `shard` (which gc-shard the run belongs to) and `generation` (whose key namespace physically holds the object) fields — both additive proto — so per-shard association and retention never parse key paths. This is deterministic by construction (same inputs ⇒ same refs), so seal determinism and crash-replay adoption are unchanged.

A shard with an empty delta AND an empty retired list is **pure ref-carry: zero run I/O**. A shard with an empty delta but a non-empty retired list still runs the merge with empty deltas — settlement of retired entries must happen every pass — so the ref-carry shortcut requires **both** buckets empty. A fully idle round (no journal changes anywhere, no retired entries) therefore touches **zero** run objects: no GET, no PUT.

**Consumers resolve runs through seal refs, never by key construction.** Because the run for generation `G` may physically live under an older generation's key namespace, no consumer builds a run key from `(generation, shard)`. The fold reads the parent seal's `blob_target_runs` and resolves each shard's run through the seal's `RunRef`s; `previewDeletes` resolves `gc/state → adopted seal → refs`. See §4.3 for how retention keeps a referenced older-generation run alive.

### 3.3 In-degree representation: source-edge set {#indegree-source-edge-set}

**Status: DONE** (H1b fix, 2026-07-01; `CaGcIndegRefoldCore.tla` validated the fix — see `06-tla-models.md §indeg-refold`).

**The persisted per-`(generation, shard)` artifact is `(blob_hash → set of active source edges)`, not an integer refcount.** A source edge is identified by `(ManifestId, path)`; its id is `sourceEdgeId(ManifestId, path)` (a stable deterministic 16-byte id). The run key is `(blob_hash BE-16 ++ source_id BE-16)` — lexicographic order is `(blob_hash, source_id)`.

Fold is **idempotent**: activating a source edge inserts it into the set (union); removing one deletes it from the set (set-difference). Re-folding either operation is a no-op. This structural idempotency eliminates the entire class of integer-refcount underflow bugs (H1b, H2 — see below under REJECTED).

`in_degree(blob) = |edge_set(blob)|`, computed transiently within a round from the run; never persisted as a separate counter.

`zeroInDegree` = blobs whose edge set became empty **this generation** (an explicit zero-transition marker row is written; prior-generation zero markers are dropped). Only blobs with an explicit zero-marker are GC candidates; blobs that never appeared as edge targets are never candidates (`INV_JOURNAL_COVERAGE` — a blob is only a candidate if GC ever saw it as a reference target).

**REJECTED: integer refcount.** The implementation previously persisted `(blob_hash → int64 count)` and folded as `prior_count + Σ ±1`. This is **not** idempotent: re-folding a removal (H1b — a fence-window removal whose cursor was not advanced past the fence) drives the count to −1, triggering a fail-closed `CORRUPTED_DATA` throw and wedging GC. The model `CaGcRootLocalPartManifestCore.tla` never caught this because it uses set-based idempotent in-degree — the implementation diverged from the model. The source-edge-set design is what the big model actually proves; the change makes the implementation faithful to it. See `06-tla-models.md §indeg-refold` for the focused model that validated the fix.

### 3.4 The three-cursor merge (verify, graduate, condemn) {#three-cursor-merge}

The heart of the ack-floor round. Per gc-shard, one streaming pass extends the existing two-cursor `foldDeltasIntoGeneration` (prior snapshot run + sorted incoming edge deltas) with a **third cursor over the prior retired run** — all three inputs sorted by `blob_hash`. The single pass emits the new snapshot run, the new retired run, and the delete list. It **verifies old candidates, graduates the safe ones, and condemns new ones at once**, replacing the separate retire + fence + recheck phases.

Let `min_ack` be the floor latched in §3.1a and `condemn_round = state.round + 1`. Per blob, at the merge point:

| In-degree | Retired? | Action |
|-----------|----------|--------|
| `> 0` | yes | **spare** — drop the entry; emit a B170 recheck-verdict event with the recovered in-degree. |
| `= 0` | retired, `condemn_round < min_ack` | **graduate** — mark the entry `delete_pending` and re-publish it (two-phase, below). |
| `= 0` | retired, `condemn_round ≥ min_ack` | keep the entry unchanged (not yet provably seen by every live writer). |
| `= 0` | not retired | **condemn** — `HEAD` the blob to capture its current token (absent ⇒ nothing to delete, skip); append `(hash, token, condemn_round)` to the retired output. |

Because `min_ack ≤ round − 1` always (acks cannot exceed the last published round), an entry condemned in this pass structurally cannot graduate in the same pass — the two-round pipeline falls out of the arithmetic, with no explicit rule.

**Clamp suppression (`suppress_destructive`, 2026-07-03):** the table above is overridden pass-wide the instant the fold recorded **any** clamp anomaly (§absent-at-head): a `suppress_destructive` flag is threaded `Gc::runRegularRound` → `foldDeltasIntoGeneration` / `ShardReducer::reduce` → `settleEntry`. While set, no entry graduates to `delete_pending` and no already-`delete_pending` entry is re-delivered for `deleteExact`; every retired entry (pending or not) is carried unchanged to `still_retired`. Condemning and sparing are unaffected. This restores the ack-floor lemma "landed before the cut ⇒ folded before graduation," which a clamped shard's frozen cursor otherwise violates — found live in the night soak as 31 dangling blobs. Deletes resume on the first clamp-free pass.

**Condemn fail-closed (absent-at-condemn):** a blob already absent when its `HEAD` returns 404 records **nothing** — there is no token to condemn. GC must **never fabricate a token** on a 404: a synthetic token would let a stale exact-token delete match a future unrelated incarnation reusing the same hash, violating `INV_NO_RETURN`.

**Recovery wins over everything, `delete_pending` included** (fail-closed spare): a retired entry whose in-degree recovered to `> 0` is spared even if it was already `delete_pending`. A pending entry observed with recovered in-degree is structurally impossible (floor-passed ⇒ every live writer sees it condemned ⇒ no new reference), so it is spared *and* logged loudly.

The retired run is an **observation-bearing artifact**: the token is the value this pass's `HEAD` observed; two leaders may observe different tokens if a re-incarnation happened between their HEADs. The first durable write wins (read-if-present, not byte-equal-or-`CORRUPTED_DATA`). Once an entry is condemned, GC writes a per-hash `Condemned` `.meta` marker (`writeCondemnedMeta`, `CasGc.cpp`) on the bounded `gc_meta_pool_size` job pool — the writer's dedup/reuse gate point-reads it via `loadMeta` (`CasBlobMeta.h`) and treats `Condemned` as `ABORTED` → re-upload-from-source (`INV-1`, `CasBuild.cpp`), rather than referencing the condemned blob. There is no round-indexed `RetireView`; the meta is a 2-state freshness marker (`Clean` / `Condemned`), not a linearization point.

**Missing manifest-body policy (inherited from fold, unchanged):** a missing/invalid committed-or-promoted new-binding body **clamps** the affected shard (fail-closed, surfaced to `fsck`, **not** spare-by-default); a missing precommit body is non-activating; an old-binding removal uses edges already sealed at fold and never reads a deleted body. Clamped coverage is now first-class (`ShardCoverage classification = 4`) and forbids the token-diff Skip: a barrier-clamped shard whose listed token never changes (the missing precommit body arrives via a manifest PUT, not a shard rewrite) must still be re-read, or its edges are lost forever. This was a real regression found while porting the merge.

### 3.5 Two-phase graduation (`delete_pending`) {#two-phase-graduation}

Physical deletion is **two-phase**, and this is what makes a deposed (zombie) leader's fresh decisions harmless. The single-CAS round removed the old protocol's incidental zombie ejection (a deposed leader used to fail one of the ~4 per-phase CASes long before reaching a delete). A deposed leader running the one-pass round could otherwise graduate from a *stale* retired list — an entry spared by the new leader and re-condemned at `r' ≥ min_ack` would look floor-passed under its old `r`.

The fix: graduation is two-phase.

1. A pass that first floor-passes an entry marks it `delete_pending = true` and **keeps it in the published list** (writers still see it condemned → recreate).
2. `deleteExact` executes only for entries that were **already published as pending in the previous list version**, strictly **before** this pass's CAS (§3.6, "pre-CAS deletes").

Pending is terminal — no spare is possible (floor-passed ⇒ every live writer sees the entry ⇒ no new reference), so re-executing pending deletes is safe at **any** staleness; a zombie's fresh decisions never survive its failing CAS. Crash-safety is leak-free: a crash before the CAS leaves the prior list still pending → the next pass re-issues. Physical deletion therefore lags condemnation by one extra pass (condemn K → pending at the first floor-pass → deleted the next pass) — immaterial in practice. This is strictly more conservative than the TLA+ `GComplete` (which deletes at graduation) and implements the model's drop-on-confirmed-outcome discipline (`06 §ackfloor-zombie`).

### 3.6 Deletes, publish, and the single CAS {#deletes-publish-cas}

The round tail is a fixed sequence of one destructive phase, artifact publication, and exactly one CAS.

**Pre-CAS deletes (the single content-delete site).** For each entry that was `delete_pending` in the *previous* published list, `deleteExact(blobKey, token)` uses `If-Match`:

- `Deleted` → done.
- `TokenMismatch` (412) → a writer recreated the blob → done (the fresh incarnation is a live object; a future round re-condemns it if unreferenced).
- `NotFound` → already deleted (crash-resume replay) → done.

Each emits a B170 `BlobDelete` + `GcRecheckVerdict` outcome (`Deleted` / `Replaced` / `Absent`). Spared entries emit `Spared` + a verdict event (and a loud WARNING if the spared entry was `delete_pending` — structurally impossible). Manifest-body cleanup (`mfCleanup`, delete-after-adopted-decrements) rides this phase unchanged. These deletes are justified by **previously published** state only, so replay under a fresh attempt is idempotent — which is why no separate crash-resume step exists.

**`created_delete_marker → LOGICAL_ERROR` (per-delete versioning guard):** if a `deleteExact` outcome reports `created_delete_marker = true`, the backend created a versioning tombstone instead of a real delete — the bucket is versioning-enabled, which the startup probe (`01 §backend-contract`) should have rejected. GC throws `LOGICAL_ERROR` fail-closed: a delete-marker regime could resurrect an exact-token-deleted object (`INV_NO_RETURN`).

**Outcome logs & retired-list publish (publish-order invariant).** Outcome logs (observation-bearing ⇒ `putIfAbsent`-adopt) and the per-gc-shard retired-list runs (written **always**, even empty, at `retiredKey(generation, attempt, round, shard)` via `putIfAbsent` + byte-adopt) are durable **before** the CAS. A reader that observes round K can therefore always load retired list K. This subsumes the old retire-visibility barrier / `ViewableRound`.

**The single `gc/state` CAS.** One lease-token CAS publishes, atomically: `round := K`, the adopted `(snap_generation, snap_attempt)`, `retired_refs` (the new per-gc-shard run keys), the per-shard folded tokens/cursors, and `snap_pruned_through`. Superseded-generation prune (§4.2) runs before the CAS (zombie-safe). Failure ⇒ `ABORTED "retry next round"`. This is the **only** CAS per round (the old protocol used ~4: fold-adopt, retire, fence, seal).

**Post-CAS cleanup.** Manifest-body deletes for adopted decrements, `reclaimDroppedShards`, trim (§3.8), and the orphan manifest sweep cursor pass all run after the CAS, exactly as before.

**Crash-resume (no explicit step).** Attempt-scoped write-once artifacts mean a crashed pass leaves only never-adopted debris (pruned by retention). A new leader (or the same one) re-runs the pass under a fresh attempt; already-executed deletes land on the `NotFound` branch; only the adopted attempt is reader-visible. The former `tryResumeIncompleteRound` and completion seals are gone.

### 3.7 Part-manifest cleanup {#manifest-cleanup}

Part manifests (`_manifests/...`) are deleted when their owning reference is removed. The part-manifest cleanup bundle records `ManifestId`s whose owner was removed and whose blob decrements were sealed into the generation. The delete is an exact-token `deleteExact` issued in the post-CAS cleanup phase (§3.6), gated by the same fold ordering as blob deletes — delete-after-adopted-decrements, one window now (the old recheck double-window skip is gone).

A part manifest has at most one structural owner at any time (`SingleManifestOwner`). An owner removal that leaves no successor is a true removal → the manifest body is debris after its blob decrements are sealed.

**Orphan part-manifest sweep** (pre-precommit debris): manifest bodies written before `PrecommitAdd` that were never activated have no owner. A bounded background sweep (one namespace, one eligible build prefix per round) enumerates and deletes them by exact token after confirming they are absent from the namespace's sealed owner view. Eligible build prefixes are those whose `writer_instance_id`/`build_sequence` the watermark confirms can no longer write (e.g. `min_active > build_sequence`). See `CaGc.cpp::orphanPartManifestSweep`.

### 3.8 Trim {#trim}

After the round CAS is durable, `trim` removes journal records from root shards that are at or below the committed `folded_cursor` (there is no separate completion seal any more; `trim` reads the fold seal). This is `INV_JOURNAL_COVERAGE`: only records durably represented in the committed snap generation may be trimmed.

**Lazy-trim gate (B12):** trim does not compact a shard's journal on every pass it could. A shard is compacted only when the trimmable-event count reaches `gc_trim_min_events` (default 256; 0 = eager, always compact) **OR** the shard's encoded body size reaches `gc_trim_body_soft_limit` (default 8 MiB, a backstop that bounds journal growth even if the count gate never fires) — plus an explicit one-round maintenance-compaction bypass. Below both gates the shard is skipped so its listed token stays stable for the discovery token-diff Skip.

**B140-dangle HISTORY:** the original implementation stored snap edges and `folded_cursor` as two separately-durable objects with no enforced coherence. A lease-steal between the snap write and the `gc/state` CAS could leave `folded_cursor` ahead of the actual snap coverage → `trim` over-trimmed → a live part's edge was permanently lost → in-degree undercount → data loss (`fsck dangling`). The fix (v2, `2026-06-18`) embeds the fold cursor inside the snap codec so the two are always co-durable. The `CaB140DangleMerge.tla` model proved the fix closes the dangle. See `06-tla-models.md §b140-dangle`.

### 3.9 Resident-snap incremental GC and the durable-vs-resident cursor {#resident-snap-checkpoint}

**Status: SUPERSEDED** (2026-07-02 ack-floor one-pass rewrite). This section originally described a
resident, incrementally-folded `GcSnap` kept in the long-lived per-leader GC object and persisted only
at a checkpoint gated by `gc_checkpoint_records` / `gc_checkpoint_rounds`, with two cursors
(resident vs. durable `folded_cursor`) reconciled at checkpoint time. That machinery does not exist in
the current implementation: there is no resident `GcSnap`, no checkpoint gate, and no
resident/durable cursor split. Every round now does a full attempt-scoped fold plus exactly one
`gc/state` CAS — see §3.2/§3.6 for the current fold + publish model and §5 for the attempt-scoped
artifact layout that replaced incremental checkpointing.

---

## 4. Snap prune {#snap-prune}

**Status: DONE** (B174 snap prune, `gc_snap_generations_to_keep`, soak-validated; `CaGcRootLocalPartManifestCore.tla` is unaffected — prune is pure space reclamation).

### 4.1 Delete-time and retire-404 node pruning {#node-pruning}

**Status: SUPERSEDED.** This section originally described `GcSnap::forget(kind, hash)` (P9), which
removed a node from a persisted `known` candidate-node set the instant GC confirmed it was gone
(delete-time prune + condemn-404 prune), to stop ~46k re-`HEAD`s per idle round against nodes GC had
already deleted. That machinery — `GcSnap`, its `known` set, and `forget` — does not exist in the
current implementation (superseded by the source-edge-set model, §3.3). The source-edge-set model has
**no persisted node registry to forget from**: GC candidates are derived transiently, per generation,
from the explicit zero-transition marker rows the fold emits (§3.3) — a blob that is not a
zero-transition candidate this generation is never re-`HEAD`ed, with no separate forgetting step
needed. The re-`HEAD`-storm problem P9 solved no longer applies under this model.

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

### 4.3 Ref-aware retention + hand-off delete {#ref-aware-retention}

**Status: DONE** (T0 reference-parent runs, 2026-07-02; `specs/2026-07-02-cas-gc-snapshot-streaming-design.md`). Reference-parent runs (§3.2a) mean the live adopted seal may reference a run object that physically lives under an **older** generation's key namespace. Wholesale generation prune (§4.2) must not delete such a run out from under the live seal.

**Retention skip (generation granularity).** `pruneSupersededGenerations` takes the set of generations still referenced by the adopted seal's `blob_target_runs` and **skips** any such generation in its wholesale-delete loop (one log line per retained generation). The skip is at **generation granularity**: the prune cursor is a monotone high-water mark over every generation it *visits*, retained ones included. The loop starts at `snap_pruned_through + 1`, runs `g <= prune_floor`, and on a referenced generation does `continue` without deleting — but `g` still increments, and after the loop `snap_pruned_through = g − 1`. Consequence: once a retained generation is behind the cursor, the wholesale prune **never revisits it**.

**Post-CAS hand-off delete.** Because the cursor advances past a retained generation, the run that finally REPLACES a shard's parent-ref must clean up the whole superseded generation, not just its one carried run object. In `runRegularRound`, for every parent ref whose `generation <= snap_pruned_through` (already behind the cursor) and whose generation no NEW live ref references, the round wholesale-deletes that generation's entire `gc/gen/<g>/` prefix **post-CAS** (best-effort; `NotFound` / `TokenMismatch` tolerated). A single-run hand-off would permanently leak the rest of that generation's prefix (fold seal, attempt subtree, retired/outcome sets, other shards' runs), so the whole-prefix delete is the one that matches the generation-granularity skip.

**Leak-freedom.** Every formerly-referenced generation is eventually fully reclaimed by exactly one of two disjoint paths: (1) the ref moves off it **before** the cursor reaches it → the normal wholesale prune reclaims it when it ages past `keep`; or (2) the cursor passes it while it is still referenced (skipped) → the round that finally moves the ref off it hand-off deletes its whole prefix post-CAS. The single-crash window between the CAS and the hand-off strands at most one generation's prefix and is **fsck-visible best-effort**, like the rest of post-CAS cleanup (§3.6); a plain retry does not re-attempt it (the cursor already advanced), so fsck is the backstop — no permanent leak.

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
gc/gen/<G_f>/attempt/<a>/outcomes/<round>/<shard>
```

No artifact is written to a final `gc/gen/<g>/…` key outside an `attempt/` prefix before its adopt CAS. Two competing leaders for the same generation write to **different** attempt prefixes (`lease.seq` is unique per steal). Divergent-run and divergent-seal collisions are structurally impossible.

**Round lifecycle (one pass, one CAS):**

1. **Fold + merge (hidden).** The leader latches the ack floor, then writes all attempt artifacts under `gc/gen/<G_f>/attempt/<a>/` — the fold seal, the new in-degree snapshot runs, the retired-list runs, and the outcome logs from the three-cursor merge (§3.4). Nothing is reader-visible. Competing leaders write under their own `a`.
2. **Pre-CAS deletes.** Executed only for entries **previously published** as `delete_pending` (§3.5) — justified by prior committed state, so idempotent across attempts.
3. **The single adopt CAS.** One lease-token `gc/state` CAS sets `snap_generation = G_f`, `snap_attempt = a`, `retired_refs`, `round := K`, and the folded cursors atomically. Commits iff the leader still holds the lease; a deposed leader's CAS fails and its entire attempt is unadopted garbage. This replaces the old two CASes (fold-adopt + completion advance) and the intervening fence/retire CASes.

**Artifact-class rule** (how concurrent writes to the same attempt key are reconciled):

- **Deterministic artifacts** (in-degree runs, fold seal): byte-reproducible from their inputs. Any producer finding one present must find it **byte-equal → adopt** (deterministic replay) or **divergent → `CORRUPTED_DATA`** (fail-closed, impossible under correct operation).
- **Observation-bearing artifacts** (retired-list runs, outcome logs): carry HEAD-observed tokens that two observers may legitimately differ on (a re-incarnation between their HEADs). **First-durable-write wins** (read-if-present). A later producer reads the present artifact and uses it as authority; it never recomputes-and-compares.

**Publish-order invariant (subsumes `ViewableRound`):** the retired-list runs (and the snapshot runs) for round K are durable **before** the single `gc/state` CAS that publishes `round := K` and `retired_refs`. Because the refs and the round land in the *same* CAS, a reader that observes round K can always load the complete retired list of version K — there is no window where the round advances ahead of its retired-token set. This is the operational statement of `INV_ONLY_ADOPTED_VIEWABLE`; the old requirement of "advance the round only after every shard's retired set is durable" is now enforced by the single-CAS structure itself.

**Crash-resume (no explicit resume step):** the former `tryResumeIncompleteRound` is removed. One CAS per round means a crash leaves only never-adopted attempt-scoped debris (pruned by retention). The next pass re-runs from scratch under a fresh attempt; already-executed pre-CAS deletes replay idempotently on the `NotFound` branch because they were justified by previously-published state.

**Pruning** (space reclamation, not safety-load-bearing):

1. *Generation retention* (§4.2) deletes `gc/gen/<g>/` wholesale when `g < retention_floor` — all attempts of obsolete generations.
2. *Attempt orphan sweep* looks at `gc/gen/<snap_generation>/attempt/*`, skips `snap_attempt`, deletes attempts with `seq < min_live_lease_seq`. Bounded per round, fail-open, exact-token.

**TLA+ reference:** `CaGcRootLocalPartManifestCore.tla` `_sab_deposedleaderwritesfinalgen.cfg` — a deposed leader writes artifacts to a final gen key; this **must** produce a counterexample (`INV_ONLY_ADOPTED_VIEWABLE` or an R0 safety violation). The design config holds all R0 invariants. See `06-tla-models.md §root-local-manifest`.

---

## 6. Orphan removal {#orphan-removal}

### 6.1 Blob orphan removal {#blob-orphan-removal}

**Status: DONE** (via the normal round — the three-cursor merge condemns zero-in-degree blobs and, after the ack floor passes them, graduates and deletes them, §3.4–3.6).

A blob becomes an orphan when all manifests referencing it are removed (their edges are folded out). The fold emits explicit zero-transition markers for such blobs; the three-cursor merge (§3.4) condemns them (capturing the current token) and, once the ack floor passes them, graduates them to `delete_pending`; the pass after that deletes them exactly (§3.5–3.6).

The only path to blob deletion is through this pipeline. There is no direct delete bypassing the merge + two-phase-graduation gates.

### 6.2 Part-manifest orphan removal {#manifest-orphan-removal}

**Status: DONE** (owner-driven cleanup in part-manifest cleanup bundle; orphan sweep for pre-precommit debris is DONE).

A part manifest is orphaned when its owner (committed ref or precommit) is removed. The fold emits its `ManifestId` into the part-manifest cleanup bundle; the post-CAS cleanup phase (§3.6) issues `deleteExact` for it after the adopted decrements confirm no concurrent re-attachment.

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

**Status: TLA+ gate GREEN (2026-07-01); implementation phases 1–5 DONE** (Tasks 2–6: `ShardIncarnation` stamped on `RootShard`, `discoverUniverse` LIST-based over `cas/refs/`, `RootsRegistry` deleted, the newborn self-floor in `CasBuild.cpp`, and `Gc::reclaimDroppedShards` tombstone reclaim are all in code). `CaGcShardIncarnationCore.tla` `_design.cfg` holds `INV_NO_DANGLING` + `INV_NO_ORPHAN_EDGE` across 724,944 distinct states. See `06-tla-models.md §shard-incarnation`.

### 7.1 Problem: `dropNamespace` never deregisters {#drop-namespace-problem}

Before this fix, `dropNamespace` (`CasStore.cpp:1392-1452`) tombstoned each touched shard (appends removal `RootOwnerEvent`s, clears refs via `mutateShard`), but left the namespace in `gc/registry`. GC discovered from `registry.namespaces × shardsToVisit` and fenced the full cartesian product — so per-round GC cost was proportional to **every table ever created**, not the live ones. The empty shard objects and registry entries accumulated without bound (S30 scenario). A scalability defect, not a correctness bug.

### 7.2 Rejected fixes {#d1-rejected}

- **Writer deregisters at `dropNamespace`:** unsafe. The removal `RootOwnerEvent`s carry the `−1` blob-in-degree edges; GC folds them only in the shard's fold window `(folded_cursor, current_shard_version]`. If the namespace vanishes from discovery before GC folds that window, the `−1`s are lost → blobs keep phantom in-degree → permanent leak.
- **GC infers "empty ⇒ retire":** a freshly-created table with no inserts yet is indistinguishable from a dropped empty one.
- **Path-keyed cursor + delete-and-recreate:** ABA hazard. A recreated shard resets `shard_version` to 0; the old sealed `folded_cursor = K` silently skips events at versions `1..≤K` → lost edges → dangle or leak.

### 7.3 Design: incarnation + LIST-based discovery {#d1-design}

Two orthogonal coordinates replace all five overlapping counters:

**Coordinate 1 — `incarnation`.** A durable, monotone, never-reused value stamped into a `RootShard` at its (re)creation and immutable for that object's life. Source: `(writer_epoch, build_sequence)` of the build that first creates the shard, both already durable-monotone-never-reused. On delete + recreate, the new create stamps a strictly-greater coordinate. **`INC-MONO`**: for a fixed `(ns, shard)`, every successive materialization carries a strictly greater incarnation.

**Coordinate 2 — GC `round`.** The pool-global clock (unchanged). Writer-visible condemnation is a per-hash meta point-read (`BlobMeta.state`, §3.4), not a round-indexed floor a writer refreshes: a writer that observes `Condemned` via `loadMeta` re-uploads from source (`INV-1`) rather than referencing the blob. `BlobMeta.condemn_round` is carried in the meta body but is only an ABA guard against a stale spare-then-recondemn race, never a writer-visible floor. `round` itself stays pool-global and cannot be per-`server_root`.

The fold cursor is keyed by `(ns, shard, incarnation)` instead of `(ns, shard)`. A recreated shard draws a strictly-greater incarnation → the old sealed cursor never matches → fold always processes a new incarnation from zero. ABA is closed by construction.

**Discovery replaces the registry:** `discoverUniverse` becomes `LIST(cas/refs/)`. The registry (`gc/registry`, `RootsRegistry`, `rootsRegistryKey`) is deleted entirely. `listNamespaces` (used for FREEZE shadow-tree enumeration) migrates to a `LIST` over `cas/refs/`.

**Newborn namespace ordering (no separate object):** the registry's irreducible role was ordering a first publish that dedup-references an existing blob against a concurrent last-drop-and-GC. D1 replaces this with the existing **precommit machinery**: a first publish creates the ref-shard object at its canonical path carrying a precommit binding (with the fresh incarnation). The shard is LIST-discoverable immediately. The precommit gate (fold barrier + watermark reclaim) provides create-ordering without a separate pending-newborns object. **THM-NO-RETURN (central):** with the registry removed, for a first publish that dedup-references blob `B`, either (a) the newborn shard is in GC's LIST universe and GC folds its precommit `+1` before condemning `B`, or (b) the writer's meta point-read (`loadMeta`) observes `B` as `Condemned` and re-uploads from source (`INV-1`) rather than referencing the condemned blob. No interleaving can dangle a live ref. This is the theorem the TLA+ gate proved.

**Shard-object reclaim:** `dropNamespace` appends, as its last journal event, an explicit **tombstone** marker (`RootOwnerEvent` variant meaning "namespace dropped, no owner"). GC, during fold, when a shard is empty (no refs), its last journal event is the tombstone, and its journal has been folded past a completed fence, issues `deleteExact(rootShardKey(ns, shard), token)`. Any writer append (revive) changes the token → `deleteExact` returns `TokenMismatch` → the delete is refused; the object survives with new content. An idle-but-live shard (all parts dropped, table alive) has no tombstone → not reclaimed → still discovered. The tombstone is the drop signal.

**TLA+ validation:** `CaGcShardIncarnationCore.tla` `_design.cfg`. Three negative controls confirm irreducibility: `SabotageNewbornNoFloor` (round self-floor is irreducible), `SabotagePathKeyedCursor` (incarnation is irreducible — ABA without it), `SabotageDeleteBeforeFold` (fold-before-reclaim ordering is irreducible). **The one-vs-two coordinates question is answered: two** — neither coordinate alone suffices.

---

## 7a. GC baseline guard + raw rebuild {#gc-rebuild}

**Status: DONE (2026-07-03).** Spec: `2026-07-03-cas-gc-rebuild-design.md`. Answers the question every
prior section assumes away: what happens when `gc/state` itself is lost or corrupted (an operator
`mc rm`, a botched migration, backend corruption) over a pool whose per-shard journals have already
been **trimmed** past the point a from-scratch fold could recover? Without a guard, a fresh
`runRegularRound` would fold only the surviving journal tails, under-count every blob referenced by
trimmed history, and mass-delete live data — the one class of event `INV_NO_LOSS` cannot survive
without an explicit baseline.

**The guard (Part 1 — ships ahead of the rebuild, fails closed by default).** Before folding, the
round classifies `gc/state` as healthy or not: healthy means it decodes AND, when it claims a
baseline (`snap_generation > 0`), the fold seal for that `(generation, attempt)` is present AND every
run/retired-list it references is HEAD-present. A `gen == 0` state additionally requires that **no**
shard journal proves trimmed history (`journal.empty() && shard_version > 0`, or a surviving
journal's earliest record has `transition_version > 1`) — a fresh-pool-shaped state sitting over
trimmed journals is exactly the disaster this guard exists to catch, not a legitimately-empty pool.
Any journal proving trim with no adopted seal under live state throws `CORRUPTED_DATA` and aborts
the round — never a silent under-count.

**The rebuild algorithm (Part 2 — `Gc::rebuildBaseline(bool force)`).** Reuses the round's own
bricks; no new scanner or merge class:

1. **Health check** (the same classification above) — refuses with `performed=false` unless `force`
   is set, so an operator never discards live bookkeeping by accident.
2. **Lease** — `acquireOrRenewLease` (Part 1's protocol, unchanged): refuses if another leader (a
   regular round, or a concurrent rebuild) currently holds it.
3. **Numbering** — mint a `generation` strictly above every surviving `gc/gen/<n>/` prefix (so
   `putDeterministicArtifact` can never collide with debris from the lost era).
4. **Universe replay** — `discoverUniverse` (the same `LIST(cas/refs/)` sweep the regular round
   uses) drives, per shard: committed refs (`root.refs` is the authoritative committed state) →
   `foldManifestEdges(+1)`; live precommits (journal replay: apply `old_binding` erases / `new_binding`
   inserts, keep what remains `Precommit`) → edges when the body is present, else clamp the shard's
   cursor below that transition (the fold-barrier semantics from §3.2).
5. **`foldDeltasIntoGeneration`**, attempt-iterated per gc-shard bucket (empty priors, budget-bounded
   memory — the same primitive the regular round's three-cursor merge uses), builds the rebuilt
   in-degree runs and the fold seal.
6. **A rebuild condemns nothing** (spec `2026-07-27` §7). It rebuilds cursors and edges; it reclaims
   no object of any kind. Earlier builds ended the pass with a `LIST(blobs/)` that condemned every
   physically-present body outside the rebuilt edge set — the reasoning being that a blob whose edges
   were already gone by rebuild time never transitions to zero and so has no row the fold could ever
   settle. The premise was that a full traversal knows every live blob, and it is false: **both** legs
   of the traversal (step 4's owner replay and the trimmed-but-live manifest pass below) are
   listing-driven, so a store that omits a durable key from one enumeration hides a **live** owner and
   the same pass then condemns the blob that owner pins. That is r5-finding-4 — one lying enumeration,
   and acked data is scheduled for deletion. Omission is not hypothetical: it is the observed
   `0x1430c` shape that made every ref walk arithmetic in the first place.
7. **Round mint** — `round = max(heartbeat-floor max_ack, max shard fence_round seen, the old
   state.round, max_gen) + 1`. Minting above every surviving mount ack (not just the old round) closes
   a skew hazard: a stale mount's ack could otherwise float a fresh condemnation past its own floor
   before that mount re-observes the rebuilt list.
8. **Single CAS** — one `gc/state` write publishes the new `(round, generation, attempt, retired_refs)`
   atomically, same as a regular round. A refused rebuild (any step above) writes nothing; `gc/state`
   is untouched.

**Over-protection, not under-protection (design delta 2 — a deliberate, bounded leak class).** A
build alive across trim has no journal evidence (the evidence was trimmed); its manifests look
unowned. The rebuild cannot tell "unowned because dead" from "unowned because trimmed-but-live", so
it includes edges for every **unowned** manifest that is **not provably build-dead** (the live
watermark fact) — i.e. it over-protects. An unowned manifest that later dies with no journal evidence
leaks its blob's edges until a future rebuild reclaims it. This is documented, bounded (one
rebuild-to-rebuild window, `fsck`-visible as `unaccounted`), and strictly preferred to the
alternative: `INV_OVER_COUNT_ONLY` — over-count, never under-count — is the invariant a
disaster-recovery tool must never violate.

**Refusals (all fail-closed, all write nothing):**
- gc/state and every referenced artifact are already healthy (bypass with `FORCE`).
- another GC leader holds the lease.
- a **committed** ref names a missing or invalid manifest body — real data loss the rebuild refuses
  to bless; the refusal names the offending owner so `fsck` forensics can follow up before anyone
  reaches for `FORCE`.

**The two command forms** (§8/§testing tie-in: `08-testing-and-soak.md §gc-rebuild-runbook`):
- `SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE] [<disk>]` — on any live replica; synchronous, one
  rebuild attempt, throws with `report.refusal` on refusal, `LOG_INFO`s the report's counters on
  success.
- `clickhouse-disks --disk <ca> ca-gc-rebuild [--force]` — when no server is up; requires the disk to
  be opened `<readonly>true</readonly>` (same rule as `fsck`/`ca-gc-dryrun`: this tool must never
  claim a live server's mount), prints the report as `key=value` pairs, exits nonzero on refusal.

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
| Heartbeat floor | O(servers) GETs (LIST `gc/server-roots/` + GET each); rare fence-out PUT |
| Discover | 1 LIST sweep of `cas/refs/` (token-diff: skip GET if token unchanged) |
| Fold + three-cursor merge | 1 GET per changed root shard; 1 GET per prior retired run; 1 HEAD per newly-condemned candidate; 1 PUT per in-degree run + 1 PUT per retired run + 1 PUT fold seal |
| Deletes | `deleteExact` per previously-published `delete_pending` entry (free on AWS) |
| Snap prune | `snap_shards × (HEAD + DELETE)` per pruned generation (steady state: 1 generation/round) |
| Round CAS | 1 `gc/state` CAS-PUT (the only CAS per round) |
| Heartbeat | merged into the writer's mount-lease beat (`03 §merged-heartbeat`): +1 `gc/state` GET, −1 PUT per beat |

The ack-floor round is **O(delta) + O(servers)** requests plus the single LIST sweep — no O(universe) GET or PUT phase exists. This is the whole point of replacing fence+recheck; the per-round cost dropped from ~2.4 M requests to ~2 000–3 000 at the 100k-tables example. See `07-s3-budget.md §gc-budget` for the full breakdown.

**Node-forgetting impact (SUPERSEDED):** this line originally credited a `GcSnap::forget` node-forgetting mechanism (P9) with eliminating ~46k re-`HEAD`s per idle round against previously-deleted candidates. That mechanism does not exist under the current source-edge-set model (§3.3, §4.1) — there is no persisted node registry to prune, so the re-`HEAD` storm it solved does not arise in the first place (candidates are derived transiently from zero-transition markers each generation, §3.3). The `gc/` prefix's reduction from 82% of pool storage to a bounded sawtooth is generation retention (§4.2, B174), not node-forgetting.

---

## 11. Testing {#testing}

**Status: comprehensive gtest suite exists; soak S04/S33/S03/S11 all drain to `fsck unreachable=0, dangling=0, gc_residual=0` with the current binary.**

Key test files:
- `src/Disks/tests/gtest_cas_gc_ack_floor.cpp` — the `CasGcAckFloor` protocol suite (condemn → pending → delete pipeline, `NoOpRoundDoesNotMutateRefShards`, stale-ack-holds-the-floor, pre-ack publish spares, expired-mount fence-out, recreated-blob-delete-is-`TokenMismatch`-ok); ported from the old `gtest_cas_gc_fence_recheck.cpp`
- `src/Disks/tests/gtest_cas_mount.cpp` — `CasHeartbeatFloor` (`computeHeartbeatFloor` classification + fence-out), `CasHeartbeat` (merged keeper)
- `src/Disks/tests/gtest_cas_store.cpp` — `CasStoreBeat` (ack advances only after view load; drain blocks ack while a mutation is in flight; a `gc/state` read failure leaves the ack unchanged)
- `src/Disks/tests/gtest_cas_blob_indegree.cpp` — `CasThreeCursorMerge` (spare / graduate→pending / condemn rules, the `condemn_round = min_ack−1 / = min_ack` boundary)
- `src/Disks/tests/gtest_cas_gc_undercount_repro.cpp` — H1b regression; GREEN after source-edge-set fix
- `src/Disks/tests/gtest_cas_gc_snap.cpp` — node-forgetting / snap-prune unit tests
- `utils/ca-soak/scenarios/` — adversarial scenario suite (S01–S35); S30/S34/S35 test D1 create/drop churn; S33 tests concurrent GC leaders

**Known blind spot:** tests running with `gc_shards=1` (the default) do not exercise sharded bugs. All new fold/discovery tests added for D1 must also run with `gc_shards > 1`.

---

## 12. DONE / TODO / REJECTED / DESIRABLE {#status}

| Area | Status | Note |
|------|--------|------|
| One-pass ack-floor round (floor → three-cursor merge → single CAS) | **DONE** | `cas-gc-ack-floor-fence`; soak-validated live on AWS S3, 2026-07-03 |
| Heartbeat ack floor + expired-mount fence-out (`computeHeartbeatFloor`) | **DONE** | Round's only clock (injected `now_ms_fn`) |
| Merged heartbeat (mount lease ∪ build watermark, `observed_gc_round` ack) | **DONE** | `WatermarkKeeper` removed; −1 PUT/beat |
| Two-phase graduation (`delete_pending`) | **DONE** | Zombie-safe pre-CAS deletes; deletion lags condemn by one pass |
| Clamp-suppressed passes (`suppress_destructive`) | **DONE** | 2026-07-03 night-soak safety fix; no graduation/redelete while any shard is clamped (§three-cursor-merge) |
| `CaGcAckFloorCore.tla` + `CaGcAckFloorZombie.tla` | **DONE** | 7 sabotages + order invariant; `delete_pending` proved load-bearing |
| Exact-token delete (single content-delete site) | **DONE** | |
| Source-edge-set in-degree (H1b fix) | **DONE** | 2026-07-01; faithful to big TLA+ model |
| Per-round all-shard fence (`fence_round` bump, `fence_version`) | **REJECTED** | Replaced by ack-floor; was O(universe) CAS-PUT/round |
| Fold-through-fence recheck phase | **REJECTED** | Replaced by the three-cursor merge; was O(universe) GET + quadratic `inDegreeInGeneration` |
| Ack-floor soak validation (spare-then-recondemn under kill; SIGSTOP holds floor; O(delta) request guard) | **TODO** | On `utils/ca-soak`; the round's regression guard against reintroducing a universe sweep |
| Delta-runs + compaction for the snapshot (bytes O(edges)/pass) | **DESIRABLE** | Next dominant cost; deferred O(buffer) streaming work |
| Attempt-scoped generations (concurrent-leader safety) | **DONE** | 2026-07-01; `_sab_deposedleaderwritesfinalgen` confirmed |
| Advisory heartbeat (B160, false-steal fix) | **DONE** | `CaGcLeaseCore.tla` proved |
| Snap prune — node forgetting (P9) | **SUPERSEDED** | `GcSnap::forget`/`known` set removed with the source-edge-set model (§3.3); no persisted node registry to forget from (§4.1) |
| Snap prune — generation retention (B174) | **DONE** | 3 gen default; sawtooth gc/ storage |
| Part-manifest cleanup (owner-driven) | **DONE** | |
| Orphan part-manifest sweep (pre-precommit debris) | **DONE** | Bounded per round |
| Shard incarnation (D1 phase 1) | **DONE** | `ShardIncarnation` stamped on `RootShard` (Task 2) |
| LIST-based discovery + registry deletion (D1 phase 2) | **DONE** | `discoverUniverse` over `cas/refs/`; `RootsRegistry` deleted (Task 3–4) |
| Newborn = precommit-shard (D1 phase 3) | **DONE** | Newborn self-floor in `CasBuild.cpp` (Task 5) |
| Shard tombstone reclaim (D1 phase 4) | **DONE** | `Gc::reclaimDroppedShards` (Task 6) |
| S30 soak validation (D1 phase 5) | **DONE** | Task 6 commit message cites the S30 scenario |
| Distributed `gc_shards > 1` parallel GC | **DESIRABLE** | Attempt-scoped gen is its prerequisite; shard claim/scheduler not yet built |
| Integer refcount persisted artifact | **REJECTED** | H1b underflow; replaced by source-edge set |
| `gc_lock` in-process mutex | **REJECTED** | Blocks writers; replaced by exact-token + attempt-scoped gen |
| TLA+ leadership uniqueness assumption | **REJECTED** | Safety proof makes no leadership assumption; see `INV_ONLY_ADOPTED_VIEWABLE` |
| Keeper as durable state for GC | **REJECTED** | Keeper is optional accelerator only; all GC durable state is in S3 |
| Registry as authority for namespace universe | **REJECTED** → D1 | Monotone growth; being replaced by LIST-based discovery |
| "empty ⇒ deregister" inference | **REJECTED** | Indistinguishable from idle-but-live namespace |
| Path-keyed fold cursor without incarnation | **REJECTED** | ABA hazard on drop+recreate |

## Absent-at-HEAD is not durably-absent {#absent-at-head}

Named rule (2026-07-03 forensics). An object store may answer `HEAD -> 404` for an object that
exists: the night-soak clamp era was caused by RustFS returning FALSE 404s on live manifest bodies
while its metacache was degraded by the rustfs#3231 leak storm (proof: the clamping manifests'
`+1` folds read the bodies seconds BEFORE the clamps; no deletion event exists for them; the
release fold at the storm's end read them again). Consequences, both already enforced:

- the FOLD treats an unreadable body as a **clamp** (cursor freeze), never a guess and never a
  wedge (`gc_fold_clamp` event carries namespace/shard/manifest/reason since 2026-07-03);
- a clamped pass is **destruction-suppressed** — no graduations, no pending deletes — because
  landed events behind a clamp are exactly the references a false 404 would otherwise let the
  floor delete over.

The rule generalizes: no DESTRUCTIVE decision may treat a single point-in-time absence observation
as durable truth; only exact-token deletes over entries that survived the full condemn -> floor ->
graduate pipeline qualify. (Upstream: false-404-under-load is worth a RustFS report of its own,
tied to rustfs#3231 degradation.)
