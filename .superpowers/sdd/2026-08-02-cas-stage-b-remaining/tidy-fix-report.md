# Tidy-fix finisher report (T8, CAS scope)

Finisher verification of the draft at `b0f87e8aaf1` ("ca: draft — clang-tidy fixes for CAS scope
(UNVERIFIED-DRAFT, no runs)"), cherry-picked together with its report commit `355ceba8b6a` onto
`cas-gc-rebuild` (base tip `7e20be96be9`, cherry-picked cleanly, no conflicts). This report
records what the draft's write-only pass could not: actual `clang-tidy` runs, actual test runs,
and three real defects the draft introduced or missed that only running the suites caught.

## Headline: the draft was NOT diagnostic-clean or behavior-neutral as written

Three problems surfaced during verification, all fixed in this pass:

1. **Two broken `NOLINTNEXTLINE` placements.** `NOLINTNEXTLINE` only suppresses the literal next
   source line. The draft wrote two 3-line comments where the tag sat on the *first* line of the
   comment block, two lines above the code it was meant to cover — so the diagnostic still fired
   as a hard error under `-warnings-as-errors`. Reproduced directly with `clang-tidy-21` before
   fixing:
   - `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasNamespaceLifeId.h:59`
     (`bugprone-suspicious-stringview-data-usage` on the `unhexUInt<UInt128>(s.data())` call).
   - `src/Disks/tests/gtest_cas_ref_statemachine.cpp` (`E3AdmitsPreviewLeavesStateByteIdentical`,
     `performance-unnecessary-copy-initialization` on `const RefTableState before = state;`) — this
     is the exact site the draft report itself flagged as "do not fix this one"; the suppression
     rationale was correct, only the comment's line placement broke it. Fixed by moving the
     `// NOLINTNEXTLINE(check)` tag onto the line immediately above the flagged code in both cases,
     keeping the explanatory prose above that.

2. **One diagnostic the draft missed entirely.** `src/Disks/tests/gtest_cas_parallel_commit.cpp:230`,
   `CaTxnRollbackFixture::currentManifest`, is `readability-make-member-function-const` — the same
   class the draft fixed on five sibling methods in the same fixture (`stageSimplePart`,
   `partAccess`, `beginTxn`, `armPromoteFailure`, `armAfterPromoteHook`), but this one method was
   skipped. Fixed by adding `const`, consistent with the other five.

3. **A real test regression from one of the draft's two "CODE findings".** The draft's finding
   about `CatalogAfterListBackend`'s catalog probe (`gtest_cas_namespace_janitor.cpp`) claimed
   "verified no assertion in either test depends on those counters" for routing the probe's
   internal `get` through `CountingBackend` instead of `InMemoryBackend`. That claim was false for
   this file: `CasNamespaceJanitor.PostListCatalogCutProtectsConcurrentCreationWithOneGet` asserts
   `backend.getCount(layout.refCatalogKey()) == 1u` — the whole point of the test is proving the
   janitor's own production code issues exactly one `get` of the catalog key. `CatalogAfterListBackend`'s
   probe simulates a *different, concurrent* actor's read (a competing namespace creation racing the
   janitor), and it was deliberately routed around the counter for that reason. Rerouting it through
   `CountingBackend::get` made the simulated concurrent actor's read count against the same total,
   so the assertion now saw 2 instead of 1 and failed. This was caught by the CAS gate (identical
   failure in both release and ASan runs), not by reading the diff. Fixed by reverting this one hunk
   back to `InMemoryBackend::get`, with a `NOLINT(bugprone-parent-virtual-call)` and a comment
   explaining why routing through the counter here would be wrong (the opposite of finding #2's other
   half, `PostFoldUnreadableTerminalBackend::existsIgnoringFault` in `gtest_cas_gc_frontier_gate.cpp`,
   which IS a true routing bug with no counter dependency — confirmed by the same gate passing clean
   with that one fixed).

None of these three would have been caught by re-reading the diff; all three were caught by actually
running `clang-tidy` and the test suites, which is the entire reason this verification pass exists.

