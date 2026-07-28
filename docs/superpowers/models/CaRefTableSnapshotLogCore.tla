-------------------- MODULE CaRefTableSnapshotLogCore --------------------
(* One namespace's ref stream: contiguous append-only log + snapshot + `_ckpt` — spec
   2026-07-27-cas-ref-chain-complete-cut-design.md (v9), §2 INV-1/INV-2/INV-4 and §4 Recovery.
   TLA+ phase (gate), Task 1.

   Scope:

   - The namespace's true history is a sequence of immutable ref transactions with per-namespace
     CONTIGUOUS ids (`writtenEver`, the recovery oracle — it never shrinks even after cleanup
     deletes objects). Each durable id has one op: `birth` | `mut` | `remove`. Folding the ops of
     an id set in id order is the table state (`WState` = Replay(full history)).
   - INV-1, the allocator: there is no pool-wide atomic. The next id is derived from state
     (`NextSlot` = `MaxWritten` + 1) and at most one append is in flight (`pendingSlot`). An id is
     freed for reuse only under the EVERY-ATTEMPT rule: nothing was sent, or every sent attempt has
     its own conclusive rejection. A slot that ever had an AMBIGUOUS attempt (`ambiguousEver`)
     keeps the lane wedged; a successor's `EpochSeal` at that slot is itself a conclusive rejection.
   - INV-2, the seal: recovery closes the dead tail in-band with the `slot-occupy` primitive. At the
     first absent expected-next id the store's conditional create either succeeds (`Created` —
     `RSealSlot` occupies the slot, and the dead lane's straggler can never land there) or fails
     (`Occupied` — `RAdoptStraggler`: the straggler landed first, so adopt it and keep walking).
     The store's conditional create IS the fence; `_sab_blindput` shows an unconditional PUT breaks
     it.
   - INV-4, `_ckpt`: `ckpt` = `[base, seal]` is a token-CAS object, merged by semantic maximum.
     Logs are deletable only at or below `ckpt.base`, snapshots only STRICTLY below it. Recovery
     POINT-READS the base from `ckpt` and walks the tail by ARITHMETIC (`last + 1`); a missing base
     with an unchanged token is corruption, a missing base with an advanced token is a restart.
   - Recovery (§4): `_ckpt` → exact-key snapshot (three-way revalidation) → arithmetic tail →
     CAS-walk + seal → install. Acked => durable => dense => found.

   Deliberate scoping of this focused model (documented so the oracle stays decisive):

   - ONE namespace, ONE incarnation. The catalog, incarnations, namespace rebirth and the durable
     `Completed` marker of rev.4 are DELETED from this module — namespace lifecycle is
     `CaRefCatalogCore`'s and `CaRefNsCleanupStaleLeaderCore`'s subject. Ops reduce to
     birth/mut/remove.
   - The LIST hint is NOT modelled on the honest path, because v9 consumes it nowhere: recovery is a
     point read plus arithmetic. It survives as `RScanStep`/`RScanInstall`, an OMISSION-CAPABLE
     enumeration reachable only under `SabotageScanIsTruth`, whose whole purpose is to reproduce the
     observed `0x1430c`/`0x1430d` hidden-hole defect as a model counterexample.
   - Ordinary appends are frozen while a reader recovers (`ReaderInactive`) because recovery is a
     fresh mount with the previous writer's lane drained. What the drained lane can still do — land
     its in-flight PUT — is exactly `LatePredecessorPut` (`LatePred`) and `RAdoptStraggler`.
   - `LatePredecessorPut` is the rev.4 EXPECTED-FAIL, FLIPPED. Under v9 it is GREEN
     (`_v9_flip_latepred`): contiguity puts every straggler at the frontier, never below a published
     snapshot, and the slot-occupy seal fences the one that never landed. `_sab_noseal` is the same
     behaviour with the occupancy removed and reproduces the rev.4 counterexample, so the flip is
     proven, not assumed.
   - Seals are never cleaned in this model (they are tiny, and a walk crossing a dead epoch must
     find them).

   Ghost variables carry sabotage detection and are empty/FALSE in every honest run:
   `concludedEver` (slots a completed recovery declared final), `burnedIds` (ids the allocator
   skipped), `orphanIds` (attempts abandoned while still in flight), `phantomEver` (a conclusively
   rejected write became durable).

   Each Sabotage* constant breaks exactly one load-bearing rule and MUST yield a counterexample;
   with all Sabotage* false the model is GREEN for both `LatePred` settings. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    MaxSeq,                          \* a writer may append ids 1..MaxSeq
    MaxRestarts,                     \* bounded recovery restarts (spec §4)
    LatePred,                        \* the drained lane's PUT lands while a successor recovers
    SabotageReuseAfterAmbiguous,     \* free an id whose attempt was AMBIGUOUS (breaks every-attempt)
    SabotageGapOnFail,               \* rev.4 allocator: a failed create burns its id, leaving a gap
    SabotageNoSeal,                  \* recovery concludes the frontier without OCCUPYING the slot
    SabotageBlindPut,                \* the lane PUTs unconditionally instead of conditional-create
    SabotageScanIsTruth,             \* the reader folds what the LIST hint returned, not the walk
    SabotageCleanupAboveCkpt,        \* cleanup deletes logs above `ckpt.base`
    SabotageStaleCkptCorruption      \* cleanup deletes the snapshot `ckpt.base` still points at

Seqs == 1..MaxSeq                    \* ids a writer may append
Ids  == 1..(MaxSeq + 1)              \* + the slot a frontier seal may occupy
Ops  == {"none", "birth", "mut", "remove"}
Lifes == {"empty", "live", "removed"}
EmptyState == [life |-> "empty", committed |-> {}, removeSeq |-> 0]

VARIABLES
    op,            \* [Ids -> Ops]   content bound to an id at ATTEMPT time (durable iff in writtenEver)
    writtenEver,   \* SUBSET Seqs    every id ever durably appended (recovery oracle; never shrinks)
    logs,          \* SUBSET Seqs    present _log/<id> data objects
    snaps,         \* SUBSET Seqs    present _snap/<id> objects
    publishedEver, \* SUBSET Seqs    snapshot ids ever published
    snapCov,       \* [Ids -> state] frozen body captured at each snapshot publish
    ckpt,          \* [base |-> 0..MaxSeq, seal |-> 0..MaxSeq+1]  the _ckpt object; 0 = none
    pendingSlot,   \* 0..MaxSeq      the single in-flight append (0 = the lane is idle)
    ambiguousEver, \* SUBSET Seqs    slots that ever had an attempt of unknown outcome
    sealedIds,     \* SUBSET Ids     slots PHYSICALLY occupied by an EpochSeal
    concludedEver, \* ghost: slots a completed recovery declared final (= sealedIds when honest)
    burnedIds,     \* ghost: ids the allocator skipped without writing them (sabotage only)
    orphanIds,     \* ghost: attempts abandoned while their PUT may still land (sabotage only)
    phantomEver,   \* ghost: a write whose failure was reported to the caller became durable
    rPhase,        \* "idle"|"ckpt"|"base"|"walk"|"scan"|"done"|"stuck"|"corrupt"
    rBase,         \* the ckpt.base this recovery sampled
    rWalkPos,      \* arithmetic walk cursor (last id proven present)
    rTail,         \* SUBSET Seqs    ids this recovery will fold above the base
    rScanPos,      \* hint-enumeration cursor (resume-after-last-returned-key)
    rSeenLogs,     \* SUBSET Seqs    ids the hint enumeration returned
    rRestarts      \* restart counter

storeVars  == << op, writtenEver, logs, snaps, publishedEver, snapCov >>
laneVars   == << pendingSlot, ambiguousEver >>
sealVars   == << sealedIds, concludedEver >>
ghostVars  == << burnedIds, orphanIds, phantomEver >>
readerVars == << rPhase, rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>

vars == << op, writtenEver, logs, snaps, publishedEver, snapCov, ckpt,
           pendingSlot, ambiguousEver, sealedIds, concludedEver,
           burnedIds, orphanIds, phantomEver,
           rPhase, rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>

MinOf(S) == CHOOSE x \in S : \A y \in S : x <= y
MaxOf(S) == CHOOSE x \in S : \A y \in S : x >= y
MaxOr0(S) == IF S = {} THEN 0 ELSE MaxOf(S)

(* Fold one op into the table state. birth/remove reset content; mut adds its id. Never invoked
   with "none" on the honest path (folds range only over durable ids); OTHER is an identity guard,
   which also makes an `EpochSeal` an applied no-op (spec §5, B1/B2). *)
ApplyOp(s, id) ==
    CASE op[id] = "birth"  -> [life |-> "live",    committed |-> {},                   removeSeq |-> 0]
      [] op[id] = "mut"    -> [life |-> s.life,    committed |-> s.committed \cup {id}, removeSeq |-> s.removeSeq]
      [] op[id] = "remove" -> [life |-> "removed", committed |-> {},                   removeSeq |-> id]
      [] OTHER             -> s

RECURSIVE FoldIds(_, _)
FoldIds(base, S) ==
    IF S = {} THEN base
    ELSE LET m == MinOf(S) IN FoldIds(ApplyOp(base, m), S \ {m})

WState == FoldIds(EmptyState, writtenEver)               \* Replay(full history) — the oracle

(* INV-1: ids derive from state. `burnedIds` is the rev.4 allocator's gap and is empty when honest. *)
MaxWritten == MaxOr0(writtenEver \cup sealedIds \cup burnedIds)
NextSlot == MaxWritten + 1

Init ==
    /\ op = [i \in Ids |-> "none"]
    /\ writtenEver = {}
    /\ logs = {}
    /\ snaps = {}
    /\ publishedEver = {}
    /\ snapCov = [i \in Ids |-> EmptyState]
    /\ ckpt = [base |-> 0, seal |-> 0]
    /\ pendingSlot = 0
    /\ ambiguousEver = {}
    /\ sealedIds = {}
    /\ concludedEver = {}
    /\ burnedIds = {}
    /\ orphanIds = {}
    /\ phantomEver = FALSE
    /\ rPhase = "idle"
    /\ rBase = 0
    /\ rWalkPos = 0
    /\ rTail = {}
    /\ rScanPos = 0
    /\ rSeenLogs = {}
    /\ rRestarts = 0

ReaderInactive == rPhase = "idle"

(* ---- writer lane: at most one in-flight append, ids derived from state (INV-1) ---- *)

(* The bytes are bound to the id when the attempt is FORMED, not when it lands: an abandoned attempt
   that reaches the store later carries exactly these bytes. Durability is `writtenEver` alone. *)
WAppendStart(o) ==
    /\ ReaderInactive
    /\ pendingSlot = 0
    /\ NextSlot <= MaxSeq
    /\ \/ (o = "birth"  /\ WState.life = "empty")
       \/ (o = "mut"    /\ WState.life = "live")
       \/ (o = "remove" /\ WState.life = "live")
    /\ pendingSlot' = NextSlot
    /\ op' = [op EXCEPT ![NextSlot] = o]
    /\ UNCHANGED << writtenEver, logs, snaps, publishedEver, snapCov >>
    /\ UNCHANGED << ckpt, ambiguousEver >>
    /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

WriterAppend == \E o \in {"birth", "mut", "remove"} : WAppendStart(o)

(* An attempt was sent and its outcome is unknown. The lane is now WEDGED: it may not free the id. *)
WAttemptAmbiguous ==
    /\ pendingSlot # 0
    /\ pendingSlot \notin ambiguousEver
    /\ ambiguousEver' = ambiguousEver \cup {pendingSlot}
    /\ UNCHANGED << pendingSlot, ckpt >>
    /\ UNCHANGED storeVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* The conditional create succeeded: the record is durable at its slot. `SabotageBlindPut` drops the
   conditional-create fence, letting the lane overwrite a slot a successor already sealed. *)
WResolveDurable ==
    /\ ReaderInactive
    /\ pendingSlot # 0
    /\ (pendingSlot \notin sealedIds \/ SabotageBlindPut)
    /\ writtenEver' = writtenEver \cup {pendingSlot}
    /\ logs' = logs \cup {pendingSlot}
    /\ pendingSlot' = 0
    /\ UNCHANGED << op, snaps, publishedEver, snapCov >>
    /\ UNCHANGED << ckpt, ambiguousEver >>
    /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* Every-attempt rule: only a PROVEN-never-applied attempt frees the id, and the freed id is REUSED
   by the next WAppendStart — no gap. Under `SabotageReuseAfterAmbiguous` the lane also frees a slot
   whose attempt was ambiguous; that attempt's bytes are still out there (`orphanIds`) and its caller
   has already been told the operation FAILED. Under `SabotageGapOnFail` the freed id is burned
   instead of reused — the rev.4 "a definitely-failed create leaves a safe id gap" allocator. *)
