# CAS relink / write-release seam — TLA+ gate phase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> ## ⛔ PLANNING STOPPED 2026-07-30 — DO NOT EXECUTE THIS PLAN AS IT STANDS {#planning-stopped}
>
> A pre-committed stopping rule fired. Four codex rounds each found that the model could not express
> some arm of `CasRefLedger::resolveWedgeOnce`; round 4 found four more AND the structural reason the
> approach kept failing: **the cited cardinalities are NESTED DECISIONS, not mutually exclusive
> alternatives**, so six `WedgeResolution` values × three `SlotOccupyResult::Kind` × four occupant
> classes × seven `Reason` × two exception exits does NOT imply twelve terminal paths, and an
> enumeration built that way is neither disjoint nor complete. The conclusive finding is therefore not
> "one more arm was missed" but "a faithful model of this machine keeps being out of reach, and the
> reason is that the machine COMPOSES decisions rather than selecting among alternatives."
>
> Per the rule, the model goes AS-IS to the user-approved lane-state-machine restatement pass
> (BACKLOG `{#lane-state-machine-restatement}`): simplifying the machine comes before gating anything
> on it. **Do not resume this plan** until that pass reports; when it does, this document is the
> starting point, not a document to be re-reviewed.
>
> ROUND-4 FINDINGS PRESERVED AS INPUT (so resumption is not from scratch):
> 1. `classifyRefLogOccupant` (`CasRefLedger.cpp:162`, called `:1838`) can rethrow allocation /
>    memory-limit / other non-corruption exceptions — an unenumerated path.
> 2. **`slotOccupy` returns `Created` — the transaction is now DURABLE — and the subsequent recheck
>    then reports `FenceMoved` / `Superseded` / `WedgeReplaced`, so the function returns
>    `StillWedged`.** Mapping row 5 calls that "NO TRANSITION", which would LOSE the newly durable
>    transaction; row 11 covers only `Created → Adopted`. The rows are neither disjoint nor
>    one-per-path. This one is also a CODE observation worth the pass's attention: a
>    durability-producing retry whose outcome is inert records the durable fact only in the
>    wedge-plus-marker, never in the resolution's return value.
> 3. The post-durable install catch (`:1997`) raises the floor, poisons, keeps the wedge and
>    RETHROWS — intended unreachable under §A1, but a real throw path (reachable through the test
>    probe) that must be enumerated or structurally excluded.
> 4. Row 3 is too narrow: the pre-I/O candidate preparation (`wedge = *rt->wedge`,
>    `candidate.emplace(rt->state)`) can throw BEFORE the named decode/apply calls.
>
> TWO REAL DEFECTS IN v4 to fix if the gate is ever resumed:
> - **The gate-critical witness is broken.** `WedgeResolveInstall` sets `sawRetryCreatedAdopt`
>   unconditionally (`H(TRUE)`), so the shorter route `Admit → Arm → SenderUnresolvedLanded →
>   WedgeResolveInstall` violates `W_RetryCreatedAdopt` WITHOUT executing `WedgeRetryCreated` — the
>   witness does not prove the happy path it exists to prove. That is the vacuity trap it was written
>   to prevent, one level up.
> - `_sab_nopoison` accepts a red through the stale route, which the mapping itself declares an
>   intentional OVER-APPROXIMATION unreachable with today's one-leader machine — contradicting this
>   plan's own rule that a sabotage driven by fabricated states is worthless.
> - Minor: the self-review says `sFloorCovers` is read as a guard by five actions; six read it
>   (`WedgeRetryCreated`, `WedgeResolveInstall`, `WedgeResolveRejected`, `WedgeResolveCorrupted`,
>   `WedgeResolveStale`, `FloorReconcile`).
>
> THE AUTHOR'S OWN POST-MORTEM, kept because it makes resumption cheaper and states the method rule:
> - **Why the product argument was wrong**, in their words: `WedgeResolution` is the RESULT of the
>   occupant adjudication, `Reason` is a REFINEMENT of one `WedgeResolution` value, and neither is
>   independent of `SlotOccupyResult::Kind`. "A product of cardinalities bounds a cross-product of
>   INDEPENDENT choices; this machine composes, so the product bounds nothing. My 'exhaustive by
>   construction' claim was a category error dressed as rigour, and it is worse than the
>   inspection-based enumeration it replaced, because it READS as a proof."
> - **The `Created`-then-inert-recheck row is an UNDER-approximation**, which this plan's own Global
>   Constraints already condemn ("an over-approximation is the safe side; an under-approximation is a
>   defect"). The rule caught it; row 5 was classified inert BEFORE the `Created` arm existed and was
>   never re-derived afterwards.
> - **THE WITNESS FIX, one variable:** `sawRetryCreatedAdopt' = … \/ H(TRUE)` carries no provenance,
>   and `WedgeResolveInstall` is reachable from row 10 as well as row 11 — since TLC reports the
>   SHORTEST counterexample, the witness fires RELIABLY on the row-10 route, so it is guaranteed
>   vacuous rather than flaky. Fix: set a `retryCreatedThisWedge` flag in `WedgeRetryCreated`, clear
>   it wherever the wedge clears, and gate the history conjunct on `H(retryCreatedThisWedge[m])`.
> - **`_sab_nopoison`'s rule, generalized:** trace-checking was required of `_sab_nowedge` and not of
>   `_sab_nopoison`, which was then permitted a red through the very row-9 arm the mapping labels an
>   over-approximation. The rule to carry: **a red may not rest SOLELY on an over-approximated route.**
> - **THE METHOD RULE worth more than this plan:** a cardinality argument is only a BOUND when the
>   dimensions are INDEPENDENT, and the cheapest way to find out is to ask which of them is computed
>   FROM another.
>
> WHAT r4 CONFIRMED SOUND, and it is most of the document: `WedgeRetryCreated`'s state transition and
> guards; the licensing contract quote; `MaxId = 6` sufficiency; B1(b) (both exception exits modelled
> as inert, `RefusedPreAttempt` correctly attributed to an `Unresolved` RESULT); the `sApplyOwed`
> writer set; `MarkerCoversDurableWindow` holding in every relevant state (verified state by state);
> the 32-config accounting with every green's red preceding it; and `_sab_nowedge`'s only-route
> argument. The gate's STRUCTURE is not what failed — the machine's describability is.

> ## Restatement completed 2026-07-30 {#restatement-completed}
>
> The replacement was built from semantic obligations first and then implemented:
> `docs/superpowers/specs/2026-07-30-cas-ref-lane-state-machine.md`,
> `docs/superpowers/models/CaRefLaneCore.tla`, and
> `docs/superpowers/models/CaRelinkLaneComposition.tla`. The old product/enumeration plan remains
> stopped and historical; any resumed relink work should consume the new `Ready`-only certification
> contract instead. TLC results are recorded in
> `docs/superpowers/models/CaRefLaneCore_RESULTS.md`.

**Goal:** Discharge §12 of `docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md` — build the refined model the redesign requires, run it, and answer the one question that can still stop the design: **does `_sab_stalecache` flip from RED to GREEN once the apply-pending marker is represented?** If it does not, the design does not ship.

**Architecture:** One new TLA+ module, `docs/superpowers/models/CaRelinkReofferCore.tla`, which REFINES `CaRelinkConfirmCore` rather than editing it (§12's disposition ruling: the v11 model is kept as the historical witness of the v11 protocol and is not touched, because its `_sab_*` reds are the evidence that v11's rules were each load-bearing). The refinement adds five things the v11 model cannot express: the apply-pending marker with a *separate reader-visible value* (`sApply` / `sApplySeen` — the model-level twin of seam §6, including every arm of `CasRefLedger::resolveWedgeOnce`, whose bounded retry is itself a durability-producing event), an equal-namespace/different-`disk_name` mount pair (§12.5 row i, test row 17's B1 shape), a leader tenure that commits several durable chunks of *different kinds* (§12.5 row ii), a receiver whose acceptance is the *conjunction* of a certified answer and a returned identity (§4.2, §4.4), and a committed-relink fact that covers the landed-`Unresolved` publication as well as `Committed`. Around them, a 2×2 necessity matrix decides the gate, a cross-mount battery decides the validator's qualification, and the re-derived rule set decides that every retained rule is still load-bearing under the fence-first ordering. Cfg names are deliberately carried over from the v11 model so that the flip is greppable side by side: `CaRelinkConfirmCore_sab_stalecache` RED next to `CaRelinkReofferCore_sab_stalecache` GREEN.

**Tech Stack:** TLA+ / TLC 2 (`tmp/tla2tools.jar` → TLAToolbox 1.7.4, OpenJDK 21), invoked exactly as the existing `docs/superpowers/models/run_*.sh` harnesses do. No C++ in this phase.

## Global Constraints

- Branch `cas-gc-rebuild`; commit after every task; **NEVER `git push`**.
- Commit messages: `ca: tla — <what>` plus this session's standard trailer lines.
- **NEVER weaken an existing invariant, or an existing model, to make a new config pass.** `CaRelinkConfirmCore.tla` and all twelve `CaRelinkConfirmCore_*.cfg` files are **read-only for this whole plan** — the only file of that family this plan may touch is `CaRelinkConfirmCore_RESULTS.md`, and only to append a pointer section (Task 4). If a new config only passes after an invariant is narrowed, that is a FAIL to record, not an edit to make.
- **Every model transition must be traceable to a cited code site, and the transition relation must be traceable to an EXHAUSTIVE enumeration of the code's outcomes.** A sabotage that is red only because the model fabricated a state the code cannot reach is worthless — and a green that is green because the model cannot reach a state the code *can* is worse, because it hides the gap inside the verdict. The wedge block therefore carries a mapping that enumerates every value of `WedgeResolution`, every value of `SlotOccupyResult::Kind`, every occupant class, every `Reason`, and both exception exits, with one line each; where an arm maps to no transition, the mapping says which and why. Auditing that mapping against the code is Task 1 step 12.
- **Where the model over-approximates the code, say so at the invariant or action.** An over-approximation (a state the code believes unreachable but the model admits) is the safe direction for a safety gate and is kept; an under-approximation is a defect. Both are recorded, never silently chosen.
- **Every cfg change is reviewed against its sabotage intent before it is run.** Each `_sab_*` cfg header states, in one sentence, which single rule it removes and which invariant that must break; a cfg whose observed red is a DIFFERENT invariant than the one named is a FAIL, not a pass — the runner asserts the invariant NAME, not the exit code.
- **A green is only evidence once the property it rests on has been seen RED — in the same task, not a later one.** Sabotages run before greens, in every task and in the runner's config order. Concretely: no green may list an invariant whose `_sab_*` config does not yet exist. `TypeOK` is not exempt (`_sab_typeprobe` is its control) and neither is `PromotedNeverDangles` (`_sab_publishafterconfirm` lives in Task 1 for exactly this reason).
- **Structural exclusions are recorded, not asserted.** Where a hazardous state is made unrepresentable by an action guard rather than excluded by an invariant, the guard's comment says so and RESULTS lists it under structural exclusions. An invariant over a state the transition relation cannot enter would be green for free, which is the opposite of evidence.
- **Every TLC run is logged with markers, and no two runs can collide.** The runner takes a `RUN_ID` (`date +%Y%m%dT%H%M%S`-plus-PID), puts every metadir under `tmp/tlc-meta-relinkreoffer/$RUN_ID/<cfg>` and every log under `tmp/tlc-runs/relinkreoffer/$RUN_ID/tlc_<cfg>.log`, and holds an `flock` on `tmp/.relinkreoffer.lock` for the whole battery so two invocations cannot interleave. Each invocation is bracketed by `=== TLC BEGIN <module>_<cfg> <ISO-8601> ===` / `=== TLC END <module>_<cfg> rc=<n> <ISO-8601> ===`. Stable convenience symlinks `tmp/tlc_CaRelinkReofferCore_<cfg>.log` point at the newest run; **RESULTS cites the per-run path, never the symlink**, so a row can always be re-read.
- TLC invocation pattern (derived from `docs/superpowers/models/run_refcatalog.sh`): `/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC -metadir <per-run metadir> -workers "${TLC_WORKERS:-1}" -config <cfg>.cfg CaRelinkReofferCore.tla`. Verdict: `grep -q "No error has been found"` ⇒ green; `grep -q "is violated"` ⇒ violation, with the invariant name extracted by `grep -oE '(Invariant|Property) [A-Za-z_]+ is violated'`.
- `-workers 1` by default, **not** `-workers auto`, following `run_refcatalog.sh`: parallel BFS makes WHICH shortest counterexample TLC prints nondeterministic between identical runs, and every trace this plan narrates in RESULTS is a specific action sequence. Override with `TLC_WORKERS=auto` when only a verdict is wanted.
- **Every declared CONSTANT must be assigned in every `.cfg`.** Task 1 declares the COMPLETE constants block — all twenty-two, including dials only exercised in Tasks 2 and 3 — and Task 1's configs set the unexercised ones to their honest value. Adding a constant later would force an edit to every previously written cfg; do not do that. **v4 note: the constants block is unchanged from v2/v3 and is deliberately not touched.** The wedge fixes of Task 1 step 4 are new VARIABLES, actions and guards, never new dials, precisely so that no cfg has to churn.
- Model sizes: `MaxId = 6`, `MaxRound = 5`, `MaxChunks = 2`, `Receivers = {r1}`. These bounds were checked against the minimum trace depths of every sabotage before this plan was written (the deepest need two journal ids and three GC rounds; the two-chunk witness needs about eight transitions; `_sab_publishafterconfirm` about ten; the retry-created adoption witness about six). If a config exceeds ~10 min under `-workers 1`, shrink `MaxId` then `MaxRound` — **never drop a property**. Record the bound actually used in RESULTS.
- The spec and the code are the requirements source. Where this plan and spec §12 disagree, the spec wins and the discrepancy is reported rather than silently resolved. Where a REVIEW's paraphrase and the code disagree, **the code wins and the discrepancy is reported** — Task 1 step 1 records one such case.
- **The gate can fail.** If Task 1 step 10 finds `_sab_stalecache` RED, STOP: do not start Task 2. Record the counterexample, write the RESULTS file with `RELINK TLA GATE: FAIL`, and report the design as refuted. That is a legitimate, successful outcome of this plan.

---

## File map

| File | Responsibility |
| --- | --- |
| `docs/superpowers/models/CaRelinkReofferCore.tla` | NEW. The refined model: sender lane with the marker, its reader-visible twin, and every arm of the wedge lifecycle including the bounded retry that CREATES; two same-pool mounts; the fence-first answer as an (answer, identity) pair; the four-state receiver; GC. |
| `docs/superpowers/models/CaRelinkReofferCore_*.cfg` | NEW, 32 configs: 14 sabotages, 7 greens, 11 witnesses. |
| `docs/superpowers/models/run_relinkreoffer.sh` | NEW. Expected-verdict harness in the `run_refcatalog.sh` shape, with per-run metadirs, per-run logs, an `flock` guard, and BEGIN/END markers. |
| `docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md` | NEW. The phase verdict: `RELINK TLA GATE: PASS|FAIL` plus the do-not-implement consequence. |
| `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md` | APPEND ONLY (Task 4). One section pointing at the refinement and stating that the v11 `_sab_stalecache` RED is deliberately preserved. |

---

### Task 1: The refined module and the apply-marker 2×2 — THE GATE

This is the task that can stop the design. Everything else in the plan is conditional on its step 10.

**Files:**
- Create: `docs/superpowers/models/CaRelinkReofferCore.tla`
- Create: `..._sab_noapplypending.cfg`, `..._sab_noapplypending_window.cfg`, `..._sab_relaxedmarker.cfg`, `..._sab_typeprobe.cfg`, `..._sab_publishafterconfirm.cfg`, `..._v11_baseline.cfg`, `..._ctl_v11nomarker.cfg`, `..._sab_stalecache.cfg`, `..._witness_busylane.cfg`, `..._witness_midtenure.cfg`, `..._witness_proven.cfg`, `..._witness_delete.cfg`, `..._witness_corruptwindow.cfg`, `..._witness_retrycreated.cfg`
- Create: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: `docs/superpowers/models/CaRelinkConfirmCore.tla` (all 436 lines), `docs/superpowers/models/run_refcatalog.sh`, spec §12.3, §5.1.1, §5.1.2, seam §6, and — **before writing one line of TLA+ for the wedge** — `CasRefLedger::commitRefChunk` and `CasRefLedger::resolveWedgeOnce` IN FULL, plus the three type declarations that bound their outcome spaces:
  - `CasRefLedger.h:759-771` — `enum class WedgeResolution`: **six** values, `NoWedge`, `Adopted`, `FloorReconciled`, `Rejected`, `StillWedged`, `Corrupted`. Its own comment: *"`Adopted` is the ONLY outcome that lets the flush proceed to carve a new batch; every other one fails the whole carve with `survivor_error`."*
  - `Backend/CasRequestControl.h:399` — `enum class SlotOccupyResult::Kind { Created, Occupied, Unresolved }`: **three** values.
  - `CasRefLedger.h:784-801` — `resolveWedgeOnce`'s contract, and the sentence that settles what a retry can do: *"The conditional CREATE is what makes the rule 'every attempt has its own conclusive rejection' affordable: the ref-log key is write-once, so a create either lands our exact bytes (the transaction is durable — and it is the SAME transaction, byte for byte) or conflicts with whatever is there, which the follow-up read then names."*
  - `CasRefLedger.cpp:1726-1734` — `enum class Reason`: **seven** values, `None`, `FenceMoved`, `Superseded`, `WedgeReplaced`, `RefusedPreAttempt`, `ResolveFoundNothing`, `StaleState`.
  - and the arm sites themselves: `:1745-1746`, `:1758-1766`, `:1786-1793` (the pre-I/O decode/apply), `:1815-1830` (the `slotOccupy` try/catch), `:1836-1840` (the occupant adjudication), `:1884-1899`, `:1900-1926`, `:1927-1934`, `:1936-1950`, `:1951-2033`, `:2036-2131` (the reactions), and `:3134-3145` (the `Unresolved` install that CREATES a wedge in the first place).

**Interfaces:**
- Consumes: nothing (first task).
- Produces: the vocabulary every later task references — module name `CaRelinkReofferCore`; the 22 constants listed in step 2; operators `Validator(m, b)`, `HeldValidator`, `GateRefuses(m)`, `SenderAnswer(m)`, `SenderIdentity(m)`, `OfferIdentity(m)`, `RAccepts(ans, idn)`, `BlobOf(m)`, `NsOf(m)`, `AdoptedBlobs`; invariants `TypeOK`, `ConfirmedRelinkNeverDangles`, `PromotedNeverDangles`, `MarkerCoversDurableWindow`, `MarkerSeenMatchesMarker`, `NeverPublishedTwice`, `ChangedImpliesFenced`; witnesses `W_BusyLaneProven`, `W_MidTenureProven`, `W_ProvenCommitted`, `W_BlobDeleted`, `W_CorruptWindow`, `W_RetryCreatedAdopt`.

---

- [ ] **Step 1: Enumerate `resolveWedgeOnce` exhaustively BEFORE writing TLA+, and write the enumeration down.**

Read `CaRelinkConfirmCore.tla` in full, then the sites above. Then produce the table below in a scratch note and check it against the code line by line. **This table becomes the module's mapping block verbatim (step 4) and it is the thing Task 1 step 12 audits.** It is reproduced here because two rounds of review found gaps in it, and the third found that the gaps were in arms nobody had enumerated:

| # | Path through `resolveWedgeOnce` | Site | `WedgeResolution` | Model |
| --- | --- | --- | --- | --- |
| 0 | the `Unresolved` install that CREATES the wedge (this is `commitRefChunk`, not the resolver) | `:3134-3145` | — | `SenderUnresolvedLanded` / `SenderUnresolvedNotLanded`. Marker deliberately STAYS `ApplyPending`. |
| 1 | `!rt->wedge` | `:1745-1746` | `NoWedge` | **NO TRANSITION** — the absence of `sWedge`; the ordinary flush pays nothing. |
| 2 | `durableFloorCovers(wedge->txn_id)` | `:1758-1766` | `FloorReconciled` | `FloorReconcile` — wedge retired, txn NOT installed, `Poisoned` RETAINED (its `clearApplyPending` is a documented no-op on a Poisoned table), apply still owed. |
| 3 | the pre-I/O `decodeRefLogTxn` / `applyRefLogTxn` THROWS (allocation) | `:1786-1793` | — (exception, no result) | **NO TRANSITION.** Its comment: the throw is *"indistinguishable from 'the resolution has not been attempted yet': the lane stays wedged and a later flush retries the whole thing"*. No lock is held, nothing is installed, nothing is unwedged. |
| 4 | `slotOccupy` THROWS | `:1815-1830` | `StillWedged` (no `Reason`) | **NO TRANSITION.** A *definite* refusal of THIS attempt says nothing about the earlier ambiguous one; the lane stays wedged and the id is not consumed. **Distinct from `Reason::RefusedPreAttempt`**, which is an `Unresolved` RESULT, not a throw — the throw path never reaches the recheck block and never sets `reason`. |
| 5 | recheck: `fence_moved` \| `superseded` \| `!same_wedge` | `:1884-1899` | `StillWedged` / `FenceMoved`, `Superseded`, `WedgeReplaced` | **NO TRANSITION** — *"Nothing is installed and nothing is unwedged either way, which is the whole meaning of INERT here."* |
| 6 | `occupied.kind = Unresolved` | `:1900-1908` | `StillWedged` / `RefusedPreAttempt` or `ResolveFoundNothing` | **NO TRANSITION** — the wedge is deliberately kept, the marker is untouched, and there is no deadline reset: *"a permanently quiet wedged namespace waits."* |
| 7 | `occupant = SuccessorSeal` | `:1909-1926` | `Rejected` | `WedgeResolveRejected` — wedge reset, `last_epoch_seal` recorded, marker cleared. Floor deliberately NOT raised. Reachable only when our bytes never landed. |
| 8 | `occupant = Foreign` | `:1927-1934` | `Corrupted` | `WedgeResolveCorrupted`, then `CorruptFenceReaction` as a SEPARATE step (`:2050-2060`). Marker cleared, wedge KEPT. |
| 9 | `getGreatestApplied() # candidate_base_id` | `:1936-1950` | `StillWedged` / `StaleState` | `WedgeResolveStale` — floor raised over the id, poisoned, wedge KEPT *"so the next flush's floor reconciliation retires it"*. |
| 10 | else — ADOPTION, `slotOccupy` returned **`Occupied` with OUR OWN bytes** (the earlier ambiguous attempt landed) | `:1951-2033` | `Adopted` | `WedgeResolveInstall`, from a state reached by `SenderUnresolvedLanded`. |
| 11 | else — ADOPTION, `slotOccupy` returned **`Created`** (this retry's conditional create LANDED, so the wedged txn becomes durable ON THIS ATTEMPT) | `:1836-1840` + `:1951-2033`, contract at `.h:784-801` | `Adopted` | **`WedgeRetryCreated` then `WedgeResolveInstall`** — two steps, because the durability and the install are separated by an off-lock classification a reader can observe. **This is the wedge protocol's happy path and v3 could not represent it at all.** |
| 12 | `Created`, then the adoption is refused because the table advanced | `:1836-1840` + `:1936-1950` | `StillWedged` / `StaleState` | `WedgeRetryCreated` then `WedgeResolveStale` — no new action needed once (11) exists. |

Two things to record while the code is open, because both are conclusions rather than transcription:

**(i) A recorded discrepancy with the review's paraphrase, per the Global Constraint that the code wins.** Round 3's fix instruction said *"a retry that finds DIFFERENT bytes is the stale/poison arm."* The code says otherwise: different bytes at the slot are adjudicated by `classifyRefLogOccupant` into `SuccessorSeal` ⇒ row 7 `Rejected`, or `Foreign` ⇒ row 8 `Corrupted`. The stale/poison arm (row 9) keys on **the TABLE having advanced** (`getGreatestApplied() # candidate_base_id`), not on the occupant's bytes. The model follows the code. Row 12 is what a reader of that sentence probably meant — `Created` followed by a refused adoption — and it exists.

**(ii) Row 9 is an over-approximation, deliberately.** The code's own comment concedes *"One leader per table makes this unreachable today; it is checked, not assumed, because the cost is a comparison and the failure mode is silent data loss."* Nothing in the model advances the table during a resolution either. The model nevertheless admits row 9 as reachable, because the code RETAINS the guard and a safety gate should cover the guards the code retains; admitting a state the code believes unreachable adds states, which is the safe direction. This is stated at `WedgeResolveStale` and in RESULTS, so nobody later reads it as a modelling error.

Then write the five sentences below — they become the module's header, and getting them wrong is how this task goes wrong:

1. `SenderDurable` (`:180-189`) makes a transaction durable while leaving `sCacheRef` un-updated, and the ONLY predicate that refuses in that state is rule 3's `quiescent == SabotageStaleCache \/ (~sPending /\ ~sLeader)` (`:269`). `sPoison` is set only by `SenderPoison` — the apply-THREW case — so **the v11 model has no representation of the code's `ApplyPending` marker at all**, and therefore none of the twelve rows above.
2. The marker is a relaxed atomic written with no lock held, so **what a reader OBSERVES is a different value from what the writer STORED** — and the v11 model, having no marker, has neither.
3. `NsS` is a single fixed sender namespace and `Token` identifies its blobs globally, so **a cross-mount validator collision is unrepresentable.**
4. `SenderAdmit → SenderDurable → SenderApply` is one transaction per tenure and every transaction targets the tracked binding, so **`~sLeader` and "the marker is clear" coincide by construction** and cannot be told apart.
5. `RConfirm` records a single `Gate1Answer`, so **the certified ANSWER and the returned IDENTITY are one object** and §4.2's requirement that both independently agree is unrepresentable.

- [ ] **Step 2: Write the module's header, universe and named assumptions.**

Create `docs/superpowers/models/CaRelinkReofferCore.tla`. The header comment MUST contain, in this order: (a) what is under test — the re-offer confirm of spec `2026-07-29-cas-relink-reoffer-redesign.md` §4.2's five-step fence-first ordering; (b) the five sentences from step 1, stated as what this module adds; (c) the loud `_sab_stalecache` note below; (d) the named-assumptions block below verbatim; (e) the scope notes below verbatim. The twelve-row mapping is a separate block written in step 4, immediately above the wedge actions.

The `_sab_stalecache` note, verbatim — a fresh reader who misses this will misread the entire battery:

```tla
(* *** `SabotageStaleCache = TRUE` IS THE v12 DESIGN, NOT A SABOTAGE. ***
   In `CaRelinkConfirmCore` that flag DROPPED gate 1 rule 3's `!pending.empty() || leader_active`
   terms and MUST produce a counterexample.  This design DELETES those terms (§4.2, §5.1), so here
   the same flag selects the shipped predicate.  The name is carried over deliberately: the whole
   evidence §12.3 asks for is that the SAME-NAMED config flipped verdict once the apply marker was
   represented -- `CaRelinkConfirmCore_sab_stalecache` RED beside
   `CaRelinkReofferCore_sab_stalecache` GREEN.  Renaming it would destroy the comparison. *)
```

The named assumptions, verbatim (§12.2, §12.5 iii, §12's `FreshCertifiedResponse`) — the `ASSUME` is a tripwire, not a proof: deleting an assumption without updating the count fails every config:

```tla
(* ---- THE THREE NAMED ASSUMPTIONS -- ASSUMED, NOT MODELLED ---------------------------------- *)
(* Each is named so that weakening the mechanism that discharges it breaks a NAMED thing rather
   than a silent one.  None of the three is checked here; all three are cited to where they ARE. *)
NamedAssumptions == {
    (* CommittedEdgesAreGcVisible (§12.2): a committed ref edge that is durable before a removal is
       appended is observed by every GC fold that observes the removal.  MODEL FORM: `GFold` folds
       ALL of `Avail` -- there is no `MaxHoles` dial in this module, because §12.1 reassigned
       listing completeness to the v9 chain models.  DISCHARGED BY: `CaRefDeltaIntakeCore_v9_safe`
       and `_v9_hintomission` GREEN plus `CaRefTableSnapshotLogCore_sab_scanistruth` RED
       (`2026-07-28-v9-phase-RESULTS.md`).  The historical refutation of the ASSUMPTION -- one
       incomplete page, once -- stays where it was found: `CaRelinkConfirmCore_sab_holeylist`,
       BACKLOG {#list-as-journal-dataloss-2026-07-25}. *)
    "CommittedEdgesAreGcVisible",
    (* UnresolvedPromoteNeverBytes (§12.5 iii): an unresolved promote never leads to a byte fetch.
       ASSUMED of the CODE.  DISCHARGED BY: relink spec §11 row 9.  Its CONSEQUENCE is modelled --
       `_sab_s2bytefetch` shows what breaks when the assumption is false -- so this module records
       why the assumption matters without pretending to establish it. *)
    "UnresolvedPromoteNeverBytes",
    (* FreshCertifiedResponse (§12): the response the receiver acts on was produced by the sender,
       in reply to THIS request, after T1 -- not replayed, and not an offer response mistaken for a
       confirm.  Request/response GENERATIONS are a transport property and stay out.  PARTIALLY
       MODELLED, and the split is exact: the OFFER-CONFUSION half IS represented -- `OfferIdentity`
       is an ungated resolve carrying NO certified answer, always available as a transport outcome,
       and `_sab_inferanswer` is red on it -- while the REPLAY half (a genuine earlier response,
       bit-for-bit valid) is not, because it needs generations.  DISCHARGED BY: relink spec §11
       row 16 (nonce echo) for the replay half and rows 15(a)/15(b) for the confusion half.  If any
       of those rows is weakened, this assumption goes with it. *)
    "FreshCertifiedResponse" }

ASSUME Cardinality(NamedAssumptions) = 3
```

The scope notes, verbatim:

```tla
(* ---- WHAT THIS MODULE DELIBERATELY DOES NOT MODEL ------------------------------------------ *)
(* From the write-release seam (`2026-07-29-cas-part-write-release-seam.md`):
     * §3's emission at `~PartWriteTxn` and §4's `attempted` transmission mark are ACCOUNTING, not
       safety.  Seam §3.3 states the counter is an upper bound on residue, and seam §9 point 5
       states the contract "never changes what a consumer does on any exit" -- so neither has
       safety content a model can gate.  They are discharged by seam §8 rows S1-S6c and by relink
       rows 19-20, not here.
     * §8 row S7 (marker synchronization) IS the row with model content, and it is carried by TWO
       invariants, not one: `MarkerCoversDurableWindow` for the INTERVAL ("the marker is set for the
       whole durable-but-unapplied window") and `MarkerSeenMatchesMarker` for the OBSERVATION ("a
       reader acquiring after the arm observes the armed value").  What stays code-level is the
       MECHANISM that makes the second true -- `state_mutex` at the call site supplying the
       happens-before (seam §6) -- because no untimed model can check a memory model.  A model can
       check the property the memory model must deliver, and this one does.
   Recovery COMPLETENESS -- that a recovered view is a complete replay of the durable log -- is not
   this answer's contract (§4.2, "Completeness is recovery's contract, not this answer's").  It is
   unrepresentable here by construction: `RecoverForAnswer` installs the durable view ATOMICALLY,
   which is also how §4.3's "abandoning before the install leaves nothing partial" is encoded.
   The wedge-resolution tenure is out of scope as a TENURE question (§12.5); its OUTCOMES are not
   out of scope at all -- every value of `WedgeResolution`, every value of `SlotOccupyResult::Kind`,
   every occupant class, every `Reason` and both exception exits are enumerated in the mapping block
   above the wedge actions, and the ones that map to no transition say so and say why. *)
```

Now the universe. **Unchanged from v2/v3 — do not churn it:**

```tla
-------------------- MODULE CaRelinkReofferCore --------------------
EXTENDS Integers, FiniteSets

CONSTANTS
    Receivers,                     \* relink receivers (one suffices for this safety class)
    MaxId,                         \* bound on the pool-wide ref-transaction id counter
    MaxRound,                      \* bound on the number of GC rounds
    MaxChunks,                     \* durable chunk transactions ONE mount may commit in a behaviour
    SecondMount,                   \* a second same-pool mount: EQUAL root_namespace, DIFFERENT disk
    EqualNamespaces,               \* TRUE = the two mounts share a namespace string (§3's legal case)
    ModelColdTable,                \* TRUE = a mount may be evicted, so rule 2 is on the answer path
    TrackHistory,                  \* TRUE only in _witness_* cfgs: keeps history vars out of greens
    SabotageStaleCache,            \* drop v11 rule 3's pending/leader terms -- TRUE IS THE v12 DESIGN
    SabotageNoApplyPending,        \* the marker is NEVER ARMED (§12.3 step 2, first half)
    SabotageRelaxedMarker,         \* the arm STORES but does not PUBLISH: sApplySeen lags (seam §6)
    SabotageNoPoison,              \* the gate ignores apply_state = Poisoned
    SabotageNoWedge,               \* the gate ignores the wedge
    SabotageNoFence,               \* the gate ignores the mount fence (rule 1, hoisted first)
    SabotageNoRowExact,            \* rule 5 degenerates to "some binding is present"
    SabotageBareValidator,         \* validator = ManifestRef alone (B1's collision shape)
    SabotageNoDiskQual,            \* validator = namespace + ref, disk_name dropped (B1's fix)
    SabotageInferAnswer,           \* the receiver INFERS the answer from a matching identity (§4.1.3)
    SabotageSkipIdentity,          \* the receiver trusts `proven` and skips §4.4 condition 4
    SabotagePublishAfterConfirm,   \* invert the order: confirm+promote BEFORE the durable +1
    SabotageS2ByteFetch,           \* S2 byte-fetches instead of throwing retry-later
    SabotageTypeProbe              \* TypeOK's negative control: write an out-of-domain value

Token      == "m1"       \* the ManifestRef the offer minted; BOTH mounts may bind this same text
Other      == "m2"
NoBinding  == "none"
Absent     == "absent"   \* no identity on the response (§4.2 row 3: no binding => no validator)
Bindings   == {Token, Other, NoBinding}

MountA     == "dA"       \* the mount that made the offer
MountB     == "dB"       \* a second CA disk, SAME pool, SAME server_root_id, DIFFERENT disk_name
Mounts     == IF SecondMount THEN {MountA, MountB} ELSE {MountA}
NsA        == "ns_s"
NsB        == IF EqualNamespaces THEN "ns_s" ELSE "ns_t"
NsOf(m)    == IF m = MountA THEN NsA ELSE NsB

BlobA      == "bA"       \* the blob mount A's manifest `m1` owns -- the one the receiver ADOPTS
BlobB      == "bB"       \* the blob mount B's manifest `m1` owns -- a DIFFERENT object, same text
Blobs      == IF SecondMount THEN {BlobA, BlobB} ELSE {BlobA}
BlobOf(m)  == IF m = MountA THEN BlobA ELSE BlobB
AdoptedBlobs == {BlobA}
EdgeOf(m)  == IF m = MountA THEN "s_dA" ELSE "s_dB"

ChunkKinds == {"tracked", "unrelated"}   \* what the in-flight chunk mutates (§12.5 ii)
ApplyStates == {"clean", "pending", "poisoned"}
Sources    == {"s_dA", "s_dB"} \cup Receivers
Namespaces == {NsA, NsB} \cup Receivers
Ids        == 0..MaxId
Rounds     == 0..MaxRound
Records    == [id: Ids, ns: Namespaces, blob: Blobs, src: Sources, op: {"add", "del"}]
```

- [ ] **Step 3: Write the variables and `Init`.**

**Unchanged from v3.** `sFloorCovers` — the model's `RefTableState::durableFloorCovers(wedge->txn_id)` — is what makes the two-step stale resolution representable and the poisoned-to-clean install unrepresentable.

```tla
VARIABLES
    round, present, condemned, pendingDelete, folded, cursor, gcPhase,   \* GC
    journal, nextId,                                                     \* the durable ref log
    sDurableRef,      \* GROUND TRUTH: the namespace's durable binding, foreign writes included
    sCacheRef,        \* the mount's in-memory committed row -- what the answer reads
    sTarget, sKind, sPending, sLeader, sArmed,
    sApply,           \* the marker as STORED
    sApplySeen,       \* the marker as a `state_mutex` reader OBSERVES it (seam §6)
    sApplyOwed,       \* GHOST: this mount has an own DURABLE transaction not yet installed
    sFloorCovers,     \* the durable floor records the wedged txn as durable-but-not-applied
    sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks,
    partMount,                                                           \* which mount holds the part
    rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
    rAnsweredFenced, rPublishes, rUnresolved,
    sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
    sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow,
    sawRetryCreatedAdopt

gcVars     == << round, present, condemned, pendingDelete, folded, cursor, gcPhase >>
senderVars == << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                 sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sRecovered,
                 sChunks, sTenureChunks >>
recvVars   == << rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
                 rAnsweredFenced, rPublishes, rUnresolved >>
logVars    == << journal, nextId >>
histVars   == << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                 sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision,
                 sawCorruptWindow, sawRetryCreatedAdopt >>
vars       == << gcVars, senderVars, recvVars, logVars, histVars, partMount >>

Max(S)   == CHOOSE x \in S : \A y \in S : y <= x
Indeg(b) == Cardinality({ e \in folded : e.b = b })
H(cond)  == TrackHistory /\ cond    \* history is recorded ONLY in the witness cfgs

Init ==
    /\ round = 0
    /\ present = [b \in Blobs |-> TRUE]
    /\ condemned = {}
    /\ pendingDelete = {}
    (* History: each mount's binding of Token over ITS OWN blob is committed and already folded. *)
    /\ folded = { [b |-> BlobOf(m), src |-> EdgeOf(m)] : m \in Mounts }
    /\ cursor = [ns \in Namespaces |-> 0]
    /\ gcPhase = "idle"
    /\ journal = {}
    /\ nextId = 1
    /\ sDurableRef = [m \in Mounts |-> Token]
    /\ sCacheRef   = [m \in Mounts |-> Token]
    /\ sTarget     = [m \in Mounts |-> Token]
    /\ sKind       = [m \in Mounts |-> "tracked"]
    /\ sPending    = [m \in Mounts |-> FALSE]
    /\ sLeader     = [m \in Mounts |-> FALSE]
    /\ sArmed      = [m \in Mounts |-> FALSE]
    /\ sApply      = [m \in Mounts |-> "clean"]
    /\ sApplySeen  = [m \in Mounts |-> "clean"]
    /\ sApplyOwed  = [m \in Mounts |-> FALSE]
    /\ sFloorCovers = [m \in Mounts |-> FALSE]
    /\ sWedge      = [m \in Mounts |-> FALSE]
    /\ sForeign    = [m \in Mounts |-> FALSE]
    /\ sFence      = [m \in Mounts |-> TRUE]
    /\ sRecovered  = [m \in Mounts |-> TRUE]
    /\ sChunks       = [m \in Mounts |-> 0]
    /\ sTenureChunks = [m \in Mounts |-> 0]
    /\ partMount = MountA
    /\ rState  = [r \in Receivers |-> "init"]
    /\ rAnswer = [r \in Receivers |-> "none"]
    /\ rIdentity = [r \in Receivers |-> Absent]
    /\ rAccepted  = [r \in Receivers |-> FALSE]
    /\ rCommitted = [r \in Receivers |-> FALSE]
    /\ rDurableBefore  = [r \in Receivers |-> FALSE]
    /\ rAnsweredFenced = [r \in Receivers |-> FALSE]
    /\ rPublishes  = [r \in Receivers |-> 0]
    /\ rUnresolved = [r \in Receivers |-> FALSE]
    /\ sawBusyProven = FALSE /\ sawMidTenureProven = FALSE /\ sawProvenCommitted = FALSE
    /\ sawLandedS2 = FALSE /\ sawChangedThenBytes = FALSE /\ sawUnknown = FALSE
    /\ sawColdRefused = FALSE /\ sawCollision = FALSE /\ sawCorruptWindow = FALSE
    /\ sawRetryCreatedAdopt = FALSE
```

- [ ] **Step 4: Write the sender lane — chunk kinds, the marker's two values, and the FULL wedge lifecycle including the retry that CREATES.**

Every action carries the code citation that licenses it. The non-wedge actions (`SenderAdmit` through `SenderPoison`) are **unchanged from v3** and hand-verified by review; do not churn them.

```tla
(* ---- the sender's ref lane: ONE tenure, SEVERAL durable chunks of TWO kinds ------------------ *)

(* Admission.  `pending` and `leader_active` are exactly the two predicates v11 rule 3 read under
   `ref_queue_mutex` -- and exactly the two this design deletes.  The tenure OPENS here and does not
   close per chunk.  TWO KINDS, and the second is why a multi-chunk tenure is reachable at all: a
   "tracked" chunk mutates the binding under test and therefore requires it to still exist, while an
   "unrelated" chunk mutates some OTHER ref of the same namespace and requires nothing -- which is
   the ordinary case of a busy writer's lane (§2) and the case §12.5 ii asks the model to express. *)
SenderAdmit(m, nb, k) ==
    /\ sFence[m] /\ sRecovered[m]
    /\ ~sPending[m] /\ ~sWedge[m] /\ sApply[m] = "clean"
    /\ (k = "tracked") => (sDurableRef[m] = Token /\ nb # Token)
    /\ sChunks[m] < MaxChunks
    /\ nextId <= MaxId
    /\ sPending' = [sPending EXCEPT ![m] = TRUE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = TRUE]
    /\ sTarget'  = [sTarget  EXCEPT ![m] = IF k = "tracked" THEN nb ELSE sTarget[m]]
    /\ sKind'    = [sKind    EXCEPT ![m] = k]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = IF sLeader[m] THEN sTenureChunks[m] ELSE 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sArmed, sApply, sApplySeen, sApplyOwed, sFloorCovers,
                    sWedge, sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* THE ARM.  Seam §6: `armApplyPending` inside a `state_mutex` scope IMMEDIATELY before the PUT --
   "the last statement that still runs while nothing of this transaction can possibly be durable"
   (`CasRefLedger.cpp:2808`).  `sArmed` makes the ORDERING structural.
   TWO SABOTAGES, and they are different failures:
     * SabotageNoApplyPending -- the marker is not there at all: BOTH values stay clean.
     * SabotageRelaxedMarker  -- the marker IS stored but the store is not PUBLISHED to a reader
       taking `state_mutex` afterwards, which is exactly what the relaxed
       `compare_exchange_strong` at `:1681`, called at `:2808` with no lock held, permits.  The
       STORED value moves; the SEEN value lags.  These are not the same state machine, which is why
       both configs exist. *)
SenderArm(m) ==
    /\ sPending[m] /\ ~sArmed[m] /\ sApply[m] = "clean"
    /\ sArmed' = [sArmed EXCEPT ![m] = TRUE]
    /\ sApply' = [sApply EXCEPT ![m] = IF SabotageNoApplyPending THEN "clean" ELSE "pending"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] =
           IF SabotageNoApplyPending    THEN "clean"
           ELSE IF SabotageRelaxedMarker THEN sApplySeen[m]   \* the store is not published
           ELSE                              "pending"]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The conditional PUT is acked: DURABLE, GC can fold it, the committed row still lags.  NO durable
   byte without a prior arm -- that guard is the ordering the whole design rests on.  A "tracked"
   chunk moves the binding and appends the removal GC will fold; an "unrelated" chunk is durable and
   owes an apply without touching the tracked binding or the blob universe. *)
SenderDurable(m) ==
    /\ sPending[m] /\ sArmed[m] /\ ~sWedge[m] /\ ~sApplyOwed[m]
    /\ nextId <= MaxId
    /\ IF sKind[m] = "tracked"
         THEN /\ sDurableRef[m] = Token
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m),
                      src |-> EdgeOf(m), op |-> "del"] }
              /\ nextId' = nextId + 1
              /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
         ELSE /\ UNCHANGED logVars
              /\ UNCHANGED sDurableRef
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = TRUE]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply, sApplySeen,
                    sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* THE INSTALL.  `clearApplyPending` is the last statement of the install region (`:2992`), in the
   same allocation-free scope as `rt->state.swap(*candidate)`: "'recorded' and 'no apply owed'
   become true together or not at all".  The TENURE SURVIVES -- `sLeader` untouched -- so the next
   chunk of the same tenure may open while the marker is clean and the view is complete.  That is
   the state v11 refused and v12 answers from. *)
SenderInstall(m) ==
    /\ sArmed[m] /\ sApplyOwed[m] /\ ~sWedge[m]
    /\ sCacheRef' = [sCacheRef EXCEPT ![m] =
           IF sKind[m] = "tracked" THEN sDurableRef[m] ELSE sCacheRef[m]]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = FALSE]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ sPending'   = [sPending   EXCEPT ![m] = FALSE]
    /\ sChunks'       = [sChunks       EXCEPT ![m] = sChunks[m] + 1]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = sTenureChunks[m] + 1]
    /\ UNCHANGED << sDurableRef, sTarget, sKind, sLeader, sFloorCovers, sWedge, sForeign,
                    sFence, sRecovered >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

SenderCloseTenure(m) ==
    /\ sLeader[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sLeader' = [sLeader EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sArmed, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* The in-memory apply THREW although the object is durable.  The tenure closes -- the lane looks
   perfectly quiescent -- and only the Poisoned arm of rule 4 can see it. *)
SenderPoison(m) ==
    /\ sArmed[m] /\ sApplyOwed[m] /\ ~sWedge[m]
    /\ sApply'     = [sApply     EXCEPT ![m] = "poisoned"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "poisoned"]
    /\ sArmed'   = [sArmed   EXCEPT ![m] = FALSE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sApplyOwed, sFloorCovers, sWedge,
                    sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ============================================================================================ *)
(* ---- THE WEDGE LIFECYCLE: EVERY arm of `CasRefLedger::resolveWedgeOnce`, enumerated ---------- *)
(* ============================================================================================ *)
(* Bounded by three declarations, so this list is exhaustive by construction rather than by
   inspection: `WedgeResolution` has SIX values (`CasRefLedger.h:759-771`),
   `SlotOccupyResult::Kind` has THREE (`Backend/CasRequestControl.h:399`), the occupant adjudication
   has FOUR classes (`NotOccupied | Mine | SuccessorSeal | Foreign`), `Reason` has SEVEN values
   (`:1726-1734`), and there are TWO exception exits.  Twelve paths result.  Rows that map to NO
   transition say so and say why -- an inert outcome is `UNCHANGED vars`, which `NoOp` supplies.

  #  PATH                                        SITE            MODEL
  -- ------------------------------------------- --------------- --------------------------------
  0  the Unresolved install that CREATES the     :3134-3145      SenderUnresolvedLanded /
     wedge (commitRefChunk, not the resolver)                    SenderUnresolvedNotLanded.
     -- the marker deliberately STAYS pending                    Both leave sApply = "pending".
  1  !rt->wedge                       NoWedge    :1745-1746      NO TRANSITION -- the absence of
                                                                 sWedge; the ordinary flush.
  2  durableFloorCovers(txn_id)                  :1758-1766      FloorReconcile -- wedge RETIRED,
              FloorReconciled                                    txn NOT installed, Poisoned and
                                                                 sApplyOwed both RETAINED.
  3  pre-I/O decode/apply THROWS      (no kind)  :1786-1793      NO TRANSITION -- "indistinguishable
                                                                 from 'the resolution has not been
                                                                 attempted yet'"; no lock held.
  4  slotOccupy THROWS                StillWedged :1815-1830     NO TRANSITION -- a DEFINITE refusal
                                                                 of THIS attempt says nothing about
                                                                 the earlier ambiguous one.  NOT
                                                                 Reason::RefusedPreAttempt: this
                                                                 path never reaches the recheck and
                                                                 never sets `reason` at all.
  5  fence_moved | superseded |       StillWedged :1884-1899     NO TRANSITION -- "Nothing is
     !same_wedge  (FenceMoved,                                   installed and nothing is unwedged
     Superseded, WedgeReplaced)                                  either way ... the whole meaning
                                                                 of INERT here."
  6  kind = Unresolved                StillWedged :1900-1908     NO TRANSITION -- wedge deliberately
     (RefusedPreAttempt |                                        kept, marker untouched, no deadline
      ResolveFoundNothing)                                       reset: "a permanently quiet wedged
                                                                 namespace waits."
  7  occupant = SuccessorSeal         Rejected    :1909-1926     WedgeResolveRejected -- wedge reset,
                                                                 marker cleared, floor deliberately
                                                                 NOT raised.
  8  occupant = Foreign               Corrupted   :1927-1934     WedgeResolveCorrupted, then
                                                  + :2050-2060   CorruptFenceReaction as a SEPARATE
                                                                 step.  Marker cleared, wedge KEPT.
  9  getGreatestApplied() #           StillWedged :1936-1950     WedgeResolveStale -- floor raised,
     candidate_base_id (StaleState)                              poisoned, wedge KEPT.  See the
                                                                 OVER-APPROXIMATION note below.
 10  ADOPTION, kind = Occupied with   Adopted     :1951-2033     WedgeResolveInstall, from the state
     OUR OWN bytes (the earlier                                  SenderUnresolvedLanded reaches.
     ambiguous attempt landed)
 11  ADOPTION, kind = Created -- THIS Adopted     :1836-1840     WedgeRetryCreated, THEN
     retry's conditional create                    + :1951-2033  WedgeResolveInstall.  TWO steps:
     LANDED, so the wedged txn                     contract at   the create is durable while the
     becomes durable ON THIS ATTEMPT               .h:784-801    occupant classification still runs
     -- THE WEDGE PROTOCOL'S HAPPY PATH                          OFF the lock, so a reader CAN take
                                                                 state_mutex in between.  Modelling
                                                                 it atomically would hide that
                                                                 durable-but-unapplied window.
 12  Created, then adoption refused   StillWedged :1836-1840     WedgeRetryCreated, THEN
     because the table advanced                    + :1936-1950  WedgeResolveStale.  No new action.

  A RECORDED DISCREPANCY, per the house rule that the code wins: a review round paraphrased row 12
  as "a retry that finds DIFFERENT bytes is the stale/poison arm".  It is not.  DIFFERENT bytes are
  adjudicated by `classifyRefLogOccupant` into row 7 (`SuccessorSeal` => Rejected) or row 8
  (`Foreign` => Corrupted).  The stale arm keys on the TABLE having advanced, not on the occupant.

  AN OVER-APPROXIMATION, stated so nobody reads it as an error: row 9's own comment concedes "One
  leader per table makes this unreachable today; it is checked, not assumed, because the cost is a
  comparison and the failure mode is silent data loss."  Nothing in this model advances the table
  during a resolution either.  The model admits row 9 as REACHABLE anyway, because the code RETAINS
  the guard and a safety gate should cover the guards the code retains.  Admitting a state the code
  believes unreachable adds states, which is the safe direction. *)

(* UNRESOLVED (`:3134-3145`).  The PUT outcome is unknown, the wedge is installed, and *** the
   MARKER STAYS `ApplyPending` ***: its own comment says an `Unresolved` outcome is "precisely 'an
   object that may be durable and is not applied'", cleared only when the resolution installs the
   transaction or proves it never landed.  The ledger's state is IDENTICAL in both arms below -- only
   the ground truth differs, which is the whole point of a wedge.  Both arms cover BOTH chunk kinds:
   an unrelated chunk wedges exactly as a tracked one does, and an asymmetry here would leave the
   retry path (row 11) exercised for only half the item classes. *)
SenderUnresolvedLanded(m) ==
    /\ sPending[m] /\ sArmed[m] /\ ~sWedge[m] /\ ~sApplyOwed[m]
    /\ nextId <= MaxId
    /\ IF sKind[m] = "tracked"
         THEN /\ sDurableRef[m] = Token
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m),
                      src |-> EdgeOf(m), op |-> "del"] }
              /\ nextId' = nextId + 1
              /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
         ELSE /\ UNCHANGED logVars
              /\ UNCHANGED sDurableRef
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = TRUE]
    /\ sWedge'   = [sWedge   EXCEPT ![m] = TRUE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sArmed, sApply, sApplySeen, sFloorCovers, sForeign,
                    sFence, sRecovered, sChunks >>          \* sApply STAYS "pending" -- by contract
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

SenderUnresolvedNotLanded(m) ==
    /\ sPending[m] /\ sArmed[m] /\ ~sWedge[m] /\ ~sApplyOwed[m]
    /\ sWedge'   = [sWedge   EXCEPT ![m] = TRUE]
    /\ sPending' = [sPending EXCEPT ![m] = FALSE]
    /\ sLeader'  = [sLeader  EXCEPT ![m] = FALSE]
    /\ sTenureChunks' = [sTenureChunks EXCEPT ![m] = 0]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sArmed, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sForeign, sFence, sRecovered, sChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* *** ROW 11, STEP ONE -- THE BOUNDED RETRY THAT CREATES.  THE ARM v3 COULD NOT EXPRESS. ***
   `resolveWedgeOnce` issues ONE `slotOccupy(wedge.key, wedge.bytes, ...)` per calling flush, and its
   contract (`CasRefLedger.h:784-801`) is explicit about what a CREATE means: "the ref-log key is
   write-once, so a create either lands our exact bytes (the transaction is durable -- and it is the
   SAME transaction, byte for byte) or conflicts with whatever is there".  So the retry is itself a
   DURABILITY-PRODUCING event: from a wedge whose earlier attempt did NOT land, this attempt makes
   the wedged transaction durable, and only THEN may the resolution adopt it (`WedgeResolveInstall`)
   or refuse the adoption (`WedgeResolveStale`).
   IT IS A SEPARATE STEP, not folded into the install, and the reason is observable: the occupant
   classification runs OFF the lock ("Classify the occupant OFF the lock: pure, and the decode
   allocates", `:1832-1836`), so between the create landing durably and the install re-acquiring
   `state_mutex` a reader CAN take the lock and see a wedged, marker-pending, stale-cache table whose
   transaction is already durable.  Rules 3 and 4 both refuse there -- which is the point -- but a
   model that made this atomic would hide the window instead of covering it.
   WHY THE GUARDS: `~sApplyOwed` is "our earlier attempt did not land" (a landed one leaves the slot
   Occupied-with-mine, which is row 10, not row 11); `~sForeign` because a Corrupted lane is fenced
   closed and left wedged for inspection, with no further attempt that could succeed; `~sFloorCovers`
   because a floor-covered wedge takes row 2 before any attempt is issued. *)
WedgeRetryCreated(m) ==
    /\ sWedge[m] /\ ~sApplyOwed[m] /\ ~sForeign[m] /\ ~sFloorCovers[m]
    /\ sApply[m] = "pending"
    /\ nextId <= MaxId
    /\ IF sKind[m] = "tracked"
         THEN /\ sDurableRef[m] = Token
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m),
                      src |-> EdgeOf(m), op |-> "del"] }
              /\ nextId' = nextId + 1
              /\ sDurableRef' = [sDurableRef EXCEPT ![m] = sTarget[m]]
         ELSE /\ UNCHANGED logVars
              /\ UNCHANGED sDurableRef
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = TRUE]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply, sApplySeen,
                    sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* ROWS 10 AND 11, STEP TWO -- ADOPTION (`:1951-2033`): durable, and the table has not advanced under
   the resolution, so it can be recorded.  Reached from row 10 (`SenderUnresolvedLanded`) and from
   row 11 (`WedgeRetryCreated`); the code cannot tell the two apart at this point and neither does
   this action, which is why one action serves both.
   *** TWO GUARDS, AND BOTH ARE STRUCTURAL EXCLUSIONS, NOT DECORATION. ***
     * `~sFloorCovers[m]` -- `durableFloorCovers` is tested FIRST in the real function (`:1758`),
       before a candidate is even built, so a floor-covered wedge can only take row 2.  An install
       here would re-apply an id that now sits AT the floor, which `applyTxnInPlace` refuses as
       non-contiguous -- "every later flush would throw in the same place and the lane would stay
       wedged for the runtime's life".
     * `sApply[m] # "poisoned"` -- a poisoned cache is by definition missing a durable transaction
       and may never be silently advanced to clean.  An earlier draft of this model guarded this
       action on `sWedge /\ sApplyOwed` ALONE, which left a poisoned-to-clean install enabled
       immediately after `WedgeResolveStale`: an impossible transition presented as a reachable one.
   Neither guard has an invariant of its own, deliberately: they make the states UNREPRESENTABLE, and
   an invariant over a state the transition relation cannot enter would be green for free.  They are
   recorded as structural exclusions in `2026-07-29-relink-seam-tla-RESULTS.md`. *)
WedgeResolveInstall(m) ==
    /\ sWedge[m] /\ sApplyOwed[m]
    /\ ~sFloorCovers[m]
    /\ sApply[m] # "poisoned"
    /\ sCacheRef'  = [sCacheRef  EXCEPT ![m] =
           IF sKind[m] = "tracked" THEN sDurableRef[m] ELSE sCacheRef[m]]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = FALSE]
    /\ sWedge'     = [sWedge     EXCEPT ![m] = FALSE]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ sChunks'    = [sChunks    EXCEPT ![m] = sChunks[m] + 1]
    /\ sawRetryCreatedAdopt' = sawRetryCreatedAdopt \/ H(TRUE)
    /\ UNCHANGED << sDurableRef, sTarget, sKind, sPending, sLeader, sFloorCovers, sForeign,
                    sFence, sRecovered, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                    sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision,
                    sawCorruptWindow >>

(* ROW 7 -- REJECTED (`:1909-1926`): our own epoch seal occupies the slot, so our bytes PROVABLY
   never landed.  "no apply is owed" -- the wedge resets and the marker clears together.  Reached
   only when the floor does NOT cover the id, because that test comes first. *)
WedgeResolveRejected(m) ==
    /\ sWedge[m] /\ ~sApplyOwed[m] /\ ~sForeign[m] /\ ~sFloorCovers[m]
    /\ sWedge'     = [sWedge     EXCEPT ![m] = FALSE]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sFloorCovers, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ROW 8 -- CORRUPTED (`:1927-1934`) -- *** THE ONE STATE WHERE A WEDGED LANE IS NOT ApplyPending. ***
   A FOREIGN object at a key that mount-lease exclusivity says is exclusively ours.  The code clears
   the marker ("our bytes provably never landed, so no apply is owed") and deliberately KEEPS the
   wedge for inspection.  Two consequences the model must carry, and both matter:
     * exclusivity is BREACHED, so this namespace has another writer whose durable removals this
       mount's committed row cannot see -- `sForeign` is what enables `ForeignRemove` below;
     * the fence-closing reaction is a SEPARATE, later step (`:2050-2060`), so there is a window in
       which the lane is fence-live, marker-clean and wedged.  Rule 3 is the ONLY guard in it.
   This window is `_sab_nowedge`'s counterexample and `_witness_corruptwindow`'s subject. *)
