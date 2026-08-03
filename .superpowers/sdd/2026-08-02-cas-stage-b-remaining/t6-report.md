# T6 report — destruction enablement (FINISHER)

Worktree `/home/mfilimonov/workspace/ClickHouse/lane-g`, branch `laneg/t6-finish`.

Branch point (exact): `73755caa6e5654f8d93151c9d02c9ef76cd96689`
(`cas-gc-rebuild` tip at dispatch — contains T5's probe-A deletion draft `5b775616c36`, the T3 arc
and the tidy fixes).

## 1. Commits

| SHA | Subject | Contents |
|---|---|---|
| `9d72d33d053` | `ca: draft — gc universe authoritative flip (UNVERIFIED-DRAFT, no runs)` | cherry-pick of `030f1697e74`, one conflict resolved |
| `0f6c31caeaf` | `ca: draft — Stage-A return-item closeout (UNVERIFIED-DRAFT, no runs)` | cherry-pick of `e5b20658964`, clean |
| `a1686eb699a` | `ca: gc — universe authoritative: production destruction enabled (Stage B)` | the flip-side fix wave (compile fix + 3 assertion corrections) |
| `a37f5fc7f81` | `ca: tests — close every Stage-A return item on the destruction contract` | the closeout-side verification + one comment-policy fix |
| (see §11) | `ca: tests — drop_pool_member: dead-life residue drains instead of merely pending` | the integration-lane janitor assertion, re-run green |

The draft's two subjects were preserved on the cherry-picks, and the plan's two subjects carry the
verified waves as ordinary follow-up commits, per the dispatch. No rebase, no amend, no push.

## 2. Conflict table

Exactly ONE conflict, in `CasGc.cpp`, and it was comment-only.

| File | Conflict | Resolution |
|---|---|---|
| `Gc/CasGc.cpp` | the hand-off gate's suppression paragraph. HEAD (T5) said `PHASE 14/18` and cited Task 7b plus a `BACKLOG.md` anchor and a pinning test name; T6's side said `PHASE 15/19` and dropped every citation | kept BOTH intents: T6's prose (no citations, states the bounded permanent cost as the reason) with T5's renumbered `PHASE 14/18` |
| `Gc/CasGc.h` | auto-merged | verified after: the flip (`kDefault = Authoritative`) present AND T5's probe-A removals stand |
| `src/Disks/tests/gtest_cas_gc_log.cpp` | auto-merged | verified after: T5's `ref_list_probe` removal from `FoldingRoundEmitsEveryPhaseInOrder` (zero `ref_list_probe` hits remain) AND T6's restored `EmitsStartFinishWithCounts` assertions both present; the two tests are disjoint, which is why the auto-merge was correct |
| `src/Disks/tests/cas_test_helpers.h`, `gtest_cas_gc_frontier_gate.cpp` | auto-merged | covered by the gate |

Phase-banner note, checked and NOT a defect: two of the 18 `GcPhaseTimer` sites carry no
`PHASE n/18` banner (`namespace_cleanup`, `pre_fold_ref_drain`). The same two-unbannered-sites shape
exists before T5's renumbering (17 banners over 19 timers there), so this is pre-existing and not a
renumbering regression.

## 3. Step 0 — T6a's verdict

Discharged before the flip, per the dispatch (`BENIGN-TRANSIENT`, prerequisite graded discharged).
The carry — post-flip healthy rounds must ALSO show zero `no usable checkpoint` anomalies — is
carried into §8's integration criteria.

## 4. The O-3 fix wave — the blast radius, measured

The draft's O-3 predicted an unbounded blast radius: 239 bare `gc.runRegularRound()` calls across 22
files, each changing meaning with the flip, concentrated in `gc_round` (62), `gc_fold` (30),
`ack_floor` (27), `hold_grammar` (27) and `rebuild` (21).

**The measurement contradicts the prediction.** The first full release gate at the integrated tip
(after one compile fix) returned **1986 passed / 3 failed of 1989** — and NONE of the three sat in
any of the five predicted files. The bare-call class turned out to be almost entirely inert, because
those fixtures publish live refs that are never dropped, so a reclaiming round has nothing condemned
to reclaim.

Blast-radius detector: `build/t6f_gate_release_wave0.log`.

