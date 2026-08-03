# T7 lane A — adversarial review

Reviewed read-only from `/home/mfilimonov/workspace/ClickHouse/master` via git objects:
`a19066a7893` (the `listedTok` verdict + model edits) and `3d5bc5b35fb` (the ninth-family battery
runner + RESULTS + report) on `laneg/t7`, base `f8df7d9a5e8`. No edits, no commits, no checkouts,
no TLC runs — recorded results were verified for internal consistency against the lane-g logs.

## Verdict: APPROVE-WITH-NONBLOCKING

The load-bearing A1 retirement argument holds and is stronger than the RESULTS text claims. No
live coverage was dropped by retiring the four configs. Every code/test-class check came back
clean; all six findings are PROSE, so by the standing directive they batch into
`docs/superpowers/cas/deferred-docs-fixes.md` and open no fix round. Integration into
`cas-gc-rebuild` is not gated on any of them.

---

## Item 1 — the retirement argument (A1): SOUND, and better-grounded than stated

The claim under test: "no production seam still consumes a LIST-derived token as skip authority
for a manifest/owner-transition fold read." I verified the enumeration rather than the prose.

**The decisive evidence lane A did not cite.** `docs/superpowers/cas/06-tla-models.md`
§`{#skipparksdeadprecommit}` names a *landed* production seam for exactly this shape:
`Gc::computeDiscoverDecisions` overriding a would-be `Skip` to `Read` against a sealed
`ShardCoverage`, with `ProfileEvents::CasGcPrecommitRevisitForced`. If that seam existed, the
verdict would be unsound. It does not: the only surviving mention anywhere in `src/` is a comment
in `gtest_cas_fold_seal_codec.cpp` recording its removal —

> the token-diff Skip machinery (`computeDiscoverDecisions`/`discoverDecisionsForTest`) no longer
> exists -- the "did it change" signal is simply logs above the durable cursor.

and it further records that the live-precommit watermark fields (`has_live_precommit`,
`min_live_precommit_*`) that fed the reclaim were deleted from `RefCoverage` with it. So the
premise is not merely unfound, it is provably deleted, with an in-tree witness.