## Per-TU re-tidy

All 43 files the draft touched (12 production + 31 test files; 4 of the 12 production files are
headers with no direct compile-database entry, verified through one including `.cpp` TU each:
`CasLayout.h` → `CasRefCatalogFormat.cpp`, `CasGcMaintenanceState.h` → `CasGcMaintenanceState.cpp`,
`CasPartWriteTxn.h` → `CasPool.cpp`, `CasNamespaceLifeId.h` → `gtest_cas_namespace_life_id.cpp`) were
run individually through `clang-tidy-21 -p build_amd_tidy <file>`.

First full sequential pass over all 43 files, run as fixes were being made rather than strictly
before any of them: it caught 6 `error:` lines at 2 source locations that had not yet been fixed by
the time the sequential run reached them — 5 recurrences of the `CasNamespaceLifeId.h` line (it
re-parses once per including file) and 1 `gtest_cas_parallel_commit.cpp` line. Zero diagnostics
anywhere else in that pass. Log: `build_amd_tidy/tidyf_retidy.log`.

`gtest_cas_ref_statemachine.cpp`'s broken `NOLINTNEXTLINE` was reproduced separately, via a direct
single-file `clang-tidy-21 -p build_amd_tidy src/Disks/tests/gtest_cas_ref_statemachine.cpp`, before
fixing it — the sequential pass above doesn't show this error because the fix had already landed by
the time that background run reached this file in its alphabetical order, not because the file was
clean beforehand.

Every file touched by a finisher fix was independently re-verified with a fresh
`clang-tidy-21 -p build_amd_tidy <file>` immediately after that fix landed, confirming zero
`error:` output: `CasRefCatalogFormat.cpp` (the `CasNamespaceLifeId.h` proxy),
`gtest_cas_ref_statemachine.cpp`, `gtest_cas_parallel_commit.cpp`, and
`gtest_cas_namespace_janitor.cpp` after the `CatalogAfterListBackend` revert (this last file had zero
clang-tidy diagnostics before or after the revert — its regression was a test-behavior bug the gate
caught, not a tidy finding; see the Headline section).

Combined, this gives full per-TU coverage: every file not touched by a finisher fix was already
proven clean by the first 43-file pass; every file a finisher fix DID touch was proven clean by its
own dedicated post-fix re-run. A second complete 43-file sequential re-run was also started as
additional confirmation (`build_amd_tidy/tidyf_retidy_final.log`); at the time of this report it had
completed 10 of 43 files with zero `error:` lines and was still running unattended in the
background — see that log directly for its final state if it matters for a later task.

## Behavior-neutrality gates

Both gates run via `utils/cas-gate/run_cas_gate_per_suite.sh`, which regenerates the suite list from
the built binary's own `--gtest_list_tests` and fails loud on any unclaimed suite (0 unclaimed in
both builds, both runs).

First run (before the `CasNamespaceJanitor` fix — while `CasNamespaceLifeId.h`,
`gtest_cas_ref_statemachine.cpp`, `gtest_cas_ref_recovery_cas_walk.cpp` and
`gtest_cas_parallel_commit.cpp` were already fixed, but the janitor regression was not yet found):

| Build | Suites | Pass | Fail | Abort |
|---|---|---|---|---|
| release (`build/`) | 278 | 277 | 1 | 0 |
| ASan (`build_asan/`) | 296 | 295 | 1 | 0 |

Both failed the identical suite, `CasNamespaceJanitor` (test
`PostListCatalogCutProtectsConcurrentCreationWithOneGet`) — the regression described above. Logs:
`build/tidyf_gate_release.log` + `build/suite_logs/CasNamespaceJanitor.log`,
`build_asan/tidyf_gate_asan.log` + `build_asan/suite_logs/CasNamespaceJanitor.log`.

Final run (after the `CatalogAfterListBackend` revert):

| Build | Suites | Pass | Fail | Abort |
|---|---|---|---|---|
| release (`build/`) | 278 | 278 | 0 | 0 |
| ASan (`build_asan/`) | 296 | 296 | 0 | 0 |