WedgeResolveCorrupted(m) ==
    /\ sWedge[m] /\ ~sApplyOwed[m] /\ ~sForeign[m] /\ ~sFloorCovers[m]
    /\ sForeign'   = [sForeign   EXCEPT ![m] = TRUE]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sArmed'     = [sArmed     EXCEPT ![m] = FALSE]
    /\ sawCorruptWindow' = sawCorruptWindow \/ H(sFence[m])
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sFloorCovers, sWedge, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, partMount >>       \* wedge KEPT
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                    sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision,
                    sawRetryCreatedAdopt >>

(* The reaction (`:2050-2060`): the mount is fenced closed and a remount is scheduled. *)
CorruptFenceReaction(m) ==
    /\ sForeign[m] /\ sFence[m]
    /\ sFence' = [sFence EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                    sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sRecovered,
                    sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ROW 9 -- STALE STATE (`:1936-1950`), STEP ONE OF TWO.  The object is proven durable and this
   runtime cannot record it, "which is exactly what the floor and `Poisoned` are for":
   `noteDurableIdNotApplied` raises the durable floor over the wedged id, `poisonApplyState` poisons
   the table, and *** the WEDGE IS KEPT *** -- "so the next flush's floor reconciliation retires it
   instead of re-applying an id that now sits at the floor".  `sApplyOwed` stays TRUE, which is the
   truth: the transaction is durable and this cache does not contain it.  Reached from row 10 AND
   from row 11 (a retry that created, then could not be adopted -- row 12).  OVER-APPROXIMATED
   deliberately: see the mapping block. *)
