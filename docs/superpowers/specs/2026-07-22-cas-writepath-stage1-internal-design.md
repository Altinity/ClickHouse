# CAS write path, stage 1 — CAS-internal improvements (no upstream changes)

- **Date:** 2026-07-22
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
2. **Serial work inside a single part's commit** and several CAS-internal defects found during two
   adversarial design reviews. These need no upstream change and are this stage.

Both prior designs tried to solve component 1 with a deterministic batch seam below the sink
(`IMetadataStorage`/`renameParts` level). Both died in review: v1 on ownership lifecycle,
per-item results, rollback semantics, and byte-estimate admission; v2 on the topology fact above
(a batch seam below the sink never sees more than one transaction on the INSERT path). The
deterministic phasing machinery (`commitTransactions` seam, phased engine, ledger submission
groups) is abandoned — stage 2 replaces it with plain concurrency feeding the existing
opportunistic queue.

This stage stands alone: every item below is a correct and useful change even if stage 2 never
ships, and none of it blocks on stage-2 decisions.

## Design {#design}

### 1. Parallel blob upload within a part {#parallel-blob-upload}

`ContentAddressedTransaction::publishStaging` uploads a part's `pending_blobs` in a plain serial
loop (`uploadPendingBlobs`). Change it to fan out on a dedicated pool:

- **New public side-effect-free primitive** `Cas::PartWriteTxn::uploadBlobDetached(pending_blob)
  → BlobUploadResult`. It performs the upload half of today's `putBlob` path for a staged pending
  blob — staging read/promotion, dedup HEAD-first gate, backend PUT — and touches **no**
  transaction state (today's `putBlob` mutates `build->deps` on several paths, and `PartWriteTxn`
  is documented single-writer, so concurrent calls into the existing entry point are a data race).
  It returns a value-only result: blob ref, size, backend outcome, and the complete dep record the
  caller must merge. Every branch of the existing upload path (dedup-cache hit, HEAD-first hit,
  local staging, S3-native staging promotion, condemned-object re-upload) must return a complete
  result value — partial results recreate the shared-state mutation the primitive exists to avoid.
- `uploadPendingBlobs` becomes: fan out one task per pending blob onto the CAS upload pool →
  structured join → **serially** merge each `BlobUploadResult` into `build` (the calling thread is
  the only writer; no mutex). The serial `putBlob` path remains for non-staged/inline callers; the
  primitive is its extracted upload half, not a fork.
- **One dedicated server-wide pool**, disjoint from `getThreadPoolWriter` (blob uploads may
  internally use the S3 multipart writer pool; nesting the outer fan-out there invites the
  nested-pool deadlock). The calling thread only submits and joins — it never occupies a pool slot
  — so pool tasks never wait on same-pool tasks and pool size 1 is correct (degenerates to
  serial). Fail-loud lifecycle: initialized at server wiring, getter throws if uninitialized, size
  is a server setting, `0` rejected at configuration parse (the reverted Task-4/5 lessons).
- **Capture and lifetime discipline** (the B90 lesson, `threadPoolCallbackRunner.h:68` warning):
  tasks capture owning/value state only; the runner is declared inside the scope that owns the
  captured storage so its destructor drains first on every path, including a throw during
  dispatch; the query `ThreadGroup` is propagated per task the `ThreadPoolCallbackRunnerLocal`
  way. A failed blob task fails the part's publish after the join completes — drain precedes any
  unwind.
- Sizing note for stage 2: the pool is shared across all concurrent part commits by design; when
  stage 2 lands, uploads from all in-flight parts multiplex here without further changes.
- Expected effect now: overlaps a part's blob PUTs and dedup HEADs. On the measured INSERT profile
  CAS packs ~2 blobs/part, so the win there is bounded (~N−1 PUT latencies per part); merges and
  large parts with many blobs benefit proportionally more.

### 2. Ledger: fix the throwable pending→owned transfer (existing bug) {#owned-transfer}

Found by review round 2 and real today, independent of everything else: the flush carve pops items
from `pending` under `ref_queue_mutex`, releases the mutex, and only then appends them to the
leader's `owned_items` (`CasRefLedger.cpp:1240-1265`). `owned_items.insert` can allocate and
throw; the leadership-exit guard then completes only what `owned_items` already recorded — the
just-carved items are neither pending nor owned and their waiters block forever.