WResolveConclusiveReject ==
    /\ ReaderInactive
    /\ pendingSlot # 0
    /\ (pendingSlot \notin ambiguousEver \/ SabotageReuseAfterAmbiguous)
    /\ IF pendingSlot \in ambiguousEver
       THEN /\ orphanIds' = orphanIds \cup {pendingSlot}
            /\ UNCHANGED op
       ELSE /\ orphanIds' = orphanIds
            /\ op' = [op EXCEPT ![pendingSlot] = "none"]
    /\ burnedIds' = IF SabotageGapOnFail THEN burnedIds \cup {pendingSlot} ELSE burnedIds
    /\ pendingSlot' = 0
    /\ UNCHANGED << writtenEver, logs, snaps, publishedEver, snapCov >>
    /\ UNCHANGED << ckpt, ambiguousEver, phantomEver >>
    /\ UNCHANGED sealVars /\ UNCHANGED readerVars

(* A successor's EpochSeal found at the slot is itself a conclusive rejection: the wedged lane
   retries T+1 (state-derived), it never mints T+2. *)
WResolveSealRejected ==
    /\ pendingSlot # 0
    /\ pendingSlot \in sealedIds
    /\ op' = [op EXCEPT ![pendingSlot] = "none"]
    /\ pendingSlot' = 0
    /\ UNCHANGED << writtenEver, logs, snaps, publishedEver, snapCov >>
    /\ UNCHANGED << ckpt, ambiguousEver >>
    /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* An abandoned attempt reaches the store after its caller was told it failed. Reachable only after
   `SabotageReuseAfterAmbiguous` produced an orphan; the conditional create still fences it out of
   any slot that is taken. Its landing is what makes the reported failure a lie. *)
