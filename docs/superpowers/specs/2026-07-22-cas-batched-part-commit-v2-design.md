# CAS batched part-commit v2 — ownership-safe seam, count-based ledger groups

- **Date:** 2026-07-22
- **Status:** design approved (second design; v1 `2026-07-22-cas-multicommit-phased-design.md` is
  SUPERSEDED after a Codex/gpt-5.6-sol xhigh adversarial review returned `BLOCKING FLAW` — 4
  blocking + 5 important findings, all adjudicated as real; this design resolves each one, see
  `tmp/multicommit-spec-review-codex-full.log` for the full review)
- **Area:** `src/Storages/MergeTree/MergeTreeData.cpp` (`Transaction::renameParts`),
  `src/Storages/MergeTree/IDataPartStorage.h` / `DataPartStorageOnDiskFull` (one new method),
  `src/Disks/IDisk.h` / `DiskObjectStorage` (one new method),
  `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (phased engine, ledger
  submission groups, count-based budget, `payload` removal, upload primitive, recovery streaming)
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"OPTIMIZATION OPPORTUNITY (write-path
  latency, HIGH) — CAS-on-S3 INSERT ~7.6× slower than standard S3"* (logged 2026-07-21).

## Problem {#problem}

A CAS-on-S3 INSERT of 10M wide rows into 500 partitions measured **170.6 s** versus **22.4 s** on a
standard S3 `ReplicatedMergeTree` — 7.6× slower. The staging `query_log` profile:

- `CasRefBatchFlushes` = `CasRefBatchedMutations` = 1026 → **ref-ledger batch size exactly 1.0**:
  513 precommits + 513 promotes each paid a serial ref-log `PUT` (~46 ms each);
  `CasRefQueueWaitMicroseconds` = 36.2 s.
- Average write concurrency ~4.7 threads (standard disk ~12× via `getThreadPoolWriter`).
- Per-part blob uploads run serially inside `ContentAddressedTransaction::uploadPendingBlobs`.

The granularity investigation (`tmp/cas-commit-granularity.md`) established that a plain INSERT
performs one `ContentAddressedTransaction::commit()` **per part** (`parts.size() == 1` always) —
`MergeTreeData::Transaction::renameParts` loops per-part `commitTransaction()`. Parallelism inside
`commit()` (the reverted first attempt, Tasks 4-5 of the parallel-write-path plan) can never help
INSERT. The fix must batch **across** the per-part disk transactions at the `renameParts` seam.

## Scope {#scope}

**In scope:** the `renameParts` topology — N independent single-part disk transactions committed in
one place. This is the production topology for INSERT and for every background operation that lands
parts through `MergeTreeData::Transaction`.

**Out of scope (recorded as a backlog known-issue, commit `cc9a8e63401`):** bulk partition
operations. `REPLACE PARTITION` / `MOVE PARTITION TO TABLE` pass `ClonePartParams{.txn = ...}` (a
MergeTree transaction, not a shared disk transaction — `StorageMergeTree.cpp:2785`, `:2967`); each
clone commits its own disk transaction inside `freeze`/`cloneAndLoadDataPart` before `renameParts`
runs, so they never present a batchable set at this seam. The `external_transaction` multi-part
path (`IDataPartStorage.h:263`, used by detached-part cloning) keeps today's sequential
`ContentAddressedTransaction::commit()` behavior unchanged.

**Non-goal (future):** folding `published_at_ms` into the `OwnerTransition` op to drop one op per
promote (halves promote op count); not part of this effort.

## Design {#design}

### 1. The seam: `takeTransactionForBatchCommit` + `IDisk::commitTransactions` {#seam}

Codex finding 1 (Blocking) killed the v1 raw-span seam: `commitTransaction()` is an ownership
lifecycle, not a bare commit — it no-ops for a projection riding the parent's shared transaction,
fires the `part_storage_fail_commit_transaction` failpoint (the `05014` regression gate for the
part-durable-before-Keeper invariant), and resets the owner pointer so the safety-net loop in
`Transaction::commit` (`MergeTreeData.cpp:~9008`) does not commit a second time. A raw batch of
`DiskTransactionPtr` can do none of that.

The v2 seam moves ownership **out** instead, exploiting the real atomicity contract: a failed
`renameParts` aborts the whole operation before the Keeper barrier, and disk garbage from
uncommitted parts is reclaimed by the standard uncommitted-part cleanup (a part present on disk but
never registered in Keeper is removed on restart/cleanup). Nobody needs a failed transaction handed
back to its owner.

```cpp
/// IDataPartStorage — the ONLY addition on the parts side:
/// Move the owned disk transaction out for a batched commit. Returns nullptr when this part
/// storage does not own one (projection riding the parent's shared transaction, or no active
/// transaction). Fires the per-part commit failpoint. After this call hasActiveTransaction()
/// is false.
virtual DiskTransactionPtr takeTransactionForBatchCommit();