Logs: `build/tidyf_gate_release2.log` (+ `build/per_suite_results.txt`),
`build_asan/tidyf_gate_asan2.log` (+ `build_asan/per_suite_results.txt`). Suite counts (278/296)
match the standing baseline exactly.

### Test-count neutrality (source-level, not binary-count)

Rather than trust a raw individual-test count extracted from log text (which turned out to disagree
with the previously-quoted baseline numbers by a constant, method-dependent offset in both builds —
not attributable to any change made here, and not chased further since it is orthogonal to this
task's actual risk), test-count neutrality was verified at the source level: for every one of the 31
touched test files, the count of `TEST`/`TEST_F`/`TEST_P` macro invocations was diffed against the
pre-draft version (`b0f87e8aaf1^`). Exactly one file changed count, by exactly one test:
`gtest_cas_ref_recovery_cas_walk.cpp` (38 → 39) — the
`PutHookBackendComposesHidingListBackendCasPutFaultInjection` test added below. Every other touched
test file has an identical test count before and after all tidy fixes.

## PutHookBackend composition — the draft's own checklist item

The draft's checklist asked the finisher to prove the `PutHookBackend::casPut` routing fix (CODE
finding 1: it was reaching past its immediate parent `HidingListBackend` straight to
`CountingBackend`, silently disabling `HidingListBackend`'s fault-injection hooks) actually composes,
since no existing test armed both layers on one instance. Added
`CasRefRecoveryCasWalk.PutHookBackendComposesHidingListBackendCasPutFaultInjection` in
`src/Disks/tests/gtest_cas_ref_recovery_cas_walk.cpp`: constructs a `PutHookBackend`, arms
`HidingListBackend::before_cas_put` and `PutHookBackend::on_key`/`watched_substr` on the same
instance, calls `casPut` once, and asserts both hooks fired. Passes in both release and ASan runs
(part of the 278/296 all-green gate above).

## CODE findings verification (class hierarchy checks)

Confirmed by reading the class declarations directly, not just the diff:
- `PutHookBackend : public HidingListBackend : public CountingBackend`
  (`gtest_cas_ref_recovery_cas_walk.cpp`) — routing `casPut` through `HidingListBackend::casPut`
  is the immediate-parent call the check wants.
- `PostFoldUnreadableTerminalBackend final : public CountingBackend`
  (`gtest_cas_gc_frontier_gate.cpp`) and `CatalogAfterListBackend : public CountingBackend`
  (`gtest_cas_namespace_janitor.cpp`) both had `InMemoryBackend` as their *grandparent*, confirming
  the parent-virtual-call diagnosis was structurally correct for both — but, as detailed above, the
  fix was correct behavior for the frontier-gate case and a regression for the janitor case, because
  the two call sites have different semantic roles (own-code introspection vs. simulating a foreign
  actor).
- The `bugprone-optional-value-conversion` fix in `gtest_cas_orphan_manifest_sweep.cpp`
  (`before.token` passed directly instead of `*before.token` into a `casPut` overload that takes
  `const std::optional<Token> &`) is a straightforward simplification with no behavioral surface —
  confirmed by the same all-green gate run, which exercises this suite.

## Commit

One finisher commit follows this report, containing:
- The `CasNamespaceLifeId.h` and `gtest_cas_ref_statemachine.cpp` `NOLINTNEXTLINE` placement fixes.
- The `gtest_cas_parallel_commit.cpp` missed `const`.
- The `gtest_cas_namespace_janitor.cpp` regression revert (`InMemoryBackend::get` restored,
  `NOLINT(bugprone-parent-virtual-call)` added with reasoning).
- The new `PutHookBackendComposesHidingListBackendCasPutFaultInjection` test.
- This report.

The cherry-picked draft commits (`0fbbaed3df5`/`ef0e6c40ee9` on this branch, originally
`b0f87e8aaf1`/`355ceba8b6a`) keep their own identity; this commit is the verification closure on
top of them.