Fix: make the pending→owned transfer non-throwing and atomic — reserve `owned_items` capacity
before popping anything (capacity ≥ current size + carve cap makes the later inserts no-throw), or
restructure the two containers so transfer is a splice. Add a test that injects an allocation
failure between pop and ownership publication and asserts every carved item still completes.

### 3. Budget: counts only, with exact edge semantics {#budget}

Replaces the byte-estimate admission that review round 1 falsified, with the round-2 corrections:

- `ref_txn_max_ops` **1000 → 5000**; the carve item cap (`kMaxRefBatch`) **128 → 1000**. The
  validation loop counts ops per admitted item (`build_ops` result size — exact by construction).
- **Spill only when the flush is non-empty:** if admitting the next item's ops would exceed the op
  cap and the current flush already has survivors, the remaining carved items are given back to
  the head of `pending` in order and struck from `owned_items` ("carved minus given back" is the
  guard's obligation). The flush proceeds with what fit; the next flush takes the rest.
- **A single normal item whose own op count exceeds the cap fails alone** — completed with an
  error like any per-item validation failure, neighbors unaffected. (Give-back would re-carve it
  forever: the empty-flush livelock review round 2 called out.)
- **Removal-class is exempt.** A `dropNamespace` `WholeShard` item legitimately builds one op per
  committed ref plus `RemoveNamespace` (`CasRefLedger.cpp:2151-2182`), far above any op cap; it is
  already carved as a singleton and budgeted by `ref_removal_max_bytes` = 64 MiB. The op-count cap
  applies to normal-class items only, and `checkBudget` keeps the byte limit for removal-class.
- `ref_txn_max_bytes` (1 MiB) for the normal class is **removed** encode- and decode-side; the
  64 MiB `RefLog` object read cap (`CasFormat.cpp:77`) remains the decode-side bound.
- **Per-op size cap 4 KiB** on normal-class ops, enforced exactly per op at admission, failing the
  op's item alone. This is a fail-closed guard, **not** claimed unreachable: `checkCanonicalRefName`
  imposes no length limit and part names grow with partition-key values
  (`MergeTreePartition.cpp:272`), so a pathological ref name can exceed any constant. Worst-case
  normal transaction stays ≤ 5000 × 4 KiB = 20 MiB, under the 64 MiB read cap. A test covers a
  maximum-length ref name at the cap boundary.

### 4. `RefOp::payload` removal {#payload-removal}

Production never populates the opaque `payload` string; the promote-time op and `updateRefPayload`
carry only `published_at_ms`. Per the pre-release no-compat policy:

- Remove `payload` from `RefOp` (`CasRefLogFormat.h`), `RefPayloadUpdate` (`CasRefProtocol.h:111`),
  the snapshot committed row (`CasRefSnapshotFormat.h:41`), and both codecs.
- Rename `SetPayload` → `SetPublishedAt` (wire word `set_payload` → `set_published_at`),
  `RefPayloadUpdate` → `RefPublishedAtUpdate`, `updateRefPayload` → `updateRefPublishedAt`.
- Full consumer inventory (review round 2): `RefTableState::applySetPayload`
  (`CasRefProtocol.cpp`), `CasInspect` (renders payload size and the op kind —
  `CasInspect.cpp:137,185`), fsck/codec round-trip tests, encoding-pin tests, benchmarks, and any
  test using `payload` as a generic byte carrier (rewritten against real fields or dropped).
- **Operational requirement, stated explicitly:** wire-word and field removal are safe only
  because every existing pool is recreated before this ships (pre-release, no persisted data —
  the standing CA policy). No decoder tolerance for the old field is added.
- Non-goal (future): folding `published_at_ms` into `OwnerTransition` to drop one op per promote.

### 5. Recovery: streaming replay with candidate discipline {#recovery-streaming}

Motivated by the op-cap bump (worst-case normal transaction grows to 20 MiB) and a tail that can
be long when snapshot publication is failing. Today recovery materializes every post-snapshot
decoded `RefLogTxn` in a vector before replay (`CasRefLedger.cpp:404-423`), then installs the
result only after the whole tail was fetched — all-or-restart.

Change: stream — sort the (small) object keys for txn-id order, then GET → decode → apply → discard
one transaction at a time, **into a private candidate `RefTableState`**, never into the live
`rt.state`. On any vanished-object or decode failure the candidate is discarded and recovery
restarts with a fresh LIST — exactly today's all-or-restart discipline, now with O(one transaction)
memory. The candidate, snapshot/tail counters, and `recovered` flag publish together under
`state_mutex`, as today. The stale 1 MiB budget comment near `CasFormat.cpp:80` is updated.

## Settings {#settings}

- One new server-level setting: the CAS upload pool size (fail-loud: `0`/uninitialized rejected).
- No behavior change for non-CAS disks or engines (nothing outside the CAS tree is touched).

## Testing {#testing}

Unit (CA gtest gate `Cas*:CA*` — mind the filter-gap lesson):

1. **`deps` equivalence** — a multi-blob part uploaded via the `uploadBlobDetached` fan-out ends
   with the same `build` state as the serial `putBlob` path, across branches: dedup-cache hit,
   HEAD-first hit, HEAD-first miss, local staging, S3-native staging (with_rustfs), and a
   mid-upload backend failure.
2. **Pool saturation** — pool size 1 and pool size N with blobs > N: completes within a bounded
   watchdog latch (not a CI-timeout oracle); a self-wait deadlock fails fast.
3. **Drain-precedes-unwind** — one failing blob task plus one deliberately slow blob task: the
   part's publish failure is raised only after the join; no `sleep`-based sequencing (latches).
4. **Capture lifetime** — a throw during task dispatch (pool queue full / injected) drains
   already-running tasks before unwind; ThreadSanitizer-clean.
5. **Carve transfer no-throw** — allocation-failure injection between queue pop and ownership
   publication: every carved item completes (with the flush result or its error), no waiter hangs.
6. **Count spill** — a set of concurrent items whose total ops exceed `ref_txn_max_ops` splits
   across flushes by give-back; every item commits; zero exceptions; the overflow item leads the
   next flush.
7. **Oversized item fails alone** — one item with > `ref_txn_max_ops` ops fails only itself
   (no empty-flush livelock); one op > 4 KiB (maximum-length ref name) fails only its item.
8. **Removal-class exemption** — a `dropNamespace` over > 5000 refs succeeds as a singleton
   `WholeShard` transaction under the 64 MiB removal byte budget.
9. **Payload removal pins** — encoding-pin tests: `set_published_at` wire word, no `payload` key
   in ref-log or snapshot output; `CasInspect` renders the renamed op; codec round-trips.
10. **Recovery streaming** — a long tail of maximum-op-count transactions replays under a hard
    peak-memory bound that the old whole-tail implementation would exceed; a mid-tail vanished
    object discards the candidate and restarts cleanly (concurrent recovery waiter unblocked
    exactly once).

Integration / soak:

- CA battery (stateless + integration lanes) green; TXN/GC soak green (the soak exercises the
  bumped op cap through mutation storms and the recovery path through kill-restart cycles).
- Re-profile the 500-partition INSERT: expect the blob-upload segment of each part's commit to
  shrink (bounded win, ~2 blobs/part); `CasRefBatchFlushes`/`CasRefBatchedMutations` stays ~1.0
  **by design** in this stage — the batch-size collapse is stage 2's acceptance metric, not this
  stage's. State this explicitly so the profile is not misread as a stage-1 failure.

## Explicitly out of scope {#out-of-scope}

- **Stage 2:** concurrent `commitPart` dispatch in `ReplicatedMergeTreeSink::finishDelayed` (and
  the non-replicated sink) — separate brainstorm/spec; requires upstream-surface consultation.
  The emergent effects (ledger batch size ≫ 1, cross-part upload multiplexing on this stage's
  pool) belong to that design.
- Bulk partition operations (backlog known-issue `cc9a8e63401`).
- The HEAD-before-PUT dedup gate and the unconditional promote manifest GET (separate backlog
  items).
- Any deterministic batch seam below the sink (`commitTransactions`, phased engine, ledger
  submission groups) — abandoned with v1/v2.

## Reused prior work {#prior-work}

Tasks 1-3 of the parallel-write-path plan remain landed and load-bearing (ref-lane
leadership-exit exception safety — extended here by §2 and §3's give-back accounting —
`CommitOutcome` + `dropRefIfMatches`, ordered-vector + preallocated-slot commit structure).
Tasks 4-5 stay reverted; the dedicated pool returns in §1 for blob uploads only.
