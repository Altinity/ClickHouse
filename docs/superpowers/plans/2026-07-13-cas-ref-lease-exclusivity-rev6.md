---
description: 'Implementation plan for CAS ref protocol rev.6: lease-boundary exclusivity (observation-based liveness, conditional T_mat, epoch-closing seal, grace removal)'
sidebar_label: 'CAS Ref Lease Exclusivity rev.6 (plan)'
sidebar_position: 20260713
slug: /superpowers/plans/cas-ref-lease-exclusivity-rev6
title: 'CAS Ref Lease Exclusivity rev.6 Implementation Plan'
doc_type: 'reference'
---

# CAS Ref Lease Exclusivity rev.6 Implementation Plan {#rev6-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved spec
`docs/superpowers/specs/2026-07-13-cas-ref-lease-exclusivity-rev6-design.md`: solve writer
exclusivity once at the mount-lease boundary (clock-free observation, conditional T_mat,
epoch-closing recovery seal) and delete the ref protocol's grace machinery.

**Architecture:** TLA+ gates first (extend the ref model to prove `NoDivergentFold` under the new
mount rule; extend the mount model to prove observation-based reclaim). Then lease-side hardening
(observation reclaim, clean-release drain, T_mat), then the seal, then the hot-path simplification
(publish-from-live), then anomaly policy and detectors. Recovery is lazy per namespace in this
codebase, so "seal before writable" is realized as "seal inside `ensureRefTableRecovered`, before
the table's state is exposed" — equivalent for `NoDivergentFold` because an unrecovered table has
no writer-side state to diverge from.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), TLA+/TLC (`docs/superpowers/models/`,
runner scripts + `tmp/tla2tools.jar`).

## Global Constraints {#global-constraints}

- Branch: `cas-gc-rebuild`. New commits only — never rebase or amend. Never commit to `master`.
- C++: Allman braces. Never `sleep` to fix a race. CAS is pre-release: **no compat scaffolding** —
  codec format versions may be bumped fail-closed, old versions rejected.
- Builds: `ninja -C <build_dir> unit_tests_dbms` with output redirected to a log file in the build
  directory; never pass `-j`; use a subagent to summarize the log. Some negative tests need a
  `chassert`-active build (debug/ASan).
- Tests: redirect each run to a unique log file in the build directory; use a subagent to summarize.
- gtest run: `./<build_dir>/src/unit_tests_dbms --gtest_filter='<F>'`.
- TLC run (models dir `docs/superpowers/models/`): `./run_refsnaplog.sh`, `./run_mount.sh <cfg>`;
  logs land in repo-root `tmp/`.
- Core code dir (abbreviated `Core/` below):
  `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`.
- Spec constants: observation threshold = `mount_lease_ttl_ms + mount_lease_ttl_ms/20 +
  poll_interval_ms`; `materialization_grace_ms` default `30000`; seal id =
  `RefTxnId{my_epoch - 1, UINT64_MAX}`; snapshot codec format version bumps `1 → 2` adding
  `sealed_from`.
- Commit messages: `cas: <what>`, ending with the `Co-Authored-By: Claude Fable 5
  <noreply@anthropic.com>` trailer.

## Execution order {#execution-order}

Tasks 1–2 (TLA+ gates) first and independent of each other. Then 3 → 4 → 5 → 6 → 7 → 8 (each
consumes the previous task's interfaces). 9 independent after 2. 10 after 8. 11–12 after 10.
13–14 last.

---

### Task 1: TLA+ gate — ref model rev.6 (`NoDivergentFold`) {#task-1}

(amended 2026-07-14: `CoveredFold` fix + `_rev6_latedelivery` expectation corrected to GREEN —
in-flight transient inexpressible under the reader-freeze abstraction; falsifiability held by
`rev6_freshreader`. See the amendment note after Step 5 below for the full explanation and the
verified guard names — do not re-derive the "expected VIOLATION" framing below at face value, it
was superseded.)

**Files:**
- Modify: `docs/superpowers/models/CaRefTableSnapshotLogCore.tla`
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_rev6_safe.cfg`
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_rev6_latedelivery.cfg`
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_rev6_freshreader.cfg`
- Modify: `docs/superpowers/models/run_refsnaplog.sh`

**Interfaces:**
- Consumes: existing model (vars `op, writtenEver, logs, snaps, publishedEver, snapCov, nextId,
  completed, badRecreate`, reader vars, actions `LatePredecessorPut`, invariant `INV_RECOVERY`;
  constant `LatePred`).
- Produces: constant `Rev6MountRule`, ghost var `droppedEver`, oracle `WStateRev6`, invariants
  `NoDivergentFold` (strict) and `INV_FRESH_READER` (weak, for the post-T_mat violation case);
  runner expectation table updated. Later tasks cite these names in code comments only.

- [ ] **Step 1: Add the rev.6 semantics to the model**

In `CaRefTableSnapshotLogCore.tla`:

```tla
CONSTANTS ..., LatePred, Rev6MountRule    \* Rev6MountRule: coverage-at-birth drops a late PUT

VARIABLES ..., droppedEver,   \* ghost: late PUTs that landed under an existing snapshot
          rStartedAfterDrop   \* ghost: reader started after the last late delivery

\* Init adds: droppedEver = {} /\ rStartedAfterDrop = TRUE

\* rev.6 oracle: contract-clean truth excludes never-ACKed writes dropped by coverage-at-birth
WStateRev6 == FoldIds(EmptyState, writtenEver \ droppedEver)

LatePredecessorPut ==
    /\ LatePred = TRUE
    /\ \E L \in Seqs :
        /\ op[L] = "none" /\ L \notin writtenEver
        /\ \E X \in snaps : X > L          \* lands under an existing snapshot
        /\ LifeBelow(L) = "live"
        /\ op' = [op EXCEPT ![L] = "mut"]
        /\ writtenEver' = writtenEver \cup {L}
        /\ logs' = logs \cup {L}
        /\ droppedEver' = IF Rev6MountRule THEN droppedEver \cup {L} ELSE droppedEver
        /\ rStartedAfterDrop' = FALSE      \* any in-flight reader may transiently see L
        /\ UNCHANGED <<snaps, publishedEver, snapCov, nextId, completed, badRecreate,
                       rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts>>

\* ReaderStart additionally sets rStartedAfterDrop' = TRUE

\* Strict: every finished reader reconstructs the rev.6 oracle.
NoDivergentFold == (rPhase = "done" /\ Rev6MountRule) => (Reconstruct = WStateRev6)

\* Weak (post-T_mat violation containment): a reader that STARTED after the last late
\* delivery always agrees. In-flight readers may transiently include the dropped log;
\* folds re-derive each round, error direction is spare-not-delete.
INV_FRESH_READER == (rPhase = "done" /\ Rev6MountRule /\ rStartedAfterDrop)
                        => (Reconstruct = WStateRev6)

\* Snapshot byte-determinism under rev.6: every published body equals the oracle fold below it.
INV_SNAP_DETERMINISTIC == Rev6MountRule =>
    \A X \in snaps : snapCov[X] = FoldIds(EmptyState, {i \in (writtenEver \ droppedEver) : i <= X})
```

**AMENDED 2026-07-14 — load-bearing, do not omit:** the snippet above is not sufficient by itself.
`WriterPublishSnapshot`'s own fold formula must ALSO exclude `droppedEver` under `Rev6MountRule`, or
`INV_SNAP_DETERMINISTIC` is unsatisfiable by construction (any snapshot published at/above a dropped
id, after the drop landed, would silently re-include it — this was caught by the gate itself, not by
inspection, the first time this task ran):
```tla
CoveredFold(X) == IF Rev6MountRule
                   THEN FoldIds(EmptyState, { i \in (writtenEver \ droppedEver) : i <= X })
                   ELSE FoldIds(EmptyState, { i \in writtenEver : i <= X })
\* WriterPublishSnapshot: snapCov' = [snapCov EXCEPT ![X] = CoveredFold(X)]   (was: raw refold)
```

Keep `INV_RECOVERY` for the legacy configs (guard it with `~Rev6MountRule` where the two would
conflict is NOT needed — legacy configs set `Rev6MountRule = FALSE` so `NoDivergentFold` is
vacuous there and `INV_RECOVERY` keeps its old meaning).

- [ ] **Step 2: Write the three new configs**

`CaRefTableSnapshotLogCore_rev6_safe.cfg` — deliveries only pre-coverage (T_mat honored). Model
this by keeping `LatePred = FALSE` (no under-snapshot delivery can occur) and asserting all
invariants:

```text
SPECIFICATION Spec
CONSTANTS MaxSeq = 4  MaxRestarts = 2
  SabotageDeleteBeforeSnapshot = FALSE  SabotageVanishIsCorruption = FALSE
  SabotageRecreateBeforeCompleted = FALSE  SabotageRemountKeepsOldEpoch = FALSE
  LatePred = FALSE  Rev6MountRule = TRUE
INVARIANTS TypeOK INV_RECOVERY INV_NOFAIL INV_RECREATE NoDivergentFold INV_SNAP_DETERMINISTIC INV_FRESH_READER
```

`_rev6_latedelivery.cfg` — the late-delivery-under-an-existing-snapshot demo: `LatePred = TRUE,
Rev6MountRule = TRUE`, INVARIANTS `TypeOK NoDivergentFold` → **AMENDED 2026-07-14: expected GREEN**,
`MaxSeq = 5` (not 4 — see amendment note after Step 5 for why 5, not 4, is the bound to use). The
original brief expected a VIOLATION here ("an in-flight reader transiently sees the dropped log");
that premise does not hold once `WriterPublishSnapshot` correctly excludes `droppedEver` (see the
`CoveredFold` amendment to Step 1 above) — do not re-introduce the raw refold to manufacture a
violation.

`_rev6_freshreader.cfg` — same constants, `MaxSeq = 5`, INVARIANTS
`TypeOK INV_FRESH_READER INV_SNAP_DETERMINISTIC` → expected **GREEN** (fresh observers are
deterministic; snapshots stay byte-deterministic). This config is the regression guard for the
`CoveredFold` fix: it was RED on `INV_SNAP_DETERMINISTIC` before the fix, proving the invariant is
falsifiable, not vacuously green.

Legacy configs are untouched except each existing `.cfg` gains `Rev6MountRule = FALSE`.

- [ ] **Step 3: Run the legacy expectation table (must be unchanged)**

Run: `cd docs/superpowers/models && ./run_refsnaplog.sh > ../../../tmp/tlc_refsnaplog_legacy.log 2>&1`
Expected: same PASS table as before the change (safe green, each sab red on its invariant,
`latepred` red = PASS). Use a subagent to summarize the log.

- [ ] **Step 4: Add the three rev.6 configs to `run_refsnaplog.sh` with their expectations**
(`rev6_safe` → GREEN, `rev6_latedelivery` → **AMENDED: GREEN, not violation-is-PASS**,
`rev6_freshreader` → GREEN) and run again: `./run_refsnaplog.sh > ../../../tmp/tlc_refsnaplog_rev6.log
2>&1`. Expected: full PASS table. If `rev6_freshreader` is red, the model (or the design) has a hole
— STOP and report; do not proceed to implementation tasks. (This is exactly what happened the first
time this task ran, with the original un-amended snippet — see the amendment note below.)

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/models/CaRefTableSnapshotLogCore.tla docs/superpowers/models/*.cfg docs/superpowers/models/run_refsnaplog.sh
git commit -m "cas: TLA ref model rev.6 — NoDivergentFold under coverage-at-birth seal"
```

#### Amendment (2026-07-14): `CoveredFold` fix and the `_rev6_latedelivery` GREEN {#task-1-amendment}

Running this task as originally written produces `rev6_freshreader` RED on
`INV_SNAP_DETERMINISTIC` — the Step-1 snippet transcribed above never propagates `droppedEver` into
`WriterPublishSnapshot`'s own fold, so any snapshot published at/above a dropped id, after the drop
landed, silently re-includes it. The Step 1 section above has been amended in place with the
`CoveredFold` fix; apply that, not the original unfixed formula.

With `CoveredFold` in place, `_rev6_latedelivery` (checking `NoDivergentFold` alone) unexpectedly also
came back GREEN rather than the originally-expected violation, at every bound tested:
- `MaxSeq = 5`, exhaustive: no error, 35,656,456 states generated, 0 left on queue (8s) — a clean
  proof, not a timeout.
- `MaxSeq = 6`, exhaustive: no violation found before being stopped at 426,892,469 states generated /
  13,277,931 states left on queue (still climbing) after ~5 minutes wall time / 10+ GB resident —
  abandoned as impractical rather than left to explore an apparently-empty region indefinitely.
- `MaxSeq = 12`, random simulation (`-simulate num=1000000 -depth 60`, `MaxRestarts=3`): 183,514,671
  states checked across 1,000,000 traces in ~100s, zero violations.

This is not "the bound was too small" — re-tracing the original (pre-fix) `MaxSeq=6` counterexample
showed its violating reader had picked a snapshot whose body wrongly included the dropped id, i.e.
that counterexample was itself the `INV_SNAP_DETERMINISTIC` defect surfacing through a different
invariant. Once the fold is correct, no distinct counterexample has been found.

**Verified guard (do not assume, check `CaRefTableSnapshotLogCore.tla` directly if this drifts):**
`ReaderInactive == rPhase = "idle"` gates exactly six actions — `WriterBirth`, `WriterMut`,
`WriterRemove`, `WriterRebirth`, `WriterFail`, `GcComplete` — the ordinary namespace-lifecycle writes.
Once the model's single global reader starts (`rPhase` only ever moves forward,
idle→scan→fetch→{done,failed,stuck}, never back to idle), none of those six can fire again for the
rest of the run. `ReaderInactive` does **not** gate `WriterPublishSnapshot` (explicitly commented
"off-lane; may run during a reader's recovery"), nor `GcCleanupLog`, `GcCleanupSnap`, or
`LatePredecessorPut` — all four remain concurrent with an in-flight reader, by design. **The green
result does not depend on publish being frozen** (it isn't); it depends on the conjunction of:
1. ordinary writes freezing on reader-start (so any id later dropped must have been created either
   before the reader started, or by the drop action itself — never by a concurrent ordinary write);
2. the model's key space sorting all `_log` keys before all `_snap` keys regardless of id
   (`KeyLt`: kind-major), so the reader's single ordered scan can only fail to enumerate a currently-
   present snapshot if that snapshot did not yet exist at the instant its scan completed;
3. `GcCleanupSnap` never deleting the sole/newest present snapshot (its guard requires a strictly
   greater one to already coexist);
4. `LatePredecessorPut`'s own precondition requiring an existing covering snapshot `X > L` before a
   drop can happen at all — and, because that snapshot was present before or at `L`'s creation, (2)+(3)
   guarantee the reader's scan (whether it started before or after `X` published) will always still
   enumerate `X` or a still-newer replacement before its own scan can complete;
5. `CoveredFold` ensuring every published `snapCov[X]` already excludes `droppedEver` as of its own
   publish time (whether because a not-yet-landed `L` simply wasn't `writtenEver` yet, or because `L`
   was already dropped by the time a later `X` published) — and both `droppedEver` and `writtenEver`
   only grow, never shrink, so this stays true going forward.

Chained together: whatever snapshot a reader ultimately picks is always `>= X > L` for any `L` that
could possibly be dropped during its lifetime, so `L` can only ever land in `Reconstruct`'s *base*
(already excluded by (5)), never its *tail* (which structurally excludes anything `<= rPickedSnap`).
`NoDivergentFold` only constrains `rPhase = "done"` states, so the "in-flight reader transiently sees
it" case the original brief described was never actually reachable in a way this invariant could
observe — the only real failure mode was the `CoveredFold`/`INV_SNAP_DETERMINISTIC` bug, now fixed.

**This is an abstraction-limit finding, not a stronger-than-designed system property.** The real
system's in-flight T_mat-violation transient (accepted, spec's T_mat/seal sections — Tasks 6-8) is not
expressible in this model, because this model serializes the reader against ordinary writes
(property 1 above) in a way the real system does not: a real reader observes a live, concurrently-
mutating table, not a frozen one. `_rev6_latedelivery` is therefore GREEN here as a fact about this
model's reader-freeze abstraction, not a proof that the real system's transient cannot happen.
`INV_FRESH_READER` remains the fresh-observer guarantee the design actually needs; `rev6_freshreader`
is the regression guard for the `CoveredFold` fix (proven falsifiable — it was RED before the fix).

