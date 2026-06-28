---
description: Design for splitting the content-addressed pool layout into a hot CAS-protocol prefix (cas/refs, cas/manifests) and the cold mirrored tree (roots/), so GC discovery is a single LIST over ref shards instead of a recursive walk of the whole roots/ subtree; plus a cursor-paced bounded orphan-manifest sweep and the writer_epoch/manifest_ordinal identity reshape.
sidebar_label: CA layout hot/cold split
sidebar_position: 1
slug: /development/cas-layout-hot-cold-split-design
title: CA pool layout hot/cold split (cas/refs + cas/manifests)
doc_type: reference
---

# CA pool layout hot/cold split — `cas/refs` + `cas/manifests` {#ca-layout-hot-cold-split}

## Problem {#problem}

GC discovery is the hot path: the background scheduler runs a round roughly every 2 seconds, and the
first thing each round does is read the **per-root-shard tokens** to decide which shards changed since the
last fold (the Phase-2 token-diff: skip a shard whose listed token still equals the sealed
`folded_token`). Today those ref shards live at `roots/<namespace>/<shard>`, **interleaved under the same
`roots/` subtree** as two unrelated things:

- the per-build **part-manifest backlog** — `roots/<ns>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto` (tens of thousands of objects in a busy pool); and
- **verbatim mirrored files** — `roots/<ns>/_files/…` (e.g. `metadata_version.txt`, `deduplication_logs/…`) and the ordinary mirrored server tree.

So `Gc::listRootShardTokens` must do a **recursive LIST of all of `roots/`** to read ~`namespaces × root_shards` ref-shard tokens, paging through the entire manifest backlog every round. Two independent costs compound:

1. **Implementation:** `CasObjectStorageBackend::list` calls `object_storage->listObjects(prefix, children, /*max_keys=*/0)` — unbounded — and paginates client-side at 1000, so each page re-fetches *all* keys under `roots/` → **O(N²/page)** RPCs to walk the prefix.
2. **Structural:** even with server-side pagination, discovery must enumerate the whole `_manifests/` backlog just to read the handful of ref-shard tokens it actually needs.

This is `GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS` (`utils/ca-soak/scenarios/BACKLOG.md`) — the mechanism
behind the fsck/GC LIST-budget blowups under soak.

## Goal {#goal}

Make hot-path GC discovery cost **`GET gc/registry` + `LIST cas/refs/`**, where `LIST cas/refs/` returns
*only* ref-shard objects (and their tokens) — never part-manifest bodies, verbatim files, or the mirrored
tree. Move the part-manifest backlog to its own cold prefix swept by a cursor-paced, budgeted background
pass rather than a per-round full scan. Preserve the existing GC/fence safety model unchanged.

The change is **layout + naming**: it does not introduce content-addressed trees/manifests, a new index
object, cross-namespace manifest sharing, or a registry redesign. `gc/registry` remains the authority for
the namespace universe and the registry fence; `LIST cas/refs/` is only a token accelerator.

## Target layout {#target-layout}

```
<pool>/
  cas/
    refs/
      <root_namespace>/
        <shard>                     # RefShard objects — the ONLY thing hot GC discovery lists
        <shard>
    manifests/
      <root_namespace>/
        <writer_epoch>/
          <build_sequence>/
            000001.proto            # PartManifest bodies — cold; cursor-swept
            000002.proto
  roots/
    <ordinary mirrored server tree>            # verbatim files ClickHouse writes (not content-addressed)
    <root_namespace>/
      _files/
        metadata_version.txt
        deduplication_logs/deduplication_log_1.txt
  blobs/
    <aa>/<blob_hash>                # content-addressed blob bodies (unchanged)
  trees/                            # unchanged (TreeId-keyed; orthogonal to this change)
  gc/
    registry                        # namespace universe + registry fence (UNCHANGED authority)
    state                           # GcState (+ new best-effort manifest_sweep_cursor)
    gen/<generation>/attempt/<attempt>/…
  _pool_meta
```

A concrete `@cas@` namespace makes the eviction visible. For
`root_namespace = server-01/store/ab/3f2e9b1c-7a4d-4b44-9f6e-0b8c2d1a9e77@cas@`:

```
# AFTER — hot discovery lists only:
cas/refs/server-01/store/ab/3f2e9b1c-…@cas@/0
cas/refs/server-01/store/ab/3f2e9b1c-…@cas@/1
# manifests + verbatim files live elsewhere:
cas/manifests/server-01/store/ab/3f2e9b1c-…@cas@/aaaa…:42/17/000001.proto
roots/server-01/store/ab/3f2e9b1c-…@cas@/_files/metadata_version.txt

# BEFORE — LIST roots/ also walked, per namespace:
roots/server-01/store/ab/3f2e9b1c-…@cas@/0
roots/server-01/store/ab/3f2e9b1c-…@cas@/_manifests/aaaa…:42/17/9a/9a8f…c1.proto
roots/server-01/store/ab/3f2e9b1c-…@cas@/_files/metadata_version.txt
```

`@cas@` stays part of the namespace path; the CAS protocol objects simply leave the shared mirrored tree.
Because the only objects under `cas/refs/` are ref shards (`<ns>/<shard>`, shard numeric), a recursive
`LIST cas/refs/` is now `O(total ref shards)` and the existing cursor-key parse (`<ns>/<shard>`, split on
the last `/`) is unchanged.

## Terms {#terms}

- **RefShard** — the mutable, authoritative shard for refs. Holds the current `ref_name → PartManifestRef`
  index plus the ordered owner-transition log GC folds. (Today's "root shard"; relocated, not redefined.)
- **PartManifest** — the immutable full file list of one part: paths, blob hashes, sizes, inline entries.
- **PartManifestRef** — `(writer_epoch, build_sequence, manifest_ordinal)`.
- **PartManifestId** — `(root_namespace, PartManifestRef)`. Still namespace-qualified (the safety identity).
- **writer_epoch** — rename of the current `writer_instance_id`, formatted `<server_id_hex>:<process_epoch>`.
  `process_epoch` is the existing random-nonzero-per-`Store::open` value (`CasStore.cpp:99`); GC treats a
  different epoch as a dead process (equality only).
- **manifest_ordinal** — fixed-width decimal per build, `000001.proto … 999999.proto`, allocated monotone
  within one `Build`, never reused; cap `999999` then fail closed with `LIMIT_EXCEEDED`. Replaces the
  random `UInt128` `manifest_instance_id` + `<aa>` fan-out.

## Implementation phases {#phases}

The spec documents the full A+B+C target; implementation ships in three independently-reviewable,
independently-shippable phases, sequenced so the load-bearing identity change lands last.

### Phase 1 — A: relocate refs + manifests (identity-preserving) {#phase-1}

- `CasLayout`: `rootShardKey(ns, shard)` → `cas/refs/<ns>/<shard>`; `manifestKey(id)` →
  `cas/manifests/<ns>/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`
  (**keep the current ManifestRef fields and `<aa>` fan-out** — only the prefix moves). New
  `casRefsPrefix()` = `cas/refs/`; `casManifestsPrefix()` = `cas/manifests/`. `roots/` keeps the verbatim
  mirrored tree + `<ns>/_files/`.
- `Gc::listRootShardTokens` (discovery): LIST `casRefsPrefix()` instead of `rootsPrefix()`. The token-diff
  comparison (`listed_token == folded_token`) is unchanged.
- **fsck and namespace-ops follow the relocation** (non-optional, or they break): `CasFsck` enumerates
  `cas/refs/` + `cas/manifests/` + `roots/` + `blobs/` separately (and parses manifest keys from the new
  prefix); the orphan-manifest sweep enumerates `cas/manifests/<ns>/…`; "is namespace empty" / drop logic
  consults the split (refs in `cas/refs/<ns>/`, manifests in `cas/manifests/<ns>/`, verbatim in
  `roots/<ns>/_files/`).
- `gc/registry` unchanged. TLA+ identity unchanged (objects moved, identities and the proof model
  untouched).

### Phase 2 — C: cursor-paced bounded orphan-manifest sweep {#phase-2}

Replace the per-round full-namespace `pickOneSweepTarget` + `sweepNamespace` scan with a cursor-paced,
budgeted pass over `cas/manifests/`. **Same liveness authority as today** (see
[liveness](#sweep-liveness)); this is S3-budget/KISS work, no identity change.

The cursor is **best-effort cleanup state, not safety state**. It lives in `GcState` for convenience, but
**`fold`, `retire`, and `recheck` must not depend on it**. Losing or resetting it only causes a repeated
scan — never a reachability change.

```
GcState {
    …
    manifest_sweep_cursor : string    # best-effort; NOT consulted by fold/retire/recheck
}
```

Two independent budgets (LIST is far cheaper than destructive ops, which can hit HEAD fallback +
exact-token retries):

- `manifest_sweep_list_budget_keys` (N) — max keys listed per round;
- `manifest_sweep_delete_budget_keys` (M) — max `deleteExact` issued per round.

Cadence: **after** the round's safety-critical completion + retention prune (so the sweep never gates or
delays a GC round):

```
page = LIST cas/manifests/ after manifest_sweep_cursor, limit N
for key in page:
    if delete_budget exhausted: break
    if malformed key:                                   continue
    if not provably build-dead (watermark):             continue   # prefixEligible
    if referenced by active owner / pending removal:    continue   # activeManifestKeys
    deleteExact(key, listed_token if present else HEAD token)       # NotFound/TokenMismatch spared
manifest_sweep_cursor = page.next_cursor
if page.end: manifest_sweep_cursor = ""                            # wrap to the start
```

The cursor advances in the round's existing lease-guarded `gc/state` CAS. **Failure modes are all benign:**
if the round aborts before that CAS, cursor progress is lost (re-scan next round — fine); if two leaders
race, only the accepted `gc/state` cursor wins (fine); on any uncertainty the sweep **fails open / skips**,
never deletes.

### Phase 3 — B: `writer_epoch` + `manifest_ordinal` identity reshape {#phase-3}

- Rename `writer_instance_id` → `writer_epoch` (same `<server_id_hex>:<process_epoch>` content; codecs,
  `PartManifestRef`, `RefMatchesBody`, parsers, tests updated).
- Replace the random `manifest_instance_id` (`UInt128`) + `<aa>` fan-out with a per-build monotone
  `manifest_ordinal`: `cas/manifests/<ns>/<writer_epoch>/<build_sequence>/<ordinal>.proto`.
  `Build::stageManifest` allocates `000001, 000002, …` within one `Build`, no reuse, cap `999999` →
  `LIMIT_EXCEEDED`.
- PartManifest body repeats its `PartManifestRef` and `root_namespace`; `RefMatchesBody` stays mandatory.
- **Identity recheck:** `NoManifestIdReuse` becomes **allocator uniqueness** of
  `(root_namespace, writer_epoch, build_sequence, manifest_ordinal)`. This is sound on the existing
  mechanism: `writer_epoch` is unique per process (random `process_epoch`, dead-on-mismatch), `build_sequence`
  is the strictly-increasing per-process allocator (`Store::allocateBuildSeq`), and `manifest_ordinal` is a
  fresh per-`Build` monotone counter. The TLA+ `ManifestIds` abstraction stays an opaque per-namespace id,
  so this is a re-interpretation of how that id is minted + a targeted invariant recheck, not a structural
  model rewrite.

## S3 budget {#s3-budget}

- **Hot discovery, before:** `GET gc/registry` + recursive `LIST roots/` (≈ all ref shards **+** the whole
  `_manifests/` backlog **+** verbatim files), with the `max_keys=0` client-paging multiplier.
- **Hot discovery, after:** `GET gc/registry` + `LIST cas/refs/` (≈ `namespaces × root_shards` keys only).
  Independent of backlog size; with the `max_keys` pagination fix (below) it is one page-RPC per ~1000
  shards.
- **Cold sweep:** bounded `LIST cas/manifests/` after the persisted cursor, ≤ N keys + ≤ M deletes per
  round — no per-round full scan.
- **Independent quick win (recommended to land with Phase 1):** fix `CasObjectStorageBackend::list` to pass
  a real `max_keys` to `listObjects` (server-side pagination) instead of `max_keys=0` + client slicing —
  turns every prefix LIST from O(N²/page) into O(N). Orthogonal to the layout move; both help.

## Sweep liveness (manifest-body protection) {#sweep-liveness}

A PartManifest body's liveness is **owner-state + build-death + delete-after-sealed-decrements ordering** —
**not** blob in-degree (blob in-degree governs *blobs*; manifest bodies are governed by owner events). The
sweep must use the same protect test as today (`CasOrphanManifestSweep`):

- **Build-death (eligibility):** the build's `(writer_epoch, build_sequence)` is below the live floor in the
  per-server watermark (`min_active > build_sequence`, or the farewell/retired sentinel). A missing
  watermark ⇒ **not** eligible (never a frozen-seq/judged-dead guess).
- **Owner-state protection (never sweep these):** every committed ref's `manifest_ref` in `RootShard.refs`;
  every live precommit binding (a precommit `new_binding` not later removed below the sealed fold cursor);
  and every **pending** precommit removal whose `-1` is **not yet sealed/folded** (`transition_version >`
  sealed cursor) — protected so the GC fold can still read the body to emit that `-1`
  (delete-after-sealed-decrements; closes the B8 race).
- **Deletes are exact-token and fail-open:** `deleteExact(key, token)`; `NotFound`/`TokenMismatch` are
  spared (a fresh owner reclaimed the key, or a prior delete landed).

Phase 2 changes only the *cadence and pacing* of this test (cursor + budgets over `cas/manifests/`), not
the test itself.

## Safety {#safety}

The change preserves the existing proof shape:

- **Registry remains authority.** `gc/registry` still orders the first publish into a new namespace against
  the GC fence (`Store::ensureRegistered`). `LIST cas/refs/` is only a token accelerator and **never shrinks
  the universe** — discovery enumerates namespaces from the registry, not from the LIST.
- **Missing / ambiguous listed ref shard forces Read** (fail-closed), exactly as today — relocation does not
  change the default-Read discovery decision.
- **PartManifestId stays namespace-qualified.** No cross-namespace manifest sharing; the safety identity is
  unchanged in Phases 1–2 and re-grounded (allocator uniqueness) in Phase 3.
- **Exact-token deletes unchanged.** Blob and manifest deletes remain exact-token, idempotent, fail-open on
  `NotFound`/`TokenMismatch`.
- **No content-addressed tree/manifests** ⇒ no revival race introduced. Manifests stay mutable-keyed (by
  `PartManifestId`), not content-hash-keyed.
- **Sweep cursor is non-load-bearing.** No reachability decision (`fold`/`retire`/`recheck`) reads it.
- **Phase 3 uniqueness** rests on the existing per-process epoch + monotone build_seq + per-build ordinal —
  no new distributed coordination.

## TLA+ posture {#tla}

- **Phases 1 & 2:** identity-preserving and cursor-non-load-bearing ⇒ **no TLA+ identity change**. The
  existing `CaGcRootLocalPartManifestCore` proof model already treats manifest/ref objects as opaque
  identities and the namespace universe as registry-authoritative; relocating their keys and cursor-pacing
  the sweep do not touch any modeled action's pre/post-state. (The model never modeled the physical LIST
  prefix.) A short note records that the discovery-LIST source moved.
- **Phase 3:** recheck the manifest-identity invariant under the new allocator. `NoManifestIdReuse` →
  uniqueness of `(namespace, writer_epoch, build_sequence, manifest_ordinal)` by construction; confirm the
  model's `ManifestInstances`-as-opaque-id assumption still holds when the id is a per-build ordinal under a
  per-process-unique `(writer_epoch, build_sequence)`. This is a targeted invariant recheck, not a new gate.

## Testing {#testing}

- **Phase 1:** gtest — discovery LISTs `cas/refs/` and ignores `cas/manifests/` + `roots/` noise; a planted
  `cas/manifests/` backlog does not appear in discovery; fsck over the split classifies dangling/unreachable
  correctly; round-trip publish→fold→retire→reclaim still drains. Soak: confirm hot-discovery LIST no longer
  scales with manifest-backlog size.
- **Phase 2:** gtest — cursor advances/wraps; list/delete budgets respected; a build-dead orphan manifest is
  swept while a committed/live-precommit/pending-removal manifest is spared (the owner-state test); cursor
  loss/reset is a no-op for reachability (drive a round with the cursor cleared, assert no over-delete).
- **Phase 3:** gtest — ordinal allocation is monotone per build, caps at `999999` → `LIMIT_EXCEEDED`;
  `RefMatchesBody` holds; `(writer_epoch, build_sequence, ordinal)` round-trips through codecs;
  reuse-attempt across a simulated restart (fresh epoch) does not collide. TLA+ identity recheck green.

## Scope and non-goals {#scope}

- **In scope:** the layout relocation (A), cursor-paced sweep (C), and the `writer_epoch`/`manifest_ordinal`
  reshape (B), shipped Phase 1 → 2 → 3.
- **Recommended companion:** the `max_keys` server-side pagination fix in `CasObjectStorageBackend::list`
  (independent; de-fangs the O(N²) regardless of layout).
- **Non-goals:** no registry redesign; no new discovery index object (rejected — it would force a CAS on a
  hot single object per root mutation); no cross-namespace manifest sharing; no content-addressed
  tree/manifests; `blobs/` and `trees/` are unaffected. CA is pre-release with no persisted data, so the
  layout move needs **no migration**.
