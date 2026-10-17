---
description: "Whole-branch review of fix/antalya-26.6/cas-part-file-probes-no-list (CAS-95.1) by the opus final reviewer; six minors, all folded into the fix wave."
sidebar_label: "CAS-95.1 branch review (opus)"
sidebar_position: 41
slug: /superpowers/reports/cas-95-1-branch-review-opus
title: "CAS-95.1 branch review (opus)"
doc_type: "report"
---

# Final whole-branch review: CAS-95.1, 8d62c314ec1..80dcaed4008 {#final-whole-branch-review-cas-95-1-8d62c314ec1-80dcaed4008}

Reviewer: final-review (ca-review). Read-only; one pass over the full diff, then focused checks in
/home/mfilimonov/workspace/ClickHouse/cas-95-1 at 80dcaed4008, one per named risk.

## Strengths {#strengths}

- The unresolved-ref fall-through is exactly the old code's. After the `if (p)` block,
  `classifyDirectory` used to pick `TableSubdir` if `parseTableFilePath` succeeded and
  `GenericIntermediate` otherwise. The new `PartFile` arm answers `dr.tf ? tableSubdir* :
  liveTree*`, which is the same decision. The two helpers are verbatim extractions.
- The no-fallback rule holds. `CachedPartFolderAccess::getView` returns null only when `resolve`
  returns nullopt. Resolve means a lookup in the committed ref map, or a namespace the catalog does
  not name. Every read failure rethrows, including the single-flight followers. The failure test
  proves no LIST is issued on a failed read.
- Both `switch (dr.shape)` sites (`existsDirectory`, `listDirectory`) handle `PartFile` and neither
  has a `default:`, so `-Wswitch` would catch a missed arm. There are no other `DirShape` consumers.
- `isDirectoryEmpty` still short-circuits part dirs and projection dirs before iterating. The
  projection answer (true) is preserved, and a nested non-projection dir is now non-empty, as the
  spec intends.
- The test seam's claim "every method the backend reaches is overridden" holds for a local pool.
  The methods it does not override (`iterate`, `readSmallObjectAndGetObjectMetadata`,
  `tryGetObjectMetadataWithNativeToken`, `removeObjectIfTokenMatches`,
  `removeObjectsIfExistUnderProfile`) are reached only under `Mode::Native`. The emulated branch
  lists through `listObjects` and reads through `readObject`, which are recorded. The unresolved-ref
  test's exact `listCount == 1` proves that the LIST path is recorded, so the zero-LIST assertions
  are not vacuous.
- The core gtests would fail on the base commit:
  - Default and cold-cache probes: base LISTs `_files/`.
  - Detached/non-Atomic: base LISTs through `TableSubdir`/`Generic`.
  - Failed manifest read: base never GETs, so nothing throws.
  - Fifty parts: base issues 300 LISTs.
  The unresolved-ref and unpublished tests are deliberate regression pins.
- The stateless oracle reads the ATTACH query's own ProfileEvents by a per-database `query_id` on a
  per-database ad-hoc disk, so parallel tests cannot pollute it. The red run on the lane-g binary
  (105/1005) proves the counter is live.
- The bare `--send_logs_level=fatal` on FORGET is safe. `clickhouse-test` always appends
  `--allow_repeated_settings` to `CLICKHOUSE_CLIENT_OPT` (in its function that assembles the client
  options), so the sibling tests' explicit flag is redundant, not required.
- No upstream file is touched. No `LOGICAL_ERROR` is added. No added line cites a plan, task,
  backlog item or finding ID.
