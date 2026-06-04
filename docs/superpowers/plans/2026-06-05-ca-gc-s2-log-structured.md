# CA GC Convergence — S2: Log-Structured Streaming GC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build … > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Tests FOREGROUND, bounded (`timeout` ≤ 590), non-empty `--test`, never `clickhouse local`, never background a build/test.

**Goal:** Replace the GC's full `parts/`+`blobs/` `LIST`-and-re-mark scan with a **log-structured, streaming compaction** (spec §5). Commit/drop append `event_id`-keyed `+`/`-` delta objects (with the resolved blob pins **and** the `(part_id) edge`) under `gc/log/<epoch>.<shard>/`, coalesced into group-commit batches. The fenced GC leader, per shard, **closes the epoch (plain fenced PUT, no CAS) before folding** (§5.1 rule 1), then streaming merge-joins the epoch's sorted deltas against the sorted snapshot run `gc/snap/<padded-epoch>.<shard>`, dedup-on-fold by `event_id`, writes the new snapshot as it streams, and **emits any key whose running count reaches 0 as a candidate in the same pass**. This **retires the bucket-wide scan (G3)**: candidates fall out of the compaction. Safety still rests on the **unchanged `gc_lock` + fence lease** — S2 does not drop the lock and does not add generations/tombstones (those are S3/S4). The full scan survives only as the rare reconciliation fallback (§9).

**Architecture:** A new per-pool, per-shard log+snapshot subsystem layered onto the existing `ContentAddressedGC` + `PoolCoordination`. The write path (`ContentAddressedTransaction::commit` / `commitOnePart`, and the unlink/`removeRecursive` path) enqueues coalesced deltas to the shard's open epoch instead of (only) updating the in-memory index. `runSweepOnce` becomes a per-shard compaction: it no longer calls `listLivePartIds`/`markReachableBlobs` on the normal path; instead it folds `gc/snap` + `gc/log` for the shard under the fence. Sharding is by hash-prefix (`H0`), keyed into the layout from day one; the first impl runs one worker across all shards (parallel-per-shard is a later config change, §5). The S1 `InMemoryBlobRefIndex` becomes an optional per-epoch pre-filter hint — the snapshot+log is now the authoritative reverse index (spec §10 "the in-memory index downgrades").

**Spec:** `docs/superpowers/specs/2026-06-04-ca-gc-convergence-design.md` (§4 layout, §5 log-structured GC, §5.1 epoch protocol, §8 I1/I6, §9 rebuild/failures, §11 S2 tests, §12.3 op budgets). **Branch:** `cas-mergetree-poc`. Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- `ContentAddressedGC::runSweepOnce` (`ContentAddressedGC.cpp:301`) under `gc_lock` (`:310`); current authoritative `listLivePartIds` (`:89`) + `markReachableBlobs` (`:127`) + `firstUnreachable`/`grace` ageing (`:214`) + the delete of `to_remove` (~`:442`). S2 replaces the candidate source; the delete primitive (`removeObjectsIfExist`) and `grace` are unchanged.
- `PoolCoordination` (`PoolCoordination.{h,cpp}`): `condCreateIfAbsent` (`:234`), `allocateFenceToken`, `GcLock` + `takeLeadership`/`renewLease`/`releaseLock`, the `fence/<n>` objects, and the `leadership_lost`/fence-still-mine gate. The **fenced PUT for epoch-close** reuses this single-writer-per-shard guarantee (no `If-Match` needed — §5.1 rule 1).
- `PoolPaths.{h,cpp}`: key builders (`blobKey`, `partsPrefix`, `blobsPrefix`, `refsRootPrefix`, `sessionsPrefix`, `fencePrefix`). S2 adds `gcLogPrefix(shard, epoch)`, `gcLogEventKey(shard, epoch, event_id)`, `gcSnapKey(shard, padded_epoch)`, `gcCurrentEpochKey(shard)`, and a `shardForHash(BlobHash)` helper (prefix → shard).
- Commit publish point: `ContentAddressedTransaction::commit`/`commitOnePart` (`part_id` + `manifest` in scope). Unlink: `unlinkPartDirRefs` / `removeRecursive` part-dir branch.
- `PartManifest` (`PartManifest.{h,cpp}`): the manifest's `BlobEntry` list resolves a part's `(H)` pins; the delta `+` carries those plus the `(part_id) edge` (§9).
- S1 index: `InMemoryBlobRefIndex` (`ContentAddressedGC.h:116-145`) — kept as an optional pre-filter, no longer authoritative.

