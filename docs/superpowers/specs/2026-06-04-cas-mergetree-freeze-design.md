---
description: Design spec for FREEZE / UNFREEZE PARTITION on a content-addressed MergeTree disk — a per-part frozen ref-set in a GC-rooted shadow namespace that shares the live part's blobs zero-copy.
sidebar_label: 'CAS MergeTree FREEZE PARTITION'
sidebar_position: 8
slug: /superpowers/specs/cas-mergetree-freeze-partition
title: 'Content-Addressed MergeTree — FREEZE / UNFREEZE Design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — FREEZE / UNFREEZE Design {#cas-freeze-partition}

**Status:** design spec, awaiting review. **Date:** 2026-06-04. **Backlog:** B4 (the FREEZE gate). Second
of the FREEZE+FETCH pair (FETCH landed first; this is the larger, separate FREEZE spec).

## 1. Goal and scope {#goal}

Support `ALTER TABLE … FREEZE PARTITION`, `FREEZE`, `UNFREEZE PARTITION`, and `UNFREEZE` on a
content-addressed (CA) disk. All four are gated `Code: 344 SUPPORT_IS_DISABLED` today (the CA branch of
`checkAlterPartitionIsPossible`). On a normal disk `FREEZE` clones each active part into
`shadow/<backup_name>/<relative_data_path>/<part>/` as a hardlink snapshot, so the data survives even after
the live part is merged or dropped; the user later restores it (`ATTACH` from `detached`, or an out-of-band
copy). `UNFREEZE` removes a named backup.

**In scope:** lift the gate; make a `FREEZE` land a usable, GC-pinned snapshot on CA via a **per-part frozen
ref-set** in a new pool-root `shadow/` namespace; make `UNFREEZE` remove it; un-gate the 7 FREEZE tests + a
focused inline-CA oracle.
**Out of scope:** `FREEZE` of a remote-to-remote copy across disks (`freezeRemote`); `BACKUP`/`RESTORE`
(B16/B34/B35, separate); any change to how non-CA `FREEZE` works.

## 2. How FREEZE works (verified) {#how}

`MergeTreeData::freezePartitionsByMatcher` (`MergeTreeData.cpp:9654`) snapshots a vector of active parts.
For each matched part it calls `data_part_storage->freeze(backup_part_path, part_dir, …, params)` with
`params.make_source_readonly = true`, where `backup_part_path = shadow/<backup_name>/<relative_data_path>`
and `relative_data_path` for a CA disk is `store/<uuid[:3]>/<uuid>/`. So the on-disk target the disk layer
sees is `shadow/<backup_name>/store/<uuid[:3]>/<uuid>/<part>/<file>` (relative to the disk root, i.e. the
pool `key_prefix`).

`DataPartStorageOnDiskBase::freeze` (`DataPartStorageOnDiskBase.cpp:504`) is **already CA-aware**: when the
caller supplied no transaction and the disk `isContentAddressed`, it opens one self-owned whole-part
transaction (`owned_transaction`) so the whole clone commits as a single content-addressed part (the B21
fix — without it the per-file `createHardLink` autocommit would publish a one-file ref per file and corrupt
the clone). The clone runs through `Backup(…)`, which re-records each file; on CA each file
content-addresses and `condCreateIfAbsent`-dedups against the pool, so a freeze of a part already in the
pool **uploads no new blobs** and — because `computePartId` is content-only (B6) — yields the **same
`part_id`** as the live part.

`UNFREEZE` (`unfreezePartitionsByMatcher`, `MergeTreeData.cpp:9789`) lists the backup subtree
`shadow/<backup_name>/<relative_data_path>` and removes the matched parts via
`Unfreezer::unfreezePartitionsFromTableDirectory`.

**Two things are nonetheless broken on CA today** (both block the gate lift):

- **(a) GC reachability.** GC roots are the union of every key with a `/refs/` segment under
  `refsRootPrefix = <key_prefix>/store/` (`ContentAddressedGC.cpp:97`, `listLivePartIds`). The freeze path
  is `<key_prefix>/shadow/<backup>/store/<uuid>/<part>` — its `store/` segment is **under** `shadow/`, not
  under the scanned `store/` root — so a frozen snapshot's blobs are GC-reclaimable the moment the live part
  merges away. That defeats the entire purpose of `FREEZE`.
- **(b) Ref collision.** `parsePartFilePath` (`PoolPaths.cpp:283`) anchors on the `<uuid[:3]>/<uuid>` pair
  regardless of a leading `shadow/<backup>/` prefix, so the freeze commit currently resolves to the **same**
  ref location as the live active part (`store/<server>/<uuid>/refs/<part>`) and clobbers it.

