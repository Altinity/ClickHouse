---
description: 'Revised design for [TXN-ONE-PIPELINE]: every CA operation updates one eager transaction overlay, and the existing disk commit publishes it without a new precommit API.'
sidebar_label: 'CAS one-pipeline disk transaction'
sidebar_position: 62
slug: /superpowers/specs/2026-07-15-cas-txn-one-pipeline-design
title: 'CAS One-Pipeline Disk Transaction Design'
doc_type: 'reference'
---

# CAS One-Pipeline Disk Transaction Design {#cas-txn-one-pipeline-design}

**Status:** revised design for review, 2026-07-15. This document supersedes the staged-intent,
subclass, and two-phase disk-transaction variants previously recorded for
`[TXN-ONE-PIPELINE]`. The A/B/C classification in
`docs/superpowers/cas/upstream-patch-inventory.md` remains the source of truth for the existing
patch inventory, but not for implementation order where it assumes a new disk `precommit` phase.

**Goal:** remove the eager/deferred split from a CA disk transaction. Every operation changes one
transaction-private overlay when it is called. The existing disk `commit` is the only operation
that makes that overlay visible in the durable CAS namespace. No new transaction phase, part
storage method, or early MergeTree call site is added.

**Fork constraint:** the design minimizes the surface shared with upstream ClickHouse. The main
code receives only generic routing and write-buffer extension points needed to remove the current
per-method `isContentAddressed` branches. CA publication and rollback remain inside the CA
metadata transaction.

## Motivation {#motivation}

The `01603` column-TTL failure, the B58 lost-projection manifests, and the B63 silently incorrect
projection aggregates had one cause: a single CA transaction had two execution timelines.
`writeFile`, `createHardLink`, `moveDirectory`, and later part-file unlinks changed CA staging at
call time, while other operations waited in `operations_to_execute` until `commit`. Program order
between the eager and deferred timelines was not preserved.

CA also requires read-your-writes before `commit`. Projection loading, projection read-back,
checksum construction, and manifest construction inspect a part while its parent transaction is
still open. Replaying a deferred queue in `commit`, or in an additional late preparation phase,
cannot make those operations visible to an earlier read.

The required invariant is therefore simpler than a two-phase protocol:

> A CA transaction has one mutable view. Every operation updates that view at call time, and every
> read through the transaction observes it.

Once this invariant holds, operation order is ordinary program order. There is no delayed delete
that can fire after a later create, no special eager-unlink classification, and no need to repair
ordering independently in each disk method.

## Transaction Model {#transaction-model}

A CA transaction has three externally relevant states: `Open`, `Committed`, and `Aborted`.

1. In `Open`, every mutation executes immediately against the transaction overlay. This includes
   writes, creates, hardlinks, deletes, file moves, directory moves, and ref moves. "Immediately"
   means that the transaction view changes at the call site; it does not mean that the shared live
   ref is published at that point.
2. Reads combine the transaction overlay with the committed base state. They observe preceding
   writes, renames, and deletes from the same transaction.
3. `commit` seals the overlay and performs the only durable publication. It constructs manifests,
   runs the internal CAS build protocol, promotes blobs as needed, publishes or moves refs, and
   applies the final overlay state.
4. `undo` or destruction before a successful `commit` discards the overlay and private staging.
   Since no live ref was published early, abort does not compensate an early destination ref.

Blob bytes may be written to a private local or S3 staging backend while the transaction is open.
That is still eager staging: the bytes and their metadata are available to the transaction but
not reachable through a live ref. The existing internal ordering
`precommitAdd` → blob upload/adoption → promote remains part of `commit`; `precommitAdd` is a
CAS GC-safety primitive and is not a public disk-transaction phase.

The implementation must not represent deletes as a second list of actions to replay after the
overlay. A delete changes the overlay immediately, just like a create. For example, create →
delete → create leaves the path present in the final overlay; no delayed delete remains to
produce an ABA failure during `commit`.

## Why There Is No Disk Precommit {#why-there-is-no-disk-precommit}