- Sanitizer sweep: the only throw expectation is
  `EXPECT_THROW(existsDirectory(...), DB::Exception)` in
  `FailedManifestReadPropagatesAndDoesNotList`. The gate log shows the exception that actually
  arrives is `NETWORK_ERROR` from `throwCasWriteRetryLater` ("read of '<manifest>': no lease
  budget for the reissue"), not `LOGICAL_ERROR`. It is safe under the sanitizer lanes.

## Issues {#issues}

### Critical (Must Fix) {#critical-must-fix}

None.

### Important (Should Fix) {#important-should-fix}

None.

### Minor (Nice to Have) {#minor-nice-to-have}

1. **CODE: the `dirPrefixOf` branch for a trailing `/` is dead, and its comment claims work it
   never does.** The helper lives in `ContentAddressedMetadataStorage.cpp`.
   - Why the branch is dead: `splitNonEmpty` in `PartPathParser.cpp` drops empty components.
     `parsePartFilePath` joins the remaining components with `/`, and `route` only splits off the
     first component. So `Route::file` never ends in `/`, and `file.ends_with('/')` is always false.
   - Why the test does not pin the helper: `CASDirectoryProbes.PartFileAnswersFromTheViewWithoutAList`
     asserts `existsDirectory(part + "/sub/")` under the comment "trailing slash". It passes because
     of the parser. It would still pass with the helper replaced by `file + "/"`.
   - Why the comment is wrong: "whether or not the probe path carried one" describes a slash that
     never reaches the helper.
   - This came from the plan: Review Focus 1's premise ("the route's `file` would become `sub/`")
     was false. `existsFileOrDirectory` already uses plain `r->file + "/"`.
   - Fix: drop the helper and write `dr.r->file + "/"` in both `PartFile` arms. Keep the
     trailing-slash assertion, but say in its comment that the parser normalises the slash.

2. **TEST: the cold-cache GET oracle is weaker than spec §4 test 2.**
   - Spec §4 test 2 asks for "exactly one manifest GET per probe" with both caches at 0.
   - The test asserts `getCount(manifests) > 0`. A regression that GETs the manifest twice per
     probe, or GETs other objects, passes.
   - Exactly 9 of the probes in that block reach `getView`. `isDirectoryEmpty(p.proj)`
     short-circuits and makes no request.
   - Fix: measure one probe's manifest-GET count k first, then assert `== 9 * k`. A single
     `readManifestShared` may issue more than one GET, and the spec's "one" may be imprecise. Record
     the deviation if `k != 1`.

3. **TEST: `FailedManifestReadPropagatesAndDoesNotList` pins "some `DB::Exception`", not the failed
   read.**
   - Spec §4 test 4b says "throws the injected error". The injected `CANNOT_READ_ALL_DATA` is
     classified as retryable and retried until the lease budget runs out, which takes about 20 s.
     `NETWORK_ERROR` then arrives, and the mount lease is left past its deadline: the teardown logs
     "release during Pool teardown failed".
   - A future regression that throws for an unrelated reason, such as a lease fence, would pass.
   - The no-LIST half is sound.
   - Fix: assert the code (for example with `expectThrowsCode`), or check that the message names
     the manifest key. Optionally lower the mount-lease TTL or budget for this pool to cut the
     runtime, which also addresses the Task 3 runtime minor.

4. **TEST: the stateless oracle is vacuous when `CASRootList` reads 0 for both ATTACHes.**
   - `l200 = l20` and `l200 < 20` both hold at 0/0. The recorded red run proves the counter is live
     today, but nothing in the test proves it next time.
   - The table enumeration's LIST is legitimate and always present, so the counter is never 0 on a
     working tree.
   - Fix: add `l20 > 0` to the SELECT, one more column in the reference.

5. **PROSE (IMPRECISE): the docs sentence in `read-path.md`.** "only a probe whose part does not
   resolve falls back to the table-level file listing" is true for Atomic paths only. A non-Atomic
   unresolved probe falls back to the mirrored live-tree LIST. The ledger's minor (LIST without
   backticks) sits on the same line.

6. **PROSE (IMPRECISE): the new comments frame the invariant as history.** "answers exactly as
   before this shape existed" appears in the `existsDirectory` `PartFile` arm and in the header
   doc of `tableSubdirExists`. It stops meaning anything once the old code is gone. State the
   invariant instead: "answers as the table-level subdirectory does". Also,
   "The parser calls every first component after the table root except `deduplication_logs` the
   part component" in `classifyDirectory` holds for Atomic paths only; non-Atomic anchors on the
   rightmost part-shaped component.

## Deferred minors triage {#deferred-minors-triage}

- **Task 1, helpers in the `public:` section: leave.** The whole neighborhood is public already,
  and this is not the branch to re-split it.