| File | Test fixed | Subject rationale for the fix chosen |
|---|---|---|
| `gtest_cas_gc_frontier_gate.cpp` | `AnUndecodableCheckpointAnomalySuppressesEveryDeleteFamily` | Not a wave item — a **compile error**: the draft called a two-argument `putOverwrite` that does not exist. Subject is the anomaly arm, so the fix keeps the arm and repairs the fixture: resolve the life through `CasRefCatalog::lifeIfCataloged` (never a fixture mint) and overwrite the exact `_ckpt` with its own token. |
| `gtest_cas_gc_frontier_gate.cpp` | `HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` | Subject is the production default's treatment of a LIST-hidden catalog-named namespace. Its `deleteTotal() == 0` was a suppression-era premise, so the assertion was ADAPTED to a reclaiming round: the load-bearing property (the blob the hidden namespace owns survives) is kept, and the test now reads the gate's own verdict so it states WHICH of the two reasons produced the survival. NOT converted to `StageA_Suppressed` — converting it would have re-pinned the very premise the flip retires. |
| `gtest_cas_gc_frontier_gate.cpp` | `AnExhaustedProbeBudgetSuppressesEveryDeleteFamily` | Subject IS a suppressor (the mandatory 3c arm), so it must stay on the production path; the fixture was wrong, not the posture. Fixed the fixture and added `frontier_unprobed_budget > 0` so the arm pins its own suppressor. See §5. |
| `gtest_cas_gc_round_defer.cpp` | `FoldAndDeferEachBuildExactlyOneCompletePostListWalkPlan` | Subject is walk-plan construction, which was unaffected (`CasGcRefWalkPlansBuilt == 1` and `walk_plan_builds == 1` both passed). Only the incidental catalog-GET count moved, 2 → 3, so the assertion was ADAPTED and the third reader named. |

**No blanket conversions were made, and the wave added none.** The draft had already converted the
five suppression-subject tests; my wave neither added a `StageA_Suppressed` conversion nor removed
one. Of the four fixes above, one is a compile repair and three adapt assertions to a reclaiming
round — which is the correct side of the draft's own rule for every one of them, because none of
the three has suppression as its subject.

Gate after the wave: **1989/1989 across 278 suites** (`build/t6f_gate_release_final.log`).

### 4.1 The one finding that needed adjudication, not adaptation

`HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault` failed with two deletes on what
its own text called a suppressed round:

```
src/Disks/tests/gtest_cas_gc_frontier_gate.cpp:602: Failure
Expected equality of these values:
  backend->deleteTotal()
    Which is: 2
  0u
a catalog-named namespace that cannot prove its own frontier leaves the round-wide frontier incomplete, so the round destroys NOTHING. Deleted:
    p/cas/manifests/00/visible@cas@/0000000000000001-0000000000000002/000001.zst
    p/gc/gen/2/attempt/2/fold_seal
```

Two deletes on a round the test believed suppressed is either an ungated destructive site (a real
bug, and the dispatch says STOP) or a false premise. I did not adapt on a guess: I replaced the bare
loop with `runRoundCapturingGate` and asserted `frontier_complete == true` /
`suppress_destructive == false`, so a gate hole would have failed the test rather than being papered
over. The round reports a COMPLETE frontier and NO suppression, which settles it:

- `manifest_deletes` (`PHASE 15/18`) IS gated — `mf_cleanup_now` is an empty map under suppression.
- the orphan sweep's delete loop consumes `folded.orphan_sweep.nominations`, and
  `planManifestCursorPage` is only called under `!suppress_destructive`, so nominations are empty
  under suppression.
- the premise is what was false: `hidePrefix` hides LIST only, the arithmetic intake reads at
  `cursor + 1` by EXACT key, and the namespace is catalog-named — so it is provable, the frontier
  completes, and the dropped manifest plus a superseded fold seal are legitimately reclaimed.

The blob the hidden namespace owns still survives, and now on its own folded in-degree rather than
on a global off-switch — which is the same "survives on PROOF, not suppression" shape the plan
specifies for the kill-shot. **No reclaim of live data was found anywhere in this task.**

## 5. Step 1b — the five arms, and two that did not test what they named

