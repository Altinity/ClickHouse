---
description: M1 Phase 4 — reachability garbage collection for the content-addressed pool (reclaim unreferenced blobs/footers after grace).
sidebar_label: 'CAS MergeTree M4 plan'
sidebar_position: 4
slug: /superpowers/plans/cas-mergetree-m4
title: 'Content-Addressed MergeTree M1 — Phase 4 Plan (garbage collection)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Phase 4 Plan (garbage collection) {#cas-mergetree-m4-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development`. Steps use checkbox (`- [ ]`).

**Goal:** Reclaim unreferenced content-addressed objects. Today removal only unlinks refs (Phase-3.5 `removeRecursive`); `blobs/<checksum>` and `parts/<part_id>` footers of dropped/merged-away parts **leak forever**. Phase 4 adds a single-process background **reachability sweeper** that deletes blobs/footers that have been unreachable for longer than a grace period.

**Architecture:** A per-pool background thread (one `content_addressed` disk = one pool = one sweeper, mirroring `BlobKillerThread`) periodically: (1) enumerates the **live refs** in the pool → resolves each to its `part_id` (the live part set); (2) marks the reachable footers (= the live `part_id`s) and reachable blobs (Phase-1 `markReachableBlobs`, which reads each live footer); (3) lists `parts/` and `blobs/`, computes the **unreferenced** set = listed − reachable; (4) feeds it to Phase-1 `selectForSweep` with **grace measured from loss-of-reachability**, and deletes only objects unreachable past `grace`. M1 is **single-process** (no GC coordinator — B11) and **reachability-recompute** (ground truth from object storage, robust across restarts) — NOT delta-refcount-driven; the `BlobRefIndex` is kept as a fed-but-not-load-bearing cache (the B9 delta/sharded path plugs in there later).

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), Phase-1 `Reachability` (`markReachableBlobs`/`selectForSweep`) + `Footer` + `PoolPaths`, `IObjectStorage::listObjects`/`removeObjectsIfExist`, `BackgroundSchedulePoolTaskHolder` (mirror `src/Disks/DiskObjectStorage/Replication/BlobKillerThread.{h,cpp}`).

**Source spec:** `docs/superpowers/specs/content_addressed_shared_mergetree_design.md` (§5 reachability GC, grace-from-loss-of-reachability) + the PoC `poc/cas_mergetree/` GC scenarios (`test_lifecycle_then_gc`, `test_gc_keeps_carry_forward`, `test_gc_never_deletes_reachable`). Phases 1–3.6 are committed + pushed on `cas-mergetree-poc`.

---

## GC safety invariants (READ BEFORE CODING — getting these wrong = data loss) {#safety}

1. **Reachability from LIVE REFS only.** Roots are the published refs (`store/<server>/<uuid>/refs/<part>`). A live ref → `part_id` → footer → blob keys. An orphaned footer (no ref) is itself unreachable; its blobs are reachable only if **another live footer** also references them (dedup). `markReachableBlobs(live_part_ids, resolve)` already encodes this — DO NOT mark blobs reachable through orphaned footers.
2. **Grace is measured from first loss of reachability, NOT object age** (Phase-1 `selectForSweep`). The sweeper MUST persist the `first_unreachable` map across sweeps (in-memory across the process lifetime is fine for M1). Reachable-again clears the timer.
3. **`grace` MUST exceed the maximum commit window.** The write path uploads a part's blobs and writes its footer BEFORE publishing the ref (build-local-then-upload, then `commit` publishes the ref). Between upload and ref-publish, those blobs+footer are **unreferenced but about to become reachable**. Grace-from-unreachability protects them only if `grace` > the longest possible insert/merge commit. Default `grace` conservatively (e.g. 3600s); never 0. **Commit-failure orphans** (footer/blobs written, ref never published, e.g. a crash mid-commit) are legitimate GC fodder — they get collected once unreferenced past grace.
4. **Fail-close on a missing footer (B18).** The `FooterResolver` passed to `markReachableBlobs` MUST throw if a LIVE ref's footer is missing — never return an empty `Footer` (that would drop the ref's blobs from the reachable set → data loss). On any resolver/enumeration error, the sweep MUST abort WITHOUT deleting anything (a partial reachable set must never drive deletion).
5. **Sweep scope = `blobs/` and `parts/` ONLY.** Refs, table-files (`files/`), and generic disk files are owned by the table and removed synchronously by `removeRecursive` — the sweeper never touches them (and never touches `frozen/`/`detached/` — B4/B12, deferred, would be additional roots).
6. **Single-process (M1).** One pool = one sweeper. Concurrent uncoordinated sweepers from two servers sharing a pool are UNSAFE; B11 (Keeper-coordinated GC) + the Phase-5 `_pool_meta` self-check guard that. Document the single-process assumption; do not add coordination here.

