---
description: 'Design spec for [TXN-ONE-PIPELINE]: the CA disk transaction becomes one pipeline — every operation executes at call time in its domain-appropriate place, a new two-phase precommit/commit contract owns the publish, and the accumulated per-method isContentAddressed branches in upstream code are deleted.'
sidebar_label: 'CAS one-pipeline disk transaction'
sidebar_position: 62
slug: /superpowers/specs/2026-07-15-cas-txn-one-pipeline-design
title: 'CAS One-Pipeline Disk Transaction Design'
doc_type: 'reference'
---

# CAS One-Pipeline Disk Transaction Design {#cas-txn-one-pipeline-design}

**Status:** approved design, 2026-07-15. Supersedes the staged-intents wording of the
`[TXN-ONE-PIPELINE]` entry in `docs/superpowers/cas/BACKLOG.md` §4.

**Goal:** eliminate the two-pipeline dispatch split (eager vs deferred-to-commit) in the CA disk
transaction as a class, not per symptom. Every operation executes at call time; a standard
two-phase `precommit`/`commit` disk-transaction contract owns the publish; all twelve class-A
`isContentAddressed()` patches in upstream code (inventory:
`docs/superpowers/cas/upstream-patch-inventory.md`) are deleted together with the mechanism that
made them necessary. The result is a net deletion: fewer than a hundred added lines, all generic,
none naming CA.

**Sequencing (decision):** lands FIRST, before codecs v3
(`2026-07-15-cas-codecs-v3-design.md`) and the source-layout refactoring
(`2026-07-15-cas-source-layout-refactoring-design.md`). It is a behavior change and is validated
against the tree the soak history knows; it also shrinks the upstream patch surface before the
layout refactoring demands its quiet zero-behavior-change window.

## Motivation {#motivation}

The `01603` column-TTL abort (`de8a38b1e87`), the B58 lost-projection manifests, and the B63
silently-wrong projection aggregates were all one bug class: within a single CA disk transaction,
some operations executed eagerly (`writeFile`, `createHardLink`, `moveDirectory`, later part-file
unlinks) while others deferred to commit replay, and order between the two pipelines inverted.
Each per-method `isContentAddressed()` branch in `DiskObjectStorageTransaction.cpp` was added
through one of those bugs.

Why CA cannot live like plain s3 (everything deferred, replayed at commit) — verified against the
code, load-bearing for this design:

- Plain s3 survives on two escape hatches. First, **early sub-commits**: a rebuilt projection's
  sub-transaction commits early (`MutateTask.cpp:1819`), making its files readable before the
  parent commits. Second, **fail-open readers**: `loadProjections` checks `existsDirectory` and
  silently skips what it cannot see (`IMergeTreeDataPart.cpp:1384`).
- Whole-part atomicity closes both hatches on CA. An early sub-commit publishes half a part (the
  B21/B36 corruption class), so projection sub-parts must ride the parent transaction — and then
  pre-commit readers (`loadProjections`, the temp-projection read-back) can only be served by the
  transaction's own staging. Reader blindness on CA is not a perf loss but a correctness loss:
  the manifest and `checksums.txt` are built from what the transaction sees (B58), and the B63
  workaround that registered projections while blind produced silently wrong aggregates.
- Therefore the transaction's staging must be up to date at the moment of every call — operations
  must execute when invoked. Once anything is eager, a coexisting deferred queue recreates the
  inversion class, so the queue must not exist for CA at all.

Verified along the way: upstream plain s3 still has the reader-blindness wart today — a mutation
always wraps the new part storage in a disk transaction (`MutateTask.cpp:3153`), carried-forward
projection hardlinks are queued (`DataPartStorageOnDiskFull.cpp:307`), and `loadProjections` at
finalize silently skips them, so the mutated part lives in memory without those projections until
reload, and downstream consumers of the in-memory map (`MergedBlockOutputStream.cpp:222`,
`MergeTask.cpp:1180`, `DataPartsExchange.cpp:235`) act on the blind state. Recorded as a backlog
observation (candidate upstream issue), out of scope here.

## The Model {#model}

One rule, no domains crossing in time:

1. **Every operation executes at call time**, in its domain-appropriate place:
   - part-file operations (write, hardlink, rename, unlink) apply to the transaction's
     **staging overlay** — the transaction-private in-memory state backed by scratch bytes;
   - verbatim table-level and mountpoint operations (including deletes) apply to the
     **real location** immediately.
2. **`precommit` lifts staging into the real place**: for each staged part — manifest from the
   overlay → `precommitAdd` → upload missing blobs → promote → publish the ref. After a
   successful `precommit` the transaction is **sealed**: any further mutating operation throws
   `LOGICAL_ERROR` (a caller writing past the published manifest is a correctness violation).
3. **`commit` finalizes bookkeeping only**: marks the transaction committed (disarming the abort
   compensation) and performs residual cleanup (B188 staged-temp removal). It has no durable
   role left.
4. **Abort** (undo / destructor of an uncommitted transaction) discards staging and compensates
   the only thing `precommit` lifted — the refs it published (the existing `dropRefBestEffort`
   mechanism, now tied to an explicit phase). Already-executed immediate destructive operations
   are **not** rolled back.