WedgeResolveStale(m) ==
    /\ sWedge[m] /\ sApplyOwed[m] /\ ~sFloorCovers[m]
    /\ sFloorCovers' = [sFloorCovers EXCEPT ![m] = TRUE]
    /\ sApply'       = [sApply       EXCEPT ![m] = "poisoned"]
    /\ sApplySeen'   = [sApplySeen   EXCEPT ![m] = "poisoned"]
    /\ sArmed'       = [sArmed       EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApplyOwed,
                    sWedge, sForeign, sFence, sRecovered, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ROW 2 -- FLOOR RECONCILIATION (`:1758-1766`), STEP TWO OF TWO.  The NEXT invocation sees
   `durableFloorCovers(wedge->txn_id)` and takes this path before any candidate is built: the wedge
   is RETIRED and the transaction is NOT installed.  Three things stay exactly as they were, and each
   is a code fact rather than a modelling convenience:
     * the cached row is NOT advanced -- "nothing is installed on this path";
     * `Poisoned` is RETAINED -- the `clearApplyPending` call there is documented as "A no-op on a
       `Poisoned` table (the transition is terminal and this is a CAS)";
     * `sApplyOwed` is RETAINED -- "The table stays POISONED: its cached view is missing that
       transaction until a recovery re-derives it from the log."
   So `MarkerCoversDurableWindow` holds through both steps without any exclusion, and the lane is
   stopped (every admission requires a clean marker) until a recovery re-derives the cache. *)
