# T6 draft report — destruction enablement (UNVERIFIED-DRAFT)

Worktree `/home/mfilimonov/workspace/ClickHouse/draft-t6`, branch `draft/t6`, base `8e86c58a0f5`.

**NOTHING IN THIS DRAFT WAS BUILT OR RUN.** No `ninja`, no `unit_tests_dbms`, no praktika, no TLC. Every
behavioural claim below and in the committed comments is `UNVERIFIED-DRAFT`. The finisher owns the
evidence.

- Commit 1 — `030f1697e74` `ca: draft — gc universe authoritative flip (UNVERIFIED-DRAFT, no runs)`
- Commit 2 — see the bottom of this file (this report is added into it)

---

## 1. Step 0 — T6a's verdict consumed

`t6-frontier-attribution.md` returns BENIGN-TRANSIENT and `t6a-review.md` grades the prerequisite
**discharged**. Carried into the finisher's criteria: post-flip healthy rounds must also show **zero
`no usable checkpoint` anomalies**, not only `unproven == 0` and `probe_budget == 0`.

One base-SHA note that matters for evidence hygiene: `git merge-base --is-ancestor 477fe702a7a HEAD`
reports **not an ancestor** at my base, yet the frontier instrumentation (`FoldResult::FrontierUnproven`,
`FrontierDeficit`, the `committed_through` ceiling test) is present in the tree. The content was
integrated by some other commit, so the review's derivation applies, but the review's own gate evidence
(its `1826/1826` run) is about a different tree object than mine — consistent with the review's finding 7.

## 2. The flip and the rename (`Gc/CasGc.h`)