| Arm | Test | Status |
|---|---|---|
| healthy | `AHealthyCatalogRoundOpensTheGateAndReclaims` | green; proven non-vacuous by M1 (§6) |
| (3a) negative policy — MANDATORY | `EveryInventoriedDestructiveSiteIsInertUnderSuppression` | green on an explicit `StageA_Suppressed`; proven non-vacuous by M1, which breaks its CONTROL |
| (1) one anomaly | `AnUndecodableCheckpointAnomalySuppressesEveryDeleteFamily` | did not COMPILE as drafted; fixed, green. Its `anomaly_counts.front() > 0` gate passes, so the arm really records an anomaly rather than taking the hold arm |
| (2) carried hold | `ACarriedHoldSuppressesEveryDeleteFamily` | green as drafted |
| (3b) empty universe | `AGenuinelyEmptyUniverseRefusesTheFrontierDespiteZeroEqualsZero` | green; proven non-vacuous by M2 (§6), which is the strongest single piece of evidence in this task |
| (3c) budget | `AnExhaustedProbeBudgetSuppressesEveryDeleteFamily` | **tested nothing as drafted**; fixed, green |

Arm (3c) as drafted asserted suppression on a round that was neither suppressed nor
budget-exhausted: the observed round reported `3 of 3 namespace(s) proven`, `frontier_complete` true
and `suppress_destructive` false. The reason is in `Gc::fold`'s intake loop: the budget is decremented
only for a catalog namespace with a parent coverage, no hold, no listing entry AND absent from
`checkpoints.recovery_checkpoints`. The draft published and hid the namespace but left its `_ckpt` in
place, so it was proven for free and no probe was ever needed. The fixture now also deletes the
`_ckpt` (by exact token) and hides the prefix at the CATALOG-resolved life, and a new
`frontier_unprobed_budget > 0` assertion — read off the `fold_ref_intake` row, not recomputed — makes
the arm pin ITS OWN suppressor instead of whichever one happens to fire. Without that assertion the
repaired test would still have passed had the namespace become unprovable for some other reason.

`GateVerdict` gained one field (`frontier_unprobed_budget`) to carry that metric.

## 6. Mutation demonstrations

Load-bearing mutation demonstration performed after implementation; mutation reverted; patch and
failing output preserved.

**M1 — `kDefault = StageA_Suppressed`** (the flip itself, one line in `Gc/CasGc.h`). Three tests go
red; 42 of 45 still pass:

```
[  FAILED  ] CasGcFrontierGate.AHealthyCatalogRoundOpensTheGateAndReclaims
  gtest_cas_gc_frontier_gate.cpp:704: Value of: verdict.frontier_complete  Actual: false  Expected: true
  gtest_cas_gc_frontier_gate.cpp:706: Value of: verdict.suppress_destructive  Actual: true  Expected: false
[  FAILED  ] CasGcFrontierGate.EveryInventoriedDestructiveSiteIsInertUnderSuppression
  gtest_cas_gc_frontier_gate.cpp:787: Expected: (backend->deleteTotal()) > (0u), actual: 0 vs 0
      the work queue was real -- a round with a universe drains it
[  FAILED  ] CasGcFrontierGate.HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault
```

The (3a) mandatory arm's failure is the interesting one: what breaks is its CONTROL, i.e. the half
that proves the production path drains. That is exactly what makes "suppressed on the policy term
alone" a measurement rather than an assumption — the test can tell `StageA_Suppressed` from
`kDefault`, which is the one thing the model cannot stand in for.
Logs: `build/t6f_mut1_build.log`, `build/t6f_mut1_run.log`.

**M2 — delete the empty-universe floor** (`&& result.frontier_namespaces > 0` → `&& true` in
`Gc::fold`). The 3b arm goes red, and it names real destruction:

```
[  FAILED  ] CasGcFrontierGate.AGenuinelyEmptyUniverseRefusesTheFrontierDespiteZeroEqualsZero
  gtest_cas_gc_frontier_gate.cpp:959: backend->deleteTotal()  Which is: 9   vs  0u
  an authoritative-but-EMPTY universe must refuse to be a complete frontier -- 0 == 0 is not a proof. Deleted:
      p/blobs/ch128/00/0000000000000000000000000000bead
      p/blobs/ch128/00/0000000000000000000000000000bead.meta
      p/gc/gen/1/attempt/1/blob_target/0/0
      ...
```

The mutated binary also ABORTS later in the run (`libc++ Hardening assertion this->has_value()
failed: optional operator->`, in `CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor`) —
a downstream consequence of the mutation, not a defect in the shipped tree, and additional evidence
that the floor is what keeps that path unreachable.
Logs: `build/t6f_mut2_build.log`, `build/t6f_mut2_run.log`.

