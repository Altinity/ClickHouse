---
description: 'Design for supporting mutations, data-ALTER, and lightweight DELETE (patch parts, B5) on a content_addressed MergeTree disk, by lifting the supportsHardLinks gate and routing every part-producing path through the existing whole-part transaction with carry-forward by reference.'
sidebar_label: 'CAS MergeTree mutations design'
sidebar_position: 9
slug: /superpowers/specs/cas-mergetree-mutations
title: 'Content-Addressed MergeTree — Mutations, data-ALTER, and patch parts (B5) design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — mutations, data-ALTER, and patch parts {#mutations}

## Goal {#goal}

Support `ALTER TABLE … UPDATE/DELETE` (heavy mutations), `MATERIALIZE INDEX/COLUMN/TTL`,
data-`ALTER` (`MODIFY COLUMN` type change), and lightweight `DELETE` in its native
**patch-part** object model (backlog **B5**) on a `content_addressed` MergeTree disk.
These are currently gated off on CA because `DiskObjectStorage::supportsHardLinks`
returns `false`.

## Why this is tractable {#why}

The mutation machinery already exists and is the *natural* fit for content-addressing:

- `MutateTask` builds the mutated part through **one whole-part `DataPartStorage`
  transaction** (`beginTransaction` at `MutateTask.cpp:2871`/`:2993` →
  `commitTransaction` at `:1673`) — exactly like a merge. It is **not** the B21
  per-file-autocommit corruption hazard.
- Inside that transaction it does `createHardLinkFrom` for unchanged columns
  (`MutateTask.cpp:1961`) and `writeFile` for changed ones. On CA,
  `ContentAddressedTransaction::createHardLink` already implements
  **carry-forward by reference**: the new part's manifest entry points at the
  *same* blob hash as the source — **zero re-upload** of unchanged columns. Changed
  columns are written as fresh blobs. The single `commit` publishes one new
  `part_id` + manifest + ref.
- The capability audit shows `supportsHardLinks` has **exactly two consumers**, both
  pure gates in `MergeTreeData.cpp`: `checkAlterIsPossible:4481` (data-`ALTER`) and
  `checkMutationIsPossible:4979` (mutations + lightweight `DELETE`). **No code branches
  on the flag** to *choose* a per-file-autocommit path; `createHardLink` in `MutateTask`
  is unconditional and already CA-routed. The corrupting per-file-clone paths
  (partition clone, BACKUP hard-link, replication) are gated by their **own
  independent** checks (`checkAlterPartitionIsPossible`, B34, B33).

So mutation support is: **lift the gate carefully + verify end-to-end + handle edge
cases**, not a from-scratch whole-part-contract build (B30's safety is already
delivered by the gates).

## Approach (selected: hybrid) {#approach}

gtest **only the new/risky invariants** (mutation carry-forward dedup; patch-part
reachability + no-leftovers), then use the **real stateless suite** as the acceptance
oracle (M6-style), then add dedicated stateless correctness oracles + a no-leftovers
proof, and **un-tag** the `no-content-addressed-storage` tests that now pass.

## Design {#design}

### 1. Gate-lift (entry point) {#gate-lift}

`DiskObjectStorage::supportsHardLinks` returns `true` for a `content_addressed`
metadata storage. Per the audit this un-gates exactly: data-`ALTER`
(`MergeTreeData.cpp:4481`) and mutations + lightweight `DELETE`
(`MergeTreeData.cpp:4979`). The corrupting per-file-clone paths stay gated by their
own checks. A **regression test asserts partition-clone still rejects** on CA (the
flag did not leak the clone path open).

### 2. Heavy mutations = carry-forward + fresh columns, one whole-part commit {#heavy-mutations}

No new code on the hot path — `MutateTask` already routes through the CA whole-part
transaction. Per mutation:

- Unchanged columns → `createHardLinkFrom` → CA `createHardLink` references the same
  blob hash (no re-upload).
- Changed columns → `writeFile` → fresh blobs.
- One `commitTransaction` → new `part_id` + manifest + ref.