`AuthoritativeForTest` → `Authoritative`, swept tree-wide by exact token
(`perl -pi -e 's/\bAuthoritativeForTest\b/Authoritative/g'` over `git grep -l`'s 7 files); the post-sweep
`git grep -n AuthoritativeForTest -- src/ tests/ utils/` returns zero. `kDefault = Authoritative`.

The `kDefault` comment was rewritten to invariant + reason only: the gate opens on a complete,
catalog-proven frontier; any of {anomaly, carried hold, unproven namespace, empty universe} suppresses
POOL-WIDE, because blob in-degree is pool-wide and no ownership partition of the blob space exists. No
task ids, no test lists, no counts. The `UniversePolicy` preamble lost its Stage-A framing and its
"PRODUCTION-UNREACHABLE, BY CONSTRUCTION" paragraph, which the flip makes false.

`StageA_Suppressed` **kept its name** — the brief names it explicitly as the negative-policy seam. It is
now a stage reference in a live enumerator; see open question O-1.

## 3. Dead-arm removal (`Gc/CasGc.cpp`), and the ceiling argument

The ceiling test is the first statement of `while (expected)`:
`if (*grounding->committed_through < *expected) { … break; }`. Between it and every site below,
`expected` is reassigned only on the crossing path, which `continue`s back through the ceiling test, and
`grounding` is never reassigned. Therefore **every statement after the ceiling test runs under
`*expected <= *grounding->committed_through`**. All three removals follow, and in each the surviving path
is the stricter one:

| Arm | Why dead | What survives |
|---|---|---|
| frozen-tail break's second conjunct `*grounding->committed_through < *expected` | negation of the ceiling invariant, so always false — the whole `if` is unreachable | the ceiling test bounds the walk; removing only the conjunct would ACTIVATE dead code, so the whole `if` went |
| absent-record `if (*expected <= *grounding->committed_through) hold(...) else frontier_proven = true;` | condition always true | unconditional `hold` — the stricter branch |
| `const bool catalog_names_this_namespace = true;` + `if (!catalog_names_this_namespace)` | constant-true, in old and new code alike | nothing; walk targets come from the catalog read, so the branch was a check on an impossibility |

Round finiteness now rests solely on `committed_through` being a round-start snapshot
(`readCheckpointWitnesses` reads `_ckpt` once per namespace per round and it is never re-read inside the
walk). The other two `frontier_proven = true` sites — the ceiling arm's
`resolved_through == *grounding->committed_through`, and
`nextRefLogIdWithinCommittedFrontier` returning `nullopt` after an applied record — are what still prove
an honest end of stream, so removing the third did not remove the only proof producer.

**Scope disclosure (not asked for by the brief).** With the frozen-tail break gone, `WalkTarget::frozen_tail`
had no reader at all — written at three `push_back` sites, read nowhere. I removed the field, the three
`std::nullopt`/`tail`/`max(tail, offending_position)` arguments, and the paragraph of header prose asserting
that the field bounds the walk. Reason: an unread field plus a comment claiming it bounds the round is a
false statement about the code, and the field's absence is what makes the ceiling test's role legible. The
`intake_tails_advanced`/`unchanged`/`below_cursor` classification and its three-class comment are unchanged.

### Bucket decision: REMOVE both

`no_catalog_entry` and `append_above_frozen_tail` became unfireable-by-construction, so I removed the
struct fields, the `FrontierUnproven::NoCatalogEntry`/`AppendAboveFrozenTail` enumerators, and their rows
in `count`, `total` and `describe`.

Reasoning for removal over retention-with-a-comment:

- **The sum invariant survives exactly.** The invariant is
  `sum(buckets) == frontier_namespaces - frontier_proven`. Every unproven namespace routes through the
  single `result.frontier_deficit.count(unproven_reason)` call plus the `probe_budget` and `fold_aborted`
  additions. A bucket no exit can set contributed `0` to that sum on every round; dropping a term that is
  identically zero cannot change the equality.
- **`unattributed` is the actual fence, and it is unaffected.** `unproven_reason` still initialises to
  `Unattributed`, so an exit that stops naming itself still surfaces as a number.
- Retaining them would publish cause names the code cannot produce, which is the opposite of what the
  deficit exists for — an operator reading `no_catalog_entry=0` learns nothing, but a reader of the enum
  would reasonably conclude that exit exists.
- **No external consumer.** `git grep` for `no_catalog_entry|append_above_frozen_tail|NoCatalogEntry|AppendAboveFrozenTail|frontier_deficit|frozen_tail` outside the two `CasGc.*` files returned nothing (no test, no soak assertion, no model).

I also **added** one metric rather than only removing: `fold_reduce` now publishes
`frontier_complete` alongside `suppress_destructive`, so a test can read the gate's own verdict instead of
recomputing the formula from the tally. Without it the Step-1b `frontier_complete == true` assertion would
have had to re-derive `namespaces > 0 && proven == namespaces` in the test, which agrees with a wrong
formula as readily as with the right one.

## 4. The kill-shot edit

`gtest_cas_list_liar_end_to_end.cpp`:

- `AnEntirelyHiddenNamespacesEdgeIsRefusedByTheProductionDefault` now drives `gc.runRegularRound()` with
  **no policy argument** and keeps the stronger pair of assertions (`head(...).exists` **and**
  `deleteTotal() == 0`). Its rationale is rewritten: the blob survives because `hidden` is catalog-`Live`
  and, having sealed no cursor, cannot prove its own frontier, so
  `frontier_proven == frontier_namespaces` fails on member count alone.
- `TheSameHiddenNamespacesBlobSurvivesEvenWhenTheUniverseIsDeclaredAuthoritative` **deleted** — post-flip
  it is the same round as the case above with a weaker assertion set.
- Section banner rewritten; the "BOTH WAYS" framing and the Stage-A attributions are gone.
- The parenthetical in `TheSameBlobDrainsOnceHiddenGenuinelyProvesItsOwnFrontier` claiming "the fold's
  frozen-tail rule … refuses to read past a round's own listed tail even by exact key" is **deleted**: it
  described the arm the review proved dead, so it was already false before this change.
- The same collapse was applied in `gtest_cas_gc_frontier_gate.cpp`: its
  `TheSameHiddenPlusOneSurvivesEvenWhenTheUniverseIsDeclaredAuthoritative` twin is deleted, and
  `HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` runs the production path.

## 5. Step 1b — the five arms

Placed in `gtest_cas_gc_frontier_gate.cpp` (fixture fit: it owns `buildPoolWithWorkAtEverySite`, the
delete-family helpers and the probe-budget pool factory). Two new shared helpers: `runRoundCapturingGate`
(reads `frontier_complete`/`suppress_destructive` off `fold_reduce` and the tally off `fold_ref_intake`)
and `expectEveryDeleteFamilyInert` (four families plus an aggregate catch-all, each with its own message).

| Arm | Test | Notes |
|---|---|---|
| healthy | `AHealthyCatalogRoundOpensTheGateAndReclaims` (new) | `frontier_complete == true`, `suppress_destructive == false`, `frontier_namespaces > 0`, `proven == namespaces`, and the condemned blob actually drains. Replaces `ProductionDefaultDestroysNothingEvenWithEveryProofGreen`, whose premise the flip makes false. |
| (3a) negative policy | `EveryInventoriedDestructiveSiteIsInertUnderSuppression` (converted) | now passes `StageA_Suppressed` explicitly via `runRoundCapturingGate`, asserts both booleans, uses the per-family helper, and its control drains on the production path — which is also what makes "suppressed on that term alone" true rather than assumed. |
| (1) one anomaly | `AnUndecodableCheckpointAnomalySuppressesEveryDeleteFamily` (new) | a namespace whose `_ckpt` is present but garbage. Asserts the anomaly count is **nonzero** first, so a silent exit cannot make the test pass for the wrong reason. Discloses in-comment that it also leaves the frontier incomplete, so it pins "an anomaly suppresses", not "only the anomaly does". |
| (2) carried hold | `ACarriedHoldSuppressesEveryDeleteFamily` (new) | committed gap: `_ckpt.committed_through = {1,2}` with only `{1,1}` written, so the walk reads `{1,2}` absent below the ceiling and holds. `ASSERT_GT(tables_held, 0)` on the first round is the non-vacuity gate; the measured rounds are the LATER ones, because the gate's second term reads the seal, not this round's anomaly list. |
| (3b) empty universe | `AGenuinelyEmptyUniverseRefusesTheFrontierDespiteZeroEqualsZero` (pre-existing) | already the empty-floor test with per-family assertions and a non-vacuous control. Left as is. |
| (3c) budget | `AnExhaustedProbeBudgetSuppressesEveryDeleteFamily` (new) | seals the quiet namespace's cursor **while it is still listed** (under `StageA_Suppressed` so nothing drains early), asserts the cursor is nonzero, then hides the prefix. Without the seal the namespace is a no-genesis shape the budget never reaches, and the test would have measured a different suppressor. |

Also converted to an explicit `StageA_Suppressed`, because their subject is the suppression and the flip
would otherwise silently invert them: `ASuppressedRoundDoesNotAdvanceTheGenerationPruneCursor`,
`TheHandOffReclaimIsInertUnderSuppression`,
`TheOrphanManifestSweepAndItsCursorAreInertUnderSuppression`,
`CasOrphanNomination.SuppressedRoundNominatesNothing`.

## 6. Closeout inventories (reproducible-inventory rule)

All three run at `8e86c58a0f5` in `/home/mfilimonov/workspace/ClickHouse/draft-t6`.

### `git grep -in "task 7b" -- 'src/' 'tests/' 'utils/'`

**17 non-doc files**, not the 15 the plan and the design spec expect. The same command at the audit
baseline `ce312f547c3` also returns 17 (`git grep -il … | wc -l` = 17 at both SHAs), so the plan's figure
was wrong when written rather than gone stale. The design spec's decomposition (`8 STAGE-A RETURN ITEM +
7 STAGE-A CONTRACT`) sums to 15 and omits two files that carry a bare "Task 7b" reference and neither
banner: `Gc/CasGc.cpp` and `gtest_cas_gc_frontier_gate.cpp`.

Files: `src/…/Gc/CasGc.cpp`, `src/…/Gc/CasGc.h`, `src/Disks/tests/gtest_ca_wiring.cpp`,
`src/Disks/tests/gtest_cas_gc_frontier_gate.cpp`, `src/Disks/tests/gtest_cas_gc_log.cpp`,
`tests/broken_tests.yaml`, `tests/integration/test_cas_replicated_relink/test.py`,
`tests/integration/test_content_addressed_drop_pool_member/test.py`,
`tests/integration/test_content_addressed_gc_s3/test.py`,
`tests/integration/test_content_addressed_ref_snaplog/test.py`,
`tests/integration/test_content_addressed_shared_pool/test.py`,
`tests/queries/0_stateless/04290_content_addressed_no_leftovers.sh`,
`tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh`,
`tests/queries/0_stateless/05008_ca_gc_snap_prune.sh`,
`tests/queries/0_stateless/05010_content_addressed_mounts_gc_health.sh`,
`utils/ca-soak/scenarios/cards/s28_s33_corner.py`,
`utils/ca-soak/scenarios/framework/assertions.py`.

Note `utils/ca-soak/scenarios/tests/test_leftovers_stage_a.py` is in the brief's edit list but carries no
"Task 7b" token, so it is an 18th file the grep alone does not find — the greps are not a partition of the
work.

### `git grep -n "STAGE-A RETURN ITEM"` — 8 non-doc files

`Gc/CasGc.h`, `gtest_ca_wiring.cpp`, `gtest_cas_gc_log.cpp`, `tests/broken_tests.yaml`, and the four
stateless `.sh` tests.

### `git grep -n "STAGE-A CONTRACT"` — 7 non-doc files

`test_cas_replicated_relink`, `test_content_addressed_drop_pool_member`, `test_content_addressed_gc_s3`,
`test_content_addressed_ref_snaplog`, `test_content_addressed_shared_pool` (2 sites),
`cards/s28_s33_corner.py`, `framework/assertions.py` (2 banner sites + 2 message strings).

### Post-cleanup

All three greps return **zero non-doc hits** (remaining hits are the plan, the spec, the midpoint audit and
the Stage-A RESULTS record — historical documents). Zero `broken_tests.yaml` entries remain.

## 7. Per-site closeout table

| Site | What it asserts now |
|---|---|
| `tests/broken_tests.yaml` | All four entries and their shared banner removed; the four tests are no longer excused. |
| `gtest_cas_gc_log.cpp` `EmitsStartFinishWithCounts` | A `saw_deleted` round is required, `deleting_finish_idx >= marking_finish_idx`, and `total_deleted > 0` — the deleted count reaching the Finish row, plus the ordering that says the row describes the round it names. |
| `gtest_ca_wiring.cpp` displacement test | `EXPECT_EQ(after.unreachable, 0u)` restored: the displaced closure is reclaimed, not merely recognized. |
| `test_content_addressed_gc_s3` | Renamed back to `test_gc_reclaims_dropped_blobs`. Polls with an early exit to `final <= baseline`, requires `objects_deleted + manifests_deleted > 0`, keeps the fully-proven-frontier round check, and flips the suppressed-phase check to `== 0`. |
| `test_content_addressed_drop_pool_member` | Polls to `final <= blobs_baseline` and requires GC's own bookkeeping to report a deletion; the existing `dangling=0`/`unaccounted=0` fsck checks are untouched. |
| `test_content_addressed_ref_snaplog` (2 sites) | Content drains to `content_baseline` with a bookkeeping cross-check; and `ca-gc-dryrun` on the drained pool must report `preview_deletes=0` (was: any value). |
| `test_content_addressed_shared_pool` (2 sites) | Both drain to `baseline`, each with the pool-wide `_gc_bookkeeping` cross-check flipped to `deleted > 0`. The crash-recovery arm keeps its own point: the hard kill left nothing that survives the drop. |
| `test_cas_replicated_relink` | The soundness guard is restored as a bounded reclaim loop: at least one of the abandoned attempt's blobs must be reclaimed, which is what makes the earlier "they survived during the stall" assertion mean the relink pin rather than an inactive GC. |
| `framework/assertions.py` `assert_no_leftovers` | Zero tolerance: any leak class of any prefix fails. The `_suppressed`/`_hard` split and the permitted-family verdict are gone; `dangling` stays a separate unconditional check. |
| `framework/assertions.py` `assert_reclaimable_drained` | The `blobs == 0 && _manifests > 0` pass arm is gone; `reclaimable` (blobs + `_manifests`) speaks for itself again. |
| `cards/s28_s33_corner.py` | `not monotone` restored as the whole condition: neither `root_dirs` nor `CasRootGet` may grow with ever-created tables — both halves of D1. |
| `tests/.../test_leftovers_stage_a.py` → `test_leftovers.py` | Renamed (`git mv`) and flipped: a manifest leak now FAILS and its count and class must reach the verdict; the mixed-leak case must name every class rather than a total; `dangling`, `unaccounted` and the clean-pool pass are unchanged in intent. |
| four stateless `.sh` tests | Banner paragraphs removed. The tests themselves are **byte-identical** — `git diff --stat` shows deletions only. |

## 8. Expected gate and lane effects for the finisher

**Stateless.** `05008`, `04290`, `04295` should flip green unchanged, with the drain-to-`PENDING = 0` loop
converging — that convergence is the end-to-end proof of the flip. `05010` should flip green because the
per-round `destructive work SUPPRESSED` warning stops being emitted on a healthy pool; if it still warns,
read the `unproven:` clause on the line, which now names the bucket.

**Integration.** All six lanes in the plan's Step-3 command go through a banner flip: `gc_s3`,
`shared_pool` (2), `drop_pool_member`, `ref_snaplog` (2), `cas_replicated_relink`.
`test_content_addressed_s3` carries no banner and should be unaffected.

**Unit.** The CA gate must be regenerated (`utils/cas-gate/generate_cas_suites.sh build`) before the run:
this draft adds four test names and removes two, all inside already-claimed suites, so the suite list
should be unchanged — but that is a prediction, not a measurement.

**Finisher checklist**

1. `git rebase`-free integration in order **T5 then T6**; resolve the `Gc/CasGc.h`/`Gc/CasGc.cpp`/`gtest_cas_gc_log.cpp` overlap (see §9).
2. Full CA release gate with the **generated** filter, on the integrated tree, **before** trusting anything here — this is also what discharges the T6a review's findings 2 and 3.
3. Full CA ASan gate (the ASan binary's own suite list).
4. Both CA-S3 stateless lanes; the six-selector integration command; `test_content_addressed_gc_s3` in particular.
5. Assert **delete-family metrics nonzero** in the lane logs and **zero anomalies**, including **zero `no usable checkpoint` anomalies** (the T6a carry) and zero `unattributed` in any `unproven:` clause.
6. Re-run the soak scenario unit tests (`utils/ca-soak/scenarios/tests/`) — `test_leftovers.py` is new content under a new filename.
7. Every `UNVERIFIED-DRAFT` claim in the two draft commits must be either confirmed by a run or corrected before the commits become non-draft.

## 9. Parallel-draft overlap (noted, not resolved)

`draft/t5` (probe-A deletion) also edits `Gc/CasGc.h`, `Gc/CasGc.cpp` and `gtest_cas_gc_log.cpp`.
My touches in those files:

- `CasGc.h`: the `UniversePolicy` block, `runRegularRound`'s `policy` doc, `frontier_complete`'s doc, the
  `FrontierUnproven`/`FrontierDeficit` block, one sentence at `kRetainRollupRepeatPasses`.
- `CasGc.cpp`: `FrontierDeficit::total`/`describe`/`count`; the hand-off gate comment; the universe
  comment before `WalkTarget`; the `WalkTarget` struct and the three `push_back` sites; the
  `catalog_names_this_namespace` removal; the absent-record arm; the frozen-tail arm; the retain-rollup
  and `skipped`-by-class comments; the destructive-gate block; the `fold_reduce` metric addition; the
  orphan-sweep argument.
- `gtest_cas_gc_log.cpp`: only `EmitsStartFinishWithCounts` (header comment, three counters, the final
  assertion block).

Phases were **not** renumbered anywhere (that is T5's).

## 10. Open questions

- **O-1 (naming).** `StageA_Suppressed` keeps a stage name in a live enumerator after the stage it names
  is over. The brief pins the identifier, so I did not touch it. A follow-up rename (e.g.
  `NoUniverseSupplied`) would be mechanical but touches ~12 sites and would invalidate this draft's
  comments; the finisher should decide, not guess.
- **O-2 (the flip is not one line).** The plan says the flip changes only what `frontier_complete` may
  become, and "if the flip requires touching any other term, the flip is wrong". It does not touch a gate
  term — but it does flip `planManifestCursorPage`'s `catalog_recovery_authoritative` argument from
  effectively-always-`false` to `true` in production, because that call site sits inside
  `if (!suppress_destructive && …)` and was therefore unreachable before. I replaced the duplicated
  `policy == UniversePolicy::Authoritative` expression with the already-computed `universe_authoritative`
  and stated the reason in a comment; the argument's VALUE is unchanged relative to the policy. It still
  deserves a reviewer's eye: it is the one place where the flip enables a behaviour the plan's "ONE LINE"
  framing does not mention.
- **O-3 (the unmarked blast radius, the biggest risk in this task).** At the base SHA, bare
  `gc.runRegularRound()` appears **240 times across 24 files** under `src/Disks/tests/`
  (`git grep -c "runRegularRound()" 8e86c58a0f5 -- 'src/Disks/tests/'`; 239 in 22 files at this
  draft's HEAD, after my conversions). Every one of them changes meaning with the
  flip: previously each folded but never deleted; now each may delete. I converted the ones I could
  identify statically as suppression-subject tests (listed in §5) and the five explicit
  `UniversePolicy::kDefault` drive sites. **I cannot enumerate the rest without running the suite.** The
  plan's marker inventory does not cover this class at all — none of these sites carries a banner. The
  finisher's first CA gate run is the detector; expect failures shaped as "an object the test expected to
  still exist was reclaimed", concentrated in `gtest_cas_gc_round.cpp` (62 calls),
  `gtest_cas_gc_fold.cpp` (30), `gtest_cas_gc_ack_floor.cpp` (27), `gtest_cas_gc_hold_grammar.cpp` (27)
  and `gtest_cas_gc_rebuild.cpp` (21). The correct fix for each is the same as the ones I made: if the
  test's subject is the suppression, pass `StageA_Suppressed` explicitly; if not, the assertion needs to
  accommodate a reclaiming round.
- **O-4 (`gtest_cas_gc_bounded_walk.cpp`).** Its header essay attributed the round's bound to the listed
  tail. I rewrote it to name `committed_through`, because the frozen-tail arm it described is the one the
  review proved dead. Its section-(d) title also contradicted its own body — the test asserts
  `frontier_proven == 1` and that reclamation happens, while the title said "STOPPING AT THE TAIL IS NOT A
  FRONTIER PROOF"; I retitled it. **I believe every assertion in that file is unaffected** (test (a)
  pins `last_folded_ref_id == {1, planted}` and its fixture sets
  `committed_through = {1, planted}`, so the ceiling stops the walk at exactly the same id the dead arm
  would have), but that is a reading of the code, not a run. It is the file the finisher should check first.
- **O-5 (`_manifests` retention on a healthy pool).** `assert_reclaimable_drained` no longer tolerates a
  `_manifests` residual. The Stage-A justification was that manifest bodies are deleted at a gated site
  without being condemned first. Post-flip they should drain, but if any structural reason keeps them from
  draining that is now a soak FAILURE rather than a pass — which is the intent, and also the assertion
  most likely to be the first honest red.
