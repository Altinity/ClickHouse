-------------------- MODULE CaRefDeltaIntakeCore --------------------
(* GC ref-intake core — spec 2026-07-11-cas-ref-table-snapshot-log-design.md
   §gc-step-enumerate-once (the three-premise proof), §gc-step-produce-manifest-edge-delta,
   §gc-step-clean-ref-objects, §writer-side-linearization, §late-predecessor-put.

   Two tables (T1, T2) share one lexically ordered key space: every T1 key sorts below every T2
   key, and within a table keys sort by id (KeyOrd). This is premise 3, prefix contiguity, and it
   is baked into KeyOrd rather than sabotaged — no key of one table can ever fall between two keys
   of the other.

   The writer append rule (premise 1 — strictly increasing durable ids per table, at most one
   unresolved append, wedge until resolved) is likewise never sabotaged here: pendingId enforces
   the wedge structurally. What this model stresses is premise 2 (resume-after-returned-key
   pagination) and the two protocol rules built on top of the three premises: cursor adoption is
   atomic with the fold commit, and cleanup requires BOTH cursor and snapshot coverage.

   One round at a time: BeginRound -> repeated PageStep -> ScanComplete -> FoldCommitWin/Lose ->
   idle. The candidate cursor `cand[t]` is the greatest key returned this round for table `t`;
   `csnap` is the cursor snapshotted at round start, used to decide which returned keys are "new"
   (this round's delta) versus already folded. A GC round's commit may lose; on loss the previous
   cursor and folded set remain authoritative (I3).

   Each Sabotage* toggle breaks exactly one load-bearing rule and must yield a counterexample.
   EnableLatePredecessorPut gates the documented open limitation (an old-epoch log that never
   became durable in this process shows up after the cursor has already passed it) — it is a
   separate, expected-fail configuration, never enabled alongside the Sabotage* toggles. *)
EXTENDS Integers

CONSTANTS
    T1, T2, MaxSeq,
    SabotageResumeSkip,           \* a page may advance past a durable key without returning it
    SabotageAdoptBeforeCommit,    \* cursor becomes visible to cleanup before the commit's win/lose
    SabotageCleanupIgnoresCursor, \* cleanup requires only snapshot coverage, not cursor coverage
    EnableLatePredecessorPut      \* gates the documented cross-epoch counterexample (expected-fail)

ASSUME T1 # T2
ASSUME MaxSeq \in Nat /\ MaxSeq >= 1

Tables == {T1, T2}
Ids == 1..MaxSeq
AllKeys == { [t |-> t, i |-> i] : t \in Tables, i \in Ids }

(* Table index fixes lexical order: every T1 key sorts below every T2 key (premise 3). *)
TabIdx(t) == IF t = T1 THEN 0 ELSE 1
KeyOrd(k) == (TabIdx(k.t) * (MaxSeq + 1)) + k.i

(* The one old-epoch id that may arrive late in the _latepred configuration; unrelated to any
   Sabotage* toggle and never enabled alongside them. *)
LateId == 2

VARIABLES
    durable,       \* SUBSET AllKeys: currently durable logs (deletable by Cleanup)
    everDurable,   \* SUBSET AllKeys: every key ever made durable (monotonic; the I1 oracle)
    nextId,        \* [Tables -> 1..MaxSeq+1]: per-table allocation counter (gaps allowed)
    pendingId,     \* [Tables -> 0..MaxSeq]: in-flight append id, 0 = no unresolved append (wedge)
    cursor,        \* [Tables -> 0..MaxSeq]: durable, adopted last_folded_ref_id
    cand,          \* [Tables -> 0..MaxSeq]: this round's candidate cursor (greatest key returned)
    csnap,         \* [Tables -> 0..MaxSeq]: cursor snapshotted at this round's BeginRound
    snap,          \* [Tables -> 0..MaxSeq]: abstract writer-published snapshot coverage
    delta,         \* SUBSET AllKeys: keys > csnap[t] returned so far this round (candidate fold)
    folded,        \* SUBSET AllKeys: durably adopted deltas (the reachability fold input)
    scanPos,       \* current global scan position (KeyOrd value); 0 = below all keys
    gcPhase,       \* "idle" | "scanning" | "complete"
    roundOutcome,  \* "none" | "won" | "lost" -- outcome of the current/last round's commit
    dupFlag,       \* sticky: a commit tried to adopt a key already in folded (I2 oracle)
    lateFired      \* sticky: LatePredecessorPut has already fired (fires at most once)

vars == << durable, everDurable, nextId, pendingId, cursor, cand, csnap, snap, delta, folded,
           scanPos, gcPhase, roundOutcome, dupFlag, lateFired >>

Init ==
    /\ durable = {}
    /\ everDurable = {}
    /\ nextId = [t \in Tables |-> 1]
    /\ pendingId = [t \in Tables |-> 0]
    /\ cursor = [t \in Tables |-> 0]
    /\ cand = [t \in Tables |-> 0]
    /\ csnap = [t \in Tables |-> 0]
    /\ snap = [t \in Tables |-> 0]
    /\ delta = {}
    /\ folded = {}
    /\ scanPos = 0
    /\ gcPhase = "idle"
    /\ roundOutcome = "none"
    /\ dupFlag = FALSE
    /\ lateFired = FALSE

(* ---- writer actions (premise 1: never sabotaged) ---- *)

WAppendStart(t) ==
    /\ pendingId[t] = 0
    /\ nextId[t] <= MaxSeq
    /\ pendingId' = [pendingId EXCEPT ![t] = nextId[t]]
    /\ nextId' = [nextId EXCEPT ![t] = @ + 1]
    /\ UNCHANGED << durable, everDurable, cursor, cand, csnap, snap, delta, folded,
                    scanPos, gcPhase, roundOutcome, dupFlag, lateFired >>

(* Resolution by success: the id becomes durable and unwedges the table's append lane. *)
WAppendDurable(t) ==
    /\ pendingId[t] # 0
    /\ LET id == pendingId[t] IN
         /\ durable' = durable \cup { [t |-> t, i |-> id] }
         /\ everDurable' = everDurable \cup { [t |-> t, i |-> id] }
    /\ pendingId' = [pendingId EXCEPT ![t] = 0]
    /\ UNCHANGED << nextId, cursor, cand, csnap, snap, delta, folded,
                    scanPos, gcPhase, roundOutcome, dupFlag, lateFired >>

(* Resolution WITHOUT durability: the id never becomes durable, the lane is not wedged, and the id
   remains a safe gap (premise 1 allows gaps; only strict increase is required).

   TWO implementation paths reach this action, and they are the same transition here because they
   are the same fact — nothing became durable:
     - `DefiniteFailure`: the attempt was sent and PROVEN never applied;
     - `CasUnresolvedReason::NoAttemptSent` (finding #37 defect 3): both pre-attempt gates rejected
       before the first request, so the attempt was never sent at all.
   Only a genuinely ambiguous outcome — an attempt that MAY have landed — keeps `pendingId` set, and
   that is `WAppendStart` with no resolution yet. So the Task-18 behaviour needs no new action: it
   widens which C++ paths reach `WAppendAbandon`, not what the model admits. *)
WAppendAbandon(t) ==
    /\ pendingId[t] # 0
    /\ pendingId' = [pendingId EXCEPT ![t] = 0]
    /\ UNCHANGED << durable, everDurable, nextId, cursor, cand, csnap, snap, delta, folded,
                    scanPos, gcPhase, roundOutcome, dupFlag, lateFired >>

DurableIds(t) == { k.i : k \in { kk \in durable : kk.t = t } }

(* Abstract writer-published snapshot: may be raised to any currently durable id of the table;
   may exceed the adopted cursor (spec §gc-step-create-snapshots). *)
WRaiseSnap(t) ==
    /\ \E i \in DurableIds(t) :
         /\ i > snap[t]
         /\ snap' = [snap EXCEPT ![t] = i]
    /\ UNCHANGED << durable, everDurable, nextId, pendingId, cursor, cand, csnap, delta, folded,
                    scanPos, gcPhase, roundOutcome, dupFlag, lateFired >>

(* ---- GC round: scan, fold commit ---- *)

BeginRound ==
    /\ gcPhase = "idle"
    /\ gcPhase' = "scanning"
    /\ scanPos' = 0
    /\ csnap' = cursor
    /\ cand' = cursor
    /\ delta' = {}
    /\ roundOutcome' = "none"
    /\ UNCHANGED << durable, everDurable, nextId, pendingId, cursor, snap, folded, dupFlag, lateFired >>

Cands == { k \in durable : KeyOrd(k) > scanPos }

(* One page, page size 1. Honestly the smallest surviving durable key strictly greater than the
   scan position (resume-after-returned-key, premise 2). SabotageResumeSkip drops the
   "must be the minimum" constraint, letting a page jump ahead and permanently skip a durable key
   that never gets returned this round -- the opaque-continuation-token-over-scan hazard. *)
PageStep ==
    /\ gcPhase = "scanning"
    /\ Cands # {}
    /\ \E key \in Cands :
         /\ (~SabotageResumeSkip => \A other \in Cands : KeyOrd(key) <= KeyOrd(other))
         /\ scanPos' = KeyOrd(key)
         /\ IF key.i > csnap[key.t]
            THEN /\ delta' = delta \cup {key}
                 /\ cand' = [cand EXCEPT ![key.t] = IF key.i > cand[key.t] THEN key.i ELSE cand[key.t]]
            ELSE UNCHANGED << delta, cand >>
    /\ UNCHANGED << durable, everDurable, nextId, pendingId, cursor, csnap, snap, folded,
                    gcPhase, roundOutcome, dupFlag, lateFired >>

(* Scan exhausted. Honestly the cursor stays untouched until the commit actually wins (I3).
   SabotageAdoptBeforeCommit instead makes the candidate cursor live here, before the commit's
   win/lose is decided -- "cursor visible to cleanup before the commit action". *)
ScanComplete ==
    /\ gcPhase = "scanning"
    /\ Cands = {}
    /\ gcPhase' = "complete"
    /\ cursor' = IF SabotageAdoptBeforeCommit THEN cand ELSE cursor
    /\ UNCHANGED << durable, everDurable, nextId, pendingId, cand, csnap, snap, delta, folded,
                    scanPos, roundOutcome, dupFlag, lateFired >>

FoldCommitWin ==
    /\ gcPhase = "complete"
    /\ cursor' = cand
    /\ dupFlag' = dupFlag \/ ((delta \cap folded) # {})
    /\ folded' = folded \cup delta
    /\ roundOutcome' = "won"
    /\ gcPhase' = "idle"
    /\ UNCHANGED << durable, everDurable, nextId, pendingId, cand, csnap, snap, delta,
                    scanPos, lateFired >>

(* A losing commit adopts nothing: cursor and folded carry over unchanged from before this
   round (I3). Under honest/S1/S3 the cursor already held that value throughout the round; under
   S2 it may already have been bumped at ScanComplete, so I3 is checked against `csnap`, the
   value captured at BeginRound, not against "cursor before this action". *)
FoldCommitLose ==
    /\ gcPhase = "complete"
    /\ roundOutcome' = "lost"
    /\ gcPhase' = "idle"
    /\ UNCHANGED << durable, everDurable, nextId, pendingId, cursor, cand, csnap, snap, delta,
                    folded, scanPos, dupFlag, lateFired >>

(* ---- ref-object cleanup: storage housekeeping only, no fold-account effect ---- *)

(* Honestly requires BOTH: the adopted cursor covers the id (its delta cannot be lost) AND an
   observed snapshot covers it. SabotageCleanupIgnoresCursor drops the cursor requirement,
   letting cleanup delete an unfolded log the moment a snapshot claims to cover it. *)
Cleanup(t, i) ==
    /\ [t |-> t, i |-> i] \in durable
    /\ i <= snap[t]
    /\ (SabotageCleanupIgnoresCursor \/ i <= cursor[t])
    /\ durable' = durable \ { [t |-> t, i |-> i] }
    /\ UNCHANGED << everDurable, nextId, pendingId, cursor, cand, csnap, snap, delta, folded,
                    scanPos, gcPhase, roundOutcome, dupFlag, lateFired >>

(* ---- documented open limitation: Late Predecessor PUT (spec §late-predecessor-put) ---- *)

(* An unfinished predecessor request for table T1's id LateId, never durable during this process's
   recovery LIST, completes only after the cursor has already advanced past LateId (i.e. some
   higher id was folded while LateId sat as an apparent gap). This is not reachable through any
   ordinary writer/scanner/cleanup action above; it is the acknowledged cross-epoch hole, gated by
   EnableLatePredecessorPut so it never contaminates the safe or Sabotage* configurations. *)
LatePredecessorPut ==
    /\ ~lateFired
    /\ cursor[T1] >= LateId
    /\ [t |-> T1, i |-> LateId] \notin everDurable
    /\ durable' = durable \cup { [t |-> T1, i |-> LateId] }
    /\ everDurable' = everDurable \cup { [t |-> T1, i |-> LateId] }
    /\ lateFired' = TRUE
    /\ UNCHANGED << nextId, pendingId, cursor, cand, csnap, snap, delta, folded,
                    scanPos, gcPhase, roundOutcome, dupFlag >>

(* Self-loop so bounded counters (nextId, cursor) exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
    \/ \E t \in Tables : WAppendStart(t) \/ WAppendDurable(t) \/ WAppendAbandon(t) \/ WRaiseSnap(t)
    \/ \E t \in Tables, i \in Ids : Cleanup(t, i)
    \/ BeginRound \/ PageStep \/ ScanComplete \/ FoldCommitWin \/ FoldCommitLose
    \/ (EnableLatePredecessorPut /\ LatePredecessorPut)
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

TypeOK ==
    /\ durable \subseteq AllKeys
    /\ everDurable \subseteq AllKeys
    /\ durable \subseteq everDurable
    /\ delta \subseteq AllKeys
    /\ folded \subseteq AllKeys
    /\ nextId \in [Tables -> 1..(MaxSeq + 1)]
    /\ pendingId \in [Tables -> 0..MaxSeq]
    /\ cursor \in [Tables -> 0..MaxSeq]
    /\ cand \in [Tables -> 0..MaxSeq]
    /\ csnap \in [Tables -> 0..MaxSeq]
    /\ snap \in [Tables -> 0..MaxSeq]
    /\ scanPos \in 0..(2 * (MaxSeq + 1))
    /\ gcPhase \in {"idle", "scanning", "complete"}
    /\ roundOutcome \in {"none", "won", "lost"}
    /\ dupFlag \in BOOLEAN
    /\ lateFired \in BOOLEAN

(* (I1) The adopted cursor never passes a durable-but-unfolded log, even if that log was later
   deleted -- everDurable (not durable) is the oracle, exactly the property the three-premise
   proof in spec §gc-step-enumerate-once establishes. *)
NoMissedFold == \A k \in everDurable : (k.i <= cursor[k.t]) => (k \in folded)

(* (I2) Every key is adopted into folded[] at most once. *)
ExactlyOnce == ~dupFlag

(* (I3) A losing commit's round adopts nothing: if this round's outcome is "lost", the cursor
   still equals the value captured at BeginRound. *)
LosingCommitAdoptsNothing == (roundOutcome = "lost") => (cursor = csnap)

=============================================================================