DiskTransactionPtr DataPartStorageOnDiskFull::takeTransactionForBatchCommit()
{
    if (has_shared_transaction || !transaction)
        return nullptr;
    fiu_do_on(FailPoints::part_storage_fail_commit_transaction, { throw ...; });
    return std::exchange(transaction, nullptr);
}
```

```cpp
/// MergeTreeData::Transaction::renameParts — replaces the per-part commitTransaction() loop:
std::map<DiskPtr, std::vector<DiskTransactionPtr>> groups;   /// batch only within one disk
for (const auto & part : precommitted_parts)
    if (auto txn = part->getDataPartStorage().takeTransactionForBatchCommit())
        groups[disk_of(part)].push_back(std::move(txn));

for (auto & [disk, txns] : groups)
    disk->commitTransactions(txns);
```

```cpp
/// IDisk — default implementation: EXACTLY today's sequential semantics.
virtual void commitTransactions(std::span<const DiskTransactionPtr> txns)
{
    for (const auto & txn : txns)
        txn->commit();          /// first throw propagates; the rest never ran — like today
}
```

Properties (each answers a piece of Codex finding 1):

- **No double commit:** pointers are taken, `hasActiveTransaction()` is false, the safety-net loop
  in `Transaction::commit` is a no-op by construction.
- **Shared transactions excluded:** the projection case returns nullptr; the parent commits once.
- **Failpoint preserved per part**, at take time — the batched equivalent of "closing THIS part's
  disk transaction fails" (fires before anything in the batch is durable, which is the strictest
  reading of the invariant the `05014` test pins).
- **Failure semantics:** any throw propagates out of `renameParts` before the Keeper barrier
  (`ReplicatedMergeTreeSink.cpp:~1010` calls `renameParts` before its Keeper multi). Committed
  members stay committed — identical to today's loop failing at member i — and the outer
  `MergeTreeData::Transaction::rollback` compensates every precommitted part with new operations
  over committed disk state (its existing contract). Uncommitted taken transactions die in the
  local vector; their destructors run the same undo they would have run from the storage.
- **Non-CAS disks:** never override; the default loop is byte-for-byte today's behavior.
- **Grouping is per disk** — a transaction batch never spans disks.

### 2. CAS override: outer phases preserved, metadata phase batched {#cas-override}

Codex finding 1 also flagged that batching at the metadata layer must not bypass the outer
`DiskObjectStorageTransaction::commit` behavior (object-storage operation execution,
staging-overlay validation, the `disk_object_storage_fail_commit_metadata_transaction` failpoint,
undo bookkeeping, cleanup). The CAS override therefore splits each member's commit into its
existing outer phases around one batched metadata phase:

```cpp
void DiskObjectStorage::commitTransactions(std::span<const DiskTransactionPtr> txns)
{
    if (!metadata_storage_supports_batched_commit)
        { IDisk::commitTransactions(txns); return; }        /// plain S3 etc: the default loop

    for (each member)  outer_pre(member);                   /// execute object-storage operations,
                                                            /// staging validation, disk failpoint —
                                                            /// exactly the pre-metadata part of
                                                            /// DiskObjectStorageTransaction::commit
    metadata_storage->commitTransactions(metadata_txns);    /// ONE batched call — the phased engine
    for (each member)  outer_post(member);                  /// cleanup/bookkeeping per member
}
```

`IMetadataStorage::commitTransactions` gets the same default-loop shape; only the CAS metadata
storage overrides it.

### 3. The phased engine {#phased-engine}

Each member transaction is a `ContentAddressedTransaction` holding exactly one `PartStaging`
(`ContentAddressedTransaction.h:131-153`: `build`, staged manifest `entries`, `content_removed`,
`published`, `pending_blobs`). The engine gathers the single `StagedPart` of every member —
`{ns, ref, PartStaging *, owning txn *, POD outcome slot}` — and drives:

```
precommitParts:  fan out per-part stageManifest (manifest object PUTs) on the CAS pool -> join;
                 ONE ledger submitBatch of all parts' precommit ops
uploadParts:     flatten ALL parts' pending_blobs into one task list -> fan out on the CAS pool
                 (uploadBlobDetached, §6) -> join -> serially merge results into each part's build