FloorReconcile(m) ==
    /\ sWedge[m] /\ sFloorCovers[m]
    /\ sWedge' = [sWedge EXCEPT ![m] = FALSE]
    /\ sArmed' = [sArmed EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sForeign, sFence, sRecovered, sChunks,
                    sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* ---- fence loss, the foreign writer, and rule 2's two arms ----------------------------------- *)

FenceLoss(m) ==
    /\ sFence[m] /\ ~sPending[m] /\ ~sArmed[m]
    /\ sFence' = [sFence EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                    sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sRecovered,
                    sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* Another writer removes the binding: durable, folded by GC, invisible to this mount's committed
   row.  Enabled by EITHER of the two ways exclusivity can fail -- a lost fence (this instance was
   deposed) or a proven foreign occupant (exclusivity was breached while the fence still reads
   live).  It never touches `sApplyOwed`, because it is not this mount's transaction -- which is
   precisely why `MarkerCoversDurableWindow` needs no exclusion clause. *)
ForeignRemove(m) ==
    /\ (~sFence[m] \/ sForeign[m])
    /\ sDurableRef[m] = Token /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsOf(m), blob |-> BlobOf(m), src |-> EdgeOf(m), op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = [sDurableRef EXCEPT ![m] = NoBinding]
    /\ UNCHANGED << sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply, sApplySeen,
                    sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sRecovered, sChunks,
                    sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, histVars, partMount >>

(* Eviction requires no IN-FLIGHT transaction, NOT a clean marker: a poisoned cache is exactly the
   one you want evicted, and eviction plus recovery is how the code's "until a recovery re-derives
   it from the log" is reached.  Without this the poisoned/floor-reconciled lane would be a dead end
   whenever `ModelColdTable = FALSE`; with `ModelColdTable = FALSE` it still is, and that is
   accepted -- a dead end is a state, not an error, and `NoOp` keeps TLC from reporting a
   deadlock. *)
EvictTable(m) ==
    /\ ModelColdTable /\ sRecovered[m]
    /\ ~sPending[m] /\ ~sArmed[m] /\ ~sLeader[m] /\ ~sWedge[m] /\ sApply[m] # "pending"
    /\ sRecovered' = [sRecovered EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sKind, sPending, sLeader, sArmed, sApply,
                    sApplySeen, sApplyOwed, sFloorCovers, sWedge, sForeign, sFence, sChunks,
                    sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>

(* §4.3: recovery installs its result ATOMICALLY, so abandoning before the install leaves nothing
   partial and a half-recovered view is unrepresentable.  It is also the ONLY exit from the poisoned
   state, and it clears the marker, the owed apply and the floor record together -- "a recovery
   re-derives the cache from the log". *)
RecoverForAnswer(m) ==
    /\ ~sRecovered[m] /\ ~sPending[m] /\ ~sArmed[m] /\ ~sWedge[m]
    /\ sRecovered' = [sRecovered EXCEPT ![m] = TRUE]
    /\ sCacheRef'  = [sCacheRef  EXCEPT ![m] = sDurableRef[m]]
    /\ sApply'     = [sApply     EXCEPT ![m] = "clean"]
    /\ sApplySeen' = [sApplySeen EXCEPT ![m] = "clean"]
    /\ sApplyOwed' = [sApplyOwed EXCEPT ![m] = FALSE]
    /\ sFloorCovers' = [sFloorCovers EXCEPT ![m] = FALSE]
    /\ UNCHANGED << sDurableRef, sTarget, sKind, sPending, sLeader, sArmed, sWedge, sForeign,
                    sFence, sChunks, sTenureChunks >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars, partMount >>
```

- [ ] **Step 5: Write the validator, and the answer as an (answer, identity) PAIR.**

**Unchanged from v2/v3 — review verified every cited symbol here; do not churn it.**

```tla
(* ---- the validator ---------------------------------------------------------------------------- *)

(* The digest of §4.1 is modelled as the TUPLE it hashes, and record equality is digest equality.
   That is exact under §4.1's stated adversary model -- both endpoints are inside the interserver
   authentication boundary, so the threat is ACCIDENTAL collision at 128 bits, not forgery.
   `pool_uuid` and `ref_name` are omitted because they are constant across both mounts here (one
   pool, one table, one ref); they discriminate nothing this model can vary.  The sabotages remove
   fields from BOTH sides, because both sides run the same code -- an implementation that "forgot
   disk_name" forgets it symmetrically, and that symmetry is what makes the collision possible. *)
Validator(m, b) ==
    IF SabotageBareValidator   THEN [ref |-> b]
    ELSE IF SabotageNoDiskQual THEN [ns |-> NsOf(m), ref |-> b]
    ELSE                            [ns |-> NsOf(m), disk |-> m, ref |-> b]

HeldValidator == Validator(MountA, Token)   \* what the receiver adopted from the offer, at T0

(* ---- §4.2's fence-first ordering, and the TWO INDEPENDENT response fields -------------------- *)

(* Rule 1 (fence) is FIRST, which is the reorder.  Rule 4 reads the marker a `state_mutex` reader
   OBSERVES -- `sApplySeen`, not `sApply` -- which is what makes seam §6 a property this model can
   check rather than a comment. *)
GateRefuses(m) ==
    \/ ~(SabotageNoFence \/ sFence[m])                              \* 1. mount fence -- HOISTED
    \/ ~sRecovered[m]                                               \* 2. residency and recovery
    \/ ~(SabotageNoWedge \/ ~sWedge[m])                             \* 3. wedge
    \/ ~(sApplySeen[m] = "clean"                                    \* 4. apply state, AS OBSERVED
         \/ (SabotageNoPoison /\ sApplySeen[m] = "poisoned"))
    \/ ~(SabotageStaleCache \/ (~sPending[m] /\ ~sLeader[m]))       \* v11 rule 3 -- TRUE = v12

RowPresent(m) == sCacheRef[m] # NoBinding

(* THE TWO FIELDS ARE COMPUTED SEPARATELY, because §4.2 requires them to agree INDEPENDENTLY:
   "`proven` with a non-matching identity is a contradiction, not a `Yes` ... Each is separately
   sufficient to fail closed."  A model that derived one from the other could not express that. *)
SenderIdentity(m) == IF GateRefuses(m) \/ ~RowPresent(m) THEN Absent
                                                         ELSE Validator(m, sCacheRef[m])

SenderAnswer(m) ==
    IF GateRefuses(m) \/ ~RowPresent(m) THEN "unknown"
    ELSE IF SabotageNoRowExact THEN "proven"
    ELSE IF Validator(m, sCacheRef[m]) = HeldValidator THEN "proven"
    ELSE "changed"

(* §4.1.3: an OFFER response is an UNGATED resolve that carries NO certified answer at all.  It is
   always available as a transport outcome, because a stripped mode parameter is something the wire
   genuinely permits (§11 row 15) -- and its identity may well EQUAL the held validator, which is
   exactly why the explicit answer cookie exists. *)
OfferIdentity(m) == IF RowPresent(m) THEN Validator(m, sCacheRef[m]) ELSE Absent

(* §4.4 conditions 3 and 4.  Conditions 1 and 2 -- empty body, nonce echo -- are
   `FreshCertifiedResponse`'s territory and are not modelled. *)
RAccepts(ans, idn) ==
    IF SabotageInferAnswer        THEN idn = HeldValidator         \* infer the answer: §4.1.3 (1)
    ELSE IF SabotageSkipIdentity  THEN ans = "proven"              \* skip condition 4
    ELSE                               ans = "proven" /\ idn = HeldValidator
```

- [ ] **Step 6: Write the receiver, GC, `Next`, and Task 1's invariants and witnesses.**

The receiver, GC and invariant blocks are **unchanged from v3** apart from `sawRetryCreatedAdopt` in `TypeOK` and `WedgeRetryCreated` in `Next`.

```tla
(* ---- the receiver (§6; S0 and S2 arrive in Task 3) ----------------------------------------- *)

RStage(r) ==
    /\ \/ rState[r] = "init"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "S3")
    /\ ~(\E rec \in journal : rec.ns = r /\ rec.op = "add")
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "add"] }
    /\ nextId' = nextId + 1
    /\ rState' = [rState EXCEPT ![r] = IF rState[r] = "init" THEN "S1" ELSE "S3"]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* T2: the re-offer, answered by whichever mount holds the part NOW (§3: the request carries `part`
   and `endpoint`, so the sender routes it exactly as the offer did).  TWO response shapes: the
   gated CONFIRM pair, and the ungated OFFER (§4.1.3's stripped-mode outcome, which carries no
   certified answer).  The receiver's acceptance is `RAccepts` over BOTH fields. *)
RConfirmResponse(r, ans, idn) ==
    /\ rAnswer'   = [rAnswer   EXCEPT ![r] = ans]
    /\ rIdentity' = [rIdentity EXCEPT ![r] = idn]
    /\ rAccepted' = [rAccepted EXCEPT ![r] = RAccepts(ans, idn)]
    /\ rAnsweredFenced' = [rAnsweredFenced EXCEPT ![r] = sFence[partMount]]
    /\ rDurableBefore'  = [rDurableBefore  EXCEPT ![r] = (rState[r] = "S1")]
    /\ rState' = [rState EXCEPT ![r] = "answered"]
    /\ UNCHANGED << rCommitted, rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>

RConfirm(r) ==
    /\ \/ rState[r] = "S1"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "init")
    /\ LET m == partMount IN
       \/ (* the CONFIRM response: the gated pair *)
          /\ RConfirmResponse(r, SenderAnswer(m), SenderIdentity(m))
          /\ sawBusyProven' = sawBusyProven \/
               H(SenderAnswer(m) = "proven" /\ (sPending[m] \/ sLeader[m]))
          /\ sawMidTenureProven' = sawMidTenureProven \/
               H(SenderAnswer(m) = "proven" /\ sLeader[m] /\ sTenureChunks[m] >= 1)
          /\ sawUnknown' = sawUnknown \/ H(SenderAnswer(m) = "unknown" /\ GateRefuses(m))
          /\ sawColdRefused' = sawColdRefused \/ H(SenderAnswer(m) = "unknown" /\ ~sRecovered[m])
          /\ sawCollision' = sawCollision \/
               H(SenderAnswer(m) = "proven" /\ m # MountA /\ sCacheRef[m] = Token
                 /\ NsOf(m) = NsOf(MountA))
          /\ UNCHANGED << sawProvenCommitted, sawLandedS2, sawChangedThenBytes, sawCorruptWindow,
                          sawRetryCreatedAdopt >>
       \/ (* the OFFER response: ungated, and carrying NO certified answer (§4.1.3, §11 row 15) *)
          /\ RConfirmResponse(r, Absent, OfferIdentity(partMount))
          /\ UNCHANGED histVars

RPromoteCommit(r) ==
    /\ rState[r] = "answered" /\ rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "S3"]
    /\ rCommitted'  = [rCommitted  EXCEPT ![r] = TRUE]
    /\ rPublishes'  = [rPublishes  EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawProvenCommitted' = sawProvenCommitted \/ H(rDurableBefore[r])
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rDurableBefore, rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawLandedS2, sawChangedThenBytes,
                    sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow,
                    sawRetryCreatedAdopt >>

(* Anything not accepted aborts, and the abort RELEASES the receiver's protection (a durable -1).
   Task 3 splits this into the `changed` byte-fetch arm and the `unknown` retry-later arm. *)
RAbort(r) ==
    /\ rState[r] = "answered" /\ ~rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "done_retry"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* TypeOK's NEGATIVE CONTROL.  Writes a value outside `rPublishes`' declared domain, so `TypeOK`
   must break -- which is what discharges the "green only after red" rule for the type invariant
   itself.  It is enabled by nothing else and appears in exactly one cfg. *)
TypeProbe(r) ==
    /\ SabotageTypeProbe
    /\ rPublishes' = [rPublishes EXCEPT ![r] = 3]
    /\ UNCHANGED << rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
                    rAnsweredFenced, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

(* ---- GC: fold, then three-phase graduation with SPARING on positive in-degree ---------------- *)

ApplyOne(F, rec) ==
    IF rec.op = "add" THEN F \cup { [b |-> rec.blob, src |-> rec.src] }
                      ELSE F \ { [b |-> rec.blob, src |-> rec.src] }
RECURSIVE ApplyOrdered(_, _)
ApplyOrdered(F, S) ==
    IF S = {} THEN F
    ELSE LET m == CHOOSE x \in S : \A y \in S : x.id <= y.id
         IN ApplyOrdered(ApplyOne(F, m), S \ {m})

Avail == { rec \in journal : rec.id > cursor[rec.ns] }

(* CommittedEdgesAreGcVisible in model form: the fold observes ALL of `Avail`.  There is no
   `MaxHoles` dial here -- §12.1 reassigned listing completeness to the v9 chain models. *)
GFold ==
    /\ gcPhase = "idle" /\ round < MaxRound
    /\ folded' = ApplyOrdered(folded, Avail)
    /\ cursor' = [ ns \in Namespaces |->
                     LET seen == { rec.id : rec \in { x \in Avail : x.ns = ns } }
                     IN IF seen = {} THEN cursor[ns] ELSE Max(seen) ]
    /\ gcPhase' = "folded"
    /\ UNCHANGED << round, present, condemned, pendingDelete >>
    /\ UNCHANGED << senderVars, recvVars, logVars, histVars, partMount >>

GSettle ==
    /\ gcPhase = "folded"
    /\ LET live  == { b \in Blobs : Indeg(b) > 0 }
           kills == { b \in pendingDelete : present[b] /\ Indeg(b) = 0 }
           grads == { b \in (condemned \ pendingDelete) : present[b] /\ Indeg(b) = 0 }
           newly == { b \in Blobs : present[b] /\ Indeg(b) = 0 /\ b \notin condemned }
       IN /\ present'       = [ b \in Blobs |-> IF b \in kills THEN FALSE ELSE present[b] ]
          /\ condemned'     = ((condemned \ live) \ kills) \cup newly
          /\ pendingDelete' = ((pendingDelete \ live) \ kills) \cup grads
    /\ round' = round + 1
    /\ gcPhase' = "idle"
    /\ UNCHANGED << folded, cursor >>
    /\ UNCHANGED << senderVars, recvVars, logVars, histVars, partMount >>

NoOp == UNCHANGED vars     \* house pattern; every cfg also sets CHECK_DEADLOCK FALSE.  It is ALSO
                           \* the model of mapping rows 1, 3, 4, 5 and 6 -- every INERT outcome of
                           \* `resolveWedgeOnce`, which changes no state by construction.

Next ==
    \/ \E m \in Mounts :
         \/ \E nb \in {Other, NoBinding} : \E k \in ChunkKinds : SenderAdmit(m, nb, k)
         \/ SenderArm(m) \/ SenderDurable(m) \/ SenderInstall(m) \/ SenderCloseTenure(m)
         \/ SenderPoison(m)
         \/ SenderUnresolvedLanded(m) \/ SenderUnresolvedNotLanded(m)
         \/ WedgeRetryCreated(m)
         \/ WedgeResolveInstall(m) \/ WedgeResolveRejected(m) \/ WedgeResolveCorrupted(m)
         \/ WedgeResolveStale(m) \/ FloorReconcile(m) \/ CorruptFenceReaction(m)
         \/ FenceLoss(m) \/ ForeignRemove(m) \/ EvictTable(m) \/ RecoverForAnswer(m)
    \/ \E r \in Receivers : RStage(r) \/ RConfirm(r) \/ RPromoteCommit(r) \/ RAbort(r)
                            \/ TypeProbe(r)
    \/ GFold \/ GSettle
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ----------------------------------------------------------------------------- *)

(* COMPLETE over every variable, deliberately: a type invariant that omits a variable stops
   protecting against a wrong `EXCEPT` or a dropped `UNCHANGED` in exactly the actions this model
   was written to get right.  `_sab_typeprobe` is its negative control. *)
TypeOK ==
    /\ round \in Rounds
    /\ present \in [Blobs -> BOOLEAN]
    /\ pendingDelete \subseteq condemned /\ condemned \subseteq Blobs
    /\ \A e \in folded : e.b \in Blobs /\ e.src \in Sources
    /\ cursor \in [Namespaces -> Ids]
    /\ gcPhase \in {"idle", "folded"}
    /\ journal \subseteq Records
    /\ nextId \in 1..(MaxId + 1)
    /\ sDurableRef \in [Mounts -> Bindings]
    /\ sCacheRef   \in [Mounts -> Bindings]
    /\ sTarget     \in [Mounts -> Bindings]
    /\ sKind       \in [Mounts -> ChunkKinds]
    /\ sPending    \in [Mounts -> BOOLEAN]
    /\ sLeader     \in [Mounts -> BOOLEAN]
    /\ sArmed      \in [Mounts -> BOOLEAN]
    /\ sApply      \in [Mounts -> ApplyStates]
    /\ sApplySeen  \in [Mounts -> ApplyStates]
    /\ sApplyOwed  \in [Mounts -> BOOLEAN]
    /\ sFloorCovers \in [Mounts -> BOOLEAN]
    /\ sWedge      \in [Mounts -> BOOLEAN]
    /\ sForeign    \in [Mounts -> BOOLEAN]
    /\ sFence      \in [Mounts -> BOOLEAN]
    /\ sRecovered  \in [Mounts -> BOOLEAN]
    /\ sChunks       \in [Mounts -> 0..MaxChunks]
    /\ sTenureChunks \in [Mounts -> 0..MaxChunks]
    /\ partMount \in Mounts
    /\ rState \in [Receivers -> {"init", "S1", "answered", "S0", "S2", "S3",
                                 "done_bytes", "done_retry"}]
    /\ rAnswer \in [Receivers -> {"none", "proven", "changed", "unknown", Absent}]
    /\ rIdentity \in [Receivers -> {Absent} \cup
                        { Validator(m, b) : m \in Mounts, b \in Bindings }]
    /\ rAccepted  \in [Receivers -> BOOLEAN]
    /\ rCommitted \in [Receivers -> BOOLEAN]
    /\ rDurableBefore  \in [Receivers -> BOOLEAN]
    /\ rAnsweredFenced \in [Receivers -> BOOLEAN]
    /\ rPublishes  \in [Receivers -> 0..2]
    /\ rUnresolved \in [Receivers -> BOOLEAN]
    /\ \A v \in {sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                 sawChangedThenBytes, sawUnknown, sawColdRefused, sawCollision,
                 sawCorruptWindow, sawRetryCreatedAdopt} : v \in BOOLEAN

LiveBlobs == { b \in Blobs : present[b] }

