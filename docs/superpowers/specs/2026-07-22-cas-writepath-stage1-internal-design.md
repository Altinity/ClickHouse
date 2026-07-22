# CAS write path, stage 1 — CAS-internal improvements (no upstream changes)

- **Date:** 2026-07-22 (rev. 2 — all nine findings of Codex review round 3 folded in: chunked
  flush replaces give-back, two-phase carve, decode-side byte cap retained, transaction-detached
  upload contract, duplicate-ref grouping, merge-nothing failure contract, corruption≠vanish
  recovery split, in-place replay builder, consumer-inventory completion)
- **Status:** design approved (third design of the write-path effort; v1
  `2026-07-22-cas-multicommit-phased-design.md` and v2
  `2026-07-22-cas-batched-part-commit-v2-design.md` are SUPERSEDED — two rounds of Codex
  adversarial review, both `BLOCKING FLAW`; round 2's decisive finding: the production INSERT
  path commits parts one at a time, so any batch seam at `renameParts` receives spans of one)
- **Area:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` only. Zero upstream
  (MergeTree/Disks-generic) changes in this stage.
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"OPTIMIZATION OPPORTUNITY (write-path
  latency, HIGH) — CAS-on-S3 INSERT ~7.6× slower than standard S3"* (logged 2026-07-21).

## Context: why the effort split into stages {#context}

The measured 7.6× INSERT slowdown (170.6 s vs 22.4 s, 500 partitions) has two independent
components:

1. **No cross-part concurrency.** `ReplicatedMergeTreeSink::finishDelayed`
   (`ReplicatedMergeTreeSink.cpp:434`) iterates partitions serially; each `commitPart` creates its
   own single-part `MergeTreeData::Transaction`, calls `renameParts`, then its Keeper multi
   (`:995-1011`). Exactly one part is ever in flight, so the ref-ledger's opportunistic batching
   starves (measured batch size 1.0: `CasRefBatchFlushes` = `CasRefBatchedMutations` = 1026) and
   every part pays serial manifest/blob/ledger round-trips. Fixing this requires touching the sink
   — **stage 2**, a separate design (concurrent `commitPart` dispatch; the ledger then batches
   emergently, and per-part blob fan-out from this stage multiplexes all in-flight parts on the
   shared pool: cross-part upload parallelism is the product of the two stages).
2. **Serial work inside a single part's commit** and several CAS-internal defects found during the
   adversarial design reviews. These need no upstream change and are this stage.

Both prior designs tried to solve component 1 with a deterministic batch seam below the sink.
Both died in review: v1 on ownership lifecycle, per-item results, rollback semantics, and
byte-estimate admission; v2 on the topology fact above. The deterministic phasing machinery
(`commitTransactions` seam, phased engine, ledger submission groups) is abandoned — stage 2
replaces it with plain concurrency feeding the existing opportunistic queue.

This stage stands alone: every item below is a correct and useful change even if stage 2 never
ships, and none of it blocks on stage-2 decisions.

## Design {#design}

### 1. Parallel blob upload within a part {#parallel-blob-upload}

`ContentAddressedTransaction::publishStaging` uploads a part's `pending_blobs` in a plain serial
loop (`uploadPendingBlobs`, `ContentAddressedTransaction.cpp:241-283`). Change it to fan out on a
dedicated pool.

- **New public transaction-detached primitive** `Cas::PartWriteTxn::uploadBlobDetached
  (pending_blob) → BlobUploadResult`. Contract: **no `PartWriteTxn` mutation** — the task must not
  touch `build` state (`deps` and friends), because `PartWriteTxn` is documented single-writer and
  today's `putBlob` mutates `deps` on several paths (`CasPartWriteTxn.cpp:276` and others).
  The primitive is NOT side-effect-free, and must not be: several pool-level effects are
  ordering-sensitive durable mutations that belong inside the task —
  - backend body/meta operations: the controlled conditional create, freshness-meta transition to
    `Clean` (`CasPartWriteTxn.cpp:510`), condemned-object resurrection (re-upload from the
    retained source + condemned-meta cleanup; the S3 path's sanctioned unconditional same-content
    rewrite, `CasPartWriteTxn.cpp:607`) — the resurrect invariant (never GET a condemned object)
    is order-preserved because each branch stays intact inside its task;
  - dedup-cache reads and insertions (`CacheBase` locks internally, `CacheBase.h:140` —
    concurrent insertion is safe and keeps the hint live for concurrent writers);
  - event-sink emission, request-controller accounting, ProfileEvents.
  Every branch (dedup-cache hit, HEAD-first hit/miss + live adopt with its meta point-read and
  backfill, fresh local streaming, S3-native staging promotion, condemned local/S3 resurrection)
  returns a **complete** `BlobUploadResult` value carrying the dep record and outcome; no branch
  may leave its dep effect behind as a side effect.
- **Merge is encapsulated:** a new `PartWriteTxn::mergeBlobUploadResults(span<BlobUploadResult>)`
  applies the returned dep records on the calling thread after the join — the private dep
  representation is not exposed to `ContentAddressedTransaction`.
- **One task per unique ref:** `pending_blobs` can hold duplicate `BlobRef`s (staged hardlink
  copies push a copy of the record, `ContentAddressedTransaction.h:205`; the current filter
  deduplicates membership, not the loop, `.cpp:248`). The fan-out groups pending blobs by
  `BlobRef` before dispatch, validates identical declared sizes (conflicting sizes are rejected as
  a logical error), launches exactly one task per unique ref, and merges exactly one dep result.
- **Failure contract — merge nothing:** the join always drains every task; if any task failed, NO
  result is merged — the build stays at its pre-fan-out pending-dep state, the part's publish
  fails, the precommit is abandoned (existing path), local staging is deleted, S3 staging follows
  the existing lease-sweeper policy. Successful sibling uploads are NOT a new orphan class: the
  manifest edge was durably precommitted before any upload (`ContentAddressedTransaction.cpp:373`),
  abort removes that precommit, and the resulting zero-edge bodies are exactly the GC-reclaimable
  debris the transaction already documents (`.cpp:414`). Serial-vs-parallel equivalence is claimed
  for successful runs only — the serial loop's partial-mutation-then-fail states are intentionally
  not reproduced.
- **One dedicated server-wide pool**, disjoint from `getThreadPoolWriter` (blob uploads may
  internally use the S3 multipart writer pool; nesting the outer fan-out there invites the
  nested-pool deadlock). The calling thread only submits and joins — it never occupies a pool slot
  — so pool tasks never wait on same-pool tasks and pool size 1 is correct (degenerates to
  serial). Fail-loud lifecycle: initialized at server wiring, getter throws if uninitialized, size
  is a server setting, `0` rejected at configuration parse.
- **Capture and lifetime discipline** (the B90 lesson, `threadPoolCallbackRunner.h:68` warning):
  tasks capture owning/value state only; the runner is declared inside the scope that owns the
  captured storage so its destructor drains first on every path, including a throw during
  dispatch; the query `ThreadGroup` is propagated per task the `ThreadPoolCallbackRunnerLocal`
  way.
- Sizing note for stage 2: the pool is shared across all concurrent part commits by design.
- Expected effect now: overlaps a part's blob PUTs and dedup HEADs. On the measured INSERT profile
  CAS packs ~2 blobs/part, so the win there is bounded; merges and large parts with many blobs
  benefit proportionally more.

### 2. Ledger: two-phase carve (fix the throwable transfer window) {#two-phase-carve}

Real today, independent of everything else: the flush carve pops items from `pending` under
`ref_queue_mutex` while interleaving allocating operations — `seen_refs.insert`,
`batch.push_back`, and after the mutex is released, `owned_items.insert`
(`CasRefLedger.cpp:1240-1265`). Any of those throwing after the first pop leaves already-popped
items neither pending nor owned: their waiters block forever and their callbacks' lifetimes become
unsafe. (Reserving `owned_items` alone is NOT sufficient — the intra-loop `seen_refs`/`batch`
allocations throw between pops.)

Fix — **two-phase carve**:

1. **Plan (may throw, mutates nothing):** under `ref_queue_mutex`, scan `pending` without popping;
   build the selection (`seen_refs`, the batch vector, all reservations including
   `owned_items` capacity ≥ current size + selection size). Any throw here leaves the queue
   untouched.
2. **Publish (no-throw):** still under the mutex, pop the selected items and append them to
   `owned_items` using only non-throwing operations (capacity pre-reserved, moves of
   `shared_ptr`s).

A related pre-existing hole in the validation loop is fixed alongside (found in review round 3):
`working` is updated before the allocating `final_ops.insert` / `survivors.push_back`
(`CasRefLedger.cpp:1340`) — an allocation failure there can leave a failed item's effects in
`working` or its ops in `final_ops`. Fix: reserve `final_ops`/`survivors` growth before applying
the item to `working`; publish `working` only after all accumulation steps are past their throwing
points.

### 3. Budget: counts only, chunked flush {#budget}

Replaces the byte-estimate admission that review round 1 falsified. Review round 3 then killed the
"give-back" spill (returning a built item to `pending` re-invokes its `build_ops`, violating the
documented at-most-once contract, `CasRefLedger.h:102`, with impure builder callbacks; and
striking the leader's own pre-owned item from `owned_items` reopens the guard hole). rev. 2
replaces spilling with **chunked flushing** — nothing is ever given back:

- `ref_txn_max_ops` **1000 → 5000**; the carve item cap (`kMaxRefBatch`) **128 → 1000**. The
  validation loop counts ops per admitted item (`build_ops` result size — exact by construction).
- **Chunked flush:** when admitting the next item's ops would exceed the op cap, the leader
  **appends the accumulated transaction immediately** (encode + `PUT`, exactly the existing append
  step), then continues the same validation loop into a fresh transaction against the updated
  `working` state. The overflowing item is validated exactly once, in the chunk where it lands;
  ownership (`owned_items`) is untouched across chunks; one leadership tenure may emit several
  ref-log transactions. If a chunk's append fails, the remaining carved items fail with it — the
  existing tenure-failure semantics, no new states.
- **A single item whose own op count exceeds the cap fails alone** — completed with an error like
  any per-item validation failure (its ops never enter a chunk), neighbors unaffected.
- **Removal-class is exempt, detected by op inspection:** an item is removal-class iff its built
  ops contain `RemoveNamespace` — the codec already classifies this way
  (`CasRefLogFormat.cpp:51`); `WholeShard` scope alone is NOT the discriminator (stale-precommit
  reclaim is also `WholeShard`, `CasRefLedger.cpp:1979`). Removal-class transactions keep the
  64 MiB `ref_removal_max_bytes` byte budget and have no op cap; they are already carved as
  singletons.
- **Byte limits: encode-side estimation machinery is what dies; the decode-side cap stays.**
  The normal-class whole-transaction byte cap is retained at **20 MiB** as a decode-side
  acceptance bound (`checkBudget` on decode) and as a post-encode assert on the writer (the
  canonical writer cannot reach it: ≤ 5000 ops × 4 KiB). It is no longer an admission input —
  admission is ops-only — so no accumulated-size estimation exists anywhere. Rationale (review
  round 3): without a decode cap, a tolerated-unknown-field-padded or raw-body object up to the
  64 MiB object cap would decode as a "normal" transaction; additionally `openObject` applies
  `object_cap` only to zstd frames (`CasTextFormat.cpp:384`) — that gap is closed by enforcing
  `object_cap` for raw bodies too.
- **Per-op size cap 4 KiB** on normal-class ops, enforced exactly per op at admission (encode one
  op — no accumulation), failing the op's item alone. Not claimed unreachable:
  `checkCanonicalRefName` imposes no length limit and part names grow with partition-key values
  (`MergeTreePartition.cpp:272`). The decoder enforces the same per-op bound.

### 4. `RefOp::payload` removal {#payload-removal}

Production never populates the opaque `payload` string; the promote-time op and `updateRefPayload`
carry only `published_at_ms`. Per the pre-release no-compat policy:

- Remove `payload` from `RefOp` (`CasRefLogFormat.h`), `RefPayloadUpdate` (`CasRefProtocol.h:111`),
  the snapshot committed row (`CasRefSnapshotFormat.h:41`), and both codecs.
- Rename `SetPayload` → `SetPublishedAt` (wire word `set_payload` → `set_published_at`),
  `RefPayloadUpdate` → `RefPublishedAtUpdate`, `updateRefPayload` → `updateRefPublishedAt`.
- Full consumer inventory: `RefTableState::applySetPayload` (`CasRefProtocol.cpp`), promote's op
  construction (`CasPartWriteTxn.cpp:1089`), the ledger's state-growing op classification
  (`CasRefLedger.cpp:1321`) and the forwarding `Pool` update API, `CasInspect` (renders payload
  size and the op kind — `CasInspect.cpp:137,185`), fsck/codec round-trip tests, encoding-pin
  tests, benchmarks, and any test using `payload` as a generic byte carrier (rewritten against
  real fields or dropped).
- **Operational requirement, stated explicitly:** wire-word and field removal are safe only
  because every existing pool is recreated before this ships (pre-release, no persisted data —
  the standing CA policy). No decoder tolerance for the old field is added.
- Non-goal (future): folding `published_at_ms` into `OwnerTransition` to drop one op per promote.

### 5. Recovery: streaming replay with candidate discipline {#recovery-streaming}

Motivated by the op-cap bump (worst-case normal transaction grows to 20 MiB) and a tail that can
be long when snapshot publication is failing. Today recovery materializes every post-snapshot
decoded `RefLogTxn` in a vector before replay (`CasRefLedger.cpp:404-423`), then installs the
result only after the whole tail was fetched.

Change: stream — sort the (small) object keys for txn-id order, then GET → decode → apply →
discard one transaction at a time, **into a private candidate `RefTableState`**, never into the
live `rt.state`:

- **Failure split (review round 3 — these are different classes, not one):** a **vanished**
  selected object (LIST/GET race) discards the candidate and restarts with a fresh LIST — the
  existing bounded inner-restart behavior. A **decode/apply corruption** discards the candidate
  and propagates the error fast — exactly today's non-transient classification
  (`CasRefLedger.cpp:64,549`); re-listing a durably corrupt object would be a retry loop.
- **In-place replay builder:** applying via the public scratch-copying `applyRefLogTxn` per
  transaction would copy the growing candidate once per txn; a narrowly scoped builder owns the
  candidate, applies each decoded transaction in place (the private poisoning/in-place path,
  `CasRefProtocol.cpp:361` — the pattern current replay already uses), and discards the candidate
  on any failure.
- Tail count/bytes tracked as scalars; the candidate, cleanup markers, snapshot identity,
  counters, budgets and the `recovered` flag publish together under `state_mutex`, and recovery
  waiters are notified only after that complete publication — as today.
- **The other full-tail materializers are covered too** (review round 3): the shared recovery used
  by the orphan sweep (`recoverRefTableDetailed`, `CasRefProtocol.cpp:667`) and fsck's snapshot
  oracle (`CasFsck.cpp:209`) stream through the same builder. (GC folding already decodes one log
  at a time, `CasGc.cpp:1073` — unchanged.)
- The stale 1 MiB budget comment near `CasFormat.cpp:80` is updated.

## Settings {#settings}

- One new server-level setting: the CAS upload pool size (fail-loud: `0`/uninitialized rejected).
- No behavior change for non-CAS disks or engines (nothing outside the CAS tree is touched).

## Testing {#testing}

Unit (CA gtest gate `Cas*:CA*` — mind the filter-gap lesson):

1. **`deps` equivalence on success** — a multi-blob part uploaded via the fan-out ends with the
   same `build` state as the serial path, across branches: dedup-cache hit, HEAD-first hit,
   HEAD-first miss + live adopt, local staging, S3-native staging (with_rustfs), condemned local
   and condemned-S3 resurrection.
2. **Duplicate refs** — pending blobs with the same `BlobRef` (staged hardlink copy) launch ONE
   task and merge ONE dep; conflicting declared sizes are rejected; a condemned-S3 duplicate pair
   performs the resurrection without corruption (content-correct under the token-insensitive
   backend contract).
3. **Merge-nothing failure** — one blob task fails, a sibling succeeds: no result is merged, the
   build is at its pre-fan-out state, the precommit is abandoned, and the sibling's uploaded body
   is subsequently reclaimed by a GC round in the test (pinning the no-new-orphan-class claim).
4. **Concurrent dedup-cache insertion** — two tasks inserting/reading the same cache concurrently,
   TSan-clean.
5. **Pool saturation** — pool size 1 and pool size N with blobs > N: completes within a bounded
   watchdog latch; a self-wait deadlock fails fast.
6. **Drain-precedes-unwind** — one failing task plus one deliberately slow task: publish failure
   raised only after the join; a throw during dispatch drains already-running tasks; no
   `sleep`-based sequencing (latches).
7. **Two-phase carve** — allocation-failure injection covering the WHOLE selection/transfer
   protocol (plan-phase allocations: `seen_refs`, batch growth, reserves; not just
   `owned_items.insert`): every already-selected item either completes normally or was never
   popped; no waiter hangs.
8. **Validation-loop exception safety** — allocation failure at the `final_ops`/`survivors`
   insertion point: the failed item's effects are absent from `working` and from the encoded
   transaction.
9. **Chunked flush** — a carve whose total ops exceed `ref_txn_max_ops`: multiple ref-log
   transactions appear in one tenure, every item completes exactly once (`build_ops` invocation
   counters assert at-most-once), folded state matches the sequential result; a chunk-append
   failure fails the remaining items with the existing tenure semantics.
10. **Oversized item / oversized op fail alone** — an item with > `ref_txn_max_ops` ops fails only
    itself; an op > 4 KiB (maximum-length ref name) fails only its item; neighbors commit.
11. **Removal-class detection** — a `dropNamespace` over > 5000 refs succeeds (byte-budgeted, no
    op cap); a stale-precommit-reclaim `WholeShard` item is NOT exempted (op-inspection
    discriminator, not scope).
12. **Decode-side bounds** — a raw (uncompressed) over-cap object is rejected (`object_cap`
    applies to raw bodies); an unknown-field-padded normal transaction over 20 MiB is rejected at
    decode; a 5000×4 KiB canonical transaction round-trips.
13. **Payload removal pins** — `set_published_at` wire word, no `payload` key in ref-log or
    snapshot output; `CasInspect` renders the renamed op; codec round-trips.
14. **Recovery streaming** — a long tail of maximum-op-count transactions replays under a hard
    peak-memory bound the old whole-tail implementation would exceed; a mid-tail vanished object
    discards the candidate and re-LISTs; an injected corrupt object fails fast (no re-LIST loop);
    a concurrent recovery waiter is unblocked exactly once; the orphan-sweep recovery and fsck
    paths run under the same memory bound.

Integration / soak:

- CA battery (stateless + integration lanes) green; TXN/GC soak green (the soak exercises the
  bumped op cap through mutation storms and the recovery path through kill-restart cycles).
- Re-profile the 500-partition INSERT: expect the blob-upload segment of each part's commit to
  shrink (bounded win, ~2 blobs/part); `CasRefBatchFlushes`/`CasRefBatchedMutations` stays ~1.0
  **by design** in this stage — the batch-size collapse is stage 2's acceptance metric, not this
  stage's.

## Explicitly out of scope {#out-of-scope}

- **Stage 2:** concurrent `commitPart` dispatch in `ReplicatedMergeTreeSink::finishDelayed` (and
  the non-replicated sink) — separate brainstorm/spec; requires upstream-surface consultation.
- Bulk partition operations (backlog known-issue `cc9a8e63401`).
- The HEAD-before-PUT dedup gate and the unconditional promote manifest GET (separate backlog
  items).
- Any deterministic batch seam below the sink (`commitTransactions`, phased engine, ledger
  submission groups) — abandoned with v1/v2.

## Reused prior work {#prior-work}

Tasks 1-3 of the parallel-write-path plan remain landed and load-bearing (ref-lane
leadership-exit exception safety — extended here by §2's two-phase carve and §3's chunk
accounting — `CommitOutcome` + `dropRefIfMatches`, ordered-vector + preallocated-slot commit
structure). Tasks 4-5 stay reverted; the dedicated pool returns in §1 for blob uploads only.
