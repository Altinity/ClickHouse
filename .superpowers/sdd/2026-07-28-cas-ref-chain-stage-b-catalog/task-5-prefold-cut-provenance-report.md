# Task 5 pre-fold cut provenance report

**Status:** DONE

**Base:** `143df2c4bf52f2fa3447d0526e4776e8b2b2a5f1`

**Implementation commit:** `7fec7a009b7` (`ca: prove pre-fold cut provenance at intake boundary`)

## Outcome

The pre-C++ phase-0 seam is closed compositionally. `CaRefPreFoldDrainCore` remains the only owner of
adopted-parent validation, catalog drain, conclusive CAS resolution and fresh-cut ordering. It now
exports the fresh catalog cut as an immutable full-catalog token/value pair to a tiny ref-plan
consumer boundary. The consumer records the exact pair it used and derives whether the modelled
drained life enters the plan.

`IntakeConsumesFreshPostDrainCut` requires the consumed pair to equal the pair produced by
`TakeFreshCut`. `_sab_intake_uses_predrain_cut` completes the honest drain and fresh-cut transition,
then deliberately feeds ref-plan intake the earlier drain observation. This goes RED on the intended
provenance invariant. `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE` takes the honest route through a real
exact `Removing` deletion and proves the drained row is absent from both the consumed cut and plan.

`CaRefDeltaIntakeCore` was not modified. It continues to own only walk-plan key-set and fold semantics;
no second lifecycle protocol was introduced. Its `_sab_adoptbeforecommit`/`NoMissedFold` pair is still
not cited as cut-provenance evidence.

No C++ file changed and no Task 5 production checklist item was marked complete.

## TDD RED

The production change this control catches is wiring plan intake to the drain observation, or another
stale catalog snapshot, instead of the immutable pair returned by `TakeFreshCut`.

The sabotage/property was added before the honest intake assignment and run with:

```bash
/usr/bin/java -XX:+UseParallelGC -cp tmp/tla2tools.jar tlc2.TLC \
  -metadir build/test_task5_prefold_cut_provenance_red_meta_20260801 -workers 1 \
  -config docs/superpowers/models/CaRefPreFoldDrainCore_sab_intake_uses_predrain_cut.cfg \
  docs/superpowers/models/CaRefPreFoldDrainCore.tla \
  > build/test_task5_prefold_cut_provenance_red_20260801.log 2>&1
```

Expected and actual: RED only on `IntakeConsumesFreshPostDrainCut`; 209 generated / 111 distinct /
depth 7. The decisive trace values are:

- drain observation: `observedEntry = "removing"`, `observedToken = 1`;
- honest post-drain cut: `cutEntry = "absent"`, `cutToken = 2`;
- sabotaged consumption: `entry = "removing"`, `catalog_token = 1`,
  `plan_has_life = TRUE`.

A child log analyzer independently confirmed the invariant, counts and trace.

## GREEN and non-vacuity witness

The minimal honest implementation assigns the exact `cutToken`/`cutEntry` pair to `intakeCut` inside
the existing `AdoptFromCut` transition. There is no new lifecycle state or ordering transition.

The complete focused runner was rerun after final model cleanup:

```bash
bash docs/superpowers/models/run_prefold_drain.sh \
  > build/test_task5_prefold_cut_provenance_final_20260801.log 2>&1
```

Result: 13/13 expectations met with no warnings or unexpected errors. The new rows were:

- `_sab_intake_uses_predrain_cut` → RED `IntakeConsumesFreshPostDrainCut`;
- `_safe` → GREEN;
- `_witness_drained_row_absent_from_intake` → RED-as-witness
  `WITNESS_DRAINED_ROW_ABSENT_FROM_INTAKE`.

The post-commit direct honest control was:

```bash
/usr/bin/java -XX:+UseParallelGC -XX:-UsePerfData -cp tmp/tla2tools.jar tlc2.TLC \
  -metadir build/test_task5_prefold_cut_provenance_safe_clean_final_meta_20260801 -workers 1 \
  -config docs/superpowers/models/CaRefPreFoldDrainCore_safe.cfg \
  docs/superpowers/models/CaRefPreFoldDrainCore.tla \
  > build/test_task5_prefold_cut_provenance_safe_clean_final_20260801.log 2>&1
```

Result: GREEN; 2,272 generated / 874 distinct / depth 14, with zero states left in the queue and no
warnings.

The clean witness run was:

```bash
/usr/bin/java -XX:+UseParallelGC -XX:-UsePerfData -cp tmp/tla2tools.jar tlc2.TLC \
  -metadir build/test_task5_prefold_cut_provenance_witness_clean_meta_20260801 -workers 1 \
  -config docs/superpowers/models/CaRefPreFoldDrainCore_witness_drained_row_absent_from_intake.cfg \
  docs/superpowers/models/CaRefPreFoldDrainCore.tla \
  > build/test_task5_prefold_cut_provenance_witness_clean_20260801.log 2>&1
```

Result: the intended witness is reached at 208 generated / 110 distinct / depth 7. Its final state has
`Removing`/token 1 in the drain observation, `absent`/token 2 in both the fresh and consumed cuts, and
`plan_has_life = FALSE`. A child analyzer independently confirmed these values and the absence of
warnings.

## Delta regression controls

The unchanged focused Delta plan controls were rerun serially. They are regression evidence only, not
the provenance proof:

```bash
/usr/bin/java -XX:+UseParallelGC -XX:-UsePerfData -cp tmp/tla2tools.jar tlc2.TLC \
  -metadir build/test_task5_prefold_cut_provenance_delta_plan_safe_clean_meta_20260801 -workers 1 \
  -config docs/superpowers/models/CaRefDeltaIntakeCore_plan_safe.cfg \
  docs/superpowers/models/CaRefDeltaIntakeCore.tla \
  > build/test_task5_prefold_cut_provenance_delta_plan_safe_clean_20260801.log 2>&1

/usr/bin/java -XX:+UseParallelGC -XX:-UsePerfData -cp tmp/tla2tools.jar tlc2.TLC \
  -metadir build/test_task5_prefold_cut_provenance_delta_adapter_red_clean_meta_20260801 -workers 1 \
  -config docs/superpowers/models/CaRefDeltaIntakeCore_sab_adaptermints.cfg \
  docs/superpowers/models/CaRefDeltaIntakeCore.tla \
  > build/test_task5_prefold_cut_provenance_delta_adapter_red_clean_20260801.log 2>&1

/usr/bin/java -XX:+UseParallelGC -XX:-UsePerfData -cp tmp/tla2tools.jar tlc2.TLC \
  -metadir build/test_task5_prefold_cut_provenance_delta_plan_witness_clean_meta_20260801 -workers 1 \
  -config docs/superpowers/models/CaRefDeltaIntakeCore_witness_planbuilt.cfg \
  docs/superpowers/models/CaRefDeltaIntakeCore.tla \
  > build/test_task5_prefold_cut_provenance_delta_plan_witness_clean_20260801.log 2>&1
```

Results, independently analyzed:

| Control | Expected/actual | Generated / distinct / depth |
|---|---|---:|
| plan safe | GREEN | 2,307 / 257 / 10 |
| adapter mint | RED `PlanKeySetExact` | 12 / 11 / 3 |
| plan-built witness | RED `WITNESS_PLAN_BUILT` | 2 / 2 / 2 |

All three logs are free of warnings and unexpected errors. An initial parallel launch of these direct
controls and the pre-fold witness produced JVM `hsperfdata` lock warnings. Those warning-bearing logs
are superseded and are not evidence; every result above comes from the serial `*_clean_20260801.log`
replacement.

## Hygiene

Commands and redirected logs:

```bash
bash -n docs/superpowers/models/run_prefold_drain.sh \
  > build/test_task5_prefold_cut_provenance_bash_n_20260801.log 2>&1

git diff --cached --check \
  > build/test_task5_prefold_cut_provenance_cached_diff_check_20260801.log 2>&1

for f in docs/superpowers/models/CaRefPreFoldDrainCore_*.cfg; do
  if ! rg -q 'SabotageIntakeUsesPreDrainCut' "$f"; then echo "$f"; fi
done > build/test_task5_prefold_cut_provenance_cfg_constants_clean_20260801.log 2>&1
```

All three correct final audits were silent. The constant audit names no incomplete configuration. A
first attempt used ripgrep's `-L` option as if it were GNU grep's files-without-match option; the child
analyzer exposed the nonempty output, that command was discarded, and the explicit loop above replaced
it before commit.

`git diff --cached --name-status` independently confirmed exactly 18 implementation paths, two new
configs, no C++, no unrelated path and no `.superpowers/sdd/task-5-report.md`.

## Files

Implementation commit `7fec7a009b7`:

- Modified `docs/superpowers/models/CaRefPreFoldDrainCore.tla`.
- Added `CaRefPreFoldDrainCore_sab_intake_uses_predrain_cut.cfg` and
  `CaRefPreFoldDrainCore_witness_drained_row_absent_from_intake.cfg`.
- Updated every existing `CaRefPreFoldDrainCore_*.cfg` for the new sabotage constant; the safe and
  takeover-witness configs also check the provenance invariant.
- Updated `run_prefold_drain.sh`, `CaRefPreFoldDrainCore_RESULTS.md`,
  `CaRefDeltaIntakeCore_RESULTS.md`, `README.md`, and `2026-07-28-v9-phase-RESULTS.md`.
- Updated the authoritative complete-cut spec §9 and Task 5 plan Step 11.

Report follow-up:

- Updated `task-5-prefold-amendment-report.md` and `progress.md` to close the former open seam.
- Added this report.

## Ownership rationale

The interface has one owner. The pre-fold model already knows when CAS debt is resolved and when the
fresh cut is taken; therefore it is the only model that can attach honest provenance to the cut. The
consumer boundary carries only `{catalog_token, entry, plan_has_life}`. It does not reproduce parent
adoption, lease takeover, ambiguity resolution, rescan, `DEFER`, or `REBUILD` policy. Delta remains a
pure downstream consumer whose separate key-set proof ensures adapters cannot mint rows.

The full-catalog token is part of the equality deliberately: comparing only the row value would fail to
detect a stale snapshot whose value happens to match the fresh one.

## Self-review

- TDD ordering is preserved: sabotage/property RED was recorded before honest intake wiring.
- Mutation check: replacing `cutToken`/`cutEntry` with `observedToken`/`observedEntry` in the consumer
  is exactly the committed sabotage and fails the named invariant.
- Non-vacuity is explicit: the witness requires an actual token-advancing exact deletion and observes
  the drained row absent from both consumed cut and plan.
- `CaRefDeltaIntakeCore.tla` is byte-unchanged; lifecycle ownership was not duplicated.
- `_sab_adoptbeforecommit` remains documented only as a `NoMissedFold` control.
- Every configuration binds the new constant, the runner checks the named violation, and the final
  runner exhausts all 13 expected verdicts.
- The spec, plan, README, focused results and phase matrix now state the proved claim and its scope.
- No C++ file, production checklist item, branch, rebase, amend, or destructive cleanup was involved.

## Preserved dirt

Preserved unchanged:

- `.superpowers/sdd/task-5-report.md`;
- the five obsolete untracked `RemovalReady` configs:
  `CaRefCatalogCore_sab_ckptdeletewhileremoving.cfg`,
  `CaRefCatalogCore_sab_promoteunderhold.cfg`,
  `CaRefCatalogCore_sab_removalreadywithoutitem.cfg`,
  `CaRefCatalogCore_sab_skipremovalready.cfg`, and
  `CaRefNsCleanupStaleLeaderCore_sab_promoteunderhold.cfg`;
- all other pre-existing modified and untracked workspace files.

## Concerns

None.
