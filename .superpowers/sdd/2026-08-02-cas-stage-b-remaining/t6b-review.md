# T6b review — per-round work-envelope budgets (codex T6-1/T6-2/T6-3)

**Reviewer:** independent review agent (Opus 5), read-only on source.
**Reviewed tree:** worktree `/home/mfilimonov/workspace/ClickHouse/lane-g`, branch `laneg/t6b`.
Tip at review time: `2e3eaac4973` (comment-only cleanup landed mid-review, as the dispatch anticipated).
Code commits under review: `a218af234d0` (Slice 1), `5295d6c54ae` (Slices 2+3), plus `84c24d3abbf`
(comment-only, provenance citations stripped — re-verified that both budget fences survive it, see
"Shared-worktree note" below).

**VERDICT: APPROVE-WITH-NONBLOCKING.**

The three caps are real, threaded into the production round path, checked before the expensive work,
and fail-closed on exhaustion. Both mutation demonstrations I re-derived myself went red for the
stated reason. Both full CA gates are green in my own runs. What keeps this from a plain APPROVE is
not a defect in what landed but the **closure claim**: T6-2 and T6-1 are answered *partially*, and
two named residuals (`spared` outcome entries, `mf_cleanup`) are neither capped nor disclosed. Those
are follow-up items, not blockers — nothing that landed is unsafe, and no existing invariant is
broken.

---

## 1. Does each codex finding actually get answered?

### T6-1 (orphan planner) — ANSWERED on the count axes, PARTIAL on bytes

Codex's specific complaint was that `planManifestCursorPage` exact-GET and **retained** every
parseable key of a `list_budget`-sized page *before* eligibility, ownership, or the smaller nomination
budget was consulted.

Verified in `planManifestCursorPage` (`Gc/CasOrphanManifestSweep.cpp`): the freeze loop is now

```
if (frozen >= nomination_budget)
    break;
if (parseListedManifestObject(layout, listed.key))
{
    observed_candidates.emplace(listed.key, backend.get(listed.key));
```

— **the budget check precedes the GET**, which is the plan's explicit requirement. I also checked the
identity the loop depends on: the freeze loop keys on `listed.key` and the decision loop looks up
`parsed->key`; `parseListedManifestObject` sets `.key = key` verbatim, so the lookup cannot
systematically miss.