**Deferred (do NOT implement):** ephemeral reader pins for stateless/long reads (B3 — an M1 limitation: a read of a dropped part lasting longer than `grace` could lose its blobs), Keeper GC coordinator + lock + mark-epoch revalidation (B11), persisted/sharded `BlobRefIndex` + delta-driven sweep (B9), expedited/compliance delete bypassing grace (B14), `frozen/`/`detached/` roots (B4/B12).

## Build & test {#build}
`build/` configured. Build in BACKGROUND, redirect to a log, have a subagent summarize: `cmake --build build --target clickhouse unit_tests_dbms > build/cas_m4_build.log 2>&1`. Unit tests: `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'` (currently 26) + regression `--gtest_filter='*PlainRewritable*:*DiskObjectStorage*'` (66). Stateless via the `clickhouse-praktika-tests` skill: `python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test <name>` (binary symlinked at `ci/tmp/clickhouse`; read `ci/tmp/test_result.txt`). No `<...>` in `///` comments; Allman; `DB::Exception`.

## File structure {#file-structure}
```
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
  PoolScan.{h,cpp}            # NEW: enumerate live part_ids (from refs) + list parts/ and blobs/ keys
  ContentAddressedGC.{h,cpp}  # NEW: runSweepOnce(now, grace) — pure-ish core: reachable -> unreferenced -> selectForSweep -> delete; holds first_unreachable
  ContentAddressedGCThread.{h,cpp}  # NEW: BackgroundSchedulePool task (mirror BlobKillerThread) driving runSweepOnce on an interval
  ContentAddressedMetadataStorage.{h,cpp}  # MODIFY: own the GC thread; start in startup(), stop in shutdown()/dtor
src/Disks/tests/gtest_content_addressed_metadata.cpp  # MODIFY: GC unit tests
tests/queries/0_stateless/<NNNNN>_content_addressed_gc.sql  # NEW: DROP-then-reclaim functional test (short grace)
```

---

## Task 1: pool scan — live part set + object listing (TDD) {#task-1}
- [ ] Test (append to `gtest_content_addressed_metadata.cpp`): seed a pool over `LocalObjectStorage` with 2 refs (2 distinct `part_id`s), 2 footers, 3 blobs, plus 1 orphan footer + 1 orphan blob (no ref). Assert `listLivePartIds(object_storage, key_prefix)` returns exactly the 2 referenced `part_id`s; `listKeysUnder(object_storage, partsPrefix(key_prefix))` returns 3 footers; `listKeysUnder(object_storage, blobsPrefix(key_prefix))` returns 4 blobs.
- [ ] Implement `PoolScan`: `listLivePartIds` = `listObjects` under `store/` across all `<server>/<uuid>/refs/` prefixes (or a `refsRootPrefix(key_prefix)`), read each ref payload → `part_id` (reuse the ref-parse used by the read path; tolerate a version byte per B22(c)). `listKeysUnder(prefix)` wraps `object_storage->listObjects`. Add `partsPrefix`/`blobsPrefix`/`refsRootPrefix` helpers to `PoolPaths` if missing. Fail-close: any list/read error propagates (caller aborts the sweep).
- [ ] Build → green; commit `"CAS M1 P4: pool scan (live part ids + object listing)"`.

## Task 2: GC sweep core `runSweepOnce` (TDD) {#task-2}
- [ ] Test: using the Task-1 seeded pool + `first_unreachable={}`:
  - `runSweepOnce(now=0, grace=100)` deletes NOTHING (orphans just became unreachable) and records `first_unreachable` for the orphan footer + orphan blob.
  - `runSweepOnce(now=50, grace=100)` still deletes nothing.
  - `runSweepOnce(now=200, grace=100)` deletes exactly the orphan footer + orphan blob; the 2 live footers + 3 referenced blobs remain.
  - **Carry-forward/dedup test:** a blob referenced by TWO live parts stays; after one part's ref is unlinked it is STILL referenced by the other → never enters the unreferenced set.
  - **Reachable-again test:** an object unreferenced at t0 then referenced again (e.g. a re-inserted identical part) has its `first_unreachable` cleared and is never deleted.
  - **Fail-close test:** a LIVE ref whose footer object is missing makes `runSweepOnce` THROW and delete nothing.
