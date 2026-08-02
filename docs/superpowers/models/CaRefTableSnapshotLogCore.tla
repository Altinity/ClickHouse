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
    SabotageStaleCkptCorruption,     \* cleanup deletes the snapshot `ckpt.base` still points at
    SabotageSealClobbersBase,        \* the sealer writes back its SAMPLED base instead of merging
    FrontierSlice,                   \* Task 5b's focused log -> frontier -> install/ack state machine
    SabotageAckBeforeFrontier,       \* acknowledge a durable log before `_ckpt.committed_through`
    SabotageAllocateBeforeFrontier,  \* allocate the successor id before the frontier is durable
    SabotageInstallAboveFrontier,    \* recovery installs an unfrontiered object
    SabotageStaleFrontier,           \* a stale writer advances without a valid same-fence proof
    SabotageSnapshotAboveFrontier,   \* checkpoint snapshot exceeds `committed_through`
    SabotageSealAboveFrontier        \* checkpoint seal exceeds `committed_through`

Seqs == 1..MaxSeq                    \* ids a writer may append
Ids  == 1..(MaxSeq + 1)              \* + the slot a frontier seal may occupy
Ops  == {"none", "birth", "mut", "remove"}
Lifes == {"empty", "live", "removed"}
EmptyState == [life |-> "empty", committed |-> {}, removeSeq |-> 0]

FrontierPhases ==
    {"Idle", "Prepared", "LogDurable", "FrontierIssued", "FrontierDurable",
     "CheckpointRead", "RecoveryValidated", "SealDurable",
     "Installed", "Acknowledged", "NeedsRecovery", "Rejected"}
FrontierKinds == {"none", "log", "seal"}
FrontierFences == 1..2
FrontierState ==
    [phase : FrontierPhases,
     currentFence : FrontierFences,
     candidateId : 0..MaxSeq,
     candidateFence : 0..2,
     candidateKind : FrontierKinds,
     objectKind : [Seqs -> FrontierKinds],
     objectFence : [Seqs -> 0..2],
     objectPrev : [Seqs -> 0..MaxSeq],
     committedThrough : 0..MaxSeq,
     checkpointSnapshot : 0..MaxSeq,
     snapshotBodyThrough : 0..MaxSeq,
     lastEpochSeal : 0..MaxSeq,
     installedThrough : 0..MaxSeq,
     acknowledgedThrough : 0..MaxSeq,
     nextId : 1..(MaxSeq + 1),
     issuedId : 0..MaxSeq,
     issuedFence : 0..2,
     issuedKind : FrontierKinds,
     issuedToken : 0..(3 * MaxSeq + 4),
     checkpointToken : 0..(3 * MaxSeq + 4),
     sampledThrough : 0..MaxSeq,
     sampledToken : 0..(3 * MaxSeq + 4),
     lostResponsePending : BOOLEAN,
     recoveryAdoptingSuccessor : BOOLEAN,
     validFrontierProofs : SUBSET Seqs]

FrontierWitnessState ==
    [crashPrepared : BOOLEAN,
     crashLogDurable : BOOLEAN,
     crashFrontierDurable : BOOLEAN,
     crashInstalled : BOOLEAN,
     lostFrontierResponse : BOOLEAN,
     exactSuccessorAdopted : BOOLEAN,
     oldWriterLostToSeal : BOOLEAN,
     issuedCasLinearizedAfterFenceMove : BOOLEAN,
     snapshotPublishedAtFrontier : BOOLEAN,
     sealPublishedAtFrontier : BOOLEAN,
     lostSealResponseResolved : BOOLEAN]