(* THE THEOREM.  A relink COMMITTED on an ACCEPTED certificate whose activation (+1) was durable
   BEFORE the confirm never references a physically deleted blob.  Two deliberate choices:
     * the antecedent is `rAccepted`, not `rAnswer = "proven"` -- a sabotage that makes the receiver
       accept the WRONG thing must not escape the theorem by never producing the word `proven`;
     * `rCommitted` is not `rState = "S3"` -- a landed `Unresolved` promote (Task 3) publishes the
       relink while the receiver's own classification stays S2, and that publication is squarely
       inside what this theorem is about. *)
ConfirmedRelinkNeverDangles ==
    \A r \in Receivers :
        (rCommitted[r] /\ rAccepted[r] /\ rDurableBefore[r])
            => AdoptedBlobs \subseteq LiveBlobs

(* The antecedent-free form: broken by inverting the order, which leaves the guarded theorem
   vacuously satisfied.  Its red is `_sab_publishafterconfirm`, which lives in THIS task because a
   green may not rest on an invariant whose sabotage does not yet exist. *)
PromotedNeverDangles ==
    \A r \in Receivers : rCommitted[r] => AdoptedBlobs \subseteq LiveBlobs

(* SEAM §8 ROW S7, HALF ONE -- THE INTERVAL.  §5.1.2's requirement, stated as a model property:
   while an apply is OWED, the marker is not clean.  Note what it needs and does NOT need: the
   subject is this mount's OWN durable transaction (`sApplyOwed`), so no exclusion for the wedge and
   none for the fence is required, and none is added.  It holds through the retry that CREATES (the
   marker is already pending and stays so), through BOTH steps of the stale resolution, and through
   floor reconciliation. *)
MarkerCoversDurableWindow ==
    \A m \in Mounts : sApplyOwed[m] => (sApply[m] # "clean")

(* SEAM §8 ROW S7, HALF TWO -- THE OBSERVATION.  Seam §6: "a reader acquiring AFTER the arm
   necessarily observes `ApplyPending`".  With the arm and the read both under `state_mutex`, the
   mutex supplies the happens-before and the two values coincide.  This is what the fix must
   deliver; `_sab_relaxedmarker` is the world in which it does not. *)
MarkerSeenMatchesMarker == \A m \in Mounts : sApplySeen[m] = sApply[m]

(* ---- witnesses (negated reachability; a TLC violation means the state IS reachable) --------- *)

(* THE FLIP'S NON-VACUITY, and the model-level statement of §2's availability fix: a `proven` is
   actually given while the lane is BUSY -- the exact state v11 rule 3 refused. *)
W_BusyLaneProven == ~sawBusyProven

(* §12.5 ii, and STRONGER than "two chunks exist": a `proven` is given mid-tenure, with the tenure
   OPEN and a chunk of THAT tenure already installed. *)
W_MidTenureProven == ~sawMidTenureProven

W_ProvenCommitted == ~sawProvenCommitted    \* non-vacuity of the theorem's antecedent
W_BlobDeleted == \A b \in Blobs : present[b] \* non-vacuity of the consequent: GC really deletes

(* `CasRefLedger.cpp:1927-1934` + `:2050-2060`: the fence-live, marker-clean, WEDGED window between
   a Corrupted resolution and the fence-closing reaction really exists.  Task 3's `_sab_nowedge`
   depends on it, so it is proven reachable rather than assumed. *)
W_CorruptWindow == ~sawCorruptWindow

(* MAPPING ROW 11 -- THE WEDGE PROTOCOL'S HAPPY PATH IS REACHABLE.  `WedgeResolveInstall` fires, so a
   wedged transaction really is adopted in this model.  An unreachable happy path is the same
   vacuity trap as an unreachable hazard, one layer down: if the bounded retry could never resolve,
   every state downstream of a resolved wedge would be missing from the state space and
   `_sab_stalecache`'s green would be green for an incomplete reason. *)
W_RetryCreatedAdopt == ~sawRetryCreatedAdopt

=============================================================================
```

- [ ] **Step 7: Write the runner with expected verdicts, per-run isolation and markers.**

**Unchanged from v3** except the `CONFIGS` array. Create `docs/superpowers/models/run_relinkreoffer.sh`, `chmod +x`. Copy the structure of `run_refcatalog.sh` — same result extraction, same `violation:<NAME>` assertion, same `ALL EXPECTATIONS MET` / exit code — with `MODULE=CaRelinkReofferCore` and the isolation/marker block below, which exists because two concurrent batteries must not be able to delete each other's metadir or overwrite each other's log:

```bash
RUN_ID="${RUN_ID:-$(date +%Y%m%dT%H%M%S)-$$}"
LOCK=../../../tmp/.relinkreoffer.lock
: > "$LOCK" 2>/dev/null || true
exec 9>"$LOCK"
flock -n 9 || { echo "another relinkreoffer battery is running (lock: $LOCK)" >&2; exit 4; }
RUNDIR=../../../tmp/tlc-runs/relinkreoffer/$RUN_ID
METAROOT=../../../tmp/tlc-meta-relinkreoffer/$RUN_ID
mkdir -p "$RUNDIR" "$METAROOT"
echo "RUN_ID=$RUN_ID  logs=$RUNDIR"
```

and, per config:

```bash
  log="$RUNDIR/tlc_${name}.log"
  meta="$METAROOT/$name"
  { echo "=== TLC BEGIN ${MODULE}_${name} $(date -Is) ==="; } > "$log"
  start=$SECONDS
  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC \
    -metadir "$meta" -workers "${TLC_WORKERS:-1}" -config "$cfg" "$MODULE.tla" >>"$log" 2>&1
  rc=$?
  elapsed=$((SECONDS - start))
  { echo "=== TLC END ${MODULE}_${name} rc=${rc} $(date -Is) ==="; } >> "$log"
  ln -sfn "tlc-runs/relinkreoffer/$RUN_ID/tlc_${name}.log" \
          "../../../tmp/tlc_${MODULE}_${name}.log"
```

The stable symlink is a convenience for interactive work; **RESULTS must cite `$RUNDIR/tlc_<cfg>.log`**, never the symlink. The header comment lists every config with its expectation and the one-line reason, in the `run_refcatalog.sh` style, and states the `-workers 1` rationale.

Its `CONFIGS` array for THIS task is exactly:

```bash
CONFIGS=(
  "sab_typeprobe             violation  TypeOK"
  "sab_noapplypending        violation  ConfirmedRelinkNeverDangles"
  "sab_noapplypending_window violation  MarkerCoversDurableWindow"
  "sab_relaxedmarker         violation  MarkerSeenMatchesMarker"
  "sab_publishafterconfirm   violation  PromotedNeverDangles"
  "v11_baseline              green      -"
  "ctl_v11nomarker           green      -"
  "sab_stalecache            green      -"
  "witness_busylane          violation  W_BusyLaneProven"
  "witness_midtenure         violation  W_MidTenureProven"
  "witness_proven            violation  W_ProvenCommitted"
  "witness_delete            violation  W_BlobDeleted"
  "witness_corruptwindow     violation  W_CorruptWindow"
  "witness_retrycreated      violation  W_RetryCreatedAdopt"
)
```

Tasks 2 and 3 extend this array; the sabotage-before-green ordering is preserved as they do.

- [ ] **Step 8: Write the five failing-first configs.**

The full constants block, given once — every later cfg in this plan is a delta against it. **Unchanged from v2/v3:**

```
SPECIFICATION Spec
CONSTANTS
    Receivers = {r1}
    MaxId = 6
    MaxRound = 5
    MaxChunks = 2
    SecondMount = FALSE
    EqualNamespaces = TRUE
    ModelColdTable = FALSE
    TrackHistory = FALSE
    SabotageStaleCache = TRUE
    SabotageNoApplyPending = FALSE
    SabotageRelaxedMarker = FALSE
    SabotageNoPoison = FALSE
    SabotageNoWedge = FALSE
    SabotageNoFence = FALSE
    SabotageNoRowExact = FALSE
    SabotageBareValidator = FALSE
    SabotageNoDiskQual = FALSE
    SabotageInferAnswer = FALSE
    SabotageSkipIdentity = FALSE
    SabotagePublishAfterConfirm = FALSE
    SabotageS2ByteFetch = FALSE
    SabotageTypeProbe = FALSE
CHECK_DEADLOCK FALSE
```

| cfg | delta | `INVARIANTS` | header states |
| --- | --- | --- | --- |
| `sab_typeprobe` | `SabotageTypeProbe = TRUE` | `TypeOK` | `TypeOK`'s negative control: an action writes `rPublishes := 3`, outside its declared `0..2`. Its red is what makes every green's `TypeOK` evidence rather than decoration, and it is why type invariants need no exemption from the green-only-after-red rule. |
| `sab_noapplypending` | `SabotageNoApplyPending = TRUE` | `ConfirmedRelinkNeverDangles` | §12.3 step 2, first half: the marker is never armed while the design's deletion of v11 rule 3's lane-quiescence terms stands. TLC MUST report a counterexample — the confirm reads a committed row that lags a DURABLE removal, exactly the v11 `_sab_stalecache` trace. This is the guard doing real work. |
| `sab_noapplypending_window` | `SabotageNoApplyPending = TRUE` | `MarkerCoversDurableWindow` | The same toggle's SECOND consequence, and the direct statement of §5.1.2's requirement on the seam fix. Seam §8 row S7, interval half. |
| `sab_relaxedmarker` | `SabotageRelaxedMarker = TRUE` | `MarkerSeenMatchesMarker` | Seam §8 row S7, OBSERVATION half: the arm stores `ApplyPending` and the store is not published, which is what a relaxed `compare_exchange_strong` called with no lock held permits (`CasRefLedger.cpp:1681` called at `:2808`). **Distinct from `_sab_noapplypending` by construction, not by trace comparison:** there the marker's stored value never moves, here it does, and the two configs break DIFFERENT invariants. |
| `sab_publishafterconfirm` | `SabotagePublishAfterConfirm = TRUE` | `PromotedNeverDangles` | The design's ORDER is inverted — confirm and promote BEFORE the receiver's `+1` is durable. The guarded theorem stays vacuously satisfied (`rDurableBefore` is FALSE), so the antecedent-free form is what must break. **It lives in Task 1, not Task 3, because Task 1's and Task 2's greens list `PromotedNeverDangles` and a green may not rest on an invariant whose red does not yet exist.** The alternative — dropping the invariant from the earlier greens and adding it in Task 3 — was rejected because it would run the 2×2 gate without the antecedent-free theorem, weakening the very cell that decides the design. §core-idea survives verbatim and this is its check. |

- [ ] **Step 9: Run the five sabotages FIRST. All five MUST be red.**

```bash
bash /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/run_relinkreoffer.sh
```
Expected: five `violation:<the named invariant>` PASS rows; the nine not-yet-written configs report `error` — expected, and why they are written next.

Read each counterexample and check it against the code, not just against the expectation:
- `sab_noapplypending`'s trace must go `Admit(tracked) → Arm → Durable → (GFold → GSettle)×3 → Stage → Confirm → PromoteCommit` — note the alternation: consecutive `GSettle`s are disabled by `gcPhase`, so each graduation phase costs a fold and a settle. The blob is deleted BEFORE the receiver's `+1` and the answer comes from the stale row. Minimum depth ≈ 12.
- `sab_relaxedmarker`'s trace must reach a state with `sApply = "pending"` and `sApplySeen = "clean"` — depth 2 or 3. If it reports both values clean, the sabotage is wired into the wrong branch of `SenderArm`.
- `sab_publishafterconfirm`'s trace must confirm from `"init"` and stage from `"S3"`. Minimum depth ≈ 10.

If any sabotage comes back GREEN, fix the MODEL (never the invariant) and re-run.

- [ ] **Step 10: Write the three 2×2 configs and run THE GATE.**

Three configs, deltas against step 8's block:

| cfg | `SabotageStaleCache` | `SabotageNoApplyPending` | `INVARIANTS` | expected |
| --- | --- | --- | --- | --- |
| `v11_baseline` | `FALSE` | `FALSE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` `MarkerSeenMatchesMarker` | green |
| `ctl_v11nomarker` | `FALSE` | `TRUE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` | green |
| `sab_stalecache` | `TRUE` | `FALSE` | `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` `MarkerSeenMatchesMarker` | **green — THE FLIP** |

`ctl_v11nomarker` omits both marker invariants: with the marker unarmed they are FALSE by construction, and step 9 already proved the first one red. Omitting them here is scoping, not weakening — put that sentence in the cfg header. Its green is not theorem-vacuous: `Stage → Confirm → PromoteCommit` makes `rDurableBefore` true, and the v11 terms refuse throughout the durable-but-unapplied window because `SenderInstall` preserves `sLeader` until `SenderCloseTenure`.

`_sab_stalecache`'s header must carry:

```
\* *** THE GATE (spec §12.3 step 3). ***  v11 rule 3's lane-quiescence terms are DROPPED -- which is
\* what this design ships -- and the apply-pending marker is INTACT.  In `CaRelinkConfirmCore` the
\* same-named cfg MUST be RED; here it MUST be GREEN, and that flip is the proof that the terms this
\* design deletes were redundant rather than load-bearing.  If TLC reports a violation, the design is
\* wrong and MUST NOT BE IMPLEMENTED.
```

Run the harness again. Expected: three greens.

**THE VERDICT.** GREEN → the gate's first half is met; continue. RED → **STOP**. Do not start Task 2. Preserve the run's log directory, jump to Task 5, write RESULTS with `RELINK TLA GATE: FAIL`, name the violated invariant, quote the counterexample, and report the design as refuted per §1 gate 2 and §12.3 step 3.

- [ ] **Step 11: Write the six witnesses and prove the flip is not vacuous.**

All six use the DESIGN's settings (`SabotageStaleCache = TRUE`, `SabotageNoApplyPending = FALSE`, `SabotageRelaxedMarker = FALSE`) — a witness run under different settings proves nothing about the config it de-vacuums — and all six set `TrackHistory = TRUE`. One negated invariant each:

| cfg | `INVARIANT` | what its violation proves |
| --- | --- | --- |
| `witness_busylane` | `W_BusyLaneProven` | a `proven` is actually given while `sPending \/ sLeader`. **This is what makes `_sab_stalecache` GREEN mean something.** |
| `witness_midtenure` | `W_MidTenureProven` | a `proven` is given with the tenure OPEN and a chunk of that tenure already installed — §12.5 ii, and reachable only because `SenderAdmit` has an `"unrelated"` kind |
| `witness_proven` | `W_ProvenCommitted` | the theorem's antecedent is reachable |
| `witness_delete` | `W_BlobDeleted` | GC physically deletes, so the consequent is not trivially true |
| `witness_corruptwindow` | `W_CorruptWindow` | the fence-live, marker-clean, wedged window between a Corrupted resolution and the fence-closing reaction is reachable |
| `witness_retrycreated` | `W_RetryCreatedAdopt` | **the wedge protocol's HAPPY PATH is reachable** — a wedged transaction is actually adopted. Minimum route: `Admit → Arm → UnresolvedNotLanded → WedgeRetryCreated → WedgeResolveInstall`, ≈6 transitions |

Run the harness. Expected: 14 rows, `ALL EXPECTATIONS MET`.

Three witnesses are gate-critical, and a GREEN on any of them is to be treated exactly as a RED `_sab_stalecache` — stop and report:
- `witness_busylane` green ⇒ the busy-lane `proven` is unreachable, so the flip's green is vacuous.
- `witness_midtenure` green ⇒ the multi-chunk tenure is unreachable and the flip is the one-transaction artefact §12.5 ii warns about. Most likely causes, in order: `SenderAdmit`'s `"tracked"` guard leaking onto the `"unrelated"` kind; `sTenureChunks` not reset by `SenderCloseTenure`; `sTenureChunks` not incremented by `SenderInstall`.
- `witness_retrycreated` green ⇒ **no wedge is ever resolved**, so every state downstream of a resolved wedge is missing from the state space and the whole wedge lifecycle is decoration. Most likely cause: a guard on `WedgeRetryCreated` that no reachable wedge state satisfies — check `sApply[m] = "pending"` against what `SenderUnresolvedNotLanded` actually leaves, and check `nextId <= MaxId` against the ids the prefix already consumed.

Fix the model and re-run steps 9–11 in order.

- [ ] **Step 12: Audit every sender action against its citation and the mapping against the code, then commit.**

Two audits, and the second is the one three review rounds have turned on.

**Audit A — the nineteen actions of `Next`'s mount disjunction:** `SenderAdmit`, `SenderArm`, `SenderDurable`, `SenderInstall`, `SenderCloseTenure`, `SenderPoison`, `SenderUnresolvedLanded`, `SenderUnresolvedNotLanded`, `WedgeRetryCreated`, `WedgeResolveInstall`, `WedgeResolveRejected`, `WedgeResolveCorrupted`, `WedgeResolveStale`, `FloorReconcile`, `CorruptFenceReaction`, `FenceLoss`, `ForeignRemove`, `EvictTable`, `RecoverForAnswer`. Confirm each matches the code site named in its comment.

**Audit B — the mapping is EXHAUSTIVE, checked against the declarations rather than against memory.** Open `CasRefLedger.h:759-771`, `Backend/CasRequestControl.h:399` and `CasRefLedger.cpp:1726-1734` and confirm:
- all **six** `WedgeResolution` values appear in the mapping (`NoWedge`, `Adopted`, `FloorReconciled`, `Rejected`, `StillWedged`, `Corrupted`);
- all **three** `SlotOccupyResult::Kind` values appear (`Created`, `Occupied`, `Unresolved`) — and `Created` has an action, which is the gap the third review round found;
- all **four** occupant classes appear (`NotOccupied`, `Mine`, `SuccessorSeal`, `Foreign`);
- all **seven** `Reason` values appear, and each is attributed to the arm that sets it — in particular `RefusedPreAttempt` to the `Unresolved` RESULT at `:1900-1908` and **not** to the `slotOccupy` throw at `:1815-1830`, which sets no `reason` at all;
- both exception exits appear (the pre-I/O decode/apply at `:1786-1793`, and the `slotOccupy` catch);
- every row that maps to NO transition says which and why.

If any of those six checks fails, the mapping is not exhaustive and the model's fidelity is unproven — fix it before committing, and record what was missing.

Six actions have been caught wrong by review at least once; verify them individually:

1. `SenderUnresolvedLanded` and `SenderUnresolvedNotLanded` leave `sApply` at `"pending"` (`:3134-3145`).
2. `WedgeRetryCreated` exists, makes the wedged txn durable, KEEPS the wedge and leaves the marker `"pending"` (`.h:784-801`).
3. `WedgeResolveCorrupted` clears the marker and KEEPS `sWedge` (`:1927-1934`); `CorruptFenceReaction` is a SEPARATE action (`:2050-2060`).
4. `WedgeResolveStale` sets `sFloorCovers`, poisons, and KEEPS `sWedge` (`:1936-1950`).
5. `FloorReconcile` retires the wedge, installs NOTHING, and retains both `Poisoned` and `sApplyOwed` (`:1758-1766`).
6. `WedgeResolveInstall` is guarded by `~sFloorCovers[m]` AND `sApply[m] # "poisoned"`.

Finally, `grep -c 'sFloorCovers' CaRelinkReofferCore.tla` and `grep -c 'sawRetryCreatedAdopt' CaRelinkReofferCore.tla`, and confirm every action either writes each or lists it in an `UNCHANGED` — TLC reports a missing assignment, but an accidental omission inside a tuple will not be caught for you.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_typeprobe.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_noapplypending_window.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_relaxedmarker.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_publishafterconfirm.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v11_baseline.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_v11nomarker.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_stalecache.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_busylane.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_midtenure.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_proven.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_delete.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_corruptwindow.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_retrycreated.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — CaRelinkReofferCore: apply-marker refinement, exhaustive resolveWedgeOnce mapping (incl. the retry that CREATES), and the 2x2 gate (_sab_stalecache FLIPS GREEN)"
```

---

### Task 2: The cross-mount collision, and the answer/identity split

§12.5 row i, verbatim: *"MODEL IT, and specifically: two mounts with EQUAL `root_namespace` and DIFFERENT `disk_name` ... Without that shape a model can pass while qualifying by namespace only."* Plus §4.2's requirement that the certified answer and the returned identity agree independently — which lives here because it is the same subject: what the validator is, and who compares it.

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (add `PartMove` and `W_CollisionReached`; wire `PartMove` into `Next`)
- Create: `..._sab_barevalidator.cfg`, `..._sab_nodiskqualification.cfg`, `..._sab_inferanswer.cfg`, `..._ctl_distinctns.cfg`, `..._ctl_skipidentity.cfg`, `..._witness_collisionreached.cfg`
- Modify: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: spec §3, §4.1 (the validator paragraph), §4.1.3, §4.2's answer table, §4.4, §11 rows 15 and 17, §12.5 row i.

**Interfaces:**
- Consumes: Task 1's `Validator(m, b)`, `HeldValidator`, `NsOf(m)`, `BlobOf(m)`, `partMount`, `SenderAnswer(m)`, `SenderIdentity(m)`, `OfferIdentity(m)`, `RAccepts(ans, idn)`, `sawCollision`, `SecondMount`, `EqualNamespaces`, `SabotageBareValidator`, `SabotageNoDiskQual`, `SabotageInferAnswer`, `SabotageSkipIdentity`.
- Produces: `W_CollisionReached`; the two reds §12.5 row i names by name; the answer/identity scoping result.

---

- [ ] **Step 1: Add the part move and the collision witness.**

Insert after `RecoverForAnswer`:

```tla
(* `MOVE ... TO DISK` between offer and confirm.  The offer was answered by mount A; after this the
   confirm routes to mount B -- "a part that moves between same-pool disks can be answered for by a
   mount that never made the offer" (§3).  This is test row 17's driver. *)
PartMove ==
    /\ SecondMount /\ partMount = MountA
    /\ partMount' = MountB
    /\ UNCHANGED << gcVars, senderVars, recvVars, logVars, histVars >>
```

Add `\/ PartMove` to `Next`, and beside the other witnesses:

```tla
(* §12.5 i's non-vacuity: the collision STATE is reachable -- a `proven` emitted by a mount that is
   NOT the offering mount, holding the SAME ManifestRef text, under an EQUAL namespace string.
   Without this, the two validator reds could be red for some other reason and `_ctl_distinctns`
   could be green for free. *)
W_CollisionReached == ~sawCollision
```

- [ ] **Step 2: Write the three must-red configs.**

Deltas against Task 1 step 8's block. `INVARIANTS ConfirmedRelinkNeverDangles` in all three. **The mount setting differs between them, and deliberately:** the two validator sabotages need the second mount because a cross-mount collision is what they are about; `_sab_inferanswer` needs no second mount at all, because an ungated offer response is a single-mount hazard, and running it with one mount keeps its state space small.

`sab_barevalidator` — `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageBareValidator = TRUE`:

```
\* SABOTAGE (§12.5 i): the validator is the bare ManifestRef, with no qualification at all -- the
\* B1 collision shape.  `CasTypes.h:131-134`: "Two namespaces may legally carry the same ManifestRef
\* tuple without addressing the same object."  TLC MUST report a ConfirmedRelinkNeverDangles
\* counterexample: mount A's binding is removed and its blob deleted; the part MOVES to mount B,
\* which still binds the same ManifestRef TEXT over a DIFFERENT object; the bare comparison matches
\* and the receiver promotes over blobs nobody is protecting.
```

`sab_nodiskqualification` — `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageNoDiskQual = TRUE`:

```
\* SABOTAGE (§12.5 i) -- *** THE ONE THAT PINS B1'S ACTUAL FIX. ***  The validator KEEPS the
\* namespace and DROPS `disk_name`.  §3: a namespace is `<server_root_id>/store/<u3>/<uuid>@cas@`,
\* so two content-addressed disks with the same `server_root_id`, in one pool, serving one table
\* produce the IDENTICAL namespace string -- which is why `resolveContentAddressedConfirm` demands
\* exactly one matching mount today.  With EqualNamespaces = TRUE the namespace discriminates
\* NOTHING, so TLC MUST report a ConfirmedRelinkNeverDangles counterexample.  A model without this
\* shape passes while qualifying by namespace only; `_ctl_distinctns` is the control that proves the
\* shape is what makes this red.
```

`sab_inferanswer` — `SecondMount = FALSE`, `SabotageInferAnswer = TRUE`:

```
\* SABOTAGE (§4.1.3 defence 1): the receiver INFERS the answer from a matching identity instead of
\* requiring the explicit `content_addressed_answer` cookie.  "The offer response carries the same
\* validator, and the offer path is not gated" -- `getRelinkOffer` applies none of §4.2's fence,
\* wedge or apply-state checks, because an offer is a proposal, not a certificate.  So a stripped
\* mode parameter (§11 row 15) makes the sender reply with an ordinary OFFER whose identity may
\* equal the held validator, and a receiver comparing validators alone promotes on an UNGATED
\* resolve.  ONE MOUNT is enough: this hazard is about the offer path being ungated, not about
\* mounts.  TLC MUST report a ConfirmedRelinkNeverDangles counterexample.
```

- [ ] **Step 3: Run all three. All three MUST be red — this is the failing-first step.**

Add to `CONFIGS`, immediately after `sab_relaxedmarker`:

```bash
  "sab_barevalidator        violation  ConfirmedRelinkNeverDangles"
  "sab_nodiskqualification  violation  ConfirmedRelinkNeverDangles"
  "sab_inferanswer          violation  ConfirmedRelinkNeverDangles"
```

Run. Expected: three `violation:ConfirmedRelinkNeverDangles`.

If `sab_nodiskqualification` is GREEN the collision is unreachable: check, in this order, that `PartMove` is in `Next`; that mount B's row holds `Token` (`Init` over `Mounts`); that `MaxId` admits the trace (A's removal and the receiver's `+1` — two ids and three GC rounds suffice). If `sab_inferanswer` is GREEN, the offer-response arm of `RConfirm` is unreachable — most likely the disjunction collapsed because both arms were written into one conjunct list. Fix the model; do not touch the invariant.

- [ ] **Step 4: Write the two controls, whose GREEN is the result.**

`ctl_distinctns` — identical to `sab_nodiskqualification` except `EqualNamespaces = FALSE`; `INVARIANTS TypeOK ConfirmedRelinkNeverDangles PromotedNeverDangles`:

```
\* CONTROL for _sab_nodiskqualification.  The SAME sabotage -- `disk_name` dropped -- with the two
\* mounts carrying DIFFERENT namespace strings.  Expected GREEN: the namespace alone separates them,
\* so nothing collides.  That is the whole point of §12.5 i's "specifically": the red next door is
\* caused by the EQUAL-namespace configuration, not by the second mount existing.  This control
\* proves only the restricted claim -- namespace qualification suffices WHEN namespaces differ.
```

`ctl_skipidentity` — `SabotageSkipIdentity = TRUE`, `SecondMount = TRUE`, `EqualNamespaces = TRUE`; same three invariants:

```
\* CONTROL, and *** GREEN IS THE RESULT, NOT A DISAPPOINTMENT. ***  The receiver trusts `proven` and
\* SKIPS §4.4 condition 4 (the identity comparison).  Expected GREEN, and the green is the finding:
\* under a faithful sender the certified answer and the identity match are computed from the SAME
\* committed row, so they are one fact and the receiver's own comparison is redundant.  What it
\* defends is a response that did not come from THIS confirm -- a replay, or an offer mistaken for a
\* confirm -- and that is `FreshCertifiedResponse`'s scope (§11 rows 15, 16), discharged by test
\* rather than by this model.  Recorded as a MEASURED scoping result so that nobody later reads
\* §4.2's "each is separately sufficient to fail closed" as an unmodelled gap: half of it IS
\* modelled (`_sab_inferanswer`, red), and this config is why the other half is not.
\* NOTE: the fields are still DECOUPLED in the model -- sender answer, returned identity and
\* receiver acceptance are three separate objects -- which is what makes this measurement possible
\* at all.  Do not re-fuse them.
```

- [ ] **Step 5: Write the collision witness.**

`witness_collisionreached`: `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `TrackHistory = TRUE`, `SabotageStaleCache = TRUE`, `SabotageNoDiskQual = TRUE` — the witness must run under the settings whose red it de-vacuums. `INVARIANT W_CollisionReached`.

- [ ] **Step 6: Run the battery so far.**

Extend `CONFIGS` with `"ctl_distinctns green -"` and `"ctl_skipidentity green -"` among the greens, and `"witness_collisionreached violation W_CollisionReached"` among the witnesses. Run. Expected: **20 rows**, `ALL EXPECTATIONS MET`.

- [ ] **Step 7: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_barevalidator.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nodiskqualification.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_inferanswer.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_distinctns.cfg \
        docs/superpowers/models/CaRelinkReofferCore_ctl_skipidentity.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_collisionreached.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — cross-mount collision (equal-ns/different-disk) + the answer/identity split: _sab_nodiskqualification and _sab_inferanswer red, both controls green"
```

---

### Task 3: Re-derivation against the new rule set, and the four-state receiver

§12.4: the fence moves first, `No` becomes an authoritative outcome with its own successor action, the model must show the byte fetch following a `No` cannot publish twice, and the `_witness_confirmno` / `_witness_confirmunknown` witnesses must be re-derived because the states they prove reachable are no longer the same states.

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (split `RAbort`; add S0/S2 with a landed-`Unresolved` publication; add `NeverPublishedTwice`, `ChangedImpliesFenced` and four witnesses)
- Create: `..._sab_nofence.cfg`, `..._sab_nofence_changed.cfg`, `..._sab_nopoison.cfg`, `..._sab_nowedge.cfg`, `..._sab_norowexact.cfg`, `..._sab_s2bytefetch.cfg`, `..._v12_design_full.cfg`, `..._v12_coldanswer.cfg`, `..._witness_changed.cfg`, `..._witness_unknown.cfg`, `..._witness_budgetunknown.cfg`, `..._witness_landeds2.cfg`
- Modify: `docs/superpowers/models/run_relinkreoffer.sh`
- Read first: spec §4.2, §4.4, §6.1–§6.4, §12.4.

**Interfaces:**
- Consumes: everything from Tasks 1–2.
- Produces: `NeverPublishedTwice`, `ChangedImpliesFenced`, `W_ChangedThenBytes`, `W_UnknownRefusal`, `W_ColdRefused`, `W_LandedS2`; and `_v12_design_full`, the single green in which the whole universe is checked together.

---

- [ ] **Step 1: Replace `RAbort` with the answer-specific successors, and add S0/S2.**

Delete `RAbort` and insert:

```tla
(* §4.4: `changed` with a present, different identity => abort the prepared relink, then FETCH THE
   BYTES FROM THE SAME SENDER.  Today that is forbidden; under fence-first there is no doubt left to
   protect against, because rule 1 established the answering mount held its fence. *)
RChangedThenBytes(r) ==
    /\ rState[r] = "answered" /\ ~rAccepted[r] /\ rAnswer[r] = "changed"
    /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawChangedThenBytes' = sawChangedThenBytes \/ H(TRUE)
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawLandedS2,
                    sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow,
                    sawRetryCreatedAdopt >>

(* §4.4 and §6.2: everything else is one outcome -- abort, then throw the retry-later
   NETWORK_ERROR.  NO byte re-request. *)
RUnknownThenRetry(r) ==
    /\ rState[r] = "answered" /\ ~rAccepted[r] /\ rAnswer[r] # "changed"
    /\ rState' = [rState EXCEPT ![r] = "done_retry"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* §6.2: `promote` has THREE outcomes.  `MechanismFallbackAllowed` was rejected BEFORE its ref-log
   append, so "nothing was committed" is PROVEN -- state S0, whose action is a byte fetch. *)
RPromoteFallback(r) ==
    /\ rState[r] = "answered" /\ rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "S0"]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes, rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

S0Bytes(r) ==
    /\ rState[r] = "S0"
    /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
    /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

(* §6.3: `Unresolved` -- the ref-log append was attempted and came back without a verdict, so the
   receiver's ref MAY be committed.  Modelled as exactly that: an ambiguity resolved LATER, either
   way, by something the receiver does not control. *)
RPromoteUnresolved(r) ==
    /\ rState[r] = "answered" /\ rAccepted[r]
    /\ rState' = [rState EXCEPT ![r] = "S2"]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = TRUE]
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rPublishes >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>

(* THE APPEND HAD LANDED: the relink IS committed, even though the receiver's own classification is
   still S2.  `rCommitted` is what both dangle theorems quantify over, which is why this
   publication cannot escape them -- an earlier draft of this model tested `rState = "S3"` and let
   exactly this trace through. *)
S2ResolveLanded(r) ==
    /\ rUnresolved[r]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = FALSE]
    /\ rCommitted'  = [rCommitted  EXCEPT ![r] = TRUE]
    /\ rPublishes'  = [rPublishes  EXCEPT ![r] = rPublishes[r] + 1]
    /\ sawLandedS2' = sawLandedS2 \/ H(TRUE)
    /\ UNCHANGED << rState, rAnswer, rIdentity, rAccepted, rDurableBefore, rAnsweredFenced >>
    /\ UNCHANGED << gcVars, senderVars, logVars, partMount >>
    /\ UNCHANGED << sawBusyProven, sawMidTenureProven, sawProvenCommitted, sawChangedThenBytes,
                    sawUnknown, sawColdRefused, sawCollision, sawCorruptWindow,
                    sawRetryCreatedAdopt >>