An earlier version introduced `IDiskTransaction::precommit`,
`IMetadataTransaction::precommit`, and `IDataPartStorage::publishStagedData`. Their purpose was to
publish CA refs after the final part rename but before the external `Keeper` decision and before
the `data_parts` lock. That is not a CA transaction-order requirement.

The current Replicated flow already defines the general order as:

1. `MergeTreeData::Transaction::renameParts`;
2. `Keeper::tryMultiNoThrow`;
3. `MergeTreeData::Transaction::commit`;
4. `IDataPartStorage::commitTransaction` and the underlying disk `commit`.

The hardware-error recovery branch may deliberately delay step 4 while it determines whether the
`Keeper` multi succeeded. The final disk `commit` also currently runs from the general
`MergeTreeData::Transaction::commit` path, including paths that hold `data_parts`.

This ordering applies to every real deferred disk transaction reached through
`IDataPartStorage`; it is not introduced by CA. A CA-only `precommit` would give one metadata
implementation an extra visibility boundary while leaving the general `Keeper`/disk ordering
unchanged. It would therefore not solve the general atomicity question. It would instead add:

- two similarly named preparation concepts: the existing
  `IDataPartStorage::precommitTransaction` writer hook and a new disk publish phase;
- a second part-storage bridge such as `publishStagedData`;
- new calls in shared `MergeTree` paths and special handling for self-owned transactions;
- compensation for refs deliberately published before the existing commit decision.

That is disproportionate for `[TXN-ONE-PIPELINE]` and increases the fork's conflict surface.
Consequently this design leaves the existing ClickHouse commit call sites and transaction API
unchanged. `IDataPartStorage::precommitTransaction` retains its current writer/finalization
meaning and remains a noop for `DataPartStorageOnDiskFull`.

If the ordering between an external `Keeper` decision and a real disk transaction must change,
it needs a separate design covering all affected disk implementations. Likewise, moving remote
I/O out of `data_parts` is a general commit-positioning/performance problem, not a reason to add a
CA-only correctness phase.

The window after a successful `Keeper` multi and before disk `commit` uses the existing
`ReplicatedMergeTree` unknown-result and missing-part recovery semantics. If the process
terminates in that window, the client receives no acknowledgement and must retry according to the
normal idempotent insert contract. On restart, `checkPartsImpl` finds the part in the replica's
`Keeper` part set but not on disk and places it into the replication queue. The part-check thread
then searches other replicas for the part or a covering part and fetches it through the ordinary
mechanism. If no replica can provide it, `onPartIsLostForever` and
`createEmptyPartInsteadOfLost` replace it with an empty part and account it in
`lost_part_count`.

This may lose data when the last copy disappeared before disk `commit`, but that is the explicit
general lost-part behavior, not a CA-specific transaction protocol to repair in this change. A
targeted fault-injection test should document that CA follows this existing path; it is a
regression test, not a separate architectural audit or a gate requiring a new recovery mechanism.

## Dispatch Funnel {#dispatch-funnel}

`DiskObjectStorageTransaction` remains one class. Every mutating method routes its metadata
effect through one generic helper:

```cpp
template <typename Operation>
void dispatch(Operation && operation)
{
    if (metadata_storage->transactionIsStagingOverlay())
        operation(metadata_transaction);
    else
        operations_to_execute.emplace_back(std::forward<Operation>(operation));
}
```

`IMetadataStorage::transactionIsStagingOverlay` returns `false` by default and `true` for CA. For
ordinary object-storage metadata, behavior is unchanged: effects enter
`operations_to_execute` and replay in FIFO order during `commit` or `tryCommit`. For CA, effects
update `ContentAddressedTransaction` immediately and the disk-layer queue remains empty.

`DiskObjectStorageTransaction::commit` and `tryCommit` validate in release builds that an eager
transaction has an empty queue. A future method that bypasses `dispatch` therefore fails before
publication rather than silently recreating two timelines.

