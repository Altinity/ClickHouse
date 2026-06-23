# CA precommit inline-closure — design (B199-S2)

**Status:** design for review
**Date:** 2026-06-23
**Backlog:** B199-S2 (the never-expanded-tree leak class). Builds on B199-S1 (commit `6d1e2daae40`,
retire absent-tree `stripTree`) which closed the *expanded*-tree leak. Supersedes the earlier
"eager read-and-seed at reclaim" and "seed-if-not-known (Q2)" framings — see §"Why this supersedes".

## Problem

GC reclaims an object only if it appears in the in-degree snap, which is built by **expanding** trees
(reading the tree object once, recording `tree→child` edges — `CasGc.cpp:1342-1365`). When a build is
abandoned (crash / chaos kill before commit), `reclaimAbandonedPrecommit` (`CasGc.cpp:~1810`, called from
`fold`, `CasGc.cpp:1655`) drops the precommit ref and journals a `Remove`, then **delegates closure
release to the cascade** — which only works if the tree was expanded. If the tree object is gone before
its precommit `Add` was folded (a stale/competing-leader delete; the lease is work-dedup, not safety),
the expansion `readTree` 404s, hits the precommit pending-tolerance skip (`CasGc.cpp:1307-1316`), and the
closure is never recorded → the build's unique blobs leak as `unreachable` debris forever (space-only;
`dangling=0` — not data loss). Diagnostic repro: `CasGcLeak.DisplacedUnexpandedTreeBlobsLeakPrecommitPath_S2_NoFoldBetween`
and the raw `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` (both intentional-RED today).

There is **no safe late point** to recover the closure: a condemned object must not be GET-ed
([[feedback-ca-resurrect-invariant]]), and a deleted one can't be read at all. The only safe capture
point is the **writer at precommit**, which holds the full staged structure in hand.

## Goal

Close S2 **by construction** — make the precommit object carry its own closure, so GC never needs the
tree object for precommit protection or cleanup — while **reducing** the tree-traversal code (today three
near-duplicate walks: fold-expand, fsck, and the would-be reclaim walk), not adding to it. No new S3
object (operator constraint): the closure lives in the precommit ref payload we already publish.

## Design

### 1. Inline closure in the precommit `RefPayload`
`RefPayload` (`Core/CasRootShardCodec.h:29`, protobuf via `cas_root_shard.proto`, B164a) gains an inline
closure — the staged `[TreeEntry]` structure the build already built in `stageTree` (nested; adopted
subtrees recorded as **leaf** roots, since their children are protected by the live source they were
adopted from and must not be reclaimed with this build). Populated **only for precommit refs**; table
refs keep just `tree_id`.

The closure is **always populated** on every precommit. The feature is in development with **no prod
users and no pre-existing precommit objects**, so there is NO old-precommit / empty-closure case to
support and NO fallback to the old lazy tree-read (see §3 — the precommit tree-read is deleted outright).
The protobuf field being technically optional is just a codec detail, not a runtime contingency.

Stored form is the decoded `[TreeEntry]` (what `stageTree` produced) — not re-encoded tree bytes — so the
walk consumes `[TreeEntry]` uniformly with no re-encode and no duplication of the `trees/` object's bytes.

### 2. One traversal — `walk(root, entriesOf, visit)`
Replace the three near-duplicate closure walks with a single routine, parameterized by a **source** and a
**visitor**:

```
walk(root, entriesOf, visit):
    if not seen.insert(root): return            // dedup
    visit(root)                                  // per-node action
    for entry in entriesOf(root):                // source-provided children
        switch entry.placement:
            Blob:     visit_edge(root, Blob, entry.file_hash)
            PackSlice: visit_edge(root, Pack, entry.pack_hash)
            Subtree:  visit_edge(root, Tree, entry.file_hash); walk(entry.file_hash, entriesOf, visit)
            Inline:   (no object)
```

- **Sources (`entriesOf`):** `inline` (precommit — from the deserialized payload; **no I/O, never
  absent**) | `backend` (`readTree(node)` — the existing lazy expansion; **all absent/displaced/pending
  handling lives here and ONLY here**, `CasGc.cpp:1268-1340`).
- **Visitors:** `fold` → `addTreeEdge`/`addPackEdge` + `markExpanded`; `fsck` → mark reachable; `reclaim`
  → drop the precommit's flat edges (feeds the existing cascade).
- The walk **recurses on `Subtree`** (like fsck does today, `CasFsck.cpp:108`), which **fixes the latent
  fold-expand gap** (`CasGc.cpp:1353` adds the subtree edge but never expands it — masked today by flat
  manifests). `fsck`'s `walk` and `fold`'s expand are refactored onto this one routine.