VARIABLES
    op,            \* [Ids -> Ops]   content bound to an id at ATTEMPT time (durable iff in writtenEver)
    writtenEver,   \* SUBSET Seqs    every id ever durably appended (recovery oracle; never shrinks)
    logs,          \* SUBSET Seqs    present _log/<id> data objects
    snaps,         \* SUBSET Seqs    present _snap/<id> objects
    publishedEver, \* SUBSET Seqs    snapshot ids ever published
    snapCov,       \* [Ids -> state] frozen body captured at each snapshot publish
    ckpt,          \* [base |-> 0..MaxSeq, seal |-> 0..MaxSeq+1]  the _ckpt object; 0 = none
                   \* `base` = checkpoint_snapshot_id, `seal` = last_epoch_seal. Both writers
                   \* read-modify-write the WHOLE body and merge by semantic maximum per field
                   \* (INV-4), so a stale reader-writer can regress a field it did not intend to
                   \* touch — see `_sab_sealclobbersbase`. `seal` carries no proof obligation in
                   \* THIS module and is deliberately write-only here: its consumers are a later
                   \* mount locating the previous epoch's terminating record and the GC fold
                   \* crossing epochs (spec §5), neither of which exists in a single-recoverer,
                   \* single-namespace model. Its obligation belongs to `CaCasMountCore` (task 5)
                   \* and `CaRefDeltaIntakeCore` (task 2). It is still merged and typed here so
                   \* those models inherit a field that behaves correctly.
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
    rRestarts,     \* restart counter
    frontier,      \* Task 5b's focused checkpoint-frontier state
    frontierWitness \* reachability controls for every required honest crash/race window

storeVars  == << op, writtenEver, logs, snaps, publishedEver, snapCov >>
laneVars   == << pendingSlot, ambiguousEver >>
sealVars   == << sealedIds, concludedEver >>
ghostVars  == << burnedIds, orphanIds, phantomEver >>
readerVars == << rPhase, rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>
legacyVars == << op, writtenEver, logs, snaps, publishedEver, snapCov, ckpt,
                 pendingSlot, ambiguousEver, sealedIds, concludedEver,
                 burnedIds, orphanIds, phantomEver,
                 rPhase, rBase, rWalkPos, rTail, rScanPos, rSeenLogs, rRestarts >>
frontierVars == << frontier, frontierWitness >>

vars == << legacyVars, frontierVars >>

MinOf(S) == CHOOSE x \in S : \A y \in S : x <= y
MaxOf(S) == CHOOSE x \in S : \A y \in S : x >= y
MaxOr0(S) == IF S = {} THEN 0 ELSE MaxOf(S)
Max2(a, b) == IF a >= b THEN a ELSE b

(* INV-4's ONE update algorithm, shared by both `_ckpt` writers (the snapshot publisher and the
   sealer): read the whole body, merge by SEMANTIC MAXIMUM per field, token-CAS. Writing the whole
   body is what makes a stale field dangerous — a writer that skips the merge and writes back the
   value it sampled earlier silently regresses the OTHER writer's progress. Modelling it as a
   record-valued merge rather than an `EXCEPT !.field` surgical update is deliberate: the surgical
   form cannot express that regression, and the real code does not have it. *)
CkptMerge(base, seal) == [base |-> Max2(ckpt.base, base), seal |-> Max2(ckpt.seal, seal)]

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
    /\ frontier =
        [phase |-> "Idle",
         currentFence |-> 1,
         candidateId |-> 0,
         candidateFence |-> 0,
         candidateKind |-> "none",
         objectKind |-> [i \in Seqs |-> "none"],
         objectFence |-> [i \in Seqs |-> 0],
         objectPrev |-> [i \in Seqs |-> 0],
         committedThrough |-> 0,
         checkpointSnapshot |-> 0,
         snapshotBodyThrough |-> 0,
         lastEpochSeal |-> 0,
         installedThrough |-> 0,
         acknowledgedThrough |-> 0,
         nextId |-> 1,
         issuedId |-> 0,
         issuedFence |-> 0,
         issuedKind |-> "none",
         issuedToken |-> 0,
         checkpointToken |-> 0,
         sampledThrough |-> 0,
         sampledToken |-> 0,
         lostResponsePending |-> FALSE,
         recoveryAdoptingSuccessor |-> FALSE,
         validFrontierProofs |-> {}]
    /\ frontierWitness =
        [crashPrepared |-> FALSE,
         crashLogDurable |-> FALSE,
         crashFrontierDurable |-> FALSE,
         crashInstalled |-> FALSE,
         lostFrontierResponse |-> FALSE,
         exactSuccessorAdopted |-> FALSE,
         oldWriterLostToSeal |-> FALSE,
         issuedCasLinearizedAfterFenceMove |-> FALSE,
         snapshotPublishedAtFrontier |-> FALSE,
         sealPublishedAtFrontier |-> FALSE,
         lostSealResponseResolved |-> FALSE]

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

