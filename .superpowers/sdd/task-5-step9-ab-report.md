# Task 5 Step 9 A/B: catalog-cut namespace-file contract

## Scope

- Exact base observed before edits: `53e2a135980db75eed21b2ad2b553093308088e2`.
- Replaced the synthetic catalog-retirement headline in
  `gtest_cas_ns_file_incarnation.cpp` with one coupled production-path contract.
- Snapshot codec fields and `RefLifecycle::Removed` were deliberately not changed; they belong to
  Step 9 slice C.

## Contract exercised

`CasNsFileIncarnation.ColdReaderUsesCatalogCutWhileOldFileSurvivesRemoval`:

1. births a real life through `beginPartWrite`, `stageManifest`, `precommitAdd`, and `promote`;
2. writes an exact-life namespace file and makes LIST omit only that `_files` key;
3. executes real `dropNamespace`, terminal fold N, and pre-fold drain N+1 until the catalog row is
   absent;
4. proves direct backend HEAD/GET still return the old bytes;
5. opens a second cold `Pool` on the same backend and pool but a distinct server-root slot, and proves
   fresh name resolution follows the absent catalog cut;
6. performs a real same-name rebirth with distinguishable successor bytes and proves an exact retained
   predecessor life returns old bytes or NotFound, never successor bytes; and
7. asserts the complete touched-key set contains no `/_cleanup/` marker.

The behavior was already GREEN after the current catalog-cut implementation, so this slice is a
characterization test, not a production fix.

## Non-vacuity controls

Each controlled fixture mutation was compiled, run in isolation, observed RED at its intended pin,
and then removed. The final test-file SHA-256 returned exactly to
`a5241b808b6bdd04ba66b96695e889b42aaec7292770041eb4b5f13ee1b9069e`.

- Catalog authority: republishing a fresh `Live` row immediately before cold resolution failed the
  fresh-absence assertion. Log: `build_debug/step9_ab_mutation_catalog_exact.log`.
- Physical residue: exact-token deletion of the hidden old `_files` object failed the
  `logical removal must not depend on physical empty` assertion. Log:
  `build_debug/step9_ab_mutation_physical_exact.log`.
- Marker absence: one counted HEAD against
  `p/cas/ns/state/mutation/_cleanup/probe` failed the zero-marker assertion and named the exact key.
  Log: `build_debug/step9_ab_mutation_marker_exact.log`.
- Janitor observation: revealing the hidden file immediately after the explicit precondition LIST
  prevented the hole counter from advancing during N/N+1. The phase-specific delta assertion failed
  with `actual: 1 vs 1`, proving the precondition can no longer satisfy it vacuously. Log:
  `build_debug/step9_ab_fix1_mutation_exact.log`.

## Verification

- Final build/link: `ninja -C build_debug unit_tests_dbms`
  (`build_debug/step9_ab_fix1_final_build.log`).
- Exact contract: `build_debug/src/unit_tests_dbms
  --gtest_filter=CasNsFileIncarnation.ColdReaderUsesCatalogCutWhileOldFileSurvivesRemoval`
  (`build_debug/step9_ab_fix1_final_exact.log`).
- Owning suite: `build_debug/src/unit_tests_dbms --gtest_filter=CasNsFileIncarnation.*`
  (`build_debug/step9_ab_fix1_final_suite.log`).

## Shared-worktree notes

- The initial compile exposed the now-required direct `CasPartWriteTxn.h` include and one dead local
  helper; both are part of this slice.
- Root authorized one unstaged compatibility edit in the concurrently-owned
  `gtest_cas_ref_writer.cpp`: a `RefCowMap` proxy loop now uses value structured binding. Its pre-edit
  SHA-256 was `c66982c22bfca9028a4dd483bbf86f312755de8582ac22f5ea4422e191a7b9e4`. This file is
  not owned, staged, or committed by this slice.
- Two intermediate full builds were externally interrupted by concurrent source snapshots (transient
  NUL reads in unrelated headers, then a temporarily unused decommission test helper). Retrying after
  their owners finished produced the final clean build above.
- The protected aggregate `.superpowers/sdd/task-5-report.md` remains untouched by this slice.