**The intake-fold analogue checks out verbatim.** In `Gc::fold`, the
`intake_tails_advanced`/`intake_tails_unchanged`/`intake_tails_below_cursor` classification is a
counter plus a *fold* bound; the exact-key `GET` at `cursor + 1` is unconditional inside the walk,
and the frozen-tail comparison sits strictly after the read. `tail == cursor` skips nothing. The
comment lane A quotes ("not a cheaper version of this design; it is a GC that permanently
reclaims nothing") is real and in that function's header.

**Corroborating negatives, from a full sweep of the GC tree.** `CasOrphanManifestSweep`'s
`planManifestCursorPage` uses the listing only to nominate candidates and deliberately refuses to
advance `decided_through` past an unexamined key; `manifestDeletionPremise` uses
`tail_removal_targets` only in the retain direction and disclaims the negative direction in-code;
`activeManifestKeys` walks by exact `GET` and states "LIST cannot decide whether a removal
exists"; `Gc::readCheckpointWitnesses` explicitly refuses `RefTableListing::has_ckpt` and always
reads the exact key; `graduationDue` takes no LIST at all; the destructive gate has no
listing-derived term. `pruneSupersededGenerations` derives prune targets arithmetically.
`CasBlobInDegree`, `CasGcScheduler`, `CatalogLifecycleReconciler`, `CasGcMaintenanceState`,
`CasGcShardPlan` contain no listing-derived value.

### Finding 1 (PROSE, IMPRECISE) — the verdict's generalization overstates; two live seams go unnamed

RESULTS asserts that "a listing may only ever offer a newer candidate, a diagnostic, or a garbage
nomination, never a correctness decision." Two live counterexamples exist:

- `RefPlan::changedRows` + `shouldDeferRound` (`CasGc.cpp`) skip an entire round's fold, its
  pre-CAS deletes and its `gc/state` CAS, on `row.tail_observation` (pure LIST output, the
  round's greatest listed log id) versus the durable `last_folded_ref_id` — *literally* this
  model's `listedTok[n] = foldedTok[n]` comparison. It is safe for reasons the model's
  `GDiscoverSkip` does not share: DEFER performs no destructive work and advances no coverage
  cursor, `graduationDue` (seal-only, zero LIST) forces a fold before any destructive decision,
  and `gc_fold_max_defer_rounds` bounds the delay. A lying listing costs reclamation latency, not
  safety.
- `Gc::newestFoldSealRef`'s virgin-by-enumeration verdict on the hand-run rebuild path treats
  listing absence as authority; it is already booked in-code as a NAMED RESIDUAL and surfaced via
  `RebuildReport::virgin_by_enumeration`.

Neither breaks the narrow verdict, which is about skipping a *fold read* on a token match. But
`shouldDeferRound` is the first thing a skeptical reader will find, and the verdict is the
document a future reader will lean on. It should name the DEFER seam and its fence explicitly and
narrow the absolute sentence. **Not a reversal of the retirement.**

## Item 2 — what the retirement removed: NO live coverage lost

The four removed configs are exactly the four that ever set `EnableTokenDiff = TRUE`; at
`3d5bc5b35fb` no config sets it, and 44 configs remain, so the "complete retirement" claim is
exact. The state-space claim ("every remaining config unaffected") is sound — the retired arm's
actions are all gated on `EnableTokenDiff`.

The real question was `sab_/fix_skipparksdeadprecommit`'s `LiveDeadPrecommitReclaimed`, which is
now referenced by **zero** configs (verified: the four surviving `PROPERTY`-bearing cfgs are
`live` ×2, `sab_deletebodybeforedecrements`, `sab_noorphansweep`, `stage3`, `stage4`). Neither
survivor covers it: `OrphanManifestDebrisDrains` requires `owner[m] = None`, and `FairSpec`'s
abandon-fairness conjunct requires `~mBody[m]`, while the property quantifies over
`mBody[m] /\ owner[m] \in Builds /\ BuildDead(...)`.

**It is nonetheless not a live-coverage loss, because the modeled behavior is itself retired in
production.** The same gtest comment records that GC-side reclaim is gone: "Per spec
§Responsibility Boundary, reclaiming an abandoned precommit is now the WRITER's job (it appends
the exact `owner_transition` removal)." `GReclaimDeadPrecommit` models a GC fold-visit reclaim
with no production counterpart. So the test passes — but for a second, independent reason RESULTS
never states.

### Finding 2 (PROSE, IMPRECISE) — the retirement marking is not a partition

The `.tla` comment enumerates what the retirement makes inert as
`EnableTokenDiff`/`TokenObservable`/`GDiscoverSkip`/`GDiscoverRead` "and their two Sabotage*
controls". That list does not sum to the whole. Also now permanently inert and unmentioned:

- `GReclaimDeadPrecommit` (gated on `EnableTokenDiff`, so never enabled by any config);
- `LiveDeadPrecommitReclaimed`, now an orphan property no config checks;
- the `WF_vars(\E m \in ManifestIds : GReclaimDeadPrecommit(m))` conjunct **in `Spec` itself** —
  every config uses `SPECIFICATION Spec`, so all 44 now carry a fairness conjunct on a
  permanently-disabled action. Vacuous and harmless, but undocumented.

Relatedly, that conjunct's existing comment ("this WF conjunct is vacuous ... in every
pre-token-diff cfg and in every safety-only run") is now stale: it is vacuous in *every* config.
The fix is one more clause in the same comment plus one word in the other, and it should say the
second reason for retirement (GC-side reclaim moved to the writer), which is what actually
protects the coverage claim.

## Item 3 — battery honesty: substantially honest, one false count

Verified against the runner logs, not the prose:

- The RESULTS "exact runner tail" is **byte-faithful** to
  `lane-g/build/t7_gc_partmanifest.log`, including the two `FAIL` rows and the trailing
  `SOME EXPECTATIONS UNMET`. No green-washing: the failed first pass is reproduced with its
  failure verdict intact.
- Spot-checked state counts against the per-config TLC logs, all exact:
  `sab_deletebodybeforedecrements` 33,805,258 / 9,295,903 depth 23 (temporal row);
  `sab_crosssharddisplacement` 11/10 depth 2, `sab_reducerownsfence` 12/11 depth 2,
  `stage5_sharding` 12/11 depth 2 (the three known-model-error rows, each confirmed as
  "TLC threw an unexpected exception" at depth 2 — the arm is genuinely unexplored);
  `stage2` 377,632,669 / 68,550,326 depth 50 with 0 left on queue.
- Both rerun logs match: `live` green at 7536s, 74,147,107 / 17,845,340, depth 38, 0 queued;
  `stage5_lazytrim` timeout at 14401s, 1,333,723,653 / 233,198,128, depth 31, 24,395,446 queued.
  The "closing one-minute samples each removed on the order of tens of thousands off the queue"
  claim is accurate (the last interval drains 15,258; earlier ones 30–45k) and the "not close to
  draining" conclusion is supported.
- The claim that `live` "reproduces the exact state counts on record from before the
  `docs/superpowers` consolidation" is **TRUE** — I recovered the deleted file at
  `3a054b9ffe6^:docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md`, whose `live`
  row reads `74,147,107 | 17,845,340`. Verified, not relayed.
- Checker identity is real: the battery logs open `TLC2 Version 2026.07.18.145032 (rev: 30cc360)`
  with `1 worker`, and `lane-g/tmp/tla2tools.jar` hashes to the recorded
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Both debts are stated as debts, in their own `{#standing-model-debt}` section, with the sharding
  arm explicitly called "not exercised by TLC" and `stage5_lazytrim` explicitly "not a fixed row
  and not a downgraded expectation". No softening.
- Row/config bijection: the runner carries 39 + 5 = 44 rows, exactly 44
  `CaGcRootLocalPartManifestCore_*.cfg` exist at `3d5bc5b35fb`, and every row's cfg resolves — so
  no config is silently uncovered and no row is dead.

### Finding 3 (PROSE, FALSE) — "resolved 46 of 48 rows"

`{#first-pass-two-timeouts}` says the first whole-suite run "resolved 46 of 48 rows". The battery
has 44 rows and two timeouts: it is **42 of 44**. 48 is the pre-retirement total (43 fast + 5),
which the same section's preceding paragraph correctly identifies as superseded — so RESULTS
contradicts itself two paragraphs apart, and contradicts both the log and the lane report (which
says "42 of 44" correctly).

### Finding 4 (PROSE, IMPRECISE) — `stage3`'s timeout is excused circularly

RESULTS distinguishes `stage3` ("`incomplete` by design — its `TLC_SLOW_TIMEOUT=60` timeout IS
the expected outcome") from `stage5_lazytrim` (a named debt). As written the distinction is
provenance-of-expectation, not substance: `stage3` is a 15-invariant + `MonotoneGC` **positive**
safety row that is never checked to completion, i.e. materially the same class of unproven claim
as `lazytrim`. Asking what covers it is the test, and the honest answer exists but is unwritten:
`stage4` is GREEN at the same `MaxRound = 2`/`MaxLog = 3`/`MaxToken = 2` with a strict superset
of `stage3`'s checks (plus `ManifestActivationMatchesEdges`) and more features on, differing only
in having one blob; `stage2` is GREEN at `stage3`'s two blobs with `MaxRound = 1`. So `stage3`'s
uniquely-unproven residue is two-blob interleavings at round depth 2 — small, and worth one
sentence. RESULTS should either make that union argument or book `stage3` as a third named debt.
Either is principled; "by design" alone is not.

## Item 4 — Finding 5 (PROSE) — disclosed placeholder

`t7-laneA-report.md` closes Step A2 with "Committed as `<see final message>`". The actual SHA is
`3d5bc5b35fb`. Self-disclosed and harmless, but it makes the committed report non-self-contained.

## Item 5 — the reconcile: CLEAN

`git diff 3d5bc5b35fb:docs/superpowers/models/run_gc_partmanifest.sh` against MAIN's uncommitted
copy yields exactly four added lines in MAIN and nothing else:

```
+    "sab_skipchangedshard            violation               INV_NO_DANGLE"
+    "sab_skipparksdeadprecommit      temporal                LiveDeadPrecommitReclaimed"
+    "fix_skipparksdeadprecommit      green                   -"
+    "stage5_tokendiff                green                   -"
```

MAIN's copy is the pre-retirement rewrite; lane A's committed file is that same rewrite minus
exactly the four retired rows. No divergence in logic, ordering, classifier, or gate wiring.
**The integration reconcile is a clean `git checkout -- docs/superpowers/models/run_gc_partmanifest.sh`
in the master worktree once `3d5bc5b35fb` is in.** Dropping MAIN's copy loses nothing except rows
whose `.cfg` files no longer exist (which would fail the battery outright, since a missing cfg
makes TLC error and the row is not `incomplete`-expected).

## Runner quality: no findings

Checked the fences against what they claim rather than what the comment says:

- `check_tlc_pin` runs before any TLC invocation and `exit 3`s; `check_tlc_temporal_expectations`
  is called *after* the `SLOW` append, so it covers the slow rows too.
- The temporal arm requires both `Error: Temporal property ${want} was violated.` in the log
  **and** `^PROPERTY[[:space:]]+${want}$` in the cfg — so a cfg that regressed to `PROPERTIES`
  falls through to `error` and FAILs rather than passing. This fence checks what it claims. All
  `PROPERTY` lines in all surviving cfgs are singular (verified directly).
- A temporal violation of a *different* property than `want` also falls through to `error` → FAIL.
  Fail-closed.
- The `known-model-error` classifier requires the undefined-identifier message **and** both
  provenance line numbers, each derived at runtime by grepping `origVars` and `GReduceShard`'s
  `UNCHANGED vars` out of the live `.tla` — so it cannot launder an unrelated TLC crash, and it
  breaks loudly (`exit 3`) if either site moves out of existence.
- Fail-closed selection: unknown selector and SLOW-only-without-`SLOW=1` both set `overall=1`.
- Per-config metadir and log keyed on `$$` + nanoseconds, so concurrent runs cannot collide.
- One deliberate soft spot, disclosed in the output itself: an `incomplete` row that comes back
  green reports `green (tighten expectation)` with verdict `KNOWN`, never `FAIL`. That is the
  right call for an improvement, but it means `stage3` could quietly become provable without
  anything failing. Interacts with Finding 4; not a defect on its own.

## Finding 6 (PROSE, FALSE — highest-priority batch item) — the numbered doc set now asserts deleted code is landed

Outside lane A's diff, but the retirement is what makes it wrong, and
`docs/superpowers/cas/06-tla-models.md` is a durable numbered doc:

- §`{#skipparksdeadprecommit}` states "**C++ fix (LANDED, branch `cas-gc-rebuild`).**
  `Gc::computeDiscoverDecisions` overrides a would-be `Skip` to Read when the sealed
  `ShardCoverage`'s minimal live precommit `isPrecommitDead` ..." — none of
  `computeDiscoverDecisions`, `ShardCoverage`, `CasGcPrecommitRevisitForced` or the
  `min_live_precommit_*` fields exists in `src/` any longer. A reader who trusts this doc reaches
  the opposite conclusion from lane A's verdict, on the exact question the verdict answers.
- The same section's **Files:** line names `_sab_skipparksdeadprecommit.cfg` and
  `_fix_skipparksdeadprecommit.cfg`, both deleted by `a19066a7893`.
- The stage table lists `| 5 token-diff discovery | stage5_tokendiff | 8.3M | 28s |` as a live
  row, and the sharding-debt paragraph lists `stage5_tokendiff` among "the non-sharding stages
  ... unaffected".
- `docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` describes the runner's three
  temporal rows as including `sab_skipparksdeadprecommit`/`LiveDeadPrecommitReclaimed`, now two
  rows and no such row.

Per the standing directive this is batch material for
`docs/superpowers/cas/deferred-docs-fixes.md`, not a fix round — but it should go in first,
because it is a "landed" claim about deleted code in the doc set future readers are pointed at.

## Summary

| # | Class | Grade | Substance |
|---|---|---|---|
| 1 | PROSE | IMPRECISE | Verdict's "never a correctness decision" absolute; `shouldDeferRound`/`changedRows` and `newestFoldSealRef` unnamed |
| 2 | PROSE | IMPRECISE | Retirement marking not a partition: `GReclaimDeadPrecommit`, `LiveDeadPrecommitReclaimed`, the `Spec` `WF` conjunct; stale "pre-token-diff cfg" wording |
| 3 | PROSE | FALSE | "resolved 46 of 48 rows" — it is 42 of 44 |
| 4 | PROSE | IMPRECISE | `stage3`'s timeout excused as "by design" without the coverage argument that would justify it |
| 5 | PROSE | — | Report's `<see final message>` placeholder; SHA is `3d5bc5b35fb` |
| 6 | PROSE | FALSE | `06-tla-models.md` §skipparksdeadprecommit asserts a LANDED C++ fix in a deleted seam; stale cfg/stage references |

No CODE findings. No TEST findings. No live model coverage dropped. Battery honest and
reproducible. Reconcile clean.
