---
description: 'Design: every per-part file becomes an ordinary manifest tree entry (mutable set = empty); committed-part standalone writes/removes go through an honest audited manifest repoint; the MVCC tmp+rename dance is short-circuited on atomic-write storages'
sidebar_label: 'CAS All-Tree Part Files'
sidebar_position: 20260714
slug: /superpowers/specs/cas-all-tree-part-files
title: 'CAS Design: All-Tree Part Files (mutable set = empty) + Committed-Part Repoint'
doc_type: 'reference'
---

# CAS Design: All-Tree Part Files + Committed-Part Repoint {#cas-all-tree-part-files}

**Date:** 2026-07-14
**Status:** APPROVED (user design session 2026-07-14), awaiting implementation plan.
**Drivers:** (1) minimize the mutable-file set — investigation proved 2 of the 3
`kMutablePerPartFiles` are not actually mutable; (2) make the CA disk ready for the vanilla
corner-case writes (`writeChecksums`/`writeColumns` backfill, a hypothetically revived
`writeMetadataVersion`, future upstream sidecar files); (3) **fork compactness** — the feature
lives in a fork, so the diff outside `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`
must stay small and rebase-friendly. This design makes the outside diff *smaller* than today.

## 1. Findings that drove the design {#findings}

Verified against HEAD (branch `cas-gc-rebuild`) and `upstream/master`, 2026-07-14:

- **`txn_version.txt` is the only genuinely in-place-mutable per-part file.**
  `VersionMetadataOnDisk::storeInfoToDataPartStorage` rewrites it on a committed part
  (`createFile(tmp)` → `writeFile(tmp)` → `replaceFile(tmp → txn_version.txt)`, three separate
  autocommit operations) on every MVCC event: creation-CSN stamp after transaction commit, removal-TID
  lock, rollback-to-NULL, removal CSN. Non-transactional parts never materialize the file at all
  (deferred-persist in `VersionMetadataOnDisk`).
- **`uuid.txt` is write-once** — written only into uncommitted part directories
  (`MergedBlockOutputStream.cpp`, `MutateTask.cpp`, fetch), and it is *checksummed* (absent from
  `getFileNamesWithoutChecksums`), so an in-place change would break `CHECK TABLE` even on vanilla.
  The UUID of one logical part is identical on all replicas (propagated via the replication log
  entry `new_part_uuid` and fetch).
- **`metadata_version.txt` is write-once** — all live writers target uncommitted directories
  (creation, mutation, `cloneAndLoadDataPart` tmp clone, fetch). The only in-place writer,
  `IMergeTreeDataPart::writeMetadataVersion`, is dead code since `555fdaf6bd5` (2026-02, the
  chained-RENAME-COLUMN fix removed the ATTACH-time overwrite); zero callers on `upstream/master`
  too (the upstream revert branch `revert-96351-…` was never merged). It legitimately *diverges*
  across replicas for the same part (merge executed under different metadata snapshots during an
  `ALTER`) — which is why vanilla excludes it from checksums and from fetch checksum registration.
- **Vanilla "part immutability" was always soft**: `loadChecksums(require=false)` backfills a
  missing `checksums.txt` in place (`IMergeTreeDataPart.cpp` `writeChecksums`, no readonly guard);
  `loadColumns` backfills a missing `columns.txt` (readonly-guarded). Both are upgrade/repair paths
  for very old parts — unreachable by construction on a CA pool (no legacy parts; every CA part is
  written by a whole-part transaction that always includes both files) but they define the *class*
  of upstream behavior the CA disk must be ready for: upstream adds sidecar files over time
  (`columns_substreams.txt` is a recent example).
