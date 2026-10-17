# CAS GC request cuts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Four request cuts in the CAS GC round that move no decision (except A0's proven-safe one) and add no setting, plus one LIST measurement: A0 carry the confirmed condemn marker, A4 decode each manifest body once per round, A2 free the pinned HEAD read-ahead window, B4 drain dead-life stream debris in batch deletes, CAS-29.13 measure and attribute the LIST requests per logical page.

**Architecture:** Every change lives inside `Gc::fold` / `settleEntry` / the janitor, behind the existing `GcReadAhead` and `CasOperation` seams; A4 adds one small unit (`CasGcManifestMemo`) with a byte-charged FIFO; everything else edits existing functions. Every task is failing-first against a no-change oracle (the same input through the old and the new path) with the in-memory backend's request counters.

**Tech Stack:** C++ (Allman braces, `chassert`), gtest suites named `CAS*` in `src/Disks/tests/`, `utils/cas-gate/run_cas_gate_per_suite.sh`, ProfileEvents, the ca-soak stand for the before/after run.

**Spec:** `docs/superpowers/specs/2026-09-29-cas-gc-request-cuts-design.md` (rev.3, ready for planning; codex rounds 1–3). Parent: `docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md`.

## Global Constraints

- **Worktree and branch.** `/home/mfilimonov/workspace/ClickHouse/gc-request-cuts`, branch `gc-request-cuts` created from `altinity/antalya-26.6` (61d24118251, PR #2351 merged; the fold uses `gc_io_concurrency`, there is no `cas_gc_concurrency`). Rename the branch to `cas/gc-request-cuts` in Task 0. Never merge from or into `cas-gc-rebuild`. Docs of the spec/plan stay on `cas-gc-rebuild`; this branch carries code, tests, `docs/en/antalya/cas/…` and the changelog line only.
- **Commits.** `git commit -s`, exact paths, imperative subject ≤ 72 chars, no rebase/amend, no push. Commit bodies name the reason and the spec section (e.g. "spec A4"), never a plan, a review or Claude.
- **Build.** `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && ninja -C build unit_tests_dbms clickhouse > build/build_<task>.log 2>&1; echo NINJA_EXIT=$?` (no `-j`); grep the log for `error:`; warnings in touched files are errors.
- **Gate.** `build/src/unit_tests_dbms --gtest_filter='CAS*'` run directly (`utils/cas-gate/` does not exist on antalya-26.6; filter exactly `CAS*`, never widened; baseline 2549 tests, 277 s) must be green after every task; `LOGICAL_ERROR` expectations are death tests under `DEBUG_OR_SANITIZER_BUILD`. The ASan lane on the exact pushed tree is the final gate (Task 6).
- **Memory cap.** Every gtest and mutation-probe run is wrapped as `( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter=… )`; a test that fills a bounded structure inserts a fixed count derived from the bound and asserts the bound every N inserts. A probe that disabled the memo budget without a cap took 85 of 91 GB and got the desktop session OOM-killed.
- **No decision moves** except A0's (spec §1, §2); no new setting; no on-S3 format change; no change to LIST trust, licences, exact-token deletes of mutable objects, HEAD-before-PUT, phase order or cursors. A4's bound is the fixed 64 MiB charge (spec §3); B4 batches only parsed `_log`/`_snap` keys (spec §5).
- **Oracle rule** (spec §7): each new test runs one input through the current path and the changed path and compares outcomes and carried rows, not counts alone. Exact names: counters `CASRefManifestBodyFoldGets` (unchanged meaning: consumed bodies), new `CASRefManifestBodyMemoHits`, `CASGCReadAheadHit`/`Miss`/`Wasted`; janitor result fields `deleted`, `leaked`, `anomalies`; helper `CasOperation::removeManyWriteOnce(const std::vector<WriteOnceKey> &, const Retry &)`; chunk size `clamp(gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys)`.
- **Prose.** Comments 1–2 lines stating a constraint; no history words; test names say the pinned behaviour.
- **Codex verification items carried here** (spec status line): (V1) the A0 oracle's preconditions per case: which process holds the in-memory memo and where the edge cut lies (Task 1); (V2) the A4 memory test asserts `charge ≤ 64 MiB` and `allocation ≤ 1.25 × charge`, so the real ceiling is 80 MiB; the README/docs line says 64 MiB charged, 80 MiB worst case (Task 2); (V3) B4's fault inequality holds per chunk, not per page: a key that vanished after LIST counts as deleted by a successful batch (Task 4).

## Review Focus

1. A0 must never graduate a row whose confirmation belongs to a different token: pinned by the supersede test (Task 1) and the delayed-write test.
2. A4 must produce byte-identical deltas, events and cleanup tokens on a hit, including a `-1` hit after a `+1` miss under a different `txn_ordinal`: pinned by the publish-then-drop test and the two-transactions test (Task 2).
3. A2 must not discard a hint a later take in the same shard needs: pinned by the mixed-algorithm and absent-take tests (Task 3).
4. B4 must never delete a `_ckpt`/`_files`/state key through the batch path, nor a key of a live life: pinned by the kinds test and the recreate-between-LIST-and-delete test (Task 4).
5. B4 must publish no cursor when admission was lost inside the fallback: pinned by the admission-loss tests (Task 4).

---

### Task 0: Worktree build and gate baseline

**Files:** none changed.

- [ ] **Step 1: Branch name.** `cd /home/mfilimonov/workspace/ClickHouse/gc-request-cuts && git branch -m gc-request-cuts cas/gc-request-cuts && git log --oneline -1` → must show 61d24118251 or its child.
- [ ] **Step 2: Build.** `ninja -C build unit_tests_dbms clickhouse > build/build_task0.log 2>&1; echo NINJA_EXIT=$?` (first build of this worktree: expect an hour or more; run under `nohup` with a marker line and poll every 10 minutes). `NINJA_EXIT=0`, no `error:` in the log.
- [ ] **Step 3: Gate baseline.** `bash utils/cas-gate/run_cas_gate_per_suite.sh > build/gate_task0.log 2>&1; echo GATE_EXIT=$?` → 0; record the suite count and wall time in the report. A red suite on the untouched base is reported, not fixed here.
- [ ] **Step 4: Counter inventory.** `grep -n -E 'CASRefManifestBodyFoldGets|CASGCReadAhead(Hit|Miss|Wasted)|CASGCCondemnMarkerUnconfirmedCarry' src/Common/ProfileEvents.cpp` → all present; paste into the report.

---

### Task 1: A0, carry the confirmed condemn marker (spec §2)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp` (`settleEntry`, the `graduationAvailable()` branch at ~498)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp` (carry), `src/Disks/tests/gtest_cas_gc_fold.cpp` (republish-and-clear, unreadable meta, delayed write, supersede)
- Model: `docs/superpowers/models/CaGcCondemnMarkerGate.tla`, `_fix.cfg`, `_bug.cfg`, `_RESULTS.md` (on `cas-gc-rebuild`, where the models live; the code commits go to the antalya branch)

**Interfaces:** none new. The row flag `marker_confirmed` (bit value 2 of the flags byte) is already encoded/decoded.

- [ ] **Step 0: Model check.** Extend `docs/superpowers/models/CaGcCondemnMarkerGate.tla` with a persisted confirmation flag on the retired entry (`ret.pconf`): set where `ret.conf` is set, kept by `GRestart` (which still clears `ret.conf`), cleared by the supersede transition; `GSettleGraduate` accepts `ret.conf \/ ret.pconf \/ loadMeta = "condemned"`. In the same model add `observed_tok` and a `GObserve` action enabled between `GCut` and the settle (the prefetched HEAD of the read-ahead), and make `GSettleCondemn` condemn `observed_tok` instead of the live `body.tok` (spec §4 Interleaving). Run TLC on `CaGcCondemnMarkerGate_fix.cfg` (expect `NoDangle` to hold) and on `_bug.cfg` (expect the dangle trace still found) through `docs/superpowers/models/run_condemnmarker.sh` (`TLC_WORKERS=8`, `nice 10`), and append both results with state counts to `CaGcCondemnMarkerGate_RESULTS.md`. A violation on the fix configuration stops Task 1: report the trace, do not weaken the gate.
- [ ] **Step 1: Failing test, carry.** In `gtest_cas_blob_indegree.cpp`, `TEST(CASBlobInDegree, CarriedRowKeepsTheConfirmedMarker)`: fold N=3 condemned rows below the floor with a `work_budget` whose graduation budget is 1 and a `confirm_condemned_marker` callback that counts calls and returns true. Assert: round 1 → 3 calls, 1 graduated (`delete_pending`), 2 carried; decode the carried run rows → `marker_confirmed == true` for both carried rows (today: false). Round 2 over the carried run with a fresh callback → 0 calls for the carried rows, 1 more graduated.
- [ ] **Step 2: Run → red** (`build/src/unit_tests_dbms --gtest_filter='CASBlobInDegree.CarriedRow*'`).
- [ ] **Step 3: Implement.** In the `!graduationAvailable()` branch push `RetiredEntry carried = e; carried.marker_confirmed = true; rmr.still_retired.push_back(std::move(carried));` — only there; the `else` branch (unconfirmed) and the supersede path are untouched. One comment: `/// The confirmation is a durable fact about (hash, token); carrying it saves the re-read after a restart (spec A0).` (drop "spec A0" in the code; keep the reason).
- [ ] **Step 4: Failing tests in `gtest_cas_gc_fold.cpp`,** each with the oracle table of spec §2 (V1: state in each test which process holds the memo — always a fresh `Gc`, i.e. an empty memo — and that the writer's edge lands after the round's intake cut):
  - `CASGCFold.PersistedConfirmationGraduatesAfterRepublishAndClear`: confirm token T, exhaust the budget, fresh `Gc`, writer republishes the hash (token T′) and clears the marker to `Clean`; round graduates the flagged row (new) where the old path carried it; the next round's delete ends `Replaced`; T′ present.
  - `CASGCFold.PersistedConfirmationGraduatesWithUnreadableMeta`: meta GET throws at graduation → the flagged row graduates; a present T is deleted next round.
  - `CASGCFold.DelayedMarkerRetryOverReplacementDoesNotDeleteIt`: the delayed retry stamps `Condemned` over T′ and records the captured T; the delete for T ends `Replaced`; T′ survives.
  - `CASGCFold.SupersededRowStartsUnconfirmed`: a flagged row superseded by T′ yields a row with `marker_confirmed == false`.
  - Each test asserts equality with the old path for the unaffected sibling row (marker still `Condemned`).
- [ ] **Step 5: Run → red on the new expectations, green after Step 3** (the old path is reproduced in the test by a `Gc` built with the persisted flag cleared before the second round).
- [ ] **Step 6: Gate** `bash utils/cas-gate/run_cas_gate_per_suite.sh > build/gate_task1.log 2>&1` → green.
- [ ] **Step 7: Commit** the model changes on `cas-gc-rebuild` (exact paths under `docs/superpowers/models/`), then on the antalya branch `git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp src/Disks/tests/gtest_cas_blob_indegree.cpp src/Disks/tests/gtest_cas_gc_fold.cpp && git commit -s -m "CAS GC: carry the confirmed condemn marker across rounds"`.

---

### Task 2: A4, decode each manifest body once per round (spec §3)

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcManifestMemo.h`, `.cpp`
- Modify: `.../Gc/CasGc.cpp` (`Gc::foldManifestEdges` ~1423; the log-decode hint loop that calls `reads.hintRead(manifestKey)`; `Gc::fold` where `GcReadAhead reads(...)` is built ~1843), `src/Common/ProfileEvents.cpp` (`CASRefManifestBodyMemoHits`)
- Test: `src/Disks/tests/gtest_cas_gc_manifest_memo.cpp` (new, unit), `src/Disks/tests/gtest_cas_gc_fold.cpp` (fold-level oracle tests)

**Interfaces (produced):**
```cpp
// Gc/CasGcManifestMemo.h
struct ManifestFoldEntry { BlobRef ref; UInt128 source_id; String path; };   // one per Blob-placement entry, body order
struct ManifestFold { Etag etag; std::vector<ManifestFoldEntry> entries; };
class GcManifestMemo
{
public:
    static constexpr size_t kBudgetBytes = 64ull << 20;
    const ManifestFold * find(const ManifestId & id);            // nullptr on miss
    bool insert(const ManifestId & id, ManifestFold fold);       // false and not stored when the entry's own charge > kBudgetBytes; evicts oldest inserts until charge fits
    size_t charged() const;                                      // bytes charged now
    size_t hits() const; size_t evictions() const;
private:
    // charge per insert: sizeof(map node) + etag key/value capacity() + 64 B bookkeeping + per entry sizeof(ManifestFoldEntry) + path.capacity();
    // namespace strings are interned per memo (one copy per namespace) and charged once
};
```

- [ ] **Step 1: Failing unit tests** in `gtest_cas_gc_manifest_memo.cpp` (`CASGCManifestMemo.*`): insert/find hit; miss; oldest-first eviction past `kBudgetBytes` (use a memo built with a test budget through a constructor argument defaulting to `kBudgetBytes`); an entry whose own charge exceeds the budget is refused (`insert` false) and `charged()` unchanged; empty manifests are charged (≥ 256 B each) and evict past the bound; a 512-byte namespace shared by 1000 manifests is charged once; long paths are charged by capacity. (V2) `CASGCManifestMemo.AllocationStaysWithinTheCharge`: fill to the budget with 512-byte namespaces and 200-byte paths, measure allocation with `CurrentMemoryTracker` around the inserts → `allocation ≤ 1.25 × charge` and `charge ≤ kBudgetBytes`.
- [ ] **Step 2: Run → red** (missing header).
- [ ] **Step 3: Implement the memo** (header + cpp, `std::unordered_map<ManifestId, ManifestFold>` plus an insertion-order deque for FIFO eviction; namespace interning by `std::unordered_set<String>` of the memo).
- [ ] **Step 4: Failing fold tests** in `gtest_cas_gc_fold.cpp`, each comparing deltas, `RootAdd`/`RootRemove` events, `mf_cleanup` tokens and outcomes against the no-memo oracle (a fold run with the memo disabled through a test hook `Gc::setManifestMemoForTest(bool)`):
  - `CASGCFold.ManifestFoldedForPublishAndDropIsReadOnce`: 1 consumed-body GET instead of 2; the drop hit reproduces the `-1` deltas and the cleanup token.
  - `CASGCFold.ManifestMemoHitUsesTheCurrentSignAndOrdinal`: one manifest across two transactions and a repoint; a later edge that clamps the log discards the staged deltas.
  - `CASGCFold.ManifestMemoSkipsHintsAndFallsBackInline`: eviction between the hint skip and the take → one `CASGCReadAheadMiss`, one GET, same outcome.
  - `CASGCFold.FoldedManifestBodyIsNeverDeletedDuringTheFold`: the in-memory test backend asserts (fails the test) on a delete of a manifest key the running fold has memoized; run the publish-then-drop fold with a concurrent orphan-sweep and writer-cleanup step in the test → no assertion (spec §3 Interleaving).
  - `CASGCFold.AbsentManifestBodyIsReprobed`: absent on the first edge, present on the second → two reads, no memo entry for the absence.
  - Counter assertions: `CASRefManifestBodyFoldGets + CASRefManifestBodyMemoHits` equals the old path's `CASRefManifestBodyFoldGets` for consumed bodies; a repeated oversized manifest counts two GETs and zero hits.
- [ ] **Step 5: Wire the memo into `Gc::fold`:** a `GcManifestMemo memo;` local next to `GcReadAhead reads`, passed to `foldManifestEdges` (new parameter) and consulted by the hint loop (`if (!memo.find(id)) reads.hintRead(key)`); in `foldManifestEdges` a hit skips `takeRead` + validations and rebuilds `BlobDelta`s from `entries` with the current `sign`/`txn_ordinal`, emits the same events, and sets `mf_cleanup[id] = fold->etag` for `sign < 0`; a miss does today's work and inserts `{got->etag, entries}` after validation. Add `CASRefManifestBodyMemoHits` to `ProfileEvents.cpp` with a one-line description.
- [ ] **Step 6: Run → green; gate → green.** Log `build/gate_task2.log`.
- [ ] **Step 7: Commit** (exact paths of the five files + ProfileEvents.cpp): `"CAS GC: decode each manifest body once per fold"`.

---

### Task 3: A2, free the pinned HEAD read-ahead window (spec §4)

**Files:**
- Modify: `.../Gc/CasGc.cpp` (`topUpHeadHints` ~1983 and `head_blob`; the shard loop that resets `head_hint_shard`/`next_head_hint`)
- Test: `src/Disks/tests/gtest_cas_gc_fold.cpp` (positional rule against a reference model), `src/Disks/tests/gtest_cas_gc_read_ahead.cpp` (a `discardHead` unit test if the method needs one)

**Interfaces:** `GcReadAhead` unchanged (`hintHead`, `takeHead`, `discardHead`, `pending()`, `window()`). Per-shard caller state: `size_t next_hint`, `std::deque<size_t> outstanding` (candidate positions hinted, not taken), `size_t last_take` (position or `npos`).

- [ ] **Step 1: Reference model** (ten lines in the test file): given candidates `C[0..n)` in `BlobRef` order, window `W`, and a take sequence, compute expected Hit/Miss/Wasted per spec §4 ("the outstanding set is the contiguous range after the last take position `p`; a take at `q` hits iff `q ≤ p + W`; the first take hits iff `q < W`; an absent take keeps its successor hintable").
- [ ] **Step 2: Failing tests** (`CASGCFold.HeadWindow*`), outcomes equal the concurrency-1 oracle; Hit/Miss/Wasted equal the model: gaps of one, several and more than three windows; mixed algorithms in one shard (`XXH3_128` before `Sha256` by `BlobRef`); descending `BlobRef` ranges across a shard transition (outstanding heads of the finished shard are discarded and counted Wasted); a take absent from the candidates immediately followed by its successor (the successor hits); leftover untaken read slots from a clamped log do not pin the head window.
- [ ] **Step 3: Run → red** (today: misses while the window is pinned).
- [ ] **Step 4: Implement** in the caller: on take `ref`: `p = lower_bound(candidates from last_take+1, ref)`; `discardHead` every outstanding position `< p`; hit iff `p` outstanding and `C[p] == ref`, else inline; `next_hint = max(next_hint, p + (C[p] == ref ? 1 : 0))`; top up from `next_hint` while `outstanding.size() < reads.window()`; at shard transition `discardHead` every outstanding position; `chassert(p >= last_take)` (a take below the last take reads inline in release). Two comments at most.
- [ ] **Step 5: Run → green; gate → green** (`build/gate_task3.log`).
- [ ] **Step 6: Commit** `"CAS GC: free the pinned HEAD read-ahead window"`.

---

### Task 4: B4, dead-life stream debris in batch deletes (spec §5)

**Files:**
- Modify: `.../Gc/CasNamespaceJanitor.cpp` (`runOnePage`, per-key loop ~74–160), `.../Formats/CasLayout.h`/`.cpp` (a `WriteOnceKey` factory overload from a parsed dead physical identity, mirroring the existing `NamespaceLifeId` factories)
- Test: `src/Disks/tests/gtest_cas_namespace_janitor.cpp`

**Interfaces:** `Layout::writeOnceStreamKey(const ParsedRefObjectKey &) -> std::optional<WriteOnceKey>` (returns nullopt when the minted canonical key differs from the listed key). `NamespaceJanitor::Result` unchanged (`deleted`, `leaked`, `anomalies`, `pages`, `keys`).

- [ ] **Step 1: Failing tests** (`CASNamespaceJanitor.*`), each against the per-key oracle where the spec requires equality and against the spec §5 rule where it permits an undercount:
  - `DeadLifeStreamKeysDrainInBatches`: S stream keys + M state keys, tokenful listing → `⌈S/chunk⌉` bulk requests + M DELETEs, no HEAD; tokenless listing → plus HEADs for the M state keys only.
  - `StateKeysNeverEnterTheBatchPath`: `_ckpt`, `_files`, a maintenance-state key on the same page keep exact-token handling; the existing `TokenlessListHeadsDeadKeysAndRetainsConcurrentReplacement` stays green.
  - `RecreatedLifeBetweenListAndDeleteKeepsItsKeys`: same-name recreation after LIST → the new incarnation's keys survive (different prefix).
  - `PartialBatchFailureUndercountsNeverOvercounts`: batch deletes A then throws; fallback finds A absent, removes B → `deleted == 1`; (V3) with chunk size 1: A vanished after LIST counts 1 through its successful batch, B's failed batch then fallback counts 1 → `deleted == 2`; assert the per-chunk rule, not a page-wide inequality.
  - `ExhaustedRetriesAndUnsupportedBatchFallBackPerKey`: `NOT_IMPLEMENTED` → the rest of the page per key; exhausted retries → that chunk per key; `leaked`/anomalies equal the per-key oracle's for those keys.
  - `AdmissionLossPublishesNoCursor`: lost before a chunk, inside the fallback, after the last chunk → `page_decided == false`, cursor unchanged.
- [ ] **Step 2: Run → red.**
- [ ] **Step 3: Implement**: collect eligible `Log`/`Snap` keys (parsed, life not resolved, cut not ambiguous, deletes not suppressed, minted key equals listed key) into chunks; before each chunk `op.admitted()` else `page_decided = false; break`; `op.removeManyWriteOnce(chunk, Retry::standard())` → `deleted += chunk.size()`; on any exception run today's per-key loop over that chunk's keys (HEAD only when no listed token), keeping its admission check per delete; after `NOT_IMPLEMENTED` the rest of the page takes the per-key path; final `op.admitted()` before the cursor publication as today.
- [ ] **Step 4: Run → green; gate → green** (`build/gate_task4.log`).
- [ ] **Step 5: Commit** `"CAS GC janitor: delete dead-life stream keys in batches"`.

---

### Task 5: CAS-29.13, measure and attribute the LIST requests per page (spec §6)

**Files:**
- Modify: `.../Backend/CasObjectStorageBackend.cpp` (`listUnder`: one `LOG_TRACE` per call: cursor empty or not, keys returned, `DiskS3ListObjects` delta around the call)
- Create: `tests/integration/test_cas_gc_list_pages/test.py` + `configs/` (a CAS disk over RustFS, `with_rustfs`)

- [ ] **Step 1: Trace line** in `listUnder` (Trace level, one line per call).
- [ ] **Step 2: Integration test**: seed 20k `_log` keys under one life's stream prefix (through the ref-log writer or a direct backend loop), run one GC round whose only large LIST is the global walk (`SYSTEM CAS GC` or the interval), read `system.events` deltas for `DiskS3ListObjects` and `CASRefGlobalListPages` and the trace lines; assert only that the ratio is recorded (no threshold) and print the attribution table: first page (`limit + 1`), async prefetch past the page's last key, resumed iterators, retries (`CASRequestAttempt` minus calls), other LIST callers in `defer_decision`.
- [ ] **Step 3: Run** with `python3 -m ci.praktika run "integration" --test test_cas_gc_list_pages` (binary at `ci/tmp/clickhouse` per the praktika recipe) → the table in the report and in the PR description; expected ratio ≈ 2–3 on RustFS (otel.demo's 9.3 includes retries and other callers).
- [ ] **Step 4: Commit** `"CAS: trace LIST requests per logical page and measure them under RustFS"`.

---

### Task 6: Docs, changelog, stand run, PR

**Files:**
- Modify: `docs/en/antalya/cas/architecture/garbage-collection.md` (janitor batch deletes, `janitor_deleted` meaning, the memo counter, the 64 MiB charge / 80 MiB worst case), the changelog entry in the PR body.

- [ ] **Step 1: Docs** (headers with `{#anchors}`, tech-prose; no plan/review/Claude mentions).
- [ ] **Step 2: Stand run**: ca-soak S11 delete phase before (base 61d24118251) and after (this branch), same scenario and duration; record `cas_gc_log` per-phase durations and the ProfileEvents of spec §7 plus distinct `ManifestId`s per round; table in the report. Recipe: `utils/ca-soak/`, `python3 -m soak.run` phase 3 `--duration` (see `utils/ca-soak/scenarios/RUN_HISTORY.md`); binaries via the cas-nightly image or a local build.
- [ ] **Step 3: ASan lane** on the exact tree: build `build_asan` in the worktree for `unit_tests_dbms` and run the gate → green.
- [ ] **Step 4: PR body** from `.github/PULL_REQUEST_TEMPLATE.md` (Altinity/ClickHouse, base `antalya-26.6`): summary, `Related:` lines for the backlog ids CAS-29.3/29.5/29.12/29.9/29.13 and issue #2429, category Performance Improvement, changelog entry, the stand table and the LIST attribution table. Not pushed until the user says so.
- [ ] **Step 5: Commit** the docs; final `git log --oneline 61d24118251..HEAD` in the report.