WOrphanLands ==
    /\ \E k \in orphanIds :
        /\ k \notin writtenEver
        /\ k \notin sealedIds
        /\ k # pendingSlot
        /\ op[k] # "none"
        /\ writtenEver' = writtenEver \cup {k}
        /\ logs' = logs \cup {k}
        /\ orphanIds' = orphanIds \ {k}
    /\ phantomEver' = TRUE
    /\ UNCHANGED << op, snaps, publishedEver, snapCov >>
    /\ UNCHANGED << ckpt, burnedIds >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED readerVars

(* ---- adversarial: the drained lane's PUT lands while a successor recovers ----

   rev.4 §late-predecessor-put was the documented EXPECTED-FAIL: a fenced predecessor's PUT
   materialized BELOW an already-published snapshot. Under v9 it cannot: ids are contiguous, so a
   straggler is always at the frontier (`pendingSlot` = MaxWritten + 1 > every published id), and
   the frontier slot is either adopted (`RAdoptStraggler`) or occupied by the seal — the store's
   conditional create is the fence (INV-2). Hence `_v9_flip_latepred` is GREEN, and `_sab_noseal`
   reproduces the old counterexample by removing the occupancy. *)
LatePredecessorPut ==
    /\ LatePred
    /\ ~ReaderInactive
    /\ pendingSlot # 0
    /\ (pendingSlot \notin sealedIds \/ SabotageBlindPut)
    /\ writtenEver' = writtenEver \cup {pendingSlot}
    /\ logs' = logs \cup {pendingSlot}
    /\ pendingSlot' = 0
    /\ UNCHANGED << op, snaps, publishedEver, snapCov >>
    /\ UNCHANGED << ckpt, ambiguousEver >>
    /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* ---- snapshot publication and the `_ckpt` CAS (INV-4), off-lane ---- *)

