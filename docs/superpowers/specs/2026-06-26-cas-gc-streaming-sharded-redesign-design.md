---
description: Design for a streaming target-sharded garbage collector based on root-ref closure deltas
sidebar_label: CAS GC root-closure sharded redesign
sidebar_position: 1
slug: /superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design
title: CA GC root-closure target-sharded redesign design
doc_type: reference
---

# CA GC - root-closure delta, streaming target-sharded redesign - design {#ca-gc-root-closure-delta-streaming-target-sharded-redesign-design}

**Status:** design (2026-06-26, rev. 5). Branch `codex-gc-proposal-2026-06-26`. **NOT behavior-preserving** - an algorithmic redesign of the GC round and the root-ref journal. CA is pre-release (zero persisted data, zero compat scaffolding). Every phase is gated on a green `TLA+` extension of `CaIncarnationCore.tla` before any code lands (requirement R0).

This revision supersedes rev. 3's safe-but-heavy same-round cascade design. The new load-bearing decision is: **publish/remove/repoint records carry the fully expanded root-ref closure as a compact streamed delta**, so GC never owns tree-child expansion state and never performs cascade. Rev. 5 rejects memoized closure objects as a Phase-1 mechanism: journal-size backpressure plus a compact streaming protobuf journal is the simpler base design. This directly resolves the three design objections from the review: no resident `O(pool)` snap, no storm of small S3 GC-state objects, and no cross-round or same-round cascade machinery. It still keeps the Codex safety findings that matter: root-edge displacement is explicit, intermediate mapper products are non-authoritative until sealed, round visibility waits for all retired sets, the fence remains global, and exact-token delete is unchanged.

## Goal {#goal}

Replace the single-leader, whole-pool-resident GC with a **streaming, target-sharded** collector that is optimal per round and parallelizable across replicas, **without weakening any safety invariant**. Requirements, verbatim:

- **R0 - safety is non-negotiable and `TLA+`-provable.** No change may make the system less reliable under any delay, race, or failure. Over-delete (`INV_NO_RETURN`) and loss (`INV_NO_LOSS`/`INV_NO_DANGLE`) must be re-proven, not argued.
- **R1 - each round optimal.** Operations per blob/tree ideally `O(in-degree-zeroed delta)`; memory ideally `O(1)`; journal operations ideally `O(changed RootShardManifest)`; snap access ideally exactly one streaming read and one write.
- **R2 - shardable GC** (default 1 shard) so two replicas can run two GCs on disjoint shards in parallel.
- **R3 - simple, reliable, debuggable.** Each GC's state legible at a glance; resumable from any point; every step idempotent and unambiguous.

Design decisions: **root-ref closure deltas** are the only source of target in-degree; root journals are stored as compact streamed protobuf records, not as one materialized repeated field; byte-based journal backpressure is the operational guard that lets GC catch up; target state is stored as streaming sorted generations; authoritative GC memory is `O(stream buffers)`; durable S3 state is coarse write-once objects; sharding is only across target reducers, with one global coordinator.

## Ground truth - what the code does today {#ground-truth-what-the-code-does-today}

(From a full read of `CasGc.{h,cpp}`, `CasGcSnap.{h,cpp}`, `CasObjectStorageBackend.cpp`, `CasStore.cpp`, `CasBuild.cpp`, `cas_format.proto`, `CasGcScheduler.cpp`, cross-checked against the Codex review.)

