-------------------------- MODULE CaCasMountCore --------------------------
(***************************************************************************)
(* TLA+ safety gate for the content-addressed "mount ownership + server-   *)
(* root identity" subsystem.                                               *)
(*                                                                         *)
(* One modeled server_root_id, three objects + two server Actors (A,B,     *)
(* each a distinct fixed ServerUUID):                                      *)
(*                                                                         *)
(*   owner   : Actors \cup {None}  -- sticky identity; once set never      *)
(*             changes (the server_root claim is first-writer-wins).       *)
(*   epoch   : 0..MaxEpoch         -- durable monotone counter living in   *)
(*             its OWN object (NOT the mount).                             *)
(*   mount   : None, or [uuid, epoch, deadline] -- a TTL lease.            *)
(*                                                                         *)
(* A mount lease is a CAS register; a claim/steal/reclaim is ONE atomic    *)
(* action. Abstract time is the global `clock`; `Tick` advances it so a    *)
(* deadline can pass and a reclaim becomes reachable.                      *)
(*                                                                         *)
(* GATE (positive cfg, all Sab*=FALSE): owner is sticky, a mount is never  *)
(* held by a non-owner, epochs are strictly monotone + writer-unique, and  *)
(* a superseded writer performs no new mutation.                           *)
(*                                                                         *)
(* Sabotage CONSTANT booleans (each woven in as in CaGcLeaseCore's         *)
(* EnableHeartbeat) reproduce the bad state the matching invariant guards: *)
(*   SabForeignTakeover  -> ForeignUuidNeverAutoTakesOver violated         *)
(*   SabEpochReset       -> WriterEpochMonotoneUnique violated             *)
(*   SabSupersededWrites -> SupersededWriterMakesNoMutation violated       *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Actors,               \* server identities (distinct ServerUUIDs), e.g. {A, B}
    None,                 \* "no owner / no mount" sentinel (distinct from any Actor)
    MaxClock,             \* bound on abstract clock (TLC finiteness)
    MaxEpoch,             \* bound on the durable epoch counter
    TTL,                  \* lease time-to-live added to clock at claim/renew
    SabForeignTakeover,   \* FALSE = honest; TRUE drops owner=a guard on an expired-mount claim
    SabEpochReset,        \* FALSE = honest; TRUE enables an action zeroing epoch when mount cleared
    SabSupersededWrites   \* FALSE = honest; TRUE drops the epoch-match conjunct from Write

VARIABLES
    owner,        \* Actors \cup {None}; sticky once non-None
    epoch,        \* 0..MaxEpoch; durable monotone counter (its own object)
    mount,        \* None, or [uuid |-> Actor, epoch |-> Nat, deadline |-> Nat]
    clock,        \* 0..MaxClock; abstract time
    localEpoch,   \* [Actors -> 0..MaxEpoch]; the epoch an actor last allocated for itself
    localLost,    \* [Actors -> BOOLEAN]; TRUE once the actor learned it was superseded
    rejected,     \* [Actors -> BOOLEAN]; TRUE once a foreign actor was permanently rejected
    wrote,        \* SUBSET (Actors \X (0..MaxEpoch)); mutations performed, tagged by live epoch
    rootEmpty,    \* BOOLEAN; FALSE once any Write happened
    firstOwner,   \* history: the first actor to ever own (sticky-owner witness)
    lostThenWrote,\* history: TRUE if a superseded actor ever entered a NEW write (must stay FALSE)
    reclaimed     \* history: [Actors -> BOOLEAN]; TRUE once a reclaimed its OWN expired mount

vars == << owner, epoch, mount, clock, localEpoch, localLost, rejected, wrote,
           rootEmpty, firstOwner, lostThenWrote, reclaimed >>

Init ==
    /\ owner = None
    /\ epoch = 0
    /\ mount = None
    /\ clock = 0
    /\ localEpoch = [a \in Actors |-> 0]
    /\ localLost = [a \in Actors |-> FALSE]
    /\ rejected = [a \in Actors |-> FALSE]
    /\ wrote = {}
    /\ rootEmpty = TRUE
    /\ firstOwner = None
    /\ lostThenWrote = FALSE
    /\ reclaimed = [a \in Actors |-> FALSE]

----------------------------------------------------------------------------
\* Claim an empty, unowned server_root: first-writer-wins, sticky from here on.
ClaimOwnerEmpty(a) ==
    /\ ~rejected[a]
    /\ owner = None
    /\ rootEmpty
    /\ owner' = a
    /\ firstOwner' = IF firstOwner = None THEN a ELSE firstOwner
    /\ UNCHANGED << epoch, mount, clock, localEpoch, localLost, rejected, wrote,
                    rootEmpty, lostThenWrote, reclaimed >>

\* A foreign uuid hits an already-owned root: fail closed, permanently rejected.
RejectForeignOwner(a) ==
    /\ owner # None
    /\ owner # a
    /\ ~rejected[a]
    /\ rejected' = [rejected EXCEPT ![a] = TRUE]
    /\ UNCHANGED << owner, epoch, mount, clock, localEpoch, localLost, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

\* The owner bumps the durable epoch counter and records it as its live epoch.
AllocEpoch(a) ==
    /\ ~rejected[a]
    /\ owner = a
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ localEpoch' = [localEpoch EXCEPT ![a] = epoch + 1]
    /\ UNCHANGED << owner, mount, clock, localLost, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

\* Claim/adopt/reclaim the mount lease in ONE atomic CAS.
\*   Honest guard: owner=a AND
\*     mount=None                                  (fresh claim), OR
\*     mount.deadline <= clock                     (expired -> reclaim), OR
\*     mount.uuid=a /\ mount.epoch=localEpoch[a]    (adopt our own LIVE mount).
\* A same-uuid DIFFERENT-epoch LIVE mount is NOT claimable -> double-start blocked.
\* SabForeignTakeover drops the owner=a guard on the expired branch -> a non-owner
\* "auto-takes-over" an expired mount.
ClaimMount(a) ==
    LET expired   == (mount # None) /\ (mount.deadline <= clock)
        ownExpired== expired /\ (mount.uuid = a)
        ownerOK   == IF SabForeignTakeover /\ expired THEN TRUE ELSE owner = a
        adoptLive == (mount # None) /\ (mount.uuid = a) /\ (mount.epoch = localEpoch[a])
        canClaim  == (mount = None) \/ expired \/ adoptLive
    IN
    /\ ~rejected[a]
    /\ ownerOK
    /\ canClaim
    /\ mount' = [uuid |-> a, epoch |-> localEpoch[a], deadline |-> clock + TTL]
    /\ reclaimed' = IF ownExpired THEN [reclaimed EXCEPT ![a] = TRUE] ELSE reclaimed
    /\ UNCHANGED << owner, epoch, clock, localEpoch, localLost, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote >>

\* The mount holder renews. If still at its own live epoch, extend the deadline;
\* otherwise a newer epoch took the mount -> learn we were superseded.
Renew(a) ==
    /\ ~rejected[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ IF mount.epoch = localEpoch[a]
       THEN /\ mount' = [mount EXCEPT !.deadline = clock + TTL]
            /\ UNCHANGED localLost
       ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
            /\ UNCHANGED mount
    /\ UNCHANGED << owner, epoch, clock, localEpoch, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

\* Abstract time advances (so a deadline can pass -> reclaim reachable).
Tick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED << owner, epoch, mount, localEpoch, localLost, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

\* A server process crashes: no special state, just lets later Ticks pass the
\* deadline without the holder renewing -> a reclaim is reachable.
Die(a) ==
    /\ ~rejected[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ localLost' = [localLost EXCEPT ![a] = TRUE]
    /\ UNCHANGED << owner, epoch, mount, clock, localEpoch, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

\* A mutation under the mount. Honest guard requires a LIVE, OWN, current-epoch,
\* not-lost mount. SabSupersededWrites drops the epoch-match conjunct so a
\* superseded holder mutates.
Write(a) ==
    LET epochOK == IF SabSupersededWrites THEN TRUE ELSE mount.epoch = localEpoch[a]
        notLostOK == IF SabSupersededWrites THEN TRUE ELSE ~localLost[a]
    IN
    /\ ~rejected[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ mount.deadline > clock
    /\ notLostOK
    /\ epochOK
    /\ wrote' = wrote \union {<< a, localEpoch[a] >>}
    /\ rootEmpty' = FALSE
    /\ lostThenWrote' = (lostThenWrote \/ localLost[a])
    /\ UNCHANGED << owner, epoch, mount, clock, localEpoch, localLost, rejected,
                    firstOwner, reclaimed >>

\* SABOTAGE action (only enabled under SabEpochReset): zero the durable epoch
\* counter when the mount is cleared. Breaks epoch monotonicity.
SabResetEpoch ==
    /\ SabEpochReset
    /\ mount = None
    /\ epoch > 0
    /\ epoch' = 0
    /\ UNCHANGED << owner, mount, clock, localEpoch, localLost, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

\* Clear an expired mount (lets SabResetEpoch fire; also a benign honest step).
ClearExpiredMount ==
    /\ mount # None
    /\ mount.deadline <= clock
    /\ mount' = None
    /\ UNCHANGED << owner, epoch, clock, localEpoch, localLost, rejected, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed >>

Next ==
    \/ Tick
    \/ ClearExpiredMount
    \/ SabResetEpoch
    \/ \E a \in Actors : ClaimOwnerEmpty(a)
    \/ \E a \in Actors : RejectForeignOwner(a)
    \/ \E a \in Actors : AllocEpoch(a)
    \/ \E a \in Actors : ClaimMount(a)
    \/ \E a \in Actors : Renew(a)
    \/ \E a \in Actors : Die(a)
    \/ \E a \in Actors : Write(a)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
TypeOK ==
    /\ owner \in Actors \union {None}
    /\ epoch \in 0..MaxEpoch
    /\ clock \in 0..MaxClock
    /\ \/ mount = None
       \/ /\ mount.uuid \in Actors
          /\ mount.epoch \in 0..MaxEpoch
          /\ mount.deadline \in 0..(MaxClock + TTL)
    /\ localEpoch \in [Actors -> 0..MaxEpoch]
    /\ localLost \in [Actors -> BOOLEAN]
    /\ rejected \in [Actors -> BOOLEAN]
    /\ wrote \subseteq (Actors \X (0..MaxEpoch))
    /\ rootEmpty \in BOOLEAN
    /\ firstOwner \in Actors \union {None}
    /\ lostThenWrote \in BOOLEAN
    /\ reclaimed \in [Actors -> BOOLEAN]

\* Owner is sticky: once set it never transitions to a different non-None value.
NoTwoServerUuidsOwnSameServerRoot ==
    (owner # None) => (owner = firstOwner)

\* A mount is never held by a non-owner, for ANY clock/deadline.
ForeignUuidNeverAutoTakesOver ==
    (mount # None) => (mount.uuid = owner)

\* Epochs are writer-unique AND monotone-bounded:
\*  (1) no two distinct actors share a written epoch;
\*  (2) the durable `epoch` counter is a monotone ceiling over every written
\*      epoch -- it only ever grows, so a written epoch can never exceed it.
\* Zeroing `epoch` (SabEpochReset) after a write drops the ceiling below an
\* already-written epoch and violates (2): a reused/reset epoch is non-monotone.
WriterEpochMonotoneUnique ==
    /\ \A x \in wrote : \A y \in wrote : (x[2] = y[2]) => (x[1] = y[1])
    /\ \A x \in wrote : x[2] <= epoch

\* A superseded (lost) writer never enters a NEW mutation.
SupersededWriterMakesNoMutation ==
    ~lostThenWrote

----------------------------------------------------------------------------
\* LIVENESS WITNESS (asserted as an invariant so TLC reports it VIOLATED when
\* the good state is REACHABLE): some actor reclaims a mount it had to expiry.
W_SameUuidReclaimsExpired ==
    ~(\E a \in Actors : reclaimed[a])
=============================================================================