(* Publication is two steps — body PUT, then the `_ckpt` token-CAS — so cleanup can interleave
   between them (spec §9: "`_ckpt` races (cleanup between PUT and CAS ...)"). *)
WriterPublishSnapshot ==
    /\ \E X \in writtenEver :
        /\ X \notin publishedEver
        /\ X > ckpt.base
        /\ publishedEver' = publishedEver \cup {X}
        /\ snaps' = snaps \cup {X}
        /\ snapCov' = [snapCov EXCEPT ![X] = FoldIds(EmptyState, { i \in writtenEver : i <= X })]
    /\ UNCHANGED << op, writtenEver, logs, ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* One update algorithm, merge by semantic maximum per field, token-CAS (INV-4). *)
WriterCkptAdvance ==
    /\ \E X \in snaps :
        /\ X > ckpt.base
        /\ ckpt' = [ckpt EXCEPT !.base = X]
    /\ UNCHANGED storeVars
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* ---- GC ref-object cleanup, gated by `_ckpt` (INV-4) ---- *)

(* Logs are covered by the checkpoint snapshot at `ckpt.base`, so they are deletable AT OR BELOW it.
   The sabotage drops the gate: recovery's arithmetic walk then reads a 404 at a DURABLE id with an
   unchanged token, mistakes it for the frontier and seals there — a truncated install. *)
