# Task 5 report — parallel intra-part blob upload fan-out

**STATUS: DONE.** Commit `fff1c21989d` on `cas-gc-rebuild`.

## What changed

- `ContentAddressedTransaction.cpp` — `uploadPendingBlobs` now builds one
  `Cas::BlobUploadRequest` per referenced pending blob and delegates to a new free
  function `DB::Cas::fanOutBlobUploads(build, requests, pool, hooks=nullptr)`. The
  old duplicate-membership filter is removed (subsumed by the fan-out's grouping).
- `ContentAddressedTransaction.h` — declares `fanOutBlobUploads` +
  `BlobUploadFanoutHooksForTest` (test-only `on_dispatch`/`in_task` seams). Added
  `CasPartWriteTxn.h`, `ThreadPool_fwd.h`, `<span>` includes.
- `ProfileEvents.cpp` — `CasBlobUploadFanoutBatches` (one per fan-out with work) +
  `CasBlobUploadFanoutTasks` (one per unique-ref task dispatched).
- `cas_test_helpers.h` — robust `ensureBlobUploadPoolForTest(size=8)`
  (init-if-not-initialized, NOT call_once).
- `gtest_cas_blob_upload_pool_env.cpp` (NEW) — a gtest listener that ensures the
  pool before EVERY test.
- `gtest_cas_upload_fanout.cpp` (NEW) — the six spec tests + two death tests.

## Key decisions

1. **Fan-out core is a free function taking `ThreadPool &` as a parameter**, not
   reaching for `Cas::blobUploadPool()` internally. This is what lets one test run
   the SAME inputs through a size-1 pool (serial reference) and a size-N pool
   (fanned) in one process — the server-wide pool is once-only per binary.
   Production `uploadPendingBlobs` passes `Cas::blobUploadPool()`.
2. **`ThreadPoolCallbackRunnerLocal<void>` + pre-sized result slots.** Each task
   writes only its own stable slot; the `results` vector is declared BEFORE the
   runner so the runner's destructor drains every task before the vector is
   destroyed on every path (including a dispatch-loop throw — B90). Merge-nothing:
   `waitForAllToFinishAndRethrowFirstError()` drains all then rethrows the first
   ascending-ref failure; a rethrow bypasses the merge.
3. **`ThreadName::UNKNOWN`.** Stage-1 must not add a CAS-specific `ThreadName` to
   the shared `setThreadName.h` enum (outside the allowed file set). UNKNOWN is the
   no-op name and avoids the `CasPool` pool-thread self-join assertions.
4. **Blast-radius fix.** `uploadPendingBlobs` now calls the fail-loud
   `blobUploadPool()`, which would ABORT the whole `unit_tests_dbms` process (via
   LOGICAL_ERROR under a sanitizer) for any existing CA-commit gtest if the pool
   were down. A global gtest listener (`gtest_cas_blob_upload_pool_env.cpp`) ensures
   the pool before every test — bulletproof against test ordering AND the
   `gtest_cas_blob_upload_pool.cpp` lifecycle suite that deliberately tears it down.
5. **Equivalence test compares stable fields + backend end state, NOT token
   values.** The InMemoryBackend mints tokens from one monotonic counter, so
   concurrent fresh/resurrect uploads land tokens in a non-deterministic order;
   comparing (kind, size, adopted, has-token) + logical payload + meta state
   captures the behavioral equivalence without token-order fragility.

## Tests (all six + routed items)

- `DepsEquivalenceAcrossBranches` — serial (pool 1) vs fanned (pool 4) over all
  seven `uploadBlobDetached` branches (six under HEAD-first-off, HeadHit under
  HEAD-first-forced); identical deps + backend end state.
- `CondemnedBranchesNeverGet` (routed T3-review (a)) — CountingBackend asserts ZERO
  get/getStream on the condemned BODY keys through a resurrect fan-out. (Global GET
  count is nonzero — the two condemned-meta point-reads — which is correct; the
  invariant is per-body-key.)
- `DuplicateRefsLaunchOneTask` / `DuplicateCondemnedS3ResurrectsCorrectly` — dup
  refs collapse to one task (on_dispatch counter); condemned-S3 dup pair resurrects
  content-correctly.
- `ConflictingDuplicateSizesRejected` + `DeclaredSizeMustMatchSourceSize` (routed
  T3-review (b)) — DeathTest split (asan aborts on LOGICAL_ERROR).
- `MergeNothingOnFailure` — poisoned sibling fails, good sibling's body becomes
  ordinary GC-reclaimable debris (a real GC round reclaims it; poison sibling never
  uploaded).
- `ConcurrentDedupCacheInsertion` — two latch-crossed tasks concurrently insert into
  the shared dedup cache (TSan-relevant).
- `PoolSaturationBounded` — pool 2 reaches peak concurrency 2; pool 1 degenerates to
  serial (peak 1) and fails FAST via a bounded wait (decoupled bounds; `entered ==
  total` release prevents a straggler hang).
- `DrainPrecedesUnwind` + `DispatchThrowStillDrains` — failure surfaces only after
  the join drained the slow sibling; a dispatch-loop throw still drains the
  already-scheduled task.

## Evidence

- **RED** (compile-RED, whole fan-out API is new symbols):
  `build_asan/build_s1t5_red.log` — `NINJA_EXIT=1`, exactly
  `7 error: unknown type name 'BlobUploadFanoutHooksForTest'` +
  `7 error: use of undeclared identifier 'fanOutBlobUploads'`, nothing unrelated.
- **New suite:** `build_asan/test_s1t5_fanout.log` — `[  PASSED  ] 11 tests.`
- **GREEN gate:** `build_asan/test_s1t5_gate.log` —
  `[==========] 1236 tests from 226 test suites ran. ... [  PASSED  ] 1236 tests.`
  (1225 baseline + 11 new = 1236; post-rebuild, `NINJA_EXIT=0` in
  `build_asan/build_s1t5.log`). No known-flake trip.

## Concerns

- The two release-only test bodies (`ConflictingDuplicateSizesRejected`,
  `DeclaredSizeMustMatchSourceSize`) are `#ifndef DEBUG_OR_SANITIZER_BUILD`, so they
  were NOT compiled in the asan gate — only their DeathTest counterparts were. They
  mirror the death-test bodies and the established T3/T4 precedent, so risk is low,
  but a release build has not compile-checked them.
- The gtest listener initializes the CAS blob upload pool (8 threads) for the ENTIRE
  `unit_tests_dbms` binary. Harmless (only used if a test reaches uploadPendingBlobs)
  but binary-global.

## Fix round 1 (rev-s1-t5 Important)

**Finding:** `uploadPendingBlobs` (`ContentAddressedTransaction.cpp:310`, was `:1711` in
the review's line numbering before other edits shifted it) now unconditionally calls
the fail-loud `Cas::blobUploadPool()`, but the only `initializeBlobUploadPool` call in
the tree was `programs/server/Server.cpp:1721`. `clickhouse-local` (`programs/local/LocalServer.cpp`)
and `clickhouse-disks` (`programs/disks/DisksApp.cpp`) both register disks and can reach
a CA commit without ever calling it, hitting `LOGICAL_ERROR` (abort under a sanitizer).

**Fix:**
- `programs/local/LocalServer.cpp` — added the `CasBlobUploadPool.h` include, the
  `content_addressed_blob_upload_pool_size` `ServerSetting` extern, and
  `DB::Cas::initializeBlobUploadPool(server_settings[ServerSetting::content_addressed_blob_upload_pool_size]);`
  at the end of `LocalServer::initialize` (right after `getFormatParsingThreadPool().initialize(...)`,
  the analogous point to `Server.cpp:1721`). Verified `initialize()` runs exactly once per
  process (single `LocalServer app;` in `mainEntryClickHouseLocal`, including under the
  libFuzzer harness, which calls `mainEntryClickHouseLocal` once and loops via
  `processQueryText` inside `runLibFuzzer`, not by re-invoking `initialize`) — no
  double-init risk. Added `DB::Cas::shutdownBlobUploadPool();` in `LocalServer::cleanup()`
  (idempotent + noexcept per the header contract), placed before `global_context->shutdown()`.
- `programs/disks/DisksApp.cpp` — `clickhouse-disks` loads no `ServerSettings`, so used
  the literal `16` (matches `content_addressed_blob_upload_pool_size`'s default in
  `src/Core/ServerSettings.cpp`) with a comment naming that source of truth, calling
  `DB::Cas::initializeBlobUploadPool(16);` in `DisksApp::main` right after the existing
  `getIOThreadPool().initialize(...)`. Added `DB::Cas::shutdownBlobUploadPool();` to the
  existing `SCOPE_EXIT_SAFE` block in `mainEntryClickHouseDisks`, alongside
  `DB::StaticThreadPool::shutdownAll()`. `DisksApp app;` is likewise constructed once per
  process — no double-init risk.

**Verification:**
- Rebuild: `nice -n 19 ninja -C build_asan clickhouse` — both `LocalServer.cpp.o` and
  `DisksApp.cpp.o` compiled clean, `programs/clickhouse` linked successfully
  (`build_asan/build_s1t5fix.log`, 27/28 steps, no errors, `NINJA_EXIT=0` appended).
- End-to-end smoke (`build_asan/test_s1t5fix_smoke.log`, script `tmp/test_s1t5fix_smoke.sh`):
  ran the rebuilt `clickhouse local` with an inline `disk(type = object_storage,
  object_storage_type = local, metadata_type = content_addressed, ...)` MergeTree —
  `CREATE TABLE` + `INSERT ... SELECT number, toString(number) FROM numbers(1000)` +
  `SELECT count(), sum(a)` round-tripped, returning `1000  499500`, exit code 0, no
  `LOGICAL_ERROR`/"blob upload pool is not initialized" in the output. PASS.
  Attempted a pre-fix RED demonstration against the `build/` directory's existing
  `clickhouse` binary (not rebuilt by this fix round) — it also passed, so that binary's
  provenance predates this bug (built before T5's fail-loud pool call landed there, or at
  a different point in this shared worktree's history) and does not serve as a valid RED
  witness. Not re-demonstrable without a from-scratch rebuild at the pre-fix commit;
  relying on the reviewer's source trace (`ContentAddressedTransaction.cpp:310` →
  `Cas::blobUploadPool()` → throws `LOGICAL_ERROR` when `pool_instance` is null,
  `CasBlobUploadPool.cpp:51-58`) for the RED side.
- `clickhouse-disks` smoke: investigated instead of run. `clickhouse-disks write`
  always uses an autocommit `DiskObjectStorageTransaction` (`DiskObjectStorage.cpp:936-945`
  → `writeFileWithAutoCommit`); for a non-part-shaped path (a plain file) it takes the
  verbatim `CaInlineWriteBuffer` branch with no `pending_blobs` and no pool involvement,
  and for a part-shaped blob-required file (`data.bin`/`.mrk*`/`primary.idx`)
  `ContentAddressedTransaction::tryCreateWriteBuffer` throws `NOT_IMPLEMENTED` for
  autocommit before any blob staging (`ContentAddressedTransaction.cpp:766-772`). So the
  ordinary `write` verb cannot reach `blobUploadPool()` today — the DisksApp init is
  defense-in-depth (consistent + cheap, per the review's own fallback) rather than a
  closed live bug for that verb. This refines the review's "secondary, more exotic, but
  reachable" note: reachability for `clickhouse-disks` specifically is not demonstrated
  for the `write` verb; the init still guards any other/future path that might reach the
  pool non-autocommit.
- Gate: **not re-run.** Only `programs/local/LocalServer.cpp` and `programs/disks/DisksApp.cpp`
  changed — no `src/` library code touched, so `unit_tests_dbms` (the definitive CA gate)
  is unaffected by this fix and does not need a rerun.

**Commit:** program-file-only, message `ca: stage1 T5 fix — blob upload pool wiring for
clickhouse-local and clickhouse-disks (rev-s1-t5 Important)`.
