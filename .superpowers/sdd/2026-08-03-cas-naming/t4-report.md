# Task 4 report — gtest suite prefix `Cas*` -> `CAS*` and the cas-gate

## Step 1 — enumeration (counts re-derived, and they drifted)
`gtest_ca*.cpp`: **132** files, **300** distinct suites, **297** `Cas`-prefixed, **296** after excluding
`CascadeWriteBuffer`. The plan expected 131 / 266 / ~265 — a uniform **+31**, consistent with the tree having
grown since the plan was written. The *structure* matches exactly (one `Cascade` exclusion; the same three
non-`Cas` suites), so this is drift, not a structural mismatch.

Exhaustiveness checked from the other side: `git grep -ohP '^TEST(_F|_P)?\(\s*\KCas\w+' -- src/` returns 297
names, and every file holding one matches `gtest_ca*.cpp`. No CAS suite lives outside the glob.

## Step 2 — the collision pre-check changed the mechanism
**62 of the 296 suite names are also production symbols** (`CasPool`, `CasBackend`, `CasFsck`, `CasEvent`,
`CasLayout`, `CasGcRound`, …). The plan's Step-2 sed is a whole-word rename over the whole tests directory;
run as written it would have rewritten every *use of those production classes inside the test files*.

So the classification that made a safe mechanism possible: **all 62 colliding names are plain
`TEST(Suite, Case)` suites — none is a fixture.** Only 2 suites are fixture-based
(`CasBackendContract`, `CasGcCompletedRemovalFenceRace`) and neither collides. Therefore renaming *only the
macro's first argument* is sufficient and cannot touch a production reference.

What was actually applied:
- **2023 macro sites** across 130 files: the first argument of `TEST` / `TEST_F` / `TEST_P` only.
- The 2 fixture classes: whole-word, scoped to their own single file each (definition + `TEST_P` +
  `INSTANTIATE` second argument).
- Instantiation prefixes `CasInMemory`/`CasLocal`, scoped to `gtest_cas_backend_contract.cpp`.
- **79 comment/reference sites** naming a *non-colliding* suite, so comments stay true — guarded against
  rewriting source **file** names (`\bName\b(?!\.(?:cpp|h))`).
- **455 `Suite.TestCase` references** to *colliding* names. That shape is gtest-filter syntax and cannot be
  the production symbol, which is what makes it safe to rename while the bare name is not.

### A defect I introduced and caught
The `Suite.TestCase` lookahead `(?=\.(?:\w|\*))` also matched `Name.h` in `#include` paths, renaming 435
include tokens to files that do not exist. The build failed loudly with `fatal error: … CASBackend.h file not
found`. Fixed by reverting every `CAS<X>.<ext>` token whose real file is `Cas<X>.<ext>`, checked against the
126 actual `Cas*.{h,cpp}` basenames in the tree — not against a guess.

## Step 3 — gate scripts
`generate_cas_suites.sh`: all five prefix literals (`== Cas*` x2, `!= Cas*`, the `^TEST…\s*Cas` file probe,
`grep '^Cas'`), every error message, and the header prose now say `CAS`. The 19 `KNOWN_COMPILE_GUARDED`
death-test names got the same prefix. `CascadeWriteBuffer` added to `EXCLUDE_REASONS` — required, because the
check is now `!= CAS*` and its accidental `Cas` prefix no longer matches.
`run_cas_gate_per_suite.sh` contains no `Cas` literal (verified by grep); unchanged.

## Step 4 — the gate found a suite my own enumeration missed
First generator run **failed**:
`CasWinnerShape/CASGcCompletedRemovalFenceRace (INSTANTIATE_TEST_SUITE_P prefix does not start with 'CAS')`.

A **third** instantiation prefix existed. Both the plan's Step-1 command and my repeat of it use `grep -oP`,
which is line-based, and this `INSTANTIATE_TEST_SUITE_P(` spans lines — so the prefix was invisible to the
enumeration and visible only to the gate. Re-scanned with a multi-line-aware parser: exactly three prefixes,
all now `CAS*`. This is the gate working as designed, and it is why the invariant is checked against the
built binary rather than the source.