CleanupLogGate(L) == IF SabotageCleanupAboveCkpt THEN TRUE ELSE L <= ckpt.base
GcCleanupLog ==
    /\ \E L \in logs :
        /\ CleanupLogGate(L)
        /\ logs' = logs \ {L}
    /\ UNCHANGED << op, writtenEver, snaps, publishedEver, snapCov, ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* Snapshots are deletable only STRICTLY BELOW `ckpt.base`, so a stale pointer can only
   under-clean. The sabotage races the base away, making a sampled base vanish under an UNCHANGED
   token — which the reader is required to call corruption. *)
CleanupSnapGate(S) == IF SabotageStaleCkptCorruption THEN S <= ckpt.base ELSE S < ckpt.base
GcCleanupSnap ==
    /\ \E S \in snaps :
        /\ CleanupSnapGate(S)
        /\ snaps' = snaps \ {S}
    /\ UNCHANGED << op, writtenEver, logs, publishedEver, snapCov, ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* ---- reader: `_ckpt` point read -> base -> arithmetic walk -> slot-occupy seal (§4) ---- *)

(* `NextSlot <= MaxSeq + 1` keeps the frontier seal inside the modelled id space; it is a bound, not
   a protocol rule. *)
ReaderStart ==
    /\ rPhase = "idle"
    /\ NextSlot <= MaxSeq + 1
    /\ rPhase' = "ckpt"
    /\ UNCHANGED << rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

(* The base is a POINT READ of `_ckpt`, never a listing. Each attempt re-reads it. *)
RReadCkpt ==
    /\ rPhase = "ckpt"
    /\ rBase' = ckpt.base
    /\ rWalkPos' = ckpt.base
    /\ rTail' = {}
    /\ rScanPos' = 0
    /\ rSeenLogs' = {}
    /\ rPhase' = "base"
    /\ UNCHANGED rRestarts
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

RRestartOrStuck ==
    IF rRestarts < MaxRestarts
    THEN /\ rPhase' = "ckpt"
         /\ rRestarts' = rRestarts + 1
         /\ UNCHANGED << rBase, rWalkPos, rTail, rScanPos, rSeenLogs >>
    ELSE /\ rPhase' = "stuck"
         /\ UNCHANGED << rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>

(* Exact-key fetch of the sampled base, with the three-way revalidation of INV-4: present -> walk;
   missing with an ADVANCED token -> restart; missing with an UNCHANGED token -> corruption, which
   the deletion gate makes unreachable in an honest run. *)
RFetchBase ==
    /\ rPhase = "base"
    /\ \/ /\ (rBase = 0 \/ rBase \in snaps)
          /\ rPhase' = IF SabotageScanIsTruth THEN "scan" ELSE "walk"
          /\ UNCHANGED << rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>
       \/ /\ rBase # 0
          /\ rBase \notin snaps
          /\ IF ckpt.base > rBase
             THEN RRestartOrStuck
             ELSE /\ rPhase' = "corrupt"
                  /\ UNCHANGED << rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

WalkNext == rWalkPos + 1

(* Arithmetic advance: the expected next id is `rWalkPos + 1` and its presence is an exact-key point
   read. A dead epoch is crossed by consuming its seal, which folds as a no-op. The hint plays no
   part. *)
RWalkStep ==
    /\ rPhase = "walk"
    /\ WalkNext \in Ids
    /\ (WalkNext \in logs \/ WalkNext \in sealedIds)
    /\ rWalkPos' = WalkNext
    /\ rTail' = IF WalkNext \in logs THEN rTail \cup {WalkNext} ELSE rTail
    /\ UNCHANGED << rPhase, rBase, rScanPos, rSeenLogs, rRestarts >>
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

