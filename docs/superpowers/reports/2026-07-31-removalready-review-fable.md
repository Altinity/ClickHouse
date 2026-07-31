---
description: 'Adversarial review of the RemovalReady counter-proposal for CAS Task 5: verdict adopt, with the carrier enumeration it cannot skip, four silent drops of its own, and two posture changes that need owners'
sidebar_label: 'RemovalReady review (Fable)'
sidebar_position: 103
slug: /superpowers/reports/removalready-review-fable-2026-07-31
title: 'RemovalReady counter-proposal — adversarial review, Fable'
doc_type: 'reference'
---

# RemovalReady counter-proposal — adversarial review (Fable) {#removalready-review}

Written 2026-07-31 against the critique, the 433-line proposal, plan `:1106-1268`, spec INV-3/§3 as
amended by `c5a6243a325`, and the tree. Every load-bearing claim below was re-verified at the source,
including the two that indict my own round-3 report.

## Verdict up front {#verdict}

**Adopt `RemovalReady`.** The two findings that killed the ordering-only design are real — I verified
both at their sources, and both invalidate positions I argued three hours ago:

- **Blocking 1 is exactly as stated.** `Gc/CasGc.cpp:1990-1998` force-adds *every* `Live`/`Removing`
  catalog entry to `walk_targets`, unbudgeted ("the catalog is the universe authority now, so a
  namespace it lists is not optional coverage"), and `:2557` writes `per_ns_shard[cursor_key]` for
  every walked target. So the pruned-seal observation is not stable while the entry says `Removing`:
  the very frontier-proof loop Stage B added re-creates the cursor each round. My round-3 claim that
  ordering alone makes rebirth-inheritance unconstructible was wrong, and killing the
  incarnation-scoped cursor *on that argument* was premature. `RemovalReady` is the correct repair:
  it makes "never fold this again" a durable catalog fact instead of a temporal claim about seals.
- **Blocking 2 is exactly as stated.** `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore.tla:35-44`
  constructs precisely the interleaving my D1 declared unconstructible — leader captures, stalls
  through entry-delete *and* rebirth, resumes once. I flagged my derivation as prose needing a model
  (round 3 §5.1) and still collapsed the test family; the counterexample already existed in the tree.
  Capture-at-deposition is load-bearing, its test and the stale-leader-after-rebirth data-loss test
  stay, and the proposal keeps both. Correct.

The rest of this review is the price of adoption: one enumeration the proposal wrongly deletes, four
silent drops of its own, two posture changes that need explicit owners, and the answers to the five
questions asked.

## Q1 — Is cursor retirement actually monotone, or is the oscillation relocated? {#q1-monotone}

**Monotone — but only if the `RemovalReady` predicate is enforced at every cursor/walk producer, and
the proposal deletes the very enumeration that proves it is.** Its "what this removes" list includes
"enumeration of every possible `walk_targets` carrier". That is self-undermining: the monotonicity
claim *is* a claim about all carriers. The enumeration does not disappear — it becomes the
implementation checklist of the universe predicate. I did it; there are five producers:

1. **The catalog-only frontier loop** (`Gc/CasGc.cpp:1990-1998`) — the proposal's named site; exclude
   `RemovalReady` from the walked set. Covered.
2. **The parent-cursor carry** ("unhinted namespaces carry verbatim", `:1814-1840` block; step 6's
   "omits the shard-0 cursor from the next seal") — the carry loop must consult the catalog state per
   carried cursor, not merely per walked target. Covered by step 6, but the task text must name this
   as a *carry-loop* change, distinct from the walk filter.
3. **Hint-named namespaces** (`ref_tables` from the round's listing) — a `RemovalReady` namespace's
   surviving debris (`<ns>/<inc>/_log` keys the one bounded pass missed) is still LISTed, so the hint
   names it and the walk loop at `:2001` would fold it and write a cursor at `:2557`. **Not covered
   by the proposal's text.** The exclusion must apply to hint admission too, and the R10-style
   un-cataloged classification must treat `RemovalReady` as "cataloged, not walkable" — a third bucket
   distinct from both `Live`/`Removing` and absent.
4. **REBUILD** — spec §7 rebuilds cursors from catalog + `_ckpt` + arithmetic tails. A `RemovalReady`
   entry still has its `_ckpt` (deleted only at step 7) and its debris tail, so an unfiltered REBUILD
   resurrects the cursor and `FORCE REBUILD` breaks monotonicity. **Not covered.** REBUILD's universe
   must apply the same predicate. (The proposal's comparison table even implies REBUILD is untouched —
   that is exactly backwards: it is untouched by *key format* but must gain the *predicate*.)
5. **Carried holds** — a hold forces an exact walk "even when the hint omits the namespace" (spec §5).
   A namespace cannot legitimately hold a hold in `RemovalReady` — the terminal cannot have folded
   through an offending position — but that is a derivation, not an enforcement. The
   `Removing → RemovalReady` CAS needs an explicit **no-hold precondition** (refuse the transition
   while `ShardCoverage::classification == 4` exists for the namespace), plus the sabotage test: plant
   a hold, attempt the transition, require refusal. Otherwise a hold carried into `RemovalReady` walks
   a namespace the state promises is never walked, and the two rules deadlock or destroy the hold —
   both wrong.

One more window, benign but worth stating so the step-7 test pins it: a round whose catalog cut was
taken *before* the CAS may write the cursor once more after the CAS lands. GC rounds are serialized
through the single `gc/state` adoption, so exactly one such round exists; the next round's cut sees
`RemovalReady` and re-prunes, and step 7's revalidation ("current adopted seal … cursor absent") makes
the driver wait out that one round. Monotone-after-visibility, convergence ≤ 1 round. The "run several
GC rounds after the stop" tests the proposal specifies are the right shape; add the deliberate
stale-cut interleaving (start a round, land the CAS mid-round, finish the round) as its own case.

**With those five sites written into the task, the answer to Q1 is yes — the oscillation is not
relocated, because the state is consulted at the same commit point that would re-create the cursor,
rather than being an assertion about a past seal.**

## Q2 — Is "smaller than life-keyed cursors" true? {#q2-smaller}

**Yes, and I change my position** — my round-1/round-3 stance was "cut the re-key, keep the
ordering", and Blocking 1 killed the ordering half, so the honest comparison is now `RemovalReady`
versus life-keyed cursors, not versus nothing. Measured against the tree: the cursor identity
surfaces in **eight files** (`Formats/CasFoldSealFormat.{h,cpp}`, `Gc/CasGcShardPlan.h`,
`Gc/CasGc.{h,cpp}`, `Gc/CasOrphanManifestSweep.cpp`, `Tools/CasInspect.cpp`, plus the catalog format's
own reference), spanning the wire grammar, every builder/parser, REBUILD, fsck and the sweep's
coverage lookup — against one enum value, one codec branch, one transition, and one predicate applied
at the five sites of Q1.

Two arguments beyond the count, because the commissioner has said incarnations *in GC state* do not
worry them and corner-case code does:

- Life-keying solves inheritance but **creates a new corner-case class**: dead-life cursors. A reborn
  name's cursor starts fresh at `{life₂, 0, 0}`, and the old `{life₁}` cursor sits in the seal with no
  catalog entry to retire it — someone must prune it or the seal grows by one row per historical
  removal, which is the `Retired`-state growth argument relocated into `gc/state`. That someone is
  retirement machinery, i.e. exactly what `RemovalReady` provides — so life-keying does not avoid the
  state, it only hides the need for it.
- `RemovalReady` is a *catalog state*, which is the shape the commissioner asked lifecycle facts to
  take ("better that this lives directly in the catalog"). Life-keyed cursors encode a lifecycle fact
  in a key format — the pattern this whole review cycle has been deleting.

So the claim survives my own churn-grounds objection: the churn is real but bounded to five predicate
sites, and the alternative's churn is wire-format-wide *plus* an unfixed retirement problem.

## Q3 — Does the mandatory-catalog requirement carry its cost honestly? {#q3-mandatory}

**The cost is real, small, and pre-release-week sized — and it is not new lifecycle design, it is the
deletion of an existing special case.** Today `CasRefCatalog::read` maps an absent key to an empty
catalog "mirroring the bootstrap contract of every other token-CAS singleton"
(`Pool/CasRefCatalog.h:21-29`), and the R11 family (empty-universe-vacuous, R11b, R11c) exists
precisely because "absent" and "virgin" are conflated downstream. The change — pool creation writes
the empty catalog before the format is marked ready; thereafter absent-or-undecodable is
`CORRUPTED_DATA` stopping creation, removal and GC — replaces the R11 damaged-catalog special-casing
with one bootstrap ordering. Migration is recreate-only, which this branch already mandates
(pre-release, no persisted-data compat). Concretely: one write in pool bootstrap ordered before the
format-ready mark, one behavior change in `read`, every absent-branch caller revisited (they are few:
`read`'s callers all go through the `Snapshot.token == nullopt` contract), one fail-closed test. A
day, maybe two with the callers audit.

Two honest costs to state in the task rather than discover later: (a) the blast radius of a lying
store — a single authoritative-absent misread of `cas/ref_catalog` now halts the pool's DDL and GC
until a re-read succeeds; that is a *stop*, not damage, and it is the fail-close direction this
design chose everywhere else, but the error message must name the object and the recovery (re-read /
REBUILD refusal path, never "recreate the catalog"). (b) `casUpdate`'s existing present-then-vanished
`LOGICAL_ERROR` (`Pool/CasRefCatalog.h:82-88`) becomes reachable from more entry points; its message
should be unified with the new one so operators see one corruption story, not two.

On the tu-quoque — the other reviewer previously rejected this as too much lifecycle design and now
proposes it: the difference is that round 2's rejection was of *retirement* lifecycles (markers,
`Retired` rows — unbounded); this is a *bootstrap* boundary (one-time, O(1)). The reversal is
consistent once those are distinguished, and the proposal does distinguish them. Not a strike.

## Q4 — Deleting the fabricated-missing-entry anomaly test {#q4-anomaly}

**The impossibility argument is sound, and the deletion is legitimate — as a relocation, not a
removal.** The two worlds (legal removal's surviving debris; fabricated entry loss with the same
debris) are byte-identical in the store once `_cleanup` and the cleanup item are gone; a classifier
cannot conjure the distinction, and my round-3 step 7 demanded exactly that. Codex's Blocking 3 is
right and the proposal's resolution — catalog fully authoritative — is the correct one of the three,
*because the other two are already rejected elsewhere* (finite removal evidence = a marker class;
entry-until-proof = the physical-empty gate).

But the soundness has two preconditions, and both must be pinned or the deletion is unsafe:

1. **Individual entries must be unable to vanish illegitimately.** True structurally — one atomically
   read, token-CAS-written object; partial loss is impossible, whole loss is now `CORRUPTED_DATA`
   (Q3). What remains is a *legal-shaped* rogue CAS, and that is preventable only at the mutation API.
   So the test moves there: the only path that may delete an entry is step 7's exact-CAS of a complete
   observed `RemovalReady` row; add the negative — a `Live`/`Removing`/`Creating` row cannot be
   deleted through any exported mutation, and `casAdmitEntry` still cannot carry a removal (its
   admission-only shape, `Pool/CasRefCatalog.h:112-121`). The proposal's state-machine tests cover the
   transition table; the *no-other-deleter-exists* negative is the relocated fabrication test and must
   be named as such.
2. **Silence must not be total.** The proposal classifies "a canonical cursor for a namespace absent
   from the catalog" as retired metadata, silently omitted. Under the new invariants that shape has
   exactly three producers: the benign one-stale-round window (Q1), a bug in the five-site predicate,
   or catalog corruption that somehow passed the Q3 gate. Omitting it from the seal is right — the
   measured pool-wide suppression stall must never return — but omitting it *silently* masks the
   second producer. Count it (ProfileEvent + one log line naming the cursor), never suppress on it.
   Alert-only, exactly the posture probe A ended at.

**Where the reasoning must live** so no future reader re-adds the requirement: (a) the spec's
read-side/catalog-authority text, as one sentence — "entry absence is authoritative; canonical
non-current-life objects are inert debris, never evidence of damage; invalid entry deletion is
prevented at the mutation API, not inferred from debris"; (b) a comment at the classifier site
stating the *reason* (two indistinguishable worlds) without citing reports or rounds, per the
comment policy; (c) the relocated negative test's name should say it (`FabricatedEntryLossIs
PreventedAtTheMutationApiNotDetectedFromDebris` or similar), because tests are the one place a
requirement's ghost reliably haunts.

## Q5 — What the proposal itself drops silently {#q5-drops}

Diffed against old Task 5 (`:1106-1268`), the rewrite, and the critique's own nine-item list. Four
real drops, two posture changes misfiled as drops, and several small ones.

**Dropped, must return:**

1. **The perpetual janitor — and with it the M5 leak closes over.** The proposal folds `_files`
   cleanup into the *one* bounded attempt (step 4) and never mentions the ongoing janitor again,
   while spec INV-3 still mandates "a lazy janitor deletes foreign-incarnation debris whenever
   listed" and the old Task 5 made it "the ONLY reclaimer of dead-incarnation `_files`"
   (`:1158-1165`). If the single attempt's LIST omits a file, nothing ever reclaims it — the Task-4b
   leak interval (`:1210-1224`) returns, now permanent by design. Leak-only, but it silently repeals
   a spec sentence. Either restore the opportunistic janitor (it is small, and its suppression test
   is already in the proposal's list) or amend the spec to say dead-life debris may leak until fsck
   — a decision, not a default. With the janitor restored, `namespaceAllLivesPrefix` and its
   concept-negative tests (critique drop #8) return with it.
2. **The unauthorized terminal-append negative test** — the critique's own drop #1, absent again from
   the proposal's required-tests list. Step 2 states the fence rule; no test pins it. A happy-path
   owner test cannot catch the fence check's removal.
3. **The no-physical-empty executable proof, reshaped.** `NamespaceRemovalDoesNotListOrDeleteFiles`
   asserted *no* `_files` LIST on the mandatory path; the proposal deliberately allows a best-effort
   `_files` LIST inside step 4, so the old test is wrong as written — but its *point* (no LIST result
   gates a lifecycle transition) must survive as an op-journal test: `_files` objects planted and
   their cleanup forced to fail → the item still reaches `Completed`, the `Removing → RemovalReady`
   CAS still fires, entry deletion still lands. The proposal's reader-behavior test covers the
   read-side half only. Name the gating half explicitly or the physical-empty dependency can creep
   back through step 4's failure handling.
4. **Task-7 coupling.** Step 7's dead-root resume points at "the existing dead-root/decommission
   path", but Task 7's branches were specified against `Removing`-without-`_ckpt`; they must now also
   own `RemovalReady`-without-`_ckpt` (post-step-7.1 stop) and `RemovalReady`-with-`_ckpt`
   (pre-step-7.1 stop on a dead root). Two new resumption rows in Task 7's table, or the window
   regains an unowned case — the exact orphaning M1 existed to prevent.

**Posture changes that need explicit owners, not test lines:**

5. **A corrupt terminal record now wedges the name permanently.** Step 4: unreadable/unvalidatable
   terminal → item stays `Pending`, fail closed — so `RemovalReady` is never reached and the name is
   never recreatable, until `FORCE REBUILD` or decommission. The old design's "cleanup failure is
   leak-only, never blocks lifecycle completion" (critique drop #4) is *deliberately* inverted here
   for the manifest half, and the trade is defensible (exactness over liveness for real corruption)
   — but it is a posture flip on a wedge-shaped failure, and per this project's own parser-posture
   rule it needs the escape path named in the same breath: which actor unwedges it, and what the
   operator sees meanwhile (the stuck-removal surfacing of step 2 must fire for a `Pending`-wedged
   item too, not only for a missing terminal).
6. **The `Removed` snapshot's fate is left conditional** ("if its only remaining role is…"). It has a
   second consumer: `namespaceIsRemoved`'s cold-runtime recovery reads the `Removed` lifecycle
   (`Pool/CasPool.h:500-509`), which the reader-absence predicate rides on. The proposal's
   reader-behavior tests pin the behavior but nothing decides the mechanism. Decide it in the task:
   reader absence comes from the catalog cut (`Removing`/`RemovalReady`/absent), the `Removed`
   snapshot and the `_cleanup` gate both die together, and `namespaceIsRemoved` is rewired or
   deleted. Conditionals in a task brief become drift.

**Small, re-add as one line each:** the hygiene riders (false `per_ns_shard` comment `:1260-1263`,
4-C's accepted-cost comment `:1275-1276`, the "don't test frees-at-`Removing`" note); the per-entry-
point split of the zero-mutation R12 test (`listRefs` / `resolveRef`+DROP DETACHED / table removal
separately, per the critique's step-1 note); execution gates and the commit step (critique drop #9);
and the TLA phase-0 obligations the proposal implies but does not list — `CaRefCatalogCore` gains the
fourth state and its transition table, `CaRefDeltaIntakeCore`'s catalog sample gains `RemovalReady`
(so the frontier-proof sabotages can express it), and the no-hold precondition (Q1 site 5) wants a
sabotage config.

**One thing the proposal is charged with that I do not count against it:** GC becomes a catalog
*writer* (the `Removing → RemovalReady` CAS) for the first time. That is new coupling and needs its
fence seam stated (the GC leader's generation threaded through `casUpdate`'s `mutate`, same as every
other fenced caller) — but it is one write per completed removal, invisible next to the hot-spot
numbers, and the alternative (the mount-side driver performing it) reintroduces a liveness dependency
on a writer that may never remount. GC is the right actor; just name the fence.

## Answers in one line each {#summary}

1. **Monotone: yes**, iff the predicate lands at all five producer sites (walk filter, cursor carry,
   hint admission, REBUILD, no-hold precondition) — the carrier enumeration is the checklist, and the
   proposal must stop claiming to delete it.
2. **Smaller: yes** — eight files of wire-format churn plus an unsolved dead-cursor retirement problem
   versus one state and five predicate sites; I retract my round-1 position.
3. **Mandatory catalog: real, small, honest** — it deletes the R11 special case rather than adding
   lifecycle design; name the lying-store blast radius.
4. **Anomaly-test deletion: sound as a relocation** — pin the mutation-API negative, keep alert-only
   counting for entry-less cursors, write the two-worlds reason into spec + classifier comment.
5. **Its own drops: four real** (perpetual janitor/M5, terminal-append fence test, the reshaped
   no-gating proof, Task-7's two new resumption rows) **plus two unowned posture changes** (permanent
   wedge on corrupt terminal, the `Removed`-snapshot conditional).

**Adopt, with the fix list above folded into the task text before implementation starts.** The state
machine is right; what failed twice today was not anyone's protocol but the practice of asserting
"no path exists" without enumerating the paths — Q1's five sites are that enumeration for this
design, done once, in writing, where the next rewrite can find it.