- **Round order** (`CasGc.h`): `fold -> retire -> fence -> recheck -> exact-token delete -> cascade -> trim`. The lease is **work-dedup only**; "the `TLA+` model proves the round safe with NO leadership assumption at all" - this property must be preserved.
- **One global resident snap.** `snap_shards` is hard-pinned to 1: `Gc::fold` throws `NOT_IMPLEMENTED` for `snap_shards != 1`, specifically because root `last-op-wins` displacement across target shards is undesigned. The whole pool graph (`edges`, `known`, `indeg`, `expanded`, `children_by_tree`, `folded_cursor`) is resident on the leader. Memory is `O(pool)`.
- **Cost model, stated precisely.** `Store::readShard` issues one backend `get` (returns its token); in the Native backend that `get` is internally `nativeHead` plus ranged GET. Separately, `Gc::retire` issues an explicit `backend.head` per zero-in-degree candidate, and `Gc::fence` does a read-before-CAS (`mutateShard` = read + `casPut`) per shard. The per-round floor is `O(S)`, `S = #namespaces * root_shards`: every shard manifest is read on four-to-five passes (`fold`, coherence guard, `recheck`, `trim`) and fenced unconditionally. There is no LIST token-diff.
- **Cascade is safe only because it is in-round today.** `Gc::cascadeAndPersist` calls `stripTree` this round, drops the dead tree's child edges from the snap via `children_by_tree`, and clears the expansion marker before the round completes. A later `Add` of the same tree hash re-expands children. Deferring this strip is unsafe (`SabotageCascadeRace`).
- **`JournalRecord.closure` already exists, but is not root-ref authority and not the desired wire shape.** `cas_format.proto` has `repeated ClosureNodeProto closure`. `CasBuild.cpp` populates it only for precommit-namespace `Add` records. Normal table `Add`, precommit `Remove`, `Store::dropRef`, and namespace-drop `Remove` records write an empty closure. Rev. 5 promotes the concept to root journal authority, but changes the payload from tree-shaped `ClosureNodeProto` records to a flat canonical closure-delta stream that GC can fold without another tree walk.
- **Root manifests are currently whole-object protobufs.** `CasRootShardCodec.cpp` uses `RootShardManifest::ParseFromArray`, and `journal` is a `repeated JournalRecord`. That is fine for today's small manifests, but it is not the Phase-1 target: a root manifest carrying large closure deltas must be readable by a streaming record parser so GC can fold journal records without materializing all refs and all closure entries in memory.
- **`recheck` rereads every fenced root shard through the fence window** before the single `deleteExact` site; it does not trust the pre-fence fold.
- **Idempotency/resumability is already excellent**: exact-token deletes, write-once retired/outcome/snap paths with adopt-crashed-attempt, per-`(ns,shard)` cursors, crash-resume off durable retired sets. Preserve this machinery.

## Formal-model ground truth (`CaIncarnationCore.tla`) {#formal-model-ground-truth}

The model already carries the scaffolding we need: per-shard `man[s]`/`cursor[s]`/`fencePos[s]`, multiple `Leaders`, a resident snap abstraction, and incarnation tokens (`tokOf`, `deadTok`, exact-token `Land`).

- Active safety invariants: `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`; monotone `gc/state` and registry/fence positions.
- `GRetire` is safe only for in-degree state already folded into `cursor`, and maps the retired token to the candidate's current `tokOf[h]` (this is what the per-candidate `HEAD` observes today).
- `GRecheckDelete` requires `FoldedThroughFence == \A s : cursor[s] >= fencePos[s]`; the negative control `SabotageNoRecheckFold` breaks `INV_NO_DANGLE`. `GFenceRegistry` precedes `GFenceShard` (load-bearing; `SabotageFoldTimeUniverse` reproduces the fixed namespace-creation window).
- `ViewableRound`: a writer may see round `R` only after all of round `R`'s retire entries are durable, not a subset.
- `SabotageCutOverclaim`: any streaming/full-GC cut that advances cursors past incorporated state is unsafe.
- `SabotageCascadeRace`: delayed cascade is unsafe because a stale tree-hash-keyed child decrement can strip a later-revived live tree.

The redesign must keep the proof hooks intact: authoritative folded state before `retire`; all retire entries durable before round visibility; global fence coverage before delete; exact-token delete; and no destructive action based on unsealed intermediate products. Rev. 5 removes the cascade action from the new model by changing what the journal means: target in-degree is derived directly from live root-ref closure deltas, not from GC-maintained tree-child edges.

## Central principle - count root-ref closures, not tree-child edges {#central-principle-count-root-ref-closures}

A live CA object is protected because it is in the closure of at least one live root ref. Therefore the cleanest authoritative count is:

```
indeg[o] = Cardinality({ root ref r : r is live and o in Closure(r.tree) })
```

where `Closure(tree)` is the sorted unique set of all protected content objects reachable from the tree, including the root tree itself and all descendant trees/blobs.

With this definition:

- Adding a root ref emits `+1` for every object in its closure.
- Removing a root ref emits `-1` for exactly the same ref incarnation's closure.
- Repointing a root ref emits `-old_closure` and `+new_closure` in one root-shard transition.
- Deleting a tree object never changes in-degree, because tree-child edges are **not** authoritative GC state.

So cascade disappears. When the last root ref to a subtree is removed, all objects in that subtree can become zero-in-degree in the same fold. The round does not need to delete the parent first and then propagate child decrements; the decrement already happened at the root-ref transition.

