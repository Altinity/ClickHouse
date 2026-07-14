-------------------- MODULE CaRefTableSnapshotLogCore --------------------
(* One table's snapshot + append-only log protocol — spec
   2026-07-11-cas-ref-table-snapshot-log-design.md (rev.4), Phase 1, Task 1 gate.

   Scope (spec sections: Summary, Object Layout, Snapshot Format, Transaction Log Format,
   State Transitions, Startup And Recovery, Concurrent Startup And Cleanup, tla-models):

   - The table's true history is a sequence of immutable ref transactions with strictly increasing
     ids (`writtenEver`, the recovery oracle — it never shrinks even after cleanup deletes objects).
     Each id has one op: `birth` | `mut` | `remove` | `rebirth`. Folding the ops of an id set in id
     order is the table state (`WState` = Replay(full history)).
   - A snapshot `X` is a body captured at publish time holding Replay of every id <= X
     (`snapCov[X]`, frozen bytes). Grace age is abstracted as "a snapshot never covers the newest
     log" (there is always a durable log above X).
   - A reader recovers with exactly one ordered scan: `_log` keys (kind 0) before `_snap` keys
     (kind 1), one page (=one key) at a time so cleanup/publication interleave between pages,
     resume-after-last-returned-key (`rScanPos`). It picks the greatest snapshot it enumerated,
     then fetches bodies. If a selected object vanished between enumeration and fetch it RESTARTS
     with a fresh scan (bounded, counted); publish-before-delete + `_log`<`_snap` ordering make the
     fresh scan return the covering newer snapshot.
   - Cleanup deletes a log <= X and a snapshot < X only for a snapshot X it OBSERVED durable
     (present in `snaps` = returned by its own scan).
   - Namespace removal (`remove`) then recreation (`rebirth`) is gated on a durable `Completed`
     marker (`completed`), the GC namespace-cleanup completion the writer observes in its recovery
     LIST.

   Deliberate Phase-1 scoping of this focused model (documented so the oracle stays decisive):
   - Append resolution is atomic: `WriterMut`/`WriterBirth`/... materialize a durable log (the
     "late-but-linearized" success); `WriterFail` leaves a safe id gap (the "never" outcome). The
     at-most-one-unresolved append lane and pagination-vs-append safety are the subject of
     CaRefDeltaIntakeCore (Task 2); here ordinary appends are frozen while a reader is recovering
     (`ReaderInactive`) because recovery is a fresh mount with the previous writer's lane drained.
   - The one genuinely-unresolved cross-epoch hazard — a predecessor PUT that materializes below an
     already-published snapshot after the successor's scan — is `LatePredecessorPut`, enabled only
     by the `LatePred` constant (the `_latepred` config). Per spec §late-predecessor-put it MUST
     break complete recovery; that counterexample is the documented Phase-1 limitation, retained as
     expected-fail, never removed by assuming a mount fence cancels an in-flight S3 request.

   rev.6 addendum (spec 2026-07-13-cas-ref-lease-exclusivity-rev6-design.md): `Rev6MountRule`
   models coverage-at-birth sealing a successor's mount against exactly this straggler. The late
   PUT still lands (an in-flight S3 request cannot be cancelled) but is folded out of the
   contract-clean oracle `WStateRev6` via the ghost `droppedEver`. `NoDivergentFold` is the strict
   oracle-match for a finished reader and is EXPECTED TO VIOLATE under `_rev6_latedelivery` (an
   in-flight reader may transiently observe the dropped log — the documented accepted transient,
   error direction spare-not-delete); `INV_FRESH_READER` is the weaker post-T_mat containment that
   any reader STARTING after the drop (`rStartedAfterDrop`) always agrees, and `INV_SNAP_DETERMINISTIC`
   pins that published snapshot bytes stay deterministic under the same oracle. Legacy configs set
   `Rev6MountRule = FALSE`, making all three vacuous there.

   Each Sabotage* constant breaks exactly one load-bearing rule and MUST yield a counterexample;
   with all Sabotage* false and LatePred false the model is GREEN. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    MaxSeq,                          \* ref-txn ids are 1..MaxSeq (the shared log/snapshot timeline)
    MaxRestarts,                     \* bounded recovery restarts (spec §startup-and-recovery)
    SabotageDeleteBeforeSnapshot,    \* cleanup deletes a log with no present covering snapshot
    SabotageVanishIsCorruption,      \* reader treats a vanished selected object as corruption, not restart
    SabotageRecreateBeforeCompleted, \* namespace_birth over Removed without the durable Completed marker
    SabotageRemountKeepsOldEpoch,    \* a self-remount keeps stamping fresh appends at the old (below-durable) epoch
    LatePred,                        \* enable LatePredecessorPut (adversarial; expected-fail)
    Rev6MountRule                    \* coverage-at-birth drops a late PUT (rev.6 lease-exclusivity rule)

Seqs == 1..MaxSeq
Ops == {"none", "birth", "mut", "remove", "rebirth"}
Lifes == {"empty", "live", "removed"}
EmptyState == [life |-> "empty", committed |-> {}, removeSeq |-> 0]
StartPos == [kind |-> -1, id |-> -1]

VARIABLES
    op,            \* [Seqs -> Ops]  op assigned to each id ("none" = never durably written)
    writtenEver,   \* SUBSET Seqs    every id ever durably appended (recovery oracle; never shrinks)
    logs,          \* SUBSET Seqs    present _log/<id> objects
    snaps,         \* SUBSET Seqs    present _snap/<id> objects
    publishedEver, \* SUBSET Seqs    snapshot ids ever published
    snapCov,       \* [Seqs -> state] frozen body state captured at each snapshot publish
    nextId,        \* next id to allocate (1..MaxSeq+1)
    completed,     \* BOOLEAN  GC namespace-cleanup Completed for the current remove (marker durable)
    badRecreate,   \* ghost: a recreation happened without a durable Completed (sabotage only)
    droppedEver,   \* ghost: late PUTs that landed under an existing snapshot (rev.6 coverage-at-birth)
    rPhase,        \* "idle"|"scan"|"fetch"|"done"|"failed"|"stuck"
    rScanPos,      \* last key returned by the ordered scan (resume-after-last-returned-key)
    rSeenLogs,     \* SUBSET Seqs  log ids enumerated in this scan
    rSeenSnaps,    \* SUBSET Seqs  snapshot ids enumerated in this scan
    rPickedSnap,   \* chosen snapshot id (0 = empty base)
    rRestarts,     \* restart counter
    rStartedAfterDrop \* ghost: this reader started after the last late delivery (rev.6)

vars == << op, writtenEver, logs, snaps, publishedEver, snapCov, nextId, completed,
           badRecreate, droppedEver, rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap,
           rRestarts, rStartedAfterDrop >>

MinOf(S) == CHOOSE x \in S : \A y \in S : x <= y
MaxOf(S) == CHOOSE x \in S : \A y \in S : x >= y

(* Fold one op into the table state. birth/remove/rebirth reset content; mut adds its id. Never
   invoked with "none" (folds range only over durable ids); OTHER is an identity guard. *)
ApplyOp(s, id) ==
    CASE op[id] = "birth"   -> [life |-> "live",    committed |-> {},                   removeSeq |-> 0]
      [] op[id] = "mut"     -> [life |-> s.life,    committed |-> s.committed \cup {id}, removeSeq |-> s.removeSeq]
      [] op[id] = "remove"  -> [life |-> "removed", committed |-> {},                   removeSeq |-> id]
      [] op[id] = "rebirth" -> [life |-> "live",    committed |-> {},                   removeSeq |-> 0]
      [] OTHER              -> s

RECURSIVE FoldIds(_, _)
FoldIds(base, S) ==
    IF S = {} THEN base
    ELSE LET m == MinOf(S) IN FoldIds(ApplyOp(base, m), S \ {m})

WState == FoldIds(EmptyState, writtenEver)               \* Replay(full history) — the oracle
LifeBelow(L) == FoldIds(EmptyState, { i \in writtenEver : i < L }).life

\* rev.6 oracle: contract-clean truth excludes never-ACKed writes dropped by coverage-at-birth
WStateRev6 == FoldIds(EmptyState, writtenEver \ droppedEver)

(* Ordered key space: all _log keys (kind 0) sort before all _snap keys (kind 1); within a kind by
   id. This is `_log` < `_snap` from `l < s` in the spec's canonical prefix order. *)
KeyLt(a, b) == (a.kind < b.kind) \/ (a.kind = b.kind /\ a.id < b.id)
PresentKeys == { [kind |-> 0, id |-> i] : i \in logs } \cup { [kind |-> 1, id |-> i] : i \in snaps }

Init ==
    /\ op = [i \in Seqs |-> "none"]
    /\ writtenEver = {}
    /\ logs = {}
    /\ snaps = {}
    /\ publishedEver = {}
    /\ snapCov = [i \in Seqs |-> EmptyState]
    /\ nextId = 1
    /\ completed = FALSE
    /\ badRecreate = FALSE
    /\ droppedEver = {}
    /\ rPhase = "idle"
    /\ rScanPos = StartPos
    /\ rSeenLogs = {}
    /\ rSeenSnaps = {}
    /\ rPickedSnap = 0
    /\ rRestarts = 0
    /\ rStartedAfterDrop = TRUE

ReaderInactive == rPhase = "idle"

(* ---- writer: build the immutable log history (frozen while a reader recovers) ---- *)

WriterBirth ==
    /\ ReaderInactive
    /\ nextId <= MaxSeq
    /\ WState.life = "empty"
    /\ op' = [op EXCEPT ![nextId] = "birth"]
    /\ writtenEver' = writtenEver \cup {nextId}
    /\ logs' = logs \cup {nextId}
    /\ nextId' = nextId + 1
    /\ UNCHANGED << snaps, publishedEver, snapCov, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

WriterMut ==
    /\ ReaderInactive
    /\ nextId <= MaxSeq
    /\ WState.life = "live"
    /\ op' = [op EXCEPT ![nextId] = "mut"]
    /\ writtenEver' = writtenEver \cup {nextId}
    /\ logs' = logs \cup {nextId}
    /\ nextId' = nextId + 1
    /\ UNCHANGED << snaps, publishedEver, snapCov, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

WriterRemove ==
    /\ ReaderInactive
    /\ nextId <= MaxSeq
    /\ WState.life = "live"
    /\ op' = [op EXCEPT ![nextId] = "remove"]
    /\ writtenEver' = writtenEver \cup {nextId}
    /\ logs' = logs \cup {nextId}
    /\ nextId' = nextId + 1
    /\ completed' = FALSE                     \* a fresh removal starts an uncompleted cleanup item
    /\ UNCHANGED << snaps, publishedEver, snapCov, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* Recreation gate (spec §namespace-birth): namespace_birth over Removed requires the durable
   Completed marker. Honest run needs `completed`; the sabotage bypasses it and trips badRecreate. *)
WriterRebirth ==
    /\ ReaderInactive
    /\ nextId <= MaxSeq
    /\ WState.life = "removed"
    /\ (completed \/ SabotageRecreateBeforeCompleted)
    /\ op' = [op EXCEPT ![nextId] = "rebirth"]
    /\ writtenEver' = writtenEver \cup {nextId}
    /\ logs' = logs \cup {nextId}
    /\ nextId' = nextId + 1
    /\ badRecreate' = (badRecreate \/ ~completed)
    /\ UNCHANGED << snaps, publishedEver, snapCov, completed,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* A definitely-failed conditional create leaves a safe id gap (spec §writer-side-linearization). *)