### 3. Precommit protection = flat direct edges (not tree-expansion)
On folding a precommit `Add`, instead of `readTree`-expanding the tree, `walk(inline, addFlatEdge)` seeds
**direct edges `ref_pc → member`** for every closure member (recursively, per §2). Consequences:
- The precommit `Add` fold **no longer reads the tree** ⇒ **delete** the precommit branch of the
  expansion: the `readTree` for precommit refs and the precommit pending-tolerance/404 path
  (`CasGc.cpp:1307-1316`). The displaced/`FailClosed` handling for **table** namespaces is unchanged.
- A blob shared between an in-flight precommit and a committed part carries **two edges**
  (`table_tree→blob` + `ref_pc→blob`); in-degree = their sum. Pure edge arithmetic — no "seed if not
  known" special-case (that was an artifact of the seed-at-0 framing).
- Members are in the graph via the precommit edge **regardless of the tree object** ⇒ S2 closed by
  construction.

### 4. Reclaim = mirror −1 over the same inline closure
`reclaimAbandonedPrecommit`, on a dead precommit ref, runs `walk(inline, dropEdge)` to drop the
`ref_pc→member` edges (and the precommit root ref), exactly mirroring §3. Newly-zeroed members become
ordinary zero-in-degree candidates the next round and go through the **unchanged** retire → fence →
recheck → exact-token-delete tail. A member that was never uploaded (partial build) → `deleteExact`
returns `NotFound` ⇒ idempotent no-op (`CasGc.h:278`). Shared members keep their committed edge → spared.

### 5. Commit path — unchanged
A committed table ref → `tree_id` → `walk(backend)` expansion exactly as today. `uploadStagedTree` still
materializes the `trees/` object (for readers + dedup). The inline closure and the `trees/` object
overlap only in the brief `publish → commit-removes-precommit` window; after commit only `trees/` remains.

## Why this supersedes the earlier framings
- "Eager read-and-seed at reclaim" — can't read a gone/condemned tree; defeated by the same race.
- "Seed-if-not-known (Q2-A)" — needed only for the seed-at-indegree-0 model; the flat-edge model makes
  shared-blob safety pure edge arithmetic, so the guard is unnecessary.

## TLA+ (gate — operator-required)
Extend `docs/superpowers/models/CaBuildRootPrecommit.tla`: precommit seeds flat closure edges from a
recorded list; abort/reclaim drops them; a closure member may be never-uploaded (absent). Check
`INV-NO-DANGLE`, `INV-NO-LOSS`, `INV-NO-RETURN`, and a **no-leak** property (an abandoned build's closure
is eventually reclaimed). TLC clean within bounds is a gate before/with implementation.

## Files
- `Core/CasRootShardCodec.{h}` + `cas_root_shard.proto` — `RefPayload` inline closure field (+ codec).
- `Core/CasBuild.cpp` — `Build::precommit` populates the inline closure from staging.
- `Core/CasGc.cpp` — precommit `Add` → flat-edge seed via the unified walk; **delete** precommit
  tree-read + pending-tolerance; `reclaimAbandonedPrecommit` → mirror-drop via the unified walk (pass the
  fold snap in). Refactor fold-expand onto the unified walk.
- `Core/CasGcSnap.{h,cpp}` — flat-edge add/drop helpers if the existing `addTreeEdge`/`stripTree` don't
  cover `ref→member` directly; keep shard-agnostic (no new `snap_shards==1` debt).
- `Core/CasFsck.cpp` — refactor `walk` onto the unified routine.
- `docs/superpowers/models/CaBuildRootPrecommit.tla` — the model extension above.
- `src/Disks/tests/gtest_cas_gc_leak.cpp` — S2 tests flip GREEN; add a nested-manifest (subtree) test
  exercising the recursion fix.

## Testing
- `..._S2_NoFoldBetween` and raw `DisplacedUnexpandedTreeBlobsLeak` → GREEN (`unreachable=0`).
- `..._S1_FoldBetween` stays GREEN; `CasGcDangle.*` / `CasReuseGcRace.*` stay GREEN; `dangling=0`.
- New nested-manifest test: a tree with a built `Subtree` whose blobs must be expanded/reclaimed (proves
  the unified walk recurses; today's fold would miss them).
- Full `Cas*`/`CaWiring*` suite no-regress (known reds only).
- After: a chaos soak (multi-leader stale-delete is S2's trigger) showing `unreachable` drains to ~0.

## Out of scope
- Variant 3 (all refs carry flat closures; GC never walks trees) — fattens hot manifests; not now.
- Packs (`PackSlice`) — M-F; the walk handles the placement but packs aren't produced yet.
- `snap_shards > 1` — remains M-C3 NOT_IMPLEMENTED; helpers written shard-agnostic but not exercised.