---

### Task 2: TLA+ gate — mount model observation-based reclaim {#task-2}

(amended 2026-07-14, TEN rounds: `notLostOK` made the target violation unreachable (round 1);
mechanical-vs-knowledge split + `GlobalSupersededWriterMakesNoMutation` (round 2); `crashed[a]`, physics
vs politeness (round 3); product-verified `AdoptWrite` body-check (round 4); systematic ordering sweep +
corrected `AllocEpoch` guard (round 5); `_sab_supersededwrites` reframed as a regression canary (round
6); a `heldToken` token-chain fix for review C1 surfaced two further re-arm/collateral findings (round
7); the reconciled package — reclaims install the successor's BODY, `Write` becomes pure-local,
`heldToken` deleted, `ClaimMount` strict-order guard — fixed round 7's findings and RETIRED
`_sab_supersededwrites`/`SabSupersededWrites` (the canary's whole mechanism became inert), but surfaced
that `GcFence` was never made Drift-aware (round 8); `GcFence` fixed, which unmasked that
`ClearExpiredMount` had the identical bare-wall-clock defect (round 9); `ClearExpiredMount` fixed + a
full audit of every clock comparison in the model confirmed no further observer-side gap (round 10) —
**the matrix is now 11 cfgs, all correct, `_rev6_observe` is FULLY GREEN AND EXHAUSTIVE.** See the
amendment notes after Step 4 below for the full account — do not re-derive the "expected VIOLATION of
`SupersededWriterMakesNoMutation`" framing below at face value for `_sab_wallclockreclaim` (dropped from
its invariant list, round 8), do not look for `_sab_supersededwrites.cfg` (retired, round 8), and do not
assume `_rev6_observe`'s expected verdict is merely "`W_ObservedReclaim` reachable, safety unverified
beyond that point" (round 6-era framing) — it is now fully GREEN across all 8 checked invariants,
exhaustively.)

**Files (final, post round-10):**
- Modify: `docs/superpowers/models/CaCasMountCore.tla`
- Create: `docs/superpowers/models/CaCasMountCore_rev6_observe.cfg` (now the fully-GREEN reclaim-faithfulness
  regression detector — see round-8/10 amendments)
- Create: `docs/superpowers/models/CaCasMountCore_sab_wallclockreclaim.cfg`
- Create (round 7): `docs/superpowers/models/CaCasMountCore_witness_observedreclaim.cfg`,
  `CaCasMountCore_witness_recoveryafterobservedreclaim.cfg` (split out of `_rev6_observe.cfg` — a
  monotone witness invariant sharing a cfg with the safety invariants it structurally pre-empts is a
  gate defect, since TLC halts at the FIRST violated invariant)
- **Deleted (round 8):** `docs/superpowers/models/CaCasMountCore_sab_supersededwrites.cfg` — the
  round-6 canary reframe's whole sabotage mechanism became inert once round 8's `ClaimMount` strict-order
  guard landed; retired rather than kept as a vacuous canary (see round-8 amendment)

**Interfaces:**
- Consumes: existing vars (`owner, epoch, mount, mtoken, clock, ...`), invariants
  (`SupersededWriterMakesNoMutation`, `WriterEpochMonotoneUnique`), `TTL` constant.
- Produces: constants `Drift`, `SabWallClockReclaim`; vars `fenceUntil`, `obsToken`, `obsSince`;
  action `ObservedReclaim`; witness `W_ObservedReclaim`.

- [ ] **Step 1: Model clock-rate drift and the two reclaim rules**

**AMENDED 2026-07-14 (second pass, after re-review of `9f2d85e8439`) — this block is a SKETCH of the
key shapes; the CANONICAL text is the committed `CaCasMountCore.tla` plus Amendment 2 below. Rounds
7-10 changed three things relative to the first amendment of this block: (1) `ObservedReclaim` and
`WallClockReclaim` INSTALL the successor body (`mount' = [uuid |-> mount.uuid, epoch |-> epoch',
deadline |-> clock + TTL, fenced |-> FALSE]`) and bump `mtoken` — they are NOT `UNCHANGED
<<mount, mtoken>>`; (2) `Write` is the PURE LOCAL check `~rejected /\ ~wedged /\ ~crashed[a] /\
owner = a /\ clock < fenceUntil` — `epochOK` and `~mount.fenced` are GONE (product: `mayMutate`
reads only local fence fields, `CasStore.cpp:201-205`); (3) `GcFence`/`ClearExpiredMount` are
Drift-aware (`mount.deadline + Drift <= clock`). Do not implement from the sketch below where it
contradicts this note.** In `CaCasMountCore.tla`:

```tla
CONSTANTS ..., Drift,               \* max extra ticks the holder's true fence outlives the stamp
          SabWallClockReclaim       \* TRUE: reclaim trusts the stamped deadline (the old bug)

VARIABLES ..., fenceUntil,     \* holder's TRUE local-fence expiry (stamp + nondet skew <= Drift)
          obsToken, obsSince,  \* reclaimer's observation: token first seen, at which clock tick
          observedReclaimEver, \* history: TRUE once ObservedReclaim has ever completed (witness)
          crashed,             \* [Actors -> BOOLEAN]; MECHANICAL fact (process physically dead),
                                \* set ONLY by Die -- disjoint from localLost (pure KNOWLEDGE)
          supersededThenWrote  \* history: TRUE if Write ever fired while `epoch` (the durable,
                                \* GLOBAL-truth counter) had already advanced past the writer's
                                \* OWN localEpoch[a] -- a completed reclaim it never learned of

\* On every renew/claim by the holder:
\*   mount.deadline' = clock + TTL                      (the stamp others read)
\*   \E d \in 0..Drift : fenceUntil' = clock + TTL + d  (what physics actually guarantees)
\* Write's guard is PURELY MECHANICAL -- epochOK, clock < fenceUntil, ~crashed[a] -- never
\* ~localLost[a] (localLost is KNOWLEDGE and appears in no safety guard anywhere in this model).

StartObservation ==
    /\ mount # None /\ obsToken = None
    /\ obsToken' = mtoken /\ obsSince' = clock
    /\ UNCHANGED <<...>>

\* Bumps ONLY the durable epoch counter (GLOBAL truth) -- deliberately does NOT rewrite `mount`
\* (a separate object, per the model header) and does NOT touch localLost (knowledge is the
\* reclaimer's business alone, never the reclaimed holder's). Observation restarts implicitly:
\* any mount write bumps mtoken; ObservedReclaim requires match.
ObservedReclaim ==
    /\ ~SabWallClockReclaim
    /\ mount # None
    /\ obsToken # None /\ obsToken = mtoken            \* token stable since obsSince
    /\ clock - obsSince >= TTL + Drift                 \* full rate-bound wait on OUR clock
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ obsToken' = None /\ observedReclaimEver' = TRUE
    /\ UNCHANGED <<mount, mtoken, localLost, crashed, fenceUntil, obsSince, ...>>

WallClockReclaim ==   \* the sabotage: trust the stamp, no observation wait at all
    /\ SabWallClockReclaim
    /\ mount # None /\ clock > mount.deadline
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ UNCHANGED <<mount, mtoken, localLost, crashed, fenceUntil, obsToken, obsSince, ...>>

W_ObservedReclaim == ~observedReclaimEver

\* rev.6 GLOBAL-truth witness: `Write`'s honest guard has NO knowledge dependency, so the
\* dedicated regression check for drift/reclaim timing must compare against GLOBAL truth
\* (the durable epoch counter), not local knowledge (localLost, which stays the dedicated
\* regression guard for the UNRELATED SabSupersededWrites axis instead):
Write(a) == LET trulySuperseded == epoch > localEpoch[a] IN
    /\ ... /\ ~crashed[a] /\ clock < fenceUntil /\ ~mount.fenced /\ epochOK
    /\ supersededThenWrote' = (supersededThenWrote \/ trulySuperseded)
    /\ ...
GlobalSupersededWriterMakesNoMutation == ~supersededThenWrote

\* `Die(a)` sets crashed'[a] = TRUE, NEVER localLost (physics, not knowledge). `ClaimMount(a)`
\* resets BOTH localLost' = FALSE and crashed' = FALSE on a successful (re)claim -- the model's
\* only rebirth path for a crashed actor is a process restart via reclaim, so a fresh
\* incarnation must clear the mechanical "dead" fact alongside the knowledge flag.

\* `AdoptWrite`'s "token moved" branch actually implements the "classify by BODY" its comment
\* always claimed, citing the product's real re-read-and-classify (`CasServerRoot.cpp:711-737`):
\*   selfCaused == mount # None /\ mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced
\*   selfCaused -> no loss, no wedge (a benign, self-caused token bump); otherwise localLost' = TRUE

\* `AllocEpoch(a)` gains a guard matching the product's actual `allocateWriterEpoch` invocation
\* discipline (`CasStore.cpp:312-316` strict order; `:413,454-455` fence-recovery reset-before-
\* realloc): blocked while `a` holds its own UNFENCED mount, regardless of wall-clock expiry
\* (`Renew` is legal on a wall-clock-expired-but-unfenced mount -- "the beat-blocked renewal" --
\* so only a genuine FENCE means the epoch was abandoned):
\*   ~(mount # None /\ mount.uuid = a /\ ~mount.fenced)
```

