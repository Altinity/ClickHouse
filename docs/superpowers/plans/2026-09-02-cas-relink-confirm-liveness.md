---
description: 'Implementation plan for the CAS relink confirm liveness design (revision 8): ref-scoped rule 3 read from MutationScope, the carved mirror, scope validation, model and test updates, and the GCS live gate'
sidebar_label: 'CAS relink confirm liveness plan'
sidebar_position: 1
slug: /superpowers/plans/cas-relink-confirm-liveness
title: 'CAS relink confirm liveness implementation plan'
doc_type: 'plan'
---

# CAS relink confirm liveness implementation plan {#cas-relink-confirm-liveness-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `CasRefLedger::confirmExactRef` answer `Yes` for a ref no queued or in-flight lane mutation names, so two replicas replicating by relink under sustained write load stop starving each other (finding F11), without weakening the stale-row guarantee.

**Architecture:** Rule 3 of the confirm stops reading "is the lane busy" and reads "does a queued (`rt.pending`) or carved (`rt.carved`, a new mirror kept for the tenure) item carry a `MutationScope` covering this ref"; `Wedged`, `NeedsRecovery`, `Closed` and `Faulted` keep refusing table-wide. `MutationScope` becomes safety-bearing, so `flushRefBatch` validates each `Ref{name}` item's ops against its scope before anything is durable. The three TLA+ modules that encode the old table-wide contract are updated first, with sabotage and witness configurations proving the new rule is exactly as strong as needed; unit tests, a fake-GCS two-node liveness test and a ten-minute soak on the real GCS stand close the loop.

**Tech Stack:** C++ (ClickHouse, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`), gtest (`unit_tests_dbms`, gate filter `CAS*`), TLA+/TLC (`docs/superpowers/models`, `tmp/tla2tools.jar`), pytest integration tests via praktika, ca-soak (`utils/ca-soak`).

**Spec:** `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md` (revision 8). Read it before any task; every task below cites the section it implements.

## Global constraints {#global-constraints}

- Branch `cas-gc-rebuild`. No rebase, no amend, no push. Never `git add -A`; add the named files only. Never commit `utils/ca-soak/scenarios/BACKLOG.md` or `RUN_HISTORY.md` (foreign uncommitted edits).
- Every commit message ends with the two trailer lines the session uses (`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and the `Claude-Session:` line).
- Every code task gets a fresh-agent review (`ca-review`) of its diff before its commit; the subagent-driven skill provides it. A review finding labelled CODE/TEST is fixed before the commit; a PROSE finding may be batched into the task's docs step.
- TDD: write the test, watch it fail for the stated reason, then implement. A test that passes before the implementation is a fixture bug or a regression pin; the plan says which.
- Builds: `ninja -C build <target> > build/<log> 2>&1` with no `-j`; a subagent reads the log and reports the summary. Tests: output to a uniquely named log under `build/`; a subagent reads it.
- The unit-test binary is `build/src/unit_tests_dbms`. Check `grep CMAKE_BUILD_TYPE build/CMakeCache.txt` once: a `Debug`/sanitizer build runs the `*DeathTest` halves of the `LOGICAL_ERROR` tests, a release build runs the `EXPECT_THROW` halves.
- The `CAS*` gtest filter is the gate, exactly `CAS*`. A red anywhere in it is a stop, not a note.
- Do not use `sleep` in C++ to order threads; use the existing latches and hooks (`setRefPreCarveHookForTest`, `setCarveHookForTest`).
- No new field on `RefAppendAttempt`, no new argument on `appendRefOps`, no part-object flag, no wait, no timer (spec §carved, last paragraph).
- Comments carry the reason, never plan or backlog provenance. Documentation under `docs/` uses `{#anchors}` on every heading.
- Secrets: the real GCS HMAC pair lives only in the git-ignored `utils/ca-soak/configs/gcs.env` and `ci/local.env`. Never print, quote or copy anything that starts with `GOOG` from those files; the fake `GOOG1EFAKEACCESSKEYID` already checked into `tests/integration/test_cas_gcs/configs/config.xml` is not a secret.
- Before Task 3, read `tmp/gcs_live_20260902/codex_review_f11_spec_r8_report.md` if it exists (the codex re-review of revision 8). A REQUEST CHANGES verdict against the design stops execution and goes back to the spec; findings about tests or prose are folded into the matching task.

## File structure {#file-structure}

Modified:

- `docs/superpowers/models/CaRelinkConfirmCore.tla` — second admitted shape (`noop`), `SabotageTouchBlind`, ref-scoped rule 3, witness `W_YesWhilePendingNoop`.
- `docs/superpowers/models/CaRelinkConfirmCore_*.cfg` (13 files) — new constant; `run_relinkconfirm.sh` — two rows; `CaRelinkConfirmCore_RESULTS.md` — rule 3 sections.
- `docs/superpowers/models/CaRefLaneCore.tla`, `run_reflane.sh`, `CaRefLaneCore_RESULTS.md` — touch-scoped `Certify`, witness `W_CertifiedWhileOutstanding`.
- `docs/superpowers/models/CaRelinkLaneComposition.tla`, its four `.cfg`, `run_relinklane.sh` — `StartWrite(touch)`, `Confirmable`, invariant rename, witness `W_ConfirmedOutsideReady`.
- `docs/superpowers/models/README.md` — the confirm-core row and prose block, the lane-core and composition mentions of `Ready`-only certification.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` — `RefTableRuntime::carved`, `refCarvedForTest`, three comment sites.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` — carve and exit-guard bookkeeping, step-3 scope validation, rule 3, ProfileEvents and trace logging, comments.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` — `refCarvedForTest` forwarder.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h` — `MutationScope` comment.
- `src/Common/ProfileEvents.cpp` — four `CASRelinkConfirmRefused*` events.
- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` — header, helper, four rule-3 tests, six new tests.
- `src/Disks/tests/gtest_cas_ref_chunked_flush.cpp` — helper and the six mis-scoped items.
- `tests/integration/test_cas_gcs/gcs_mocks/server.py` — `/_control/delay`.
- `docs/superpowers/cas/BACKLOG/gcs.md`, `docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md` — status.

Created:

- `docs/superpowers/models/CaRelinkConfirmCore_sab_touchblind.cfg`, `CaRelinkConfirmCore_witness_yespendingnoop.cfg`, `CaRefLaneCore_witness_certifyoutstanding.cfg`, `CaRelinkLaneComposition_witness_confirmedoutsideready.cfg`.
- `tests/integration/test_cas_gcs_relink_liveness/{__init__.py,test.py,configs/storage_conf.xml}` — the two-node fake-GCS liveness case (the spec names `test_cas_gcs`; the case lives in a sibling directory that reuses `test_cas_gcs/gcs_mocks`, because the existing module fixture is single-node and restarts its server twice; say so in the commit message so the reviewer sees the deviation).

Task order matters: Tasks 1 and 2 (models) need no binary; Tasks 3 and 4 rebuild only `unit_tests_dbms`; Task 5 runs its integration test RED against the server binary built before this plan (`build/programs/clickhouse`, still the old rule 3); Task 6 changes the rule, rebuilds both binaries and turns everything GREEN; Task 7 records; Task 8 is the live gate.

---

### Task 1: `CaRelinkConfirmCore.tla` learns the mutation's shape {#task-1}

Implements spec §model, first paragraph.

**Files:**
- Modify: `docs/superpowers/models/CaRelinkConfirmCore.tla`
- Modify: `docs/superpowers/models/CaRelinkConfirmCore_*.cfg` (all 13 existing)
- Create: `docs/superpowers/models/CaRelinkConfirmCore_sab_touchblind.cfg`, `docs/superpowers/models/CaRelinkConfirmCore_witness_yespendingnoop.cfg`
- Modify: `docs/superpowers/models/run_relinkconfirm.sh`, `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md`

**Interfaces:**
- Produces: constant `SabotageTouchBlind`, variables `sShape`, `sawYesWhilePendingNoop`, action `SenderAdmitNoop`, operator `sTouches`, invariant `W_YesWhilePendingNoop`. Task 2 and the RESULTS/README edits refer to these names.

- [ ] **Step 1: Write the two new configurations first (they are the failing tests of this task)**

`docs/superpowers/models/CaRelinkConfirmCore_sab_touchblind.cfg`:

```text
\* SABOTAGE: gate 1 rule 3 keeps refusing while a mutation is admitted, but is BLIND to whether that
\* mutation touches the queried ref -- every admitted mutation reads as harmless. A touching mutation
\* (a removal or repoint of the ref) then confirms off the stale row exactly as in _sab_stalecache.
\* TLC MUST report a ConfirmedRelinkNeverDangles counterexample.
SPECIFICATION Spec
CONSTANTS
    Receivers = {r1}
    MaxId = 5
    MaxRound = 5
    MaxHoles = 0
    SabotageNoGate1 = FALSE
    SabotageStaleCache = FALSE
    SabotageTouchBlind = TRUE
    SabotageNoPoison = FALSE
    SabotageNoFence = FALSE
    SabotagePublishAfterConfirm = FALSE
INVARIANTS
    ConfirmedRelinkNeverDangles
CHECK_DEADLOCK FALSE
```

`docs/superpowers/models/CaRelinkConfirmCore_witness_yespendingnoop.cfg`:

```text
\* WITNESS (negated reachability) -- THE LIVENESS THIS REVISION BUYS. A confirm answers *yes* while
\* a mutation of ANOTHER ref (shape "noop") is admitted and not yet applied. Under the old table-wide
\* rule 3 this state is unreachable, so a green _main alone could be the old behaviour. TLC MUST
\* report W_YesWhilePendingNoop VIOLATED.
SPECIFICATION Spec
CONSTANTS
    Receivers = {r1}
    MaxId = 5
    MaxRound = 5
    MaxHoles = 0
    SabotageNoGate1 = FALSE
    SabotageStaleCache = FALSE
    SabotageTouchBlind = FALSE
    SabotageNoPoison = FALSE
    SabotageNoFence = FALSE
    SabotagePublishAfterConfirm = FALSE
INVARIANT W_YesWhilePendingNoop
CHECK_DEADLOCK FALSE
```

Add the new constant to every existing configuration (the line goes right after `SabotageStaleCache = ...`):

```bash
cd docs/superpowers/models
for f in CaRelinkConfirmCore_{main,main2r,empty_receivers,sab_holeylist,sab_nofence,sab_nogate1,sab_nopoison,sab_publishafterconfirm,sab_stalecache,witness_confirmno,witness_confirmunknown,witness_confirmyes,witness_delete}.cfg; do
  sed -i 's/^\(\s*\)SabotageStaleCache = \(TRUE\|FALSE\)$/&\n\1SabotageTouchBlind = FALSE/' "$f"
done
grep -c 'SabotageTouchBlind' CaRelinkConfirmCore_*.cfg   # every file must print 1
```

Add two rows to `CONFIGS` in `run_relinkconfirm.sh`: after the `sab_stalecache` row,

```bash
    "sab_touchblind          violation  ConfirmedRelinkNeverDangles"
```

and after the `witness_delete` row,

```bash
    "witness_yespendingnoop  violation  W_YesWhilePendingNoop"
```

- [ ] **Step 2: Run the two new rows and watch them fail for the right reason**

```bash
docs/superpowers/models/run_relinkconfirm.sh sab_touchblind witness_yespendingnoop > build/tlc_relinkconfirm_task1_red.log 2>&1; tail -5 build/tlc_relinkconfirm_task1_red.log
```

Expected: both rows `FAIL` with result `error` (TLC cannot parse a configuration naming `SabotageTouchBlind` / `W_YesWhilePendingNoop` before the module defines them). Look into the per-run log under `build/tlc-runs/relinkconfirm/` to confirm the error names the unknown constant or invariant, not something else.

- [ ] **Step 3: Extend the module**

In `CaRelinkConfirmCore.tla`:

1. Header comment, the sabotage list: replace the `SabotageStaleCache` entry with

```text
     SabotageStaleCache         -> gate 1 rule 3 dropped entirely -> the confirm reads a committed row
                                   that lags a DURABLE removal or repoint of the queried ref.
     SabotageTouchBlind         -> gate 1 rule 3 kept but blind to the admitted mutation's SHAPE ->
                                   a touching mutation reads as a mutation of some other ref, and the
                                   stale row confirms exactly as under SabotageStaleCache.