- **The relink wire already carries the sender's values**: `DataPartsExchange.cpp` sends
  `part->getMetadataVersion()` (the sender's file content) next to the manifest body, and the part
  UUID rides the vanilla protocol header. So moving both files into the manifest is
  behavior-preserving for fetch-by-relink — the receiver gets the sender's bytes either way, exactly
  as a vanilla byte fetch would deliver.
- **The committed-publish machinery already exists**: `CachedPartFolderAccess::publishEntries`
  (adopt-evidence over entries → stage a fresh manifest → precommit → promote) is shared by
  `republishRef` and `adoptPartFromManifest`. The promote guard (`CasBuild.cpp`, unique-ref
  invariant) already names the missing piece in its error text: *"use republishRef for an intended
  repoint"*.

## 2. Decision summary {#decisions}

All four decided in the 2026-07-14 design session:

1. **Every per-part file is an ordinary manifest tree entry.** `uuid.txt`,
   `metadata_version.txt`, **and** `txn_version.txt` move into the content tree (tiny → inline
   entries). The mutable set is **empty**; the per-ref `mutable_files` concept is deleted end-to-end.
2. **Committed-part standalone writes — add AND overwrite — go through an honest, audited manifest
   repoint** (not fail-closed, not a silent no-op). Rationale: this makes the CA disk robust
   against the whole class of present and future upstream sidecar behaviors while keeping the
   standard writer protocol as the only publish path. Every committed-part repoint is loud:
   a `system.content_addressed_log` event + `LOG_WARNING` + ProfileEvent (rare by construction —
   a tripwire through observability instead of a broken table load).
3. **The MVCC tmp+rename dance is short-circuited on atomic-write storages** — a small generic
   (non-CA-branded, upstream-candidate) patch in `VersionMetadataOnDisk`: when the part storage
   reports atomic file writes, store the version info with a single `writeFile`, no
   `txn_version.txt.tmp` + `replaceFile`. On CA this yields exactly **one repoint per MVCC store
   event** and lets the CA-specific eager-dispatch hook in `DiskObjectStorageTransaction` be
   deleted outright.
4. **Committed-content-file removal evolves from the B123 unconditional no-op to staged removal
   marks** resolved at publish time (see §6) — required so ATTACH really clears MVCC state, and it
   closes B123's documented fail-open.

Cost accepted: under (experimental) MergeTree transactions each MVCC store event becomes one
repoint ≈ 3–4 S3 writes + one small garbage manifest for GC, versus one cheap ref-payload CAS
today. Non-transactional workloads pay nothing (the file is never written). Cross-replica
`metadata_version` divergence during `ALTER` windows now produces two manifest bodies for one part
name — the same benign, bounded, self-cleaning dedup-MISS class as compressed-byte divergence
(`01-architecture.md §benign-cross-replica-divergence`); blob dedup and relink are unaffected.

## 3. What gets deleted {#deletions}

Inside `ContentAddressed/`:

- `kMutablePerPartFiles`, `isMutablePerPartFile` (with the `.tmp`-sibling logic) in
  `PartPathParser.h`.
- `RefPayload`/`Resolved::mutable_files` and `RefMutableFilesUpdate` — removed from the schema and
  the ref-log/snapshot codec payload (pre-release, no compat scaffolding; pools are recreated).
- `CachedPartFolderAccess::updateMutableFiles`; the `mutable_files` parameter of `promoteBuild` and
  `publishEntries`; `Build::pending_mutable_files` / `setPendingMutableFiles`.
- `PartFolderView` mutable-files serving and the `.ca_*` reserved-name filtering
  (`isReservedMutableName`) — no production writer exists.
- All four `Freshness::ForceFresh` mutable-file branches in `ContentAddressedMetadataStorage`
  (`existsFile`, `getFileSize`, `getStorageObjects`, byte-read) — these reads go through the
  cached folder view like every tree file. Part load loses its last forced uncached ref GETs.
- The mutable-file staging map handling in `ContentAddressedTransaction` (`writeFile` inline-buffer
  branch, `mutable_removed`, the by-value carry-forward branch in `createHardLink` — B62/B36/B46
  shapes ride the tree like everything else).
- The relink sidecar reconstruction (`sidecar_values`) and the `metadata_version` wire field in
  `DataPartsExchange` relink blocks (the manifest is self-contained; the protocol-header UUID field
  is vanilla and stays).

Outside `ContentAddressed/` (net shrink of the fork diff):

- `isContentAddressedMutablePartFileRename` + the `#include …/PartPathParser.h` in
  `DiskObjectStorageTransaction.cpp` (the B182 eager-dispatch hook) — deleted; with the §5
  short-circuit there is no rename to dispatch. Closes BACKLOG §9
  "[refactor: DiskObjectStorageTransaction part-path virtualization]" as moot.
- The `metadata_version_written_by_freeze` special case in `MergeTreeData.cpp` (`cloneAndLoadDataPart`)
  — the post-clone write recomputes identical bytes, so it lands as a byte-equal repoint **no-op**
  (§4); the skip is unnecessary.
- (Addition, small and generic:) the atomic-write capability probe + single-write branch in
  `VersionMetadataOnDisk` (§5) — an upstream-candidate improvement, not CA-branded.

## 4. The repoint primitive {#repoint}

`CachedPartFolderAccess::repointRef(key, entries')` — a thin wrapper over the existing
`publishEntries` sequence with one addition: the final promote passes an explicit
**intended-repoint** flag through the unique-ref guard (which today throws `ABORTED` on a committed
destination with different content). Semantics:

- **Byte-equal no-op:** if the recomputed `ManifestId` equals the currently committed one, the
  repoint performs zero pool mutations and emits no audit event.
- **Atomicity:** precommit of the new manifest records the full closure (carried-forward blob
  hashes included) *before* any backend observation — EDGE-BEFORE-OBSERVE is preserved by
  construction because this is the standard writer path. Promote is the atomic owner-move onto the
  same ref key; the old manifest becomes unreachable and is collected by the normal GC fold — no
  new GC invariant.
- **Audit:** every effective committed-part repoint logs a `system.content_addressed_log` event
  (new event type, e.g. `part_repoint`), a `LOG_WARNING` naming the part and files, and a
  ProfileEvent counter. Soak asserts the counter is zero on non-transactional workloads.
- **Trigger:** the `publishStaging` branch that today serves "staging without a Build on a
  committed ref" (currently the mutable-only autocommit shape): it now resolves the current
  manifest, carries its entries forward, applies the transaction's staged writes (add/overwrite)
  and removal marks (§6), and calls `repointRef`. One repoint per disk transaction regardless of
  how many files it touched.
- **Concurrency:** the per-table single-writer ref lane + the promote CAS serialize concurrent
  repoints; MergeTree-level part locks make committed-part standalone writes single-actor in
  practice (per-server-owned namespaces — no cross-server writer exists).

This one primitive serves: the `writeChecksums`/`writeColumns` backfill, a revived
`writeMetadataVersion`, any future upstream sidecar write, and the MVCC store events of §5.

## 5. MVCC short-circuit and the eager-hook deletion {#mvcc-short-circuit}

`VersionMetadataOnDisk::storeInfoToDataPartStorage` keeps its two-phase tmp+rename only because a
plain local-FS write is not atomic. CA writes are atomic by construction (staging → ref CAS), so:

- Add a small capability probe — `IDataPartStorage::supportsAtomicFileWrites()` (default `false`;
  `DataPartStorageOnDiskBase` delegates down to the disk/metadata storage; the CA metadata storage
  answers `true`). Naming/plumbing to be settled in the plan; the probe is generic and useful to
  any object-storage-backed metadata storage — an upstream candidate.
- When `true`, `storeInfoToDataPartStorage` performs a single `writeFile(txn_version.txt)` +
  finalize — no `createFile(tmp)`, no `replaceFile`. One autocommit transaction → one repoint per
  MVCC store event; `txn_version.txt.tmp` never exists on CA (the `removeTmpMetadataFile` recovery
  shape becomes unreachable there).
- The B182 eager-dispatch hook in `DiskObjectStorageTransaction.cpp` existed only to keep the
  tmp→final rename ordered before the part publish inside explicit-transaction merges. With no
  rename, the hook and its `PartPathParser.h` include are deleted.

MVCC correctness notes: the storing-version optimistic check in `VersionMetadataOnDisk` is
process-local (mutex + read-back) and unaffected; read-your-writes after a repoint is provided by
the primitive's owned cache side effect (the affected folder view is invalidated on success —
Phase-4 discipline), not by `ForceFresh`; crash mid-repoint leaves the old manifest committed and
the new one as precommit garbage for GC — strictly stronger than the vanilla tmp-file crash
protocol.

