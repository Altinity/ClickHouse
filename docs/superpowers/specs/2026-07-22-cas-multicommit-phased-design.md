# CAS multi-commit — phased cross-part commit at the `renameParts` seam

- **Date:** 2026-07-22
- **Status:** SUPERSEDED by `2026-07-22-cas-batched-part-commit-v2-design.md`. A Codex/gpt-5.6-sol
  xhigh adversarial review returned `BLOCKING FLAW` (4 blocking + 5 important findings — ownership
  lifecycle at the seam, void `submitBatch` without per-item results, non-reusable leadership-exit
  guard, rollback semantics contradicting the `created`-filter, byte-estimate admission, and a
  bulk-topology claim not present in the actual `REPLACE`/`MOVE PARTITION` paths). Kept for the
  problem analysis; do not implement from this document. (Historical: supersedes the inner-loop
  half of `2026-07-22-cas-parallel-write-path-design.md`; Tasks 4-5 of that plan were reverted
  after the granularity investigation showed the inner loop is the wrong layer for INSERT.)
- **Area:** `src/Storages/MergeTree/MergeTreeData.cpp` (`Transaction::renameParts`/`commit`),
  `src/Disks/IDisk.h` / `src/Disks/ObjectStorages/IMetadataStorage.h` (new seam),
  `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (phased implementation,
  ref-ledger batching, upload pool)
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"OPTIMIZATION OPPORTUNITY (write-path
  latency, HIGH) — CAS-on-S3 INSERT ~7.6× slower than standard S3"* (logged 2026-07-21).

## Problem {#problem}

A CAS-on-S3 INSERT of a 500-partition wide insert (10M rows, 30 columns) measured **170.6 s**
versus **22.4 s** on a standard S3 `ReplicatedMergeTree` — 7.6× slower. The staging `query_log`
profile pinpointed the shape of the loss:

- `CasRefBatchFlushes` = `CasRefBatchedMutations` = 1026 → **ref-ledger batch size exactly 1.0**.
  The ledger *can* batch concurrent same-namespace mutations, but nothing ever runs concurrently:
  513 precommits + 513 promotes each paid their own serial ref-log `PUT` (~46 ms each).
- `CasRefQueueWaitMicroseconds` = 36.2 s of pure ref-lane serialization.
- Average write concurrency ~4.7 threads (standard disk: ~12× effective concurrency via
  `getThreadPoolWriter`).
- Per-part blob uploads run in a plain serial `for` loop inside
  `ContentAddressedTransaction::uploadPendingBlobs`.

### Why the previous attempt was the wrong layer {#wrong-layer}

The reverted Task 5 parallelized the `for (parts)` loop **inside**
`ContentAddressedTransaction::commit()`. The granularity investigation
(`tmp/cas-commit-granularity.md`) established decisively that a plain INSERT performs **one
`commit()` per part** (`parts.size() == 1` always): `MergeTreeData::Transaction::renameParts`
(`MergeTreeData.cpp`, the `precommitted_parts` loop) calls per-part `commitTransaction()`, and each
part opened its own disk transaction back in `MergeTreeDataWriter` /
`DataPartStorageOnDiskFull::beginTransaction`. Multi-part-in-one-`commit()` exists only on bulk
paths (`REPLACE`/`MOVE PARTITION` via a shared `external_transaction`). Inner-loop parallelism
therefore cannot help INSERT at all. Cross-part concurrency must be introduced **above** the disk
transaction — at the `renameParts` seam — while keeping all parts-awareness out of generic disk
code.

## Goal {#goal}

One design that serves **both** commit topologies:

- **INSERT:** N separate disk transactions, one part each;
- **bulk (`REPLACE`/`MOVE PARTITION`):** one shared disk transaction holding N parts;

by phasing the CAS commit work across parts — *precommit-all → upload-all → promote-all* — so that
the ref-ledger receives many mutations at once (deterministic batching instead of accidental),
and blob uploads from **all** parts fan out onto a thread pool at once. Non-CAS engines and disks
are byte-for-byte unchanged in behavior.

## Design {#design}

### 1. The seam: `commitTransactions` (transaction-centric, not part-centric) {#seam}

A generic disk knows nothing about parts, so the seam speaks only of transactions:

```cpp
/// IMetadataStorage
virtual void commitTransactions(std::span<const MetadataTransactionPtr> txns);
/// default implementation: for (auto & tx : txns) tx->commit();
```

- `IDisk` gains the forwarding counterpart (`commitTransactions(std::span<const DiskTransactionPtr>)`),
  with the same default per-transaction loop; `DiskObjectStorage` forwards to its metadata storage.
- `MergeTreeData::Transaction::renameParts` (and the safety-net loop in `commit`) collects the
  per-part disk transactions of `precommitted_parts` that live on the **same disk** and hands them
  to that disk's `commitTransactions` in one call, instead of looping per-part
  `commitTransaction()`. Parts on different disks form separate groups. The existing
  "all parts durable **before** Keeper block-id registration" barrier is preserved verbatim: the
  batched call returns only when every transaction in the span is committed (or the batch failed —
  see rollback), exactly like the sequential loop did.
- Non-CAS storage (local disk, plain S3, encrypted, …) never overrides the default → the loop is
  what runs today, semantics untouched.
- Gating: the batched call is used unconditionally at the `MergeTreeData` layer (it degrades to the
  identical sequential loop by default), so no `isContentAddressed` checks leak into MergeTree code.

### 2. The phasing unit: `PartStaging` {#part-staging}

`ContentAddressedTransaction` already keys all commit-relevant state by part in
`std::map<std::pair<std::string /*ns*/, std::string /*ref*/>, PartStaging> parts`
(`ContentAddressedTransaction.h:131-153`): `build` (the `PartWriteTxn`), staged manifest
`entries`, `content_removed`, `published`, and `pending_blobs`. The per-part publish today is
`publishStaging = stageManifest → precommitAdd → uploadPendingBlobs → promote`.

The CAS override of `commitTransactions` gathers **every `PartStaging` of every transaction in the
span** into one working set and drives it through plural phase methods:

```
precommitParts(span<StagedPart>)   // stageManifest + precommitAdd for all parts
uploadParts(span<StagedPart>)      // all pending blobs of all parts, flat fan-out on the upload pool
promoteParts(span<StagedPart>)     // promote for all parts
```

where `StagedPart = {ns, ref, PartStaging *, owning transaction *, outcome slot}`. A single-part
transaction committed alone is simply a span of one — `commit()` (the single-tx path) delegates to
the same engine, so there is exactly one publish code path.

Per-part *order* is preserved: for each part, its manifest edge is durably precommitted before any
of its blobs is uploaded, and its promote runs only after all its blobs are up. This keeps the
**EDGE-BEFORE-OBSERVE** invariant (TLA+ Gate A) intact — phasing reorders work *across* parts,
never *within* a part.

Non-commit obligations of each member transaction (staging cleanup, `committed`/`failed` flags,
`cleanupPendingTempFiles`, commit callbacks) run per-transaction after its parts complete, exactly
as `commit()` does today.

### 3. Ref-ledger: deterministic `submitBatch` {#submit-batch}

Today `appendRefOps` enqueues one item and the queue leader opportunistically carves whatever is
pending — which is batch size 1 when callers are serial. New entry point:

```cpp
/// CasRefLedger — enqueue N mutation items under ref_queue_mutex in one shot, then flush.
void submitBatch(std::vector<RefMutationSubmission> items);
```

- All N items enter `pending` atomically (one `ref_queue_mutex` acquisition), then one flush cycle
  runs. The leader's carve takes **everything currently pending** — our N *plus* any items enqueued
  concurrently by parallel INSERTs into the same namespace. Concurrent callers wake as followers
  with their mutation already applied. This is strictly beneficial: concurrent load makes batches
  *bigger*, not contended.
- The existing same-`ref_name` scope-cut and `WholeShard` carve rules are unchanged (a batch of
  distinct parts has distinct refs, so the scope-cut never fires on this path).
- The Task-1 leadership-exit guard (complete + de-pend every carved item on any exceptional exit)
  covers the new entry point unchanged — `submitBatch` produces ordinary queue items.
- `precommitParts` submits all parts' precommit ops as one batch; `promoteParts` all promotes as
  one batch. For the 500-part INSERT this collapses ~1026 serial ref-log `PUT`s into **~2**.

### 4. One authoritative budget; budget-aware carve; soft overflow {#budget}

The double-limit problem: today the carve caps items at `kMaxRefBatch = 128` (an undocumented
count) while the *format* enforces `ref_txn_max_ops = 1000` / `ref_txn_max_bytes = 1 MiB` at encode
time via `checkBudget` — two knobs in two places that must be kept consistent by hand, and the
format side **rejects** (throws) when exceeded. Resolution:

- **The format budget is the single authority.** New values: `ref_txn_max_ops` **1000 → 5000**,
  `ref_txn_max_bytes` **1 MiB → 20 MiB** (consistent: below `ref_removal_max_bytes` = 64 MiB and
  below the RefLog object cap of 64 MiB). `kMaxRefBatch` is retired as an independent tuning knob:
  it stays only as an item-count sanity backstop raised to **1000**. Whichever bound trips first —
  bytes, ops, or the backstop — ends the batch the same soft way (spill, below), so the backstop
  needs no coordination with the format values.
- **The carve becomes budget-aware.** While carving, the leader keeps a running estimate of the
  encoded transaction size (bytes and op count). If adding the *next* pending item would exceed
  either limit, the carve **breaks without popping it** — the accumulated batch flushes, and the
  remainder spills into the next flush cycle (same leader loops, or the next leader picks it up).
  **No exception, no drop** — overflow is just the point where one batch ends and the next begins.
- **On the write path this means zero budget exceptions, ever:** precommit/promote ops are small
  fixed-size owner-transition records (hundreds of bytes); 5000 ops / 20 MiB is unreachable.
- The format-level `checkBudget` reject remains **only** as the fail-closed guard for a single item
  that alone exceeds the budget (a pathological oversized-payload op). Such an item can never fit
  in any batch, so spilling would loop forever; it fails **alone** (per-item validation already
  isolates it — survivors commit). Unreachable for INSERT/bulk commit ops.
- The constant bumps ship inside this effort's plan (gated by the CA battery + soak: large-txn
  recovery replay, snapshot-budget interaction), not as a drive-by edit.

### 5. Upload: dedicated pool, flat cross-part fan-out {#upload-pool}

- A dedicated CAS upload pool, **disjoint from `getThreadPoolWriter`** — CAS blob uploads may
  themselves use the S3 multipart writer pool internally, and running the outer fan-out on the same
  pool invites a nested-pool deadlock (outer tasks occupying all slots while waiting on inner
  tasks). Pool is initialized at server startup wiring (fail-loud getter: throws if uninitialized
  — the reverted Task-4 lesson), sized by a server setting.
- `uploadParts` **flattens** the `pending_blobs` of *all* parts in the span into one task list and
  fans it out onto the pool in one wave, then joins. Parallelism is bounded by the pool size, not
  by per-part structure — a span of 500 parts × 2 blobs yields 1000 independent tasks.
- **`deps` merge without locks:** blobs of the *same* part uploading concurrently must not mutate
  that part's `build->deps` map concurrently. Each upload task therefore *returns* its `DepEntry`
  (plus staging-promotion results); after the join, the engine merges each part's entries into its
  own `build` serially. Different parts have different `build` objects, so no mutex anywhere.
- Structured join before anything else proceeds: no promote of any part starts until the join
  completes (simplest correct barrier; per-part early promote is a possible later refinement,
  explicitly out of scope now).
- Within the phase engine the per-part precondition stands: a part's blobs are only in the task
  list because its manifest edge was already precommitted in `precommitParts`.

### 6. Cross-part commit concurrency {#commit-concurrency}

The phase engine itself may run per-part CPU-bound work (manifest staging/encoding) and per-part
Keeper/pool interactions concurrently across parts on a bounded worker set: a **commit pool
distinct from the upload pool** (two pools, both disjoint from `getThreadPoolWriter`) — commit
workers must never occupy the slots the upload fan-out needs. Concurrency is
bounded by a setting; `0`/uninitialized is rejected fail-loud (the reverted Task-5 hang lesson:
a zero-sized pool must be an error, not an infinite wait).

### 7. Rollback {#rollback}

Reuses the landed substrate (Tasks 2-3) and re-applies the no-throw-slot fix that was reverted
together with Task 5:

- Every `StagedPart` carries a preallocated outcome slot sized before any phase runs. The slot
  type is a **POD `CommitResult`** (no string copies after a durable promote — a `bad_alloc` while
  recording an outcome must be impossible, otherwise a created ref is lost to the rollback).
- On failure in any phase: **drain first** — every in-flight pool task joins before rollback
  touches shared state (structural: the runner's scope guarantees it on every path, including
  throw-during-dispatch).
- Then per-part exact rollback: for every part whose promote already published a manifest,
  `dropRefIfMatches` keys on the **exact manifest** that part's publish committed
  (`CommitOutcome`), so a concurrent writer's newer manifest is never clobbered. Parts that never
  precommitted need nothing; precommitted-but-not-promoted parts are cleaned by the existing
  precommit-abandon path.
- Batch atomicity semantics match today's sequential loop: a failure mid-batch leaves earlier
  parts committed (same as a failure mid-loop today) and rolls back the failing part exactly;
  `renameParts` sees the exception before Keeper block-id registration, so ZooKeeper never learns
  of parts whose durability is in doubt. No new atomicity promise is invented.

### 8. Settings and gating {#settings}

- Non-CAS: no behavior change (default `commitTransactions` loop; nothing else reached).
- CAS: `cas_commit_concurrency` (cross-part phase workers) and the upload pool size as server-level
  settings; both fail-loud on invalid values (`0` rejected).
- The ref-ledger changes (`submitBatch`, budget-aware carve, new budget constants) are
  unconditional CAS-internal improvements — they need no gate; opportunistic batching today is
  just the degenerate case.

## Testing {#testing}

Unit (CA gtest gate, `Cas*:CA*` — mind the filter-gap lesson):

1. **Parallel == sequential folded state** — the same multi-part working set committed through the
   phased engine vs the sequential loop yields identical folded ref-table state (not byte-identical
   logs: batching changes packing/ids).
2. **Both topologies** — N single-part transactions batched at the seam, and one shared
   N-part transaction, drive the same engine and produce the same state.
3. **`submitBatch` sweeps concurrent items** — items enqueued by a concurrent thread between batch
   enqueue and carve are carved into the same flush (batch size > N observed).
4. **Budget spill, no throw** — a submission whose encoded size exceeds `ref_txn_max_bytes`
   (or `ref_txn_max_ops`) splits into multiple flushes; every item commits; zero exceptions;
   the spill point never pops the item that would overflow.
5. **Oversized single item fails alone** — one pathological item over-budget by itself fails its
   own submission; neighbors in the same batch commit.
6. **Saturation-bounded completion** — pool size N, N+1 parts: completes (would hang on a
   self-wait deadlock). No `sleep`-based sequencing anywhere.
7. **Drain-precedes-rollback under a slow worker** — a failing part plus a deliberately slow
   uploading part: rollback observably starts only after the join.
8. **No-throw outcome slot** — allocation-failure injection after a durable promote still records
   the outcome (POD slot write).
9. **`deps` merge correctness** — multi-blob part uploaded via the flat fan-out ends with the same
   `deps` as the serial path.

Integration / soak:

- The CA battery (822 stateless + integration lane) green.
- A multi-part INSERT profile run re-measured: expect `CasRefBatchFlushes` ≪ `CasRefBatchedMutations`
  (batch size ≫ 1) and wall-clock materially closer to the standard-S3 baseline.
- Soak covers the bumped format constants (large-txn recovery replay).

## Explicitly out of scope {#out-of-scope}

- The HEAD-before-PUT dedup gate (`dedup_head_first_min_bytes`) — separate backlog item.
- The unconditional promote manifest GET — separate backlog item.
- Per-part early promote before the global upload join (refinement, only if measurements demand).
- Any change to non-CAS disk/metadata behavior.

## Superseded / reused prior work {#prior-work}

- `2026-07-22-cas-parallel-write-path-design.md` Tasks 1-3 are **landed and reused**: ref-lane
  leadership-exit exception safety, `CommitOutcome` + `dropRefIfMatches`, per-part rollback with
  ordered vector + preallocated slots.
- Tasks 4-5 (dedicated commit pool wired for the inner loop; inner-loop bounded workers) were
  **reverted** (`b7b9709c6dc`): wrong layer for INSERT. The pool concept returns here at the
  cross-part layer; the inner-loop parallelism does not return.