S2ResolveNotLanded(r) ==
    /\ rUnresolved[r]
    /\ rUnresolved' = [rUnresolved EXCEPT ![r] = FALSE]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> BlobA, src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rState, rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore,
                    rAnsweredFenced, rPublishes >>
    /\ UNCHANGED << gcVars, senderVars, histVars, partMount >>

(* §6.3's action: throw retry-later, NEVER a byte fetch.  "This is the one state where a byte fetch
   would be a defect, because it would publish the part a second time over a relink that may already
   be committed."  `SabotageS2ByteFetch` does exactly that, and must break NeverPublishedTwice --
   the necessity half of the named assumption `UnresolvedPromoteNeverBytes`. *)
S2Retry(r) ==
    /\ rState[r] = "S2"
    /\ IF SabotageS2ByteFetch
         THEN /\ rState' = [rState EXCEPT ![r] = "done_bytes"]
              /\ rPublishes' = [rPublishes EXCEPT ![r] = rPublishes[r] + 1]
         ELSE /\ rState' = [rState EXCEPT ![r] = "done_retry"]
              /\ UNCHANGED rPublishes
    /\ UNCHANGED << rAnswer, rIdentity, rAccepted, rCommitted, rDurableBefore, rAnsweredFenced,
                    rUnresolved >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars, partMount >>
```

Extend the receiver disjunct of `Next`:

```tla
    \/ \E r \in Receivers :
         \/ RStage(r) \/ RConfirm(r) \/ TypeProbe(r)
         \/ RPromoteCommit(r) \/ RPromoteFallback(r) \/ RPromoteUnresolved(r)
         \/ RChangedThenBytes(r) \/ RUnknownThenRetry(r)
         \/ S0Bytes(r) \/ S2Retry(r) \/ S2ResolveLanded(r) \/ S2ResolveNotLanded(r)
```

- [ ] **Step 2: Add the two new invariants and the four witnesses.**

```tla
(* §12.4: "the model must show that the byte fetch following a `No` cannot publish twice".  Counted,
   not asserted structurally: a relink promote that COMMITS publishes once; an ambiguous promote that
   turns out to have landed publishes once; a byte fetch publishes once.  Two is the defect. *)
NeverPublishedTwice == \A r \in Receivers : rPublishes[r] <= 1