This is **not** writer fanout into target shards. The write path writes one root-shard-local transition record (or one root manifest CAS containing that transition). GC later streams that closure-delta payload into target-keyed reducer runs. There are no `N` target-shard writes on publish.

## Target data model {#target-data-model}

### `RootTransitionRecord` - root journal payload with exact old/new closures {#root-transition-record}

Conceptual record:

```
(namespace, root_shard, ref_name, root_version) ->
{
    old?: { tree_hash, ref_epoch, closure_digest, closure_delta_payload },
    new?: { tree_hash, ref_epoch, closure_digest, closure_delta_payload }
}
```

Semantics:

- create: `old = none`, `new = closure(new_tree, new_ref_epoch)`;
- drop: `old = exact current closure(old_tree, old_ref_epoch)`, `new = none`;
- repoint: both halves present in the same root-shard transition.

`ref_epoch` is the root-ref incarnation, for example the root journal version that installed that tree. It is mandatory even when `tree_hash` is content-identical across two publishes. A stale remove for epoch `E1` must never be allowed to decrement epoch `E2`'s live closure.

The remove/drop path obtains `old.closure_delta_payload` by reading the currently live root ref and expanding its tree **before** the CAS that removes the ref. If the CAS loses a race, the computed closure is discarded and the writer retries from the new root state. This avoids retaining `O(live closure)` metadata in `refs`, and it also avoids reading a condemned tree after the ref is gone.

Implementation can encode this as a new replace-style proto record, or as adjacent `Remove`/`Add` records accepted by the same root-manifest CAS and sharing one `root_version`. The safety requirement is semantic: the old and new halves are one transition for `last-op-wins`, not two independent target-shard facts.

### Flat canonical closure-delta payload {#flat-canonical-closure-delta-payload}

The Phase-1 payload is not the current tree-shaped `repeated ClosureNodeProto`. It is a compact canonical delta stream:

```
ClosureDeltaPayload {
    codec_version
    closure_digest
    repeated ClosureBlock
}

ClosureBlock {
    object_kind          // blob or tree
    sorted_hashes        // delta-coded / compressed UInt128 sequence
    optional sizes       // only if needed for retire-token optimization or diagnostics
}
```

The sign is derived from the `RootTransitionRecord` half: `old` blocks emit `-1`, `new` blocks emit `+1`. Entries are sorted and unique within one closure. GC folding consumes blocks as a stream of `(kind, hash, delta, ref_epoch)` tuples and never reconstructs the tree.

This is deliberately inline journal data. Phase 1 does **not** introduce memoized closure objects keyed by `tree_hash`, and does **not** create a separate S3 object for each tree closure. If wide-table workloads later prove that inline journal bytes dominate, that is a new storage design with its own lifecycle and proof, not a hidden escape hatch in this protocol.

### `RootEdgeIndex` - owns `last-op-wins`, keyed by root-edge identity {#root-edge-index}

```
(namespace, root_shard, ref_name) ->
{
    current_tree_hash?,
    current_ref_epoch?,
    current_closure_digest?,
    root_journal_cursor,
    manifest_token
}
```

This remains the prerequisite for target sharding. A run keyed only by target cannot know which old target/closure to subtract on a re-pointed ref. `RootEdgeIndex` processes root transitions in root-journal order, verifies that `old` matches the currently recorded epoch (or fails closed), emits the paired closure deltas, and advances the root cursor only in the sealed generation commit.

### `TargetInDegreeShard` - sorted by target key; reducers own it {#target-in-degree-shard}

```
(kind, hash) -> { known, indeg, last_observed_token?, last_observed_size? }
```

Input is a sorted stream of target deltas from root closures only. The reducer merge is:

```
old target run + sorted closure-delta stream -> new target run + zero-in-degree candidates
```

All entries for one target are contiguous, so in-degree falls out of the merge. With external sort/spill, memory is `O(1) + buffers`.

### `RootManifestTokenIndex` - token-diff state, advanced with the cursor {#root-manifest-token-index}

```
root_shard_key -> { folded_cursor, folded_manifest_token }
```

`folded_cursor` and `folded_manifest_token` advance in one durable generation commit. A token-only update without incorporated journal state is forbidden (would match `SabotageCutOverclaim`).

### No `TreeExpansionIndex`, no GC-side cascade state {#no-tree-expansion-index-no-gc-side-cascade-state}