(* A 404 BELOW the checkpoint means cleanup advanced the base past this id while we walked: restart
   from the newer base (bounded), never fail. *)
RWalkVanish ==
    /\ rPhase = "walk"
    /\ WalkNext \in Ids
    /\ WalkNext \notin logs
    /\ WalkNext \notin sealedIds
    /\ ckpt.base >= WalkNext
    /\ RRestartOrStuck
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

(* slot-occupy = Created: the expected next id is absent, so this is the frontier. Occupying it
   closes the epoch in-band and fences the dead lane's straggler forever. `SabotageNoSeal` keeps the
   CONCLUSION (`concludedEver`) but skips the OCCUPANCY — the rev.4 protocol, whose ghost then lands
   in a slot recovery already declared final. *)
RSealSlot ==
    /\ rPhase = "walk"
    /\ WalkNext \in Ids
    /\ WalkNext \notin logs
    /\ WalkNext \notin sealedIds
    /\ ckpt.base < WalkNext
    /\ sealedIds' = IF SabotageNoSeal THEN sealedIds ELSE sealedIds \cup {WalkNext}
    /\ concludedEver' = concludedEver \cup {WalkNext}
    /\ ckpt' = [ckpt EXCEPT !.seal = WalkNext]
    /\ rPhase' = "done"
    /\ UNCHANGED << rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>
    /\ UNCHANGED storeVars /\ UNCHANGED laneVars /\ UNCHANGED ghostVars

(* slot-occupy = Occupied: the in-flight PUT reached the store first. Adopt it and keep walking —
   the adopted id costs the conditional PUT plus an exact GET (spec §8). *)
RAdoptStraggler ==
    /\ rPhase = "walk"
    /\ WalkNext \in Ids
    /\ WalkNext \notin logs
    /\ WalkNext \notin sealedIds
    /\ pendingSlot = WalkNext
    /\ writtenEver' = writtenEver \cup {pendingSlot}
    /\ logs' = logs \cup {pendingSlot}
    /\ pendingSlot' = 0
    /\ UNCHANGED << op, snaps, publishedEver, snapCov, ckpt, ambiguousEver >>
    /\ UNCHANGED sealVars /\ UNCHANGED ghostVars /\ UNCHANGED readerVars

(* ---- the LIST hint, reachable only under `SabotageScanIsTruth` ----

   One page = one key, resume strictly after the last returned key — but the enumeration MAY SKIP
   present keys, which is the observed `0x1430c`/`0x1430d` shape: a listing that looks complete and
   silently omits a record. `RScanInstall` requires the cursor to be past every present key, so the
   counterexample is a complete-LOOKING scan with a hole, not a reader that gave up early. *)
RScanStep ==
    /\ rPhase = "scan"
    /\ \E k \in logs :
        /\ k > rScanPos
        /\ rScanPos' = k
        /\ rSeenLogs' = rSeenLogs \cup {k}
    /\ UNCHANGED << rPhase, rBase, rWalkPos, rTail, rRestarts >>
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

RScanInstall ==
    /\ rPhase = "scan"
    /\ ~ \E k \in logs : k > rScanPos
    /\ rTail' = { i \in rSeenLogs : i > rBase }
    /\ rPhase' = "done"
    /\ UNCHANGED << rBase, rWalkPos, rScanPos, rSeenLogs, rRestarts >>
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

(* A finished or stuck mount is torn down; the next mount recovers again, walking across the seal
   the previous one left. `corrupt` is terminal. *)
ReaderReset ==
    /\ rPhase \in {"done", "stuck"}
    /\ rPhase' = "idle"
    /\ rBase' = 0
    /\ rWalkPos' = 0
    /\ rTail' = {}
    /\ rScanPos' = 0
    /\ rSeenLogs' = {}
    /\ rRestarts' = 0
    /\ UNCHANGED storeVars /\ UNCHANGED << ckpt >>
    /\ UNCHANGED laneVars /\ UNCHANGED sealVars /\ UNCHANGED ghostVars

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
    \/ WriterAppend \/ WAttemptAmbiguous \/ WResolveDurable
    \/ WResolveConclusiveReject \/ WResolveSealRejected \/ WOrphanLands
    \/ LatePredecessorPut
    \/ WriterPublishSnapshot \/ WriterCkptAdvance
    \/ GcCleanupLog \/ GcCleanupSnap
    \/ ReaderStart \/ RReadCkpt \/ RFetchBase
    \/ RWalkStep \/ RWalkVanish \/ RSealSlot \/ RAdoptStraggler
    \/ RScanStep \/ RScanInstall
    \/ ReaderReset
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