promoteParts:    ONE ledger submitBatch of all parts' promote ops
```

The per-part order `stageManifest → precommitAdd → uploadPendingBlobs → promote` is preserved by
construction — phasing reorders work *across* parts, never *within* a part — so
**EDGE-BEFORE-OBSERVE** (TLA+ Gate A: the manifest edge is durable before any of that part's blobs
is observed) holds exactly as in the sequential path. A part enters `uploadParts` only if its
precommit result (per-item, §4) reports durable; a part is promoted only after all its blobs
uploaded.

Per-member non-commit obligations (`committed`/`failed` flags, staging cleanup,
`cleanupPendingTempFiles`, commit callbacks) run per transaction after its part completes, exactly
as `commit()` does today. The single-transaction `commit()` path delegates to the same engine with
a span of one, so there is one publish code path. For the 500-part INSERT this collapses ~1026
serial ref-log `PUT`s into ~2 and gives cross-part fan-out for manifest and blob uploads.

### 4. Ledger: submission groups {#submission-groups}

Codex findings 3 and 4 (both Blocking) killed v1's `void submitBatch` + "one flush carves
everything" claim. v2 reframes: a **group of individually completable items** that may span
several flushes.

- `submitBatch(items)` enqueues N ordinary `RefMutationItem`s into `pending` under one
  `ref_queue_mutex` acquisition. Each item keeps its own result/error slot — **per-item outcomes
  exist by construction** (finding 3): the engine learns exactly which precommits became durable
  (only those parts proceed to upload) and which item failed validation (only that part's
  transaction fails).
- **Carve rules are untouched.** The leader carves by the existing rules (item cap, same-ref
  scope-cut, `WholeShard` singleton). A group of distinct-ref precommits normally rides in 1-2
  flushes; an interleaved `WholeShard` (GC) or a same-ref collision just splits the group across
  more flushes — soft, no special cases. Items from concurrent INSERTs join the same flushes
  (opportunistic batching keeps working, now with real concurrency to feed it).
- **Waiting condition:** the caller waits (or leads) until **all its items** complete, instead of
  "until my one item completes". The Task-1 leadership-exit guard is untouched: `owned_items` — the
  leader's obligation set — is still defined by what THIS leadership tenure carved, never by group
  membership. A leader exiting after a cut does not fail other tenures' items (finding 4).
- **Cancel-teardown (the poisoned edge of finding 4):** if the submitting stack unwinds while group
  items are outstanding, the items' closures reference transaction state and must not dangle.
  Teardown under the queue mutex: (a) items of this group still in `pending` are removed and
  completed as canceled; (b) items already carved into a live leadership tenure are **awaited**
  (they belong to that tenure's `owned_items` and cannot be stolen); only then does the unwind
  proceed. Same drain-before-teardown principle as the thread pools.
- No lane monopolization: a leader with an incomplete group keeps flushing — that is the work
  itself, and FIFO carving from `pending` carries other callers' items in the same flushes.

### 5. Budget: counts only {#budget}

Codex finding 5 (Important) showed a byte *estimate* cannot guarantee soft spill (undercount →
whole-batch `checkBudget` throw), and the user's verdict was to remove the headache at the root:
**the byte budget for normal transactions is deleted; the budget is counted in ops.**

- **Admission is a counter, exact by construction.** The carve keeps its item cap
  (`kMaxRefBatch` → **1000**). The validation loop accumulates the op count returned by each item's
  `build_ops`; if admitting the next item's ops would exceed `ref_txn_max_ops` (**1000 → 5000**),
  that item and all not-yet-validated carved items are **given back**: returned to the head of
  `pending` in order and struck from the leader's `owned_items` ("returned, no longer owed" — the
  guard invariant becomes "complete everything carved minus everything given back"). The flush
  proceeds with what fit; the next flush takes the rest. No exception, no drop, no estimation.
- **Byte safety is structural, not runtime-estimated.** A per-op encoded-size cap (**4 KiB**) is
  enforced per op — checking ONE op is exact and trivial, no accumulation. Worst case
  5000 × 4 KiB = 20 MiB, far below the 64 MiB `RefLog` object read cap. After §7 removes `payload`,
  `RefOp` has no arbitrary-size field left (ref names from part names + fixed-size manifest
  hashes), so the per-op cap is an unreachable fail-closed invariant, not a live limit. An op that
  somehow exceeds it fails **alone** (the existing per-item validation isolation), so a pathological
  head-of-queue item can never produce an empty-flush livelock.
- `ref_txn_max_bytes` for the normal class is **removed** (encode-side and decode-side). The
  removal-class budget (`ref_removal_max_bytes` = 64 MiB, no op cap) is untouched. `checkBudget`
  reduces to: op count ≤ 5000, each op ≤ 4 KiB — both exact, neither can fire surprisingly on an
  accumulated batch.
- The final whole-transaction `checkBudget` remains as a never-fires assert (counts were admitted
  exactly).

### 6. Upload primitive and the pool {#upload}

Codex finding 7 (Important): `putBlob` mutates `build->deps` on multiple paths, `DepEntry` is
private, `PartWriteTxn` is documented single-writer — concurrent `putBlob` for one part's blobs is
a data race regardless of any post-join merge. v2 introduces the primitive the flat fan-out needs:

- **`PartWriteTxn::uploadBlobDetached(pending_blob) → BlobUploadResult`** — public,
  side-effect-free: reads staging, performs the upload / staging-object promotion, touches **no**
  transaction state, returns a value-only result (blob ref, size, backend outcome, the would-be dep
  record). The existing serial `putBlob` path remains for non-phased callers; the primitive is its
  extracted upload half, not a fork of it.
- `uploadParts` flattens all parts' `pending_blobs` into one task list, fans out on the pool,
  joins, then **serially** merges each result into its part's `build` (the engine is the only
  writer; different parts are different builds; no mutex anywhere).
- **One dedicated CAS pool** (not two): the coordinator — the calling thread — drives phases
  synchronously (fan-out → join → ledger append → fan-out → join), so pool tasks never wait on
  other tasks of the same pool; self-wait deadlock is impossible by construction. The pool is
  **disjoint from `getThreadPoolWriter`** (blob uploads may internally use the S3 multipart writer
  pool — nesting the outer fan-out there invites the classic nested-pool deadlock). Fail-loud
  lifecycle: initialized at server wiring, getter throws if uninitialized, pool size is a server
  setting, `0` rejected (the reverted Task-4/5 lessons).
- **Capture and lifetime discipline** (the B90 lesson + `threadPoolCallbackRunner.h:68` warning):
  tasks capture owning/value state only; the runner is declared so its destructor drains before any
  captured storage dies (declaration order); query `ThreadGroup` is propagated the
  `ThreadPoolCallbackRunnerLocal` way (attach per task, no pinning of a freed group).

### 7. `RefOp::payload` removal {#payload-removal}

Production never populates the opaque `payload` string (`CasRefLogFormat.h:46` says so; the
promote-time `SetPayload` op and `updateRefPayload` carry only `published_at_ms`). Per the
pre-release no-compat policy it is deleted outright:

- Remove the `payload` field from `RefOp`, `RefPayloadUpdate`, the ref-snapshot committed row
  (`CasRefSnapshotFormat.h:41`), and both codecs (`CasRefLogFormat`, `CasRefSnapshotFormat`).
- Rename the op kind `SetPayload` → `SetPublishedAt` (wire word `set_payload` →
  `set_published_at`), `RefPayloadUpdate` → `RefPublishedAtUpdate`, `updateRefPayload` →
  `updateRefPublishedAt` — the op carries a timestamp; its name should stop implying bytes.
- Tests that used `payload` as a generic byte carrier (encoding pins, state machine, intake) are
  rewritten against real fields or dropped.
- Consequence for §5: `RefOp` becomes structurally small; the 4 KiB per-op cap is a pure
  fail-closed invariant.

### 8. Rollback {#rollback}

Scope A makes this section small (v1's finding-8 contradictions do not arise — there is no
multi-part transaction at the seam):

- Each part is its own `ContentAddressedTransaction`; a part's publish failure triggers **its own**
  existing exact rollback: the `created == true` filter + `dropRefIfMatches` on the exact manifest
  this commit bound (`ContentAddressedTransaction.cpp:449-465`) — repoints (`created == false`)
  are never dropped. This code path is unchanged.
- Batch failure at part i: parts before i stay committed (exactly like today's loop failing at
  member i), parts after i never started (their taken transactions' destructors run the standard
  undo), the engine drains all in-flight pool tasks and awaits carved ledger items
  (§4 cancel-teardown) **before** unwinding, then the throw leaves `renameParts` before the Keeper
  barrier and the outer `MergeTreeData::Transaction::rollback` compensates every part of the
  operation — committed ones included — through its existing removal path.
- Each `StagedPart` carries a preallocated **POD `CommitResult`** slot written right after that
  part's durable promote — a `bad_alloc` while recording an outcome after durability must be
  impossible (re-applies the no-throw-slot fix that was reverted with Task 5).

### 9. Recovery streaming {#recovery-streaming}

Codex finding 6 (Important): recovery materializes every post-snapshot decoded `RefLogTxn` in a
vector before replay (`CasRefLedger.cpp:404-418`); the op-cap bump raises worst-case per-txn size
(structural 20 MiB), and a long tail under a failing snapshot publication multiplies it without
bound. Fix, in the same plan as the constant bump: **streaming replay** — sort the (small) object
keys for txn-id order, then GET → decode → apply → discard one transaction at a time. Recovery
memory becomes O(one transaction) for any tail length. The stale 1 MiB comment near
`CasFormat.cpp:80` is updated with the new budget model.

## Settings and gating {#settings}

- Non-CAS disks and engines: zero behavior change (default loops end-to-end).
- CAS: one server-level pool-size setting for the dedicated pool; fail-loud on `0`/uninitialized.
- Ledger changes (submission groups, count budget, `payload` removal) are unconditional
  CAS-internal improvements; today's opportunistic batching is the degenerate case.

## Testing {#testing}

Unit (CA gtest gate `Cas*:CA*` — mind the filter-gap lesson):

1. **Folded-state equivalence** — N single-part transactions committed through the batched seam
   vs today's sequential loop yield identical folded ref-table state (not byte-identical logs).
2. **Seam ownership** — after a successful batch, every part storage reports
   `hasActiveTransaction() == false` and the safety-net loop is a no-op; a projection part with a
   shared transaction is not taken (parent commits once); `part_storage_fail_commit_transaction`
   fires per part and aborts before anything in the batch is durable (the batched extension of the
   `05014` scenario).
3. **Group spans flushes** — a submission group interleaved with a `WholeShard` item and a same-ref
   collision splits across multiple flushes; every item completes; per-item results align.
4. **Concurrent enrichment** — items enqueued by a concurrent thread land in the group's flushes
   (observed batch size > group size).
5. **Count spill** — a group whose ops exceed `ref_txn_max_ops` splits by give-back; every item
   commits; zero exceptions; the overflowing item is returned unpopped and leads the next flush.
6. **Oversized op fails alone** — one op over the per-op cap fails only its item; batch neighbors
   commit (no empty-flush livelock from a poisoned head).
7. **Cancel-teardown** — the submitting stack unwinds mid-group: un-carved items complete as
   canceled, carved items are awaited, no dangling closure runs afterward.
8. **Pool saturation** — pool size N, N+1 parts (and blobs > pool size): completes; a self-wait
   deadlock would hang the test.
9. **Drain-precedes-unwind** — a failing part plus a deliberately slow uploading blob elsewhere:
   the engine's unwind starts only after the join (no `sleep`-based sequencing).
10. **`deps` equivalence** — a multi-blob part uploaded via `uploadBlobDetached` fan-out ends with
    the same `build` deps as the serial `putBlob` path.
11. **No-throw outcome slot** — allocation-failure injection after a durable promote still records
    the POD outcome.
12. **Recovery streaming** — a long tail of maximum-op-count transactions with snapshot publication
    disabled replays with bounded memory (no whole-tail vector).
13. **`payload` removal pins** — encoding-pin tests updated: `set_published_at` wire word, no
    `payload` key in either codec's output.

Integration / soak:

- The CA battery (stateless + integration lanes) green; TXN/GC soak green.
- Re-run the 500-partition INSERT profile: expect `CasRefBatchFlushes` ≪ `CasRefBatchedMutations`
  and wall-clock materially closer to the standard-S3 baseline; verify `CasRefQueueWaitMicroseconds`
  collapse.

## Explicitly out of scope {#out-of-scope}

- Bulk partition operations (backlog known-issue `cc9a8e63401`; three improvement options recorded).
- The HEAD-before-PUT dedup gate and the unconditional promote manifest GET (separate backlog items).
- `published_at_ms` folded into `OwnerTransition` (non-goal above).
- Any change to non-CAS disk/metadata behavior or to the multi-part `external_transaction` path.

## Reused prior work {#prior-work}

Tasks 1-3 of the parallel-write-path plan are landed and load-bearing here: the ref-lane
leadership-exit exception safety (extended by give-back, §5), `CommitOutcome` + `dropRefIfMatches`
(§8), and the ordered-vector + preallocated-slot commit structure. Tasks 4-5 (inner-loop pool +
workers) stay reverted; the pool returns at the cross-part layer only (§6).