Rev. 3 needed `TreeExpansionIndex` to remember `children_by_tree` durably and to make "expanded marker durable iff child-edge add deltas durable" atomic. Rev. 5 deletes that entire obligation. GC does not store expansion markers, child-edge runs, cascade queues, or cascade wave markers. Closure correctness is a write-path/root-journal obligation instead.

## Minimal S3 object layout - coarse objects only {#minimal-s3-object-layout}

Authoritative durable state is:

- existing mutable objects: `gc/state`, `gc/registry`, and root manifests;
- root-manifest journal entries containing inline compressed sorted root-ref closure-delta blocks;
- one write-once `gc/gen/<generation>/manifest` object containing the `input-seal`, parent generation, shard run list, root cursor/token table, target shard metadata, checksums, and completion phase markers;
- large write-once target run files under `gc/gen/<generation>/target/`;
- one compact round bundle per active target shard, or one bundle for `gc_shards = 1`, for `retired` and outcome entries.

There are no permanent `jrnl[P]` micro-records, no object per edge, no object per candidate, no object per tree closure, no expansion files, and no cascade attempt files. Temporary mapper/reducer outputs are hidden under an attempt prefix, referenced by the generation manifest only after the `input-seal` is complete, and cleaned later as orphaned generation debris. They are not writer-visible protocol state.

For the default `gc_shards = 1`, a non-empty GC round should create a bounded set of GC objects: one generation manifest, one or a few large target run files, and one round bundle. The closure-delta payloads are publish/remove journal data, not extra GC scratch state.

`Gc` may keep bounded caches of recently read run pages, decoded root manifests, or closure-delta blocks. Those caches are never authoritative and have a fixed memory budget. There is no resident whole-pool `GcSnap` in any production phase.

## Journal encoding and backpressure {#journal-encoding-and-backpressure}

Phase 1 requires a streaming protobuf root-manifest/journal format. The current whole-object `RootShardManifest` shape is replaced by a record stream:

```
ManifestHeader
RefRecord...
JournalRecordHeader
ClosureBlock...
JournalRecordFooter
...
ManifestFooter
```

Each record is length-delimited protobuf. The normal `Store` path may materialize refs when it needs a point-in-time root view; GC fold uses a callback reader (`onRef`, `onJournalRecord`, `onClosureBlock`) and streams closure blocks directly into target-delta sorting/spill. This is the format-unification path for mutable CA objects; it is also what keeps Phase-1 GC memory from depending on root journal size.

Backpressure is operational, not a safety proof. Safety still comes from the rule that a root transition is visible only together with its exact closure delta and from the model-checked fold/fence/recheck/delete protocol. Backpressure prevents unbounded lag and must be byte-based:

- `journal_unfolded_bytes` per root shard;
- `root_manifest_encoded_bytes`;
- `largest_single_transition_bytes`;
- `closure_entries_per_transition`;
- `gc_lag_rounds` / oldest folded cursor age for diagnostics.

When a limit is exceeded, writers fail closed or throttle before publishing more root transitions; they do not publish a root pointer without its closure delta. A single transition that exceeds `largest_single_transition_bytes` must fail before becoming visible, not after it has created a journal entry GC cannot process within memory/latency bounds.

## Round protocol {#round-protocol}

### 1. Discovery - registry is authority; LIST only accelerates; fail-closed {#discovery}

The namespace-by-shard universe is always taken from `gc/registry`, never from LIST. A single paginated `LIST roots/` returning `(key, size, token)` is only an accelerator. Per registry shard:

- listed token == persisted `folded_manifest_token` -> skip body read;
- token absent / ambiguous / backend-weak / different -> read the manifest body and fold;
- registry shard not returned by LIST -> ambiguous -> read/mint per current rules;
- LIST never shrinks the registry universe.

So an under-read fails closed to today's behavior. Token-diff removes the unconditional fold/recheck/trim body reads of unchanged root manifests.

### 2. Fold - stream root-closure transitions into a sealed generation {#fold}

The fold reads changed root journals, streams each accepted `RootTransitionRecord`, and emits target-keyed deltas:

```
for o in old.closure: emit (o, -1, source = old.ref_epoch)
for o in new.closure: emit (o, +1, source = new.ref_epoch)
```

`RootEdgeIndex` owns root-journal order and `last-op-wins`; `TargetInDegreeShard` owns only sorted target counts. The generation manifest's `input-seal` records: registry token/version used for the universe; each root shard's classification `skipped/read/minted` with listed/previous/new token plus cursor; reducer-input segment hashes; parent generation. Partial objects are adopted only if byte-identical; otherwise ignored. They are invisible to writers and cannot advance `gc/state.round`.