```

and add one paragraph after the sabotage list:

```text
   RULE 3 IS REF-SCOPED.  The sender's lane admits two SHAPES of mutation: "touching" (a removal or
   repoint of THE ref the receiver asks about -- the hazard) and "noop" (a mutation of another ref,
   recorded as `NsNoise`'s edge-neutral op, which leaves the queried binding alone -- the F11 load).
   Rule 3 refuses while a touching mutation is admitted and not yet applied, and ONLY then.  The
   model's `sPending` spans admission to apply, which is `pending` plus `carved` in the code; a
   wedged tenure is `sPoison`, refused by lane state. *)
```

2. `CONSTANTS`: after `SabotageStaleCache,` add

```text
    SabotageTouchBlind,           \* gate 1 rule 3: blind to whether the admitted mutation touches the ref
```

3. `VARIABLES`: after `sLeader,` add

```text
    sShape,         \* {"none","touching","noop"} -- what the admitted mutation does to the queried ref
```

and after `sawConfirmUnk` (make the previous line end with a comma) add

```text
    sawYesWhilePendingNoop  \* witness history: *yes* answered while a "noop" mutation was admitted
```

4. Tuples: `senderVars == << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sPoison, sFence, sShape >>` and `histVars == << sawConfirmNo, sawConfirmUnk, sawYesWhilePendingNoop >>`.

5. After `Indeg(b) == ...` add

```text
Shapes == {"none", "touching", "noop"}

(* Rule 3's predicate.  `SabotageTouchBlind` makes the confirm blind to the shape, so a touching
   mutation reads as harmless. *)
sTouches == sShape = "touching" /\ ~SabotageTouchBlind
```

6. `Init`: add `/\ sShape = "none"` after `sLeader = FALSE` and `/\ sawYesWhilePendingNoop = FALSE` at the end.

7. Replace `SenderAdmit`, `SenderDurable`, `SenderApply`, `SenderPoison` with:

```text
(* Admission of a mutation of THE ref: the op enters the queue and a leader tenure opens.  The
   scope is recorded here, under the same mutex as admission, before anything is sent. *)
SenderAdmit(nb) ==
    /\ sFence
    /\ ~sPending
    /\ ~sPoison
    /\ sDurableRef = Token
    /\ nextId <= MaxId
    /\ sPending' = TRUE
    /\ sLeader' = TRUE
    /\ sTarget' = nb
    /\ sShape' = "touching"
    /\ UNCHANGED << sDurableRef, sCacheRef, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* Admission of a mutation of ANOTHER ref (the F11 shape): its record is edge-neutral and it leaves
   the queried binding alone.  It needs no `sDurableRef = Token` guard -- the other ref's life is
   independent of ours. *)
SenderAdmitNoop ==
    /\ sFence
    /\ ~sPending
    /\ ~sPoison
    /\ nextId <= MaxId
    /\ sPending' = TRUE
    /\ sLeader' = TRUE
    /\ sShape' = "noop"
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* The conditional PUT is acked: the transaction is DURABLE and GC can fold it.  The in-memory
   committed row is NOT yet updated -- this is the post-durable-PUT window (§Problem 2).  A noop
   tenure writes NsNoise's edge-neutral record at most once per behaviour (same guard as NsNoise). *)
SenderDurable ==
    /\ sPending
    /\ nextId <= MaxId
    /\ nextId' = nextId + 1
    /\ IF sShape = "touching"
         THEN /\ sDurableRef = Token
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> NsS, blob |-> "b1", src |-> SenderEdge, op |-> "del"] }
              /\ sDurableRef' = sTarget
         ELSE /\ ~(\E rec \in journal : rec.ns = NsS /\ rec.op = "noop")
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> NsS, blob |-> "b1", src |-> NoiseSrc, op |-> "noop"] }
              /\ UNCHANGED sDurableRef
    /\ UNCHANGED << sCacheRef, sTarget, sPending, sLeader, sPoison, sFence, sShape >>
    /\ UNCHANGED << gcVars, recvVars, histVars >>

(* The in-memory apply succeeds; the tenure closes and the lane goes quiescent.  A touching tenure
   closes only after its transaction is durable; a noop tenure has nothing this ref can observe. *)
SenderApply ==
    /\ sPending
    /\ sShape = "noop" \/ sDurableRef # Token
    /\ sCacheRef' = sDurableRef
    /\ sPending' = FALSE
    /\ sLeader' = FALSE
    /\ sShape' = "none"
    /\ UNCHANGED << sDurableRef, sTarget, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* The in-memory apply THREW although the object is durable (allocation failure on the COW apply).
   The tenure closes -- the lane looks perfectly quiescent -- but the committed row is now
   permanently stale.  Only gate 1 rule 4 (poison) can see this. *)
SenderPoison ==
    /\ sPending
    /\ sShape = "touching"
    /\ sDurableRef # Token
    /\ sPoison' = TRUE
    /\ sPending' = FALSE
    /\ sLeader' = FALSE
    /\ sShape' = "none"
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>
```

8. `FenceLoss` and `ForeignRemove`: add `sShape` to their `UNCHANGED << sDurableRef, ... >>` lists.

9. `Gate1Answer`: replace the rule 3 line with

```text
    LET quiescent == SabotageStaleCache \/ ~(sPending /\ sTouches)     \* rule 3 (ref-scoped)
```

10. `RConfirm`: inside the `LET ans == Gate1Answer IN` block add

```text
         /\ sawYesWhilePendingNoop' = (sawYesWhilePendingNoop \/ (ans = "yes" /\ sPending /\ sShape = "noop"))
```

11. `Next`: add `\/ SenderAdmitNoop` on the line after the `SenderAdmit` disjunct.

12. `TypeOK`: add `/\ sShape \in Shapes` and `/\ sawYesWhilePendingNoop \in BOOLEAN`.

13. Witnesses: after `W_ConfirmUnknown` add

```text
(* THE LIVENESS THIS REVISION BUYS: a *yes* while a mutation of ANOTHER ref is admitted. *)
W_YesWhilePendingNoop == ~sawYesWhilePendingNoop
```

- [ ] **Step 4: Run the whole battery**

```bash
docs/superpowers/models/run_relinkconfirm.sh > build/tlc_relinkconfirm_task1.log 2>&1; grep -E 'PASS|FAIL|ALL EXPECTATIONS|SOME EXPECTATIONS' build/tlc_relinkconfirm_task1.log
```

Expected: all 15 rows `PASS` and `ALL EXPECTATIONS MET`. In particular `sab_stalecache` and `sab_touchblind` both report `violation:ConfirmedRelinkNeverDangles`, `main`, `main2r`, `empty_receivers` are `green`, `witness_yespendingnoop` reports `violation:W_YesWhilePendingNoop`. If `main` is red, the rule is wrong, not the model: stop and re-read spec §rule-3 before touching anything.

- [ ] **Step 5: Rewrite the RESULTS sections from the run**

In `CaRelinkConfirmCore_RESULTS.md`: (a) in the summary table of configurations near the top, add rows for `_sab_touchblind` (expected `ConfirmedRelinkNeverDangles is violated`) and `_witness_yespendingnoop` (expected `W_YesWhilePendingNoop is violated`) with the state counts from `build/tlc-runs/relinkconfirm/<RUN_ID>/tlc_CaRelinkConfirmCore_<name>.log`; (b) replace the `_sab_stalecache` section's first sentence and closing paragraph with

```markdown
`ConfirmedRelinkNeverDangles is violated`. Rule 3 is dropped, so the confirm reads the committed row
without checking whether an admitted mutation touches the ref.
```

```markdown
With rule 3 intact S11 answers **"unknown"**: the admitted mutation is `touching` and `sPending` spans
admission to apply, which is `pending` plus `carved` in the code. Rule 3 is written against the
mutation's *scope*, not against the tenure: `_witness_yespendingnoop` reaches a *yes* during a tenure
whose mutation does not touch the ref, and `_sab_touchblind` shows that ignoring the scope re-opens
this very trace.
```

(c) add a section `### _sab_touchblind — rule 3 must read the mutation's scope {#sab-touchblind}` describing the counterexample from the log (admit a touching mutation, durable, GC folds and deletes, publish, confirm *yes* because `sTouches` is blind, promote, violation) and a section `### _witness_yespendingnoop — the liveness gain is reachable {#witness-yespendingnoop}` with the witness trace (admit noop, publish, confirm *yes*). Keep the existing anchors and the LIST-completeness caveat unchanged.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/models/CaRelinkConfirmCore.tla docs/superpowers/models/CaRelinkConfirmCore_*.cfg docs/superpowers/models/run_relinkconfirm.sh docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md
git commit -m "ca-models: rule 3 of the relink confirm is ref-scoped in CaRelinkConfirmCore

A second admitted shape, noop (a mutation of another ref), lets the model
distinguish the F11 load from the hazard. Rule 3 refuses only while a
touching mutation is admitted; SabotageTouchBlind proves the scope is
load-bearing and W_YesWhilePendingNoop proves the liveness gain is
reachable. All 15 rows of run_relinkconfirm.sh meet their expectations.

Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

---

### Task 2: The lane models certify outside `Ready` when the outstanding mutation does not touch the identity {#task-2}

Implements spec §model, second paragraph.

**Files:**
- Modify: `docs/superpowers/models/CaRefLaneCore.tla`, `docs/superpowers/models/run_reflane.sh`, `docs/superpowers/models/CaRefLaneCore_RESULTS.md`
- Create: `docs/superpowers/models/CaRefLaneCore_witness_certifyoutstanding.cfg`
- Modify: `docs/superpowers/models/CaRelinkLaneComposition.tla`, `CaRelinkLaneComposition_sab_confirmblocked.cfg`, `CaRelinkLaneComposition_sab_deleteunowned.cfg`, `CaRelinkLaneComposition_sab_skipidentity.cfg`, `CaRelinkLaneComposition_safe.cfg`, `CaRelinkLaneComposition_witness_*.cfg` (5), `run_relinklane.sh`
- Create: `docs/superpowers/models/CaRelinkLaneComposition_witness_confirmedoutsideready.cfg`
- Modify: `docs/superpowers/models/README.md`

**Interfaces:**
- Produces: lane core operator `OutstandingTouches`, variable `saw_certified_while_outstanding`, invariant `W_CertifiedWhileOutstanding`; composition variables `outstanding_touches`, `saw_confirmed_outside_ready`, operator `Confirmable`, invariant `ConfirmationRequiresUntouchedIdentity` (renamed from `ConfirmationRequiresReady`), witness `W_ConfirmedOutsideReady`.

- [ ] **Step 1: Write the failing configurations**

`CaRefLaneCore_witness_certifyoutstanding.cfg` is `CaRefLaneCore_witness_commit.cfg` with the last line replaced by

```text
INVARIANTS TypeOK ReadyCaughtUp OutstandingRetainsExactAttempt NoAttemptOutsideOutstanding CertifiedViewIsCurrent W_CertifiedWhileOutstanding
```

`CaRelinkLaneComposition_witness_confirmedoutsideready.cfg` is `CaRelinkLaneComposition_witness_blockedrefusal.cfg` with `W_BlockedRefusal` replaced by `W_ConfirmedOutsideReady` and `ConfirmationRequiresReady` replaced by `ConfirmationRequiresUntouchedIdentity`.

In every other `CaRelinkLaneComposition_*.cfg` replace `ConfirmationRequiresReady` with `ConfirmationRequiresUntouchedIdentity`:

```bash
cd docs/superpowers/models && sed -i 's/ConfirmationRequiresReady/ConfirmationRequiresUntouchedIdentity/' CaRelinkLaneComposition_*.cfg && grep -c ConfirmationRequiresUntouchedIdentity CaRelinkLaneComposition_*.cfg
```

`run_reflane.sh`: add the row `"witness_certifyoutstanding violation W_CertifiedWhileOutstanding"` after `witness_rebirthresolver`. `run_relinklane.sh`: change the first row to `"sab_confirmblocked violation ConfirmationRequiresUntouchedIdentity"` and add `"witness_confirmedoutsideready violation W_ConfirmedOutsideReady"` after `witness_delete`.

- [ ] **Step 2: Watch both runners fail**

```bash
docs/superpowers/models/run_relinklane.sh > build/tlc_relinklane_task2_red.log 2>&1; tail -12 build/tlc_relinklane_task2_red.log
docs/superpowers/models/run_reflane.sh > build/tlc_reflane_task2_red.log 2>&1; grep -E 'certifyoutstanding|EXPECTATIONS' build/tlc_reflane_task2_red.log
```

Expected: every composition row is `error` (the renamed invariant does not exist yet) and `SOME EXPECTATIONS UNMET`; in the lane core only `witness_certifyoutstanding` is `error`, everything else still passes.