(* The publisher half of the one update algorithm: it knows a new base, knows nothing new about the
   seal, and merges both (INV-4). *)
WriterCkptAdvance ==
    /\ \E X \in snaps :
        /\ X > ckpt.base
        /\ ckpt' = CkptMerge(X, ckpt.seal)
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
    (* The sealer half of the one update algorithm. Its knowledge of the base is `rBase`, sampled
       at the START of this recovery, so the merge is what keeps a base another writer advanced
       meanwhile from being dragged backwards. `SabotageSealClobbersBase` writes the sampled body
       back verbatim — the read-modify-write with the merge left out. *)
    /\ ckpt' = IF SabotageSealClobbersBase
               THEN [base |-> rBase, seal |-> WalkNext]
               ELSE CkptMerge(rBase, WalkNext)
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

(* ---- Task 5b: exact committed frontier ----

   This focused slice is selected by `FrontierSlice`; legacy v9 configs leave it frozen. It makes
   the four externally meaningful states distinct: the immutable log is durable, its exact-chain
   checkpoint contribution is durable, the value is installed, and the caller is acknowledged
   (which is also when the successor id becomes allocatable). The checkpoint contribution carries
   an explicit proof bit for each committed id. The object's immutable producer fence is distinct
   from the checkpoint request's admission fence: recovery may contribute an exact old-fence
   successor using a CAS issued under its new current fence. Every request binds the exact sampled
   checkpoint token. An already-issued request may linearize after only the fence moves, but an
   intervening checkpoint update invalidates it and a stale actor may not issue a new request.

   `objectPrev` is the exact chain link. A record or seal at `T+1` is the only object writer recovery
   may adopt above `committedThrough`; no action chases a second successor. Snapshot and seal fields
   share the checkpoint frontier and may never exceed it. *)

FrontierObjectIsExact(id, kind) ==
    /\ id \in Seqs
    /\ frontier.objectKind[id] = kind
    /\ frontier.objectFence[id] \in FrontierFences
    /\ frontier.objectPrev[id] = id - 1

FrontierCandidateMatchesObject ==
    /\ FrontierObjectIsExact(frontier.candidateId, frontier.candidateKind)
    /\ frontier.objectFence[frontier.candidateId] = frontier.candidateFence

FrontierStartLog ==
    /\ frontier.phase = "Idle"
    /\ frontier.nextId \in Seqs
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Prepared",
            !.candidateId = frontier.nextId,
            !.candidateFence = frontier.currentFence,
            !.candidateKind = "log",
            !.issuedId = 0,
            !.issuedFence = 0,
            !.issuedKind = "none",
            !.issuedToken = 0,
            !.sampledThrough = 0,
            !.sampledToken = 0,
            !.lostResponsePending = FALSE,
            !.recoveryAdoptingSuccessor = FALSE]
    /\ UNCHANGED frontierWitness

FrontierLogDurable ==
    /\ frontier.phase = "Prepared"
    /\ frontier.candidateKind = "log"
    /\ frontier.objectKind[frontier.candidateId] = "none"
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "LogDurable",
            !.objectKind[frontier.candidateId] = "log",
            !.objectFence[frontier.candidateId] = frontier.candidateFence,
            !.objectPrev[frontier.candidateId] = frontier.candidateId - 1]
    /\ UNCHANGED frontierWitness