- [ ] **Step 2: Configs.** `_rev6_observe.cfg`: `SabWallClockReclaim = FALSE, Drift = 2, TTL = 3`,
INVARIANTS include `SupersededWriterMakesNoMutation`, `GlobalSupersededWriterMakesNoMutation`, plus
witness config expectation (`W_ObservedReclaim` violated = reachable — this is the actual reported
verdict, confirmed on the final model text). `_sab_wallclockreclaim.cfg`: `SabWallClockReclaim = TRUE`
→ expected **VIOLATION of `GlobalSupersededWriterMakesNoMutation`** (AMENDED — not
`SupersededWriterMakesNoMutation`; see the amendment note for why the knowledge-based invariant is the
wrong target here). **AMENDMENT (round 6):** `_sab_supersededwrites.cfg` (a pre-existing, non-rev.6
legacy cfg, not created by this task) is reframed from a sabotage demo to a **regression canary** —
expected GREEN, exhaustive, with the identical reachable-state count as `_stage1`; see the amendment
note for the full "why" and its new header comment for the canary contract.

- [ ] **Step 3: Run** `./run_mount.sh CaCasMountCore_rev6_observe > ../../../tmp/tlc_mount_rev6.log 2>&1`
and the sabotage config; also re-run the existing `CaCasMountCore_stage1` and `_sab_*` configs
(with `Drift = 0, SabWallClockReclaim = FALSE` added) — legacy table unchanged EXCEPT
`_sab_supersededwrites` (see amendment). Subagent summarizes. **AMENDED:** in addition to the brief's
own two new actions, `CaCasMountCore.tla` needed a systematic ordering sweep across every per-actor
action (`ClaimOwnerEmpty, RejectForeignOwner, AllocEpoch, ClaimMount, AdoptRead, AdoptWrite, Renew, Die,
Write`) checking each one's guard against the product's own sequencing preconditions — full table in
the task report (`.superpowers/sdd/rev6-task-2-report.md`, Round 5 section) — which found and fixed two
further gaps beyond the brief's two new actions: `AdoptWrite`'s missing body-check, and `AllocEpoch`'s
missing live-mount guard.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaCasMountCore.tla docs/superpowers/models/CaCasMountCore_*.cfg
git commit -m "cas: TLA mount model — observation-based reclaim vs wall-clock sabotage"
```

#### Amendment (2026-07-14): mechanical-vs-knowledge split, `crashed[a]`, and two transcription-gap fixes {#task-2-amendment}

Running this task as originally written (the elided `<take over: ...>` placeholders filled in the most
literal way — bump the durable epoch AND rewrite `mount.epoch` on every reclaim) made the target
violation of `SupersededWriterMakesNoMutation` **structurally unreachable**: `Write`'s pre-existing
`notLostOK == ~localLost[a]` guard is a hard requirement whenever it fires, so
`lostThenWrote' = lostThenWrote \/ localLost[a]` can only ever OR in `FALSE` — no reclaim design,
however constructed, can make a "wrongly-declared-dead" holder trip that specific witness through
`Write` as originally guarded. Reported and stopped (round 1) rather than improvise a fix to `Write`.

**Team lead's diagnosis, upheld and encoded:** `Write`'s guard conflated two different things —
MECHANICAL enforcement the system actually performs (the epoch-conditional-write check; the
drift-aware fence-deadline check) and the holder's KNOWLEDGE-based politeness (`~localLost`, "a writer
that learned of its loss stops"). Safety must never rest on (2): the dangerous wall-clock-reclaim
scenario is precisely a holder that does **not** know (its drifted clock says the lease is fine) while
global truth has already superseded it. The fix (round 2): drop `~localLost[a]` from `Write` entirely,
keep the mechanical `clock < fenceUntil` clause, and add a GLOBAL-truth witness
(`supersededThenWrote`/`GlobalSupersededWriterMakesNoMutation`, `epoch > localEpoch[a]`) alongside the
unchanged, still-meaningful knowledge-based one (`lostThenWrote`/`SupersededWriterMakesNoMutation`,
kept as the dedicated regression guard for the **unrelated** `SabSupersededWrites` bug class —
dropping the live epoch-match check — a distinct axis from drift/reclaim-timing). Confirmed via an
isolated probe before trusting it further: with the invariant list narrowed to
`GlobalSupersededWriterMakesNoMutation` alone, a real search (912K states) found exactly the intended
story — a wall-clock-trusting reclaimer declares death one tick early; the true holder's `Drift`-extended
local fence is still valid; its next write lands after global truth already moved on.

**Three further "politeness masked as safety" instances surfaced by the full matrix, each closed in
turn, none invented — every one found first as a RED legacy cfg, diagnosed from its trace, then fixed:**

1. **`Die` (round 3).** `Die(a)` was setting `localLost'[a] = TRUE` to mean "this actor crashed", but
   `Write`'s new mechanical-only guard no longer stopped on that flag, so a "dead" actor could still
   legitimately write forever via the shallowest possible trace (claim → die → write). Fix: a dedicated
   `crashed[a]` variable — a MECHANICAL fact, physics not knowledge — set by `Die`, checked by `Write`
   (`~crashed[a]`), reset by `ClaimMount` on a successful (re)claim (the model's only crash-rebirth path
   is a process restart via reclaim; without this reset, `witness_reclaim`/`witness_remountafterfence`
   would become permanently unable to write again post-recovery). `localLost` stays pure knowledge,
   touched by `Die` never again.