The fold does not read tree objects to expand them and does not write expansion/cascade state. Closure-delta validation is a separate write-path/model obligation: each inline payload must match its `closure_digest`, be sorted/unique, and represent `Closure(tree) = Reach(tree)` for the referenced tree. A malformed or over-limit payload fails closed; it is never treated as an empty or partial closure.

### 3. Retire barrier - `AllRetiredDurable(round)` {#retire-barrier}

Each target reducer scans its new target shard and writes `gc/retired/<round>.<fence_seq>/<target_shard>`. `gc/state.round` advances only after the retired sets for all target shards of the current configuration are durable. This preserves `ViewableRound`: a writer refreshing to round `R` sees the complete round-`R` retired set, not a subset produced by faster reducers.

At `gc_shards = 1` this is today's visibility shape, but backed by a streaming target run instead of a resident snap.

### 4. Global fence - coordinator-owned, serial {#global-fence}

The coordinator fences the registry first, then the root shards. First safe implementation keeps the current all-shard fence (expensive but already proven). The fence stays global; the no-dangle two-horn argument requires a global fence epoch because a publish into one root shard can reference targets of any prefix. Reducers do not fence independently.

### 5. Recheck + exact-token delete {#recheck-exact-token-delete}

Reducers re-fold root-closure transitions through durable fence positions for their candidates. The fence-window closure deltas are incorporated into the completion generation before any cursor/token/trim advance. Delete stays exact-token at the single content-delete site.

Because closure deltas already include all descendants, parent and child objects can be retired/deleted in the same round when the last root-ref closure disappears. This is safe: once the global fence and recheck have incorporated all root transitions through the fence, there is no authoritative tree-child edge left to propagate.

The per-candidate `HEAD` in `retire` is kept. It is safe, already `O(zeroed candidates)`, and is what makes `GRetire` observe the current `tokOf[h]`. Its removal is a separate optional phase.

### 6. Trim - root journals and closure-delta payloads only below durable coverage {#trim}

Trim is now the final step:

```
fold -> retire -> fence -> recheck -> exact-token delete -> trim
```

Root-journal records and their inline closure-delta payloads can be trimmed only below the durable folded cursor/fence-window cursor recorded in the sealed generation. Trimming must preserve `INV_JOURNAL_COVERAGE`: if a future recovery needs to rebuild from the parent generation, every root transition past that parent cursor remains readable; if a transition is trimmed, its effect is already part of an adopted sealed generation.

There is no `awaiting-cascade` state and no cascade-resume case.

## Change detection - root-manifest token-diff drives touched target shards {#change-detection}

Do not decide "target shard `P` is changed iff `P`'s own token differs" - a changed root manifest can add/remove closure members in any target shard. Instead:

- `RootManifestTokenIndex` decides which root shards to read/fold.
- The fold's closure scatter produces the set of touched target shards.
- Target shard `P` is touched iff a folded closure delta lands in `P`, or `P` has retired candidates requiring recheck.
- Only touched target shards are re-merged and re-written. Untouched target shards cost zero.

## Sharding model {#sharding-model}

Default `gc_shards = 1`. Sharded mode:

- One global coordinator owns registry fence, `input-seal`, round visibility (`AllRetiredDurable`), global fence, round completion, and the `gc/state` generation-pointer advance.
- Target reducers own disjoint target-hash ranges. Per-target-shard leases are work-dedup only, like today's global lease. Reducers write unique-path outputs and use exact-token deletes, so duplicate work is safe.
- Two replicas may run different reducers concurrently (replica A shard 0, replica B shard 1). The coordinator need not do all CPU work; mappers/reducers may be distributed. No writer-visible state ever depends on unsealed mapper output.

The expensive work (root-journal fold -> target deltas; target-shard merge; per-candidate recheck/delete) parallelizes across mappers/reducers. The coordinator serializes only the global fence and round-pointer advance. Unlike rev. 3, there is **no cross-owner cascade barrier**.

## Safety / `TLA+` plan (R0) {#safety-tla-plan}