Both mutations reverted; the tree was confirmed clean (`git status` shows no tracked modification),
rebuilt, and re-verified green: `CasGcFrontierGate*:CasGcBoundedWalk*:CasGcRoundDefer*` = 68/68
(`build/t6f_postmut_verify.log`).

## 7. O-4 — `gtest_cas_gc_bounded_walk.cpp`, checked FIRST

Run before the full gate, per the dispatch: **8/8 PASS** (`build/t6f_o4_bounded_walk.log`). No edit
was needed. The draft's reading was right for the reason it gave and for one it did not: its
destruction-expecting test, `ARawRecordBeyondTheCommittedFrontierCannotSuppressDestruction`, already
passed `UniversePolicy::Authoritative` explicitly, so the flip cannot move it; and the file's other
fixtures publish refs they never drop, so a reclaiming round finds nothing condemned. The file's
GET-count assertions count requests, not object existence, so an active `ref_object_cleanup` cannot
disturb them.

## 8. Gate numbers

| Gate | Before the wave | After the wave |
|---|---|---|
| Release CA gate (278 suites, generated filter) | 1986 passed / **3 failed** of 1989 | **1989 / 1989**, 0 failed |
| ASan CA gate (296 suites, its own generated list) | not run before the wave | **1994 / 1994**, 0 failed |
| `CasGcBoundedWalk` (O-4, run first) | — | 8 / 8 |
| soak scenario unit tests (`utils/ca-soak/scenarios/tests/`) | — | **46 / 46** incl. the renamed `test_leftovers.py` |

Suite-list regeneration reported `0 unclaimed` on both binaries (278 suites release / 21 excluded;
296 suites ASan / 3 excluded). The suite COUNT is unchanged from the draft's prediction: the wave
added no test name and removed none.

No `LOGICAL_ERROR` expectation was added or removed anywhere in the two verified commits
(`git diff 73755caa6e5..HEAD -- src/Disks/tests/ | grep '^[+-].*LOGICAL_ERROR'` is empty), so the
death-test split does not apply to this task and no `--gtest_list_tests` arm proof is owed.

## 9. Stateless proof (run UNCHANGED, yaml entries removed)

```
Connecting to ClickHouse server... OK
[1 / 4] 05010_content_addressed_mounts_gc_health:                               [ OK ] 0.47 sec.
[2 / 4] 05008_ca_gc_snap_prune:                                                 [ OK ] 1.07 sec.
[3 / 4] 04295_content_addressed_mutation_no_leftovers:                          [ OK ] 2.18 sec.
[4 / 4] 04290_content_addressed_no_leftovers:                                   [ OK ] 2.03 sec.
All tests have finished.
```

`build/t6f_stateless_direct2.log`. The four tests are byte-identical to their pre-T6 form (the draft
removed only banner comments); their `broken_tests.yaml` entries are gone.

These pass because the pool really fills and really drains — `04290`'s and `04295`'s references pin
`grew_above_baseline 1` followed by `fsck_unreachable 0` and `fsck_dangling 0`, so the
drain-to-`PENDING = 0` loop converged. That convergence is the end-to-end proof of the flip.

Deviation, disclosed: these were run with `tests/clickhouse-test` against a server I started from
`build/programs/clickhouse` (rebuilt at this tip — the pre-existing binary predated the flip), NOT
through a praktika stateless job. Reason: `praktika run "Stateless tests"` is ambiguous across 38
configured jobs, and no configured lane matches a plain non-sanitized amd binary running
`no-parallel` tests. A first attempt failed on an environment mismatch, not on the product — the
harness's default `CLICKHOUSE_USER_FILES` is `/var/lib/clickhouse/user_files`, which my server does
not use, so the test's pool directory was invisible to the server (`baseline=0 after_insert=0`,
then `Unknown disk`). Exporting `CLICKHOUSE_USER_FILES` at my server's path is the only difference
between that run and the passing one; no test file was touched. Log of the failed attempt:
`build/t6f_stateless_direct.log`.

## 10. Closeout inventories (reproducible-inventory rule)

All three re-derived at MY tip, in `/home/mfilimonov/workspace/ClickHouse/lane-g`:

| Command | Result |
|---|---|
| `git grep -in "task 7b" -- 'src/' 'tests/' 'utils/'` | **zero hits** (exit 1) |
| `git grep -n "STAGE-A RETURN ITEM"` | zero non-historical: only the plan, the design spec, the midpoint audit, the old Stage-B plan and this task's own reports |
| `git grep -n "STAGE-A CONTRACT"` | zero non-historical: only the plan, the design spec, the midpoint audit, the Stage-A RESULTS record and the sdd task reports |
| `grep` for the four `broken_tests.yaml` entries | **zero entries remain** |

The draft's finding that the greps are NOT a partition of the work stands and is worth carrying
forward: `utils/ca-soak/scenarios/tests/test_leftovers_stage_a.py` carried no `Task 7b` token, and
the plan's "15 non-doc files" figure was wrong when written (17 at both the audit baseline and the
draft's base).

## 10a. Integration lanes — destruction ACTIVE for the first time

`python3 -m ci.praktika run "integration" --test "<the plan's six selectors>"`
(`build/t6f_integration.log`): **18 passed, 1 failed of 19.**

The one failure is the dispatch's item-9 prediction landing in the integration suite rather than the
gtest layer — see §11. After the fix, that lane re-run is **2 passed**
(`build/t6f_integration_dropmember.log`).

**Zero anomalies, checked in the SERVER logs and not the harness log** (the harness log contains
neither, so its silence is not evidence). Across all 9 `clickhouse-server.log` files of the six
lanes, and again across the re-run lane's 2:

| Signal | Count |
|---|---|
| `destructive work SUPPRESSED` passes | 0 |
| `no usable checkpoint` anomalies (the T6a carry) | 0 |
| `unattributed` in any `unproven:` clause | 0 |

**Delete families nonzero, asserted by the tests themselves.** Six of the seven banner-rewritten
sites are green, and each one is an executing destruction assertion rather than a log grep:

| Test | The destruction-era assertion it now carries |
|---|---|
| `test_content_addressed_gc_s3::test_gc_reclaims_dropped_blobs` | renamed back; drains to `final <= baseline` AND `objects_deleted + manifests_deleted > 0` |
| `test_content_addressed_shared_pool::test_two_servers_share_one_pool` | drains to baseline with the pool-wide bookkeeping cross-check `deleted > 0` |
| `test_content_addressed_shared_pool::test_pool_survives_node_crash` | same, after a hard kill |
| `test_content_addressed_ref_snaplog::test_ref_snaplog_lifecycle_reclaims_and_fsck_clean` | drains to `content_baseline`; `ca-gc-dryrun` on the drained pool reports `preview_deletes=0` |
| `test_cas_replicated_relink::test_stalled_publish_protects_source_blobs_and_commits_nothing` | the RESTORED soundness guard: at least one abandoned blob must be reclaimed, which is what makes the earlier "they survived during the stall" assertion mean the relink pin rather than an inactive GC |
| `test_content_addressed_drop_pool_member::test_drop_dead_pool_member_heals_the_pool` | drains to `blobs_baseline` AND `deleted > 0` (both passed on the first run — the failure was at the later janitor assertion) |

`test_content_addressed_s3` carries no banner and passed unchanged, as the draft predicted.

## 11. The janitor_pending question (dispatch item 9) — found in the integration lane

**The gtest the dispatch named needed no change.** `EXPECT_GE(rep.namespace_janitor_pending, 1u)` in
`CasFsck.CanonicalDeadLifeResidueIsJanitorPendingNotHardFinding` constructs the dead-life residue and
runs `fsck` on it directly; it never drives a GC round, so there is no janitor page for the flip to
make destructive and nothing for the assertion to race. Green in the release gate.

**The same Stage-A assertion in the INTEGRATION suite did fail**, exactly as the dispatch predicted:

```
test_content_addressed_drop_pool_member/test.py:279: in test_drop_dead_pool_member_heals_the_pool
    assert int(janitor_pending_match.group(1)) >= 1, (
E   AssertionError: expected janitor-pending dead-life residue from the healed decommission and the t1 drop:
E     reachable=0 dangling=0 unreachable=0 pending_gc=0 awaiting_gc=0 unaccounted=0 stale_edge=0
E     corrupted_runs=0 chain_broken=0 lifeless_keys=0 janitor_pending=0 ... ref_records_walked=8
E   assert 0 >= 1
```

