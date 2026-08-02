# Task 5 Steps 9 and 10a report

## Step 9: active prose

Updated only the active comment sites named by the readiness audit:

- `planRefCleanup` now documents its actual snapshot, durable-cursor, and optional-checkpoint
  boundaries. It no longer describes cleanup items or a durable `Removed` snapshot.
- `Pool::dropNamespace` now describes the `Removing` catalog transition, folded terminal evidence,
  exact catalog-row removal, and independent perpetual-janitor reclamation.
- The namespace-file incarnation tests now describe folded evidence and retained dead-life debris,
  without the removed Pending/Completed physical-empty gate.
- The shard-incarnation tests now describe absent catalog lives as inert debris and opaque life ids as
  the structural ABA boundary.
- `docs/superpowers/cas/codecs.md` now describes the actual strict tagged-text `cas_fold_seal` with
  `rfl`, `btr`, and `cnd` records rather than the removed protobuf ref-shard and cleanup-run fields.

No executable Step 9 behavior changed. In particular, the in-memory `RefLifecycle::Removed` replay
state remains intact.

## Step 10a: unmatched adopted-parent observability

At the existing unmatched `parent_ref_lives` branch, `buildRefWalkPlan` now increments the dedicated
`CasGcUnmatchedAdoptedParentLives` event and writes one warning containing the exact
`NamespaceLifePhysicalId`. It then follows the existing path unchanged: increment the aggregate
dropped-row count, emit no plan row, and continue. The observation does not mint a life, add an
anomaly, or affect destructive suppression.

The focused test uses the friend-only real plan builder and a scoped capture of the existing `CasGc`
logger. It proves a one-event delta, one exact-id log line, and unchanged absence from the plan,
parent-state output, and successor-state output.

## Evidence

- TDD RED: `build_asan/build_task5_step10a_red.log` failed at link time only because
  `CasGcUnmatchedAdoptedParentLives` did not yet exist.
- Focused ASan build: `build_asan/build_task5_step9_10a_final.log` completed all 21 Ninja steps and
  linked `unit_tests_dbms`.
- Focused ASan tests: `build_asan/test_task5_step9_10a_final.log` ran 2 tests from
  `CasGcRefWalkPlan`; 2 passed, 0 failed.
- Log inspection found no compiler warning/error or sanitizer signatures in the final logs.

The stuck-removal threshold and fold-seal `btr`/`cnd` admission bounds remain deliberately out of
scope because the readiness audit records their missing product contracts.