**RISK:** S2 does **not** change deletion or locking semantics (the `gc_lock` + fence + `grace` still gate every delete) — it changes the **candidate source**. The danger is an **under-count** that drops a live blob from the candidate stream's complement: if a delta is lost or the fold is wrong, the compaction can emit a still-referenced `(H)` as a count-0 candidate. S2 mitigates this by keeping the full-scan reconciliation as the safety net and by the op-count + correctness oracles below; the lock is still held, so the §7 handshake race is NOT yet exposed. The lock-removal (S4) is what makes log-completeness load-bearing (§5.1) — S2 builds the epoch-close + re-append + dedup machinery so S4 can rely on it, and tests it under the lock where the `LIST` is still stable.

---

## Phase 1 — log + snapshot layout and the delta write path

### Task 1: layout keys + shard helper

**Files:** Modify `…/ContentAddressed/PoolPaths.{h,cpp}`

- [ ] **Step 1:** add the §4 GC keys: `gcCurrentEpochKey(prefix, shard)` → `<prefix>/gc/current_epoch/<shard>`; `gcLogPrefix(prefix, epoch, shard)` → `<prefix>/gc/log/<epoch>.<shard>/`; `gcLogEventKey(prefix, epoch, shard, event_id)` → `…/<event_id>`; `gcSnapKey(prefix, padded_epoch, shard)` → `<prefix>/gc/snap/<padded-epoch>.<shard>` (zero-pad the epoch so lexical `LIST` order == numeric order). Add `shardForHash(BlobHash) -> ShardId` deriving the shard from the hash prefix (e.g. top bits of `H0`); make the shard count a single named constant (`kGcShardCount`) so "1 worker over N shards" → "N workers" is a config flip, not a layout change.
- [ ] **Step 2:** typed wrappers consistent with B29 — return `GcLogObjectKey`/`GcSnapObjectKey` typed keys, not bare `std::string`, so a log key can never be confused with a blob/ref key.
- [ ] **Step 3: build** `ninja -C build clickhouse > build/gcs2_t1_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/gcs2_t1_build.log` → 0.
- [ ] **Step 4: commit** `CA GC S2: gc/log + gc/snap + per-shard epoch layout keys`.

### Task 2: the delta record + codec (`event_id`-keyed, `(part_id) edge`)

**Files:** New `…/ContentAddressed/GcDelta.{h,cpp}` (modelled on `Codec.h` / `PartManifest` serialization)

- [ ] **Step 1:** define `GcDelta` = `{ op: +/-, event_id: stable u128, part_id: PartId, pins: vector<BlobHash> }`. `event_id` is a **stable** id per `(part_id × op)` (so a re-append in S2 is collapsible) — derive it deterministically from `(part_id, op)` (and a generation discriminator reserved for S3, default 0). Serialize on the shared LE/varint codec (`Codec.h`), `MAGIC+version`, fail-closed on bad magic/unknown version (B19/B28 discipline).
- [ ] **Step 2:** the `+` records the `(part_id) edge` alongside the blob pins, so the compaction counts **manifest references** the same way as blob references (§9: a manifest generation whose count hits 0 is also a candidate — S2 counts `part_id` as a key in the same merge; the generation lifecycle is S3).
- [ ] **Step 3: build** → 0 errors.
- [ ] **Step 4: commit** `CA GC S2: GcDelta record + LE codec (event_id-keyed, part_id edge)`.