2. **`AdoptWrite` (round 4).** Its "token moved" branch set `localLost'[a] = TRUE` unconditionally,
   despite its own comment's claim to "re-read and classify by BODY". Went to the product FIRST:
   `MountLeaseKeeper::claim` (`CasServerRoot.cpp:711-737`) genuinely does re-read and classify (checking
   `server_uuid`/`gc_fenced`) — but the product's own renewal thread structurally **cannot** interleave
   with its own adopt (`state_mutex` serializes `claim()`/`renewOnce()`, `CasSingleWriterSlot.cpp:42-76`,
   and the renewal thread doesn't exist until `startBackground()` is called strictly after `start()`
   returns, `CasStore.cpp:428-473`) — so this specific counterexample (the SAME actor's `Renew` sneaking
   between its own `AdoptRead`/`AdoptWrite`) is a pure **model transcription gap**: the `.tla`'s two-step
   decomposition of `claim()` into separately-schedulable `Next`-disjunction actions doesn't preserve the
   real function's atomicity. Fixed anyway, to the target design semantics the comment always
   documented: `selfCaused == mount # None /\ mount.uuid = a /\ mount.epoch = localEpoch[a] /\
   ~mount.fenced` — self-caused, no loss; otherwise, `localLost' = TRUE` as before. **Note for this
   amendment (superseding round 4's "possible backlog" framing):** the product's actual response to a
   same-identity-but-unexplained mismatch is a hard, fail-closed `LOGICAL_ERROR` (`CasServerRoot.cpp:729-733`;
   the analogous renewal-mismatch comment, `:768-769`, explicitly says *"same (uuid, epoch) unfenced — no
   plausible classification — falls through to the base's generic throw"*) — this is **intended design**,
   the product authors already considered the case and chose fail-closed per the standing
   anomaly-response principle. Not a backlog item.

3. **`AllocEpoch` (round 4-5).** Its ONLY guard was `owner = a`, with no check against `a` already
   holding a live mount — so `AllocEpoch(a)` could fire mid-flight, bumping `localEpoch[a]` away from
   `mount.epoch` while the actor's own unfenced mount sat unchanged, making `Renew`'s different-epoch
   branch false-alarm `localLost` on the actor's own benign epoch bump. Checked the product:
   `allocateWriterEpoch` (`CasServerRoot.cpp:234`) is invoked exactly once, strictly before any mount
   exists for the current `open()` attempt (`CasStore.cpp:312-316`'s own "STRICT ORDER" comment), or
   after the fence-recovery loop's `continue` — and that loop explicitly does `mount_keeper.reset()`
   (`CasStore.cpp:454-455`) before reallocating, and only re-enters on `FencedSelf`/`MountFencedException`
   (`:413`, `:448`) — **never** while an already-adopted, live, unfenced mount is held. Same
   transcription-gap class as (2), second instance. **First fix attempt was itself wrong** (self-caught,
   not asserted correct on the first try): guarding on wall-clock expiry
   (`mount.deadline > clock`) still let `stage1.cfg` flip, with `AllocEpoch` firing exactly AT the
   boundary (`clock = mount.deadline`, so the strict `>` was false there). Root cause: wall-clock expiry
   is not the real gate — `Renew` is explicitly legal on a wall-clock-expired-but-unfenced mount ("the
   beat-blocked renewal", the model's own pre-existing comment), so only a genuine FENCE means the actor
   abandoned that epoch. Corrected, fence-only guard: `~(mount # None /\ mount.uuid = a /\
   ~mount.fenced)`.

**The systematic ordering sweep (round 5).** Once two independent instances of "the model permits an
ordering the product's control flow forbids" had surfaced, every per-actor action was checked against
its own product-side sequencing precondition, specifically for gaps that could fabricate a false
`localLost` or an impossible mechanical state (full table with file:line citations in
`.superpowers/sdd/rev6-task-2-report.md`, Round 5 section):

| Action | Faithful? |
|---|---|
| `ClaimOwnerEmpty`, `RejectForeignOwner` | Yes — identity-only, dependency already structural (`owner = a` guards) |
| `AllocEpoch` | **No — fixed this round** (see above) |
| `ClaimMount` | Yes — its own decision table (`canClaim`/`refreshOK`/`fencedReclaim`/`expired`) already IS the product's real claim logic; already clears `adoptObs` on (re)claim |
| `AdoptRead` | Yes, inert-if-early — naturally blocked by its own `mount.epoch = localEpoch[a]` guard if fired "too early"; produces no fabricated state |
| `AdoptWrite` | **Fixed round 4** |
| `Renew` | Yes, inert-if-early — firing before any adopt just produces an ordinary successful renewal; the actual false-alarm path (different-epoch branch) was entirely `AllocEpoch`'s gap, now closed |
| `Die` | Yes — no product counterpart (crash abstraction); re-firing on an already-crashed actor is idempotent, harmless |
| `Write` | Yes — `clock < fenceUntil` is structurally false until `fenceUntil` is first set by a successful claim/renew/adopt, so "write before ever claiming" is already impossible |

Exactly one gap found (`AllocEpoch`), matching round 4 — the sweep did not turn up a third,
independent ordering gap among the nine per-actor actions.

**`_sab_supersededwrites`: canary, not sabotage (round 6, adjudicated).** Fixing `AllocEpoch` closed the
ONLY reachable path that let `mount.epoch # localEpoch[a]` arise for the current holder while unfenced —
`ClaimMount`/`AdoptWrite` always write both together; the corrected `AllocEpoch` guard only fires when
the mount is absent, foreign (impossible for the sticky owner), or fenced (and fenced independently
blocks `Write` via `~mount.fenced` regardless of `epochOK`); `ObservedReclaim`/`WallClockReclaim` touch
only the durable `epoch` counter, never `mount.epoch`/`localEpoch[a]`. So `SabSupersededWrites` — this
cfg's whole sabotage mechanism — became **provably vacuous**: it now generates the IDENTICAL exhaustive
state count as `_stage1.cfg` (`11,642,531` states / `2,596,245` distinct / `0` left on queue, both cfgs).
**This is an abstraction-limit finding, not a real absence of the underlying risk — the same class as
Task 1's `_rev6_latedelivery` adjudication above.** In the product, `epochOK`'s load-bearing role is
fencing a LATE-LANDING in-flight write (an old incarnation's already-in-flight S3 request landing AFTER
a takeover bumped the epoch — the conditional write IS the fence); this model's `Write` is instantaneous
with no in-flight/late-arrival phase, so that failure source is inexpressible here. Resolution: keep the
cfg, keep it in the runner, change its expectation to GREEN, and reframe its header as a **regression
canary**: if it ever goes RED, an epoch-divergence path has re-opened in the model (almost certainly an
ordering guard regressed) — investigate before trusting the rest of the matrix. Do not manufacture a new
adversary mechanism to force it red again, and do not retire it.

**Final full-matrix result at round 6 (10 official cfgs, now SUPERSEDED — see the round 7-10 amendment
below):** `_stage1` green (exhaustive); `_sab_foreigntakeover` → `ForeignUuidNeverAutoTakesOver`;
`_sab_epochreset` → `WriterEpochMonotoneUnique`; `_sab_supersededwrites` → **green (canary)**, identical
state count to `_stage1`; `_sab_adoptwedge` → `NoPermanentWedge`; `_sab_fenceresurrect` →
`FenceCostsEpoch`; `_witness_reclaim` → `W_SameUuidReclaimsExpired`; `_witness_remountafterfence` →
`W_RemountAfterFence`; `_rev6_observe` → `W_ObservedReclaim` (all safety invariants hold up to that
point); `_sab_wallclockreclaim` → `GlobalSupersededWriterMakesNoMutation` (the drift trace: a holder
claims with `fenceUntil = clock + TTL + Drift`; a wall-clock-trusting reclaimer fires one tick before the
true fence expires, advancing global truth; the holder's next write is still mechanically legitimate by
its own local clock and lands after the reclaim — exactly the story this whole task exists to catch).

#### Amendment 2 (2026-07-14, rounds 7-10): body-faithful reclaim, pure-local `Write`, and closing the observer-side wall-clock class {#task-2-amendment-2}

Round 6 left `_rev6_observe`'s expectation at "`W_ObservedReclaim` reachable, safety invariants hold up
to that point" — a witness-masked cfg, not an actual proof that `ObservedReclaim` preserves safety past
the reclaim. Making that cfg an honest, fully-checked GREEN took four more rounds, each one following
the same discipline as rounds 1-6: implement exactly what was adjudicated, run the FULL cfg matrix (not
just the cfgs named), and if ANY cfg deviates from its expected verdict — STOP and report with a full
trace, never invent a fix unilaterally. Full blow-by-blow (every trace, every probe, every candidate
considered) is in `.superpowers/sdd/rev6-task-2-report.md`; this amendment records the final story.

**Round 7 — the token-chain fix, and two new findings.** An independent review flagged Critical C1: the
model's reclaim (`ObservedReclaim`/`WallClockReclaim`) bumped only the durable `epoch` counter,
deliberately leaving `mount` untouched (round 2's choice, to avoid masking the drift bug) — but this made
`_rev6_observe`'s honest-path post-reclaim exploration structurally unable to show the OLD holder's
continuation actions (`Renew`, `ClaimMount`-refresh, `AdoptRead`/`AdoptWrite`) correctly refuse to re-arm,
because none of those actions' body-checks could see anything had changed. A design consult (working
independently, reaching the same diagnosis) recommended a compensating `heldToken` token-chain field,
threaded through `Renew`, `ClaimMount`'s `refreshOK`, and `AdoptWrite`'s success condition. Implemented
verbatim — and the full matrix surfaced two further findings from the SAME root cause (a body-invisible
reclaim needing ad hoc compensation): **Finding A**, a fourth re-arm path via `ClearExpiredMount` +
ungated fresh-mint that no prior round's guard blocked; **Finding B**, the new `heldToken` conjunct on
`AdoptWrite` collaterally blocked `_sab_fenceresurrect.cfg`'s intended violation (a plain `GcFence` token
bump, unrelated to any reclaim, spuriously tripped the new check).

**Round 8 — the reconciled package: reclaims install the successor's BODY.** Two independent follow-up
passes (a design consult, and the same reviewer working separately) converged on the same deeper
diagnosis: the model's reclaim was body-invisible while the product's real reclaim
(`CasServerRoot.cpp:405-414`) is a token-guarded `putOverwrite` that installs the SUCCESSOR's fresh-epoch,
unfenced body. Fix the body, and the model's PRE-EXISTING body classification (present since round 0/P3.1
in `Renew`/`ClaimMount`/`AdoptRead`) does the re-arm-blocking work for free — no compensating field
needed. Implemented the reconciled package (P1-P4):
- **P1**: `ObservedReclaim`/`WallClockReclaim` install `mount' = [uuid |-> mount.uuid, epoch |-> epoch',
  deadline |-> clock + TTL, fenced |-> FALSE]` on top of the epoch/mtoken bump.
- **P2 (full)**: `Write` becomes a PURE LOCAL check — `~rejected /\ ~wedged /\ ~crashed[a] /\ owner = a
  /\ clock < fenceUntil` — matching the product's real `Store::mayMutate`/`refAppendFenceOk`
  (`CasStore.cpp:201-226`, which reads no shared state at all); `mount # None`, `mount.uuid = a`,
  `epochOK`, `~mount.fenced` all removed as unfaithful per-write body reads the product never performs.
  `owner = a` kept as the identity anchor (a clock-free, constant proxy for the writer's cached identity).
- **P3**: `heldToken` (round 7) deleted entirely — no longer needed once P1 makes the body itself the
  re-arm signal.
- **P4**: `ClaimMount` gains `strictOK == localEpoch[a] >= epoch` on its THREE claim-establishing
  disjuncts (fresh mint, `fencedReclaim`, `expired /\ ~sameEpoch`), mirroring `CasStore.cpp:312-316`'s
  STRICT ORDER — this closes Finding A.
- **Retired**: `SabSupersededWrites` (the constant) and `_sab_supersededwrites.cfg` (the round-6 canary)
  — with `epochOK` gone from `Write` by construction, the flag has nothing left to toggle; deleting it
  (rather than leaving a permanently-vacuous canary in the runner) was the team lead's explicit call.
  `SupersededWriterMakesNoMutation`/`localLost`/`lostThenWrote` are KEPT (still live readers — see the
  task report's round-8 kept-vs-removed table) but dropped from `_sab_wallclockreclaim.cfg`'s invariant
  list (a faithful reclaim body means a `Renew`-then-`Write` drift path COULD trip it there, but only
  behind the shallower `GlobalSupersededWriterMakesNoMutation` violation TLC always finds first — kept
  there would invite "why is this green" confusion).

This closed Finding A and Finding B, and preserved the load-bearing drift asymmetry
(`_sab_wallclockreclaim` still correctly violates `GlobalSupersededWriterMakesNoMutation`, since P2's
pure-local `Write` never reads the reclaim's newly-installed body at all — only the reclaim's
PRECONDITION differs between the honest and sabotage forms, never what it writes). But the full matrix
surfaced a **third finding**: `_rev6_observe.cfg` was STILL RED on `SupersededWriterMakesNoMutation`, via
`GcFence` — a PRE-EXISTING, round-0/P3.1 action never updated for rev.6, using a bare wall-clock check
(`mount.deadline <= clock`) that could fence a mount WHILE the true holder's `fenceUntil` (which includes
`Drift`) had not yet expired. Previously invisible because `Write`'s old `~mount.fenced` conjunct
(removed by P2) blocked any write once fenced regardless of cause; P2's faithfulness unmasked it. The
design spec's own Decision Log had already called for this ("GC fence-out becomes observation-based,
and a `gc_fenced` lease is then a transferable certificate of observed death") — a genuine scope gap in
the ORIGINAL Task 2 brief (which only covered the reclaim side), not an error in round 8's work.

**Round 9 — `GcFence` made Drift-aware; a second, deeper gap surfaces.** Adjudicated as the MINIMAL
reduction (no new observation-state machinery): the observation threshold "stamp silent for `TTL +
Drift` on the GC's own clock" reduces algebraically to "the clock has passed the holder's MAXIMUM
possible `fenceUntil`", so `GcFence`'s guard became `mount.deadline + Drift <= clock`. This makes
`GcFence` and `Write` mutually exclusive on the same mount BY CONSTRUCTION for every `Drift` (proof in
the model text, `CaCasMountCore.tla`'s `GcFence` comment) — byte-identical to the old guard at `Drift =
0`, empirically confirmed (`_stage1.cfg`'s state counts matched round 8's exactly, both runs). Fixing
`GcFence` closed that path — but TLC then found a FOURTH, previously-masked violation through
`ClearExpiredMount`, a separate, also pre-existing action with the IDENTICAL unsound pattern (bare
`mount.deadline <= clock`, feeding `AdoptWrite`'s "vanished while adopting" fallback into a false
`localLost` trip). TLC's first-violation-only behavior had hidden this the whole time — `GcFence`'s
violation was always shallower and found first.

**Round 10 — `ClearExpiredMount` fixed, and the class closed by audit, not by luck.** Adjudicated:
decision #2's principle is not specific to the fence-out flavor — ANY observer-side death verdict on a
mount (fence it, clear it, reclaim it) must clear the same `TTL + Drift` observation threshold before
concluding the record is dead. `ClearExpiredMount`'s guard became `mount.deadline + Drift <= clock`
(identical proof, identical `Drift = 0` byte-identical property), with its comment explicitly
principle-cited rather than function-cited (unlike `GcFence`/`computeHeartbeatFloor`, no single named
product function maps to `ClearExpiredMount` — it is GC bookkeeping on an already-presumed-dead record).
A full audit then classified EVERY clock comparison in the model as HOLDER-side (uses its own
`fenceUntil`, legitimately local) or OBSERVER-side (declares a possibly-different, still-alive party
dead — must be Drift-aware):

| Site | Classification | Verdict |
|---|---|---|
| `Write`'s `clock < fenceUntil` | HOLDER-side | the writer's own ground truth — correct as is |
| `GcFence`'s `mount.deadline + Drift <= clock` | OBSERVER-side | Drift-aware (round 9) |
| `ClearExpiredMount`'s `mount.deadline + Drift <= clock` | OBSERVER-side | Drift-aware (round 10) |
| `ObservedReclaim`'s `clock - obsSince >= TTL + Drift` | OBSERVER-side | already Drift-aware by design (the reference form) |
| `WallClockReclaim`'s `clock > mount.deadline` | OBSERVER-side | the DELIBERATE sabotage this suite exists to catch (`SabWallClockReclaim`-gated, never TRUE honestly) — not "fixed" |
| `ClaimMount`'s `expired` (`mount.deadline <= clock`) | HOLDER-side | in the honest protocol `ownerOK` forces `mount.uuid = a = owner` (sticky) — `a` examining its OWN record, always installs a FRESH `fenceUntil` for itself regardless of the verdict, so no second party's stale fence is ever ignored; the one place a DIFFERENT actor could take over on this check (`SabForeignTakeover`'s bypass) is the deliberate sabotage `ForeignUuidNeverAutoTakesOver` exists to catch. Also separately reachability-vacuous in honest mode (see the model's own comment on `ClaimMount`) |
| `Tick`'s `clock < MaxClock` | n/a | pure TLC finiteness bound, not a death declaration |

Exactly two observer-side bare wall-clock sites existed (`GcFence`, `ClearExpiredMount`); both are now
Drift-aware; no third was found. `_rev6_observe.cfg` is now **fully GREEN and EXHAUSTIVE**:
2,020,927,571 states generated, 256,398,167 distinct states found, 0 states left on queue (~4m57s wall
time) — the matrix is trustworthy against this class by audit, not by luck.

**Final full-matrix result (11 cfgs, all correct, mechanism-verified on the final model text):**
`_stage1` green, exhaustive (12,877,827 states / 2,826,549 distinct / 0 left on queue — IDENTICAL at
`Drift = 0` across rounds 9 and 10, confirming both Drift-aware guards are byte-identical to their old
forms there); `_sab_foreigntakeover` → `ForeignUuidNeverAutoTakesOver`; `_sab_epochreset` →
`WriterEpochMonotoneUnique`; `_sab_adoptwedge` → `NoPermanentWedge`; `_sab_fenceresurrect` →
`FenceCostsEpoch` (via `ClaimMount`'s `refreshOK` bypass under `SabAdoptIgnoresFence` — the SHORTEST
counterexample TLC's breadth-first search reports; earlier rounds' reports mischaracterized this
mechanism as routing through `AdoptWrite`, corrected in the round-10 report); `_witness_reclaim` →
`W_SameUuidReclaimsExpired`; `_witness_remountafterfence` → `W_RemountAfterFence`; `_rev6_observe` →
**fully GREEN, exhaustive, all 8 checked invariants** (`TypeOK`, `NoTwoServerUuidsOwnSameServerRoot`,
`ForeignUuidNeverAutoTakesOver`, `WriterEpochMonotoneUnique`, `SupersededWriterMakesNoMutation`,
`GlobalSupersededWriterMakesNoMutation`, `FenceCostsEpoch`, `NoPermanentWedge`);
`_witness_observedreclaim` → `W_ObservedReclaim`; `_witness_recoveryafterobservedreclaim` →
`W_RecoveryAfterObservedReclaim` (the anti-wedge/anti-dead-end check — a reclaimed incarnation's
legitimate recovery loop still completes); `_sab_wallclockreclaim` →
`GlobalSupersededWriterMakesNoMutation` (mechanism re-confirmed: routes through `WallClockReclaim` →
`Write` directly, no `GcFence`/`ClearExpiredMount` step in the trace).

---

### Task 3: `sealed_from` in the snapshot codec (format v2) {#task-3}

**Files:**
- Modify: `Core/CasRefSnapshotCodec.h` (struct at `:41-51`, format doc `:58-62`)
- Modify: `Core/CasRefSnapshotCodec.cpp` (`kRefTableSnapshotFormatVersion` `:27`,
  `encodeRefTableSnapshot` `:184`, `decodeRefTableSnapshot` `:211`, `checkSnapshotInvariants` `:161`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp` (round-trip tests near the existing codec tests)

**Interfaces:**
- Consumes: `RefTxnId` (`Core/CasRefIds.h:27-30`).
- Produces: `RefTableSnapshot::sealed_from` — `std::optional<RefTxnId>`; format version 2 (v1
  rejected fail-closed, CAS is pre-release); invariant: if `sealed_from` present then
  `*sealed_from <= snapshot_id`.

- [ ] **Step 1: Write the failing round-trip test**

```cpp
TEST(RefSnapshotCodec, SealedFromRoundTripsAndOrderingEnforced)
{
    DB::Cas::RefTableSnapshot s;
    s.ns = "ns1";
    s.snapshot_id = DB::Cas::RefTxnId{2, UINT64_MAX};       /// a seal id: (epoch-1, MAX)
    s.lifecycle = DB::Cas::RefLifecycle::Live;
    s.sealed_from = DB::Cas::RefTxnId{2, 17};               /// greatest listed id
    const auto bytes = DB::Cas::encodeRefTableSnapshot(s);
    const auto back = DB::Cas::decodeRefTableSnapshot(bytes, "ns1", s.snapshot_id);
    EXPECT_EQ(back, s);

    s.sealed_from = DB::Cas::RefTxnId{3, 1};                /// > snapshot_id: must throw
    EXPECT_ANY_THROW(DB::Cas::encodeRefTableSnapshot(s));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_t3.log 2>&1` — fails to compile
(`sealed_from` not a member). Subagent confirms the error is the expected one.

- [ ] **Step 3: Implement**

`CasRefSnapshotCodec.h`: add `std::optional<RefTxnId> sealed_from;` to `RefTableSnapshot` (keep
`operator== = default`); extend the wire-format doc comment: `... u8 lifecycle |
[remove_txn_id if Removed] | u8 has_sealed_from | [sealed_from] | u32 n_committed | ...`.
`CasRefSnapshotCodec.cpp`: `kRefTableSnapshotFormatVersion = 2`; encode/decode the presence byte +
id; `checkSnapshotInvariants`: throw `LOGICAL_ERROR` if `sealed_from && snapshot_id < *sealed_from`;
decoder rejects any version `!= 2` (fail-closed, no v1 acceptance).

- [ ] **Step 4: Run to verify it passes**

Run: `./build_debug/src/unit_tests_dbms --gtest_filter='RefSnapshotCodec.*' > build_debug/test_t3.log 2>&1`
Expected: PASS. Also run the full existing ref suite (`--gtest_filter='RefWriter*'`) — the version
bump must not break any test (helpers re-encode with v2 automatically).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.cpp src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: snapshot codec v2 — sealed_from upper-bound metadata for the recovery seal"
```

---

### Task 4: Observation-based lease reclaim {#task-4}

**Files:**
- Modify: `Core/CasServerRoot.h` (`MountClaimResult` `:160-175`, `claimMount` decl `:190-192`,
  `claimMountAwaitingExpiry` decl `:212-218`, decision-table comment `:143-159`)
- Modify: `Core/CasServerRoot.cpp` (`claimMount` `:337-415`, `claimMountAwaitingExpiry` `:437-475`)
- Modify: `Core/CasStore.cpp` (call sites `:381` in open, `:635` in remount — pass the new args)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `Backend::get` returning `{body, token}`; `putOverwrite(key, body, token)`.
- Produces:
  ```cpp
  enum class MountPriorState { None, Clean, Fenced, UncleanObserved };
  struct MountClaimResult { Kind kind; MountLease body; MountPriorState prior; };
  MountClaimResult claimMount(Backend &, const Layout &, const String & srid,
      UInt128 our_uuid, uint64_t our_epoch, uint64_t now_ms, uint64_t ttl_ms,
      const std::optional<Token> & proven_dead_token = {}, const CasEventSink & sink = {});
  MountClaimResult claimMountAwaitingExpiry(Backend &, const Layout &, const String & srid,
      UInt128 our_uuid, uint64_t our_epoch,
      const std::function<uint64_t()> & now_ms_fn,          // wall: stamping/diagnostics only
      const std::function<uint64_t()> & mono_ms_fn,         // observation clock
      uint64_t ttl_ms, uint64_t poll_interval_ms,
      const std::function<void(uint64_t)> & sleep_ms_fn,
      const std::function<void(const MountLease &, uint64_t)> & on_wait_start = {},
      const CasEventSink & sink = {});
  ```
  Semantics: same-uuid different-epoch **not fenced, not clean-marked** lease is never reclaimed by
  wall clock; `claimMount` returns `LiveDoubleStart` for it unless `proven_dead_token` matches the
  current token (then token-guarded reclaim). `gc_fenced` and clean-marker (`min_active ==
  UINT64_MAX`) branches reclaim immediately as today, with `prior = Fenced` / `Clean`.
  `claimMountAwaitingExpiry` implements the observation loop and stamps `prior = UncleanObserved`
  on an observed takeover. Observation threshold: `ttl_ms + ttl_ms / 20 + poll_interval_ms` on
  `mono_ms_fn`. Foreign uuid: unchanged (`ForeignOwner`, never waited).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasMountObservation, ExpiredLookingLeaseIsNotReclaimedByWallClock)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout l{"p"};
    /// Predecessor epoch 7 stamped expires_at_ms = 1000; our wall clock says 999999 (long past).
    auto first = DB::Cas::claimMount(*b, l, "r", UInt128(1), 7, /*now_ms=*/500, /*ttl_ms=*/500);
    ASSERT_EQ(first.kind, DB::Cas::MountClaimResult::Claimed);
    auto r = DB::Cas::claimMount(*b, l, "r", UInt128(1), /*our_epoch=*/8, /*now_ms=*/999999, 500);
    EXPECT_EQ(r.kind, DB::Cas::MountClaimResult::LiveDoubleStart);  /// no wall-clock trust
}

TEST(CasMountObservation, TokenStableForThresholdThenReclaimed)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout l{"p"};
    ASSERT_EQ(DB::Cas::claimMount(*b, l, "r", UInt128(1), 7, 500, 500).kind,
              DB::Cas::MountClaimResult::Claimed);
    uint64_t mono = 0;
    std::vector<uint64_t> sleeps;
    auto r = DB::Cas::claimMountAwaitingExpiry(*b, l, "r", UInt128(1), 8,
        []{ return uint64_t{999999}; },                 /// wall clock: irrelevant
        [&]{ return mono; },                            /// observation clock
        /*ttl_ms=*/500, /*poll_interval_ms=*/50,
        [&](uint64_t ms){ sleeps.push_back(ms); mono += ms; });
    EXPECT_EQ(r.kind, DB::Cas::MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, DB::Cas::MountPriorState::UncleanObserved);
    EXPECT_GE(mono, 500 + 500 / 20 + 50);               /// full threshold actually waited
}

TEST(CasMountObservation, RenewalDuringObservationRestartsIt)
{
    /// Same setup; a MountLeaseKeeper for epoch 7 renews once mid-observation (token bump).
    /// Expect: the successor does NOT take at the first threshold from t0; it needs a full
    /// threshold of stability AFTER the renewal. Assert via total mono elapsed >= 2 windows - poll.
}

TEST(CasMountObservation, GcFencedIsReclaimedInstantlyWithPriorFenced)
{
    /// Fence the lease via computeHeartbeatFloor's fence write (or a manual putOverwrite with
    /// gc_fenced=true, seq+1), then claimMount with our_epoch=8: Claimed immediately,
    /// prior == MountPriorState::Fenced, no observation sleep.
}
```

(Write the two sketched bodies fully in the test file, following the pattern of the first two —
lease bodies via `claimMount`, keeper via the `MountLeaseKeeper` ctor as in
`gtest_cas_mount.cpp:117-130`.)

- [ ] **Step 2: Run to verify failure**

`ninja -C build_debug unit_tests_dbms > build_debug/build_t4.log 2>&1` — compile fails
(`MountPriorState` unknown / signature mismatch). Then after stubbing, behavioral tests fail on
the old wall-clock reclaim. Subagent confirms.

- [ ] **Step 3: Implement**

`claimMount`: replace the `expires_at_ms <= now_ms` reclaim condition (`cpp:397-414`). New branch
logic for same-uuid different-epoch:

```cpp
const bool clean_marker = got_lease.min_active == UINT64_MAX;
if (got_lease.gc_fenced || clean_marker
    || (proven_dead_token && *proven_dead_token == got->token))
{
    /// token-guarded reclaim exactly as before (putOverwrite with got->token, seq+1)
    result.prior = got_lease.gc_fenced ? MountPriorState::Fenced
                 : clean_marker        ? MountPriorState::Clean
                                       : MountPriorState::UncleanObserved;
    ...
}
else
    return {MountClaimResult::LiveDoubleStart, got_lease, MountPriorState::None};
```

`claimMountAwaitingExpiry`: observation loop replacing the wall-deadline wait (`cpp:437-475`):

```cpp
const uint64_t threshold_ms = ttl_ms + ttl_ms / 20 + poll_interval_ms;
std::optional<Token> observed;
uint64_t observed_since = 0;
size_t restarts = 0;
while (true)
{
    auto r = claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms,
        (observed && mono_ms_fn() - observed_since >= threshold_ms) ? observed : std::nullopt,
        sink);
    if (r.kind != MountClaimResult::LiveDoubleStart)
        return r;
    const auto got = b.get(l.mountKey(srid));
    if (!got) continue;                            /// vanished: next claimMount claims fresh
    if (!observed || *observed != got->token)
    {
        if (observed && ++restarts > kMaxObservationRestarts)   /// = 3, holder is alive
            return r;                                            /// LiveDoubleStart to caller
        observed = got->token;
        observed_since = mono_ms_fn();
        if (on_wait_start)
            on_wait_start(r.body, threshold_ms);
        LOG_INFO(getLogger("CasMountLease"),
            "Attempting to mount content-addressed server root {} after node change or hard "
            "restart; waiting ~{} ms (token-stability observation) to confirm the previous "
            "incarnation's operations are all finalized", srid, threshold_ms);
    }
    sleep_ms_fn(poll_interval_ms);
}
```

Update both call sites in `CasStore.cpp` (`:381`, `:635`) to pass
`mono_ms_fn = [this]{ return bootMsNow(); }` and drop the old `margin_ms` argument. Update the
decision-table comment at `CasServerRoot.h:143-159` to the new rules.

- [ ] **Step 4: Run to verify pass**

`./build_debug/src/unit_tests_dbms --gtest_filter='CasMountObservation.*:CasMount*:CasHeartbeat*:CasStore*' > build_debug/test_t4.log 2>&1`
Expected: new tests PASS; pre-existing suites PASS except tests that asserted wall-clock reclaim —
rewrite those to the observation semantics (e.g. a test claiming instantly on expired stamp now
asserts `LiveDoubleStart`). Subagent lists every pre-existing test it had to touch, with reasons.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_mount.cpp
git commit -m "cas: observation-based lease reclaim — no cross-node wall-clock trust on takeover"
```

---

### Task 5: Clean-release drain gates the farewell marker {#task-5}

**Files:**
- Modify: `Core/CasStore.h` (Store members; near `wedgedRefLaneCount` decl `:819`)
- Modify: `Core/CasStore.cpp` (`~Store` `:461-504`, `mutateRef` entry, new drain method)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `RefTableRuntime::{pending, leader_active, cv, wedge, state_mutex}`; `ref_queue_mutex`;
  `mount_keeper->stop()` (farewell) vs `mount_keeper->stopBackground()` (no terminal op).
- Produces:
  ```cpp
  /// Stops admitting new ref mutations, waits (bounded) for queues to empty and leaders to
  /// exit, then reports whether any unresolved conditional PUT (wedge) remains.
  /// true = drained: no in-flight ref-log PUT can exist; farewell marker is safe.
  bool Store::drainRefLanesForShutdown(uint64_t wait_budget_ms);
  std::atomic<bool> Store::shutting_down{false};   // checked at mutateRef admission
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasStoreShutdown, CleanStopDrainsAndWritesFarewell)
{
    /// Open a writable Store over InMemoryBackend, append one committed ref txn,
    /// destroy the Store; then decode the mount lease body:
    /// EXPECT min_active == UINT64_MAX (farewell written — drain succeeded).
}

TEST(CasStoreShutdown, UnresolvedWedgeSkipsFarewell)
{
    /// Use failNextCasPut / a ThrowingSingleAttemptBackend-style subclass so the ref-log PUT
    /// ends Unresolved (wedge set, as in existing wedge tests), then destroy the Store:
    /// EXPECT the lease body's min_active != UINT64_MAX (no farewell), and gc_fenced == false.
    /// A successor claimMount on this body must return LiveDoubleStart (unclean path).
}
```

- [ ] **Step 2: Run to verify failure** — first test may already pass (stop() writes farewell
unconditionally today); the second MUST fail (farewell currently written despite the wedge).
`--gtest_filter='CasStoreShutdown.*'`, log to `build_debug/test_t5_fail.log`.

- [ ] **Step 3: Implement**

`drainRefLanesForShutdown`: set `shutting_down = true` (new ref mutations throw
`ABORTED` "store is shutting down"); snapshot runtimes under `ref_queue_mutex`; for each, wait on
`cv` until `pending.empty() && !leader_active`, bounded overall by `wait_budget_ms` (use
`cv.wait_for` slices; no sleeps); then under each `state_mutex` collect `wedge.has_value()`.
Return `!any_wedge && !timed_out`. In `~Store` (`:461-504`), before `mount_keeper->stop()`:

```cpp
const bool drained = drainRefLanesForShutdown(
    config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms);
if (mount_keeper)
{
    if (drained)
        mount_keeper->stop();            /// terminal farewell: released clean
    else
    {
        LOG_WARNING(log, "CAS store shutdown with an unresolved ref-log PUT: skipping the "
                         "clean-release marker; the next mount will treat this end as unclean");
        mount_keeper->stopBackground();  /// no terminal op — successor observes + waits T_mat
    }
}
```

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasStoreShutdown.*:CasStore*'`, log
`build_debug/test_t5.log`, subagent summary.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: clean release drains ref lanes; farewell marker only after a successful drain"
```

---

### Task 6: `materialization_grace_ms` (T_mat) at open {#task-6}

**Files:**
- Modify: `Core/CasStore.h` (`PoolConfig` `:99-239`: new fields)
- Modify: `Core/CasStore.cpp` (`Store::open` `:290-456`)
- Modify: `Core/CasRequestControl.h` (`validateCasRequestBudget` doc `:109-118` — comment only)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
  (`:420-440` PoolConfig assembly)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` (~`:247-305`
  config-key reads)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `MountClaimResult::prior` (Task 4), `boot_ms_fn`.
- Produces:
  ```cpp
  /// PoolConfig additions:
  uint64_t materialization_grace_ms = 30000;   /// T_mat; lowering increases the risk that a
                                               /// late-materializing predecessor write is dropped
  std::function<void(uint64_t)> wait_sleep_fn = {};   /// test hook for open/remount waits
  /// Store member:
  std::atomic<bool> unclean_epoch_boundary_seen{false};   /// sticky; Task 8 reads it
  ```
  XML key: `<materialization_grace_ms>` under the disk's config prefix, read in
  `MetadataStorageFactory.cpp` alongside the existing keys and assigned in
  `ContentAddressedMetadataStorage.cpp:420-440`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasMountTmat, UncleanOpenWaitsMaterializationGrace)
{
    /// Predecessor: claim epoch 7, no farewell (simulate crash: just drop the keeper).
    /// Successor Store::open with config.materialization_grace_ms = 30000,
    /// config.wait_sleep_fn collecting sleep amounts, boot_ms_fn fake clock.
    /// EXPECT the collected waits to include the observation window AND a 30000 T_mat wait.
}

TEST(CasMountTmat, CleanOpenSkipsAllWaits)
{
    /// Predecessor released cleanly (drain + farewell from Task 5).
    /// Successor open: EXPECT zero recorded waits (no observation, no T_mat).
}

TEST(CasMountTmat, FencedPriorPaysOnlyTmat)
{
    /// Predecessor lease carries gc_fenced=true. Successor open:
    /// EXPECT exactly one wait == materialization_grace_ms, no observation window.
}
```

- [ ] **Step 2: Run to verify failure** — waits absent / fields unknown. Log
`build_debug/test_t6_fail.log`.

- [ ] **Step 3: Implement**

In `Store::open`, after the claim loop succeeds (`cpp:381-440`) and before `armMountFence`
(`:444`):

```cpp
if (claim.prior == MountPriorState::Fenced || claim.prior == MountPriorState::UncleanObserved)
{
    store->unclean_epoch_boundary_seen.store(true, std::memory_order_relaxed);
    const uint64_t t_mat = store->config.materialization_grace_ms;
    LOG_INFO(log, "Content-addressed mount {} follows an unclean predecessor; waiting {} ms "
                  "(materialization grace) for the store to finalize or drop its accepted "
                  "requests before trusting recovery listings", srid, t_mat);
    store->waitSleep(t_mat);   /// wait_sleep_fn if set, else interruptible sleep helper
}
```

Extend the `validateCasRequestBudget` doc comment: the handover-wait invariant
`observation (>= ttl) + T_mat >= all client timeouts + T_mat` holds by construction because
`attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms` is already enforced. Plumb the
XML key: in `MetadataStorageFactory.cpp` read
`config.getUInt64(config_prefix + ".materialization_grace_ms", 30000)`, pass it through to the
`PoolConfig` assembly in `ContentAddressedMetadataStorage.cpp` (follow how `server_root_id` flows
at `:285-287` → `:420-440`).

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasMountTmat.*:CasStore*'`, log
`build_debug/test_t6.log`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: materialization_grace_ms (T_mat) wait on unclean mount open"
```

---

### Task 7: Conditional T_mat on self-remount; delete the wedge→LatePredecessor conversion {#task-7}

**Files:**
- Modify: `Core/CasStore.cpp` (`tryRemountOnce` `:597-719`, `quiesceRefTablesForRemount`
  `:1241-1285`)
- Modify: `Core/CasStore.h` (`quiesceRefTablesForRemount` doc `:746-755`)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `drainRefLanesForShutdown`-style waiting (Task 5), `wait_sleep_fn`,
  `unclean_epoch_boundary_seen`, `materialization_grace_ms`.
- Produces:
  ```cpp
  /// Waits (bounded by attempt budget) for lane leaders to conclude their current attempt,
  /// then reports whether any ref-log conditional PUT remains unresolved (wedge present).
  bool Store::refLanesSettledForRemount();
  ```
  `quiesceRefTablesForRemount` no longer "converts an in-flight wedged PUT into the accepted Late
  Predecessor case" — the doc comment at `CasStore.h:746-755` is rewritten: the seal (Task 8)
  makes the dropped-epoch region uniformly invisible; remount pays T_mat only when unsettled.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasRemountTmat, DrainedRemountSkipsGrace)
{
    /// Trip the fence (fake boot clock past deadline as in WriteFenceUsesInjectedBootClock,
    /// gtest_cas_store.cpp:1151), with NO in-flight ref PUT. Drive tryRemountOnce.
    /// EXPECT: remount succeeds and wait_sleep_fn recorded NO materialization_grace_ms wait;
    /// unclean_epoch_boundary_seen stays false (drained boundary is clean for sealing).
}

TEST(CasRemountTmat, UnresolvedWedgePaysGraceAndMarksBoundaryUnclean)
{
    /// Wedge the lane first (Unresolved PUT via the throwing backend), trip fence, remount.
    /// EXPECT: exactly one recorded wait == materialization_grace_ms, and
    /// unclean_epoch_boundary_seen == true.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t7_fail.log`.

- [ ] **Step 3: Implement**

In `tryRemountOnce`, after `claimMountAwaitingExpiry` returns `Claimed` (`:635`) and before the new
keeper start (`:658`):

```cpp
const bool settled = refLanesSettledForRemount();
if (!settled)
{
    unclean_epoch_boundary_seen.store(true, std::memory_order_relaxed);
    LOG_INFO(log, "Self-remount of content-addressed mount {} with an unresolved ref-log PUT; "
                  "waiting {} ms (materialization grace) before re-listing", srid,
                  config.materialization_grace_ms);
    waitSleep(config.materialization_grace_ms);
}
```

`refLanesSettledForRemount`: same wait mechanics as `drainRefLanesForShutdown` but without the
admission latch (mutations already fail via the tripped fence), budget =
`attempt_timeout_ms + lease_safety_margin_ms`; returns `!any_wedge`. In
`quiesceRefTablesForRemount`, keep the publish-settle + `superseded_by_remount` +
`ref_tables.clear()` mechanics; delete the Late-Predecessor conversion language and behavior notes.

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasRemountTmat.*:CasStore*'`, log
`build_debug/test_t7.log`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: conditional T_mat on self-remount; retire the wedge-to-LatePredecessor case"
```

---

### Task 8: The recovery seal {#task-8}

**Files:**
- Modify: `Core/CasStore.cpp` (`ensureRefTableRecovered` `:1016-1165`)
- Modify: `Core/CasStore.h` (doc near `kRefRecoveryMaxRestarts` `:691`)
- Modify: `src/Common/ProfileEvents.cpp` (add `CasRefRecoverySealPublished` next to `:768`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`

**Interfaces:**
- Consumes: `RefTableSnapshot::sealed_from` + codec v2 (Task 3);
  `unclean_epoch_boundary_seen` (Tasks 6/7); `liveWriterEpoch()`;
  `ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok)`;
  test helpers `writeRefLogTxnRaw` / `writeRefSnapshotRaw` / `minimalLiveSnapshot`
  (`src/Disks/tests/cas_test_helpers.h:744-770`).
- Produces: seal semantics inside recovery —
  `seal_id = RefTxnId{liveWriterEpoch() - 1, UINT64_MAX}`, body = replay of everything listed,
  `snapshot_id = seal_id`, `sealed_from = greatest listed txn id`; published before the table is
  marked `recovered`; failure throws (recovery fails closed, bounded retries by the existing
  restart mechanism). Skipped when: boundary not unclean, `liveWriterEpoch() < 2`, dead-epoch
  region empty, or `newest_snapshot_id >= seal_id` (already sealed).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(RefWriterRecoverySeal, UncleanBoundarySealsAllDeadEpochsBeforeExposingState)
{
    /// Inject predecessor debris out-of-band: logs from TWO dead epochs, no snapshot:
    ///   writeRefLogTxnRaw(backend, layout, txn(epoch=1, seq=1, birth "a"));
    ///   writeRefLogTxnRaw(backend, layout, txn(epoch=2, seq=1, mut  "b"));
    /// Open the store so liveWriterEpoch() == 3 and unclean_epoch_boundary_seen == true
    /// (crash-style predecessor from Task 6 test setup). Touch the namespace (any read).
    /// EXPECT: backend now holds _snap at RefTxnId{2, UINT64_MAX}; decoded body has
    /// sealed_from == RefTxnId{2, 1}, contains "a" and "b";
    /// ProfileEvents CasRefRecoverySealPublished incremented by 1.
}

TEST(RefWriterRecoverySeal, LateLogBelowSealIsInvisibleToRecoveryAndFold)
{
    /// Continue from the sealed state; now inject a LATE dead-epoch log out-of-band:
    ///   writeRefLogTxnRaw(backend, layout, txn(epoch=2, seq=2, mut "c"));
    /// A fresh recoverRefTable (free function, CasRefIntake.h:132) over the namespace:
    /// EXPECT state equals the sealed body — "c" is not applied (id <= seal_id is covered).
}

TEST(RefWriterRecoverySeal, CleanBoundaryDoesNotSeal)
{
    /// Same debris, but unclean_epoch_boundary_seen == false (clean predecessor):
    /// EXPECT: no _snap object at {epoch-1, UINT64_MAX}; recovery state still correct.
}

TEST(RefWriterRecoverySeal, SealPutFailureFailsRecoveryClosed)
{
    /// failNextCasPut(seal key) so the seal PUT definitively fails:
    /// EXPECT the namespace touch throws; a second touch (PUT now succeeding) recovers and seals.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t8_fail.log`.

- [ ] **Step 3: Implement**

In `ensureRefTableRecovered`, after the replay of listed logs and before the seeding block
(`:1127`):

```cpp
const uint64_t my_epoch = liveWriterEpoch();
const RefTxnId seal_id{my_epoch - 1, UINT64_MAX};
const bool dead_region_nonempty = greatest_listed_id.has_value()
    && greatest_listed_id->writer_epoch < my_epoch;
const bool already_sealed = rt.newest_snapshot_id && !(*rt.newest_snapshot_id < seal_id);
if (unclean_epoch_boundary_seen.load(std::memory_order_relaxed)
    && my_epoch >= 2 && dead_region_nonempty && !already_sealed
    && recovered_state.lifecycle == RefLifecycle::Live)
{
    RefTableSnapshot seal = snapshotOf(recovered_state, ns.string());
    seal.snapshot_id = seal_id;                       /// upper bound of the covered region
    seal.sealed_from = recovered_state.greatest_applied;
    const String bytes = encodeRefTableSnapshot(seal);
    const auto fence_ok = [this] { return refAppendFenceOk(); };
    const auto outcome = ref_request_controller->putIfAbsentControlled(
        pool_layout.refSnapshotKey(ns, seal_id), bytes, fence_ok);
    if (outcome != CasWriteOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS recovery seal PUT for namespace {} did not commit; failing recovery closed "
            "(the mount stays non-writable for this table; retry will re-seal)", ns.string());
    ProfileEvents::increment(ProfileEvents::CasRefRecoverySealPublished);
    rt.newest_snapshot_id = seal_id;
    rt.base_snapshot_bytes.store(bytes.size(), std::memory_order_relaxed);
}
```

Capture `greatest_listed_id` in the existing LIST loop (`:1052-1083`). Note `state.greatest_applied`
after replay is the greatest listed id, so `sealed_from` needs no extra bookkeeping. A `Removed`
lifecycle table keeps its existing removed-snapshot path (no seal). Since a same-key retry after an
`Unresolved` outcome must re-PUT byte-identical content, the seal bytes must be deterministic:
they are — replay of the same listed set (the controller's resolve-before-reissue handles the
ambiguous case with the exact same bytes; a *changed* listed set after a failed envelope goes
through a fresh recovery restart, which re-LISTs and rebuilds both the state and the seal bytes —
still the same key only if the epoch did not change; on epoch change the key moves, which is the
safe cross-attempt move described in the spec).

- [ ] **Step 4: Run to verify pass** —
`--gtest_filter='RefWriterRecoverySeal.*:RefWriterRecovery.*:RefWriterSnapshotPublish.*' > build_debug/test_t8.log 2>&1`.
The `MountTimeTriggerPublishesAfterRecoveryReplaysLargeTail` test (`:970`) may need its expectation
adjusted if its scenario now seals first — subagent reports what changed and why.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: recovery seal — epoch-closing snapshot published before a table goes writable"
```

---

### Task 9: Observation-based GC fence-out {#task-9}

**Carry-forward from Task 2 (rounds 9-10, 2026-07-14):** the mount model's `GcFence` AND
`ClearExpiredMount` are already observation-based (`mount.deadline + Drift <= clock`) in
`docs/superpowers/models/CaCasMountCore.tla` — `_rev6_observe.cfg` is fully GREEN and exhaustive against
both. That model is the safety GATE this task implements in C++: `computeHeartbeatFloor`'s
`stable_threshold_ms` wait below is the product-side realization of the SAME principle for the
fence-out flavor specifically; do not regress the model back to a bare wall-clock `GcFence` while
working this task, and if this task's C++ change reveals the product needs an analogous fix for
whatever `ClearExpiredMount` abstracts (GC bookkeeping that clears/forgets an already-presumed-dead
mount record, not tied to one single named function — see the Task 2 round-10 amendment), treat that as
in-scope here too, not a new task.

**Files:**
- Modify: `Core/CasServerRoot.h` (`computeHeartbeatFloor` decl `:251-252`, `HeartbeatFloor`
  `:241-249`)
- Modify: `Core/CasServerRoot.cpp` (`computeHeartbeatFloor` `:477-556`)
- Modify: `Core/CasGc.h` (cross-round members near `:376-381`)
- Modify: `Core/CasGc.cpp` (call sites `:240-242`, `:2120-2122`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp` (suite `CasHeartbeatFloor`)

**Interfaces:**
- Consumes: existing fence mechanics (token-guarded `putOverwrite`, `max_reclassify = 4`); the
  cross-round observation precedent `rememberObservation` (`CasGc.h:354`); `Cas::Gc` longevity via
  `CasGcScheduler.h:87`.
- Produces:
  ```cpp
  struct MountTokenObservation { Token token; uint64_t first_seen_mono_ms = 0; };
  using MountObservationMap = std::map<String /*srid*/, MountTokenObservation>;
  HeartbeatFloor computeHeartbeatFloor(Backend &, const Layout &, uint64_t now_ms /*audit only*/,
      uint64_t mono_now_ms, uint64_t stable_threshold_ms, MountObservationMap & obs);
  ```
  `Cas::Gc` gains `MountObservationMap mount_obs;` (in-memory; a new leader starts empty →
  fencing delayed one round, safe). Fence condition: not terminated, not fenced, token unchanged
  in `obs` for `>= stable_threshold_ms` of the leader's monotonic clock. `skew_margin_ms` is gone
  from the fence decision; `listMounts`' display heuristic stays untouched.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasHeartbeatFloor, FirstSightNeverFencesEvenIfStampLooksExpired)
{
    /// Lease stamped expires_at_ms = 10, GC wall now = 999999. Empty obs map.
    /// EXPECT fenced_now == 0 and obs now contains the srid (observation started).
}

TEST(CasHeartbeatFloor, StableTokenPastThresholdIsFenced)
{
    /// Same lease; call twice with mono 0 then mono = threshold. EXPECT fenced_now == 1 on
    /// the second call, gc_fenced set in the body, seq bumped.
}

TEST(CasHeartbeatFloor, RenewalBetweenRoundsRestartsObservation)
{
    /// Observe at mono 0; renew the keeper (token bump); call at mono = threshold:
    /// EXPECT fenced_now == 0 and the obs entry rebased to the new token.
}
```

Adapt the existing `ClassifiesAndFencesOut` (`gtest_cas_mount.cpp:494`) to the two-call pattern.

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t9_fail.log`.

- [ ] **Step 3: Implement** — in `computeHeartbeatFloor`, replace the
`now_ms > expires_at_ms + skew_margin_ms` test (`:525`) with the observation lookup/update; keep
everything else (classification counters, token-guarded fence write, reclassify loop, farewell/
fenced short-circuits). Both `CasGc.cpp` call sites pass
`mono_now_ms = clock_fn_mono()` (add a monotonic clock member next to the existing wall
`clock_fn`, default `bootMs`-equivalent, test-injectable) and
`stable_threshold_ms = ttl + ttl / 20 + round_period_hint` where `round_period_hint =
mount_renew_period.count()` (one beat of slack); delete both `skew_margin_ms` derivations
(`:240-241`, `:2120-2121`).

- [ ] **Step 4: Run to verify pass** —
`--gtest_filter='CasHeartbeatFloor.*:CasGc*' > build_debug/test_t9.log 2>&1`; subagent lists
adjusted pre-existing tests.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_mount.cpp
git commit -m "cas: GC fence-out by token-stability observation on the leader's own clock"
```

---

### Task 10: Publish-from-live; delete the grace machinery {#task-10}

**Files:**
- Modify: `Core/CasStore.h` (delete `snapshot_min_log_age_ms` `:217` + doc `:199-216`; replace
  `TailLogEntry`/`tail_since_snapshot`/`snapshot_base_state` `:630-647` with counters)
- Modify: `Core/CasStore.cpp` (`maybeScheduleSnapshotPublish` `:1813-1926`,
  `trySnapshotPublishOnce` `:1977-2091`, `flushRefBatch` push `:1744-1745`,
  `ensureRefTableRecovered` seeding `:1127-1155`, `publishRemovedSnapshotNow` `:2176-2206`,
  cache-budget accounting reading `base_snapshot_bytes`/`tail_bytes_since_snapshot`)
- Modify: `src/Common/ProfileEvents.cpp` (delete `CasRefLatePredecessorObserved` `:770`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`

**Interfaces:**
- Consumes: T11 monotonic adoption (kept); backoff machinery (kept); Task 8 seal (sets
  `newest_snapshot_id` before any publish).
- Produces (RefTableRuntime replacement fields):
  ```cpp
  std::atomic<uint64_t> tail_count_since_snapshot{0};   /// applied txns above newest_snapshot_id
  std::atomic<uint64_t> tail_bytes_since_snapshot{0};   /// kept, same meaning
  /// snapshot_base_state and tail_since_snapshot are DELETED.
  ```
  Publish algorithm: copy `rt->state` once under `state_mutex` (candidate_x =
  `state.greatest_applied`), encode + conditional PUT off the lock; adoption under T11 guard
  subtracts the captured count/bytes.

- [ ] **Step 1: Write the failing tests first (behavioral targets of the new scheme)**

```cpp
TEST(RefWriterPublishFromLive, YoungTxnIsCoveredImmediately)
{
    /// Append ONE committed txn; force a publish (drive trySnapshotPublishOnce directly).
    /// OLD behavior: nothing published (grace window). NEW: snapshot at that txn id exists
    /// and its decoded body contains the row. No time manipulation at all.
}

TEST(RefWriterPublishFromLive, TriggerFiresOnCountAboveThresholdWithoutAging)
{
    /// Append snapshot_log_count_threshold + 1 txns with a boot clock that NEVER advances:
    /// EXPECT CasRefSnapshotPublishDispatched incremented (old code required aging).
}

TEST(RefWriterPublishFromLive, AdoptionSubtractsCapturedCountersUnderConcurrentAppends)
{
    /// Seed counters, run a publish; while its PUT is "in flight" (use the counting backend's
    /// hook), append more txns; after adoption EXPECT tail_count_since_snapshot equals the
    /// number appended after the copy (not zero, not negative).
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t10_fail.log`.

- [ ] **Step 3: Implement the new publish path**

`trySnapshotPublishOnce` core replacement (`:1982-2019` collapses to):

```cpp
RefTableState candidate_state;
RefTxnId candidate_x;
uint64_t captured_count, captured_bytes;
{
    std::lock_guard lock(rt->state_mutex);
    if (rt->state.lifecycle != RefLifecycle::Live)
        return false;
    if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < rt->state.greatest_applied))
        return false;                       /// nothing above the newest snapshot
    candidate_state = rt->state;            /// ONE copy, at a txn boundary
    candidate_x = rt->state.greatest_applied;
    captured_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
    captured_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
}
```

Encode/PUT/backoff/T11 stay as today (`:2021-2067`); the adoption block (`:2069-2088`) replaces the
prune loop with counter subtraction (`fetch_sub` of captured values, clamped via a CAS loop or
`fetch_sub` + assert-no-underflow in debug) and drops `snapshot_base_state` entirely.
`maybeScheduleSnapshotPublish`: the fused walk (`:1857-1876`) becomes

```cpp
const uint64_t publishable_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
const uint64_t publishable_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
const bool over_threshold = publishable_count > config.snapshot_log_count_threshold
    || publishable_bytes > config.snapshot_log_bytes_threshold;
if (over_threshold) { rt->pending_snapshot_publishes.fetch_add(1, ...); dispatch = true; }
```

`flushRefBatch` commit path (`:1744-1745`): increment the two counters instead of `push_back`.
`ensureRefTableRecovered` seeding (`:1127-1155`): seed counters from the listed tail
(count of logs above the newest snapshot, sum of their body sizes) — the mount-time trigger
semantics survive; delete `snapshot_base_state` seeding. `publishRemovedSnapshotNow`
(`:2201-2205`): zero both counters instead of clearing the vector/base. Cache-budget accounting:
`base_snapshot_bytes + tail_bytes_since_snapshot` formula unchanged. Delete
`snapshot_min_log_age_ms`, `TailLogEntry`, `tail_since_snapshot`, `snapshot_base_state`, the
`CasRefLatePredecessorObserved` ProfileEvent, and these tests wholesale:
`GraceAgeRespectedYoungLogNotCovered` (`:1008`), `LatePredecessorCounterCountsGraceWindowHoldback`
(`:1040`), `TriggerIgnoresYoungTailAboveCountThreshold` (`:1364`),
`TriggerNotLatchedBySustainedLoadYoungFloor` (`:1398`). Adapt
`TriggerIgnoresEntriesCoveredByNewestSnapshot` (`:1443`) to counters (covered entries simply are
not counted — seed via recovery). Keep and re-verify: threshold/counter tests, C4 backoff suite
(`:1202-1330`), `ConcurrentOutOfOrderPublishDoesNotRegressBaseNorDropCommittedTxns` (`:1138` —
now asserts counters instead of base state).

- [ ] **Step 4: Run the full ref suite** —
`--gtest_filter='RefWriter*:RefSnapshotCodec.*' > build_debug/test_t10.log 2>&1`; subagent
summarizes every deleted/adapted test with a one-line reason each.

- [ ] **Step 5: Grep-gate the deletion**

Run: `grep -rn "snapshot_min_log_age_ms\|tail_since_snapshot\|snapshot_base_state\|CasRefLatePredecessorObserved\|TailLogEntry" src/ | grep -v test` — expected: empty output.

- [ ] **Step 6: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/ src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: publish-from-live — grace window, base+tail replay and LatePredecessor counter removed"
```

---

### Task 11: Wedge hard contract + anomaly policy helper {#task-11}

**Files:**
- Modify: `Core/CasStore.h` (helper decl), `Core/CasStore.cpp` (`flushRefBatch` id-allocation site
  `:1706` area, wedge-resolution `CORRUPTED_DATA` branch `:1518-1526`)
- Modify: `Core/CasEvent.h` (add `CasEventType::ForeignInterference`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`, `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `tripMountLost()` + `scheduleRemount()` (existing `on_lost` mechanics,
  `CasStore.cpp:418-423`); `EventEmitter` idiom (`CasEvent.h:72-93`); `emitEvent`
  (`CasStore.h:548-552`).
- Produces:
  ```cpp
  /// Incidental-detection reaction (spec §anomaly-policy): LOGICAL_ERROR + fail-closed remount
  /// + background diagnostics strictly off the critical path.
  void Store::reportImpossibleInterference(
      const String & key, const String & reason,
      const std::optional<String> & offending_ns = {});
  ```
  Behavior: LOG_ERROR with full context; `EventEmitter{*this}.emit` a `ForeignInterference` event;
  `tripMountLost(); scheduleRemount();` then schedule ONE background `ThreadFromGlobalPool` task
  that GETs `key` (single attempt), decodes what it can (writer epoch/uuid if the body carries
  one), and LOG_ERRORs a rich diagnostic line — never on the caller's thread; finally the caller
  throws `LOGICAL_ERROR`. The wedge contract: `chassert(!rt->wedge)` at the new-id allocation
  point plus a release-mode guard that fails the batch with `LOGICAL_ERROR` through this helper
  instead of allocating.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasAnomalyPolicy, ForeignBytesAtWedgeKeyTripFenceAndRemount)
{
    /// Wedge the lane (Unresolved PUT), then overwrite the wedge key out-of-band with
    /// DIFFERENT bytes via backend->putOverwrite. Next flush resolves the wedge:
    /// EXPECT the mutation fails with LOGICAL_ERROR (was CORRUPTED_DATA), mayMutate() == false
    /// (fence tripped), and a ForeignInterference CasEvent captured by a test event sink.
}

TEST(CasAnomalyPolicy, WedgeContractReleaseFailClosed)
{
    /// Force rt->wedge while injecting a batch that would allocate a new id (simulate the
    /// impossible state by setting the wedge directly under state_mutex in the test):
    /// EXPECT the batch fails with LOGICAL_ERROR and NO new _log object appears.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t11_fail.log` (debug build so
`chassert` is active; the release-guard test asserts the outcome, not the assert).

- [ ] **Step 3: Implement** — helper as specified; rewire the `CORRUPTED_DATA` different-bytes
branch (`:1518-1526`) through it (keep the wedge in place — fail-closed); add the id-allocation
guard before `allocateRefTxnId()` in `flushRefBatch`; add the enum value + its `String` name in
the event-type table in `CasEvent.h`/`.cpp`.

- [ ] **Step 4: Run to verify pass** —
`--gtest_filter='CasAnomalyPolicy.*:RefWriter*' > build_debug/test_t11.log 2>&1`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp src/Disks/tests/gtest_cas_ref_writer.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: anomaly policy — LOGICAL_ERROR + fail-closed remount on impossible interference; wedge hard contract"
```

---

### Task 12: Sweep detector for T_mat violations (late log below the seal) {#task-12}

**Files:**
- Modify: `Core/CasRefIntake.h`/`.cpp` (`recoverRefTable` `:132`/`:139-210` — surface the newest
  snapshot's `snapshot_id` + `sealed_from`)
- Modify: `Core/CasOrphanManifestSweep.cpp` (log loop `:129-140`)
- Modify: `Core/CasEvent.h` (add `CasEventType::RefLateLogDetected`)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (or the sweep's existing gtest home — locate
  `sweepNamespace` tests and add alongside)

**Interfaces:**
- Consumes: `sealed_from` (Task 3), seal objects (Task 8), `EventEmitter`.
- Produces:
  ```cpp
  struct RecoveredRefTable { RefTableState state;
      std::optional<RefTxnId> newest_snapshot_id;
      std::optional<RefTxnId> sealed_from; };
  RecoveredRefTable recoverRefTableDetailed(Backend &, const Layout &, const RootNamespace &,
      std::function<void()> on_page_fetched = {},
      unsigned max_restarts = 3);   /// recoverRefTable stays as a thin wrapper returning .state
  ```
  CARRY-FORWARD (added 2026-07-14 after the §0 introspection plan landed; final-review finding I1):
  `recoverRefTable` NOW has an `on_page_fetched` parameter (4th, before `max_restarts`) and the GC
  callers at `CasGc.cpp` / `CasOrphanManifestSweep.cpp` pass a callback that is the sole emit path
  for `CasGcEnumerationPages` on the recovery-LIST scans. The wrapper refactor MUST preserve that
  parameter and keep the two GC call sites passing it (fsck callers stay without it). Likewise the
  `CasOrphanManifestSweep.cpp` log loop this task modifies already invokes `onGcEnumerationPage`
  per fetched page — preserve that wiring while adding the late-log detector. Line anchors in this
  task drifted vs the §0 commits (5edd9b39cec..de42a89ab87); re-locate by name.
  Detector rule: a listed `_log` id `L` with `sealed_from < L <= newest_snapshot_id` (when
  `sealed_from` is present) provably materialized after the recovery LIST → `LOG_WARNING` + one
  `RefLateLogDetected` event. Never GET the log body to "revive" it (resurrect invariant); GC's
  ordinary covered-log cleanup deletes it later.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasSweepLateLog, LogBetweenSealedFromAndSealIdIsReportedNotRevived)
{
    /// Build a namespace with a seal (via writeRefSnapshotRaw: snapshot_id={2,UINT64_MAX},
    /// sealed_from={2,3}), then inject a late log at {2,7} via writeRefLogTxnRaw.
    /// Run the sweep page over the namespace with a test event sink:
    /// EXPECT one RefLateLogDetected event naming {2,7}; the log object still present
    /// (sweep does not delete it); no state change derived from it.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t12_fail.log`.

- [ ] **Step 3: Implement** — extend the free recovery to return the detail struct; in the sweep's
LIST-classification (it already parses `_log` ids), add the range check + emission before the
above-cursor protection loop; wire the event type.

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasSweepLateLog.*:*Sweep*' >
build_debug/test_t12.log 2>&1`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/tests/
git commit -m "cas: sweep reports a late log below the seal as a T_mat violation (report, never revive)"
```

---

### Task 13: GC-defense compliance audit {#task-13}

**Files:**
- Create: `docs/superpowers/reports/2026-07-XX-cas-gc-defense-audit.md` (stamp the actual date)
- Possibly modify: only sites the audit finds non-compliant.

**Interfaces:** none produced; consumes the spec's §gc-defense-audit checklist.

- [ ] **Step 1:** For each site, read the code and write a compliance note (site, what it defends
against, request cost on the hot path, verdict): `gc/state` round-ownership re-read
(`Core/CasGc.cpp:1433` area), zombie-steal committed-pair threading (`Core/CasGc.h:243`),
deposed-leader debris handling (`Core/CasGc.cpp:1682-1695`), orphan-sweep epoch gate
(`Core/CasOrphanManifestSweep.cpp:146-163`), TokenMismatch/404 delete tolerance (`:200-206`,
`:283-302`), fold-lag clamps (`Core/CasGc.cpp:986-1052`, `:1222-1236`). The principle to check:
zero S3 requests spent *fighting* a foreign writer on hot paths; incidental checks and
CAS-linearized commits are compliant by definition.
- [ ] **Step 2:** If a site spends non-incidental requests on foreign-writer defense, fix it in a
separate commit with a test; otherwise record "compliant, keep" with a one-line justification.
- [ ] **Step 3:** Commit the report (`git add docs/superpowers/reports/... && git commit -m "cas:
GC-defense compliance audit vs rev.6 anomaly policy"`).

---

### Task 14: End-to-end checks and soak scenario {#task-14}

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s31_late_put_injection.py` (follow the structure of an
  existing card, e.g. `utils/ca-soak/scenarios/cards/s15_s18_shards_lifecycle.py`)
- Modify: `utils/ca-soak/scenarios/RUN_HISTORY.md` (append the run entry when executed)

**Interfaces:** consumes everything above via the built server binary.

- [ ] **Step 1:** Scenario card: boot a 2-node cluster (framework `cluster_boot.py`), kill -9 the
writer mid-append-storm, restart it, assert from logs: the observation + T_mat wait lines appear,
the seal publish happens (grep `CasRefRecoverySealPublished` in `system.events` /
`content_addressed_log`), no `sparing` warnings of the `delete_pending retired entry recovered
in-degree` class, and a clean stop/start pays no wait (grep absence).
- [ ] **Step 2:** Late-PUT injection: while the successor is inside its T_mat wait (long
`materialization_grace_ms` for the test), inject a dead-epoch `_log` object directly to the object
store (the scenario has S3 credentials); after mount, assert the sweep emits `RefLateLogDetected`
and queries return only sealed truth.
- [ ] **Step 3:** Run the card against a fresh debug build; triage with the existing scenario
framework conventions (host logs survive teardown; `down -v` for clean data). Record in
`RUN_HISTORY.md`.
- [ ] **Step 4:** Re-run the S13/S15/S18 cards once as a regression sweep (they exercise the
fence/remount and shard lifecycle paths this plan touched).
- [ ] **Step 5:** Commit the card + history entry
(`git commit -m "cas: soak card s31 — unclean handover, seal, late-PUT injection"`).

---

## Self-review results {#self-review}

- **Spec coverage:** decision log 1→Tasks 4/9; 2→4/9; 3→6; 4→7; 5/6→8; 7→8 (clean skip);
  8→11; 9 already landed; 10→13. §lease-acquisition→4/5/6; §gc-fence-out→9; §t-mat→6/7;
  §recovery-seal→3/8; §ref-simplification→10; §anomaly-policy→11/12; §gc-defense-audit→13;
  §tla→1/2; §configuration→6/10; §testing→per-task + 14.
- **Known deliberate mappings:** "seal before writable" realized per-namespace inside lazy
  recovery (architecture note at top); "T_mat violation invariant" split into strict
  (expected-red demo) + fresh-reader (green) configs in Task 1 — matching the spec's
  "deterministic-invisible for observers from birth" wording honestly.
- **Type consistency:** `MountPriorState` produced in Task 4, consumed 6/7;
  `sealed_from` produced 3, consumed 8/12; counters produced 10 only after seal (8) no longer
  needs the tail vector — verified Task 8 uses only `newest_snapshot_id`/`base_snapshot_bytes`,
  which survive Task 10.
- **Placeholder scan:** two TLA action bodies in Task 2 are elided with `<take over: ...>` — they
  copy the existing model's claim/write mechanics verbatim from adjacent actions; acceptable
  because the implementer edits that file with the original in front of them. No other
  placeholders.