Point 4 adopts the local-disk semantics MergeTree was written against: `FakeDiskTransaction`
executes everything immediately, deletes included, with no rollback. The deferred durable delete
was a guarantee the object-storage transaction added on top of MergeTree's base model; CA gives
it up for uniformity. Deferred deletes (intents) were rejected because they reintroduce a second
execution time and with it an ABA hazard: create (immediate) → delete (deferred) → create
(immediate) → the deferred delete fires at commit and destroys the recreated file. With
everything immediate, program order is total by construction and no ordering rules are needed.

## Mechanism {#mechanism}

### The dispatch funnel {#dispatch-funnel}

`DiskObjectStorageTransaction` keeps its single class (no subclass, no base split — decided).
All mutating methods route through one funnel:

```cpp
void dispatch(std::function<void(MetadataTransactionPtr)> op)
{
    if (metadata_storage->transactionIsStagingOverlay())
        op(metadata_transaction);
    else
        operations_to_execute.push_back(std::move(op));
}
```

`IMetadataStorage::transactionIsStagingOverlay` is a generic capability ("my transaction is a
staging overlay; apply operations to it in program order"), `false` by default, `true` for CA.
Every mutating method body becomes the same one-liner for all disks. The eager-unlink gate
(`isEagerContentAddressedUnlink`, `stagesPartFileUnlink` and its gtest) is deleted: with
immediate verbatim deletes there is nothing left to classify. The `moveFile`/`replaceFile`
residual gap (part-file→part-file shape still deferred) closes automatically.
`MultipleDisksObjectStorageTransaction` inherits the funnel unchanged. A cheap
`chassert(operations_to_execute.empty())` in the eager path of `commit` serves as a tripwire
against future code bypassing the funnel; `undo` finds an empty queue on CA and CA abort is
handled wholly by the metadata transaction.

### The write-buffer hook {#write-buffer-hook}

`writeFile` is the one operation whose *mechanism* differs, not its moment (the blob key is the
content hash, known only after the last byte; the buffer is the operation). Generic extension
point at the top of `writeFileImpl`:

```cpp
if (auto buf = metadata_transaction->tryCreateWriteBuffer(path, buf_size, mode, settings, autocommit))
    return buf;
```

Default returns `nullptr` (every existing metadata type unchanged); `ContentAddressedTransaction`
returns its hash-on-write buffer. The entire ~85-line CA block in `writeFileImpl` (append RMW,
autocommit-inline vs content-blob split, the keep-alive pin) moves into the CA tree. Exact hook
signature (and how the autocommit finalize callback obtains the disk transaction for the
`precommit`+`commit` pair) is a plan-level decision.

### The two-phase contract {#two-phase-contract}

- `IDiskTransaction::precommit` — new virtual, noop default. Ordinary disks unchanged.
- `DiskObjectStorageTransaction::precommit` forwards to `IMetadataTransaction::precommit`
  (noop default there as well).
- `ContentAddressedTransaction::precommit` = the entire publish (today's `publishStaging` loop
  moves here from `commit`), then seal.
- `ContentAddressedTransaction::commit`: if `precommit` has not run, runs it implicitly
  (idempotent, flag-guarded — classic commit-implies-prepare) with observability:
  ProfileEvent `CasImplicitPrecommitInCommit` + a debug log line, so a mis-positioned hot path
  (publish under the `data_parts` lock) surfaces in soak metrics instead of aborting rare
  recovery branches. Then bookkeeping as in [The Model](#model). `tryCommit` follows the same
  semantics.
- Mutating operation after `precommit` → fail-loud `LOGICAL_ERROR` (correctness invariant, the
  settled asymmetry: missed positioning is counted and logged; writing past the seal throws).
- **Autocommit one-shots** call `precommit` then `commit` explicitly from the finalize callback:
  they are commit-positioned by design and must not pollute the implicit-precommit metric, whose
  purpose is to catch mis-positioned hot paths.
- `moveDirectory` becomes a **pure staging re-key** (tmp prefix → final name in the overlay).
  B151's rename-window publish and the whole `rename_published_refs` machinery are deleted in
  the same phase — the publish cannot live in two places.

### Precommit call sites {#call-sites}

- **Replicated:** at the end of `MergeTreeData::Transaction::renameParts` — `precommit` each
  renamed part's storage via a new `IDataPartStorage::precommitTransaction` (mirror of
  `commitTransaction`). This positions the publish before the ZK multi for every Replicated path
  without touching `ReplicatedMergeTreeSink`.
- **Plain MergeTree:** in `MergeTreeData::Transaction::commit`, before acquiring the
  `data_parts` lock. Call paths that enter with the lock already held rely on implicit
  precommit; the metric shows in soak whether any of them is hot enough to deserve an explicit
  call.
- **Self-owned transactions** (`DataPartStorageOnDiskBase::freeze`,
  `MergeTreeData::restorePartFromBackup`, the byte-fetch landing path in `DataPartsExchange`):
  explicit `precommit`+`commit` pair (inventory step 6, in scope).
- **B58 projection sites** are not touched: a projection sub-part rides the parent whole-part
  transaction; the shared-transaction rule is expressed once at `getProjectionPartBuilder`
  (inventory step 6's "express the rule once").
- `MergeTreeData::removePartsInRangeFromWorkingSet`: the hand-placed `commitTransaction` for the
  empty covering part becomes unnecessary once `precommit` publishes before the in-memory
  rollback; the plan must verify `precommit` actually fires on this path before deleting the
  workaround (inventory flags this medium-confidence).

## De-Patching Scope {#de-patching-scope}

Everything class **A** from `docs/superpowers/cas/upstream-patch-inventory.md` (12 hunks), plus
the flagged-B shrinks of inventory step 6 (`freeze`/`restore` to the two-phase pair, the
shared-transaction rule expressed once), plus the two class-C deletions (the redundant
`StorageReplicatedMergeTree::checkAlterPartitionIsPossible` override, `.cpp` + `.h`). Extracting
the B37/B90/`LocalObjectStorage` robustness fixes as standalone upstream contributions stays out
of scope (backlog).

## Migration Phases And Gates {#migration-phases}

| Phase | Content | Gate |
|---|---|---|
| 1. Contract + publish repositioning | `precommit` at all three levels; CA `precommit` = publish, `commit` = bookkeeping; seal; implicit-precommit + ProfileEvent; autocommit pairs; explicit call sites (`renameParts`, plain `commit`, `freeze`, `restore`, fetch); `moveDirectory` demoted to re-key; B151 + `rename_published_refs` deleted | build + full cas-gtest + CA-default stateless + failpoint tests for abort between `precommit` and `commit` |
| 2. The funnel | `dispatch` + capability + `tryCreateWriteBuffer` hook; delete all per-method CA branches (`writeFile` block, `createHardLink`, `moveDirectory`, unlink gate ×6 + predicate + gtest, `moveFile`/`replaceFile` comment tombstones) | build + battery + CA-default stateless (01603 among them) |
| 3. Tail de-patch | `removePartsInRangeFromWorkingSet` workaround removal (after path verification), C deletions | build + battery |
| Final gate | full CA-default stateless run + ca-soak (time-driven phase-3 profile); targeted: `01603_remove_column_ttl`, `02941` (carried-forward projections) | both green |

Phase-1 plan work includes the **destructive-op audit**: enumerate every verbatim / mountpoint /
ref-drop delete site reachable on CA and verify none sits in an abortable multi-op transaction
that expects rollback (the estimate is none — mutation-entry removal and part cleanup happen
after their decisions are already durable — but the estimate is not a proof).

## Rejected Alternatives {#rejected-alternatives}

- **Staged delete intents materialized at `commit`** (the earlier backlog wording) — rejected:
  intents are a second execution time, reintroducing the inversion class in a corner and
  requiring an ABA-closing per-path rule (create → delete-intent → create → the intent fires at
  commit and destroys the recreated file). Everything-immediate makes program order total by
  construction.
- **Keep the queue, replay at `precommit`** — fixes write-write order and manifest completeness,
  but not write-read order: pre-precommit readers exist by necessity (whole-part atomicity bans
  the early sub-commits plain s3 uses), and a queued future is invisible to them. See
  [Motivation](#motivation).
- **Publish at disk `commit`** — an announced-but-unreadable window in the Replicated recovery
  branch (disk `commit` can run minutes after the ZK multi) plus S3 round-trips under the
  `data_parts` lock. Note the earlier "publish must precede the ZK announce" rationale was
  overstated — `PreActive` fences owner reads and a fetch of a `PreActive` part already
  fails-and-retries on plain s3 (an accepted upstream race) — but the recovery-branch window and
  the lock positioning stand.
- **`ContentAddressedDiskTransaction` subclass / split base** — rejected in favor of the funnel:
  a subclass whose every override would be the same one-liner adds a type for no semantic
  difference; a base split buys a compile-time guarantee at the cost of a permanent conflict tax
  on the most-conflicted upstream file. The funnel expresses the decision once, generically.

## Deferred To Plans {#deferred-to-plans}

- Exact names/signatures: `transactionIsStagingOverlay`, `tryCreateWriteBuffer`,
  `precommitTransaction` (naming may be adjusted at draft time).
- The autocommit finalize-callback plumbing for the explicit `precommit`+`commit` pair.
- `writeFileUsingBlobWritingFunction` and `copyFile` behavior on CA under the funnel (today's
  behavior preserved; route through the hook or keep rejecting — audit at draft time).
- The destructive-op audit inventory (phase 1).
- `removePartsInRangeFromWorkingSet` path verification (phase 3).
- Whether any lock-held plain-commit call site needs an explicit `precommit` (driven by the
  `CasImplicitPrecommitInCommit` soak numbers).

## Backlog Observations Recorded Here {#backlog-observations}

- Upstream plain s3 loses carried-forward projections from the in-memory part after a mutation
  until part reload (verified chain in [Motivation](#motivation)); candidate for an upstream
  issue with `MergeTask.cpp:1180` / `DataPartsExchange.cpp:235` as the consumer evidence.