### Task 3: coalesced delta append on commit / drop (group-commit batching)

**Files:** New `…/ContentAddressed/GcLogWriter.{h,cpp}`; modify `…/ContentAddressedTransaction.cpp`, `…/ContentAddressedMetadataStorage.{h,cpp}`

- [ ] **Step 1:** `GcLogWriter` owned per pool by the metadata storage. It buffers deltas in-memory grouped by `(shard, open-epoch)`, and on a short time/size window flushes **one object per `(shard, window)`** to `gcLogEventKey(...)` — group-commit, not one object per commit (§5 "batching is a requirement"). One coalesced object holds multiple deltas; `cas_log_batch_size` counts deltas per object.
- [ ] **Step 2:** `commitOnePart` (content-publish branch) enqueues a `+` delta (`event_id`, `part_id`, resolved `pins`) **before** writing the live ref — preserving I1/I6 (`+` before ref). For S2 (lock held) this is synchronous-enough; the async/batched + session-covers-the-gap discipline is S4. Drop/unlink enqueues a `-` delta **after** removing the ref (§7.1 unlink ordering, bias to over-count).
- [ ] **Step 3:** **writer epoch read + re-append (§5.1 rule 2).** Before enqueuing, read `gcCurrentEpochKey(shard)` (default 0 if absent) and stamp the delta with that epoch. After flush, re-read the shard epoch; if it advanced past the one written, **re-append the same logical delta (same `event_id`) into the now-open epoch** (bounded retry). The orphaned append in the closed epoch is a harmless leaked object (reconciled later; deduped by `event_id`). NOTE: while `gc_lock` is held (S2/S3) the close cannot race a commit, so this path is exercised but rarely taken — it is wired now so S4 can rely on it.
- [ ] **Step 4:** keep the S1 `InMemoryBlobRefIndex` update too (now the optional pre-filter, not authoritative).
- [ ] **Step 5: build** → 0 errors.
- [ ] **Step 6: commit** `CA GC S2: coalesced gc/log delta append on commit/drop + writer epoch re-append`.

---

## Phase 2 — streaming compaction (the new candidate source)

### Task 4: external sorted-merge compaction with epoch-close-before-fold

**Files:** New `…/ContentAddressed/GcCompaction.{h,cpp}`; modify `…/ContentAddressedGC.{h,cpp}`

- [ ] **Step 1:** per shard, under the fence (`leadership_lost`/fence-still-mine gate, reused from `PoolCoordination`): **(a) close the epoch** — plain fenced PUT `gcCurrentEpochKey(shard) = E+1` (single-writer-per-shard, no CAS — §5.1 rule 1). Only after the close, **(b)** `LIST gcLogPrefix(E, shard)`, read all delta objects, **sort the deltas by `(key)`** where key is `(H)` for blob pins and the `(part_id)` edge (external sort — spill to scratch if the frontier is large; memory bounded by the merge frontier, not blob count).
- [ ] **Step 2:** **streaming merge-join** the sorted deltas against the sorted snapshot run `gcSnapKey(E, shard)`: walk both inputs in lockstep, **dedup by `event_id`** while folding (a duplicate from a re-append collapses to one count — §5.1 idempotent deltas), sum counts per key, **stream the new snapshot `gcSnapKey(E+1, shard)` as you go**, and **emit any key whose running count reaches 0 as a candidate** in the same pass. Candidates fall out of the merge; there is no separate decrement-to-zero queue.
- [ ] **Step 3:** advance: after writing `gcSnapKey(E+1, shard)`, reclaim the old `gcSnapKey(E, shard)` + `gcLogPrefix(E, shard)` objects (the epoch was already closed in step 1). Keep this idempotent under a leader crash (seals/PUTs are idempotent; a half-advanced epoch re-runs cleanly).
- [ ] **Step 4: build** → 0 errors.
- [ ] **Step 5: commit** `CA GC S2: streaming epoch compaction (fenced close-before-fold, dedup-on-fold, candidate-from-merge)`.