The namespace fan-out and the committed-tail recovery walk are capped
(`sweepNamespaceAvailable` before any view is built; `sweepRecoveryOpAvailable` checked *before* each
ref-log GET inside `activeManifestKeys`' `while (id <= *ckpt.committed_through)` loop), and exhaustion
sets `NamespaceProtection::recovery_incomplete`, which the caller turns into a whole-namespace retain
— it never decides from the partial `active`/`tail_removal_targets` sets. Fail-closed, correct.

**Residual (finding C3, CODE, non-blocking):** the *byte* axis codex named is still unbounded. Retained
bodies fell from ≤ `list_budget` (1000) to ≤ `nomination_budget` (100) objects, but a valid manifest may
be up to 256 MiB, so worst-case retention is ~25 GiB rather than ~250 GiB. A 10× reduction is not a
bound. This is consistent with the plan (the minimal fail-close version asked only for the pre-GET
check, the namespace cap and the recovery budget — no byte budget), so it is not a scope violation; but
the ledger row that closes T6-1 must say "count axes bounded, retained-byte axis reduced not bounded",
not "closed".

### T6-2 (blob deletion pipeline) — ANSWERED for the cohort, NOT for two of codex's four bullets

Verified in `settleEntry` (`Gc/CasBlobInDegree.cpp`): both the `delete_pending`→`redelete` arm and the
graduation arm consult the budget *before* moving the entry, and on exhaustion push the entry to
`still_retired` **unchanged** — `delete_pending` stays `delete_pending`, a floor-passed condemned entry
stays condemned. Nothing is dropped, nothing is skipped ahead into `scattered`. The `redelete` drain in
`runRegularRound` needs no cap of its own because `merge.redelete` is capped upstream. Correct.

**Finding C1 (CODE, non-blocking, but it falsifies a stated claim):** the plan and the report both say
the per-shard `GcOutcomes` body "becomes bounded as a consequence" of the redelete cap. It does not.
There are exactly two `outcomes[shard].entries.push_back` sites in `runRegularRound`: the redelete loop
(now capped) and the **`merge.spared` loop, which is uncapped**. `spared` is filled by `settleEntry`'s
`indeg > 0` branch from the durable retired set, whose size has no round budget — so a round in which a
large condemned cohort regains in-degree (the mass dedup-re-adopt shape) still builds one unbounded
outcome body per shard and PUTs it. The report's inference — "each `redelete`/`spared`/`graduated` entry
maps to exactly one `OutcomeEntry` … so a bounded redelete count is a bounded outcome-entry count by
construction" — is a non-sequitur twice over: `graduated` contributes no outcome entry at all, and
`spared` is precisely the unbounded term. See judgement call (c).

**Finding C2 (CODE, non-blocking):** codex's fourth required-fix bullet for T6-2 — "Bound owner-manifest
cleanup under the same round budget or a separately explicit one" — is not implemented.
`runRegularRound`'s `manifest_deletes` phase still iterates all of `folded.mf_cleanup` with one exact
`deleteExact` per entry and no budget. A mass namespace removal produces one entry per removed manifest.
This family is not in the plan's file list, so the implementor did not skip assigned work; but T6-2
cannot be recorded as fully closed while it stands.

### T6-3 (metadata cleanup) — ANSWERED

- `deletePrefixWholesale` no longer takes `UINT64_MAX` from either caller; both pass
  `GcRoundWorkBudget::prefixWholesaleRemaining()`.
- `cleanupRefObjects` caps the cohort *cumulatively over the round*, checking `refCleanupAvailable()`
  immediately before each `deleteRefObject`, and `return`s on exhaustion (nothing marked done, nothing
  deleted anyway).
- `snap_pruned_through` advances only past a fully drained prefix — see §3.

---

## 2. Fail-close, not fail-open

I walked every exhaustion path looking for lost work. All six are retain-shaped:

| family | on exhaustion | work retained where |
|---|---|---|
| graduation | entry carried unchanged (still condemned) | `still_retired`, durable run |
| redelete | entry carried unchanged (still `delete_pending`) | `still_retired`, durable run |
| sweep namespace cap | namespace marked errored → every candidate retained, cursor still advances past them | `cas/manifests/` objects untouched |
| sweep recovery-op cap | `recovery_incomplete` → partial view **discarded**, whole namespace retained | ditto |
| `cleanupRefObjects` | `return` mid-plan; `planRefCleanup` recomputes the same candidates next round | durable ref objects |
| `deletePrefixWholesale` callers | prune loop `break`s, cursor left before the undrained generation | `gc/gen/<g>/` objects |

`deletePrefixWholesale`'s `bounded_remaining == 0` conservatively reports `out_fully_drained = false`
(the loop body never runs), and the early-return-on-budget path inside a page also reports `false` even
if the prefix happened to be empty afterwards — conservative in the safe direction. No exhaustion path
deletes anyway, drops work, or marks something done.

**Finding C4 (CODE, non-blocking, design residual):** `pruneSupersededGenerations` (pre-CAS) and the
post-CAS hand-off reclaim draw from the *same* `max_prefix_wholesale_objects`, and the prune runs first.
A prune-heavy round can leave the hand-off a zero remainder — and the hand-off is a documented **one-shot**
event with no reclaimer behind it, so its remainder is leaked until `fsck`. Before T6b the hand-off passed
`UINT64_MAX` and always finished. This is disclosed at the call site ("a round that already spent the
budget pruning superseded generations reclaims correspondingly less here"), and it is a space leak in GC
metadata, not a correctness fault. It is nonetheless a *new* leak path created by this task, and codex
explicitly warned against making the hand-off partial without durable pending work. Cheap mitigation if
the lead wants it: give the hand-off its own small budget, or reserve a fixed remainder for it. Recording
durable pending work is out of scope by the plan's OUT list.

---

## 3. Liveness — the wedge risk

The invariant ("every round either decides ≥1 candidate or advances the cursor") **holds in code**, and
it holds structurally rather than by assertion:

- Every retain branch in `planManifestCursorPage` — the two new work-budget causes included — sets
  `decided_through = listed.key` before `continue`. A retained candidate is a decided candidate.
- The only branches that leave a key undecided are (a) nomination-count exhaustion and (b) "well-formed
  key was never frozen". Neither can fire on the first parseable key of a page: `frozen` starts at 0 and
  `nomination_budget > 0` is a precondition of the freeze loop, and `result.nominations.size()` starts at
  0. So `decided_through` is always non-empty by the time `budget_exhausted` can be set, and
  `next_cursor = decided_through` strictly advances.
- `pruneSupersededGenerations` never regresses `snap_pruned_through`: `g` starts at
  `snap_pruned_through + 1`, and a `break` leaves `g - 1 == snap_pruned_through`.
- `cleanupRefObjects` and the prune both make destructive progress each round (deletes are permanent), so
  repeated rounds converge; the tests pin the convergence.

The mandatory liveness test does more than exist: `RecoveryWorkBudgetRetainsAndConvergesWithoutWedging
TheCursor` carries `ASSERT_NE(cursor, result.next_cursor) << "page N made no cursor progress"` plus
`EXPECT_TRUE(wrapped)` — I proved it detects a wedge by mutation (§4, mutation B).

---

## 4. Mutation demonstrations I re-derived myself

I did **not** rely on the report's mutation output. I chose two mutations that attack the *fences*, not
the budgets (the implementor's mutations already covered the budgets). Files were backed up with `cp`
and restored with `cp` — deliberately **not** `git checkout --`, because another agent had uncommitted
comment edits in the same two files and a checkout would have destroyed them.

**Mutation A — the fully-drained fence.** In `pruneSupersededGenerations`, deleted
`if (!fully_drained) break;` (budget itself untouched). Result:

```
CasGcSnapRetention.PruneRespectsPrefixWholesaleBudgetAndNeverStrandsAPartialGeneration
gtest_cas_gc_round.cpp:1251: Failure
round 2: snap_pruned_through (1) claims generation 1 is behind it, but 10 object(s) remain
         -- the cursor advanced past a partially-drained prefix
```

The test fails on the *correctness* invariant, not merely on a timing proxy. The fence is load-bearing.

**Mutation B — the sweep liveness fence.** In `planManifestCursorPage`'s errored-namespace retain branch,
replaced `decided_through = listed.key;` with `budget_exhausted = true;` (i.e. the retained candidate is
no longer a decision). Result:

```
CasSweepDeletionPremise.RecoveryWorkBudgetRetainsAndConvergesWithoutWedgingTheCursor
gtest_cas_sweep_deletion_premise.cpp:380: Failure
Expected: (cursor) != (result.next_cursor), actual: "" vs ""
page 0 made no cursor progress
```

The mandatory liveness test genuinely detects a wedge. 10 other tests in the same run stayed green, so
the two failures are attributable.

**Restoration verified:** `md5sum` of both files equals the pre-mutation backups; no `MUTATION` marker
survives anywhere under the CAS sources; `git status --short src/` is empty; `git show HEAD:` confirms
both fences are present in the committed tree. Rebuilt and re-ran the affected suites clean afterwards:
49/49 across `CasGcSnapRetention` / `CasSweepDeletionPremise` / `CasThreeCursorMerge` / `CasGcAckFloor` /
`CasRefGc` (`build/t6b_review_restore_test.log`, build `NINJA_EXIT=0`).

---

## 5. Scope discipline (the plan's OUT list)

- No new persisted object kinds — the budget is a stack-local struct, nothing serialized. ✔
- No protocol-step changes — no new HEAD/GET/PUT ordering anywhere; the diff only *stops earlier*. ✔
- No chunked/resumable outcome logs. ✔
- **Per-key fail-close validation not amortized:** verified specifically. `deleteRefObject` is byte-for-byte
  unchanged in the diff (`git diff e497c4a0e6e..5295d6c54ae`); the cap is a `refCleanupAvailable()` check
  *outside* it, immediately before each call. Every deleted key still pays its own HEAD + catalog re-read +
  `gc/state` re-read. No batching, no token shortcut, no cached catalog. ✔

## 6. Existing invariants

- **One-pass round:** the diff adds no CAS and no state write; `gc/state` is still committed exactly once. ✔
- **`snap_pruned_through` semantics:** strengthened, not weakened — previously it advanced over every
  *visited* generation, now only over fully processed ones (ref-retained generations still count as
  processed, which is correct: this loop has nothing left to do to them). Monotonicity preserved. ✔
- **Suppression semantics (T6):** untouched. `suppress_destructive` still short-circuits before the budget
  in `pruneSupersededGenerations`, still produces `kNothingToDelete`/`kNoRuns`/`kNoManifestCleanup`, and in
  `settleEntry` the budget arm is OR'd *after* the suppression arm, with the identical carry shape. ✔

## 7. Test quality

- The four new/extended tests are non-vacuous: each was shown red under a mutation (two by the implementor,
  two by me), and none has a comparison that holds vacuously at zero — the prune test's key assertion is
  guarded by `snap_pruned_through >= old_gen` *and* separately requires `drain_done_round > drain_start_round`,
  the ref-cleanup test asserts `countSurviving() == 5` before the first round so the later `4` cannot be
  vacuous, and the liveness test asserts a positive `retained_work_budget` per page.
- The implementor's disclosure that his *first* version of the prune test passed under mutation, and that he
  rewrote it, is exactly the right discipline and is corroborated by the assertion that is now in the tree.
- **LOGICAL_ERROR sweep** on every touched test file (`grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"`):
  every hit is pre-existing and outside the T6b hunks. The new tests contain no throw expectations at all.
  `gtest_cas_ref_catalog.cpp`'s `expectThrowsCode(LOGICAL_ERROR, …)` block already carries the death-test
  split comment and was touched only for the `Gc::fold` friend-signature pin. No new violation. ✔
- **Gate reachability:** `utils/cas-gate/generate_cas_suites.sh` passes on both build dirs; no new suite was
  introduced (the new cases live in existing `Cas*`-prefixed suites), so the enforced prefix invariant is
  intact. ✔

## 8. Gates I ran myself

All under `flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`, on the lane-g worktree.

| gate | result | log |
|---|---|---|
| release build `unit_tests_dbms` | `NINJA_EXIT=0` | `/home/mfilimonov/workspace/ClickHouse/lane-g/build/t6b_review_build.log` |
| ASan build `unit_tests_dbms` | `NINJA_EXIT=0` | `/home/mfilimonov/workspace/ClickHouse/lane-g/build_asan/t6b_review_asan_build.log` |
| suite generator (release) | 278 suites, 21 excluded, **0 unclaimed** | `/home/mfilimonov/workspace/ClickHouse/lane-g/build/t6b_review_gate_gen_release.log` |
| **full CA gate, release** | **pass=278 fail=0 abort=0** | `/home/mfilimonov/workspace/ClickHouse/lane-g/build/t6b_review_gate_release.log`, per-suite `build/per_suite_results.txt` |
| **full CA gate, ASan** | **pass=296 fail=0 abort=0** | `/home/mfilimonov/workspace/ClickHouse/lane-g/build_asan/t6b_review_gate_asan.log`, per-suite `build_asan/per_suite_results.txt` |
| mutation build + run | 2 targeted failures, 10 pass | `/home/mfilimonov/workspace/ClickHouse/lane-g/build/t6b_review_mutation_test.log` |
| post-restore rebuild + rerun | `NINJA_EXIT=0`, 49/49 | `/home/mfilimonov/workspace/ClickHouse/lane-g/build/t6b_review_restore_test.log` |

Builds were verified (`NINJA_EXIT=0`) before any green test result was trusted. No soak was run.

---

## 9. The three judgement calls

**(a) Uncalibrated defaults — ACCEPTABLE, with one number worth a second look.**
5000 graduations / 5000 redeletes / 5000 ref-cleanup objects / 20000 prefix objects / 5000 recovery ops are
all far above any plausible steady-state round and far below the BACKLOG's recorded 20,046-candidate
collapse, so they bound the pathological burst without touching normal operation; deferring calibration to
T8's soak is right, since a number invented now would be no better founded. The one I would look at again
is `gc_round_sweep_namespace_budget = 20`: unlike the others it is not a "far above normal" number — a pool
with more than 20 namespaces whose debris lands on one page will retain the rest of the page's namespaces
every round. That is safe (retention always is) and it does not wedge (the cursor still advances), but it is
a *throughput* setting masquerading as a *pathology* setting, and the orphan sweep is the only reclaimer for
that debris. Worth an explicit soak observation in T8 rather than a change now.

**(b) `recoverRefTableDetailedFromAuthority` counted as one coarse unit — CORRECTLY SCOPED, CORRECTLY
DESCRIBED.** The reasoning holds against the code: `fsck` and the GC rebuild path both need a *complete*
table, so a budget parameter on the shared primitive would either be ignored by them or would change their
contract. Bounding only the walk this file owns is the right seam. The description in both the header
comment and the report states the residual accurately, including that exhaustion *before* the call skips it
entirely. One thing neither says, worth noting for T8's residual row: the `cursor_got` GET before the tail
loop and the extra GETs inside `cross_from_missing_cursor` are also uncounted — a small, bounded undercount,
not a hole.

**(c) No direct `GcOutcomes` byte assertion — the claim is UNPROVEN, and in fact FALSE as stated.** This is
finding C1. The missing assertion is the lesser problem; the reasoning offered in its place does not hold,
because `spared` entries reach the same outcome body under no cap and `graduated` entries reach it not at
all. My recommendation is *not* to add the byte assertion in isolation — an assertion on a quantity that is
still unbounded would be a fence trusted for more than it checks. Either cap the spared arm (or the outcome
entry count directly, which is the honest bound) and then assert, or record the residual openly in the
ledger. Do not close T6-2 on the current wording.

---

## Findings, labelled

**CODE (blocking-capable; none of these blocks, see verdict)**

- **C1** — `GcOutcomes` per-shard body is not bounded: the `merge.spared` loop in `Gc::runRegularRound`
  appends one `OutcomeEntry` per spared entry with no cap, and `spared` is filled from the uncapped durable
  retired set (`settleEntry`'s `indeg > 0` branch). The plan's "bounded as a consequence" and the report's
  entry-mapping argument are both false. *Follow-up: cap the outcome-entry count per shard, or record the
  residual and stop claiming the bound.*
- **C2** — `folded.mf_cleanup` (post-CAS `manifest_deletes` phase) is still an uncapped per-round burst of
  exact deletes; codex T6-2's fourth required-fix bullet is unaddressed. Outside the plan's file list, so no
  assigned work was skipped. *Follow-up: same round budget, one new field.*
- **C3** — T6-1's retained-**byte** axis remains unbounded (≤100 bodies × up to 256 MiB each). Plan-consistent,
  but T6-1 is not fully closed.
- **C4** — prune and the one-shot post-CAS hand-off share one `max_prefix_wholesale_objects` with prune first;
  a prune-heavy round can starve the hand-off to zero and permanently strand its generation prefix (fsck-only
  recovery). Disclosed at the site; a new leak path nonetheless.

**TEST** — none. The new tests are non-vacuous, mutation-sensitive, death-test-clean and gate-reachable.

**PROSE (batched to `docs/superpowers/cas/deferred-docs-fixes.md`, never blocking)**

- **P1 (IMPRECISE)** — commit `5295d6c54ae`'s subject and the plan's Slice-2 checklist line say "page/**byte**/
  recovery budgets"; no byte budget exists in `GcRoundWorkBudget` or anywhere else. The report's own heading
  already corrected this to "page/namespace/recovery"; the commit subject and plan line did not.
- **P2 (FALSE)** — the report's Slice-1 sentence "each `redelete`/`spared`/`graduated` entry maps to exactly one
  `OutcomeEntry` … so a bounded redelete count is a bounded outcome-entry count by construction". `graduated`
  maps to none; `spared` is uncapped. (Prose form of C1.)
- **P3 (IMPRECISE)** — `GcRoundWorkBudget::max_sweep_namespaces`' comment ("how many DISTINCT namespaces **one
  page** may build a fresh protection view for") and the call-site comment ("the round's per-page namespace
  cap") describe a per-page cap; the counter is per-round cumulative and never reset. Equivalent today only
  because the sweep takes exactly one page per round — the comment states more than the field guarantees.

**Verified and NOT a finding** (recorded so the next reviewer need not redo it): the `case
SweepRetainClass::WorkBudgetExhausted: … /// unreachable: the premise never returns this class itself`
comment is accurate — `manifestDeletionPremise` never emits that class; only the planner sets it.

---

## Shared-worktree note

Another agent committed the comment cleanup (`84c24d3abbf`, then a report commit) into the same worktree
while my mutations were live in the working tree. I re-checked afterwards: `git show HEAD:` for both files
contains the intact `if (!fully_drained) break;` and `decided_through = listed.key;` fences, no `MUTATION`
marker is in HEAD or the working tree, and `git status --short src/` is empty. Nothing of mine was committed
and nothing of the other agent's was destroyed. My two gate runs executed against sources byte-identical to
the current HEAD's CAS files.