Every class reads zero: the pool is fully healed AND fully drained, which is a STRONGER outcome than
the assertion demanded, not a weaker one. The assertion pinned Stage-A timing — its own comment said
the janitor's "deletes are suppressed for the whole of Stage A", so "residue still exists" was the
only thing observable then.

Fixed posture-honestly: the residue must now DRAIN to `janitor_pending == 0`, polled with the file's
own `RECLAIM_RETRIES`/`RECLAIM_SLEEP` idiom because the janitor deletes one bounded page per round,
and `lifeless_keys=0` is now checked on EVERY poll rather than once — the residue must never be hard
corruption on the way to zero, which is the part of the original claim that was never about timing.
The Stage-A sentence about suppressed janitor deletes is gone from the comment.

Disclosure: this trades one property away. The old assertion proved the residue was CREATED; the new
one cannot, because observing it before the janitor reaches it is racy by construction. The
creation is still established by the steps above it in the same test (the decommission heals and the
slot retires, both asserted), and the new assertion would fail if the janitor's deletes regressed to
inert — which is the property the flip is on the hook for.

## 12. O-2 for the reviewer — `planManifestCursorPage`'s newly-reachable argument

The draft's O-2 asked for a reviewer's eye on the one place the flip enables a behaviour the plan's
"only `frontier_complete` changes" framing does not mention: `planManifestCursorPage`'s
`catalog_recovery_authoritative` argument, which the draft replaced with the already-computed
`universe_authoritative`. **The gate formula itself is untouched** — I read it against the plan's
normative text term by term and it is character-for-character the specified three-term form, with
the flip changing only what `universe_authoritative` may become.

What I can add that the draft could not is that the newly-reachable call is now OBSERVED, not
merely argued: `CasGcRoundDefer.FoldAndDeferEachBuildExactlyOneCompletePostListWalkPlan`'s fold half
went from two catalog GETs to three, and the third is `planManifestCursorPage`'s own
`CasRefCatalog::read`. That test is the standing witness that this call executes in production
rounds; if a future change makes the sweep unreachable again, that count is what will notice.

## 13. Other deviations and disclosures

- **One comment-policy fix beyond the brief.** `assertions.py`'s rewritten leak message still ended
  `"(see BACKLOG)"`. The rewritten line is mine now and the policy forbids internal-document
  citations, so the citation was deleted and the reason kept. Three OTHER `BACKLOG` citations in the
  same file are pre-existing and untouched — they belong in the batched prose register, not in a
  code commit.
- **PROSE items observed and NOT fixed** (for `docs/superpowers/cas/deferred-docs-fixes.md`, per the
  batching directive): `assertions.py` lines 103, 133 and 298 cite `BACKLOG.md` anchors;
  `gtest_cas_gc_frontier_gate.cpp`'s empty-universe test comment and `CasGc.cpp`'s
  `frontier_complete` comment both cite `2026-07-28-ref-rework-adjacent-findings.md {#r11-...}`.
  All five are pre-existing and none is a code or test defect.
- **`StageA_Suppressed` keeps its name** (the draft's O-1). The dispatch names the identifier
  explicitly as the negative-policy seam, so renaming it was out of scope for this task.
- **T5 reconciliation is not done here.** My branch point predates T5's closure commit; per the
  dispatch, integration is by ordinary merge at MAIN, never rebase.

## 14. State at the final tip

- `git diff --name-only a1686eb699a..HEAD` contains no `.cpp`/`.h`: every commit after the gated one
  touches only two Python files and this report, so the release and ASan gate numbers in §8 are
  about HEAD's C++ and not about an earlier tree.
- Tracked tree CLEAN; both mutations reverted and confirmed reverted before either gate number was
  taken.
- Re-verified at the final tip:
  `CasGcFrontierGate*:CasGcBoundedWalk*:CasGcRoundDefer*:CasGcLog*:CaWiring*` = **74/74**
  (`build/t6f_final_verify.log`) — this covers both restored gtest assertions,
  `CasGcLog.EmitsStartFinishWithCounts` and `CaWiring`'s `unreachable == 0`.
- Repo-root debris `k/`, `p/`, `tmp-lock-sites.txt` is dated 2026-07-31/08-01 and predates this
  session; nothing was staged with `git add -A` at any point.
