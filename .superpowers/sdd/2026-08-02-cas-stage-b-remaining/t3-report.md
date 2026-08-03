# Task T3: Task 7 closure — decommission evidence

Status: **DONE**. Two-phase heal + operator-facing UX message landed; both STOP-worthy findings the
lane surfaced were escalated, ruled on, and (one of them) closed as its own separate slice
(`ca: fsck — dead-life residue is janitor-pending, not corruption; observe-then-cut classification`).
Full release CA gate (278 suites, 1989 tests) and the ASan decommission/fsck/janitor/layout suites
both green. The integration lane is green: 2/2 in 171s.

## Starting point

Finished the draft (`d3412ec54e2`, `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t3-draft-report.md`)
cherry-picked cleanly onto `8e2d46ba38d`. Verified the draft's core assumption by reading
`Pool::mountWritable`/`CasServerRoot.cpp`: for the fixture's already-owned, already-epoched starting
state, `claimOwnerOrThrow` and `allocateWriterEpoch` both take fast paths that never call
`list("p/roots/victim/", ...)`, so the new fixture's injection ordering (arm b's
`MutateCatalogBetweenRetirementReadsBackend`) is sound. Release suite green 35/35 including both new
arm tests immediately after the cherry-pick.

## Mutation demonstrations (mandatory wording: load-bearing mutation demonstration performed after
implementation; mutation reverted; patch and failing output preserved)

- **Mutation (i)** — skip the `_ckpt`-absence `CORRUPTED_DATA` throw in `CasDecommission.cpp`. The
  draft's `if (false && ...)` pattern does not compile (`-Wunreachable-code` is a hard error on
  statically-provable dead code in this tree); reshaped to a clean block-deletion of the guard.
  Finding: does **not** go red. A deeper, independent guard in `CasRefLedger::dropNamespace` →
  `chooseRecoveryGrounding` (`CasRefCkpt.cpp:152`) throws the identical `CORRUPTED_DATA` for the same
  condition via a different message ("CAS recovery grounding: a Live or Removing namespace requires a
  readable `_ckpt` with life_epoch"). Recorded as intentional defense-in-depth layering per the
  controller's ruling, not a cleanup candidate without a separate decision — kept as-is.
- **Mutation (ii)** — drop the `SCOPE_EXIT`/`request_gc_round` wake. 3 tests red exactly as predicted:
  `RemovingWithCheckpointResumesTerminalAndKeepsSlotForGc`,
  `PartialRemovalProgressStillWakesGcWhenLaterNamespaceFails`,
  `FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound`.
- **Mutation (iii)** — skip `victim_still_owned` (arm a). Same `-Wunreachable-code` reshaping as (i).
  6 tests red, including the predicted primary `VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot`.

All three: applied, rebuilt, ran, captured failing output, reverted, rebuilt, confirmed green (35/35)
before moving to the next. Tree confirmed byte-identical to HEAD via `diff` after each revert.

## STOP finding 1 — pre-existing red integration test, discovered live

`test_content_addressed_drop_pool_member::test_drop_dead_pool_member_heals_the_pool` failed
deterministically against BOTH a stale `build/programs/clickhouse` and (after a from-scratch rebuild,
confirmed via the embedded git hash matching `git log -1`) a binary built fresh from this exact tree:

```
AssertionError: node2	1	0	4	0	0	0	0	0	catalog still owns victim namespaces in Removing/Creating state; GC completion is required before slot retirement
assert 0 == 1
```

Root cause traced to commit `224aacd8eb9` ("ca: close namespace removal and decommission duties",
2026-08-02 03:57:55): it introduced the `victim_still_owned`/`fresh_retirement_catalog` retirement
fence, which does not distinguish a catalog row that was already `Removing` before decommission
started from one this same call just legitimately transitioned from `Live`. Under that design, any
non-empty victim pool always refuses synchronous slot retirement — the test's older "pool heals
synchronously" contract (its scenario commit predates `224aacd8eb9` by a long way) was never
reconciled with the tightened fence. The test has been red since `224aacd8eb9` landed, roughly 1.5
days before this dispatch — not a regression introduced by this task.

**Ruling**: the fence is correct (`CasDecommission.cpp` untouched); the test's synchronous-heal premise
is the stale side. Normative contract: "retirement is FORBIDDEN while any entry owned by that root
remains" — no carve-out for rows the same call transitioned; encoded by
`RemovingWithCheckpointResumesTerminalAndKeepsSlotForGc` and
`FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound` by name.

**Implemented (a)+, the two-phase heal the contract actually promises**, in
`test_content_addressed_drop_pool_member/test.py::test_drop_dead_pool_member_heals_the_pool`:

1. First `DROP POOL MEMBER` call: asserts `slot_removed == 0`, `namespaces_removed >= 1` (the drop
   half did its work), and the arm-(a) warning is present — a success-with-pending-GC, not a failure.
2. Drives `SYSTEM CONTENT ADDRESSED GC RUN '<disk>'` (the same idiom
   `test_cas_replicated_relink.py`'s `gc_round` helper uses) in a bounded retry loop (up to 30
   rounds), re-issuing `DROP POOL MEMBER` after each round, because folding a fresh terminal and
   pruning its catalog row are two separate GC rounds by design (the fold-then-prune handoff
   `CasDecommissionCatalogDuties.FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound` documents).
3. Second (eventual) call: asserts `slot_removed == 1`, warnings empty — the pool healed, in two
   phases. Step 8's mounts-table check moved to after this point (only true once the slot actually
   retires).

