# Gate-hygiene slice — fence-race fixture fix + `LOGICAL_ERROR` death-split sweep

## 1. Fence-race test fix (`gtest_cas_gc_frontier_gate.cpp`)

Per `rca-fence-race.md`, `WinnerShape/CasGcCompletedRemovalFenceRace.FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement`
was a TEST defect, not a production hole: the `/Replacement` branch forged a `Live` catalog row for
the successor life without publishing a matching `_ckpt`, which a same-day-added, correct production
refusal (`chooseRecoveryGrounding`) now rejects.

**Red-first evidence** (release binary, before the fix): `build/hygiene_fence_red.log`.

```
[ RUN      ] .../FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement
unknown file: Failure
C++ exception with description "CAS recovery grounding: a Live or Removing namespace requires a
readable _ckpt with life_epoch" thrown in the test body.
```

**Fix**: in the `/Replacement` branch, before the winner's catalog CAS, publish a well-formed `_ckpt`
for the successor life (`NamespaceLifeId::fromCatalogEntry(fixture.ns, UInt128{178})`, `life_epoch = 1`),
mirroring `completeCreation`'s publish-then-flip order and reusing `seedCompletedRemoving`'s own
`RefCkpt` construction.

**Secondary fix (RCA §5)**: `ASSERT_TRUE(leader_a_failure)` accepted any exception. It now rethrows
the captured `exception_ptr` and asserts both the error code (`DB::ErrorCodes::NETWORK_ERROR`) and
that the message names the fence-loss path (`"pre-fold drain lost authority"`), so the test can no
longer pass because the deposed leader failed for the wrong reason.

**Post-fix, green** (`build/hygiene_fence_fixed.log`): both parameters pass, and — the point of the
fix — the epilogue's two `EXPECT`s under `/Replacement` (`refTableRuntimeIdentityForTest != 0`,
`refTableLifeForTest != predecessor_life`) execute and hold for the first time; they were previously
unreachable because the test aborted before reaching them.

```
[       OK ] .../FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Absent (0 ms)
[       OK ] .../FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement (0 ms)
[  PASSED  ] 2 tests.
```

## 2. `LOGICAL_ERROR` death-split relocation (`gtest_cas_ref_catalog.cpp`)

`CasRefCatalogFormat.RemovalStartedRoundIsRequiredExactlyForRemoving` mixed an
`expectThrowsCode(LOGICAL_ERROR, ...)` check (encode-side: a `Live` row carrying
`removal_started_round`) into an otherwise always-compiled test alongside two `CORRUPTED_DATA`
checks (decode-side; `CORRUPTED_DATA` does not abort and needed no change). The `LOGICAL_ERROR` arm
aborted the ASan gate at this exact site (`build_asan/t1c3_gate.log`).

Fix, following the file's own established idiom (`EncodeRejectsDuplicateNamespace` and its five
siblings, `#ifndef DEBUG_OR_SANITIZER_BUILD` / `#if defined(DEBUG_OR_SANITIZER_BUILD)` pair):
- extracted the `LOGICAL_ERROR` check into its own release-side test,
  `CasRefCatalogFormat.EncodeRejectsLiveWithRemovalStartedRound`, inside the existing
  `#ifndef DEBUG_OR_SANITIZER_BUILD` block;
