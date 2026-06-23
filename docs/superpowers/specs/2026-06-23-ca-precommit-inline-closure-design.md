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

### 1. Inline closure on the precommit journal `Add` record
The closure lives on the precommit's **journal `Add` record** (`JournalRecord`, `Core/CasRootShardCodec.h`,
protobuf via `Proto/cas_root_shard.proto`, B164a) — **not** on `RefPayload`. The record gains an inline
closure: the staged `[TreeEntry]` structure the build already built in `stageTree` (nested; adopted
subtrees recorded as **leaf** roots, since their children are protected by the live source they were
adopted from and must not be reclaimed with this build). Populated **only on precommit-namespace `Add`
records**; table-namespace records and all `Remove` records keep just `tree_id`.

**Why the `Add` record and not `RefPayload` (load-bearing — earlier framing was wrong).** GC protects a
precommit by **folding its journal `Add`**, and it must source the closure at that fold. But the
`RefPayload` is **erased before the `Add` is folded**: `Build::publish` does `refs.erase` + a `Remove`
record on commit (`CasBuild.cpp:1012`), and `reclaimAbandonedPrecommit` does the same on abandon
(`CasGc.cpp:1947-1953`). A closure on `RefPayload` is therefore gone (`root.refs.at(ref)` →
`key not found`) by the time the abandoned-build `Add` folds — exactly the S2 path we must close. The
journal `Add` record **survives** both commit-erase and table-ref displacement; it is removed only by
`trim` (`CasGc.cpp:169`) **after** the record is folded into the durable snap. So the `Add` record is the
only carrier that is present precisely when GC needs it.

This is **not** journal refcounting. It is **one** bounded closure list on the **single** precommit `Add`
record — transient (precommit-namespace only, trimmed once folded), sized by one build's staged tree. It
is NOT a per-blob `+1`/`-1` stream of journal records (that *would* be refcounting and is rejected).

The closure covers **only the nodes this build STAGED** (its own `stageTree` output — the nodes subject to
the S2 displaced-before-expansion leak). A precommit whose **root is adopted** (not staged) legitimately
carries an **empty** closure, and a staged manifest may reference **adopted subtrees** absent from the
closure. Adopted nodes are **not** a leak risk: they are protected by the **live source** they were adopted
from, so their tree OBJECT is present and GC reads it normally (see §3 — the precommit fold sources staged
nodes inline and **falls back to `readTree` for adopted nodes**). Two production paths precommit an adopted
root and so produce an empty closure: replication relink (`adoptTree` + `precommit`,
`ContentAddressedMetadataStorage.cpp`) and rename/detach/move (`adoptEvidence` + `precommit`,
`ContentAddressedTransaction.cpp`, deliberately no HEAD before precommit per B190). An empty closure is
therefore **normal, not corruption** — there is no fail-closed guard on it.

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

- **Source (`entriesOf`) — ONE unified function** for both namespaces: `closure.contains(node) ?
  closure[node] (inline, no I/O — the S2 protection for STAGED nodes) : readTree(node) (backend — for
  ADOPTED/committed nodes, whose object a live source keeps present)`. For a table-ns `Add` the closure is
  empty, so the source is always `readTree` — identical to today. The `readTree` 404/displaced/pending
  handling lives here and ONLY here (`CasGc.cpp:~1268-1340`).
- **Visitors:** `fold` → `addTreeEdge`/`addPackEdge` + `markExpanded`; `fsck` → mark reachable. (Reclaim
  is NOT a walk consumer — see §4: it reuses the existing cascade.)
- The walk **recurses on `Subtree`** (like fsck does today, `CasFsck.cpp:108`), which **fixes the latent
  fold-expand gap** (`CasGc.cpp:1353` adds the subtree edge but never expands it — masked today by flat
  manifests). `fsck`'s `walk` and `fold`'s expand are refactored onto this one routine.

### 3. Precommit protection = unified expansion (inline for staged, `readTree` for adopted)
On folding ANY `Add` (precommit or table), expand exactly as today — `addRootEdge` then per-child
`addTreeEdge`/`addPackEdge` + `markExpanded` — driving `walk(manifest_tree, entriesOf=source, …)` with the
**one unified source** of §2. The closure feeding that source is read from the **folding `JournalRecord` in
hand** (`record.closure`) — **never** from `root.refs` (which may already be erased; §1). The resulting
snap state (root edge + tree edges + marker) is identical to a committed expansion. Consequences:
- **STAGED nodes** (in the closure) are sourced inline ⇒ the precommit fold **does not read them**, so their
  children are recorded **regardless of whether the tree OBJECT exists** ⇒ **S2 closed by construction** for
  the build's own staged closure (the only nodes the displaced-before-expansion race can strand).
- **ADOPTED nodes** (closure-absent: an adopted root, or an adopted subtree inside a staged manifest) fall
  back to `readTree`. This is safe and non-circular: an adopted node's object is kept present by the **live
  source** it was adopted from, so reading it is reading a live-protected object, NOT reviving a condemned
  one ([[feedback-ca-resurrect-invariant]]). Recording their real child edges here also avoids the
  `markExpanded`-with-no-edges hazard (a later real expansion being suppressed by the once-per-tree gate).