- [ ] Implement `ContentAddressedGC::runSweepOnce(now, grace)`:
  1. `live = PoolScan::listLivePartIds(...)`.
  2. `reachable_blobs = markReachableBlobs(live, resolve)` where `resolve(part_id)` reads `parts/<part_id>` and throws on miss (B18).
  3. `parts = listKeysUnder(partsPrefix)`; `blobs = listKeysUnder(blobsPrefix)`. `unreferenced = (parts − {partKey(live)}) ∪ (blobs − reachable_blobs)`.
  4. `auto res = selectForSweep(unreferenced, first_unreachable, now, grace); first_unreachable = std::move(res.first_unreachable);`
  5. `object_storage->removeObjectsIfExist(res.to_delete)`. Return counts. Any exception before step 5 → no deletion.
- [ ] Build → green; commit `"CAS M1 P4: reachability sweep core (grace-from-unreachability)"`.

## Task 3: background GC thread + lifecycle wiring {#task-3}
- [ ] Implement `ContentAddressedGCThread` mirroring `src/Disks/DiskObjectStorage/Replication/BlobKillerThread.{h,cpp}`: a `BackgroundSchedulePoolTaskHolder` constructed with `(disk_name, context, object_storage, key_prefix, log)`; `startup()` schedules the task; the task calls `gc.runSweepOnce(now, grace)` (now from a steady clock) and reschedules after `reschedule_interval_sec`; `shutdown()` deactivates; `applyNewSettings` reads `content_addressed_gc_interval_sec` (default 600) and `content_addressed_gc_grace_sec` (default 3600) from config. Wrap each `runSweepOnce` in try/catch + `tryLogCurrentException` so a transient list error never kills the loop (it just retries next round — fail-safe: on error, delete nothing).
- [ ] Wire into `ContentAddressedMetadataStorage`: own a `ContentAddressedGCThreadPtr`; construct it in the ctor (it needs `context` — thread `ContextPtr` through `registerContentAddressedMetadataStorage` from `Context::getGlobalContextInstance()`, as the factory already does for `getObjectKeyCompatiblePrefix`); `startup()` calls `gc_thread->startup()`; add `shutdown()` (override if `IMetadataStorage` has one, else stop in the dtor) calling `gc_thread->shutdown()`. The disk's `startupImpl` already calls `metadata_storage->startup()`.
- [ ] Test: an integration-style gtest that constructs the metadata storage, seeds an orphan, calls `gc_thread->triggerAndWait()` (add a synchronous trigger like `BlobKillerThread::triggerAndWait`) with a tiny grace, and asserts the orphan is gone. (Unit-level; the functional test is Task 5.)
- [ ] Build → green; commit `"CAS M1 P4: background GC thread + disk lifecycle wiring"`.

## Task 4: feed the `BlobRefIndex` cache (B9 seam — kept current, not load-bearing) {#task-4}
- [ ] Implement: in `ContentAddressedTransaction::commit` call `blob_ref_index->addPart(part_id, footer)`; in `removeRecursive`, when a ref is unlinked, `removePart(part_id, footer)` (read the footer first; if already gone, skip). Hold the `InMemoryBlobRefIndex` on the metadata storage. Add a `// NOTE(B9): the sweep uses reachability (ground truth); this index is a cache that a persisted/sharded delta-driven sweep will consume.`
- [ ] Test: after a commit then a ref-unlink, `index.unreferenced()` reflects the deref. (This documents the seam; the sweep does NOT depend on it.)
- [ ] Build → green; commit `"CAS M1 P4: feed BlobRefIndex deltas at commit/remove (B9 seam)"`.

## Task 5: functional reclamation test (the Phase-4 oracle) {#task-5}
- [ ] Create `tests/queries/0_stateless/<NNNNN>_content_addressed_gc.sql` via `./tests/queries/0_stateless/add-test`. Use an inline `disk(... metadata_type=content_addressed ...)` with a SHORT grace + interval via the disk settings (`content_addressed_gc_grace_sec=1`, `content_addressed_gc_interval_sec=1`) so the test does not wait minutes. Scenario: CREATE + INSERT (1000 rows) + a SECOND distinct INSERT, `OPTIMIZE FINAL` (now the 2 source parts are outdated → their footers/blobs become unreferenced once `grabOldParts` drops them), then `SYSTEM ...`/sleep-free wait for the merged result, then assert `SELECT count()` is stable and correct. Then verify reclamation: query `system.disks`/an introspection path if available, OR (simpler, deterministic) assert via a second table that re-inserts the SAME merged data and dedups (blob reuse) — i.e. assert correctness is preserved across GC. Keep it black-box (compare to a normal table). DROP at the end.
- [ ] **Validate in praktika** (the `clickhouse-praktika-tests` skill): `python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test <NNNNN>_content_addressed_gc`; read `ci/tmp/test_result.txt` — MUST show `[ OK ]` / `Failed: 0`. (Note: a deterministic GC functional test is hard because reclamation is time + background-thread driven; if asserting actual object deletion proves flaky, keep the functional test to "GC does not corrupt or lose data" — correctness under merges/drops with GC enabled — and rely on the Task-2 unit tests for the deletion semantics. Do NOT write a flaky sleep-based assertion.)
- [ ] Commit `"CAS M1 P4: functional GC test (no data loss under merge/drop with GC enabled)"`.