- added the matching death twin,
  `CasRefCatalogFormatDeathTest.EncodeRejectsLiveWithRemovalStartedRoundAborts`, inside the existing
  `#if defined(DEBUG_OR_SANITIZER_BUILD)` block, asserting on the `"removal_started_round"` substring
  from the production throw site (`CasRefCatalogFormat.cpp`'s `removalRoundPairingOk` check).

This is the release-preserving shape: the check still runs (and must pass) in a release build, and
only a sanitizer build routes it through the death test instead.

## 3. Second unguarded site, found during the ASan rerun

`CasRefCatalog.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentity` (`CasRefCatalog::casUpdate`'s
identity-preserving refusal) was an `expectThrowsCode(LOGICAL_ERROR, ...)` with **no** guard at all —
not wrapped in `#ifndef DEBUG_OR_SANITIZER_BUILD`. It aborted the first ASan gate run
(`build_asan/hygiene_gate.log`, exit 134).

**Why the 28-site audit's file-presence check missed it, and why the fix doesn't repeat that
mistake:** the initial sweep pass (mandated in Part 3) verified guard *context* for the 22 sites the
brief named across six files, and separately verified the two `gtest_cas_namespace_life_id.cpp` /
`gtest_cas_text_format.cpp` bare-`EXPECT_THROW` sites — but it established "this file has guarded
blocks" by locating nearby `#ifndef`/`#endif` markers and treating any `LOGICAL_ERROR` reference in
the same file as covered by them, without checking each individual expectation's own line number
against the guard's actual start/end boundaries. `GenericCasUpdateCannotDeleteOrReplaceCatalogIdentity`
sits between two guarded blocks in `gtest_cas_ref_catalog.cpp` (the encoder-grammar block ending
around what is now line 383, and the `casUpdate`-vanish-mid-retry block starting around line 881) but
was itself outside both. A per-site check that only confirms "a guarded block exists somewhere in this
file" cannot distinguish that from "this specific site is inside one."

The corrected sweep is line-range-exact: for every `expectThrowsCode`/`EXPECT_THROW`/`EXPECT_ANY_THROW`
mentioning `LOGICAL_ERROR` in `src/Disks/tests/`, parse the file's actual
`#ifndef DEBUG_OR_SANITIZER_BUILD` … `#endif` ranges and test whether the site's own line number falls
inside one — not whether the file contains such a range at all. Re-run tree-wide, the only remaining
"unguarded" hit is the explanatory comment at `gtest_cas_ref_catalog.cpp` line ~314 (not a check),
confirming no other site of this kind remains.

Fix applied with the same idiom: the release-side test kept inside the existing
`#ifndef DEBUG_OR_SANITIZER_BUILD` behavior (now explicitly wrapped in one), with a new
`CasRefCatalogDeathTest.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentityAborts` twin asserting on
the two distinct production messages (`"cannot add or delete catalog entries"`,
`"cannot replace catalog identity"`). Verified directly against the rebuilt ASan binary:
`build_asan/hygiene_targeted_check.log` — `CasRefCatalogDeathTest.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentityAborts`
passes (the death assertion fires as expected).

## 4. Full sweep table

Every `expectThrowsCode(...LOGICAL_ERROR...)` in the six files the brief named, plus every bare
`EXPECT_THROW`/`EXPECT_ANY_THROW` mentioning `LOGICAL_ERROR` anywhere under `src/Disks/tests/`,
checked by parsing each file's actual `#ifndef DEBUG_OR_SANITIZER_BUILD` ranges and testing each
site's own line against them (not by file-level proximity):

| File | Test | Guard status |
|---|---|---|
| gtest_cas_blob_upload_pool.cpp | `CasBlobUploadPool.GetterThrowsBeforeInit` | guarded (pre-existing) |
| gtest_cas_blob_upload_pool.cpp | `CasBlobUploadPool.DoubleInitThrows` | guarded (pre-existing) |
| gtest_cas_ns_creation_lifecycle.cpp | `CasNsCreationLifecycle.CreateNamespaceRejectsAnAlreadyExistingEntry` | guarded (pre-existing) |
| gtest_cas_ns_creation_lifecycle.cpp | `CasNsCreationLifecycle.CompleteCreationRejectsANonCreatingEntry` | guarded (pre-existing) |
| gtest_cas_ns_creation_lifecycle.cpp | `CasNsCreationLifecycle.ReconcileStaleCreatorRejectsANonCreatingEntry` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsDuplicateNamespace` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsNonCanonicalOrder` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsCreatorPresentOnLive` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsCreatorAbsentOnCreating` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsZeroIncarnation` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsNameOverByteBound` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsEmptyNamespace` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.EncodeRejectsLiveWithRemovalStartedRound` | **relocated this pass** (§2) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogFormat.NsStateToWordRaisesLogicalErrorOnImpossibleValue` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalog.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentity` | **relocated this pass** (§3, found by the ASan rerun) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalog.CasAdmitEntryRejectsADuplicateNamespace` | guarded (pre-existing) |
| gtest_cas_ref_catalog.cpp | `CasRefCatalogRemoval.ExactDeletionRefusesChangedEntryAndAdmissionCannotCarryRemoval` | guarded (pre-existing) |
| gtest_cas_gc_hold_grammar.cpp | `CasGcHoldGrammar.TheEncoderRefusesEveryIllFormedCoverageRow` | guarded (pre-existing) |
| gtest_cas_upload_detached.cpp | `CasUploadDetached.MergeValidatesSizes` | guarded (pre-existing) |
| gtest_cas_upload_fanout.cpp | `CasUploadFanout.ConflictingDuplicateSizesRejected` | guarded (pre-existing) |
| gtest_cas_upload_fanout.cpp | `CasUploadFanout.DeclaredSizeMustMatchSourceSize` | guarded (pre-existing) |
| gtest_cas_namespace_life_id.cpp | `CasNamespaceLifeId.ZeroIncarnationIsUnconstructible` (bare `EXPECT_THROW`, not `expectThrowsCode`) | guarded (pre-existing; has `CasNamespaceLifeIdDeathTest` twin) |
| gtest_cas_text_format.cpp | `CasFormatTraits.CompleteUniqueAndGated`'s `traitsFor(FormatId::Roster)` check (bare `EXPECT_THROW`) | guarded (pre-existing; has `CasFormatTraitsDeathTest` twin) |

Tree-wide re-scan (parsing every file under `src/Disks/tests/` that mentions `LOGICAL_ERROR` for
guard-range membership, not file presence): zero unguarded `expectThrowsCode`/`EXPECT_THROW`/
`EXPECT_ANY_THROW` sites remain. The other ~249 bare `EXPECT_THROW`/`EXPECT_ANY_THROW` hits found
tree-wide wrap calls that throw non-abort codes (`CORRUPTED_DATA`, `FILE_DOESNT_EXIST`,
`NETWORK_ERROR`, generic `DB::Exception`/`std::exception` from unrelated backends) — spot-checked
several (`gtest_cas_slot_occupy.cpp:238`, `gtest_ca_wiring.cpp:1127`) against their production throw
sites to confirm.

## 5. Gate results

**Release — full CA gate**, under the shared `unit_tests.lock`, generated suite filter (see note
below on the generator fix): **1977 ran / 1977 passed / 277 suites, 2 disabled, exit 0**
(`build/hygiene_gate_release_v2.log`).

The count is baseline 1976 (`build/t1c3_gate.log`, pre-fix) **+1**: exactly
`CasRefCatalogFormat.EncodeRejectsLiveWithRemovalStartedRound`, the new release-side test born from
splitting the inline `LOGICAL_ERROR` check out of `RemovalStartedRoundIsRequiredExactlyForRemoving`
(§2). No other test count changed: the fence fix changes existing tests' internals, not their count;
the `GenericCasUpdateCannotDeleteOrReplaceCatalogIdentity` relocation (§3) keeps exactly one
release-side test (as before the relocation) plus an ASan-only death twin that release never runs.

Suite count parity with baseline (277) required a generator-tooling fix, disclosed in full: the
untracked `tmp/generate_cas_suites.sh` grepped only `^TEST(_F)?\(` and so never saw `TEST_P`-based
suites, whose gtest runtime name is `<INSTANTIATE_TEST_SUITE_P prefix>/<base suite>`
(`InMemory/CasBackendContract`, `Local/CasBackendContract`, `WinnerShape/CasGcCompletedRemovalFenceRace`).
A first release-gate attempt built its filter list against a fixed EXCLUDE_REASONS-based release-only
copy of the script that never resolved this gap, producing a filter three suites (33 tests) short of
baseline (274/1943) — the coverage-losing outcome the reviewer correctly flagged as unacceptable. This
was a suite-list **generation** bug, not a test-relocation regression: the code changes in §1–§3 never
evicted a release-run test into a sanitizer-only suite. The generator now also scans for
`INSTANTIATE_TEST_SUITE_P(prefix, suite` (multi-line aware, via `perl -0777`) and folds the prefixed
name into the accounted-for set; regenerated against `build_asan` it produces exactly 295 suites (4
excluded: the 3 pre-existing generic exclusions plus `CasRefInstallSafetyDeathTest`, gated on
`MEMORY_TRACKER_DEBUG_CHECKS` rather than `DEBUG_OR_SANITIZER_BUILD` and so genuinely absent from an
ordinary ASan build — a pre-existing, unrelated gap, recorded here rather than silently worked
around). For the release run specifically, the 18 suite names that are `*DeathTest` variants (which
never exist in a release build, by design) are excluded with an explicit reason each in a
release-only copy of the generator (`tmp/generate_cas_suites_release.sh`, not committed — matches the
untracked status of the base script); that produces the released-eligible 277, matching baseline
exactly.

**ASan — full CA gate**, same shared lock, 295-suite filter: launched via `nohup` with the
`HYGIENE_ASAN_DONE_V2=` marker into `build_asan/hygiene_gate_v2.log`. [Result recorded once the run
completes — see final message.]

## Artefacts

- `build/hygiene_fence_red.log` — red-first repro of the fence-race test before the fix
- `build/hygiene_fence_fixed.log` — green, both parameters, epilogue `EXPECT`s now executing
- `build/hygiene_touched_suites.log` — `WinnerShape/CasGcCompletedRemovalFenceRace.*:CasRefCatalogFormat.*` green (26/26) after Part 1+2
- `build_asan/hygiene_gate.log` — first ASan attempt, aborts (exit 134) on the second unguarded site
- `build_asan/hygiene_targeted_check.log` — `CasRefCatalogDeathTest.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentityAborts` green after the §3 fix
- `build/hygiene_gate_release_v2.log` — final release gate, 1977/1977/277, exit 0
- `build_asan/hygiene_gate_v2.log` — final ASan gate (in progress / see final message for outcome)
- `tmp/generate_cas_suites.sh` — TEST_P-aware suite generator (untracked tooling, fixed this pass)
- `tmp/generate_cas_suites_release.sh` — release-only copy adding the 18 `*DeathTest`-absent-in-release exclusions (untracked tooling)
