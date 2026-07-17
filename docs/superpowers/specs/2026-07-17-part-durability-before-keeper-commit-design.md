---
description: 'Fix for the acked-then-lost INSERT data loss: close every part disk-storage transaction in MergeTreeData::Transaction::renameParts, restoring the invariant that a part is durable before its block_id/part-znode is registered in Keeper.'
sidebar_label: 'Part durability before Keeper commit'
sidebar_position: 63
slug: /superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design
title: 'Part Durability Before Keeper Commit'
doc_type: 'reference'
---

# Part Durability Before Keeper Commit {#part-durability-before-keeper-commit}

## Problem {#problem}

A replicated `INSERT` can be acknowledged (HTTP 200) while its rows never become readable —
silent acked data loss. Reproduced deterministically (`build/dl_probe.py`: S3 outage past the CAS
write budget + replica kill during continuous sync inserts → ~15% acked-but-absent). Full trace:
`docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md`.

The violated invariant, stated once:

> **A part must be durable on its disk before its `block_id`/part-znode is registered in
> `Keeper`. The parts-transaction commit only flips visibility; the parts-transaction rollback
> compensates with new operations over committed disk state.**

`ReplicatedMergeTreeSink::commitPart` orders the commit as: `transaction.renameParts` (:976) →
`Keeper` multi with `block_id` + part-znode (:985) → `transaction.commit` (:990), and the
disk-storage transaction of each part is committed only inside `transaction.commit`
(`MergeTreeData.cpp:8797-8799`) — **after** the `Keeper` decision, **under the `data_parts`
lock**. Whether that violates the invariant depends on what the disk defers:

| Disk | Durable at `renameParts` | Deferred into the post-`Keeper` disk commit | Window |
|---|---|---|---|
| local | everything (`FakeDiskTransaction` executes ops immediately; `commit`/`undo` are no-ops) | nothing | none |
| plain S3 (upstream) | blob data (streamed inline during write) | all local metadata ops, including the tmp→final rename (`PureMetadataObjectStorageOperation`) | process crash |
| CA (после `39cf3279652`) | nothing (`moveDirectory` tmp→final is a pure overlay re-key; blob uploads are B188-deferred) | blob uploads + manifest + ref publish — the entire durability | **any S3 exception** |

When the window is entered, `Keeper` durably holds the `block_id` + part-znode while no replica
holds the part data (the local `MergeTreeData::Transaction` rolls back): a phantom part. The
`block_id` dedup znode has an independent lifetime (`replicated_deduplication_window`), so a
byte-identical client retry hits cross-replica dedup — `"already exists on other replicas as part
…; ignoring it"` (`ReplicatedMergeTreeSink.cpp:511-517`) — and is acked without inserting.
`createEmptyPartInsteadOfLost` preserves the loss. `fsck` stays clean: nothing dangles at the CA
layer.