Create `CaGcRootClosureCore.tla` (or carefully extend `CaIncarnationCore.tla`) with small bounded configs. Preserve obligations: `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, monotone `gc/state`, monotone registry/fence.

Add to the model:

- root-ref transitions with `old` and `new` closures;
- `ref_epoch` and the rule that a remove/repoint can decrement only the exact epoch it replaces;
- remove/drop computes `old` closure while the ref is still live; a lost CAS discards the computed closure and retries;
- `Closure(tree) = Reach(tree)` as a model obligation for non-sabotage writers;
- inline closure-delta payloads with sorted/unique coverage and digest validation;
- `RootEdgeIndex` separate from target in-degree;
- per-root-shard `manifestToken`/`listedToken`/`lastFoldedToken`;
- a `ListReturnsFreshToken` capability flag;
- `InputSeal` with full root-shard coverage;
- target reducers and per-target retired sets;
- `AllRetiredDurable(round)` as the sole round-viewable condition;
- lazy fence only if implemented.

Do **not** add tree expansion markers, GC child-edge state, or cascade actions to the new model. Keep the old `SabotageCascadeRace` as regression evidence for why this design avoids that state class.

Mandatory negative controls (each must produce the named counterexample):

1. stale LIST token lets a changed manifest be skipped -> `INV_NO_DANGLE`;
2. missing listed key treated as unchanged -> `INV_NO_DANGLE`;
3. cursor/token advanced past incorporated closure state -> `SabotageCutOverclaim` shape;
4. a reducer retires before all mapper inputs are sealed -> `INV_NO_DANGLE`;
5. `gc/state.round` advanced after only one target shard's retire -> `ViewableRound`/publish-gate break;
6. lazy fence skips a shard with a racing publish -> `INV_NO_DANGLE`;
7. target-sharded root edges without `RootEdgeIndex` displacement -> leak/undercount;
8. remove keyed only by `tree_hash`, not by `ref_epoch`, strips a later live content-identical tree -> `SabotageCascadeRace` shape without a cascade action;
9. root pointer becomes visible before its closure transition is durable -> `INV_NO_LOSS`/`INV_NO_DANGLE`;
10. closure underclaims reachability (missing child/blob) -> `INV_NO_LOSS`;
11. remove computes old closure after the ref is no longer live and races a content-identical revival -> `INV_NO_LOSS`;
12. malformed/over-limit closure-delta payload is treated as empty/partial instead of failing closed -> `INV_NO_LOSS`;
13. non-exact delete / token reuse -> existing `SabotageUncondDelete`/`SabotageReusedTag` still fail.

Rule: no performance optimization is "implemented" until the positive model passes and its paired sabotage still fails.

## Phasing - each phase model-gated; no code before its model is green {#phasing}

- **Phase 0 - model + streaming journal codec + backend probes.** No production behavior change. Extend/branch the `TLA+` model to root-closure transitions. Define the flat closure-delta codec (sorted unique `(kind, hash)` entries; compression; digest), the length-delimited streaming protobuf root-manifest/journal reader, and byte-based backpressure limits. Add backend capability probes for LIST token freshness (`list(k).token == head(k).token` after `put`/`casPut`/overwrite/delete) and an explicit `ListReturnsFreshToken` bit; absent means token-diff discovery falls back to per-shard reads.
- **Phase 1 - root-closure journal authority, streaming generation, `gc_shards = 1`.** First behavior-changing phase. Populate table/root `Add`, `Remove`, and repoint transitions with inline exact closure deltas; `Remove`/`dropRef` expands the currently live tree before the CAS and retries on CAS loss. Introduce `RootEdgeIndex`, `TargetInDegreeShard`, `RootManifestTokenIndex`, and the coarse `gc/gen/<generation>/manifest` plus target-run layout. Enforce journal byte backpressure before accepting over-limit transitions. Remove resident `O(pool)` authority and all GC-side cascade state immediately. Keep the current all-shard root scan and all-shard fence for the first safe implementation.
- **Phase 2 - root-manifest token-diff discovery.** Add `std::optional<Token> token` to `ListedKey`, populate it in `ObjectStorageBackend::list`/`InMemoryBackend::list`/local list paths, add a `GcRootDiscovery` helper returning `{ns, shard, key, listed_token, previous_token, action: Skip|Read|Mint, reason}`, persist `folded_manifest_token` beside `folded_cursor`, advance cursor+token in one generation commit, keep `assertSnapJournalCoherent` fail-closed.
- **Phase 3 - lazy fence + lazy trim** (after `TLA+`). A shard may skip fence only when the model proves its previous fence position plus unchanged token closes both no-dangle horns; otherwise fence it. Practical rule: registry fence always first; new/ambiguous shards always fenced; changed-token shards fenced; skipped shards fenced only if persisted proof says the previous fence suffices. Lazy trim only below durable cursor coverage (`INV_JOURNAL_COVERAGE`).
- **Phase 4 - target-shard reducers** (`gc_shards > 1`). Per-target-shard work leases; one coordinator; `AllRetiredDurable(round)` before the round advances; recheck/delete per target shard; two replicas own disjoint shards. `gc_shards = 1` remains the default.
- **Phase 5 - retire-token optimization** (optional, separate, not coupled to sharding). To drop the per-candidate `HEAD`: store `last_observed_token`/`size` in `TargetInDegreeShard`; enumerate every legal token source (content `put`, resurrect, publish evidence, closure transition, explicit observe); prove a stale stored token yields only `TokenMismatch`/under-delete, never a fabricated delete-right, and never a token for an absent object; keep a `HEAD` fallback when the token is absent or the backend proof is incomplete.

## Op-count & memory - by phase {#op-count-memory-by-phase}

| Quantity | Today (baseline) | After Phase 1 | After Phase 2-3 | After Phase 4 |
|---|---|---|---|---|
| Root-manifest body reads/round | `O(S)` | `O(S)` but pass-fused | **`O(changed)`** + 1 LIST | `O(changed)` per mapper plan |
| Fence writes/round | `O(S)` (all shards) | `O(S)` | **`O(shards needing fence proof)`** | same (global, serial) |
| Snap memory (leader/owner) | `O(pool)` resident | **`O(stream buffers)`** | `O(stream buffers)` | `O(stream buffers)` per owner |
| Snap I/O | 0 read / `O(pool)` write on change | **1 read + 1 write / touched target shard** | same, fewer touched shards | per owner |
| GC-side expansion/cascade | resident `children_by_tree`, same-round cascade | **none** | none | none |
| Per-candidate `HEAD` in retire | 1/candidate | 1/candidate | 1/candidate | 1/candidate (until Phase 5 removes it with proof) |
| Deletes/round | `O(zeroed)` | `O(zeroed)` | `O(zeroed)` | `O(zeroed)` |
| Publish/remove overhead | writes tree hash only | computes + writes inline root-ref closure delta; over-limit transitions throttle/fail closed | same | same |
| Parallelism | single leader | single leader | single leader | **disjoint reducers across replicas** |

The memory target lands at Phase 1. The "exactly one streaming read and one write" target for snap access also lands at Phase 1 for touched target shards because there is no cascade wave. Root-read/fence op-count targets land at Phase 2-3. R2 parallelism lands at Phase 4.

The honest cost shift is the publish/remove path: each accepted root-ref transition must durably carry the closure it adds/removes. That is `O(changed ref closure)` on the write path, but it is one root-shard-local journal transition, not target-shard fanout and not GC S3 scratch. Byte backpressure bounds the operational lag; it is not a substitute for the `TLA+` safety proof.

## Backend requirements {#backend-requirements}

`ListedKey` gains `std::optional<Token> token`; backends populate it from list metadata (the `ObjectMetadata.etag` is already seen by `ObjectStorageBackend::list`, just dropped). Under-read safety requires the listed token to be fresh and strongly consistent, equal to the `head`/`casPut` token for the same object. S3 qualifies since 2020; RustFS and `LocalObjectStorage` must be verified by the Phase-0 probe and fall back to per-shard `HEAD` where `ListReturnsFreshToken` is false.

## Debuggability & resume (R3) {#debuggability-resume}

Every durable round artifact is self-describing: `round, fence_seq, parent_generation, new_generation, coordinator_id, phase, registry_version, root_shards_total/read/skipped/minted/ambiguous, target_shards_total/done, retired/deleted/spared/replaced/forgotten, closure_records_folded, closure_items_folded, closure_bytes_folded, journal_unfolded_bytes, largest_single_transition_bytes, backpressure_throttles, backpressure_rejections, list_token_skips, list_token_forced_reads, lazy_fence_skips, lazy_fence_forced`. Surface this through the existing `CasEvent`/`RoundReport` path and, where possible, a debug system table.

An operator can answer at a glance: which round an artifact belongs to; which root shards were skipped and why; which root transition contributed a target delta; which reducer owns a candidate hash; whether a candidate is retired/fenced/spared/deleted; from which step it is safe to resume.

Resume rules: all generation/segment objects are write-once; byte-identical existing objects are adopted; divergent ones are ignored via a higher generation/attempt path; content is removed only at the recheck exact-token delete; `round` is not advanced until all retired sets are durable; generation pointers and root cursors are not advanced until the fence-window closure deltas are incorporated.

## Backlog reconciliation {#backlog-reconciliation}

- **Subsumed/superseded:** B147 item-3 (shard the snap), B148b (snap-token-in-retire -> Phase 5 with proof), B168 P5/P6 (widen shards / dirty-only fence), B103 (lazy fence -> Phase 3), B178 (map-reduce adopted but made safe by sealed-generation commit protocol plus `RootEdgeIndex`), B201 (LIST+token-diff -> Phase 2), B176 (snap codec mooted by sorted-generation files).
- **Replaced by root-closure deltas:** rev. 3's `TreeExpansionIndex`, expansion marker atomicity, same-round cascade wave, and cross-owner cascade barrier.
- **Out of scope (not the GC round):** B195 (`CaContentWriteBuffer` allocation), B202 (inline-by-size), B204/B205/B206 (soak harness), B207 (`fsck` consistency race - a separate checker, tracked on its own).

## Out of scope / non-goals {#out-of-scope-non-goals}

- No on-disk compat work (CA is pre-release: zero persisted data, zero compat scaffolding).
- No target-shard fanout from publish/remove; closure transitions stay root-shard-local.
- No change to delete authority (round-keyed `retired`/outcome sets) or to the exact-token delete primitive.
- No GC-side tree expansion, expansion markers, child-edge authority, or cascade.
- No per-edge/per-candidate S3 object layout. Deltas are bundled into coarse run files; small protocol metadata is packed into generation manifests and round bundles.
- No memoized closure object table in Phase 1. Inline streamed closure deltas plus byte backpressure are the base design; any external closure payload would be a separate storage optimization with its own lifecycle and proof.
- A full LSM snap (multiple runs plus background compaction, `O(delta)` writes) is a possible later phase; the streaming single-run-per-shard generation is the chosen first form.

## Resolved questions (from the cross-review) {#resolved-questions}

- **Why keep the resident `O(pool)` snap?** We do not. The first behavior-changing phase moves authority to streaming generation files; resident structures are bounded caches only.
- **How many small S3 objects does GC create?** Bounded and coarse only: one generation manifest, large target run files, and compact round bundles. No expansion/cascade files, no per-edge objects, no per-candidate objects.
- **Do we need cascade?** No, not if root journal transitions carry full exact closures. Cascade was only needed because the snap represented tree-child edges. Root-closure in-degree makes the root ref the only source of reachability authority.
- **What about content-addressed revival?** `ref_epoch` is part of the root-ref closure transition. A stale remove for epoch `E1` cannot decrement a later live epoch `E2`, even when both epochs use the same `tree_hash`.
- **How does `dropRef` get the old closure?** It expands the currently live root ref before the CAS that removes it. If CAS loses, discard and retry. Do not store `O(live closure)` inside `refs`, and do not read a tree after its ref has been removed.
- **How is journal growth controlled?** Byte-based backpressure (`journal_unfolded_bytes`, encoded manifest size, largest transition, closure entries per transition) throttles or fails closed before publishing more transitions. Safety does not rely on backpressure; it only keeps GC able to catch up.
- **Should we memoize closures by tree hash?** Not in Phase 1. That would trade journal bytes for extra durable S3 lifecycle state. Keep the base protocol inline and streaming.
- **Per-candidate `HEAD` in retire?** Kept through Phase 4; removed only in optional Phase 5 behind its own proof.

## Open questions for planning {#open-questions}

1. Exact root journal encoding: new replace-style `JournalRecord`, or paired `Remove`/`Add` records in one root-manifest CAS with one `root_version`.
2. Flat closure-delta codec details: compression block size, hash delta coding, optional size fields, digest scope, and corruption behavior.
3. Streaming protobuf migration details: record tags, length framing, footer checksums, whether `RefRecord` and `JournalRecord` live in one stream or separate sections, and how old whole-object protobuf tests migrate.
4. Backpressure thresholds and user-visible behavior: throttle vs fail closed, per-pool/per-root-shard limits, and metrics.
5. Recommended production `gc_shards` and whether it is a pool constant or reconfigurable.
6. The Phase-0 backend LIST/ETag verification matrix (RustFS, `LocalObjectStorage`) and the precise `HEAD` fallback trigger.