The funnel deletes the existing per-method CA branches together: the CA `writeFile` block,
`createHardLink`, the disk-layer `moveDirectory` branch, the eager-unlink predicate and its six
callers, and the `moveFile`/`replaceFile` gaps. `MultipleDisksObjectStorageTransaction` uses the
same policy.

## Write-Buffer Hook {#write-buffer-hook}

`writeFile` differs in mechanism, not execution time: a CA blob key is its content hash and is
known only when the last byte has been written. `writeFileImpl` therefore starts with a generic
transaction hook:

```cpp
if (auto buffer = metadata_transaction->tryCreateWriteBuffer(
        path, buf_size, mode, settings, autocommit))
    return buffer;
```

The default returns `nullptr`, preserving every existing metadata implementation. CA returns its
hash-on-write staging buffer. Append read/modify/write, inline-versus-blob selection, and the
transaction lifetime pin move from `DiskObjectStorageTransaction.cpp` into
`ContentAddressedTransaction`.

An autocommit buffer retains the owning disk transaction and calls its existing `commit` from the
finalize callback. There is no `precommit` call and no separate metadata publication path.

## Overlay Responsibilities {#overlay-responsibilities}

The overlay must cover every operation that can otherwise escape into a different execution
timeline:

- part-file entries and pending blob payloads;
- removal of part files;
- projection-prefix and part-directory re-keying;
- committed-ref moves, drops, and replacements performed through the transaction;
- verbatim table-level and mountpoint mutations reached through the same transaction.

This is a semantic requirement, not necessarily one container. The implementation may keep
specialized maps for file entries, refs, and namespace files as long as every method mutates them
at call time and every transaction read resolves the combined view consistently.

The plan must specify collision behavior for rename and replacement in the overlay. The rule is
the same as the corresponding filesystem operation applied in program order; it must not be
reconstructed later from independently replayed intent lists.

## `moveDirectory` Responsibilities {#move-directory-responsibilities}

For a staged tmp-to-final part move, `ContentAddressedTransaction::moveDirectory` only re-keys the
transaction view from the temporary ref to the final ref. It does not call `publishStaging`.

The same rule applies to committed-ref operations performed through an open CA transaction: the
overlay records the source as absent and the destination as present, and reads observe that
result. The durable ref operation occurs in `commit`.

This removes B151 early publication, `rename_published_refs`, and destructor compensation for an
early destination ref. The B183 temporary text-index case must be represented in the overlay so
that a scratch ref cannot overwrite the authoritative final part manifest; preserving that
invariant is a migration gate.

## Commit And Abort {#commit-and-abort}

`ContentAddressedTransaction::commit` owns the complete transition from overlay to durable state:

1. freeze the overlay against further mutation;
2. derive the final manifest/ref/namespace-file changes;
3. establish CAS GC protection with the internal `Cas::Build::precommitAdd` protocol;
4. upload or adopt referenced pending blobs;
5. promote manifests and apply final ref changes;
6. mark the transaction committed and remove private staging.

There is no atomic backend operation spanning several refs. Existing commit-time compensation
for refs newly created before a later publication failure therefore remains necessary. This is
commit failure handling, not compensation for publication before the commit decision. The plan
must separately audit updates of existing refs because dropping an existing ref is not a valid
rollback for a failed repoint.

Before `commit`, `undo` is simple: discard the overlay, abandon its builds, and remove private
staging. No mutation of the shared live namespace should have occurred. After `commit` succeeds,
the disk transaction is complete and is reset by its existing owner.

## Upstream Surface {#upstream-surface}

The intended shared-code delta is limited to:

- one generic eager-overlay capability used by the dispatch funnel;
- one generic metadata-transaction write-buffer hook;
- routing every mutating `DiskObjectStorageTransaction` method through the funnel;
- release-build verification that eager transactions never populate
  `operations_to_execute`.

The design does not add or change:

- `IDiskTransaction::precommit`;
- `IMetadataTransaction::precommit`;
- `IDataPartStorage::publishStagedData`;
- `IDataPartStorage::precommitTransaction` or any of its call sites;
- `MergeTreeData::Transaction::renameParts` or the Replicated `Keeper` call sequence;
- explicit `precommit`/`commit` pairs in freeze, restore, fetch, or other self-owned paths.