## 6. Removal semantics — B123 evolution {#removal-semantics}

Today `ContentAddressedTransaction::unlinkFile` of a committed content file is an unconditional,
documented no-op (B123): the MergeTree fast-removal path unlinks part files and then removes the
directory, and the removal unit on CA is the whole-part ref-drop. With `txn_version.txt` in the
tree, three flows need honest single-file deletion: ATTACH (`loadPartAndFixMetadataImpl` →
`removeVersionMetadata` must clear stale MVCC state), detached-copy cleanup
(`MergeTreeData.cpp` detach-time `removeFileIfExists`), and symmetry for future sidecar removals.

New shape: the unlink **stages a removal mark**; at publish:

- if the same transaction also removes the part directory (ref-drop) — marks are superseded,
  nothing extra happens; the dominant CA removal path (`removeSharedRecursive` → whole-dir →
  ref-drop) is unchanged and pays zero repoints;
- a batched `removeSharedFiles(request)` transaction without a dir-drop resolves to **one** repoint
  publishing the manifest minus the removed entries (the part is already out of the working set at
  that point — no readers); the ref-drop that follows in its own transaction then frees the part;
- a lone surgical unlink (`removeVersionMetadata` at ATTACH) resolves to one repoint-remove —
  correct and rare.

This *closes* B123's documented fail-open (a surgical single-file delete silently not happening);
the B123 comment is rewritten to describe the mark/supersede model.

