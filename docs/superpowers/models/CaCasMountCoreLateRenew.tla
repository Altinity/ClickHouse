----------------------- MODULE CaCasMountCoreLateRenew -----------------------
(***************************************************************************)
(* History wrapper over `CaCasMountCore` for one witness: a holder renews  *)
(* after its lease expired and goes on under the same generation.          *)
(*                                                                         *)
(* A state predicate over `CaCasMountCore`'s own variables cannot tell a   *)
(* late `Renew` from a same-epoch `ClaimMount`, nor `WedgeRetryCreate`     *)
(* from `StragglerLands` followed by `WedgeRetryOccupied`. The wrapper     *)
(* records the four steps as they happen. Every step of `NextW` is a step  *)
(* of `Next`, and `Next` alone is always allowed, so the wrapped model     *)
(* reaches exactly the states of `CaCasMountCore`.                         *)
(***************************************************************************)
EXTENDS CaCasMountCore

VARIABLES
    lateStage,  \* 0..4: how much of the sequence has happened
    lateActor,  \* the holder, None before the first step
    lateGen     \* the generation its lane was admitted under, None before the first step

wvars == << vars, lateStage, lateActor, lateGen >>

InitW ==
    /\ Init
    /\ lateStage = 0 /\ lateActor = None /\ lateGen = None

\* 1. A lane is admitted while the lease is valid.
LaneAdmittedInsideLease(a) ==
    /\ lateStage = 0
    /\ clock < fenceUntil
    /\ WedgeAdmit(a)
    /\ lateStage' = 1 /\ lateActor' = a /\ lateGen' = localEpoch[a]

SameGeneration(a) ==
    a = lateActor /\ localEpoch[a] = lateGen

\* 2. The lease expires and `Renew` extends it under the same epoch. `GcFence` is enabled at the
\*    same instant in this model and is not taken.
RenewAfterExpiry(a) ==
    /\ lateStage = 1
    /\ SameGeneration(a)
    /\ clock >= fenceUntil
    /\ mount # None /\ mount.uuid = a /\ mount.epoch = lateGen /\ ~mount.fenced
    /\ Renew(a)
    /\ lateStage' = 2 /\ UNCHANGED << lateActor, lateGen >>

\* 3. The lane admitted before the expiry runs its conditional create.
LaneRetriesAfterRenew(a) ==
    /\ lateStage = 2
    /\ SameGeneration(a)
    /\ wedgeGen[a] = lateGen
    /\ WedgeRetryCreate(a)
    /\ lateStage' = 3 /\ UNCHANGED << lateActor, lateGen >>

\* 4. An ordinary write follows.
WriteAfterRenew(a) ==
    /\ lateStage = 3
    /\ SameGeneration(a)
    /\ Write(a)
    /\ lateStage' = 4 /\ UNCHANGED << lateActor, lateGen >>

NextW ==
    \/ \E a \in Actors : LaneAdmittedInsideLease(a)
    \/ \E a \in Actors : RenewAfterExpiry(a)
    \/ \E a \in Actors : LaneRetriesAfterRenew(a)
    \/ \E a \in Actors : WriteAfterRenew(a)
    \/ Next /\ UNCHANGED << lateStage, lateActor, lateGen >>

SpecW == InitW /\ [][NextW]_wvars

TypeOKW ==
    /\ lateStage \in 0..4
    /\ lateActor \in Actors \union {None}
    /\ lateGen \in (0..MaxEpoch) \union {None}

\* Witness, negated reachability: the whole sequence happens.
W_LateRenewThenLaneAndWrite ==
    lateStage # 4
=============================================================================
