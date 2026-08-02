# Task 5 Step 10 follow-up: unmatched-parent non-suppression and stuck-removal recovery

## Unmatched adopted-parent row

Added an end-to-end pin around the existing `RefPlan` boundary. The fixture folds a valid manifest,
injects one stale opaque life row into the adopted seal, and drops the manifest before one
authoritative round. That round must observe exactly one `CasGcUnmatchedAdoptedParentLives` event,
exclude the stale row from the successor seal, and physically delete the valid manifest candidate in
the same round. Thus an unmatched adopted-parent row remains observable, but does not become a
pool-wide destructive-suppression cause.

A controlled production mutation added `dropped_parent_ref_lives > 0` to the single destructive gate.
The new test then failed in both load-bearing places: `manifests_deleted` stayed zero and the manifest
body remained physically present. The mutation was reverted, and the production source checksum was
verified identical to its pre-mutation value before the final build.

## Unreadable removal terminal

The exact stuck-removal diagnostic now gives the two credible exits for an unreadable terminal body:
restore the exact named object, or recreate the pool. It deliberately does not name `REBUILD`, which
cannot promise reconstruction of that exact object. The existing exact-key diagnostic and
non-mutation behavior are unchanged.

## Evidence

- TDD RED: `build_debug/step10_nonsuppress_red_exact.log` ran two tests. The non-suppression fixture
  passed on the current correct behavior; the diagnostic fixture failed only on the two missing
  recovery phrases.
- Baseline GREEN: `build_debug/step10_nonsuppress_green_build.log` linked `unit_tests_dbms`, and
  `build_debug/step10_nonsuppress_green_exact.log` passed 2/2 tests.
- Controlled mutation RED: `build_debug/step10_nonsuppress_mutation_build.log` linked
  `unit_tests_dbms`; `build_debug/step10_nonsuppress_mutation_exact.log` failed the new test on the
  zero deletion count and surviving physical manifest body.
- Final GREEN: `build_debug/step10_nonsuppress_final_build.log` linked `unit_tests_dbms`;
  `build_debug/step10_nonsuppress_final_exact.log` passed 2/2 tests; and
  `build_debug/step10_nonsuppress_final_suite.log` passed 34/34 focused tests from
  `CasGcFrontierGate`, `CasGcRefWalkPlan`, and `CasGcStuckRemoval`.
- `git diff --check` passed for the production file, both test files, and this report.