## 7. GC / TLA+ gate {#gc-tla}

The repoint transition is "ref repoints M1 → M2 on the same key" — structurally the shape
`republishRef` already exercises (publish new manifest, then the source ref side; here source =
destination). Phase 0 of the implementation plan: check whether the current GC part-manifest TLA+
model contains a same-key repoint transition (ref's manifest edge replaced while the old manifest
is still present); if not, add it and run the gate green before any code. Expected result: no new
invariant — precommit protects the new closure, the old manifest ages out through the normal
fold/retire path.

**Phase 0 run 2026-07-15 (CORRECTED after the Task-2 review):** `WRepoint`
(`CaGcRootLocalPartManifestCore.tla:377-394`) does **NOT** directly model the implemented trigger:
`WRepoint` requires `owner[mNew] = None` (destination completely unowned), while the real trigger
always promotes a manifest that is an ALREADY-LIVE PRECOMMIT (`WPromote`'s domain) — the original
note's "destination-unowned verified" claim was false for the actual C++ shape. The correct safety
argument for what Task 2 implemented: the record's op1 (old-committed removal) is exactly
`WDropRef`'s shape and op2 (precommit→committed) is exactly `WPromote`'s shape; the sequential
composition `WDropRef(mOld); WPromote(mNew)` is valid in the verified model (after `WDropRef`,
`RefFreeFor(ref, mNew)` holds, satisfying `WPromote`'s precondition); packaging both ops in ONE
write-once `RefLogTxn` is a sound refinement because `applyRefLogTxn` validates and applies every
op on a scratch copy replaced only on whole-transaction success (no intra-transaction intermediate
state is ever observable — atomicity only removes a race window the model tolerates), and the
per-op validation re-encodes `RefFreeFor` (the removal branch requires exact-match of the current
committed binding, the promote branch re-checks the ref is free). `AtMostOneCommittedManifestPerRef`
therefore holds for the implemented shape. TLC re-confirmation run 2026-07-15 against tree
`a4fdb2f30fd` (recorded in note commit `681997a4991`): `CaGcRootLocalPartManifestCore_live`, no
errors, 16m47s — this verifies the constituent `WDropRef`/`WPromote` actions and the invariant;
it does not (and need not) contain a joint same-key-repoint action. Optional future hardening:
a dedicated `WRepointFromPrecommit` joint action + re-run would pin the composition as a single
model transition; not required given the refinement argument above.

## 8. Testing {#testing}

- **Unit (Core):** `repointRef` add / overwrite / remove / byte-equal no-op; audit event emission;
  old manifest reclaimed by the next GC round (dangling = 0 via the test fsck oracle).
- **Wiring gtests:** standalone `writeFile` of `checksums.txt` on a committed part → repoint, bytes
  readable through the normal read path, fsck clean; unlink-storm followed by directory removal →
  zero repoints; batched `removeSharedFiles` without dir-drop → exactly one repoint; MVCC
  `storeInfo` on CA → single manifest publish, `txn_version.txt.tmp` never appears; `createHardLink`
  carry-forward of all former "mutable" names rides the tree.
- **Integration:** `test_cas_replicated_relink` stays green (no sidecar, no wire field); a
  B182-shape test (merge/mutation inside an explicit transaction); ATTACH of a detached part
  really clears txn state (fresh `VersionMetadata` after attach).
- **Soak assertions:** repoint ProfileEvent == 0 on the non-transactional soak profile; a
  transactional soak run bounds the manifest-churn rate and confirms fold reclaims repointed
  manifests.

## 9. Explicitly out of scope {#out-of-scope}

- Supporting genuinely legacy (pre-`checksums.txt`) parts on CA pools — unreachable by
  construction; the backfill corner case is served by repoint if it ever fires, not by a migration
  path (B13 covers migration via `MOVE PARTITION` re-pack).
- Relaxing `Freshness` semantics for anything other than the deleted mutable-file branches; tree
  read freshness discipline is unchanged.
- Any Keeper-side or `ReplicatedMergeTree`-side change — per the standing upstream-coupling
  minimization principle (see the B1 `manifest_hash` rejection, `BACKLOG.md §obsolete`).