(* THE FENCE-FIRST PAYOFF.  An authoritative `No` is what authorizes a same-sender byte fetch, and
   only a mount that HELD ITS FENCE may emit one -- "a fenced-out mount is not this namespace's
   writer and its answer is not an answer" (§4.2).  With the fence evaluated LAST, as v11 did, a
   fenced-out instance's row comparison could speak first. *)
ChangedImpliesFenced ==
    \A r \in Receivers : (rAnswer[r] = "changed") => rAnsweredFenced[r]

(* Re-derived from v11's `_witness_confirmno`: the state it proves reachable is no longer the same
   state.  The `changed` answer fires AND its successor byte fetch runs -- the arm that did not
   exist before the reorder. *)
W_ChangedThenBytes == ~sawChangedThenBytes

(* Re-derived from v11's `_witness_confirmunknown`: `unknown` now folds a DIFFERENT rule set -- the
   fence is hoisted out of it and the lane-quiescence terms are gone.  The flag is set only for a
   GATE refusal, not for the offer-response arm, so the witness stays precise. *)
W_UnknownRefusal == ~sawUnknown

(* §12.4's revisit of rule 2: recovery is MANDATORY on the answer path, so the cold refusal is now
   on the path every cold answer takes rather than one nobody takes. *)
W_ColdRefused == ~sawColdRefused

(* The landed-Unresolved publication -- the trace both dangle theorems used to miss -- is reachable,
   so extending them to `rCommitted` is not a vacuous strengthening. *)
W_LandedS2 == ~sawLandedS2
```

- [ ] **Step 3: Write the six must-red configs and run them FIRST.**

Deltas against Task 1 step 8's block, i.e. `SabotageStaleCache = TRUE` (every retained rule is re-derived against the SHIPPED gate, not against v11's) and everything else honest except the row's own toggle. `_sab_publishafterconfirm` is NOT here — it landed in Task 1, where the invariant it protects is first listed by a green.

| cfg | delta | `INVARIANTS` | one-line intent for the cfg header |
| --- | --- | --- | --- |
| `sab_nofence` | `SabotageNoFence = TRUE` | `ConfirmedRelinkNeverDangles` | rule 1, hoisted first, still load-bearing: a deposed instance answers `proven` about a namespace somebody else now writes |
| `sab_nofence_changed` | `SabotageNoFence = TRUE` | `ChangedImpliesFenced` | the SECOND consequence of the same toggle: a fenced-out mount emits an authoritative `No`, which is what authorizes a same-sender byte fetch |
| `sab_nopoison` | `SabotageNoPoison = TRUE` | `ConfirmedRelinkNeverDangles` | rule 4's Poisoned arm: after a poisoned apply the lane IS quiescent and the marker is not pending, so only this arm sees the permanently stale row. **THREE routes reach a poisoned table and all three are real — `SenderPoison` (the apply threw), `WedgeResolveStale` from a landed unresolved attempt (mapping row 10 → row 9), and `WedgeResolveStale` after `WedgeRetryCreated` (mapping row 12). Note which one the trace takes; any of the three is a valid red, but a trace that takes none of them means the sabotage is red for some other reason.** |
| `sab_nowedge` | `SabotageNoWedge = TRUE` | `ConfirmedRelinkNeverDangles` | rule 3, and its counterexample must run through the state `CasRefLedger.cpp:1927-1934` singles out: a Corrupted resolution has cleared the marker and KEPT the wedge, the fence-closing reaction has not run yet, and the foreign occupant that proved exclusivity was breached is free to remove the binding. In that window rule 3 is the only guard. **Check the trace: if it does not contain `WedgeResolveCorrupted`, the red is coming from somewhere else and the sabotage is not testing what it claims.** The retry path cannot supply a shortcut here — `WedgeResolveCorrupted` requires `~sApplyOwed`, and `WedgeRetryCreated` sets it — so mapping row 8 is the only route and the check is exact. |
| `sab_norowexact` | `SabotageNoRowExact = TRUE` | `ConfirmedRelinkNeverDangles` | rule 5's exactness (v11's `_sab_nogate1`, re-derived): presence instead of validator equality is an ABA |
| `sab_s2bytefetch` | `SabotageS2ByteFetch = TRUE` | `NeverPublishedTwice` | §6.3: S2 is the one state where a byte fetch double-publishes. The necessity half of `UnresolvedPromoteNeverBytes` |

Add all six to `CONFIGS` in the sabotage block and run. Expected: six violations, each matching its named invariant. A red on a DIFFERENT invariant is a FAIL per the Global Constraints — investigate the trace, do not relabel the cfg.

- [ ] **Step 4: Write the two remaining greens.**

`v12_design_full` — **the config in which the whole universe is checked together**, so no property is only ever checked in isolation. Deltas: `SecondMount = TRUE`, `EqualNamespaces = TRUE`, `SabotageStaleCache = TRUE`, everything else honest. `INVARIANTS`: `TypeOK` `ConfirmedRelinkNeverDangles` `PromotedNeverDangles` `MarkerCoversDurableWindow` `MarkerSeenMatchesMarker` `NeverPublishedTwice` `ChangedImpliesFenced`. Header:

```
\* THE DESIGN, WHOLE.  Two mounts (equal namespace, different disk), the marker armed, published and
\* cleared, the FULL wedge lifecycle -- the retry that CREATES, both adoption routes, both steps of
\* the stale resolution, floor reconciliation -- a tenure that commits several chunks of two kinds,
\* the four-state receiver with the decoupled answer/identity pair, and v11 rule 3's terms deleted:
\* every mechanism switched on at once.  Every other green isolates one thing; this is the only place
\* they are checked TOGETHER, which is what rules out a property that holds only when its neighbours
\* are off.
```

`v12_coldanswer` — identical except `SecondMount = FALSE` and `ModelColdTable = TRUE`, same seven invariants. Header: rule 2 is now on the answer path (§4.3's mandatory recovery, §12.4's revisit); this config runs eviction and peer-initiated recovery, and it is also the only place where the poisoned/floor-reconciled lane has an exit — `EvictTable` then `RecoverForAnswer`, which is the model of "until a recovery re-derives it from the log".

Run. Expected: both green. If `v12_design_full` exceeds ~10 min at `-workers 1`, drop `MaxId` to 5, then `MaxRound` to 4 — and record the bound in RESULTS. Never drop an invariant to make it finish. Note that the retry-created arm adds journal ids: if the run reports states left on queue against `nextId`, that is the bound biting, not a defect.

- [ ] **Step 5: Write the four witnesses.**

All four: `TrackHistory = TRUE`, `SabotageStaleCache = TRUE`, everything else honest.

| cfg | extra deltas | `INVARIANT` |
| --- | --- | --- |
| `witness_changed` | `SecondMount = TRUE`, `EqualNamespaces = TRUE` | `W_ChangedThenBytes` |
| `witness_unknown` | — | `W_UnknownRefusal` |
| `witness_budgetunknown` | `ModelColdTable = TRUE` | `W_ColdRefused` |
| `witness_landeds2` | — | `W_LandedS2` |

`witness_changed` uses the second mount because the cross-mount route is the one row 17 exercises and the one that must be shown live. Each header states, in one sentence, which v11 witness it re-derives (or which theorem extension it de-vacuums) and why the state is not the same one.

- [ ] **Step 6: Run the whole battery and confirm 32/32.**

Final `CONFIGS` array — sabotages first, then greens, then witnesses:

```bash
CONFIGS=(
  "sab_typeprobe             violation  TypeOK"
  "sab_noapplypending        violation  ConfirmedRelinkNeverDangles"
  "sab_noapplypending_window violation  MarkerCoversDurableWindow"
  "sab_relaxedmarker         violation  MarkerSeenMatchesMarker"
  "sab_publishafterconfirm   violation  PromotedNeverDangles"
  "sab_nofence               violation  ConfirmedRelinkNeverDangles"
  "sab_nofence_changed       violation  ChangedImpliesFenced"
  "sab_nopoison              violation  ConfirmedRelinkNeverDangles"
  "sab_nowedge               violation  ConfirmedRelinkNeverDangles"
  "sab_norowexact            violation  ConfirmedRelinkNeverDangles"
  "sab_barevalidator         violation  ConfirmedRelinkNeverDangles"
  "sab_nodiskqualification   violation  ConfirmedRelinkNeverDangles"
  "sab_inferanswer           violation  ConfirmedRelinkNeverDangles"
  "sab_s2bytefetch           violation  NeverPublishedTwice"
  "v11_baseline              green      -"
  "ctl_v11nomarker           green      -"
  "sab_stalecache            green      -"
  "ctl_distinctns            green      -"
  "ctl_skipidentity          green      -"
  "v12_design_full           green      -"
  "v12_coldanswer            green      -"
  "witness_busylane          violation  W_BusyLaneProven"
  "witness_midtenure         violation  W_MidTenureProven"
  "witness_proven            violation  W_ProvenCommitted"
  "witness_delete            violation  W_BlobDeleted"
  "witness_corruptwindow     violation  W_CorruptWindow"
  "witness_retrycreated      violation  W_RetryCreatedAdopt"
  "witness_collisionreached  violation  W_CollisionReached"
  "witness_changed           violation  W_ChangedThenBytes"
  "witness_unknown           violation  W_UnknownRefusal"
  "witness_budgetunknown     violation  W_ColdRefused"
  "witness_landeds2          violation  W_LandedS2"
)
```

Run. Expected: **32 rows — 14 red, 7 green, 11 witness-red** — `ALL EXPECTATIONS MET`, exit 0.

- [ ] **Step 7: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkReofferCore_sab_nofence.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nofence_changed.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nopoison.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_nowedge.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_norowexact.cfg \
        docs/superpowers/models/CaRelinkReofferCore_sab_s2bytefetch.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v12_design_full.cfg \
        docs/superpowers/models/CaRelinkReofferCore_v12_coldanswer.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_changed.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_unknown.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_budgetunknown.cfg \
        docs/superpowers/models/CaRelinkReofferCore_witness_landeds2.cfg \
        docs/superpowers/models/run_relinkreoffer.sh
git commit -m "ca: tla — re-derivation against the new rule set: fence-first, authoritative No, four-state receiver, landed-S2 inside the theorems, NeverPublishedTwice"
```

---

### Task 4: The S7 ruling, the v11 continuity note, and the clean end-to-end run

**Files:**
- Modify: `docs/superpowers/models/CaRelinkReofferCore.tla` (the S7 verdict, written beside the two marker invariants)
- Modify: `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md` (**append only** — one new section)
- Read first: seam §6, seam §8 row S7, spec §5.1.2.
- **Read-only, must not change:** `CaRelinkConfirmCore.tla` and every `CaRelinkConfirmCore_*.cfg`.

**Interfaces:**
- Consumes: Task 1's `MarkerCoversDurableWindow`, `MarkerSeenMatchesMarker`, `_sab_noapplypending_window`, `_sab_relaxedmarker` and `_witness_retrycreated` results.
- Produces: the S7 verdict paragraph and the v11 cross-reference that Task 5's RESULTS file quotes.

---

- [ ] **Step 1: Settle the S7 ruling on the evidence, and record why the equivalence question is closed structurally rather than by trace comparison.**

The question: **is seam §8 row S7 — "a reader taking `state_mutex` between the arm and the install observes `ApplyPending`, never a stale `Clean`" — expressible as an assertion in this model, or is it code-level-only?**

The answer is settled by the two invariants and two configs the model already carries, and this step's job is to confirm each fact from its log rather than assume it:

1. **The INTERVAL half is asserted.** `MarkerCoversDurableWindow` is green in `v11_baseline`, `sab_stalecache`, `v12_design_full`, `v12_coldanswer`; `_sab_noapplypending_window` breaks it. Confirm both from their logs. Confirm also that it holds across every wedge arm that touches durability — the retry that CREATES (`WedgeRetryCreated` sets the owed apply while the marker is already `pending` and leaves it there), both steps of the stale resolution, and floor reconciliation. The greens are the evidence; those four actions are the ones that would break it if any had cleared the marker or the owed apply.
2. **The OBSERVATION half is asserted too.** `MarkerSeenMatchesMarker` is green in the same greens; `_sab_relaxedmarker` breaks it. Confirm both from their logs. This is the property the seam's mutex placement must DELIVER, stated over the reader-visible value the model carries explicitly.
3. **`_sab_relaxedmarker` is retained, and the reason it is not duplicate evidence is structural, not empirical.** An earlier draft of this plan proposed discarding it if its counterexample matched `_sab_noapplypending`'s. **That rule is rejected as unsound**, twice over, and must not be used: (a) the two configs cannot produce identical state sequences, because `sApply` moves to `"pending"` in one and stays `"clean"` in the other by construction; (b) even an identical shortest ACTION trace at these bounds would not establish projection equivalence, nor rule out divergence at greater depth — a shortest counterexample is a fact about one bound, not a theorem about the state spaces. The sound argument is the one available for free: **the two configs are asserted against DIFFERENT invariants**, so neither can stand in for the other, and no comparison is needed.
4. **What remains code-level-only is the MECHANISM, not the property.** No untimed model can check a memory model. Seam §6's mutual-exclusion argument (arm under `state_mutex`, read under `state_mutex`, lock at the CALL SITE because `forceWedgeForTest` at `:1396` already holds it) plus seam §8 row S7's test are what establish that `sApplySeen = sApply` holds in the code. The model states the obligation; the seam discharges it.

- [ ] **Step 2: Write the verdict into the module, beside the two marker invariants.**

§5.1.2's own rule is that a gate whose justification sits in another document is a gate nobody re-checks, so the ruling lives in the module, not only in RESULTS. Insert after `MarkerSeenMatchesMarker`:

```tla
(* *** SEAM §8 ROW S7 -- THE RULING. ***
   S7 is: "a reader taking `state_mutex` between the arm and the install observes `ApplyPending`,
   never a stale `Clean`."  BOTH of its halves are asserted here, by two invariants and two reds:
     * INTERVAL   -- `MarkerCoversDurableWindow`, red under `_sab_noapplypending_window`.  This is
                     verbatim what §5.1.2 says this design REQUIRES of the seam fix, and it holds
                     across every wedge arm that touches durability, including the bounded retry
                     that CREATES.
     * OBSERVATION-- `MarkerSeenMatchesMarker`, red under `_sab_relaxedmarker`.  The reader-visible
                     value is a separate variable precisely so that the relaxed store's hazard is a
                     modelled state rather than a comment.
   The two configs are NOT duplicate evidence, and the argument is structural: they are asserted
   against DIFFERENT invariants, so neither can substitute for the other.  A TRACE-COMPARISON rule
   ("discard one if its counterexample matches the other's") was considered and REJECTED AS UNSOUND:
   the stored marker's value differs between them by construction, so identical state sequences are
   impossible, and identical shortest traces at one bound would prove nothing about the state spaces.
   WHAT STAYS CODE-LEVEL is the MECHANISM, not the property: no untimed model can check a memory
   model.  That `sApplySeen = sApply` actually holds in the code is established by seam §6 -- arm
   under `state_mutex`, read under `state_mutex`, lock at the CALL SITE, since `forceWedgeForTest`
   (`CasRefLedger.cpp:1396`) already holds it and a self-locking `armApplyPending` would deadlock
   there -- together with seam §8 row S7's test.  The model states the obligation; the seam
   discharges it.  If the seam's lock placement is ever weakened, `MarkerSeenMatchesMarker` is the
   named thing it breaks. *)
```

- [ ] **Step 3: Append the continuity section to `CaRelinkConfirmCore_RESULTS.md`.**

Append one section, `## The v12 refinement, and why this file's _sab_stalecache stays RED {#v12-refinement}`, carrying exactly these six facts:

1. `CaRelinkConfirmCore.tla` and its twelve configs are UNCHANGED and will stay unchanged — §12's disposition: the model is the historical witness of the v11 protocol, and rewriting it would destroy the record that v11's rules were each load-bearing.
2. The redesign's model is `CaRelinkReofferCore.tla`; its results live in `2026-07-29-relink-seam-tla-RESULTS.md`.
3. **The flip, as a side-by-side line, because it is the whole evidence:** `CaRelinkConfirmCore_sab_stalecache` = **RED** (`ConfirmedRelinkNeverDangles` violated) is the v11 record; `CaRelinkReofferCore_sab_stalecache` = **GREEN** is the v12 result. The difference between the two runs is one thing — the second model represents the apply-pending marker: armed strictly before the durable PUT, published to a `state_mutex` reader, cleared atomically with the install, retained across an unresolved PUT, retained across the bounded retry that creates, and cleared on exactly the one wedge arm where `CasRefLedger.cpp` clears it.
4. `_sab_holeylist` keeps its meaning unchanged: the historical witness of BACKLOG `{#list-as-journal-dataloss-2026-07-25}`. §12.1 reassigns dangle-freedom's listing half to the v9 chain models, which is why `CaRelinkReofferCore` has no `MaxHoles` dial and names `CommittedEdgesAreGcVisible` instead.
5. What the refinement found that the v11 model could not have: v11 had no marker variable, so it could represent none of `resolveWedgeOnce`'s twelve paths. Three are worth naming here because each is a state v11 was silent about — the `Unresolved` retention (`:3134-3145`), the single clean-marker exception (`:1927-1934`) with its separate fence-closing reaction (`:2050-2060`), and the two-step stale resolution (`:1936-1950` then `:1758-1766`, where the wedge is retired without installing and `Poisoned` survives). `_sab_nowedge`'s counterexample runs through the second of them.
6. And the one that is a HAPPY path rather than a hazard: the bounded retry whose conditional create LANDS (`CasRefLedger.h:784-801` — *"a create either lands our exact bytes (the transaction is durable — and it is the SAME transaction, byte for byte) or conflicts"*). v11 could not represent a wedge being resolved at all, so every state downstream of a resolved wedge was absent from its state space. `CaRelinkReofferCore_witness_retrycreated` is what proves that path reachable here.

- [ ] **Step 4: Verify the v11 family is untouched.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git status --short docs/superpowers/models/CaRelinkConfirmCore.tla docs/superpowers/models/CaRelinkConfirmCore_*.cfg
```
Expected: **no output at all.** Any modification listed here is a Global Constraints violation — revert it before continuing. The only file of that family this plan may touch is `CaRelinkConfirmCore_RESULTS.md`, which is not in the pattern above and is therefore checked by its absence from it.

- [ ] **Step 5: Re-run the whole battery under a fresh `RUN_ID`.**

Per-run metadirs make the state clean by construction, so there is no wildcard to delete:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models
RUN_ID="gate-final-$(date +%Y%m%dT%H%M%S)" bash ./run_relinkreoffer.sh \
  | tee ../../../tmp/relinkreoffer_battery_final.txt
```
Expected: **32 rows**, `ALL EXPECTATIONS MET`, exit 0. The first line prints the `RUN_ID` and the log directory — **record both; Task 5 cites that directory per row.** Keep `tmp/relinkreoffer_battery_final.txt` verbatim.

Then confirm the v11 battery still behaves as recorded, since the flip is a claim about a PAIR of runs:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models
./run_relinkconfirm.sh CaRelinkConfirmCore_sab_stalecache
./run_relinkconfirm.sh CaRelinkConfirmCore_main
```
Expected: the first reports `Invariant ConfirmedRelinkNeverDangles is violated`; the second reports `Model checking completed. No error has been found.` Record both output lines verbatim — they are the left-hand column of the flip table.

- [ ] **Step 6: Commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add docs/superpowers/models/CaRelinkReofferCore.tla \
        docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md
git commit -m "ca: tla — seam S7 ruling (both halves asserted; only the mechanism stays code-level) + v11 continuity note"
```

---

### Task 5: The RESULTS document and the gate verdict

**Files:**
- Create: `docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md`
- Read first: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` (front matter, `## Gate` section shape, and the reds-breakdown paragraph are the template), `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md`.

**Interfaces:**
- Consumes: Task 4 step 5's `RUN_ID` and log directory, `tmp/relinkreoffer_battery_final.txt`, the two v11 verdict lines, and Task 4's S7 ruling.
- Produces: the greppable verdict line the relink implementation plan's Task 0 checks before any C++ is written.

---

- [ ] **Step 1: Write the front matter and the verdict line.**

Front matter in the house shape: `description`, `sidebar_label: 'Relink/seam TLA+ gate'`, `sidebar_position: 3`, `slug: /superpowers/models/2026-07-29-relink-seam-tla-results`, `title`, `doc_type: 'reference'`.

First section, the verdict — **one line, greppable, nothing else on it**:

```markdown
## Gate {#gate}

> **`RELINK TLA GATE: PASS`**
```

`PASS` **only if** every green is green and every red is red against the invariant NAME it was required to break. Anything else is `RELINK TLA GATE: FAIL` with the failing config named on the next line.

- [ ] **Step 2: Write the consequence block, directly under the verdict.**

Verbatim:

```markdown
**What this verdict decides.** Spec §12.3 step 3 and §1 gate 2:

> If the refined `_sab_stalecache` does not pass, the design is wrong and must not be implemented.

- **PASS** means the two terms this design deletes from `CasRefLedger::confirmExactRef` rule 3 —
  `!rt.pending.empty()` and `rt.leader_active` — are REDUNDANT with the apply-pending marker, and
  deleting them is safe **conditionally on the marker being made synchronized first**
  (`2026-07-29-cas-part-write-release-seam.md` §6). The marker fix is a PREREQUISITE, not a
  companion, and the battery says so twice: `_sab_noapplypending` is red (no marker at all) and
  `_sab_relaxedmarker` is red (a marker whose store is not published to a `state_mutex` reader).
  Shipping the deletion against today's relaxed, unlocked arm is the second of those configs. The
  order is: seam §6 lands, then the relink deletion.
- **FAIL** means **DO NOT IMPLEMENT** `2026-07-29-cas-relink-reoffer-redesign.md`. No C++ of that
  design is written, no `confirmExactRef` rule is deleted, and the spec returns to design with the
  counterexample below as its input. The seam document is unaffected either way — it is
  relink-independent and stands on its own (seam §intro).
```

- [ ] **Step 3: Write the flip section — the single most important table in the file.**

```markdown
## The flip {#the-flip}

| model | cfg | rule 3's lane-quiescence terms | apply-pending marker | verdict |
|---|---|---|---|---|
| `CaRelinkConfirmCore` (v11, unchanged) | `_sab_stalecache` | dropped | **not represented** | **RED** — `ConfirmedRelinkNeverDangles` |
| `CaRelinkReofferCore` (v12) | `_v11_baseline` | present | armed + published | green |
| `CaRelinkReofferCore` (v12) | `_ctl_v11nomarker` | present | **not armed** | green |
| `CaRelinkReofferCore` (v12) | **`_sab_stalecache`** | **dropped** | **armed + published** | **GREEN — THE FLIP** |
| `CaRelinkReofferCore` (v12) | `_sab_noapplypending` | dropped | **not armed** | **RED** — `ConfirmedRelinkNeverDangles` |
| `CaRelinkReofferCore` (v12) | `_sab_relaxedmarker` | dropped | armed, **not published** | **RED** — `MarkerSeenMatchesMarker` |
```

Then three paragraphs, each with its evidence rather than its claim:

1. **What the matrix establishes.** Each guard is individually sufficient (rows 2 and 3), at least one is necessary (row 5), and the v12 substitution is a real exchange rather than a coincidence. Row 6 is what makes "synchronized" a load-bearing word instead of an adjective, and it is asserted against a different invariant from row 5, which is why the two are not one experiment run twice.
2. **Why the green is not vacuous.** Quote the violations and depths of `_witness_busylane`, `_witness_midtenure` and `_witness_retrycreated`: without the first, the busy-lane `proven` is unreachable and the green is an artefact of an empty state; without the second, the green is the one-transaction-per-tenure artefact §12.5 ii warns about; without the third, no wedge is ever resolved and every state downstream of a resolved wedge is missing from the state space. Name the mechanisms that made the second and third reachable — `SenderAdmit`'s `"unrelated"` chunk kind, and `WedgeRetryCreated` — and say plainly that earlier drafts of the model had neither.
3. **The counterexample.** Paste `_sab_noapplypending`'s trace from `$RUNDIR/tlc_sab_noapplypending.log`, annotated action by action, and state its depth.

- [ ] **Step 4: Write the full battery table and the reds breakdown.**

One row per config: `cfg | expected | observed | invariant | states (gen/distinct) | depth | seconds | log`. **Thirty-two rows**, from `tmp/relinkreoffer_battery_final.txt` and each `$RUNDIR/tlc_<cfg>.log` — real TLC numbers, never estimates. **The log column cites the per-run path under `tmp/tlc-runs/relinkreoffer/$RUN_ID/`, never the convenience symlink**, so every row can be re-read after later runs. Name the `RUN_ID` once above the table.

Paste the runner's own table verbatim in a fenced block beneath it, including its `ALL EXPECTATIONS MET` line, and state the bounds actually used (`MaxId`, `MaxRound`, `MaxChunks`) plus any config where they had to be shrunk and why.

Follow the v9 phase file's honesty convention — the totals are **32 configs: 7 green, 25 red** — and break the reds down by CLASS instead of lumping them, because "25 reds" on its own says nothing about what any of them proves:

```markdown
- **11 sabotage-class** — one load-bearing rule removed per config: `_sab_noapplypending_window`,
  `_sab_publishafterconfirm`, `_sab_nofence`, `_sab_nofence_changed`, `_sab_nopoison`,
  `_sab_nowedge`, `_sab_norowexact`, `_sab_barevalidator`, `_sab_nodiskqualification`,
  `_sab_inferanswer`, `_sab_s2bytefetch`.
- **1 type negative control** — `_sab_typeprobe`, which is what makes every green's `TypeOK` a
  checked property rather than a listed one. It exists because "a green is only evidence once the
  property has been seen red" applies to type invariants too, and the alternative was an exemption
  clause.
- **2 marker-shape reds** — `_sab_noapplypending` (the marker is ABSENT) and `_sab_relaxedmarker`
  (the marker is PRESENT but unpublished to a `state_mutex` reader). **Asserted against different
  invariants** — `ConfirmedRelinkNeverDangles` and `MarkerSeenMatchesMarker` — which is why neither
  substitutes for the other and why no trace comparison between them is needed or sound.
- **11 reachability witnesses** — negated reachability, where the violation IS the evidence. Three of
  them are gate-critical (`_witness_busylane`, `_witness_midtenure`, `_witness_retrycreated`): a
  green on any of those is treated as a red `_sab_stalecache`, because each would mean the flip's
  green rests on a state space missing the very states the flip is about.
```

- [ ] **Step 5: Write the four obligation sections, one per §12 clause.**

- **§12.3 — the required refinement.** The 2×2 plus the two marker-shape reds; cross-reference §the-flip.
- **§12.5 i — cross-mount collision.** `_sab_barevalidator` RED, `_sab_nodiskqualification` RED, `_ctl_distinctns` GREEN, `_witness_collisionreached` reachable. State the control's meaning in one sentence: the red next door is caused by the EQUAL-namespace configuration, which is what §12.5 i's "specifically" demands, and a model that ran only distinct namespaces would pass while the wire is unsafe.
- **§12.5 ii — chunk-boundary tenure.** `MaxChunks = 2`, the `"tracked"`/`"unrelated"` chunk kinds, `_witness_midtenure` reachable at `proven /\ sLeader /\ sTenureChunks >= 1`, and the sentence that matters: without it, `_sab_stalecache`'s green would be an artefact of one transaction per tenure. Record that `sTenureChunks` resets on every tenure close — including the poison and both unresolved arms — so the witness proves *same tenure* rather than *two chunks somewhere*.
- **§12.4 — re-derivation.** Fence hoisted first (`ChangedImpliesFenced`, `_sab_nofence_changed` RED); `No` given its own successor (`RChangedThenBytes`) and shown unable to publish twice (`NeverPublishedTwice`, `_sab_s2bytefetch` RED); the answer/identity pair decoupled (`_sab_inferanswer` RED, `_ctl_skipidentity` GREEN-as-result); the landed-`Unresolved` publication brought inside both theorems via `rCommitted` (`_witness_landeds2` reachable); the two v11 witnesses re-derived (`_witness_changed`, `_witness_unknown`) with one line each on why the state is no longer the same one; rule 2 revisited (`_v12_coldanswer` green, `_witness_budgetunknown` reachable); the publish-then-confirm order still load-bearing (`_sab_publishafterconfirm` RED, and one line on why it sits in Task 1 rather than Task 3).

- [ ] **Step 6: Write the wedge-fidelity section — the mapping, as a result.**

This section exists because three review rounds turned on it, and because the mapping is the artefact that makes the rest of the file trustworthy.

Reproduce the module's twelve-row mapping table, with the counts that make it exhaustive by construction stated above it: **six** `WedgeResolution` values, **three** `SlotOccupyResult::Kind` values, **four** occupant classes, **seven** `Reason` values, **two** exception exits. Then three short subsections:

- **Rows that map to no transition, and why** — rows 1, 3, 4, 5 and 6. Each is `UNCHANGED vars`, which `NoOp` supplies; the two exception exits (the pre-I/O decode/apply throw, and the `slotOccupy` catch) are called out separately from `Reason::RefusedPreAttempt`, because that reason comes from an `Unresolved` RESULT and the throw path never sets a reason at all.
- **The recorded discrepancy.** A review round paraphrased row 12 as *"a retry that finds DIFFERENT bytes is the stale/poison arm."* The code adjudicates different bytes through `classifyRefLogOccupant` into row 7 (`SuccessorSeal` ⇒ `Rejected`) or row 8 (`Foreign` ⇒ `Corrupted`); the stale arm keys on the TABLE having advanced. The model follows the code; row 12 covers what the sentence probably meant (`Created`, then a refused adoption). Recorded per the house rule that where a review's paraphrase and the code disagree, the code wins and the discrepancy is reported — **and the review confirmed the correction on the record.**
- **The over-approximation.** Row 9's own comment concedes *"One leader per table makes this unreachable today; it is checked, not assumed."* Nothing in the model advances the table during a resolution either. The model admits row 9 as reachable anyway, because the code RETAINS the guard and a safety gate should cover the guards the code retains. Admitting a state the code believes unreachable adds states, which is the safe side for a gate; the opposite would be an under-approximation and a defect.

- [ ] **Step 7: Write the assumptions and seam sections.**

- **The three named assumptions**, each with its discharge mechanism and the sentence that makes weakening it visible: `CommittedEdgesAreGcVisible` (v9 chain models — name the exact configs); `UnresolvedPromoteNeverBytes` (spec §11 row 9, with `_sab_s2bytefetch` as the in-model necessity half); `FreshCertifiedResponse` (rows 15a/15b for the offer-confusion half, which IS partly modelled via `OfferIdentity` and `_sab_inferanswer`; row 16 for the replay half, which is not). State plainly that if any of those rows is weakened, the assumption goes with it — which is the point of naming them.
- **Seam §8 row S7**, with Task 4's ruling: **both halves are asserted** — `MarkerCoversDurableWindow` for the interval (red under `_sab_noapplypending_window`) and `MarkerSeenMatchesMarker` for the observation (red under `_sab_relaxedmarker`) — and **only the MECHANISM stays code-level**, because no untimed model can check a memory model; that `sApplySeen = sApply` holds in the code is established by seam §6's mutex placement and seam §8 row S7's test. Record that the trace-comparison rule an earlier draft proposed is **rejected as unsound**, with its one-line reason: the stored marker's value differs between the two configs by construction, so identical state sequences are impossible, and identical shortest traces at one bound would prove nothing about the state spaces.
- **What the seam contributes and what it does not:** §3's emission point and §4's `attempted` mark are accounting with no safety content to gate (seam §3.3, §9 point 5), discharged by seam §8 rows S1–S6c and relink rows 19–20.

- [ ] **Step 8: Write the structural-exclusions section.**

This section exists because the Global Constraints require a guard that makes a state unrepresentable to be *recorded* rather than asserted — otherwise a reader finds a guard with no evidence beside it and cannot tell whether it was checked or forgotten.

```markdown
## Structural exclusions — guards, not invariants {#structural-exclusions}

Three hazards are excluded by an action guard rather than by an invariant. Each is listed with the
code fact that licenses it and with the reason an invariant would have been worse than useless: an
assertion over a state the transition relation cannot enter is green for free, which is the opposite
of evidence.

| Excluded state | Guard | Code fact |
|---|---|---|
| A floor-covered wedge is re-applied | `WedgeResolveInstall`'s `~sFloorCovers[m]` | `durableFloorCovers` is tested FIRST (`CasRefLedger.cpp:1758`), before a candidate is built, so a floor-covered wedge can only take mapping row 2. Re-applying an id AT the floor is what `applyTxnInPlace` refuses as non-contiguous — "every later flush would throw in the same place and the lane would stay wedged for the runtime's life" |
| A poisoned cache is advanced to clean | `WedgeResolveInstall`'s `sApply[m] # "poisoned"` | a `Poisoned` table is by definition missing a durable transaction; only a recovery may re-derive it (`:1758-1766`'s log line). A draft of this model guarded the action on `sWedge /\ sApplyOwed` alone, which left this transition enabled immediately after `WedgeResolveStale` |
| A durable byte with no prior arm | `SenderDurable`'s `sArmed[m]` | seam §6 / `:2808`: the arm is "the last statement that still runs while nothing of this transaction can possibly be durable". Making the ordering a guard rather than an invariant is what lets `SabotageNoApplyPending` remove the marker's VALUE without removing the ORDERING, which is what "drop the marker" means |

Five arms of `resolveWedgeOnce` are likewise modelled as NON-transitions, and for the same kind of
reason: mapping rows 1 (`NoWedge`), 3 (the pre-I/O decode/apply throw), 4 (the `slotOccupy` catch),
5 (fence moved / superseded / wedge replaced) and 6 (`Unresolved`) each change no state at all —
`UNCHANGED vars`, which `NoOp` already supplies. They are delay, not state change. The full
outcome→action mapping is in the module, above the wedge actions, and is reproduced in
[the wedge-fidelity section](#wedge-fidelity).
```

- [ ] **Step 9: Write the "what this model dropped, and what it found" section.**

Two subsections. The second is the one that earns its place: it is the record that stops a fourth draft repeating the first three.

**Dropped from v11, deliberately:** the `MaxHoles` / `NsNoise` holey-list machinery, per §12.1's reassignment, with `_sab_holeylist` named as where that finding still lives and `CommittedEdgesAreGcVisible` named as what replaced it here.

**Found by the refinement, and not findable in v11.** The v11 model had no marker variable, so it could represent none of `resolveWedgeOnce`'s twelve paths — neither its hazards nor its happy path. **All three defects that successive drafts of THIS model carried are recorded, because each was caught by review rather than by the model, and a record of them is the only thing that makes the fourth draft's fidelity a claim rather than a hope:**

1. **A draft cleared the marker on the unresolved PUT.** `:3134-3145` says the opposite in its own words — the marker deliberately STAYS `ApplyPending`, because an `Unresolved` outcome *is* "an object that may be durable and is not applied". The consequence was worse than a wrong transition: it made `_sab_nowedge` red for a fenced, clean-marker, stale-cache wedge that the code cannot reach, i.e. a sabotage that read as evidence while testing nothing.
2. **A later draft omitted `FloorReconcile` entirely and left `WedgeResolveInstall` enabled on `sWedge /\ sApplyOwed`**, admitting a poisoned-to-clean install immediately after `WedgeResolveStale` — an impossible transition presented as a reachable one. Both drafts also carried a blanket `~sWedge` exclusion in `MarkerCoversDurableWindow`, which was hiding (1) rather than describing the code. Once the invariant's subject became this mount's own owed apply, **no exclusion is needed at all** — not for the wedge and not for the fence — and it holds through both steps of the stale resolution.
3. **A third draft had no `WedgeRetryCreated`, so the wedge protocol's own HAPPY PATH was unreachable.** `CasRefLedger.h:784-801` is explicit that a conditional create *"either lands our exact bytes (the transaction is durable — and it is the SAME transaction, byte for byte) or conflicts"*, so the bounded retry is itself a durability-producing event; without it, `WedgeResolveInstall`'s `sApplyOwed` guard could never be satisfied from a not-landed wedge, no wedge was ever resolved, and every state downstream of a resolved wedge was missing from the state space. That is the same vacuity trap as an unreachable hazard, one layer down: `_sab_stalecache`'s green would have been green for an incomplete reason. `_witness_retrycreated` is the guard against its recurrence, and it is gate-critical for exactly that reason.

Close the subsection with the process note, because it is the transferable part: **all three were found by enumerating the code's outcome space against its type declarations, not by re-reading the model.** The mapping block exists so that the enumeration is a permanent artefact rather than one reviewer's afternoon.

- [ ] **Step 10: Verify the verdict line is unique, then commit.**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -c "RELINK TLA GATE:" docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md
```
Expected: `1`. More than one means the string appears in prose too — rephrase the prose; the verdict line stays unique so a grep can never return an ambiguous answer.

```bash
git add docs/superpowers/models/2026-07-29-relink-seam-tla-RESULTS.md
git commit -m "ca: tla — relink/seam gate RESULTS: RELINK TLA GATE verdict + the do-not-implement consequence"
```

---

## Self-review notes (done at write time; revised after codex rounds 1, 2 and 3)

**Round-3 findings and where each is now addressed.**

| # | Finding | Resolution |
| --- | --- | --- |
| B1(a) | The `Created` adoption arm was claimed in the mapping but UNREPRESENTABLE: `SenderUnresolvedNotLanded` leaves `sApplyOwed = FALSE` and moves neither the durable binding nor the journal, while `WedgeResolveInstall` requires `sApplyOwed = TRUE`, so the wedge protocol's happy path could never run and some stale-cache states were unreachable | **NEW action `WedgeRetryCreated`** (mapping row 11), licensed by `CasRefLedger.h:784-801`: from a wedge whose earlier attempt did not land, the bounded retry's conditional create makes the wedged transaction durable — journal append, `sDurableRef` move, `sApplyOwed := TRUE` — with the wedge KEPT and the marker still `pending`. Only then may `WedgeResolveInstall` adopt (row 11) or `WedgeResolveStale` refuse (row 12), so the retry-created STALE outcome needs no further action. It is a SEPARATE step from the install because the occupant classification runs off the lock (`:1832-1836`), so the durable-but-unapplied window is observable and modelling it atomically would hide it. `SenderUnresolvedLanded` was relaxed to cover both chunk kinds so the retry path is not exercised for only half the item classes. **NEW witness `W_RetryCreatedAdopt` / `_witness_retrycreated`**, gate-critical: a green there means no wedge is ever resolved. Config count 31 → 32. |
| B1(b) | The inert `StillWedged` return from the `slotOccupy` EXCEPTION handler was absent from the mapping, and is distinct from `Reason::RefusedPreAttempt` | Mapping row 4, with the distinction stated: the throw path never reaches the recheck block and never sets `reason` at all, whereas `RefusedPreAttempt` comes from an `Unresolved` RESULT at `:1900-1908`. The pre-I/O decode/apply throw is row 3, also new, also a no-transition. |
| stopping rule | "Enumerate `resolveWedgeOnce`'s outcomes EXHAUSTIVELY from the code, arm by arm, before writing a line of TLA+" | Task 1 step 1 is now that enumeration, and it is **exhaustive by construction rather than by inspection**: bounded by six `WedgeResolution` values (`CasRefLedger.h:759-771`), three `SlotOccupyResult::Kind` values (`CasRequestControl.h:399`), four occupant classes, seven `Reason` values (`:1726-1734`) and two exception exits ⇒ twelve paths. The table is the module's mapping block verbatim, Task 1 step 12 audit B checks it against those declarations rather than against memory, and RESULTS reproduces it. |

**Round-2 findings, all confirmed closed by round 3 and unchanged in v4:** B1's retention and corruption-window halves (unresolved arms preserve `pending`; corruption clears the marker and retains the wedge; the fence reaction is separate; the invariant needs no exclusion), the MAJOR (`_sab_publishafterconfirm` moved into Task 1, so no green rests on an invariant whose red does not yet exist), and the three minors (Task 2's mount-setting prose, the `(GFold → GSettle)×3` alternation, the mount-action count — now **nineteen** with `WedgeRetryCreated`).

**Round-1 findings, all confirmed closed:** B2 (chunk kinds, per-tenure counting, strengthened witness — verified by hand-traced minimal trace), M3 (`rCommitted` covering landed S2), M4 (answer/identity decoupled; `_sab_inferanswer` red, `_ctl_skipidentity` green-as-result), M5 (`sApplySeen`, `MarkerSeenMatchesMarker`, `_sab_relaxedmarker` retained on a structural argument), m6 (`TypeOK` complete + `_sab_typeprobe`), m7 (`RUN_ID`, per-run metadirs and logs, `flock`, per-run paths in RESULTS).

**Deliberately byte-stable in v4.** The 22-constant block, the base cfg block, the validator and answer/identity section, the receiver and GC blocks, the runner's isolation and marker code, Task 2 in full, and Task 3's actions and invariants are unchanged from the versions review verified. The only edits to previously-verified text are: `sawRetryCreatedAdopt` threaded through `Init`, `histVars`, `TypeOK` and every action's `UNCHANGED`; `WedgeRetryCreated` in `Next`; `SenderUnresolvedLanded`'s chunk-kind `IF`; the config counts (14 / 20 / 32 running totals, and Task 5's 7 green / 25 red breakdown); and the three trace-checking notes in Task 3 step 3 that name which actions a red must run through.

**Spec coverage.** §12.1 (listing reassigned — no `MaxHoles` dial; `CommittedEdgesAreGcVisible` named instead). §12.2 (that assumption named with discharge). §12.3 (the 2×2 plus the two marker-shape reds — the gate). §12.4 (fence-first, authoritative `No`, no double publish, landed-S2 coverage, publish-then-confirm order, witnesses re-derived, rule 2 revisited). §12.5 i (equal-namespace/different-disk mounts, both named reds, plus the control the "specifically" clause implies). §12.5 ii (chunk kinds, per-tenure counting, strengthened witness). §12.5 iii (`UnresolvedPromoteNeverBytes` named, with `_sab_s2bytefetch` as the necessity half). §12's `FreshCertifiedResponse` (named, with its modelled and unmodelled halves stated). Seam §8 S7 (both halves asserted; only the mechanism code-level). Seam §3/§4 (out of scope with the reason). §12's disposition ruling (v11 read-only, enforced by a `git status` check).

**Three places this plan goes beyond the spec, deliberately, and says so.** (1) `_ctl_v11nomarker`, `_ctl_distinctns` and `_ctl_skipidentity` — controls the spec does not name, because a flip with no control is a coincidence and a green with no scoping statement is a silent gap. (2) `_sab_s2bytefetch` — the spec ASSUMES `UnresolvedPromoteNeverBytes`; this plan keeps the assumption assumed and models only its consequence. (3) The wedge lifecycle in full, hazards and happy path alike — §12 does not ask for it, but three drafts proved that modelling rule 3 without it produces sabotages that are red for unreachable states and greens that rest on missing ones.

**Placeholder scan.** Every cfg's full constants block appears once (Task 1 step 8); every later cfg is a delta with exact toggles and an exact `INVARIANTS` list. Every module fragment is real TLA+. Nothing is deferred: the S7 question is answered on evidence the battery already produces, and the one previously open decision rule was removed as unsound rather than left branching.

**Type consistency.** `sApply` and `sApplySeen` share the domain `ApplyStates`; the gate reads `sApplySeen`, the interval invariant reads `sApply`, and only `SenderArm` may separate them. `sApplyOwed` is the sole subject of `MarkerCoversDurableWindow` and is written by exactly five actions — `SenderDurable`, `SenderUnresolvedLanded` and `WedgeRetryCreated` set it; `SenderInstall` and `WedgeResolveInstall` clear it — plus `RecoverForAnswer`, the poisoned lane's only exit. `sFloorCovers` is written by exactly two (`WedgeResolveStale` sets it, `RecoverForAnswer` clears it) and read as a guard by five. `rCommitted` is written by exactly two (`RPromoteCommit`, `S2ResolveLanded`) and is the antecedent of both theorems. `rAccepted` — not `rAnswer = "proven"` — is the theorem's certificate antecedent, so a sabotage that makes the receiver accept the wrong thing cannot escape by never producing the word `proven`. `rAnswer`'s domain includes `Absent` for the offer-response arm, and `TypeOK` lists it. Every `rState` value used in Tasks 1 and 3 appears in `TypeOK`'s enumeration. All ten history flags appear in `histVars`, in `Init`, and in `TypeOK`.

### Critical Files for Implementation

- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaRelinkConfirmCore.tla
- /home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
- /home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-29-cas-relink-reoffer-redesign.md
- /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-29-cas-part-write-release-seam.md