(* A new runtime fence can arrive before the old request resolves. It prevents NEW old-fence
   checkpoint requests; it does not cancel one the store already admitted. *)
FrontierMoveFence ==
    /\ frontier.currentFence = 1
    /\ frontier' = [frontier EXCEPT !.currentFence = 2]
    /\ UNCHANGED frontierWitness

FrontierIssueCheckpoint ==
    /\ frontier.phase = "LogDurable"
    /\ frontier.candidateId = frontier.committedThrough + 1
    /\ FrontierCandidateMatchesObject
    /\ frontier.currentFence = frontier.candidateFence
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "FrontierIssued",
            !.issuedId = frontier.candidateId,
            !.issuedFence = frontier.currentFence,
            !.issuedKind = frontier.candidateKind,
            !.issuedToken = frontier.checkpointToken,
            !.recoveryAdoptingSuccessor = FALSE]
    /\ UNCHANGED frontierWitness

(* After a crash, recovery first samples the exact checkpoint and its token. If the one exact
   successor belongs to the old writer, the NEW mount admits the checkpoint CAS under its CURRENT
   fence; the immutable object's producer fence is intentionally independent. *)
FrontierIssueRecoverySuccessor ==
    /\ frontier.phase = "CheckpointRead"
    /\ frontier.checkpointToken = frontier.sampledToken
    /\ frontier.candidateId = frontier.sampledThrough + 1
    /\ frontier.sampledThrough = frontier.committedThrough
    /\ FrontierCandidateMatchesObject
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "FrontierIssued",
            !.issuedId = frontier.candidateId,
            !.issuedFence = frontier.currentFence,
            !.issuedKind = frontier.candidateKind,
            !.issuedToken = frontier.sampledToken,
            !.recoveryAdoptingSuccessor = TRUE]
    /\ UNCHANGED frontierWitness

FrontierCheckpointLinearizes(response) ==
    /\ response \in {"delivered", "lost"}
    /\ frontier.phase = "FrontierIssued"
    /\ frontier.checkpointToken = frontier.issuedToken
    /\ frontier.issuedId = frontier.committedThrough + 1
    /\ FrontierObjectIsExact(frontier.issuedId, frontier.issuedKind)
    /\ frontier.issuedFence \in FrontierFences
    /\ frontier' =
        [frontier EXCEPT
            !.phase = IF response = "lost" THEN "NeedsRecovery" ELSE "FrontierDurable",
            !.committedThrough = frontier.issuedId,
            !.lastEpochSeal =
                IF frontier.issuedKind = "seal" THEN frontier.issuedId ELSE @,
            !.checkpointToken = @ + 1,
            !.lostResponsePending = (response = "lost"),
            !.validFrontierProofs = @ \cup {frontier.issuedId}]
    /\ frontierWitness' =
        [frontierWitness EXCEPT
            !.issuedCasLinearizedAfterFenceMove =
                @ \/ (frontier.currentFence # frontier.issuedFence)]

FrontierInstall ==
    /\ frontier.phase = "FrontierDurable"
    /\ frontier.issuedKind = "log"
    /\ frontier.candidateId <= frontier.committedThrough
    /\ FrontierCandidateMatchesObject
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Installed",
            !.installedThrough = frontier.candidateId]
    /\ frontierWitness' =
        [frontierWitness EXCEPT
            !.exactSuccessorAdopted =
                @ \/ (frontier.recoveryAdoptingSuccessor
                       /\ frontier.objectFence[frontier.candidateId] # frontier.issuedFence)]

FrontierAcknowledge ==
    /\ frontier.phase = "Installed"
    /\ frontier.installedThrough = frontier.candidateId
    /\ frontier.candidateId <= frontier.committedThrough
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Acknowledged",
            !.acknowledgedThrough = frontier.candidateId,
            !.nextId = frontier.candidateId + 1]
    /\ UNCHANGED frontierWitness

FrontierFinish ==
    /\ frontier.phase \in {"Acknowledged", "Rejected"}
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Idle",
            !.candidateId = 0,
            !.candidateFence = 0,
            !.candidateKind = "none",
            !.issuedId = 0,
            !.issuedFence = 0,
            !.issuedKind = "none",
            !.issuedToken = 0,
            !.sampledThrough = 0,
            !.sampledToken = 0,
            !.lostResponsePending = FALSE,
            !.recoveryAdoptingSuccessor = FALSE]
    /\ UNCHANGED frontierWitness

(* A crash loses only runtime state. Durable objects and the checkpoint remain. *)
FrontierCrash ==
    /\ frontier.phase \in {"Prepared", "LogDurable", "FrontierDurable", "Installed"}
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "NeedsRecovery",
            !.installedThrough = frontier.acknowledgedThrough]
    /\ frontierWitness' =
        [frontierWitness EXCEPT
            !.crashPrepared = @ \/ (frontier.phase = "Prepared"),
            !.crashLogDurable = @ \/ (frontier.phase = "LogDurable"),
            !.crashFrontierDurable = @ \/ (frontier.phase = "FrontierDurable"),
            !.crashInstalled = @ \/ (frontier.phase = "Installed")]