## 3. Design — a per-part frozen ref-set in a GC-rooted shadow namespace {#design}

### 3.1 Lift the gate {#gate}
Add `PartitionCommand::FREEZE_PARTITION`, `FREEZE_ALL_PARTITIONS`, `UNFREEZE_PARTITION`, and
`UNFREEZE_ALL_PARTITIONS` to the `supported_commands` set in the ContentAddressed branch of
`checkAlterPartitionIsPossible` (`MergeTreeData.cpp:~6583`) — these are the four values
`ALTER … FREEZE`/`UNFREEZE` dispatch on (`PartitionCommands.h:30-33`, `MergeTreeData.cpp:6999-7024`). Update the adjacent comment: `FREEZE`/`UNFREEZE` are now
supported (a CA freeze publishes a frozen ref-set under `shadow/`, a GC root).

### 3.2 The frozen ref-set namespace {#namespace}
Define a new pool-root namespace keyed parallel to the live refs but rooted at `shadow/`:

```
<key_prefix>/shadow/<backup_name>/<server_id>/<uuid>/refs/<part>        → part_id        (the frozen ref)
<key_prefix>/shadow/<backup_name>/<server_id>/<uuid>/refs/<part>.meta   → RefSidecar     (per-ref sidecar)
```

**One ref per frozen part** — not a shared container. This is the deliberate divergence from the `detached`
namespace (B36/B46), which keys many detached parts under one shared `detached` ref and therefore needs a
`gc_lock`-serialized read-modify-write on commit (and still has the B66a torn-read under concurrent fan-out).
Because each frozen part gets its own ref object, concurrent freeze (`03611`/`03612`, the parallel-verbose
tests) writes **distinct objects with no shared-RMW** — no lost update, no torn read, no extra lock. Add
`shadowRefsRootPrefix(key_prefix)` = `<key_prefix>/shadow/` and the per-part key helpers to `PoolPaths`.

### 3.3 Commit redirect {#commit}
In `ContentAddressedTransaction::commit`, detect a frozen target: when the part path's **leading component
is `shadow`** (checked **before** the generic uuid-anchor ref-location logic, per Risk §7a), publish the
ref + sidecar at the shadow location (`shadow/<backup>/<server>/<uuid>/refs/<part>`) instead of the live
`store/<server>/<uuid>/refs/<part>`. Everything else about the commit is unchanged: the manifest is built
from `recorded` exactly as for a live part, the blobs are re-validated under the `gc_lock` (B49), and the
`part_id` is content-only (B6) so it equals the live part's. No shared-ref merge (§3.2), so no detached-style
RMW. The backup name and `server_id`/`uuid` are recovered from the parsed shadow path.

### 3.4 GC root {#gc-root}
Generalize `listLivePartIds` (`ContentAddressedGC.cpp`) to walk **both** `refsRootPrefix` (`store/`) and
`shadowRefsRootPrefix` (`shadow/`). The existing `/refs/`-segment filter and `.meta` skip apply unchanged —
a shadow ref has a `/refs/` segment and its payload is a `part_id`, so it is read identically. Frozen blobs
are now reachable as long as any frozen ref names their part, independent of the live part. Dropping the
live partition (or merging it away) no longer reclaims a frozen snapshot's blobs.

### 3.5 Read / list / remove of shadow paths {#routing}
Teach `ContentAddressedMetadataStorage` to route a `shadow/<backup>/…` path to the frozen ref-set, mirroring
the existing namespace-routing it already does for `detached` and projection sub-dirs:
- `existsDirectory` / `isDirectoryEmpty` / `listDirectory` over `shadow/<backup>/store/<uuid>/` enumerate the
  frozen parts for that backup+table (read from the shadow refs prefix) — this is what
  `Unfreezer::unfreezePartitionsFromTableDirectory` walks.
- `getStorageObjects` / `readFile` / `existsFile` / `getFileSize` over a `shadow/…/<part>/<file>` resolve via
  the frozen ref's manifest (the same `resolveBlobEntry` path the live refs use), so an `ATTACH` from the
  shadow snapshot — or any read of it — works.
- `removeRecursive` over `shadow/<backup>/…` deletes the frozen refs + sidecars for the matched subtree; the
  blobs become GC-eligible exactly when no other (live or frozen) ref names their part.

### 3.6 What does NOT change {#unchanged}
Non-CA `FREEZE`/`UNFREEZE` are untouched (every new branch is `isContentAddressed`-gated, and the
`freeze()` clone-transaction path already existed for CA). The live active refs, the `detached` namespace,
`unfreezePartitionsByMatcher`'s table-filtering, and `ATTACH` semantics are unchanged. No new blobs are
written for a part already in the pool (dedup).