Kept the STAGE-A CONTRACT reclamation-scale assertions unchanged, per instruction not to pre-empt T6's
banner flip — only the retirement mechanics changed.

## STOP finding 2 — dead-life residue vs. `ca-fsck`'s fail-close (closed as its own slice)

Getting the two-phase heal to actually complete (which never happened before, since the old test
always failed earlier) surfaced a SECOND failure: the final `ca-fsck` step refused with
`BAD_ARGUMENTS`, `"12 key(s) under this pool name no namespace LIFE"`.

**Investigation** (both questions the controller asked, answered empirically, not guessed):

- **What are the 12 keys — do they parse as life-keyed?** Yes. Via `ca-fsck --detail`, all 12 carry
  the reason `"physical life id is absent from the catalog cut; deferred"` — the benign sub-case,
  never `"unrecognized key under the namespace ownership tree"` (the malformed sub-case). All under
  one syntactically valid, nonzero, 32-lowercase-hex physical life id (t2's life).
- **Do they drain under more GC rounds?** No. Drove 6 additional explicit `GC RUN` rounds, re-running
  `ca-fsck --detail` after each; the count held at exactly 12, identical keys, every round. Confirmed
  by code: `Gc::runNamespaceJanitorPage` passes the SAME `suppress_destructive` flag blob/manifest GC
  uses straight into `NamespaceJanitor::runOnePage`'s `suppress_deletes` parameter
  (`Gc/CasNamespaceJanitor.cpp`: `if (ambiguous || suppress_deletes || catalog_cut.life_index.resolve(*life_id)) continue;`
  — the janitor visits and resolves every candidate but skips every `deleteExact` call under
  suppression). The non-drain across 6 rounds is exactly the Stage-A-correct outcome, not evidence of
  a permanent leak; it is expected to drain once T6 flips `UniversePolicy::kDefault` off
  `StageA_Suppressed`. (The metrics-level "smoking gun" — `namespace_cleanup` phase rows showing
  `janitor_keys>0`/`janitor_deleted=0` from `system.content_addressed_garbage_collection_log` — was
  not independently pulled from the finished lane's persisted data directory after one attempt at
  locating the on-disk table; the code-gate evidence above was accepted as sufficient per the
  controller.)

**Ruling**: implement (b), per an independent codex consultation (`tmp/fsck_debris_consult_answer.md`
in the MAIN worktree) and the shipped
`docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:391` spec, which the prior
`ca-fsck` behavior directly contradicted. Landed as its own commit
(`ca: fsck — dead-life residue is janitor-pending, not corruption; observe-then-cut classification`),
ahead of this task's closure commit:

1. Split `FsckClass::LifelessKey` (now malformed/ambiguous-catalog only, still hard) from a new soft
   `FsckClass::JanitorPending` (canonical key, catalog-absent-at-a-post-observation-cut) — new
   `FsckReport` fields `namespace_janitor_pending`/`_bytes`/`_lives`, none in `kFsckHardFindings`
   (stayed at 5; no bump needed).
2. Fixed the `parseNamespaceFileKey` parser asymmetry (it used to accept any nonempty bytes after
   `_files/` while the writer rejected dirty names) via a shared `isCleanRelativeNamespaceFileName`
   helper used by both the writer and the parser.
3. Restructured `CasFsck.cpp`'s unscoped physical scan to observe-then-cut: buffer canonical
   candidates during the LIST, take a fresh catalog cut AFTER the listing completes, classify against
   THAT cut (matching `NamespaceJanitor::runOnePage`'s own ordering and its
   `PostListCatalogCutProtectsConcurrentCreationWithOneGet` regression test).
4. `CommandFsck.cpp`: new `janitor-pending` detail label, a non-throwing note (count/bytes/lives), and
   removed the stale exception text falsely claiming `ca-decommission` refuses on these keys.
5. `InterpreterSystemQuery.cpp`: 3 new SQL columns after `lifeless_keys`.
6. New `gtest_cas_fsck.cpp` tests: `CanonicalDeadLifeResidueIsJanitorPendingNotHardFinding`,
   `LifeAdmittedBetweenNamespaceListingAndLaterCutIsNotResidue` (mirrors
   `CasNamespaceJanitor.PostListCatalogCutProtectsConcurrentCreationWithOneGet`),
   `MalformedNamespaceTreeShapesStayHardFindings` (dirty `_files` name, zero life id, uppercase life
   id, unknown kind directory — all stay hard).
7. Updated this task's integration test's final `ca-fsck` assertion to `lifeless_keys=0` and
   `janitor_pending` nonzero (not pinned to exactly 12: by the time this step runs, `t1`'s own row has
   also been pruned by the same catalog-fold mechanism, so the pool-wide unscoped count is not a
   single namespace's key count).

## User-directed UX change (approved, narrow production edit)

Reworded the arm-(a) operator-visible warning in `CasDecommission.cpp` from
`"catalog still owns victim namespaces in Removing/Creating state; GC completion is required before
slot retirement"` to an affirmative progress report with a real (not decorative) count:

```
"pool member decommission underway: all " + std::to_string(victim_owned_count)
+ " namespace(s) owned by this member are marked for removal; upcoming GC rounds "
  "perform the final cleanup — re-run this command afterwards to retire the slot"
```

`std::any_of` became `std::count_if` to produce the count. Arm (b)'s message
("catalog changed after the victim ownership check; refusing slot retirement against a stale cut")
is untouched — a genuine concurrent-change refusal, a different situation. Tree-wide grep for the old
fragment (`"catalog still owns victim namespaces"`, `"GC completion is required before slot
retirement"`) found exactly 3 live consumers, all updated: the renamed unit test
`VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot` (now asserts on
`"pool member decommission underway: all 1 namespace(s)"`, that fixture's fixed count) and both
integration-test occurrences (phase 1's assertion and phase 2's loop, both now asserting the stable
substring `"pool member decommission underway"`). Two historical docs quote or discuss the old string
(`docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` quotes it verbatim;
`docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r6_findings.md` is unrelated prose) —
left untouched as historical audit record, not edited in place.

## Ownership inventory (Step 4, re-verified against this tree)

`grep -n "list(\|starts_with\|victim" .../Tools/CasDecommission.cpp` confirms no `list(` call exists in
the file — namespace ownership is decided exclusively via `entry.ns.string() == victim_srid` or
`.starts_with(victim_namespace_prefix)` (the trailing slash makes the prefix one canonical path
component) against one immutable catalog cut, re-validated per-entry and again in the retirement tail.
`deleteListedPrefix` calls are confined to `staging/<srid>/` and `serverRootDataPrefix(<srid>)` loose
debris under the victim's own server-root — never namespace-name-prefix deletion. No life-key prefix
fallback exists.

## Naming collision check

`git log --oneline cas-gc-rebuild | grep -i "decommission"` (checked at closure time): the subject
`ca: decommission — catalog-exact duties; retirement fenced on owned entries` is unused in history —
free to use.

## Suites and lane result

- Release CA gate (`utils/cas-gate/generate_cas_suites.sh build` → 278 suites, 0 unclaimed →
  `build/src/unit_tests_dbms` with the generated colon-joined `--gtest_filter`):
  **1989/1989 passed** (`build/t3f_gate_run2.log`).
- ASan: decommission/fsck/janitor/layout suites, **113/113 passed** (`build_asan/t3f_fsck_asan_run.log`).
- Integration lane, final run with the fully rebuilt server binary (embedded git hash matches
  `git log -1` on this tree): **2/2 passed in 171.67s** (`build/t3f_final_lane.log`). The two
  `docker info` `ERROR` lines at the top of that log are pre-flight probe hiccups the harness retries
  past; the cluster ran fine.

## Deviations from the draft/dispatch, disclosed

- Draft's `if (false && ...)` mutation pattern for (i) and (iii) does not compile under this tree's
  `-Wunreachable-code`; reshaped to clean block-deletion (same demonstration, different mechanics).
- The two-phase test rework and the fsck (b) slice were not in the original T3 dispatch — both are
  direct consequences of the two STOP escalations and their rulings, landed as instructed (fsck slice
  as its own separate commit, ahead of this one).
- Did not independently pull the `namespace_cleanup` GC-log metrics from the finished lane's on-disk
  data directory after one attempt (per the controller's explicit one-attempt cap); relying on the
  code-gate evidence for the suppression verification instead.