(* Every ambiguous/lost-response recovery begins with a real exact checkpoint read. The sampled
   token must still match before either install or a successor contribution is allowed. *)
FrontierReadCheckpoint ==
    /\ frontier.phase = "NeedsRecovery"
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "CheckpointRead",
            !.sampledThrough = frontier.committedThrough,
            !.sampledToken = frontier.checkpointToken]
    /\ UNCHANGED frontierWitness

FrontierRecoverAbsentAttempt ==
    /\ frontier.phase = "CheckpointRead"
    /\ frontier.checkpointToken = frontier.sampledToken
    /\ frontier.candidateId = frontier.sampledThrough + 1
    /\ frontier.objectKind[frontier.candidateId] = "none"
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Idle",
            !.candidateId = 0,
            !.candidateFence = 0,
            !.candidateKind = "none",
            !.issuedId = 0,
            !.issuedFence = 0,
            !.issuedKind = "none",
            !.issuedToken = 0,
            !.sampledThrough = 0,
            !.sampledToken = 0,
            !.lostResponsePending = FALSE,
            !.recoveryAdoptingSuccessor = FALSE]
    /\ UNCHANGED frontierWitness

FrontierValidateDurableCandidate ==
    /\ frontier.phase = "CheckpointRead"
    /\ frontier.checkpointToken = frontier.sampledToken
    /\ frontier.candidateId # 0
    /\ frontier.candidateId <= frontier.sampledThrough
    /\ FrontierCandidateMatchesObject
    /\ frontier' = [frontier EXCEPT !.phase = "RecoveryValidated"]
    /\ UNCHANGED frontierWitness