- [ ] **Step 3: Lane core**

In `CaRefLaneCore.tla`:

1. `VARIABLES`: add `saw_certified_while_outstanding` as a new last entry (turn the previous last entry's line into `..., ` form). Add it to the `vars` tuple in the same position.
2. `Init`: `/\ saw_certified_while_outstanding = FALSE`.
3. Immediately before `Certify ==` add

```text
(* The outstanding mutation changes the certified identity's binding.  A same-binding attempt models
   a mutation of ANOTHER ref: the lane is `Writing`, but this identity's row is exactly as
   authoritative as on an idle lane. *)
OutstandingTouches ==
    lane = "Writing" /\ attempt # NoAttempt /\ attempt.binding # cache_binding
```

4. Replace `Certify` with

```text
(* Certification (the relink confirm's gate 1 seen from the lane).  Allowed in `Ready`, and in
   `Writing` while the outstanding mutation does not touch the identity; refused in every other
   state.  `bad_certification` records a certification whose view is not current for THIS identity:
   the binding disagrees with the durable one, or the runtime is not the current one.  The id may
   legitimately lag in `Writing` (another ref's transaction is durable and not installed), so the
   currency check is on the binding, not on the id. *)
Certify ==
    /\ \/ (lane = "Ready" /\ (CurrentRuntime \/ SabotageNoFence))
       \/ (lane = "Writing" /\ CurrentRuntime /\ ~OutstandingTouches)
       \/ (SabotageCertifyBlocked /\ lane = "Writing" /\ OutstandingTouches)
    /\ bad_certification' =
        (bad_certification
         \/ ~(lane \in {"Ready", "Writing"}
              /\ CurrentRuntime
              /\ cache_binding = durable_binding))
    /\ saw_certified_while_outstanding' = (saw_certified_while_outstanding \/ lane = "Writing")
    /\ UNCHANGED << lane, cache_id, durable_id, cache_binding,
                    durable_binding, attempt, runtime_generation,
                    authority_generation, resolver_attempt,
                    resolver_generation, observation, bad_append, bad_install,
                    saw_commit, saw_unresolved, saw_retry_created, saw_durable_adoption,
                    saw_recovery, saw_stale_result, saw_closed, saw_faulted >>
```

5. Every other action's `UNCHANGED` list that names `saw_faulted` gets `saw_certified_while_outstanding` appended after it. Count before and after:

```bash
grep -c 'saw_faulted' docs/superpowers/models/CaRefLaneCore.tla
grep -c 'saw_certified_while_outstanding' docs/superpowers/models/CaRefLaneCore.tla
```

The second count must be the first count plus the definitions added above (VARIABLES, vars, Init, TypeOK, Certify's own update, the witness) minus one (Certify's UNCHANGED list does not carry it). TLC reports any action that forgot the variable as `Successor state is not completely specified`; treat that as a build error.

6. `TypeOK`: `/\ saw_certified_while_outstanding \in BOOLEAN`.
7. Witnesses: `W_CertifiedWhileOutstanding == ~saw_certified_while_outstanding` next to `W_Commit`.
8. Header comment: the `SabotageCertifyBlocked` line becomes "certify in `Writing` while the outstanding mutation touches the identity".

- [ ] **Step 4: Composition**

In `CaRelinkLaneComposition.tla`:

1. Header comment: replace "It may certify a source identity only while the lane is `Ready`;" with "It may certify a source identity while the lane is `Ready`, or while it is `Writing` and the outstanding mutation does not touch that identity (the touch is a nondeterministic parameter of `StartWrite`, since this model has no transaction content);".
2. `VARIABLES`: add `outstanding_touches,` after `lane,` and `saw_confirmed_outside_ready` after `saw_delete` (comma on the previous line). Add both to `vars`.
3. `Init`: `/\ outstanding_touches = FALSE` and `/\ saw_confirmed_outside_ready = FALSE`.
4. Replace `StartWrite`, `CommitWrite`, `WriteUnresolved`, `RequireRecovery`, `ConfirmSource`, `RefuseBlockedConfirmation`, `ConfirmWhileBlocked`:

```text
StartWrite(touch) ==
    /\ lane = "Ready"
    /\ lane' = "Writing"
    /\ outstanding_touches' = touch
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

CommitWrite ==
    /\ lane = "Writing"
    /\ lane' = "Ready"
    /\ outstanding_touches' = FALSE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

WriteUnresolved ==
    /\ lane = "Writing"
    /\ lane' = "Wedged"
    /\ outstanding_touches' = FALSE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

RequireRecovery ==
    /\ lane \in {"Writing", "Wedged"}
    /\ lane' = "NeedsRecovery"
    /\ outstanding_touches' = FALSE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

(* The lane certifies the identity: Ready, or Writing with an outstanding mutation that leaves the
   identity alone.  Wedged and the broken states never certify. *)
Confirmable == lane = "Ready" \/ (lane = "Writing" /\ ~outstanding_touches)

ConfirmSource ==
    /\ Confirmable
    /\ source_exists
    /\ confirmed_binding' = "blob"
    /\ saw_confirmation' = TRUE
    /\ saw_confirmed_outside_ready' = (saw_confirmed_outside_ready \/ lane = "Writing")
    /\ UNCHANGED << lane, outstanding_touches, source_exists, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_blocked_refusal,
                    saw_recovery, saw_promotion, saw_delete >>

RefuseBlockedConfirmation ==
    /\ ~Confirmable
    /\ saw_blocked_refusal' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, source_exists, confirmed_binding,
                    receiver_binding, promoted, bad_confirmation,
                    bad_promotion, saw_confirmation, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

ConfirmWhileBlocked ==
    /\ SabotageConfirmBlocked
    /\ ~Confirmable
    /\ source_exists
    /\ confirmed_binding' = "blob"
    /\ bad_confirmation' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, source_exists, receiver_binding, promoted,
                    bad_promotion, saw_confirmation, saw_blocked_refusal,
                    saw_recovery, saw_promotion, saw_delete, saw_confirmed_outside_ready >>
```

5. Every remaining action (`Recover`, `CloseLane`, `FaultLane`, `PromoteExactIdentity`, `PromoteDifferentIdentity`, `DeleteSource`, `DeleteBeforeOwnership`) gets `outstanding_touches` and `saw_confirmed_outside_ready` appended to its `UNCHANGED` list.
6. `Next`: replace `\/ StartWrite` with `\/ \E touch \in BOOLEAN : StartWrite(touch)`.
7. `TypeOK`: `/\ outstanding_touches \in BOOLEAN` and `/\ saw_confirmed_outside_ready \in BOOLEAN`.
8. Rename the invariant: `ConfirmationRequiresUntouchedIdentity == ~bad_confirmation`. Add `W_ConfirmedOutsideReady == ~saw_confirmed_outside_ready`.

- [ ] **Step 5: Run both batteries**

```bash
docs/superpowers/models/run_reflane.sh > build/tlc_reflane_task2.log 2>&1; grep -E 'FAIL|EXPECTATIONS' build/tlc_reflane_task2.log
docs/superpowers/models/run_relinklane.sh > build/tlc_relinklane_task2.log 2>&1; grep -E 'FAIL|EXPECTATIONS' build/tlc_relinklane_task2.log
```

Expected: no `FAIL` line, `ALL EXPECTATIONS MET` twice. `sab_certifyblocked` must still violate `CertifiedViewIsCurrent` (a touching write lands, then the sabotaged certify reads the stale binding) and `sab_nofence` too. If `safe` in the lane core goes red on `CertifiedViewIsCurrent`, read the trace before touching anything. Two causes are possible: the guard admits a touching attempt (a modelling slip in `OutstandingTouches`), or a resolver or recovery step moves `durable_binding` while the lane is `Writing` with a non-touching attempt outstanding. The second is not a slip: it is a hole in the design's argument (a durable change of the identity that no queued or carved item announces) and must be reported as a finding against the spec, not modelled away. Do not weaken the invariant in either case.

- [ ] **Step 6: RESULTS and README**

`CaRefLaneCore_RESULTS.md`:
- line 43: replace "and `Ready`-only certification" with "and touch-scoped certification (`Ready`, or `Writing` while the outstanding mutation does not touch the certified identity)".
- Relink composition section (`:68-91`): seam property 1 becomes "confirmation requires `Ready`, or `Writing` with an outstanding mutation that does not touch the identity (`ConfirmationRequiresUntouchedIdentity`);". Update the counts sentence from the new run ("All eleven expectations passed: one honest configuration, three named sabotage violations, and seven reachability witnesses") and the log paths. Rewrite the conclusion's last sentence to "The C++ implementation follows that model, and the relink seam depends only on the small certification contract: `Ready`, or `Writing` without an outstanding mutation of the certified identity."
- Add one paragraph to the lane-core section naming the new witness: "`witness_certifyoutstanding` reaches a certification in `Writing` with a same-binding attempt outstanding, so the honest run's green `CertifiedViewIsCurrent` covers the relaxed guard and not only `Ready`."

`README.md`:
- Row for `CaRelinkConfirmCore.tla` (`:141`): replace "lane quiescence" with "ref-scoped mutation refusal (the mutation's `MutationScope`)", and "each proven load-bearing" stays.
- Prose block (`:374-395`): replace "an admission queue, a leader tenure, an apply-pending poison state and a mount fence" with "an admission queue whose mutations carry a shape (touching the queried ref, or a mutation of another ref), a leader tenure, an apply-pending poison state and a mount fence"; replace "lane quiescence," in the sabotage list with "the ref-scoped refusal (dropping it, `_sab_stalecache`; keeping it but ignoring the mutation's shape, `_sab_touchblind`),"; replace "Four `_witness_*` configs" with "Five `_witness_*` configs" and add ", and that a confirm can answer *yes* while a mutation of another ref is in flight (`_witness_yespendingnoop`, the liveness the 2026-09-02 revision bought)".
- Run `grep -n -E 'Ready.{0,40}certif|certif.{0,40}Ready|lane quiescence|ConfirmationRequiresReady' docs/superpowers/models/README.md docs/superpowers/models/*_RESULTS.md` and fix every remaining hit the same way (a lane-core or composition row that says certification requires `Ready`).

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/models/CaRefLaneCore.tla docs/superpowers/models/CaRefLaneCore_witness_certifyoutstanding.cfg docs/superpowers/models/run_reflane.sh docs/superpowers/models/CaRefLaneCore_RESULTS.md docs/superpowers/models/CaRelinkLaneComposition.tla docs/superpowers/models/CaRelinkLaneComposition_*.cfg docs/superpowers/models/run_relinklane.sh docs/superpowers/models/README.md
git commit -m "ca-models: the lane certifies outside Ready when the outstanding mutation does not touch the identity

CaRefLaneCore derives the touch from a same-binding attempt; the composition
takes it as a parameter of StartWrite. Wedged and the broken states still
never certify. ConfirmationRequiresReady is renamed to
ConfirmationRequiresUntouchedIdentity; two new witnesses prove a
certification in Writing is reachable in each module.

Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

---

### Task 3: `rt.carved` mirrors the tenure's carved items {#task-3}

Implements spec §carved, first paragraph.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (`RefTableRuntime` near `pending` at `:846`, the `ref_queue_mutex` comment at `:715-716`, the test seams after `refLeaderActiveForTest` at `:477-482`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (carve PLAN/PUBLISH `:2915-2928`, exit guard `:2141-2167`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (after `refLeaderActiveForTest` at `:1038`)
- Test: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`

**Interfaces:**
- Produces: `std::vector<std::shared_ptr<RefMutationItem>> RefTableRuntime::carved` (guarded by `ref_queue_mutex`), `size_t CasRefLedger::refCarvedForTest(const RootNamespace &)`, `size_t Pool::refCarvedForTest(const RootNamespace &)`. Task 6 reads `rt.carved` in rule 3 and its tests call `refCarvedForTest`.

- [ ] **Step 1: Declare the field and the seam only (no bookkeeping yet), so the test can compile and fail on the count**

`CasRefLedger.h`, in `RefTableRuntime` right after the `pending` line:

```cpp
        /// The current tenure's carved items, from the carve (`flushRefBatch`'s PUBLISH phase) to the
        /// tenure's exit guard (`completeOwnedItemsAndReleaseLeadership`), both under `ref_queue_mutex`.
        /// The carve pops an item out of `pending`, so without this mirror a mutation is invisible to
        /// `confirmExactRef` between carve and completion -- the window in which its transaction may be
        /// durable while the committed row still lags it. Over-inclusive on purpose: an installed item
        /// and an item that failed validation before any send both stay here until the exit guard; that
        /// is one tenure of over-refusal for their refs, never an under-refusal.
        std::vector<std::shared_ptr<RefMutationItem>> carved;     /// guarded by ref_queue_mutex
```

`CasRefLedger.h:715-716`: change "`ref_queue_mutex` (which only ever guards `pending`/`leader_active`)" to "`ref_queue_mutex` (which only ever guards `pending`/`carved`/`leader_active`)".

`CasRefLedger.h`, after `refLeaderActiveForTest`:

```cpp
    /// Returns the number of items carved by the current tenure and not yet released by its exit guard.
    /// Under the queue mutex, like `refQueuePendingForTest`.
    size_t refCarvedForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_name_slots.find(ns.string());
        return it == ref_name_slots.end() ? 0 : it->second.current->carved.size();
    }
```

`CasPool.h`, after `refLeaderActiveForTest`:

```cpp
    /// Test-only: the carved-item mirror's size for `ns` (see `CasRefLedger::refCarvedForTest`).
    size_t refCarvedForTest(const RootNamespace & ns) { return ref_ledger.refCarvedForTest(ns); }
```

- [ ] **Step 2: Write the failing test**

In `gtest_cas_confirm_exact_ref.cpp`, after `WedgedLaneIsUnknown`:

```cpp
/// `carved` bookkeeping: a carved item leaves `pending` at the carve and is completed only at the
/// tenure's exit guard, so the confirm reads it from `rt.carved` in between. Sampled at
/// `PostDurableInstall` -- the transaction is durable, nothing is installed, `pending` is already
/// empty -- and again after the tenure: the mirror must hold exactly the carved item during, and be
/// empty after. The hook runs on the leader's own thread with neither lane mutex held, so the seams
/// (which take `ref_queue_mutex`) are safe to call from it.
TEST(CASConfirmExactRef, CarvedItemIsVisibleFromCarveToTenureEnd)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/confirm_carved"};
    publishEmptyPart(store, ns, "x");

    std::atomic<int> samples{0};
    std::atomic<size_t> carved_during{0};
    std::atomic<size_t> pending_during{0};
    store->setCarveHookForTest([&](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase != CasRefLedger::CarvePhaseForTest::PostDurableInstall)
            return;
        samples.fetch_add(1);
        carved_during.store(store->refCarvedForTest(ns));
        pending_during.store(store->refQueuePendingForTest(ns));
    });
    store->dropRef(ns, "x");
    store->setCarveHookForTest(nullptr);

    ASSERT_EQ(samples.load(), 1) << "the drop must commit exactly one chunk";
    EXPECT_EQ(carved_during.load(), 1u)
        << "the carved removal must be visible while its transaction is durable but not installed";
    EXPECT_EQ(pending_during.load(), 0u)
        << "the carve popped the item out of pending -- carved is the only place it can be seen";
    EXPECT_EQ(store->refCarvedForTest(ns), 0u) << "the exit guard must release the mirror";
}
```

- [ ] **Step 3: Build and watch it fail on the count**

```bash
ninja -C build unit_tests_dbms > build/build_f11_task3_red.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef.CarvedItemIsVisibleFromCarveToTenureEnd' > build/test_cas_confirm_task3_red.log 2>&1; echo EXIT=$?; grep -E 'carved_during|Expected|FAILED' build/test_cas_confirm_task3_red.log | head
```

Expected: `NINJA_EXIT=0`, then the test FAILS on `carved_during.load() == 1u` (actual 0). The `pending == 0` and `after == 0` expectations already hold.

- [ ] **Step 4: Maintain the mirror**

`CasRefLedger.cpp`, carve PLAN, right before `owned_items.reserve(owned_items.size() + selected);`:

```cpp
        /// Reserve the confirm-visible mirror too, for the same reason: the publish appends into it and
        /// must not throw.
        rt->carved.reserve(rt->carved.size() + selected);
