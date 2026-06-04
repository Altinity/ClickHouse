# CA GC Convergence — S1: Reverse Index Becomes Real (instrumentation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). Build to a log (`ninja -C build … > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Tests FOREGROUND, bounded (`timeout` ≤ 590), non-empty `--test`, never `clickhouse local`, never background a build/test.

**Goal:** Wire the existing `InMemoryBlobRefIndex` (B9 seam) as an incremental reverse index in the CA GC: `commit` adds a part's blob pins, `drop`/unlink removes them, and `runSweepOnce` **validates** the index against the authoritative full-scan result and **logs drift** — with **NO change to deletion behavior** (the full scan stays authoritative). This is S1 of the GC convergence spec: instrumentation only; safety still rests on the unchanged `gc_lock` + fence lease.

**Architecture:** Per-pool `InMemoryBlobRefIndex` instance owned by the metadata storage (alongside `gc_lock`). `commit` (after publishing a ref) calls `index.addPart(part_id, manifest)`; ref removal calls `index.removePart(...)`. In `runSweepOnce`, after the authoritative `markReachableBlobs`, compute the index's reachable view and log any disagreement as a drift metric/log line — never gating a delete on it. The index is per-process (sees only this node's commits since startup), so production drift vs the bucket-wide scan is **expected and informational**; the **gtest** (single-node-from-empty: index ≡ scan) is the real correctness check the spec calls for ("the validator must never disagree").

**Spec:** `docs/superpowers/specs/2026-06-04-ca-gc-convergence-design.md` (§10 S1, §11 S1 test, §5 end-state). **Branch:** `cas-mergetree-poc`. Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- `IBlobRefIndex`/`InMemoryBlobRefIndex` (`ContentAddressedGC.h:116-145`): `addPart(part_id, manifest)`, `removePart(part_id, manifest)`, `refcount(blob_hash)`, `unreferenced()`; `InMemoryBlobRefIndex` has `counts` (BlobHash→int64) + `applied_parts` (idempotency). Un-wired ("B9").
- Sweep: `ContentAddressedGC::runSweepOnce` (`ContentAddressedGC.cpp:284`), under `gc_lock` (`:293`); authoritative `listLivePartIds` (`:88`/`323`) + `markReachableBlobs` (`:126`/`327`); deletes `to_remove` (`:442`).
- Commit publish point: `ContentAddressedTransaction::commit` / `commitOnePart` (publishes the ref; the `manifest` + `part_id` are in scope there). Ref removal: `unlinkPartDirRefs` / `removeRecursive` part-dir branch (`ContentAddressedTransaction.cpp`).
- The metadata storage owns `gc_lock` (`ContentAddressedMetadataStorage`) — the natural owner for the index too.

---

## Phase 1 — wire the index + validator

### Task 1: own an `InMemoryBlobRefIndex` per pool + thread-safety

**Files:** Modify `…/ContentAddressed/ContentAddressedGC.h`, `…/ContentAddressedMetadataStorage.h`, `…/ContentAddressedMetadataStorage.cpp`

- [ ] **Step 1:** make `InMemoryBlobRefIndex` thread-safe — add a `mutable std::mutex mtx;` and lock in `addPart`/`removePart`/`refcount`/`unreferenced` (commits are concurrent). Keep the `applied_parts` idempotency guard.
- [ ] **Step 2:** give `ContentAddressedMetadataStorage` a `std::shared_ptr<InMemoryBlobRefIndex> blob_ref_index` member, constructed in its ctor (one per pool, alongside `gc_lock`). Expose a getter `blobRefIndex()`.
- [ ] **Step 3: build** `ninja -C build clickhouse > build/gcs1_t1_build.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/gcs1_t1_build.log` → 0.
- [ ] **Step 4: commit** `CA GC S1: own a thread-safe InMemoryBlobRefIndex per pool`.

### Task 2: update the index on commit / drop (no behavior change)

**Files:** Modify `…/ContentAddressedTransaction.cpp`

