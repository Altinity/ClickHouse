# Task 5 pre-fold-drain amendment report

## Result

The phase-0 amendment is committed as `320d4a01eea` (`ca: model pre-fold `Removing` catalog drain`).
It replaces the obsolete same-round post-adoption deletion design before C++ Task 5 work resumes.
No C++ source or test was changed.

## Decisions implemented

- The catalog-only pre-fold drain runs after lease acquisition against the authoritative adopted parent,
  before `DEFER`, hot stream LIST, successor catalog cut, fold, `REBUILD` adoption, or successor seal
  adoption.
- A deletion requires the complete exact `Removing` row plus matching parent cleanup evidence and no
  durable hold. Ambiguous or conflicting CAS results are reread and resolved; a row that may still be
  the same exact row aborts successor work.
- Full-catalog tokens require serial rescan: after every CAS outcome the drain rereads the complete
  catalog and restarts selection. The two-row companion model proves that a decision cannot skip the
  remaining eligible row after the first exact deletion changes the token.
- An already-issued stale request may land only while its adopted proof remains current; the explicit
  B-drained takeover trace makes A lose and B resolves absence before successor work. This is a helping
  barrier, not atomic revocation of an independent catalog CAS.
- Healthy `REBUILD` drains before its fresh cut. Missing or undecodable `gc/state` adopts reconstructed
  authority only, does zero catalog deletion, returns, and leaves drain work to the next invocation.
- There is no lifecycle physical cleanup: `suppress_destructive` does not govern the drain; the perpetual
  `cas/ns/` janitor and orphan-manifest sweep own physical debris. The janitor model captures physical
  ids rather than re-deriving a logical name.
- Status wording records Task 4d at `6a3dd6a9245`, mandatory catalog/bootstrap at
  `2b8475fc6f6` and `21ce9e99f4d`, Step 1 core at `a600c2e433c` with two open follow-ups, and Task 5
  production Steps 2–11 as unimplemented.

## Exact committed files

- `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md`
- `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md`
- `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md`
- `docs/superpowers/models/CaRefCatalogCore.tla`
- `docs/superpowers/models/CaRefCatalogCore_RESULTS.md`
- `docs/superpowers/models/CaRefCatalogCore_churn.cfg`
- `docs/superpowers/models/CaRefCatalogCore_finding_briefreconcileinv.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_deleteunderhold.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_deletewithforeignevidence.cfg`
- deleted `docs/superpowers/models/CaRefCatalogCore_sab_deletewithoutcleanupattempt.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_deletewithoutevidence.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_deletewithoutexactobservation.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_floorretainsdeadname.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_janitoreatsnewborn.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_reconcilelivecreator.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_reconcilestaletoken.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_sameincarnationrebirth.cfg`
- `docs/superpowers/models/CaRefCatalogCore_sab_zombiegolive.cfg`
- `docs/superpowers/models/CaRefCatalogCore_safe.cfg`
- `docs/superpowers/models/CaRefCatalogCore_witness_aliasremnant.cfg`
- `docs/superpowers/models/CaRefCatalogCore_witness_churn3.cfg`
- `docs/superpowers/models/CaRefCatalogCore_witness_orphaneaten.cfg`
- `docs/superpowers/models/CaRefCatalogCore_witness_removalleavesdebris.cfg`
- `docs/superpowers/models/CaRefDeltaIntakeCore_RESULTS.md`
- `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore.tla`
- `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore_RESULTS.md`
- `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore_sab_noincarnation.cfg`
- `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore_sab_rederive.cfg`
- `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore_safe.cfg`
- `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore_witness_captureatdeposition.cfg`
- `docs/superpowers/models/CaRefPreFoldDrainCore.tla` and its results, six sabotage/safe configs,
  witness config, and `run_prefold_drain.sh`
- `docs/superpowers/models/CaRefPreFoldDrainAllRowsCore.tla` and its safe and skip-rescan configs
- `docs/superpowers/models/README.md`
- `docs/superpowers/models/run_refcatalog.sh`
- `docs/superpowers/models/run_nscleanup_staleleader.sh`

## Verification

All outer output was redirected to these unique build logs and independently summarized by a subagent:

- `bash docs/superpowers/models/run_prefold_drain.sh > build/test_task5_prefold_drain_allrows_final_20260801.log 2>&1` — 10/10 expected results. The two-row control is red on
  `AllEligibleRowsResolvedBeforeDecision` (6 generated / 5 distinct / depth 3); its honest control is
  green (22 / 11 / depth 6).
- `bash docs/superpowers/models/run_refcatalog.sh > build/test_task5_prefold_refcatalog_20260801.log 2>&1` — 16/16 expected results, including all four local deletion-proof controls.
- `bash docs/superpowers/models/run_nscleanup_staleleader.sh > build/test_task5_prefold_nscleanup_staleleader_witness_20260801.log 2>&1` — 4/4 expected results, including the capture-before-rebirth witness (13 / 7 / depth 5).
- `run_deltaintake.sh` began with its two relevant unchanged-scope expected-red controls passing;
  the focused fold-commit control was run directly as
  `CaRefDeltaIntakeCore_sab_adoptbeforecommit.cfg` into
  `build/test_task5_prefold_deltaintake_sab_adoptbeforecommit_20260801.log` and violated
  `NoMissedFold` as expected (693 / 296 / depth 6). This is not fresh-cut-provenance evidence; the
  scope correction is recorded below. The full unchanged battery is not claimed as a new amendment result.