The mutated part's name encodes the new mutation version → a **distinct ref**. Its
mutable per-part files (`txn_version.txt`, `metadata_version.txt`, `uuid.txt`) live in
**its own ref sidecar** (B23), so they never collide with the source part's.

### 3. Data-ALTER (`MODIFY COLUMN` type change) {#data-alter}

Goes through the same `MutateTask` path (it *is* a mutation); un-gated by the same
flag. A type change rewrites the affected column (fresh blobs) and carries the rest
forward by reference.

### 4. Patch parts (B5) — lightweight DELETE native mode {#patch-parts}

A patch part is a **normal CA part**: written through the same temp-part → commit
transaction, producing its own manifest + blobs + ref under the **same**
`store/<server>/<uuid>/refs/` namespace. Part names are distinct, so there is no ref
collision and **no new namespace is required**.

- **Reachability:** the GC sweep (`ContentAddressedGC::listLivePartIds`) enumerates
  **all** refs under `refs/`, so patch-part refs become roots automatically → their
  blobs are protected from GC.
- **No-leftovers:** on `DROP`, patch-part refs unlink with the rest → their blobs
  become unreachable → reclaimed by the sweep.
- **Apply-on-read / patch-during-merge:** consume the patch part via
  `getStorageObjects` like any part — unchanged.

### 5. Active-set discovery on startup {#discovery}

`loadDataParts` enumerates `refs/` → part names → MergeTree classifies patch-vs-regular
from the part name / `MergeTreePartInfo` (existing logic, name-based). No CA-specific
discovery change; verify patch parts survive a server restart.

### 6. Edge cases / fail-close {#edge-cases}

- **Mutation rewriting all columns** — no carry-forward; all fresh blobs; still one
  commit.
- **Empty-result mutation** — produces an empty part through the same path.
- **Cancelled / failed mutation** — the temp part is discarded; **no ref is
  published**, so the freshly-written blobs are unreferenced and reclaimed by the GC
  sweep (fail-close; no partial ref).
- **`metadata_version.txt` rewrite** — handled as a sidecar rewrite on the new ref
  (B23), not a manifest change.

## Acceptance {#acceptance}

1. **New-invariant gtests** — (a) mutation carry-forward: an `ALTER UPDATE` of one
   column reuses the unchanged columns' blob hashes (assert *no* new blob object for
   carried files, a new blob for the changed column, a new `part_id`/manifest/ref);
   (b) patch-part reachability: a written patch part's blobs are reachable (survive a
   sweep) and are reclaimed after the ref unlinks.
2. **Real stateless oracle** — the mutation / lightweight-`DELETE` / `ALTER
   UPDATE`/`DELETE` stateless tests currently tagged `no-content-addressed-storage`
   pass under the CA-default config (triage + fix what breaks, M6-style).
3. **Dedicated correctness oracles** — `ALTER UPDATE`/`DELETE` and lightweight `DELETE`
   on a CA-disk table vs a normal table produce identical results; mutate→`DROP` leaves
   no blobs/parts (no-leftovers); `_pool_meta` survives.
4. **Un-tag** the `no-content-addressed-storage` tests that now pass; the remaining
   skip list shrinks to genuinely-unsupported features only.

## Out of scope {#out-of-scope}

- Replication (B1), multi-mounter pin+lease+fence GC (B32), full BACKUP/RESTORE
  (B16/B35), projections (B5-projections — a separate manifest sub-object) remain
  deferred and gated.
- `lightweight_mutation_projection`-via-projection mode is out of scope where it relies
  on projections (which stay gated); the patch-part native mode is the target.

## Self-review {#self-review}

- **Coverage:** heavy mutations (§2), data-ALTER (§3), patch parts/B5 (§4), discovery
  (§5), edge cases (§6) — each maps to an acceptance item.
- **Safety:** the gate-lift audit (two pure-gate consumers, no path-choosing branch)
  is the load-bearing fact; a regression test pins that partition-clone stays gated.
- **No layout change:** `blobs/`/`parts/`/`refs/` unchanged; patch parts reuse the
  existing ref namespace and sidecar mechanism; no new on-disk format.