WriterFail ==
    /\ ReaderInactive
    /\ nextId <= MaxSeq
    /\ nextId' = nextId + 1
    /\ UNCHANGED << op, writtenEver, logs, snaps, publishedEver, snapCov, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* GC namespace-cleanup item reaches Completed for the current removal; the writer will observe the
   marker on its next recovery LIST and may then recreate. *)
GcComplete ==
    /\ ReaderInactive
    /\ WState.life = "removed"
    /\ ~completed
    /\ completed' = TRUE
    /\ UNCHANGED << op, writtenEver, logs, snaps, publishedEver, snapCov, nextId, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* ---- snapshot publication (writer, off-lane; may run during a reader's recovery) ---- *)

(* rev.6 (`Rev6MountRule`): publication is copy-once-from-live (spec §recovery-seal/§seal-soundness)
   -- a late PUT dropped at birth is "born covered ... for every observer uniformly, forever", so it
   must never resurface in a LATER snapshot's frozen bytes either, not only in the reader oracle.
   Legacy (`~Rev6MountRule`) keeps the original raw refold over `writtenEver`. *)
CoveredFold(X) == IF Rev6MountRule
                   THEN FoldIds(EmptyState, { i \in (writtenEver \ droppedEver) : i <= X })
                   ELSE FoldIds(EmptyState, { i \in writtenEver : i <= X })

WriterPublishSnapshot ==
    /\ \E X \in writtenEver :
        /\ X \notin publishedEver
        /\ \E j \in writtenEver : j > X                       \* grace: never cover the newest log
        /\ publishedEver' = publishedEver \cup {X}
        /\ snaps' = snaps \cup {X}
        /\ snapCov' = [snapCov EXCEPT ![X] = CoveredFold(X)]
    /\ UNCHANGED << op, writtenEver, logs, nextId, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* ---- GC ref-object cleanup (may run during a reader's recovery) ---- *)

(* Correct: a log is deletable only under a snapshot OBSERVED durable in this scan (present in
   `snaps`). Sabotage: delete a log with no such covering snapshot (spec sabotage: "deleting logs
   before snapshot X is durable"). *)
Covered(L) == IF SabotageDeleteBeforeSnapshot THEN TRUE ELSE \E X \in snaps : X >= L
GcCleanupLog ==
    /\ \E L \in logs :
        /\ Covered(L)
        /\ logs' = logs \ {L}
    /\ UNCHANGED << op, writtenEver, snaps, publishedEver, snapCov, nextId, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

GcCleanupSnap ==
    /\ \E S \in snaps :
        /\ \E X \in snaps : X > S                             \* keep the newest snapshot
        /\ snaps' = snaps \ {S}
    /\ UNCHANGED << op, writtenEver, logs, publishedEver, snapCov, nextId, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* ---- adversarial: late predecessor PUT (spec §late-predecessor-put; expected-fail) ---- *)

(* A fenced predecessor's ref-log PUT materializes AFTER a successor snapshot already covered past
   its id: a durable `mut` at id L below a present snapshot, never counted by that snapshot's frozen
   bytes. Recovery through that snapshot then reconstructs a state missing L — the known Phase-1
   loss. Enabled only under LatePred; it must break INV_RECOVERY.

   rev.6 (`Rev6MountRule`): coverage-at-birth seals the successor's mount against exactly this
   straggler — the late PUT still lands (the S3 request cannot be cancelled), but it is folded out
   of the ghost oracle `WStateRev6` via `droppedEver`, so a reader that reconstructs against the
   rev.6-aware oracle stays correct. `rStartedAfterDrop` marks readers that began after this landed;
   only an in-flight reader may transiently observe the dropped log (`NoDivergentFold` vs
   `INV_FRESH_READER`, spec §late-predecessor-put rev.6 addendum). *)
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

(* ---- adversarial: self-remount keeps the old epoch and a stale cache (spec §write-fence; C1) ---- *)

(* A self-remount that fails to re-establish the ref-protocol incarnation keeps stamping FRESH appends
   with the OLD, now-below-durable epoch, from a cache that never saw a same-uuid twin's intermediate
   work. Modeled as a durable `mut` at an id L that sorts BELOW an already-durable (twin) log AND below a
   present snapshot that was frozen before L existed: recovery through that snapshot reconstructs a state
   missing L -- the missed-`+1` loss the pagination premise ("a new log is never inserted at or below an
   already durable table log id") leans on.

   The FIX routes `allocateRefTxnId` through the fresh `live_writer_epoch` (so a remount append always
   sorts strictly ABOVE every durable log -- it is just an ordinary `WriterMut` above the frontier) and
   re-recovers the dropped cache; so the honest protocol has NO such action and the safe config is GREEN.
   This is DISTINCT from `LatePredecessorPut`: that is an in-flight EXTERNAL straggler PUT the epoch bump
   cannot cancel (an accepted Phase-1 limitation), whereas this is the live writer's OWN fresh appends,
   which the epoch route provably eliminates. Enabled only under `SabotageRemountKeepsOldEpoch`; it must
   break INV_RECOVERY. *)
RemountStaleAppend ==
    /\ SabotageRemountKeepsOldEpoch
    /\ \E L \in Seqs :
        /\ op[L] = "none"
        /\ L \notin writtenEver
        /\ \E X \in snaps : X > L                             \* a present snapshot already covers past L (the twin's)
        /\ LifeBelow(L) = "live"                              \* a mid-history mut at L is well-formed
        /\ op' = [op EXCEPT ![L] = "mut"]
        /\ writtenEver' = writtenEver \cup {L}
        /\ logs' = logs \cup {L}
    /\ UNCHANGED << snaps, publishedEver, snapCov, nextId, completed, badRecreate,
                    rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts,
                    droppedEver, rStartedAfterDrop >>

(* ---- reader: one ordered scan + body fetch with restart-on-vanish ---- *)

ReaderStart ==
    /\ rPhase = "idle"
    /\ writtenEver # {}
    /\ rPhase' = "scan"
    /\ rScanPos' = StartPos
    /\ rSeenLogs' = {}
    /\ rSeenSnaps' = {}
    /\ rPickedSnap' = 0
    /\ rRestarts' = 0
    /\ rStartedAfterDrop' = TRUE   \* this reader began after every late delivery so far
    /\ UNCHANGED << op, writtenEver, logs, snaps, publishedEver, snapCov, nextId, completed,
                    badRecreate, droppedEver >>

(* One page = one key. Between pages, cleanup/publication interleave. Resume strictly after the last
   returned key. When no present key sorts after the cursor, the scan is complete: pick the greatest
   enumerated snapshot (empty base if none) and move to body fetch. *)
ReaderScanStep ==
    /\ rPhase = "scan"
    /\ LET cands == { k \in PresentKeys : KeyLt(rScanPos, k) }
       IN IF cands = {}
          THEN /\ rPickedSnap' = IF rSeenSnaps = {} THEN 0 ELSE MaxOf(rSeenSnaps)
               /\ rPhase' = "fetch"
               /\ UNCHANGED << rScanPos, rSeenLogs, rSeenSnaps, rRestarts >>
          ELSE LET k == CHOOSE k0 \in cands : \A k1 \in cands : ~ KeyLt(k1, k0)
               IN /\ rScanPos' = k
                  /\ rSeenLogs' = IF k.kind = 0 THEN rSeenLogs \cup {k.id} ELSE rSeenLogs
                  /\ rSeenSnaps' = IF k.kind = 1 THEN rSeenSnaps \cup {k.id} ELSE rSeenSnaps
                  /\ rPhase' = "scan"
                  /\ UNCHANGED << rPickedSnap, rRestarts >>
    /\ UNCHANGED << op, writtenEver, logs, snaps, publishedEver, snapCov, nextId, completed,
                    badRecreate, droppedEver, rStartedAfterDrop >>

(* Fetch the picked snapshot body and every enumerated tail log (id > picked). If all present,
   complete. If a selected object vanished: honest = restart with a fresh scan (bounded/counted);
   sabotage = declare corruption (permanent failure). Bounded restarts exhausted -> stuck (not a
   failure). Body fetch is checked atomically: a vanish mid-fetch is equivalent to a vanish before
   fetch for the restart outcome. *)
ReaderFetch ==
    /\ rPhase = "fetch"
    /\ LET needSnap == rPickedSnap > 0
           snapOk == (~ needSnap) \/ (rPickedSnap \in snaps)
           tailLogs == { i \in rSeenLogs : i > rPickedSnap }
           tailOk == tailLogs \subseteq logs
           allOk == snapOk /\ tailOk
       IN IF allOk
          THEN /\ rPhase' = "done"
               /\ UNCHANGED << rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts >>
          ELSE IF SabotageVanishIsCorruption
               THEN /\ rPhase' = "failed"
                    /\ UNCHANGED << rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts >>
               ELSE IF rRestarts < MaxRestarts
                    THEN /\ rPhase' = "scan"
                         /\ rScanPos' = StartPos
                         /\ rSeenLogs' = {}
                         /\ rSeenSnaps' = {}
                         /\ rPickedSnap' = 0
                         /\ rRestarts' = rRestarts + 1
                    ELSE /\ rPhase' = "stuck"
                         /\ UNCHANGED << rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts >>
    /\ UNCHANGED << op, writtenEver, logs, snaps, publishedEver, snapCov, nextId, completed,
                    badRecreate, droppedEver, rStartedAfterDrop >>

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
    \/ WriterBirth \/ WriterMut \/ WriterRemove \/ WriterRebirth \/ WriterFail
    \/ GcComplete
    \/ WriterPublishSnapshot
    \/ GcCleanupLog \/ GcCleanupSnap
    \/ LatePredecessorPut
    \/ RemountStaleAppend
    \/ ReaderStart \/ ReaderScanStep \/ ReaderFetch
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

(* The reader's reconstruction = newest visible valid snapshot + surviving later logs. *)
Reconstruct ==
    LET base == IF rPickedSnap = 0 THEN EmptyState ELSE snapCov[rPickedSnap]
        tail == { i \in rSeenLogs : i > rPickedSnap }
    IN FoldIds(base, tail)

(* (1) Central recovery invariant (spec §tla-models):
   Replay(newest visible valid snapshot, surviving later logs) = Replay(full history). *)
INV_RECOVERY == (rPhase = "done") => (Reconstruct = WState)

(* (2) Cleanup never strands a completing reader: a vanished selected object is a restart signal,
   never a permanent failure. *)
INV_NOFAIL == rPhase # "failed"

(* (3) Recreation never observed before a durable Completed marker. *)
INV_RECREATE == ~ badRecreate

(* ---- rev.6: coverage-at-birth seal (`Rev6MountRule`) ----
   Legacy configs set `Rev6MountRule = FALSE`, so `NoDivergentFold`/`INV_FRESH_READER`/
   `INV_SNAP_DETERMINISTIC` are vacuous there and `INV_RECOVERY` keeps its old meaning. *)

(* (4) Strict: every finished reader reconstructs the rev.6 oracle. Expected to VIOLATE under
   `_rev6_latedelivery` (an in-flight reader may transiently see the dropped log — the documented
   accepted transient); expected GREEN under `_rev6_safe` (no late delivery can occur there). *)
NoDivergentFold == (rPhase = "done" /\ Rev6MountRule) => (Reconstruct = WStateRev6)

(* (5) Weak (post-T_mat violation containment): a reader that STARTED after the last late
   delivery always agrees. In-flight readers may transiently include the dropped log; folds
   re-derive each round, error direction is spare-not-delete. *)
INV_FRESH_READER == (rPhase = "done" /\ Rev6MountRule /\ rStartedAfterDrop)
                        => (Reconstruct = WStateRev6)

(* (6) Snapshot byte-determinism under rev.6: every published body equals the oracle fold below it. *)
INV_SNAP_DETERMINISTIC == Rev6MountRule =>
    \A X \in snaps : snapCov[X] = FoldIds(EmptyState, {i \in (writtenEver \ droppedEver) : i <= X})

TypeOK ==
    /\ op \in [Seqs -> Ops]
    /\ writtenEver \subseteq Seqs
    /\ logs \subseteq writtenEver
    /\ publishedEver \subseteq writtenEver
    /\ snaps \subseteq publishedEver
    /\ nextId \in 1..(MaxSeq + 1)
    /\ completed \in BOOLEAN
    /\ badRecreate \in BOOLEAN
    /\ droppedEver \subseteq writtenEver
    /\ rPhase \in {"idle", "scan", "fetch", "done", "failed", "stuck"}
    /\ rScanPos.kind \in {-1, 0, 1}
    /\ rSeenLogs \subseteq Seqs
    /\ rSeenSnaps \subseteq Seqs
    /\ rPickedSnap \in 0..MaxSeq
    /\ rRestarts \in 0..MaxRestarts
    /\ rStartedAfterDrop \in BOOLEAN

THEOREM Spec => [](TypeOK /\ INV_RECOVERY /\ INV_NOFAIL /\ INV_RECREATE)
=============================================================================