- `git diff --check` and `bash -n` on all three modified runners completed without output.

## Self-review

The first independent review found two Important issues. The all-row full-token gap is fixed by the
serial-rescan specification and two-row model. The stale-request wording is fixed to distinguish a
current-proof late landing from the B-drained trace where A loses. The final pre-fold runner was rerun
after both changes and passed all ten expectations.

## Fix round 1/5

Review base: `ad4da3a859b14c44c05829b58509622aa1490e6e`.

Implementation commit: `7fcca3341f2` (`ca: strengthen pre-fold drain model controls`).

- The all-row companion now models an external exact-row deletion while a request is issued. Its
  resolution must return to `scan`, and `CompletionDrained` states `phase = "done" => remaining = {}`.
  The ordinary core retains the completed exact GET when a deposed actor resolves absence, because it
  cannot issue a second current-leader read; the companion owns the continuing global-drain rescan.
- `_sab_nonexactdelete` is the falsifiable exact-CAS control: it accepts a stale full-object token,
  records a sticky consequence, and violates `ExactCatalogCAS`.
- The spec now assigns local deletion proof to `CaRefCatalogCore`, cross-actor ordering to
  `CaRefPreFoldDrainCore`, full-token rescan/external resolution/exactness to
  `CaRefPreFoldDrainAllRowsCore`, captured physical identity to
  `CaRefNsCleanupStaleLeaderCore`, and only the fold/key-set boundary to `CaRefDeltaIntakeCore`.
  Delta's fresh-cut provenance is explicitly **UNVERIFIED**: its existing
  `_sab_adoptbeforecommit` checks `NoMissedFold`, not pre-fold-cut composition.
- Costs now state one adopted-seal GET, at least N+1 complete catalog GETs and N exact catalog CASes
  for N eligible rows, with additional GET/CAS retries for conflict or ambiguity.

Verification (all outer output has a unique build log and each log was independently summarized):

- `bash docs/superpowers/models/run_prefold_drain.sh > build/test_task5_prefold_drain_round1_20260801.log 2>&1`
  exposed the deposed-actor witness regression (10/11); it was traced to routing a non-current A back
  through `ReadDrainCatalog`.
- After the root-cause correction and the independent review's rejected/non-landing outcome control,
  `bash docs/superpowers/models/run_prefold_drain.sh > build/test_task5_prefold_drain_round1_reviewfix_20260801.log 2>&1`
  passed 11/11: `_allrows_sab_nonexactdelete` was red on `ExactCatalogCAS` (21 / 13 / depth 4), and
  `_allrows_safe` was green (80 / 29 / depth 6).
- `bash docs/superpowers/models/run_refcatalog.sh > build/test_task5_prefold_refcatalog_round1_20260801.log 2>&1`
  passed 16/16; `bash docs/superpowers/models/run_nscleanup_staleleader.sh >
  build/test_task5_prefold_nscleanup_round1_20260801.log 2>&1` passed 4/4.
- The unchanged Delta fold-commit control was rerun into
  `build/test_task5_prefold_deltaintake_round1_adoptbeforecommit_20260801.log`; it violated
  `NoMissedFold` (2,648 / 1,117 / depth 9), as expected, and is explicitly not cited as a provenance
  check.
- `bash -n docs/superpowers/models/run_prefold_drain.sh` and `git diff --check` passed without output.

Self-review: each of the five review findings is addressed. The remaining Delta provenance boundary
is deliberately documented as unverified rather than represented by an unrelated red control. An
independent review additionally required rejected and non-landing ambiguous all-row outcomes; both
now have only the complete-scan continuation and the final 11/11 rerun covers the amended model.

## Fix round 2/5

Review clarification: `CaRefNsCleanupStaleLeaderCore` has no cleanup-evidence state. The spec now
states its actual, narrow scope — a perpetual janitor captures a physical id from a LIST page and uses
that captured id for a delayed delete after same-name rebirth. The local evidence/no-hold/exact-row
deletion proof remains solely owned by `CaRefCatalogCore`; the duplicate later janitor-ownership clause
was removed to keep the ownership paragraph non-redundant.

Implementation commit: `06d9937c19e` (`docs: correct janitor model ownership`).

Verification: `rg -n -C 2 'CaRefNsCleanupStaleLeaderCore|per-life cleanup evidence|captured physical id|captured-physical-id|CaRefCatalogCore.*(evidence|exact-row)' docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md docs/superpowers/models/CaRefNsCleanupStaleLeaderCore.tla`
confirmed the model captures and later deletes by physical id while the spec assigns the local proof to
`CaRefCatalogCore`. `git diff --check` passed without output. No TLC run was needed because this round
changes prose only.

## Preserved unrelated dirt

Left untouched: user-owned `.superpowers/sdd/task-5-report.md`; the five untracked obsolete
`RemovalReady` configs (`CaRefCatalogCore_sab_ckptdeletewhileremoving.cfg`,
`CaRefCatalogCore_sab_promoteunderhold.cfg`, `CaRefCatalogCore_sab_removalreadywithoutitem.cfg`,
`CaRefCatalogCore_sab_skipremovalready.cfg`, and
`CaRefNsCleanupStaleLeaderCore_sab_promoteunderhold.cfg`); and all other pre-existing untracked or
modified workspace files.