- [ ] **Step 1:** in `commitOnePart` (the whole-part publish branch — where a new content manifest+ref is published), after the ref is published, call `metadata_storage.blobRefIndex()->addPart(part_id, manifest)`. Do it ONLY for the content-publish branch (a mutable-only sidecar update changes no blob pins). Idempotent via `applied_parts`. (The shadow/FREEZE ref and the detached ref also pin blobs — for S1 keep scope to the live-ref content publish; note shadow/detached as a known under-count in the drift log, acceptable for instrumentation.)
- [ ] **Step 2:** in the ref-removal path (`unlinkPartDirRefs` / the `removeRecursive` regular-part-dir branch) — where a live part's ref is unlinked — load the part's manifest (the `part_id` is resolvable from the ref before deletion, or pass it through) and call `removePart(part_id, manifest)`. If resolving the manifest at unlink time is awkward, record the (part_id, manifest) at the same point the ref is read for deletion. Keep it best-effort: a failure to update the index must NOT block the drop (log + continue) — the scan remains authoritative.
- [ ] **Step 2a:** CAUTION — do not let an index-update exception escape the commit/drop path. Wrap index calls so any error is logged and swallowed (instrumentation must never break the data path).
- [ ] **Step 3: build** → 0 errors.
- [ ] **Step 4: commit** `CA GC S1: update the reverse index on commit/drop (best-effort, instrumentation)`.

### Task 3: validator in the sweep (log drift, never gate)

**Files:** Modify `…/ContentAddressedGC.cpp`

- [ ] **Step 1:** in `runSweepOnce`, after the authoritative `reachable_blobs` is computed (and before/independent of the actual delete decision), compute the index's reachable set: `{ blobKey(prefix, H) : index.refcount(H) > 0 }`. Compare to the authoritative `reachable_blobs`. Log a single summary line at INFO (or a debug counter): `cas_gc_index_drift{missing_in_index=…, extra_in_index=…}` — sets present in one but not the other. **Do NOT change `to_remove` or any deletion** — the scan result is authoritative; the index view is logged only.
- [ ] **Step 2:** add a comment: in a multi-node pool (or a process that started after parts existed) the index is expected to under-count (it only saw this process's commits since startup); the drift log is informational. The gtest (Task 4) validates exact agreement in the controlled single-node-from-empty case.
- [ ] **Step 3: build** → 0 errors.
- [ ] **Step 4: commit** `CA GC S1: sweep validates the reverse index against the authoritative scan (drift log)`.

### Task 4: gtest — refcount ≡ scan (single-node, from empty)

**Files:** Modify `src/Disks/tests/gtest_content_addressed_metadata.cpp` (or `gtest_content_addressed.cpp` where the GC reachability tests live — grep `markReachableBlobs`/`listLivePartIds`)

- [ ] **Step 1:** a test that, on a fresh pool: commits several parts (some sharing blobs — dedup), drops some; after each operation asserts the `InMemoryBlobRefIndex`'s reachable set (`refcount(H)>0`) **exactly equals** the authoritative `markReachableBlobs(listLivePartIds(...))` set. Include: shared-blob dedup (two parts pin the same H → refcount 2 → drop one → still reachable → drop both → unreferenced), and idempotent re-add (applied_parts). This is the spec's S1 gate: "the validator must never disagree."
- [ ] **Step 2: build + run** `ninja -C build unit_tests_dbms > build/gcs1_t4_build.log 2>&1; echo $?; build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' > build/gcs1_t4_run.log 2>&1; echo $?; tail -20 build/gcs1_t4_run.log` → all pass (the prior 122 + the new one).
- [ ] **Step 3: commit** `CA GC S1: gtest — reverse index ≡ authoritative scan (single-node)`.

---

## Phase 2 — regression + finalize

### Task 5: CA regression + push

- [ ] **Step 1:** the change is instrumentation-only (no deletion behavior change), so the CA suite must stay green. Run a CA-default smoke: `04278_content_addressed_disk 04279_content_addressed_gc 04280_content_addressed_clone_partition_works 04292_content_addressed_mutations 05003_content_addressed_freeze 05004_content_addressed_transactions` (foreground, `timeout 590`, non-empty `--test`) → all pass. Especially `04279_content_addressed_gc` (the GC test) must be green (the sweep still deletes exactly as before — the index is observational).
- [ ] **Step 2:** 122+ `ContentAddressed*` gtests green.
- [ ] **Step 3: backlog** — `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: B9 / GC-S1 DONE — `InMemoryBlobRefIndex` wired as an incremental reverse index validated against the authoritative scan (instrumentation; scan still authoritative; `gc_lock` unchanged). Note it is transitional (S2's streaming compaction becomes the authoritative candidate source; the in-memory index downgrades to an optional pre-filter then).
- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- `InMemoryBlobRefIndex` is wired (commit adds, drop removes, thread-safe, best-effort) and the sweep logs index-vs-scan drift without changing any deletion.
- gtest proves index ≡ authoritative scan on a single-node-from-empty pool (incl. shared-blob dedup + idempotency).
- CA suite + 122 gtests stay green (instrumentation-only — `04279_content_addressed_gc` unchanged).
- Backlog notes S1 done + transitional. No `gc_lock` / behavior change (that's S2–S4).