### Results
- `build/naming_t4_build2.log`, `build3.log` -> `NINJA_EXIT=0`, no `error:` lines.
- Generator: `wrote 278 suites … (equals the 'CAS*' filter set; 4 excluded, 0 unclaimed)`, exit 0.
  278 reconciles exactly: 296 source suites − 19 compile-guarded + 1 extra parameterized spelling
  (`CASBackendContract` is exposed under both `CASInMemory/` and `CASLocal/`).
- Per-suite gate under `flock`: `TOTALS: pass=278 fail=0 abort=0`.
- The old filter is now empty of CAS: `--gtest_list_tests | grep '^Cas'` returns only `CascadeWriteBuffer`.

## The death-test split proven on BOTH arms, per the standing rule
A pass/fail run cannot distinguish "the split works" from "the preprocessor ignored it", so the arms were
listed, not inferred:
- Source vs list: the 19 `*DeathTest` suites in `gtest_ca*.cpp` and the 19 in `KNOWN_COMPILE_GUARDED` are the
  same set, all `CAS`-prefixed (set difference empty both ways).
- **Release** (`build`): exposes **0** `*DeathTest` suites — the expected absent arm.
- **ASan** (`build_asan`, rebuilt, `NINJA_EXIT=0`): exposes **18** — all `CAS`-spelled.
- **Debug** (`build_debug`, rebuilt, `NINJA_EXIT=0`): exposes **19**, byte-identical to
  `KNOWN_COMPILE_GUARDED`.
- The one absent from ASan is `CASRefInstallSafetyDeathTest`, gated on `MEMORY_TRACKER_DEBUG_CHECKS`. Its own
  comment documents why: that macro is defined only under `!NDEBUG`, and sanitizer builds define `NDEBUG`, so
  the guard is a no-op there. Absence in ASan is the designed state, not drift — which is exactly why a
  debug build was needed to prove the 19th.
- The old spelling is gone from all three binaries: `grep -c '^Cas[a-z].*DeathTest'` = 0 in each.

## Folded in at the lead's request
- `cas_test_helpers.h` and `gtest_ca_wiring.cpp` cited a PoC file `gtest_content_addressed_metadata.cpp`
  that no longer exists. Both sentences reworded to state the behaviour and drop the citation; per the
  comment policy, deleting the provenance is preferred to rewriting it.
- `utils/ca-soak/soak/checker.py` had a comment naming the gtest `CasGcLeak.…`; renamed with the rest.

## Deliberately not renamed
- `CascadeWriteBuffer` — accidental `Cas` prefix, now excluded with a reason.
- `gtest_cas_request_control.cpp:257` "a per-call scripted Backend for `CasRequestController` tests" — reads
  as tests *of the production class*, whose name is unchanged; a production class name is never false here.
- `utils/ca-soak/scenarios/cards/s41_wide_insert_baseline.py:114` `"CasProtocol"`, `"CasPool"`, `"CasText"`,
  `"CasBuild"`, `"CasRef"` — stack-frame **symbol substrings** for profile attribution that match production
  class/file prefixes, not suite references. Renaming them would silently break the attribution buckets.
- `utils/ca-soak/scenarios/BACKLOG.md` suite mentions — dated run records, per the ruling in `t1-report.md`.

## Pre-existing findings, not fixed here
`gtest_cas_blob_digest.cpp:70` cites `CasBlobRef.h` and `gtest_cas_ref_statemachine.cpp:117` cites
`CasRefStateMachine.cpp`; **neither file exists in the tree**. Stale filename citations, unrelated to this
rename, left alone rather than converted into a differently-wrong name.

## Carried forward to Task 5
`src/Disks/tests/gtest_cas_settings.cpp:65` cites
`tests/config/config.d/content_addressed_storage_policy_for_merge_tree_by_default.xml`, which Task 5 renames.
Task 5's seds cover `tests/` and `ci/`, not `src/`, so this line must be updated there explicitly.