Origin: the CA window is a 2026-07-16 regression — `39cf3279652` ([TXN-ONE-PIPELINE] Task 1.1)
removed the B151 publish-at-rename; R3 (#37) made the failure silent by turning the ambiguous
abort into a client-visible retry-later. The plain-S3 crash window exists **upstream** (verified
at merge-base `83b3f837cc8`) and was reasoned away in the TXN-ONE-PIPELINE spec §"window after a
successful Keeper multi" under two assumptions the reproduction proved false for CA: the window is
entered only by termination, and the client gets no acknowledgement.

## The Change {#the-change}

One generic edit. `MergeTreeData::Transaction::renameParts` closes every part's disk-storage
transaction after performing the deferred renames:

```cpp
void MergeTreeData::Transaction::renameParts()
{
    /// Materialize every part of this transaction: perform the deferred tmp->final renames, then
    /// close each part's disk-storage transaction, making the parts DURABLE on their disks.
    ///
    /// Contract: after renameParts() returns, every part of this transaction is durable at its
    /// final name. commit() only flips in-memory visibility (its commitTransaction loop remains as
    /// a safety net for paths that do not come through here); rollback() compensates with new
    /// operations over committed disk state (removing a rolled-back part reclaims its disk data;
    /// on a content-addressed disk that drops the published ref).
    ///
    /// Ordering is load-bearing: every call site invokes renameParts() BEFORE its external Keeper
    /// commit decision. A part must be durable before its block_id/part-znode is registered,
    /// otherwise a fault between the Keeper commit and the disk commit leaves a phantom part whose
    /// surviving block_id silently dedups a byte-identical client retry (acked data loss; see
    /// docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md). This also keeps the
    /// disk commit (network I/O on object storages) off the data_parts lock, which
    /// Transaction::commit holds.
    for (const auto & part_need_rename : precommitted_parts_need_rename)
    {
        LOG_TEST(data.log, "Renaming part to {}", part_need_rename->name);
        part_need_rename->renameTo(part_need_rename->name, true);
    }
    precommitted_parts_need_rename.clear();

    for (const auto & part : precommitted_parts)
        if (part->getDataPartStorage().hasActiveTransaction())
            part->getDataPartStorage().commitTransaction();
}
```

The declaration comment in `MergeTreeData.h` (:365-367) is extended with the same contract.

No CA-specific code changes. `ContentAddressedTransaction::commit` (upload + promote, fail-closed)
is simply invoked earlier, from `renameParts` instead of `Transaction::commit`. Task 1.1's
`moveDirectory` pure re-key stays; B151's `rename_published_refs` compensation machinery stays
deleted — compensation now rides the ordinary part-removal path (see
[Rollback](#rollback-semantics)).

### Why this is safe against double commit {#double-commit}

`commitTransaction` resets the storage's transaction pointer
(`DataPartStorageOnDiskFull.cpp:374-375`); the existing loop in `Transaction::commit`
(`MergeTreeData.cpp:8797-8799`) is guarded by `hasActiveTransaction` (== `transaction !=
nullptr`), so it degrades to a no-op for parts closed here. The guard is required anyway because
`commitTransaction` on a closed transaction throws `LOGICAL_ERROR` — the code is already
designed for early closure (fetch uses it today: `DataPartsExchange.cpp:1013/:1022`). Repeated
`renameParts` is idempotent (`precommitted_parts_need_rename` cleared; guard false). Borrowed
projection sub-parts ride the parent's transaction (`has_shared_transaction` no-ops), and parents
are what `precommitted_parts` holds.

## Semantics per commitPart branch {#sink-branches}

For `ReplicatedMergeTreeSink::commitPart` (the loss path):

| Branch | Before | After |
|---|---|---|
| disk/publish failure | throws from `transaction.commit` (:990) **after** the multi registered the `block_id` → phantom → silent loss on retry | throws from `renameParts` (:976) **before** the multi → no znodes → the client error/retry re-inserts honestly |
| crash between :985 and :990 | phantom part (no data anywhere) | part data durable at final name (CA: ref in the pool; restart sees the part) — strictly better than upstream plain-S3 |
| `ZOK` → `transaction.commit` (:990) | disk commit (network I/O for CA) under the `data_parts` lock, can throw after the verdict | pure in-memory visibility flip; disk loop no-op |
| dedup race `ZNODEEXISTS` (:1067) | rollback is free | `rollbackPartsToTemporaryState` + `renameTo(tmp)` over committed state (CA: committed-source move; tmp part then reclaimed by cleanup). Costs one wasted upload — bounded, GC-reclaimable, accepted |
| hardware-UNKNOWN (:997) | `transaction.commit` at :1009 runs the publish with the multi status unknown | data already durable; the branch degenerates to keep-or-remove over durable state — local-disk semantics |
| any rollback (:1093 / destructor) | destructor backstop abandons unpublished builds | same, plus published parts are reclaimed by the ordinary removal path |

R3 (#37) is unchanged: its `NETWORK_ERROR` "retry-later" now fires from :976, before any `Keeper`
state exists, so the byte-identical client retry it drives is safe. This restores R3's
ship-readiness, contingent on the validation gates below.

## Rollback semantics (compensation as new operations) {#rollback-semantics}

By `renameParts` time every part is already `PreActive` in `data_parts_indexes`
(`preparePartForCommit`, :5360 — both rename modes). Any later failure unwinds into
`Transaction::rollback` → "Undoing transaction … Removing parts" → parts become Outdated with
`remove_time=0` → cleanup deletes the on-disk data → on CA, deleting a published part drops its
ref. This exact chain was observed live in the reproduction (part `all_1117_1117_0`). A partial
`renameParts` failure (parts 1..k published, k+1 threw) is covered the same way: rollback removes
all parts; published ones are reclaimed via removal, unpublished ones via tmp-cleanup plus the
destructor's `abandon`. No path reaches the `Keeper` multi, so no znode ever outlives the data.

Cosmetic note: between a partial publish and its cleanup, a restart may see a published final ref
as an "unexpected part". This is the same class as today's crash-after-`renameParts` on a local
disk; no new hazard.

## Call-site audit {#call-site-audit}

All `renameParts` call sites, verified: every one runs before its external commit decision (family
A) or immediately before `commit` with no decision in between (family B).

| Site | Family | What follows `renameParts` |
|---|---|---|
| `ReplicatedMergeTreeSink::commitPart` :976 | A | `Keeper` multi with **dedup `block_id`** — the loss path |
| `MergeTreeDataMergerMutator::renameMergedTemporaryPart` :528 | A | caller's `checkPartChecksumsAndCommit` |
| `MutateFromLogEntryTask::finalize` :283 | A | `checkPartChecksumsAndCommit` |
| `StorageReplicatedMergeTree::executeLogEntry` :2567 (ATTACH helper) | A | `checkPartChecksumsAndCommit` |
| `StorageReplicatedMergeTree::executeReplaceRange` :3411 | A | `zookeeper->multi(ops)` |
| `StorageReplicatedMergeTree::fetchPart` :5646 | A | `checkPartChecksumsAndCommit`; the fetched part's disk txn is already closed at download (`DataPartsExchange`:1013/:1022) — the change is a no-op here |
| `StorageReplicatedMergeTree::replacePartitionFromImpl` :9224 | A | `tryMulti` |
| `StorageReplicatedMergeTree::createEmptyPartInsteadOfLost` :11217 | A | ops/multi below |
| `StorageMergeTree::renameAndCommitEmptyParts` :2290 | B | `commit` next line |
| `StorageMergeTree::movePartitionToTable` :2862/:2865 | B | `commit` next line |

Throw behavior at each site (new: CA publish errors can now surface here) lands in the existing
catch/destructor → rollback path described above; merges/mutations get their normal
postpone/backoff, queue entries retry, plain-engine callers surface a clean user error.

Paths that bypass `renameParts` (`rename_in_transaction=false`) were audited and none violates the
invariant: plain-engine INSERT/ATTACH/REPLACE commit immediately after the add (no external
decision in between; on an object disk the queued rename executes inside `commit(lock)`, which is
what the `MergeTreeSink` FIXME race requires); `movePartitionToTable`'s dest parts (:9503) come
from `cloneAndLoadDataPart`, which owns and commits its disk transaction before returning, so they
are durable before the dest multi (:9508); the empty-covering part (:5725) keeps its hand-placed
CA `commitTransaction` (its rollback-by-design path never reaches `commit`).

`rename_in_transaction` itself is untouched: it is the lock-scope switch for materialization
(deferred off-lock rename, born in `6c495863667`), and `renameParts` is its designed off-lock
materialization point — this change completes that point (rename + durability) rather than
altering the parameter's meaning.

## Performance {#performance}

Today the disk commit — for CA, S3 uploads and the ref publish — executes inside
`Transaction::commit` **while holding the `data_parts` lock**, stalling every concurrent part
lookup. This change moves it to `renameParts`, which every caller invokes off-lock (that is the
documented reason `renameParts` exists). Total I/O volume is unchanged; the multi-part publish
loop is sequential in both positions. The only added cost is re-doing work when the external
decision says no (one wasted upload per lost dedup race / failed multi) — accepted against silent
data loss.

## Out of scope {#out-of-scope}

- **`block_id` outliving a durably-committed part that is lost later** (e.g. storage failure after
  a successful commit): a pre-existing, upstream-accepted, much narrower hazard. A verify-on-dedup
  (honor the `block_id` only if the referenced part is recoverable) would close it; rejected here
  as the primary fix because it adds a read/existence probe to every dedup hit and does not close
  the merge-path windows. Recorded in `docs/superpowers/cas/BACKLOG.md`.
- **Plain-engine (`MergeTreeSink`) publish-under-lock on CA**: correctness is unaffected (no
  external coordinator); the perf note is recorded in the backlog.
- **The `MergeTreeSink` FIXME race** (plain-engine deferred rename vs merge): untouched.

## Testing {#testing}

1. **Generic failpoint regression test** (works on plain S3 — the upstream shape — no CA needed):
   replicated table on an object-storage policy, a failpoint failing the part disk-transaction
   close, sync `INSERT` with `insert_deduplicate=1` (fails), disable the failpoint, re-issue the
   byte-identical `INSERT`, `SELECT count()`. Before the fix: the close fires after the multi →
   the retry falsely dedups → 0 rows. After: it fires in `renameParts`, before the multi → the
   retry inserts → 1 row. Implementation note (found empirically): the pre-existing
   `disk_object_storage_fail_commit_metadata_transaction` cannot be used — it also fires on the
   autocommit one-shot transactions wrapping ordinary disk ops (first hit: temp-part
   `createDirectories`), killing the insert before any `Keeper` state exists. The test adds a
   targeted `part_storage_fail_commit_transaction` failpoint inside
   `DataPartStorageOnDiskFull::commitTransaction` — by construction the exact operation the fix
   repositions.
2. **CA integration scenario S40** built from the dl_probe reproducer (tracked at
   `utils/ca-soak/tools/dl_probe.py` by the plan; `build/` is git-ignored): rustfs pause past the
   write budget + replica kill under continuous sync inserts. Gating verdicts: fault schedule
   executed, outage disturbed inserts, meaningful acked volume, and acked == present (the
   data-loss gate); the cross-replica dedup-line count is a non-gating observation, because a
   client-timeout-then-commit makes some dedups legitimate. Plus fsck clean at quiescence.
3. **Existing gates**: `Ca*` gtest battery unchanged (CA-layer publish semantics did not move —
   only the caller of `commitTransaction` did); S39 (#37 fence tolerance) green; S36/S37 green;
   20-minute soak with the checkpoint row-count oracle.
4. **Plan-time audit step**: grep-verify no path writes through a part's disk transaction between
   `renameParts` and `Transaction::commit` (post-close writes must go the autocommit route, which
   CA supports as committed-ref writes).

## Upstream submission {#upstream-submission}

The plain-S3 crash window is an upstream durability defect with the same silent-loss consequence
(`insert_deduplicate` + client retry). Plan: file an upstream issue with the failpoint
reproduction from test (1), then a PR carrying this same one-function change (plus the header
contract comment). Keeping the fork edit byte-identical to the upstream candidate minimizes the
conflict surface. A rename of `renameParts` to something role-accurate (e.g. `prepareForCommit`)
is proposed upstream-first, not in the fork.

## Alternatives rejected {#alternatives-rejected}

- **CA-only publish-at-rename (B151 restore)**: closes only CA, leaves the upstream plain-S3
  window, reintroduces a CA special case inside `moveDirectory` plus destructor compensation. The
  bug is generic; the fix should be too.
- **Verify-on-dedup as the primary fix**: read-side cost on every dedup hit, does not close the
  merge/mutation windows, and keeps the ordering inversion in place.
- **A new `IDiskTransaction::precommit` phase**: rejected by the TXN-ONE-PIPELINE design for API
  surface reasons; unnecessary — `renameParts` + `commitTransaction` already express the needed
  phase with zero new API.