FrontierInstallValidated ==
    /\ frontier.phase = "RecoveryValidated"
    /\ frontier.checkpointToken = frontier.sampledToken
    /\ frontier.candidateId <= frontier.sampledThrough
    /\ FrontierCandidateMatchesObject
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Installed",
            !.installedThrough = frontier.candidateId,
            !.lostResponsePending = FALSE]
    /\ frontierWitness' =
        [frontierWitness EXCEPT
            !.lostFrontierResponse =
                @ \/ (frontier.lostResponsePending
                       /\ frontier.recoveryAdoptingSuccessor
                       /\ frontier.objectFence[frontier.candidateId] # frontier.issuedFence),
            !.exactSuccessorAdopted =
                @ \/ (frontier.recoveryAdoptingSuccessor
                       /\ frontier.objectFence[frontier.candidateId] # frontier.issuedFence)]

(* The successor's seal first wins the conditional-create slot. Its CURRENT-fence checkpoint CAS is
   a separate request; that request atomically publishes both `lastEpochSeal` and the frontier and
   may itself lose its response. *)
FrontierSuccessorSealCreate ==
    /\ frontier.phase = "Prepared"
    /\ frontier.currentFence = 2
    /\ frontier.candidateFence = 1
    /\ frontier.candidateId = frontier.committedThrough + 1
    /\ frontier.objectKind[frontier.candidateId] = "none"
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "SealDurable",
            !.objectKind[frontier.candidateId] = "seal",
            !.objectFence[frontier.candidateId] = frontier.currentFence,
            !.objectPrev[frontier.candidateId] = frontier.committedThrough]
    /\ UNCHANGED frontierWitness

FrontierIssueSuccessorSeal ==
    /\ frontier.phase = "SealDurable"
    /\ frontier.candidateId = frontier.committedThrough + 1
    /\ FrontierObjectIsExact(frontier.candidateId, "seal")
    /\ frontier.objectFence[frontier.candidateId] = frontier.currentFence
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "FrontierIssued",
            !.issuedId = frontier.candidateId,
            !.issuedFence = frontier.currentFence,
            !.issuedKind = "seal",
            !.issuedToken = frontier.checkpointToken,
            !.recoveryAdoptingSuccessor = FALSE]
    /\ UNCHANGED frontierWitness

FrontierFinishSealPublication ==
    /\ frontier.phase = "FrontierDurable"
    /\ frontier.issuedKind = "seal"
    /\ frontier.lastEpochSeal = frontier.issuedId
    /\ frontier.committedThrough = frontier.issuedId
    /\ frontier' = [frontier EXCEPT !.phase = "Rejected"]
    /\ frontierWitness' =
        [frontierWitness EXCEPT !.sealPublishedAtFrontier = TRUE]

FrontierResolveLostSeal ==
    /\ frontier.phase = "CheckpointRead"
    /\ frontier.lostResponsePending
    /\ frontier.issuedKind = "seal"
    /\ frontier.checkpointToken = frontier.sampledToken
    /\ frontier.sampledThrough = frontier.candidateId
    /\ frontier.lastEpochSeal = frontier.candidateId
    /\ FrontierObjectIsExact(frontier.candidateId, "seal")
    /\ frontier' =
        [frontier EXCEPT
            !.phase = "Rejected",
            !.lostResponsePending = FALSE]
    /\ frontierWitness' =
        [frontierWitness EXCEPT
            !.sealPublishedAtFrontier = TRUE,
            !.lostSealResponseResolved = TRUE]

FrontierOldWriterLosesToSeal ==
    /\ frontier.phase \in {"FrontierDurable", "NeedsRecovery", "CheckpointRead", "Rejected"}
    /\ frontier.candidateKind = "log"
    /\ frontier.objectKind[frontier.candidateId] = "seal"
    /\ frontier.objectFence[frontier.candidateId] = frontier.currentFence
    /\ frontier.candidateFence # frontier.currentFence
    /\ frontier.committedThrough = frontier.candidateId
    /\ frontier.lastEpochSeal = frontier.candidateId
    /\ UNCHANGED frontier
    /\ frontierWitness' =
        [frontierWitness EXCEPT !.oldWriterLostToSeal = TRUE]

FrontierPublishSnapshotBody ==
    /\ frontier.snapshotBodyThrough < frontier.committedThrough
    /\ frontier' =
        [frontier EXCEPT !.snapshotBodyThrough = frontier.committedThrough]
    /\ UNCHANGED frontierWitness

FrontierPublishSnapshotCheckpoint ==
    /\ frontier.checkpointSnapshot < frontier.snapshotBodyThrough
    /\ frontier.snapshotBodyThrough <= frontier.committedThrough
    /\ frontier' =
        [frontier EXCEPT
            !.checkpointSnapshot = frontier.snapshotBodyThrough,
            !.checkpointToken = @ + 1]
    /\ frontierWitness' =
        [frontierWitness EXCEPT !.snapshotPublishedAtFrontier = TRUE]

(* ---- Task 5b load-bearing sabotages ---- *)

FrontierAckBeforeCheckpoint ==
    /\ SabotageAckBeforeFrontier
    /\ frontier.phase = "LogDurable"
    /\ frontier' =
        [frontier EXCEPT !.acknowledgedThrough = frontier.candidateId]
    /\ UNCHANGED frontierWitness

FrontierAllocateBeforeCheckpoint ==
    /\ SabotageAllocateBeforeFrontier
    /\ frontier.phase = "LogDurable"
    /\ frontier' = [frontier EXCEPT !.nextId = frontier.candidateId + 1]
    /\ UNCHANGED frontierWitness

FrontierInstallUncommitted ==
    /\ SabotageInstallAboveFrontier
    /\ frontier.phase = "LogDurable"
    /\ frontier' =
        [frontier EXCEPT !.installedThrough = frontier.candidateId]
    /\ UNCHANGED frontierWitness

FrontierStaleAdvance ==
    /\ SabotageStaleFrontier
    /\ frontier.phase = "Prepared"
    /\ frontier.currentFence # frontier.candidateFence
    /\ frontier.candidateId = frontier.committedThrough + 1
    /\ frontier.objectKind[frontier.candidateId] = "none"
    /\ frontier' =
        [frontier EXCEPT !.committedThrough = frontier.candidateId]
    /\ UNCHANGED frontierWitness

FrontierSnapshotAboveCheckpoint ==
    /\ SabotageSnapshotAboveFrontier
    /\ frontier.committedThrough < MaxSeq
    /\ frontier' =
        [frontier EXCEPT !.checkpointSnapshot = frontier.committedThrough + 1]
    /\ UNCHANGED frontierWitness

FrontierSealAboveCheckpoint ==
    /\ SabotageSealAboveFrontier
    /\ frontier.committedThrough < MaxSeq
    /\ frontier' =
        [frontier EXCEPT !.lastEpochSeal = frontier.committedThrough + 1]
    /\ UNCHANGED frontierWitness

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

LegacyStep ==
    \/ WriterAppend \/ WAttemptAmbiguous \/ WResolveDurable
    \/ WResolveConclusiveReject \/ WResolveSealRejected \/ WOrphanLands
    \/ LatePredecessorPut
    \/ WriterPublishSnapshot \/ WriterCkptAdvance
    \/ GcCleanupLog \/ GcCleanupSnap
    \/ ReaderStart \/ RReadCkpt \/ RFetchBase
    \/ RWalkStep \/ RWalkVanish \/ RSealSlot \/ RAdoptStraggler
    \/ RScanStep \/ RScanInstall
    \/ ReaderReset

FrontierStep ==
    \/ FrontierStartLog \/ FrontierLogDurable \/ FrontierMoveFence
    \/ FrontierIssueCheckpoint
    \/ \E response \in {"delivered", "lost"} : FrontierCheckpointLinearizes(response)
    \/ FrontierInstall \/ FrontierAcknowledge \/ FrontierFinish \/ FrontierCrash
    \/ FrontierReadCheckpoint \/ FrontierRecoverAbsentAttempt
    \/ FrontierValidateDurableCandidate \/ FrontierInstallValidated
    \/ FrontierIssueRecoverySuccessor
    \/ FrontierSuccessorSealCreate \/ FrontierIssueSuccessorSeal
    \/ FrontierFinishSealPublication \/ FrontierResolveLostSeal
    \/ FrontierOldWriterLosesToSeal
    \/ FrontierPublishSnapshotBody \/ FrontierPublishSnapshotCheckpoint
    \/ FrontierAckBeforeCheckpoint \/ FrontierAllocateBeforeCheckpoint
    \/ FrontierInstallUncommitted \/ FrontierStaleAdvance
    \/ FrontierSnapshotAboveCheckpoint \/ FrontierSealAboveCheckpoint

LegacyNext ==
    /\ ~FrontierSlice
    /\ LegacyStep
    /\ UNCHANGED frontierVars

FrontierNext ==
    /\ FrontierSlice
    /\ FrontierStep
    /\ UNCHANGED legacyVars

Next == LegacyNext \/ FrontierNext \/ NoOp

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

(* Task 5b safety: durable logical history is exactly the contiguous, token-CAS-proved chain named
   by `_ckpt.committed_through`. Install, acknowledgement and successor allocation may trail that
   fact but can never lead it. *)
INV_EXACT_COMMITTED_FRONTIER ==
    ~FrontierSlice \/
    \A i \in 1..frontier.committedThrough :
        /\ frontier.objectKind[i] # "none"
        /\ frontier.objectFence[i] \in FrontierFences
        /\ frontier.objectPrev[i] = i - 1
        /\ i \in frontier.validFrontierProofs

INV_INSTALL_NOT_ABOVE_FRONTIER ==
    ~FrontierSlice \/ frontier.installedThrough <= frontier.committedThrough

INV_ACK_NOT_BEFORE_FRONTIER ==
    ~FrontierSlice \/ frontier.acknowledgedThrough <= frontier.committedThrough

INV_ACK_NOT_BEFORE_INSTALL ==
    ~FrontierSlice \/ frontier.acknowledgedThrough <= frontier.installedThrough

INV_NEXT_ID_NOT_BEFORE_FRONTIER ==
    ~FrontierSlice \/ frontier.nextId <= frontier.committedThrough + 1

INV_SNAPSHOT_NOT_ABOVE_FRONTIER ==
    ~FrontierSlice \/ frontier.checkpointSnapshot <= frontier.committedThrough

INV_SEAL_NOT_ABOVE_FRONTIER ==
    ~FrontierSlice \/ frontier.lastEpochSeal <= frontier.committedThrough

(* Reachability controls. Each is checked in its own witness config; violation means the honest
   crash/race window was reached while all safety invariants preceding it remained green. *)
W_CRASH_PREPARED == ~frontierWitness.crashPrepared
W_CRASH_LOG_DURABLE == ~frontierWitness.crashLogDurable
W_CRASH_FRONTIER_DURABLE == ~frontierWitness.crashFrontierDurable
W_CRASH_INSTALLED == ~frontierWitness.crashInstalled
W_LOST_FRONTIER_RESPONSE == ~frontierWitness.lostFrontierResponse
W_EXACT_SUCCESSOR_ADOPTED == ~frontierWitness.exactSuccessorAdopted
W_OLD_WRITER_LOST_TO_SEAL == ~frontierWitness.oldWriterLostToSeal
W_ISSUED_CAS_LINEARIZED_AFTER_FENCE_MOVE ==
    ~frontierWitness.issuedCasLinearizedAfterFenceMove
W_SNAPSHOT_PUBLISHED_AT_FRONTIER == ~frontierWitness.snapshotPublishedAtFrontier
W_SEAL_PUBLISHED_AT_FRONTIER == ~frontierWitness.sealPublishedAtFrontier
W_LOST_SEAL_RESPONSE_RESOLVED == ~frontierWitness.lostSealResponseResolved

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
    /\ frontier \in FrontierState
    /\ frontierWitness \in FrontierWitnessState

THEOREM Spec =>
    [](TypeOK /\ INV_RECOVERY /\ INV_NOFAIL /\ INV_DENSE /\ INV_NO_GHOST /\ INV_NO_PHANTOM
      /\ INV_EXACT_COMMITTED_FRONTIER /\ INV_INSTALL_NOT_ABOVE_FRONTIER
      /\ INV_ACK_NOT_BEFORE_FRONTIER /\ INV_ACK_NOT_BEFORE_INSTALL
      /\ INV_NEXT_ID_NOT_BEFORE_FRONTIER /\ INV_SNAPSHOT_NOT_ABOVE_FRONTIER
      /\ INV_SEAL_NOT_ABOVE_FRONTIER)
=============================================================================