### Task 5: rewire `runSweepOnce` to consume compaction candidates (retire the scan — G3)

**Files:** Modify `…/ContentAddressedGC.cpp`

- [ ] **Step 1:** on the **normal** path, `runSweepOnce` (per shard) calls `GcCompaction` and uses its **count-0 candidate stream** as `to_remove` candidates — **no `listLivePartIds` / `markReachableBlobs` / `LIST blobs/`** (G3). Keep the existing `grace`/`firstUnreachable` ageing and the existing delete primitive unchanged — only the candidate source changed. The `gc_lock` is still held (S2): the lock keeps the `LIST gc/log` stable, so the fold sees a complete epoch.
- [ ] **Step 2:** keep the full `parts/`+`blobs/` scan as an **explicit reconciliation entry point** (`runReconciliationScan`) — the rare fallback for rebuild / abandoned-upload / orphan-drift (§9), NOT the normal path. Schedule it by the bounded policy stub (cadence + orphan-byte threshold, §9 "orphan-drift bound") — leave the policy as a configurable knob; the heavy scan is last resort.
- [ ] **Step 3:** keep the S1 drift validator running against the compaction result for one more stage (cheap cross-check that the in-memory pre-filter agrees with the folded snapshot); log `cas_gc_index_drift`.
- [ ] **Step 4: build** → 0 errors.
- [ ] **Step 5: commit** `CA GC S2: sweep consumes compaction candidates (retire normal-path full scan, G3)`.

### Task 6: rebuild-from-snapshot+log

**Files:** Modify `…/ContentAddressed/GcCompaction.{h,cpp}`

- [ ] **Step 1:** `rebuildFromSnapshotAndLog(shard)` recomputes counts from `gcSnapKey(latest)` + all un-folded `gcLogPrefix(epoch)` for the shard — **no blob `LIST`** (§9 rebuild/catch-up). Used on leader startup/catch-up. On total log+snap loss, fall back to `runReconciliationScan` (the heavy fallback rebinds content reachability from live refs+manifests — spec §13 "gc/log truth level").
- [ ] **Step 2: build** → 0 errors.
- [ ] **Step 3: commit** `CA GC S2: rebuild reverse counts from snapshot + log (no blob scan)`.

---

## Phase 3 — gtests, op-budgets, regression, finalize

### Task 7: per-stage gtests + race oracle (§11 S2)

**Files:** Modify `src/Disks/tests/gtest_content_addressed*.cpp` (where `markReachableBlobs`/`listLivePartIds` GC tests live)

- [ ] **Step 1 — streaming-merge correctness:** seed a sorted snapshot + a set of sorted deltas (mixed `+`/`-`, shared blobs, multiple parts), fold, assert the new snapshot's counts == hand-computed and that exactly the count-0 keys are emitted as candidates. Include the `(part_id)` manifest edge reaching 0.
- [ ] **Step 2 — rebuild:** build a snapshot+log, drop the in-memory state, `rebuildFromSnapshotAndLog`, assert identical counts (no blob `LIST` issued — assert the op counter).
- [ ] **Step 3 — epoch fold/compaction + advance:** fold E, assert `current_epoch == E+1`, old `gc/snap`/`gc/log` reclaimed, new snapshot present; re-run is idempotent.
- [ ] **Step 4 — shard isolation:** deltas in shard A never appear in shard B's fold; closing A's epoch does not force a B writer to re-append (per-shard epochs, §5.1 rule 1).
- [ ] **Step 5 — append-as-epoch-folds oracle (§5.1, deterministic, NO sleeps):** drive a writer that appends a `+` for epoch E exactly as the compaction closes E and advances to E+1; assert the writer **re-appends** into E+1 (rule 2) and that the folded snapshot ∪ open-epoch deltas still counts the reference (no under-count → the blob is never a candidate while the part is live). This is the S2 dress-rehearsal of the S4 load-bearing oracle (here the lock still serializes, so it must pass trivially; it proves the re-append + dedup machinery before S4 removes the lock).
- [ ] **Step 6 — duplicate `+` dedup-on-fold:** a re-append lands the same `event_id` in two epochs; assert the fold counts it once (not an over-count).
- [ ] **Step 7: build + run** `ninja -C build unit_tests_dbms > build/gcs2_t7_build.log 2>&1; echo $?; build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcs2_t7_run.log 2>&1; echo $?; tail -20 build/gcs2_t7_run.log` → all pass.
- [ ] **Step 8: commit** `CA GC S2: gtests — merge/rebuild/fold/shard-isolation + append-as-epoch-folds + dedup oracles`.