CA-specific overlay and publication behavior stays under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.

## Migration Phases And Gates {#migration-phases-and-gates}

| Phase | Content | Gate |
|---|---|---|
| 1. Complete the overlay | Stage all CA mutations, including deletes and ref moves; make transaction reads resolve the overlay; make staged tmp-to-final `moveDirectory` a pure re-key | CA gtests; read-your-writes and operation-order tests; B183 text-index regression |
| 2. Publish only in commit | Remove B151 early publication and `rename_published_refs`; make `commit` materialize the complete overlay; simplify pre-commit `undo` | Commit-failure compensation tests; regression test that termination after the `Keeper` multi enters the ordinary missing-part recovery path |
| 3. Funnel and write hook | Add the generic dispatch capability and write-buffer hook; route all mutating methods through the funnel; delete per-method CA branches | Build; CA battery; CA-default stateless; targeted `01603_remove_column_ttl` and `02941` |
| 4. Tail de-patch | Re-evaluate remaining B and C inventory items against the new invariant and delete only those made redundant | Build and CA battery |
| Final | Run full CA-default stateless and time-driven phase-3 ca-soak | All green; no CA operation bypasses the overlay or populates the disk queue |

Each phase must be independently reviewable. Existing workarounds are removed only after their
replacement invariant and targeted regression test are present in the same phase.

## Rejected Alternatives {#rejected-alternatives}

- **Deferred delete intents:** they retain a second execution time and permit an ABA sequence in
  which an old delete removes a later create.
- **Replay the disk queue in `commit`:** it preserves write/write order but does not provide
  read-your-writes before `commit`.
- **Replay the disk queue in a new `precommit`:** it has the same early-read problem and adds an
  API without removing the need for an eager transaction view.
- **A CA-specific disk `precommit`:** the motivating `Keeper`/disk ordering is general to real
  disk transactions. A CA-only phase does not solve it and expands the upstream patch surface.
- **Reuse `IDataPartStorage::precommitTransaction`:** it is a writer/finalization hook with many
  call sites before the final rename. Changing its meaning would be both confusing and
  conflict-prone.
- **Add `IDataPartStorage::publishStagedData`:** it is another public name for an early CA-only
  visibility boundary that this design does not require.
- **Publish from `moveDirectory`:** it gives an ordinary filesystem operation a hidden transaction
  phase, relies on path classification to recognize the final rename, and requires abort
  compensation for work performed before `commit`.
- **A CA disk-transaction subclass or split base:** the semantic difference is the dispatch
  policy already expressed by the generic funnel. A new hierarchy would enlarge a frequently
  conflicting upstream file.

## Plan-Time Audits {#plan-time-audits}

- Inventory every mutation currently applied directly to a live ref, namespace file, or
  mountpoint and assign it an overlay representation.
- Verify transaction reads after write, delete, rename, replace, projection re-key, and committed
  ref move.
- Verify B183 temporary text-index behavior without early publication.
- Verify with one fault-injection regression that process termination after successful `Keeper`
  publication but before disk `commit` enters the existing missing-part fetch/lost-part path; do
  not introduce CA-specific recovery for this window.
- Audit multi-ref commit failure and existing-ref repoint behavior; do not use unconditional ref
  deletion as rollback.
- Audit `writeFileUsingBlobWritingFunction` and `copyFile`. Preserve current rejection where
  support is absent; do not introduce a fallback behavior.
- Re-evaluate the empty-covering-part `commitTransaction` workaround before deleting it; the new
  design does not assume that moving publication into `commit` automatically makes the workaround
  redundant.

## Backlog Observation {#backlog-observation}

Plain object storage can leave carried-forward projections absent from an in-memory mutated part
until reload because queued hardlinks are invisible to `loadProjections`. This is a candidate
upstream issue, but fixing ordinary object-storage read-your-writes is outside this design.