## 4. Error handling {#errors}
Fail-closed, preserved. A shadow read miss falls through to the normal committed-read path and throws
`FILE_DOESNT_EXIST`/`CORRUPTED_DATA` exactly as today — the routing never substitutes an empty result. A
frozen ref is published only after its blobs are re-validated present under the `gc_lock` (B49), and it
points at the same `part_id` as a live part whose blobs already exist, so reachability can never name a
missing blob. `UNFREEZE` of a non-existent backup removes nothing and reports nothing (unchanged).

## 5. Testing {#testing}
- **Stateless (un-gate + empirical probe):** remove `no-content-addressed-storage` from the 7 FREEZE tests —
  `00952_part_frozen_info`, `01325_freeze_mutation_stuck`, `01414_freeze_does_not_prevent_alters`,
  `01417_freeze_partition_verbose`, `01417_freeze_partition_verbose_zookeeper`,
  `03611_freeze_partition_parallel_verbose`, `03612_freeze_partition_parallel_verbose_zookeeper` — run on the
  CA-default job; fix per §3; **re-gate with a precise reason** anything that fails for an orthogonal reason
  (e.g. a verbose-output column format or `is_frozen` plumbing that is not a CA-FREEZE bug), consistent with
  the FETCH `03350` handling. Do not leave a failing un-gate.
- **Focused oracle (inline CA disk):** a CA table; insert; `FREEZE PARTITION`; **drop the live partition**;
  assert the frozen snapshot is still listable (`shadow/` survives the drop — proving the GC root); `ATTACH`
  the frozen part back (or read it) and `SELECT` the exact source data; `UNFREEZE`; assert the backup is gone
  and (optionally, via a `SYSTEM` GC trigger if available) the blobs become reclaimable. Hand-verify the
  reference.
- **Non-CA regression:** a couple of plain `FREEZE`/`UNFREEZE` tests on the default (non-CA) job → no
  regression from the gate-set change and the commit/GC/routing branches (all `isContentAddressed`-gated).
- **Unit (gtest):** a CA transaction freezing a part publishes a `shadow/` ref resolvable to the same
  `part_id` as the live part; the GC marks the frozen part's blobs reachable from the shadow root; after
  `removeRecursive` of the shadow subtree the frozen ref is gone and the blobs are no longer reachable (if
  unreferenced). All `ContentAddressed*` gtests still pass.

## 6. Plan phasing {#phasing}
1. Gate lift + `PoolPaths` shadow namespace helpers (`shadowRefsRootPrefix`, the per-part key + the leading-
   `shadow` detection); build; un-gate the 7 tests and run on CA-default to surface the actual behavior (the
   empirical risk probe).
2. Commit redirect (§3.3) + GC root generalization (§3.4) + the gtest; build; re-run.
3. Shadow path read/list/remove routing (§3.5) so `UNFREEZE` and `ATTACH`-from-shadow work; the inline-CA
   oracle; re-run the un-gated tests.
4. Non-CA regression + finalize un-gate (re-gate any orthogonal failure with a documented reason) + backlog
   (B4 → DONE for FREEZE/UNFREEZE; note `freezeRemote` / cross-disk freeze deferred); commit + push.

## 7. Risks {#risks}
- **(a) Namespace precedence.** The freeze path carries both a leading `shadow/` and an inner `store/<uuid>`
  that `parsePartFilePath` anchors on. The commit/read/list routing **must** key off the leading `shadow`
  component *before* the generic uuid-anchor logic claims the path as a live ref — otherwise §2(b)'s
  collision persists. Pinned by the gtest (a frozen ref must not land at the live ref key).
- **(b) `is_frozen` reporting.** `00952_part_frozen_info` reads frozen state via `system.parts`/`system.parts`
  columns; `freezePartitionsByMatcher` sets `part->is_frozen` in memory, but cross-session frozen detection
  may need the part loader to notice a shadow ref. Reproduction-driven in the probe; re-gate with a reason if
  it is orthogonal plumbing rather than a CA-FREEZE bug.
- **(c) Cross-table shadow subtree.** A single `shadow/<backup>` can span multiple tables/uuids. The
  per-`<uuid>` keying (§3.2) keeps tables separate; `unfreezePartitionsByMatcher` already filters by the
  table's `relative_data_path`, so listing `shadow/<backup>/store/<uuid>/` returns only that table's frozen
  parts.
- **(d) Concurrency.** Per-part frozen refs (§3.2) mean concurrent freeze writes distinct objects — no
  shared-RMW, so the parallel-verbose tests (`03611`/`03612`) are safe by construction, unlike the shared
  `detached` ref (B66a).