- The two former branches (precommit vs table) **collapse into one** — the unified source is the difference,
  so there is no separate "precommit tree-read" to delete and no empty-closure fail-closed guard. This is
  the tree-walk simplification the design set out to deliver.
- **404 on an adopted node during a precommit fold** (the live source was concurrently dropped — a
  multi-leader race) is **tolerated as pending**: do not `markExpanded`, record no partial edges, skip — the
  same precommit pending-tolerance the table path already has for a displaced root. The doomed precommit is
  caught by the fail-closed commit gate or reclaimed; failing closed here would turn a benign cross-server
  race into GC-stopping exceptions. (For a **table** ns the existing displaced-later/`FailClosed` contract is
  unchanged: a live table ref to a missing tree with no later displacing record is still a dangle.)
- A blob shared between an in-flight precommit and a committed part has one `tree→blob` edge per referencing
  tree (distinct trees ⇒ two edges into the blob); pure edge arithmetic, no "seed if not known" special-case.

### 4. Reclaim — the EXISTING path, now always effective (no new walk)
`reclaimAbandonedPrecommit` is **unchanged**: it drops the dead precommit ref and journals a `Remove`.
The next fold's `removeRootEdge` drops the precommit root edge → the manifest tree reaches in-degree 0 →
retire. From there the children are released by machinery that **already exists**:
- manifest tree object **present** → normal cascade `stripTree(manifest_tree)` releases its children;
- manifest tree object **absent** (never uploaded / deleted) → **B199-S1** (committed `6d1e2daae40`):
  retire's absent-tree branch already `stripTree`s before `forget`, releasing the children.
Either way the children (recorded by §3's unified expansion) drop to in-degree 0 and go through
the **unchanged** retire → fence → recheck → exact-token-delete tail. A never-uploaded member → `deleteExact`
`NotFound` ⇒ idempotent no-op (`CasGc.h:278`). Shared members keep their committed tree's edge → spared.
So S2 is closed by §3 alone (always-record) + the already-landed S1/cascade (always-release); reclaim
needs no closure-walk of its own.

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
- `Core/CasRootShardCodec.{h}` + `Proto/cas_root_shard.proto` — `JournalRecord` inline closure field
  (+ codec); `RefPayload` carries NO closure.
- `Core/CasBuild.cpp` — `Build::precommit` populates the inline closure on the `Add` journal record it
  appends (from staging, in-memory).
- `Core/CasGc.cpp` — refactor fold-expand onto the unified walk; every `Add` expands via the **one unified
  source** (`record.closure`-inline for staged nodes, `readTree` fallback for adopted nodes — §2/§3). KEEP
  the precommit pending-tolerance so a 404 on an adopted node during a precommit fold is tolerated (§3); the
  table-ns displaced/`FailClosed` contract is unchanged. There is NO empty-closure fail-closed guard.
  `reclaimAbandonedPrecommit` is unchanged (existing `Remove`→cascade; §4).
- `Core/CasGcSnap.{h,cpp}` — **no new edge helpers needed**: inline-sourced expansion reuses
  `addRootEdge`/`addTreeEdge`/`markExpanded` and reclaim reuses `removeRootEdge`/`stripTree` exactly as
  the committed path does. (Touch only if the unified-walk refactor wants a small shared helper.)
- `Core/CasFsck.cpp` — refactor `walk` onto the unified routine.
- `docs/superpowers/models/CaBuildRootPrecommit.tla` — the model extension above.
- `src/Disks/tests/gtest_cas_gc_leak.cpp` — S2 tests flip GREEN; add a nested-manifest (subtree) test
  exercising the recursion fix.

## Testing
- `..._S2_NoFoldBetween` (Build-level) and `CaWiringGc.DisplacedTreeBlobsReclaimedThroughRealPath`
  (genuine `ContentAddressedTransaction` path) → GREEN (`unreachable=0`, `dangling=0`).
- `..._S1_FoldBetween` stays GREEN; `CasGcDangle.*` / `CasReuseGcRace.*` stay GREEN; `dangling=0`.
- **Adopted-root regression (the empty-closure path)**: run a GC round to fixpoint AFTER a replication
  `adoptPart` and AFTER a rename/`republishRef`, asserting the precommit fold does **not** throw and
  `dangling=0` (these adopt an unstaged root → empty closure → must expand via the `readTree` fallback, not
  fail closed). This is the coverage the original tests lacked.
- New nested-manifest test: a tree with a built `Subtree` whose blobs must be expanded/reclaimed (proves
  the unified walk recurses; today's fold would miss them).
- Full `Cas*`/`CaWiring*` suite no-regress (known reds only).
- After: a chaos soak (multi-leader stale-delete is S2's trigger) showing `unreachable` drains to ~0.

## Out of scope
- Variant 3 (all refs carry flat closures; GC never walks trees) — fattens hot manifests; not now.
- Packs (`PackSlice`) — M-F; the walk handles the placement but packs aren't produced yet.
- `snap_shards > 1` — remains M-C3 NOT_IMPLEMENTED; helpers written shard-agnostic but not exercised.