(* The installed state = checkpoint base + the ids the walk proved present above it. *)
Reconstruct ==
    LET base == IF rBase = 0 THEN EmptyState ELSE snapCov[rBase]
    IN FoldIds(base, rTail)

(* (1) Central recovery invariant (spec §4: acked => durable => dense => found):
   Replay(checkpoint base, walked tail) = Replay(full history). *)
INV_RECOVERY == (rPhase = "done") => (Reconstruct = WState)

(* (2) Recovery never fails: a vanished object is a restart signal and, at worst, `stuck`.
   `corrupt` is the fail-closed branch of the three-way revalidation, and INV-4's deletion gate
   makes it unreachable while the gate holds. *)
INV_NOFAIL == rPhase # "corrupt"

(* (3) INV-1: within the namespace, durable ids are dense 1..T. The in-flight slot is the only id
   that may be absent, and only until it resolves. *)
INV_DENSE ==
    \A i \in (writtenEver \cup sealedIds) :
        \A j \in 1..i : j \in (writtenEver \cup sealedIds) \/ j = pendingSlot

(* (4) INV-2: nothing lands in a slot the recovery occupied with a seal. This is the property the
   store's conditional create provides and the whole in-band epoch close rests on. *)
INV_NO_GHOST == \A i \in writtenEver : i \notin sealedIds

(* (5) INV-1's every-attempt rule: an operation whose failure was reported to its caller never
   becomes durable. Freeing an id whose attempt was ambiguous is exactly how that promise breaks. *)
INV_NO_PHANTOM == ~ phantomEver

(* Non-vacuity witness for `_witness_hintlie` (spec §1; the observed 0x1430c/0x1430d shape): an
   enumeration that runs to the end of the key space can still omit a log that is PRESENT, so a
   reader that folds the hint installs a state missing a record nothing ever deleted. EXPECTED TO
   VIOLATE — the violation is the evidence that the hidden-hole shape is in the explored space, not
   a defect. It is the reason `_sab_scanistruth` cannot be dismissed as "the reader merely missed a
   vanish": under this witness there is nothing to vanish. *)
W_NO_HINT_HOLE == ~ (rPhase = "done" /\ \E i \in logs : i > rBase /\ i \notin rTail)

TypeOK ==
    /\ op \in [Ids -> Ops]
    /\ writtenEver \subseteq Seqs
    /\ logs \subseteq writtenEver
    /\ publishedEver \subseteq writtenEver
    /\ snaps \subseteq publishedEver
    /\ snapCov \in [Ids -> [life : Lifes, committed : SUBSET Seqs, removeSeq : 0..MaxSeq]]
    /\ ckpt.base \in 0..MaxSeq
    /\ ckpt.seal \in 0..(MaxSeq + 1)
    /\ pendingSlot \in 0..MaxSeq
    /\ ambiguousEver \subseteq Seqs
    /\ sealedIds \subseteq Ids
    /\ concludedEver \subseteq Ids
    /\ burnedIds \subseteq Seqs
    /\ orphanIds \subseteq Seqs
    /\ phantomEver \in BOOLEAN
    /\ rPhase \in {"idle", "ckpt", "base", "walk", "scan", "done", "stuck", "corrupt"}
    /\ rBase \in 0..MaxSeq
    /\ rWalkPos \in 0..(MaxSeq + 1)
    /\ rTail \subseteq Seqs
    /\ rScanPos \in 0..MaxSeq
    /\ rSeenLogs \subseteq Seqs
    /\ rRestarts \in 0..MaxRestarts

THEOREM Spec => [](TypeOK /\ INV_RECOVERY /\ INV_NOFAIL /\ INV_DENSE /\ INV_NO_GHOST /\ INV_NO_PHANTOM)
=============================================================================
