# Checkpoint 7.5a fix round 1 — reconciler result algebra and non-vacuous retirement

## Scope

Fixed the two Important findings against `2a5bb563b26` without starting immutable-runtime or janitor work.

- `CatalogLifecycleReconciler::reconcile` now selects an eligible completed-removal row before its
  component-level clean-return fence check. An eligible row always goes through
  `CasRefCatalog::deleteCompletedRemovingAtSnapshot`, so fence loss before its erase CAS resolves the
  exact row as `ExactRowStillPresent`; `DrainComplete` is reserved for a catalog cut with no eligible row.
- `seedCompletedRemoving` now recovers and retains the predecessor runtime while its catalog row is
  still `Live`. The existing absent/replacement fence-race cases therefore exercise real
  `Pool::invalidateRemovedCatalogLife` behavior and force the next name-based recovery to resolve away
  from the predecessor life. Pointer identity is used only to prove that a predecessor runtime was
  initially resident, not as a post-retirement contract.

## TDD evidence

The combined initial RED is in `build/task5_checkpoint75a_fix_r1_tdd_red.log`:

- `InitialFenceLossReportsEligibleRowStillPresent` received `DrainComplete` instead of
  `ExactRowStillPresent`.
- Both `Absent` and `Replacement` parameter cases found no recovered predecessor runtime, proving the
  old fixture made retirement vacuous.

The final retirement assertion was also mutation-checked. With the ordinary `Gc` caller's loop over
`retired_lives` temporarily removed, `build/task5_checkpoint75a_fix_r1_retirement_mutation_red.log`
failed both winner shapes because the resident life still equaled the predecessor. The production loop
was restored before the final build and focused run.

## Verification

- `build/task5_checkpoint75a_fix_r1_post_mutation_build.log`: `unit_tests_dbms` compiled and linked;
  no warnings or errors.
- `build/task5_checkpoint75a_fix_r1_post_mutation_focused.log`: 9/9 focused
  `CatalogLifecycleReconciler` and `CasGcCompletedRemovalFenceRace` tests passed.
- `build/task5_checkpoint75a_fix_r1_full_ca.log`: expanded CA filter completed with 1,752/1,752
  tests passed, zero failures or skips; two disabled tests were not run.
- `build/task5_checkpoint75a_fix_r1_prefold_tla.log`: all 18 expected pre-fold TLA+ outcomes passed.
- `git diff --check` passed for both modified source files.

Every build/test log above was independently analyzed by a child agent as required.

## Self-review

Reviewed the final diff against both findings and checked the following failure modes:

- An eligible initial row cannot return `{FencedOut, DrainComplete}`: selection occurs first, and the
  mandatory resolution cut classifies the exact row.
- The fence-before-CAS path performs exactly the initial selection `GET` and the primitive's mandatory
  resolution `GET`; no additional catalog read was introduced.
- A genuinely clean no-eligible cut retains its component-level fence check before returning the final
  catalog snapshot.
- Both absent and replacement winner shapes start with a real recovered predecessor and finish with a
  name-based resolution whose life differs from that predecessor.
- The tests do not require permanent runtime-object reuse and add no production test seam.
- The temporary mutation was fully restored; `CasGc.cpp` has no working-tree diff.

No remaining correctness concern was found within this fix-round scope.