---

## Self-review {#self-review}
- **Spec coverage:** §5 reachability GC (Task 2 `markReachableBlobs`), grace-from-loss-of-reachability (Task 2 `selectForSweep` + persisted `first_unreachable`), the background lifecycle (Task 3), commit-failure orphans + outdated-part footers as fodder (Tasks 2/5), dedup-safe (Task 2 carry-forward test). Reader pins (B3), coordinator (B11), persisted index (B9), expedited delete (B14), frozen/detached roots (B4/B12) explicitly deferred in §safety.
- **Placeholder scan:** every task has a concrete test + steps; the only "mirror, don't reinvent" is the `BackgroundSchedulePool` wiring (Task 3 → `BlobKillerThread`), which is integration glue against the live interface, as in Phases 2–3. Task 5 explicitly avoids a flaky timing assertion.
- **Type consistency:** reuses `markReachableBlobs`/`selectForSweep`/`SweepResult`/`FooterResolver` (Phase 1), `Footer`, `PoolPaths` keys, `object_storage->listObjects`/`removeObjectsIfExist`; new `PoolScan`, `ContentAddressedGC::runSweepOnce`, `ContentAddressedGCThread` used consistently.
- **Safety re-check:** the sweep deletes ONLY `blobs/`+`parts/`, ONLY past grace, ONLY when the full reachable set computed cleanly (fail-close on any error/missing footer), in a SINGLE process. These four are the data-loss guards; each has a test.

## New deferrals likely to surface {#deferrals}
- Introspection (`system.*` for blob/footer counts, GC rounds, last-sweep time) — B15.
- Persisting `first_unreachable` across restarts (M1 resets it → conservative: grace effectively restarts, never premature deletion) — note for B9/B15.
- `removeObjectsIfExist` batching / `max_blobs_in_task` throttling (mirror `BlobKillerThread`'s batch settings) if a sweep deletes many objects.

## Execution {#execution}
Task 1 → Task 5 via `superpowers:subagent-driven-development`, two-stage review each. After Phase 4 (GC reclaims orphans without data loss), Phase 5 = the `_pool_meta` self-check + fail-closed feature gate (reject projections/patch parts/unknown format version) — the last M1 safety piece. Append new deferrals to the backlog with plug-in points.

---

## ⚠ Post-review revision (2026-06-02) — supersedes the GC design above {#post-review-revision}
This plan shipped a **grace-based, default-on background reachability sweep**. A subsequent design review found that to be unsafe and buggy; **do not treat the above as the final GC design.** Specifically:
- **P1 data-loss bug (fixed):** `markReachableBlobs` returned *bare hashes* while the sweep listed *full fan-out object keys* → the reachable set never matched → the sweep would delete **live** blobs after grace. The Task-2 tests missed it (they hand-seed a self-consistent key convention) and the integration test missed it (no *live* table during a sweep). Fixed: reachable set uses full object keys + a regression test that builds parts through the real transaction and asserts a live part survives a sweep.
- **`grace` is not a safety mechanism.** Time may protect only failure detection, never live work — a write→commit slower than `grace` (or a GC pause / clock skew) loses data. The safe design is **pin + lease + fence** (backlog **B32**): in-flight writes are made visible to the GC via leased pins; GC marks `refs ∪ live pins`, re-validates under a leader lock before delete; time only reclaims a *crashed* writer's orphans; a fencing token closes the paused-writer hazard. Works without Keeper (object-store conditional writes) — Keeper (B11) is just a faster catalog.
- **Background deletion must not run un-coordinated.** The background sweep is now **OFF by default** (`content_addressed_gc_enabled`, default false); only `triggerAndWait` (manual/test) runs. Re-enable only after **B32** + the `_pool_meta` ownership self-check (Step 2 / Step 6 of the M5 hardening plan).
- `BlobRefIndex` is an **un-wired seam** (quarantined; reachability is the explicit M1 GC) until B9.

The real forward path for GC and the surrounding structural fixes is **`plans/2026-06-02-cas-mergetree-m5-hardening.md`** (Step 6 = the B32 protocol).
