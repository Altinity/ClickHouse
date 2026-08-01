# Task 5 Checkpoint 7.5a — `CatalogLifecycleReconciler`

## Scope

Extracted the catalog-only adopted-parent completed-removal drain from `Gc` into
`CatalogLifecycleReconciler`. The component owns deterministic eligible-row selection and the
`N + 1` catalog rescan loop, returning authority, exact-row resolution, retired predecessor lives,
the final clean catalog cut, and deletion count. It performs no hot ref `LIST`, ref walk, fold,
seal publication, `gc/state` mutation, runtime invalidation, or physical deletion.

`Gc` continues to validate and supply the adopted parent and fence callback. Both normal rounds and
healthy `REBUILD` apply every returned runtime retirement before accepting only
`{Authoritative, DrainComplete}` as permission for downstream work. Damaged-state `REBUILD` remains
the no-parent, zero-catalog-mutation path.

## Tests

The genuine RED was observed before production implementation:

```text
ninja -C build unit_tests_dbms
fatal error: CatalogLifecycleReconciler.h file not found
```

The direct empty-drain API test drove that extraction. Later tests were added after the minimal
implementation and are regression coverage, not claimed as additional TDD RED cycles.

- `CatalogLifecycleReconciler.*` directly covers N=0, N=3 returned-resolution cuts, exact-life
  retirement on fence movement, token-conflict retry from the mandatory snapshot, and non-fence
  authority exceptions before CAS and after mandatory resolution.
- `CasGcFrontierGate.CompletedRemovalDrainUsesNPlusOneCatalogReads` retains the ordinary post-`LIST`
  cut assertion separately from the pre-`LIST` drain.
- `WinnerShape/CasGcCompletedRemovalFenceRace.*` proves absent/replacement winners stop successor
  work after `Gc` applies the returned exact retirement.
- `CasGcFrontierGate.HealthyRebuildUsesTheCatalogLifecycleReconciler` and
  `CasGcFrontierGate.DamagedStateRebuildDoesNotDeleteCompletedRemovingRows` prove the two `REBUILD`
  routes.

Observed GREEN evidence:

- `build/task5_checkpoint75a_final_build.log`: `unit_tests_dbms` linked cleanly.
- `build/task5_checkpoint75a_final_focused_test.log`: 11 focused tests passed.
- `build/task5_checkpoint75a_prefold_tla.log`: all 18 expected pre-fold TLA+ outcomes passed.

The full CA filter was started in `build/task5_checkpoint75a_full_ca_test.log` (1752 tests), but its
process ended during the long `CasGcStopStart` suite without a final gtest summary. It is not claimed
as passing evidence.

## Review

Reviewed the extraction for a second deletion driver, redundant catalog `GET`, swallowed authority
exception, use of the pre-`LIST` cut as the plan cut, and physical deletion. None was introduced.