### Task 8: op-count budgets (§11/§12.3) — assert, don't hope

**Files:** Modify the GC/transaction code to expose deterministic counters; modify the gtest

- [ ] **Step 1:** add deterministic counters `cas_log_batch_size` (deltas per coalesced object), `cas_gc_log_list_ops` (per-epoch `LIST gc/log`), `cas_gc_blob_list_ops` (must be **0** on the normal compaction path — the G3 budget), and `cas_s3_control_ops_per_commit`.
- [ ] **Step 2:** assert in a gtest: a normal compaction issues **0** `blobs/` `LIST`s (G3); a burst of N commits coalesces into ⌈N/window⌉ log objects, not N (batching budget). A commit's synchronous control ops stay within the §12.3 budget (the S3-only floor target; full async-+ and the 1-PUT Keeper floor are S4).
- [ ] **Step 3: build + run** → counters assert green.
- [ ] **Step 4: commit** `CA GC S2: op-count budget counters + asserts (0 blob LISTs on normal path; batched log)`.

### Task 9: CA regression + backlog + push

- [ ] **Step 1:** CA-default smoke (foreground, `timeout 590`, non-empty `--test`): `04278_content_addressed_disk 04279_content_addressed_gc 04280_content_addressed_clone_partition_works 04292_content_addressed_mutations 05003_content_addressed_freeze 05004_content_addressed_transactions` → all pass. `04279_content_addressed_gc` MUST be green (reclamation still works, now compaction-driven). Run a subagent to summarize the log.
- [ ] **Step 2:** all `ContentAddressed*` gtests green.
- [ ] **Step 3: backlog** — `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: append a "CA GC S2 DONE" note under the S1 section: log-structured streaming compaction is the authoritative candidate source; normal-path full scan retired (G3); full scan survives as reconciliation fallback; in-memory index downgraded to pre-filter; epoch-close-before-fold + re-append + dedup wired (under the still-held lock) so S4 can rely on log-completeness. Reference the spec + this plan.
- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- Commit/drop append `event_id`-keyed, coalesced `+`/`-` deltas (blob pins + `part_id` edge) under `gc/log/<epoch>.<shard>/`; writer reads the shard epoch and re-appends on advance.
- The fenced leader closes the epoch (plain fenced PUT, no CAS) **before** folding, then streaming merge-joins `gc/snap` + `gc/log` with dedup-on-fold, writes the new snapshot, and emits count-0 keys as candidates in one pass.
- `runSweepOnce` consumes compaction candidates on the normal path — **0** `blobs/` `LIST`s (G3 retired); the full scan remains only as the explicit reconciliation fallback. `grace`, the delete primitive, and `gc_lock` are unchanged (no semantic change — S2 is candidate-source-only).
- Rebuild recomputes counts from snapshot+log without scanning blobs.
- gtests cover streaming-merge correctness, rebuild, epoch fold/compaction+advance, shard isolation, append-as-epoch-folds, and duplicate-`+` dedup; op-count budgets assert 0 blob `LIST`s and batched log objects.
- CA suite + all `ContentAddressed*` gtests green. Backlog notes S2 done. No generations/tombstones, no lock removal (S3/S4).