- **Task 1, no direct PartFile test in that commit: leave.** It is resolved: Task 2 added the
  tests.
- **Task 2, plan citations in test comments: leave.** Verified fixed: no added line cites a plan.
- **Task 2, `filesPrefixOf` literal fallback: leave.** Verified fixed:
  `namespaceFilesLifeOf` throws on a null life.
- **Task 2, `listDirectory` on a plain file untested: leave.** Verified fixed: it is asserted empty.
- **Task 2, temp-dir leak: leave.** Verified fixed: `CountingStoragePool` removes both directories.
- **Task 2, untidy includes: leave.** Verified fixed. `<algorithm>` is used by `std::count_if`.
- **Task 3, "reachable defect" wording: leave.** It is PROSE in a task report, not on the branch.
- **Task 3, `std::runtime_error` in `namespaceFilesLifeOf`: leave.** It is setup-only, and gtest
  reports it either way.
- **Task 3, ~20 s runtime of the failure test: fold into Minor 3.** Same fix site. Not
  merge-blocking on its own.
- **Task 5, "object-store LIST" without backticks: fix before merge.** It is PROSE, so batch it with
  Minor 5 into `deferred-docs-fixes.md`, or fix it in the same docs line if a code round opens
  anyway.
- **Task 5, `pr_body.md` `Related:` is a local path: leave.** The user phrases the PR.

## Declined to judge {#declined-to-judge}

- **Ref-table recovery failures on unresolved part-shaped paths.** Paths such as
  `partition_exports/x` or `tmp/` now resolve a ref before the old LIST. A recovery failure there
  now propagates, and the read runs the piggybacked `sweepStalePrecommitsForRead` and
  `maybeScheduleSnapshotPublish`. This is consistent with the no-fallback rule, `existsFile` already
  does the same on part paths, and the resolve is request-free once the table has loaded. Outside
  the spec.
- **`tmp_restore_*`.** Only its classification is pinned. Whether a restore's temporary part is
  published at probe time decides the view-versus-old-branch answer, and both are correct. Outside
  the plan.
- **FREEZE shadow part files.** A spec non-goal (they stay `ShadowIntermediate`).
- **Non-Atomic re-anchoring on a nested part-shaped component.** A spec non-goal (parser rule).
- **`removeDirectory` on a nested non-projection dir inside a part.** Its `isDirectoryEmpty` flips
  from true to false, so a removal would now hit `CANNOT_RMDIR`. The flip itself is the spec's
  intended change. No `MergeTree` writer produces such a directory, but I did not enumerate every
  removal caller.
- **Randomized `MergeTree` settings in CI against the stateless oracle.** One local run is green. I
  did not prove that no randomized table setting makes load LISTs scale with parts.
- **Oversized views and eviction under a very large restart.** Views larger than
  `part_folder_cache_max_entry_bytes`, and LRU eviction under the 64 MiB default on a very large
  restart, can turn probes into manifest GETs. The spec acknowledges this. There is still no LIST.
- **`existsFileOrDirectory`.** It has its own pre-existing part-file branch, and an unresolved ref
  there answers false. Untouched by the branch.
- **The null-owner SIGSEGV in `ContentAddressedTransaction::tryCreateWriteBuffer`.** Pre-existing
  and not on the branch. It is reachable only through a test helper.
- **The upstream `checkSize` `.proj` change.** A separate backlog item.

## Recommendations {#recommendations}

- Apply one small code/test fix round: Minors 1 to 4. Batch Minors 5 and 6 and the Task 5
  backticks into `deferred-docs-fixes.md`.
- Run 05053 several times under `clickhouse-test` with randomization, for example `--test-runs 5`,
  before the PR, to cover the randomized-settings risk.

## Assessment {#assessment}

**Ready to merge? With fixes.**

**Reasoning:** The production change is correct. The unresolved branch is exactly today's
behavior, failures propagate, the switches are exhaustive, and no upstream code is touched. The
remaining items are one dead helper branch and three test oracles that are looser than the spec
or can pass vacuously. Each is a few lines and none is a correctness defect in shipped behavior.