```

Carve PUBLISH loop: add one line before the pop so the loop body reads

```cpp
            batch.push_back(rt->pending.front());        /// shared_ptr copy, capacity reserved
            owned_items.push_back(rt->pending.front());  /// same item into the responsibility set
            rt->carved.push_back(rt->pending.front());   /// and into the confirm-visible mirror
            rt->pending.pop_front();
```

Exit guard (`completeOwnedItemsAndReleaseLeadership`), after the `for` loop and before `rt->leader_active = false;`:

```cpp
    /// The tenure is over: every carved item is completed (above, or by its chunk's commit) and its
    /// effect is either installed or recorded by the lane state (a wedge), so the confirm no longer
    /// needs to see it.
    rt->carved.clear();
```

Also extend the carve's PUBLISH comment block (the "PUBLISH (no-throw)" paragraph above `std::vector<std::shared_ptr<RefMutationItem>> batch;`) with one sentence: "The same items are appended to `rt->carved`, the confirm-visible mirror the exit guard clears."

- [ ] **Step 5: Build, run the test and the gate**

```bash
ninja -C build unit_tests_dbms > build/build_f11_task3.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef.CarvedItemIsVisibleFromCarveToTenureEnd' > build/test_cas_confirm_task3.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_confirm_task3.log
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_gate_task3.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_gate_task3.log
```

Expected: both `EXIT=0`, `[  PASSED  ]` with the full `CAS*` count and no `FAILED` line.

- [ ] **Step 6: Review and commit**

Fresh-agent review (`ca-review`) of the diff. Then:

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/tests/gtest_cas_confirm_exact_ref.cpp
git commit -m "ca-ref-lane: mirror the tenure's carved items in RefTableRuntime::carved

The carve pops an item out of pending, so between carve and completion a
mutation was invisible to anyone holding only the runtime. The mirror is
appended in the carve's no-throw publish (capacity reserved in the plan)
and cleared at the tenure's exit guard, both under ref_queue_mutex. Test
seam refCarvedForTest; one test samples the mirror at PostDurableInstall.

Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

---

### Task 4: `MutationScope` is validated against the item's ops before anything is durable {#task-4}

Implements spec §carved, second paragraph ("Scope validation").

**Files:**
- Modify: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` (helper `precommitAddRemovePairs` at `:180-198`, its callers at `:554-557`, `ErrorCodes` block at `:50-54`, two new tests)
- Modify: `src/Disks/tests/gtest_cas_ref_chunked_flush.cpp` (helper `addRemovePrecommitPairs` at `:461-479`, callers at `:594-598`, `:703-705`, `:826-828`, `:889-891`, and the ops built for `:594-598` a few lines above; the `oversized_op` scope at `:285`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (`flushRefBatch` step 3, inside the `try` at `:3053`, before the whole-item shape validation)

**Interfaces:**
- Consumes: nothing new.
- Produces: file-local `const String * refNamedOutsideScope(const RefOp &, const String &)` in `CasRefLedger.cpp`; the `LOGICAL_ERROR` contract "a `Ref{name}` item whose ops name another ref fails alone, before durability".

- [ ] **Step 1: Re-scope the existing test items (pure refactor; the suites must stay green)**

`gtest_cas_confirm_exact_ref.cpp` helper:

```cpp
/// `num_pairs` add-then-remove precommit op pairs on ONE ref, each pair naming a distinct manifest,
/// so an item scoped `MutationScope::ref(ref)` names exactly the ref its ops mutate (the flush
/// validates that). Every pair is undone immediately, so the LIVE state stays ~empty and validating
/// thousands of ops stays linear -- it is the OP COUNT, not the resident state, that drives the chunk
/// split under test.
std::vector<RefOp> precommitAddRemovePairs(const String & ref, size_t num_pairs, uint64_t manifest_epoch)
{
    std::vector<RefOp> ops;
    ops.reserve(num_pairs * 2);
    for (size_t i = 0; i < num_pairs; ++i)
    {
        const ManifestRef manifest{manifest_epoch, i + 1, 1};
        RefOp add;
        add.kind = RefOpKind::OwnerTransition;
        add.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref, manifest};
        ops.push_back(std::move(add));
        RefOp remove;
        remove.kind = RefOpKind::OwnerTransition;
        remove.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref, manifest};
        ops.push_back(std::move(remove));
    }
    return ops;
}
```

In the mid-tenure test's `append` lambda (`:552-558`) rename the parameter `prefix` to `ref`; the calls `append("aaa_", ...)`/`append("bbb_", ...)` and the scope `MutationScope::ref(ref)` are now consistent.

`gtest_cas_ref_chunked_flush.cpp` helper `addRemovePrecommitPairs`: same change (first parameter `ref`, drop `paddedRefName(i)` from the binding name, keep the manifest varying by `i`). Then make every caller's first argument equal to the same item's scope name:

```bash
grep -n -E 'addRemovePrecommitPairs\(|MutationScope::ref\(' src/Disks/tests/gtest_cas_ref_chunked_flush.cpp
```

At `:703-705`, `:826-828`, `:889-891`: `"aaa_"` becomes `"item_a"` and `"bbb_"` becomes `"item_b"`. For `:594-598` the ops `ops1`/`ops2`/`ops3` are built a few lines above; their prefixes become `"item_a"`, `"item_b"`, `"item_c"`. Where a test compares a chunk's durable ops against an expected vector built by the same helper (`ChunkedFlushCommitsPerChunk`), the comparison is unaffected because both sides use the new helper.

At `:285` (`OversizedOpFailsItsItemAlone`): change `MutationScope::ref("oversized_op")` to `MutationScope::ref(oversized_op.ref_name)`. At `:248` (`OversizedItemFailsAlone`): leave the scope; `fillerOps` builds default `NamespaceBirth` ops that name no ref, and step 1's op-count cap rejects the item before step 3. Add that sentence as a comment above the `launchAppend` call.

Build and run both suites:

```bash
ninja -C build unit_tests_dbms > build/build_f11_task4_rescope.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef*:CASRefWriterChunkedFlush*' > build/test_cas_rescope_task4.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_rescope_task4.log
```

Expected: `EXIT=0`, all tests pass. Then confirm no mis-scoped item remains: `grep -n 'MutationScope::ref(' src/Disks/tests/*.cpp` (untruncated) and, for each hit, the ops built for that item name only that ref (or no ref).

- [ ] **Step 2: Write the failing validation tests**

`gtest_cas_confirm_exact_ref.cpp`: add `extern const int LOGICAL_ERROR;` to the `DB::ErrorCodes` block. Then, after `CarvedItemIsVisibleFromCarveToTenureEnd`:

```cpp
/// Scope validation: `MutationScope` is what the confirm reads to decide whether an in-flight mutation
/// may change the ref it is asked about, so an item scoped to ref X whose ops mutate ref Y must fail,
/// alone, before anything is durable. It throws `LOGICAL_ERROR`, which aborts the process in debug and
/// sanitizer builds instead of behaving like a catchable exception --
/// `CASConfirmExactRefDeathTest.MisScopedItemAborts` below proves the abort positively in those builds.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASConfirmExactRef, MisScopedItemFailsBeforeAnythingIsDurable)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/confirm_misscoped"};
    const ManifestId seed = publishEmptyPart(store, ns, "seed");   /// the namespace is born already

    const uint64_t puts_before = backend->putTotal();
    RefOp add;
    add.kind = RefOpKind::OwnerTransition;
    add.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "y", ManifestRef{900000003, 1, 1}};
    try
    {
        store->appendRefOps(ns, MutationScope::ref("x"),
                            [add](const RefTableState &) { return std::vector<RefOp>{add}; },
                            RootMutationOrigin::Writer, RootMutationKind::Publish);
        FAIL() << "an item scoped to ref 'x' whose op binds ref 'y' must be refused";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
    EXPECT_EQ(backend->putTotal(), puts_before) << "the refusal must happen before any object is written";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready) << "a validation failure is not a lane fault";
    EXPECT_EQ(store->confirmExactRef(ns, "seed", seed.ref), ConfirmAnswer::Yes)
        << "the failed item must leave the table exactly as it was";
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASConfirmExactRefDeathTest, MisScopedItemAborts)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/confirm_misscoped"};
    publishEmptyPart(store, ns, "seed");

    RefOp add;
    add.kind = RefOpKind::OwnerTransition;
    add.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "y", ManifestRef{900000003, 1, 1}};
    EXPECT_DEATH({
        store->appendRefOps(ns, MutationScope::ref("x"),
                            [add](const RefTableState &) { return std::vector<RefOp>{add}; },
                            RootMutationOrigin::Writer, RootMutationKind::Publish);
    }, "");
}
#endif
```

- [ ] **Step 3: Build and watch the test fail**

```bash
ninja -C build unit_tests_dbms > build/build_f11_task4_red.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef*MisScoped*' > build/test_cas_misscoped_task4_red.log 2>&1; echo EXIT=$?; grep -E 'must be refused|EXPECT_DEATH|FAILED' build/test_cas_misscoped_task4_red.log | head
```

Expected in a release build: FAIL with "an item scoped to ref 'x' whose op binds ref 'y' must be refused" (the append succeeded). In a debug or sanitizer build: the death test FAILS because the statement returned normally.

- [ ] **Step 4: Validate the scope in step 3**

`CasRefLedger.cpp`: add a file-local helper in the anonymous namespace near the top of the file (next to the other `static`/anonymous helpers; if none exist above `flushRefBatch`, put it immediately above `CasRefLedger::flushRefBatch`):

```cpp
/// The first ref name `op` mutates that differs from `scope_ref`, or nullptr when every name it
/// carries is `scope_ref` or it carries none (`NamespaceBirth`, `RemoveNamespace` and `EpochSeal` are
/// namespace-level and belong to no ref). Both bindings of an `OwnerTransition` count: a promotion
/// names the ref twice and a rename-shaped transition would name two refs.
const String * refNamedOutsideScope(const RefOp & op, const String & scope_ref)
{
    if (op.kind == RefOpKind::OwnerTransition)
    {
        if (op.old_binding && op.old_binding->ref_name != scope_ref)
            return &op.old_binding->ref_name;
        if (op.new_binding && op.new_binding->ref_name != scope_ref)
            return &op.new_binding->ref_name;
        return nullptr;
    }
    if (op.kind == RefOpKind::SetPublishedAt && op.ref_name != scope_ref)
        return &op.ref_name;
    return nullptr;
}
```

In `flushRefBatch` step 3, inside the `try` and before the "Whole-item shape validation" comment:

```cpp
            /// Scope validation. `MutationScope` is what `confirmExactRef` reads to decide whether a
            /// queued or in-flight mutation may change the ref it is asked about, so a `Ref{name}` item
            /// whose ops mutate ANOTHER ref would let the confirm answer `Yes` off a row this very item is
            /// about to change. Checked before anything durable and failing only this item: every
            /// production caller names the exact ref its ops mutate, so a mismatch is a programming error.
            if (it->scope.kind == MutationScope::Kind::Ref)
                for (const RefOp & op : item_ops)
                    if (const String * other = refNamedOutsideScope(op, it->scope.ref_name))
                        throw Exception(ErrorCodes::LOGICAL_ERROR,
                            "ref mutation on namespace '{}' is scoped to ref '{}' but its {} op names ref '{}'",
                            ns.string(), it->scope.ref_name, refOpKindToWireWord(op.kind), *other);
```

Update the step-3 header comment (`:3045-3050`) by appending: "The scope is validated here as well, because the confirm relies on it (see `confirmExactRef`, rule 3)."

`refOpKindToWireWord` is declared in `Formats/CasRefLogFormat.h`, the header that also declares `RefOpKind`, which this file already uses, so it is reached transitively; add a direct `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>` only if the compiler asks. `ErrorCodes::LOGICAL_ERROR` is declared in the file's `ErrorCodes` block (the exit guard uses it).

- [ ] **Step 5: Build, run, gate**

```bash
ninja -C build unit_tests_dbms > build/build_f11_task4.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef*MisScoped*' > build/test_cas_misscoped_task4.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_misscoped_task4.log
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_gate_task4.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_gate_task4.log
```

Expected: `EXIT=0` for both; the gate has no `FAILED` line. A `LOGICAL_ERROR` abort anywhere in the gate on a debug build means a mis-scoped item survived Step 1: find it with the `grep` from Step 1, do not weaken the validation.

- [ ] **Step 6: Review and commit**

Fresh-agent review of the diff. Then:

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp src/Disks/tests/gtest_cas_confirm_exact_ref.cpp src/Disks/tests/gtest_cas_ref_chunked_flush.cpp
git commit -m "ca-ref-lane: validate a Ref-scoped item's ops against its MutationScope before durability

The relink confirm is about to read MutationScope to decide whether an
in-flight mutation may change the asked-about ref, so the scope becomes
safety-bearing. flushRefBatch step 3 fails, alone and before any PUT, a
Ref{name} item whose OwnerTransition bindings or SetPublishedAt name a
different ref (LOGICAL_ERROR). The six test items that declared one ref
and mutated 1500 are rewritten to one ref with many manifests.

Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

---

### Task 5: Fake-GCS two-node liveness case, RED against the current server binary {#task-5}

Implements spec §tests, the `test_cas_gcs` paragraph. Runs BEFORE the rule changes so the reproduction is watched failing.

**Files:**
- Modify: `tests/integration/test_cas_gcs/gcs_mocks/server.py` (`Store.__init__` at `:117-126`, `handle_control` at `:741`, `_dispatch` at `:830`, the `/_control/reset` branch)
- Create: `tests/integration/test_cas_gcs_relink_liveness/__init__.py` (empty), `tests/integration/test_cas_gcs_relink_liveness/configs/storage_conf.xml`, `tests/integration/test_cas_gcs_relink_liveness/test.py`

**Interfaces:**
- Produces: fake control endpoint `POST /_control/delay?substr=<key substring>&ms=<milliseconds>` (every PUT whose key contains the substring sleeps `ms` outside the store lock; `substr=&ms=0` clears it; `/_control/reset` clears it too).

- [ ] **Step 1: Check the binary praktika will run is the OLD one**

```bash
ls -la ci/tmp/clickhouse build/programs/clickhouse
git log -1 --format='%h %s' -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
```

`ci/tmp/clickhouse` must point at `build/programs/clickhouse`, and that binary's mtime must predate this plan's Task 3 commit (nothing in Tasks 3 and 4 rebuilt `clickhouse`, only `unit_tests_dbms`). If it was rebuilt, the RED run below is not a RED run; say so in the task report and still write the test.

- [ ] **Step 2: The delay knob in the fake**

`server.py`, `Store.__init__`: after `self.omit_generation = False` add

```python
        # `/_control/delay`: every PUT whose key contains `delay_substr` sleeps `delay_ms` before it is
        # served, outside the store lock so other requests keep flowing. The shape of a provider whose
        # per-object mutation rate is capped (GCS: about one per second per object).
        self.delay_substr = ""
        self.delay_ms = 0
```

`handle_control`: before the `if path == "/_control/reset"` branch add

```python
    if path == "/_control/delay" and method == "POST":
        STORE.delay_substr = query.get("substr", [""])[0]
        STORE.delay_ms = int(query.get("ms", ["0"])[0])
        return Reply(
            200,
            json.dumps({"substr": STORE.delay_substr, "ms": STORE.delay_ms}).encode(),
            {"Content-Type": "application/json"},
        )
```

In the `/_control/reset` branch (it currently resets only `STORE.requests` and `STORE.counters`), add `STORE.delay_substr = ""` and `STORE.delay_ms = 0` after `STORE.counters = {}`.

`_dispatch`: after `bucket, _, key = stripped.partition("/")` and before `with _LOCK:` add

```python
        if method == "PUT" and STORE.delay_ms and STORE.delay_substr and STORE.delay_substr in key:
            time.sleep(STORE.delay_ms / 1000.0)
```

(`time` is already imported; the reads are plain attribute reads under the GIL, and the knob is set only between tests.)

- [ ] **Step 3: The two-node test**

`tests/integration/test_cas_gcs_relink_liveness/configs/storage_conf.xml`:

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <disk_cas_gcs_shared>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <!-- Both replicas mount ONE pool in the fake's `hmacbucket`; each owns its own server-root
                     subtree. The placeholder is replaced per node by the fixture before the restart. -->
                <cas_server_root_id>__SERVER_ROOT_ID__</cas_server_root_id>
                <endpoint>http://fakegcs:8080/hmacbucket/cas/</endpoint>
                <http_client>gcs_hmac</http_client>
                <access_key_id>GOOG1EFAKEACCESSKEYID</access_key_id>
                <secret_access_key>fake-goog4-hmac-secret</secret_access_key>
                <cas_gc_enabled>1</cas_gc_enabled>
                <cas_gc_interval_sec>1</cas_gc_interval_sec>
            </disk_cas_gcs_shared>
        </disks>
        <policies>
            <cas_gcs_shared>
                <volumes><main><disk>disk_cas_gcs_shared</disk></main></volumes>
            </cas_gcs_shared>
        </policies>
    </storage_configuration>
</clickhouse>
```

`tests/integration/test_cas_gcs_relink_liveness/test.py`:

```python
"""Fetch-by-relink liveness on a slow control plane.

Two replicas of one ReplicatedMergeTree table share one CAS pool over the fake GCS service of
`test_cas_gcs`, with every `_ckpt` write delayed so each ref-lane flush is slow the way it is on real
GCS (about one mutation per second per object). Both replicas insert continuously, so each is a sender
and a receiver at once and each keeps its own lane busy. A confirm rule that refuses whenever the
sender's lane is busy starves both replication queues here (finding F11 of the 2026-09-02 live GCS
campaign); the ref-scoped rule lets them drain. The unit tests pin the rule; this is the liveness
reproduction they cannot give.
"""
import os
import threading
import time

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.mock_servers import start_mock_servers

GCS_HOST = "fakegcs"
GCS_PORT = 8080
CONFIG_IN_CONTAINER = "/etc/clickhouse-server/config.d/cas_gcs_shared.xml"
MOCK_DIR = os.path.join(os.path.dirname(__file__), "..", "test_cas_gcs", "gcs_mocks")
NODES = ("node1", "node2")

# Every `_ckpt` PUT sleeps this long: a flush's committed-frontier publication, and so the tenure, lasts
# at least this long. Chosen well above the fake's own latency and well below the replication queue's
# retry backoff, so the lanes stay busy without the test taking minutes.
CKPT_DELAY_MS = 250
INSERTS_PER_NODE = 40
ROWS_PER_INSERT = 1000
DRAIN_TIMEOUT_S = 180

cluster = ClickHouseCluster(__file__)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    # As in test_cas_gcs: a CAS disk mounts at server start and fails closed when its store is
    # unreachable, and the fake can only be launched once its container is up. So both nodes start
    # without the disk, the disk configuration is installed with the node's own server root, and each
    # node is restarted once the fake answers.
    for name in NODES:
        cluster.add_instance(name, macros={"replica": name}, with_zookeeper=True, stay_alive=True)
    cluster.add_instance(
        GCS_HOST, hostname=GCS_HOST, image="altinityinfra/python-bottle", tag="latest", stay_alive=True
    )
    try:
        cluster.start()
        start_mock_servers(cluster, MOCK_DIR, [("server.py", GCS_HOST, str(GCS_PORT))])
        for name in NODES:
            node = cluster.instances[name]
            node.copy_file_to_container(
                os.path.join(os.path.dirname(__file__), "configs", "storage_conf.xml"),
                CONFIG_IN_CONTAINER,
            )
            node.replace_in_config(CONFIG_IN_CONTAINER, "__SERVER_ROOT_ID__", name)
            node.restart_clickhouse()
        yield cluster
    finally:
        cluster.shutdown()


def _control_post(path):
    container = cluster.get_container_id(GCS_HOST)
    return cluster.exec_in_container(
        container, ["curl", "-sS", "-X", "POST", "http://localhost:{}{}".format(GCS_PORT, path)]
    )


def _queue_size(node, table):
    return int(
        node.query("SELECT count() FROM system.replication_queue WHERE table = '{}'".format(table))
    )


def _refusal_counters(node):
    return node.query(
        "SELECT event, value FROM system.events WHERE event LIKE 'CASRelinkConfirmRefused%' "
        "ORDER BY event FORMAT TSV"
    )


def test_both_queues_drain_under_slow_checkpoints():
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "relink_liveness"
    for node in (node1, node2):
        node.query("DROP TABLE IF EXISTS {} SYNC".format(table))
        node.query(
            "CREATE TABLE {t} (id Int64, v UInt64) "
            "ENGINE = ReplicatedMergeTree('/clickhouse/tables/{t}', '{{replica}}') "
            "ORDER BY id SETTINGS storage_policy = 'cas_gcs_shared'".format(t=table)
        )

    _control_post("/_control/delay?substr=_ckpt&ms={}".format(CKPT_DELAY_MS))
    try:
        errors = []

        def insert_loop(node, base):
            try:
                for i in range(INSERTS_PER_NODE):
                    node.query(
                        "INSERT INTO {} SELECT number, number * 10 FROM numbers({}, {})".format(
                            table, base + i * ROWS_PER_INSERT, ROWS_PER_INSERT
                        )
                    )
            except Exception as e:  # surfaced below, on the test thread
                errors.append((node.name, repr(e)))

        threads = [
            threading.Thread(target=insert_loop, args=(node1, 0)),
            threading.Thread(target=insert_loop, args=(node2, 10_000_000)),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert errors == [], errors

        # Liveness: each replica fetches the other's parts by relink while its own lane stays busy
        # with its own inserts and its own fetch bookkeeping. Both queues must still drain.
        deadline = time.time() + DRAIN_TIMEOUT_S
        while time.time() < deadline and (_queue_size(node1, table) or _queue_size(node2, table)):
            time.sleep(1)
        sizes = (_queue_size(node1, table), _queue_size(node2, table))
        for node in (node1, node2):
            print(node.name, "refusal counters:", _refusal_counters(node))
        assert sizes == (0, 0), (
            "replication queues did not drain in {} s with slow checkpoints: node1={} node2={}".format(
                DRAIN_TIMEOUT_S, *sizes
            )
        )
    finally:
        _control_post("/_control/delay?substr=&ms=0")

    expected = 2 * INSERTS_PER_NODE * ROWS_PER_INSERT
    assert int(node1.query("SELECT count() FROM {}".format(table))) == expected
    assert int(node2.query("SELECT count() FROM {}".format(table))) == expected
    for node in (node1, node2):
        node.query("DROP TABLE IF EXISTS {} SYNC".format(table))
```

- [ ] **Step 4: Run it against the old binary and watch it fail**

```bash
python3 -m ci.praktika run "integration" --test test_cas_gcs_relink_liveness > build/itest_cas_gcs_relink_liveness_task5_red.log 2>&1; echo EXIT=$?
jq -r 'select(.outcome=="failed") | "\(.nodeid)\n\(.longrepr)"' ci/tmp/pytest_parallel.jsonl | head -60
```

Expected: the test FAILS on `replication queues did not drain`, both sizes above zero, and the printed refusal counters are empty (the events do not exist in the old binary). Corroborate the mechanism in the server logs under `ci/tmp/` (path printed by praktika): `grep -c 'did not prove it still holds the manifest' <node1 log>` is in the hundreds.

If the test PASSES against the old binary, the fake did not reproduce the starvation: raise `CKPT_DELAY_MS` to 1000 and `INSERTS_PER_NODE` to 80 and rerun once. If it still passes, keep the test as written (it stays the regression net that runs in CI), record in the task report and in Task 7's ledger entry that the fake could not reproduce F11 and that the live gate (Task 8) carries the liveness proof. Do not spend more than these two runs on the fake.

- [ ] **Step 5: Commit the RED test**

Fresh-agent review of the diff. Then:

```bash
git add tests/integration/test_cas_gcs/gcs_mocks/server.py tests/integration/test_cas_gcs_relink_liveness/__init__.py tests/integration/test_cas_gcs_relink_liveness/configs/storage_conf.xml tests/integration/test_cas_gcs_relink_liveness/test.py
git commit -m "tests: two-node fetch-by-relink liveness case over the fake GCS with delayed _ckpt writes

The fake gains POST /_control/delay?substr=&ms= (every matching PUT sleeps
outside the store lock). Two replicas insert continuously on one shared
pool while every checkpoint publication is slow; both replication queues
must drain. Lives in a sibling directory of test_cas_gcs because that
module's fixture is single-node and restarts its server twice. RED
against the table-wide rule 3 (queues stay wedged, F11); Task 6 of the
plan turns it green.

Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

(If the RED run passed instead, the commit message says "regression net; the fake did not reproduce the starvation at 1000 ms" in place of the RED sentence.)

---

### Task 6: Rule 3 reads the scope; observability; unit tests; documentation; both binaries green {#task-6}

Implements spec §rule-3, §tests (unit), §observability, §docs.

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (after `CASRefAppendOccupantUnreadable`, `:810`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (`ProfileEvents` externs near `:50`; `confirmExactRef` header comment `:426-435`, the `try_to_lock` site `:461-462`, rule 3 `:474-481`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (`:40-44`, `:57-61`, `:156-159`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h` (`:56-60`)
- Modify: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` (header `:30-48`, `InFlightAppendIsUnknown` `:467-503`, `MidTenureChunkBoundaryIsUnknown` `:507-582`, three new tests)

**Interfaces:**
- Consumes: `rt.carved`, `refCarvedForTest` (Task 3); the scope contract (Task 4); `ChunkFaultBackend::Mode::Unresolved` and `openPoolWithConfig` (existing).
- Produces: ProfileEvents `CASRelinkConfirmRefusedRefMutationInFlight`, `CASRelinkConfirmRefusedLaneWedged`, `CASRelinkConfirmRefusedLaneBroken`, `CASRelinkConfirmRefusedStateLockBusy`; the log line `Relink confirm for ref '<ref>' in namespace '<ns>' is unknown: <reason>` at TRACE on logger `CasRefLedger`. Task 8 reads the counters from `system.events`.

- [ ] **Step 1: Write the failing tests**

In `gtest_cas_confirm_exact_ref.cpp`:

(a) Rename and flip the mid-tenure test. Replace its header comment and body:

```cpp
/// Rule 3 at a chunk boundary (`CarvePhaseForTest::ChunkReseed`): one leader tenure commits MULTIPLE
/// durable transactions, so between two chunks the table is PARTIALLY durable -- for the refs those
/// chunks mutate. The seed ref is touched by neither, so its row is exactly as authoritative as on an
/// idle lane and it confirms; the carved items' own refs refuse, because their transactions may be
/// durable and not installed. The confirm is issued on the leader's own thread, which is safe because
/// the boundary holds neither lane mutex.
TEST(CASConfirmExactRef, UntouchedRefConfirmsMidTenure)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/confirm_mid_tenure"};

    const ManifestId id = publishEmptyPart(store, ns, "seed");
    ASSERT_EQ(store->confirmExactRef(ns, "seed", id.ref), ConfirmAnswer::Yes);

    std::atomic<int> boundaries{0};
    std::atomic<int> yes_for_seed_at_boundary{0};
    std::atomic<int> unknown_for_carved_ref_at_boundary{0};
    std::atomic<int> requests_at_boundary{0};
    store->setCarveHookForTest([&](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase != CasRefLedger::CarvePhaseForTest::ChunkReseed)
            return;
        boundaries.fetch_add(1);
        const uint64_t before = backendRequests(*backend);
        if (store->confirmExactRef(ns, "seed", id.ref) == ConfirmAnswer::Yes)
            yes_for_seed_at_boundary.fetch_add(1);
        /// "aaa_" has no committed row (rule 5 would say `No`), so an `Unknown` here can only come from
        /// rule 3 reading the carved item's scope.
        if (store->confirmExactRef(ns, "aaa_", id.ref) == ConfirmAnswer::Unknown)
            unknown_for_carved_ref_at_boundary.fetch_add(1);
        requests_at_boundary.fetch_add(static_cast<int>(backendRequests(*backend) - before));
    });
```

Keep the co-batching block (the `CaseSync`, the two appends of 1500 pairs, the joins) exactly as it is, then replace the final assertions with:

```cpp
    ASSERT_GE(boundaries.load(), 1) << "the flush did not chunk -- the mid-tenure window was not exercised";
    EXPECT_EQ(yes_for_seed_at_boundary.load(), boundaries.load())
        << "a ref no carved item names must confirm mid-tenure -- that is the liveness F11 needs";
    EXPECT_EQ(unknown_for_carved_ref_at_boundary.load(), boundaries.load())
        << "a ref a carved item names must not confirm while its transaction may be durable and not installed";
    EXPECT_EQ(requests_at_boundary.load(), 0) << "the mid-tenure confirm must still be I/O-free";

    /// The tenure is over: the seed ref confirms as before.
    EXPECT_EQ(store->confirmExactRef(ns, "seed", id.ref), ConfirmAnswer::Yes);
}
```

(b) In `InFlightAppendIsUnknown`, change the two messages: "an admitted append makes the whole table's committed view provisional" becomes "an admitted mutation of THIS ref makes its committed row provisional"; and in its header comment replace "a naive implementation answers `Yes` here" sentence's neighbour "The apply-state is still `Clean` at this point, so rule 3 is what produces the `Unknown`, not rule 4." with "The lane state is still `Ready` and the item is in `pending`, so rule 3 reading the item's scope is what produces the `Unknown`."

(c) New liveness test, after `ConcurrentAppendIsOrderedAfterTheSnapshot`:

```cpp
/// The F11 shape: a mutation of ANOTHER ref is queued and its leader is parked before the carve, so
/// the lane has a pending item and an active tenure. A confirm about an untouched committed ref must
/// answer `Yes` -- the queued mutation cannot change this ref's binding or the blobs its manifest
/// protects -- while the queued ref itself answers `Unknown`.
TEST(CASConfirmExactRef, UntouchedRefConfirmsWhileAnotherRefIsQueued)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/confirm_liveness"};

    const ManifestId id_x = publishEmptyPart(store, ns, "x");
    const ManifestId id_other = publishEmptyPart(store, ns, "other");
    ASSERT_EQ(store->confirmExactRef(ns, "x", id_x.ref), ConfirmAnswer::Yes);

    LeaderLatch latch;
    latch.arm(store);
    std::thread dropper([&] { store->dropRef(ns, "other"); });
    latch.awaitEntered();

    /// Sampled while parked, asserted after the join (a failed assertion here would skip the release).
    const bool leader_active = store->refLeaderActiveForTest(ns);
    const size_t pending = store->refQueuePendingForTest(ns);
    const ConfirmAnswer untouched = store->confirmExactRef(ns, "x", id_x.ref);
    const ConfirmAnswer touched = store->confirmExactRef(ns, "other", id_other.ref);

    latch.release();
    dropper.join();
    store->setRefPreCarveHookForTest(nullptr);

    EXPECT_TRUE(leader_active);
    EXPECT_EQ(pending, 1u);
    EXPECT_EQ(untouched, ConfirmAnswer::Yes)
        << "a queued mutation of another ref must not refuse this one";
    EXPECT_EQ(touched, ConfirmAnswer::Unknown)
        << "the queued ref's own row is provisional";
    EXPECT_EQ(store->confirmExactRef(ns, "x", id_x.ref), ConfirmAnswer::Yes);
    EXPECT_EQ(store->confirmExactRef(ns, "other", id_other.ref), ConfirmAnswer::No);
}
```

(d) New same-ref stale-row regression, after (c). This one PASSES before the rule change (the old rule refuses by `Writing`) and after (by `carved`); its `pending == 0` and `carved == 1` samples are what prove the refusal after the change comes from the mirror. To watch it fail for the right reason once, comment out the `rt.carved` loop of rule 3 after Step 3 below, run it, see `Unknown` become `Yes`, and restore the loop.

```cpp
/// The stale-row hazard rule 3 exists for, on the same ref: a repoint of x from m1 to m2 is DURABLE
/// and NOT installed, so the committed row still says m1. A `Yes` here would let a receiver promote a
/// manifest whose blobs the durable repoint may already have retired (the model's `_sab_stalecache`).
/// The leader is parked at the SECOND `PostDurableInstall` of the repointing publish (the first is its
/// precommit), with no lane mutex held; `pending` is already empty, so only the carved mirror can
/// refuse.
TEST(CASConfirmExactRef, SameRefRepointDurableButNotInstalledIsUnknown)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/confirm_stale_row"};
    const ManifestId m1 = publishEmptyPart(store, ns, "x");
    ASSERT_EQ(store->confirmExactRef(ns, "x", m1.ref), ConfirmAnswer::Yes);

    struct Hold
    {
        std::mutex m;
        std::condition_variable cv;
        int seen = 0;
        bool parked = false;
        bool released = false;
    };
    auto hold = std::make_shared<Hold>();
    store->setCarveHookForTest([hold](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase != CasRefLedger::CarvePhaseForTest::PostDurableInstall)
            return;
        std::unique_lock lk(hold->m);
        if (++hold->seen != 2)
            return;   /// 1 = the precommit's chunk; 2 = the promote (the repoint) -- park here
        hold->parked = true;
        hold->cv.notify_all();
        hold->cv.wait_for(lk, std::chrono::seconds(20), [&] { return hold->released; });
    });

    ManifestId m2;
    std::thread repointer([&] { m2 = publishEmptyPart(store, ns, "x", /*allow_repoint=*/true); });
    bool parked = false;
    {
        std::unique_lock lk(hold->m);
        parked = hold->cv.wait_for(lk, std::chrono::seconds(20), [&] { return hold->parked; });
    }
    const size_t pending_now = store->refQueuePendingForTest(ns);
    const size_t carved_now = store->refCarvedForTest(ns);
    const RefLaneState lane_now = store->laneStateForTest(ns);
    const ConfirmAnswer stale = store->confirmExactRef(ns, "x", m1.ref);
    {
        std::lock_guard lk(hold->m);
        hold->released = true;
    }
    hold->cv.notify_all();
    repointer.join();
    store->setCarveHookForTest(nullptr);

    ASSERT_TRUE(parked) << "the repoint never reached its post-durable window";
    EXPECT_EQ(pending_now, 0u) << "the repoint was carved: pending cannot be what refuses";
    EXPECT_EQ(carved_now, 1u) << "the carved mirror is what the confirm must read";
    EXPECT_EQ(lane_now, RefLaneState::Writing);
    EXPECT_EQ(stale, ConfirmAnswer::Unknown)
        << "x's durable repoint is not installed: its row is stale and must not confirm m1";
    EXPECT_EQ(store->confirmExactRef(ns, "x", m1.ref), ConfirmAnswer::No) << "installed: m1 is no longer x's binding";
    EXPECT_EQ(store->confirmExactRef(ns, "x", m2.ref), ConfirmAnswer::Yes);
}
```

(e) New real-wedge sibling, after `WedgedLaneIsUnknown`. This one also passes before and after (both rules refuse `Wedged` table-wide); it pins the hole revision 7 had. The sabotage that would fail it is exempting `Wedged` in rule 3.

```cpp
/// Rule 3, a REAL wedge: the removal of x is sent, the response is lost, the single-attempt budget is
/// exhausted, and the lane wedges. `commitRefChunk` completes the chunk's items with an error before
/// the tenure ends, so the transaction that may be durable is recorded nowhere but in the attempt and
/// the lane state -- `pending` and `carved` are both empty. Every ref refuses: x because its removal
/// may be durable, `other` because nothing but the lane state records WHICH ref the wedged transaction
/// touched.
TEST(CASConfirmExactRef, WedgedTransactionRefusesEveryRef)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    PoolConfig cfg;
    /// Single-attempt budget: one ambiguous PUT is a conclusive wedge, no inter-attempt sleep.
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 5000;
    budget.lease_safety_margin_ms = 100;
    cfg.cas_request_budget = budget;
    auto store = openPoolWithConfig(backend, cfg);
    const RootNamespace ns{"srv1/confirm_real_wedge"};

    const ManifestId id_x = publishEmptyPart(store, ns, "x");
    const ManifestId id_other = publishEmptyPart(store, ns, "other");
    ASSERT_EQ(store->confirmExactRef(ns, "x", id_x.ref), ConfirmAnswer::Yes);
    ASSERT_EQ(store->confirmExactRef(ns, "other", id_other.ref), ConfirmAnswer::Yes);

    backend->fault_substr = store->layout().namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Unresolved;
    backend->fault_skip = 0;
    backend->fault_count = 1;
    EXPECT_THROW(store->dropRef(ns, "x"), DB::Exception);
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    ASSERT_EQ(store->refQueuePendingForTest(ns), 0u);
    ASSERT_EQ(store->refCarvedForTest(ns), 0u)
        << "the wedged item was completed and released; only the lane state records its transaction";

    backend->resetCounts();
    EXPECT_EQ(store->confirmExactRef(ns, "x", id_x.ref), ConfirmAnswer::Unknown) << "x's removal may be durable";
    EXPECT_EQ(store->confirmExactRef(ns, "other", id_other.ref), ConfirmAnswer::Unknown)
        << "a wedge refuses table-wide: no per-ref record of the wedged transaction survives the tenure";
    EXPECT_EQ(backendRequests(*backend), 0u) << "a wedged lane must answer without trying to resolve the wedge";
}
```

`CasRequestBudget` and `ChunkFaultBackend` come from the headers `gtest_cas_ref_chunked_flush.cpp` already includes; add the same includes if the compiler asks.

- [ ] **Step 2: Build and watch (a) and (c) fail, (d) and (e) pass**

```bash
ninja -C build unit_tests_dbms > build/build_f11_task6_red.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef.*' > build/test_cas_confirm_task6_red.log 2>&1; echo EXIT=$?; grep -E '^\[  (FAILED|PASSED)' build/test_cas_confirm_task6_red.log
```

Expected: exactly two failures, `UntouchedRefConfirmsMidTenure` (seed answers `Unknown` at the boundary) and `UntouchedRefConfirmsWhileAnotherRefIsQueued` (`untouched` is `Unknown`). `SameRefRepointDurableButNotInstalledIsUnknown`, `WedgedTransactionRefusesEveryRef`, `InFlightAppendIsUnknown` and `ConcurrentAppendIsOrderedAfterTheSnapshot` pass.

- [ ] **Step 3: The events, the rule, the trace**

`src/Common/ProfileEvents.cpp`, after the `CASRefAppendOccupantUnreadable` line:

```cpp
    M(CASRelinkConfirmRefusedRefMutationInFlight, "Number of CAS fetch-by-relink confirms this server answered Unknown because a queued or in-flight ref-lane mutation names the asked-about ref or the whole namespace. Expected under write load; the receiver retries the fetch.", ValueType::Number) \
    M(CASRelinkConfirmRefusedLaneWedged, "Number of CAS fetch-by-relink confirms answered Unknown because the namespace's ref lane holds an unresolved append (a wedge). Lasts until the next flush or a remount resolves it.", ValueType::Number) \
    M(CASRelinkConfirmRefusedLaneBroken, "Number of CAS fetch-by-relink confirms answered Unknown because the namespace's ref lane is in NeedsRecovery, Closed or Faulted state, or is Writing with nothing carved. A growing value outside induced faults is a lane defect, not load.", ValueType::Number) \
    M(CASRelinkConfirmRefusedStateLockBusy, "Number of CAS fetch-by-relink confirms answered Unknown because the ref table's state lock was held (a recovery, a listing or a snapshot publish in progress). The confirm never waits for it.", ValueType::Number) \
```

`CasRefLedger.cpp`, `namespace ProfileEvents` block near `:50`: add the four `extern const Event ...;` lines.

`confirmExactRef`: right after `RefTableRuntime & rt = *it->second.current;` add

```cpp
    /// Every refusal below is attributed here, on the node that computed it: `ConfirmAnswer` crosses
    /// two interfaces as a three-value enum and stays that way, so the counters and this trace line are
    /// the only way a live gate can tell load (`RefMutationInFlight`) from a fault (`LaneWedged`,
    /// `LaneBroken`) or contention (`StateLockBusy`).
    const auto refuse = [&](ProfileEvents::Event reason, std::string_view why)
    {
        ProfileEvents::increment(reason);
        LOG_TRACE(getLogger("CasRefLedger"), "Relink confirm for ref '{}' in namespace '{}' is unknown: {}",
                  ref_name, ns.string(), why);
        return ConfirmAnswer::Unknown;
    };
```

Change the `try_to_lock` refusal to `return refuse(ProfileEvents::CASRelinkConfirmRefusedStateLockBusy, "the table's state lock is held");`.

Replace the rule 3 comment block and its `if` (`:474-481`) with:

```cpp
    /// Rule 3 (no admitted mutation of THIS ref). The hazard is a committed row that lags a transaction
    /// of the asked-about ref: the leader does not hold `state_mutex` across the `PUT`, so between
    /// "durable" and "installed" that ref's row is stale, and a `Yes` read off it would authorize a
    /// receiver to promote over a blob the transaction may already have retired. A mutation of ANOTHER
    /// ref cannot change this ref's binding or the blobs its manifest protects, so its row is exactly as
    /// authoritative as on an idle lane; refusing for it is what starved two replicas of each other on a
    /// slow control plane. Every admitted mutation names its scope (`MutationScope`, recorded at
    /// admission under `ref_queue_mutex` and validated against its ops at flush), and it is visible in
    /// `pending` from admission to carve and in `carved` from carve to the tenure's exit guard, so "a
    /// change of this ref is queued or in flight" is read from those two. The lane states other than
    /// `Ready`/`Writing` refuse table-wide: `Wedged` holds a transaction that may be durable and is
    /// recorded nowhere but in the attempt and the lane state (its items are completed with an error
    /// before the tenure ends), and `NeedsRecovery`, `Closed`, `Faulted` are fences on the whole view.
    /// `Writing` with nothing carved cannot happen; it fails closed.
    if (rt.lane_state == RefLaneState::Wedged)
        return refuse(ProfileEvents::CASRelinkConfirmRefusedLaneWedged, "the lane holds an unresolved append");
    if (rt.lane_state != RefLaneState::Ready && rt.lane_state != RefLaneState::Writing)
        return refuse(ProfileEvents::CASRelinkConfirmRefusedLaneBroken, "the lane is neither Ready nor Writing");
    if (rt.lane_state == RefLaneState::Writing && rt.carved.empty())
        return refuse(ProfileEvents::CASRelinkConfirmRefusedLaneBroken, "the lane is Writing with nothing carved");
    const auto covers = [&](const MutationScope & scope)
    {
        return scope.kind == MutationScope::Kind::WholeShard || scope.ref_name == ref_name;
    };
    for (const auto & item : rt.pending)
        if (covers(item->scope))
            return refuse(ProfileEvents::CASRelinkConfirmRefusedRefMutationInFlight, "a queued mutation names this ref");
    for (const auto & item : rt.carved)
        if (covers(item->scope))
            return refuse(ProfileEvents::CASRelinkConfirmRefusedRefMutationInFlight, "a carved mutation names this ref");
```

`ProfileEvents::Event` needs `<Common/ProfileEvents.h>`, already included (the file increments `CASRefBatchScopeCuts`). `LOG_TRACE` needs `<Common/logger_useful.h>`, already included (the file uses `LOG_WARNING`).

- [ ] **Step 4: Build, run the suite and the gate**

```bash
ninja -C build unit_tests_dbms > build/build_f11_task6_green.log 2>&1; echo NINJA_EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CASConfirmExactRef*' > build/test_cas_confirm_task6.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_confirm_task6.log
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_gate_task6.log 2>&1; echo EXIT=$?; tail -3 build/test_cas_gate_task6.log
```

Expected: `EXIT=0` for both, no `FAILED`. Then the sabotage check for test (d): comment out the `for (const auto & item : rt.carved)` loop, rebuild, run `--gtest_filter='CASConfirmExactRef.SameRefRepointDurableButNotInstalledIsUnknown'`, expect it to FAIL with `stale` being `Yes`, restore the loop, rebuild. Record the observed failure message in the task report.

- [ ] **Step 5: Documentation that travels with the code**

`CasRefLedger.h:40-44`, replace the second paragraph of the `RefLaneState` comment with:

```cpp
/// `Ready` is the state that admits a new append. A cached row is certified (`confirmExactRef`) in
/// `Ready`, and in `Writing` when no queued or carved mutation names that row's ref. `Writing` owns the
/// exact attempt before its first possible send. `Wedged` owns that same attempt after an ambiguous
/// result and certifies nothing. `NeedsRecovery` means a transaction is known durable but cannot be
/// installed in this cache; it is a hard write and certification fence until replay completes. `Closed`
/// records a successor's epoch seal, and `Faulted` records foreign or internally inconsistent durable
/// state.
```

`CasRefLedger.h:57-61` (`ConfirmAnswer`), replace "a cold, evicted, recovering, busy or non-`Ready` table answers `Unknown` rather than doing any work to find out." with "a cold, evicted, recovering, wedged or otherwise broken table answers `Unknown` rather than doing any work to find out, and so does a table with a queued or in-flight mutation of the asked-about ref; a mutation of another ref does not refuse."

`CasRefLedger.h:156-159`, replace the rules sentence with: "The rules are evaluated as one snapshot spanning both lane mutexes, in this order: table warm and resident; lane state `Ready` or `Writing`, with no queued or carved mutation whose `MutationScope` covers the ref; exact committed-row equality; mount fence live last."

`CasRefLedger.cpp:426-435` (the "ONE snapshot across BOTH lane mutexes" paragraph): replace "`pending`/`leader_active` live under `ref_queue_mutex`" with "`pending`/`carved` live under `ref_queue_mutex`", and replace the last sentence "There is no interleaving in which a removal is admitted and this function still answers `Yes`." with "There is no interleaving in which a mutation of the asked-about ref is admitted and this function still answers `Yes`; a mutation of another ref may be admitted, and the answer is still right, because it cannot move this ref's row."

`CasRefProtocol.h:56-60` (`MutationScope`): append to the comment: "It is also safety-bearing: `CasRefLedger::confirmExactRef` refuses to certify a ref while a queued or carved item's scope covers it, and `flushRefBatch` fails an item whose ops name a ref outside its declared scope, so a caller must name exactly the ref its ops mutate."

`gtest_cas_confirm_exact_ref.cpp:30-48` header: replace the second contract bullet with:

```cpp
///   - The snapshot spans BOTH lane mutexes, so an append admitted concurrently is ordered strictly
///     after it: there is no window in which the confirm says `Yes` while a mutation OF THAT REF is
///     already admitted. A queued or in-flight mutation of another ref does not refuse -- rule 3 reads
///     each item's `MutationScope`, and refusing for the whole table starved two replicas of each other
///     under load on a slow control plane.
```

- [ ] **Step 6: Build the server, run the integration tests, review, commit**

```bash
ninja -C build clickhouse > build/build_f11_task6_server.log 2>&1; echo NINJA_EXIT=$?
ls -la ci/tmp/clickhouse build/programs/clickhouse
python3 -m ci.praktika run "integration" --test test_cas_gcs > build/itest_cas_gcs_task6.log 2>&1; echo EXIT=$?
jq -r 'select(.outcome=="failed") | "\(.nodeid)\n\(.longrepr)"' ci/tmp/pytest_parallel.jsonl | head -40
```

`--test test_cas_gcs` matches both `test_cas_gcs` and `test_cas_gcs_relink_liveness`. Expected: `Failures: 0/<N>`; the liveness case prints both nodes' refusal counters with `CASRelinkConfirmRefusedRefMutationInFlight` present and `LaneWedged`/`LaneBroken` at zero. Also run the replicated relink battery, which exercises the confirm end to end on RustFS: `python3 -m ci.praktika run "integration" --test test_cas_replicated_relink > build/itest_cas_replicated_relink_task6.log 2>&1` and expect `Failures: 0`.

Fresh-agent review of the whole diff. Then:

```bash
git add src/Common/ProfileEvents.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h src/Disks/tests/gtest_cas_confirm_exact_ref.cpp
git commit -m "ca-relink: rule 3 of confirmExactRef refuses only a queued or in-flight mutation of the asked-about ref

Rule 3 reads each pending and carved item's MutationScope; a mutation of
another ref no longer refuses, and leader_active and a non-empty pending
no longer refuse by themselves. Wedged, NeedsRecovery, Closed and Faulted
keep refusing table-wide. Four ProfileEvents and a TRACE line attribute
every Unknown. Tests: the mid-tenure case flips to Yes for the untouched
ref, a liveness case, a same-ref stale-row pin read from the carved
mirror, and a real-wedge sibling that refuses every ref.

Fixes finding F11 of the 2026-09-02 live GCS campaign.
Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

---

### Task 7: Record the state in the backlog and the campaign ledger {#task-7}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG/gcs.md` (the `[relink-confirm-lane-livelock]` item under `{#relink-confirm-lane-livelock}`)
- Modify: `docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md` (task table row for F11, status log)

- [ ] **Step 1: Backlog**

In the `[relink-confirm-lane-livelock]` item, change the status word to `IMPLEMENTED, live gate pending` and add two lines: `Spec: docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md (revision 8)` and `Plan: docs/superpowers/plans/2026-09-02-cas-relink-confirm-liveness.md; commits: <the six hashes from git log --oneline -6>`. Under the item, record in one sentence whether the fake-GCS case reproduced the starvation in Task 5 (RED observed, or not).

- [ ] **Step 2: Ledger**

Append to the status log (timestamped, local time as the existing lines): model batteries rerun (15 + 27 + 11 rows, counts from the logs), `CAS*` gate count from `build/test_cas_gate_task6.log`, integration results, and "live gate next (Task 8)". Update the F11 finding's status from OPEN to `FIXED (pending live gate)`.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/cas/BACKLOG/gcs.md docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md
git commit -m "ca-backlog: F11 relink-confirm livelock implemented, live gate pending

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

---

### Task 8: Live gate, a ten-minute phase-3 soak on the real GCS stand {#task-8}

Implements spec §live-gate. The stand `ca-live-gcs` (compose project of `utils/ca-soak/docker-compose-gcs.yml`) is up and bind-mounts `build/programs/clickhouse` into both replicas.

**Files:**
- Modify: `docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md` (results)
- Modify: `docs/superpowers/cas/BACKLOG/gcs.md` (item status)

- [ ] **Step 1: Put the new binary on the stand without touching its identity**

```bash
cd utils/ca-soak
md5sum ../../build/programs/clickhouse
docker compose --env-file configs/gcs.env -f docker-compose-gcs.yml -p ca-live-gcs up -d --force-recreate ch1 ch2 > ../../build/soak_gcs_f11_recreate.log 2>&1; echo EXIT=$?
docker exec ca-live-gcs-ch1-1 md5sum /usr/bin/clickhouse
docker exec ca-live-gcs-ch2-1 md5sum /usr/bin/clickhouse
```

Never `down -v`: the named volumes hold each server's CAS identity (see the compose file header). The three checksums must match. Then wait until both answer:

```bash
for c in ca-live-gcs-ch1-1 ca-live-gcs-ch2-1; do until docker exec $c clickhouse-client -q "SELECT 1" >/dev/null 2>&1; do sleep 2; done; docker exec $c clickhouse-client -q "SELECT name, status FROM system.cas_mounts FORMAT TSV"; done
```

Expected: both mounts `live`. (If the table is named differently on this build, `SELECT name FROM system.tables WHERE name LIKE 'cas_%'` finds it.)

- [ ] **Step 2: Baseline the refusal counters and the queue**

```bash
for c in ca-live-gcs-ch1-1 ca-live-gcs-ch2-1; do echo == $c; docker exec $c clickhouse-client -q "SELECT event, value FROM system.events WHERE event LIKE 'CASRelinkConfirmRefused%' ORDER BY event FORMAT TSV"; docker exec $c clickhouse-client -q "SELECT count() FROM system.replication_queue"; done
```

Expected: counters absent or zero, queues empty (the previous soak's table is idle).

- [ ] **Step 3: Run the soak**

The chaos schedule targets CH1, CH2, BOTH and RUSTFS; the stand has no RustFS, so point the RustFS slot at the second replica (a pause/restart of ch2, which the stand supports; a kill of that slot is capped at 60 s by `chaos.py`). `--max-pool-gb 0` because there is no local store to measure (the throttle fails closed otherwise).

```bash
cd utils/ca-soak
PYTHONPATH=. CA_SOAK_NODE1_CONTAINER=ca-live-gcs-ch1-1 CA_SOAK_NODE2_CONTAINER=ca-live-gcs-ch2-1 CA_SOAK_FSCK_CONTAINER=ca-live-gcs-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-live-gcs-ch2-1 \
  python3 -m soak.run --seed 20260902 --phase 3 --duration 10m --workers 6 --max-pool-gb 0 --metrics soak_gcs_f11_gate_10m.db > ../../build/soak_gcs_f11_gate_10m.log 2>&1; echo SOAK_EXIT=$?
```

Run it in the background and wait for the exit marker; do not poll with self-matching `pgrep`. A subagent reads `build/soak_gcs_f11_gate_10m.log` and reports: every checkpoint's model-equality result on both replicas, the fsck gate lines, the stages reached (warmup, mutations, ttl, gc_checkpoint, chaos, cliff, converge), and the final gate report.

- [ ] **Step 4: Read the gate**

```bash
for c in ca-live-gcs-ch1-1 ca-live-gcs-ch2-1; do echo == $c; docker exec $c clickhouse-client -q "SELECT event, value FROM system.events WHERE event LIKE 'CASRelinkConfirmRefused%' ORDER BY event FORMAT TSV"; docker exec $c clickhouse-client -q "SELECT count(), countIf(last_exception LIKE '%did not prove%') FROM system.replication_queue"; docker exec $c bash -c "grep -a -c 'did not prove it still holds the manifest' /var/log/clickhouse-server/clickhouse-server.log"; done
```

Pass criteria, all four: (1) `SOAK_EXIT=0` with model equality on both replicas at every checkpoint; (2) both replication queues empty at the end, with no `did not prove` entries; (3) `CASRelinkConfirmRefusedRefMutationInFlight` is the largest refusal counter on both nodes; (4) `CASRelinkConfirmRefusedLaneWedged` and `...LaneBroken` are zero, or non-zero only in the minutes around the chaos faults (compare their timestamps in the soak log with the counters sampled before and after). A failure of (1) or (2) is a stop: analyse before changing anything, and record the finding as a new F-item in the ledger. Redact any log excerpt: nothing that starts with `GOOG`, no 40-character secrets.

- [ ] **Step 5: Record and commit**

Ledger: a status-log entry with the run id, the four criteria and their values, the metrics database name (`utils/ca-soak/soak_gcs_f11_gate_10m.db`, not committed) and the log path. Backlog item `[relink-confirm-lane-livelock]`: status `CLOSED` with the gate's date, or `OPEN` with the new finding's id.

```bash
git add docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md docs/superpowers/cas/BACKLOG/gcs.md
git commit -m "ca-ledger: F11 live gate, ten-minute phase-3 soak on the GCS stand

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01R7rmcGDgQE9PFZrghSu4Pi"
```

The two-hour closing soak is not part of this plan: it runs once after every fix of the campaign has landed (spec §live-gate).

---

## Self-review against the spec {#self-review}

Spec coverage:

- §rule-3 code and `scopeCovers` → Task 6 Step 3 (`covers`). Wedged table-wide, `Writing` with empty `carved` fails closed → same step. `try_to_lock` and zero-I/O unchanged → untouched; `StateLockBusy` counter added at the existing site.
- §carved, `rt.carved` appended at carve, cleared at exit guard, `carve_all_pending` arms untouched, `forceWedgeForTest` unchanged → Task 3.
- §carved, scope validation in step 3 with `LOGICAL_ERROR`, the six mis-scoped test items rewritten, the two oversized sites aligned → Task 4.
- §model, `CaRelinkConfirmCore.tla` (`noop` shape, `sTouches`, `SabotageTouchBlind`, shape-aware `SenderApply`, `sawYesWhilePendingNoop`, `nextId <= MaxId` on every writing step, `MaxHoles = 0`) → Task 1. Lane core and composition (touch derived from `attempt.binding # cache_binding`; nondeterministic `StartWrite(touch)`; redefined blocked-certify sabotages; `W_CertifiedWhileOutstanding`, `W_ConfirmedOutsideReady`; reruns; RESULTS `:43`, `:68-91`, `:126-142`) → Tasks 1 and 2.
- §tests: the four named tests (two kept, one flipped and renamed, one kept plus a real-wedge sibling) → Task 6 Step 1 (a), (b), (e); same-ref stale-row regression (d); liveness (c); `carved` bookkeeping → Task 3; scope validation with the debug split → Task 4; header `:40-46` → Task 6 Step 5; `test_cas_gcs` two-node delayed-`_ckpt` case → Task 5 (sibling directory, deviation stated); observability (four events, `LOG_TRACE`) → Task 6 Step 3.
- §live-gate → Task 8. §docs list: `CasRefLedger.h:40,:60,:156-159` and the runtime comment → Tasks 3 and 6; `CasRefLedger.cpp:426-435`, rule 3 block, carve/exit-guard comments, step-3 comment → Tasks 3, 4, 6; `CasRefProtocol.h:56-60` → Task 6; gtest header → Task 6; models RESULTS and README → Tasks 1 and 2.
- §rollout: no protocol or persisted-format change anywhere in the plan. §out-of-scope: nothing here touches `_ckpt` coalescing, receiver damping or LIST completeness.

Placeholder scan: every code step carries its code; the only "look and decide" steps are the RESULTS state counts (read from the run logs, which do not exist yet) and the `grep` sweeps whose hits are enumerated by the command itself.

Type consistency: `refCarvedForTest` (Task 3) is the name used in Tasks 4 and 6; `carved` is a `std::vector<std::shared_ptr<RefMutationItem>>` in the header and iterated as such in rule 3; `refNamedOutsideScope` returns `const String *` and is consumed as such; the composition invariant is `ConfirmationRequiresUntouchedIdentity` in the module, every cfg and the runner; the four event names are spelled identically in `ProfileEvents.cpp`, the ledger externs, the rule and the integration test's `LIKE` pattern.
