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
(*   mount   : None, or [uuid, epoch, deadline, fenced] -- a TTL lease.    *)
(*             `fenced` models the GC heartbeat-floor fence-out stamp      *)
(*             (gc_fenced = true): terminal for that (uuid, epoch).        *)
(*   mtoken  : 0..MaxToken         -- the slot's write-version (the S3     *)
(*             ETag/token); every mount write bumps it. Token-guarded      *)
(*             writes (the keeper's adopt CAS) compare against it.        *)
(*                                                                         *)
(* 2026-07-06 P3.1 extension (fence recovery, spec                         *)
(* 2026-07-06-cas-mount-lease-fence-recovery-design.md):                   *)
(*   - GcFence: any expired, unfenced mount may be stamped fenced by the   *)
(*     pool's GC (uuid/epoch preserved, token bumped). The fence is        *)
(*     TERMINAL for that (uuid, epoch): "a fence costs an epoch".          *)
(*   - The keeper's adopt is NON-ATOMIC (AdoptRead then AdoptWrite with an *)
(*     interleaving point) -- the real `MountLeaseKeeper::claim` is        *)
(*     GET-then-CAS, and the S13 wedge lives in that window.               *)
(*   - Late renewal: `Renew` may fire after the deadline passed (the       *)
(*     beat-blocked renewal); on a fenced slot it classifies as            *)
(*     fenced-by-gc and schedules a remount (new epoch), never a wedge.    *)
(*   - Recovery loop: fenced same-epoch is NOT adoptable/refreshable; the  *)
(*     actor re-allocates a fresh epoch and reclaims (remount).            *)
(*                                                                         *)
(* GATE (positive cfg, all Sab*=FALSE): owner is sticky, a mount is never  *)
(* held by a non-owner, epochs are strictly monotone + writer-unique, a    *)
(* superseded writer performs no new mutation, NO live mount body ever     *)
(* exists under a fenced (uuid, epoch) [FenceCostsEpoch], and no actor     *)
(* ever wedges permanently [NoPermanentWedge].                             *)
(*                                                                         *)
(* Sabotage CONSTANT booleans reproduce the guarded-against states:        *)
(*   SabForeignTakeover   -> ForeignUuidNeverAutoTakesOver violated        *)
(*   SabEpochReset        -> WriterEpochMonotoneUnique violated            *)
(*   SabSupersededWrites  -> SupersededWriterMakesNoMutation violated      *)
(*   SabAdoptWedgeOnTouch -> NoPermanentWedge violated (the OLD adopt:     *)
(*     a token mismatch at AdoptWrite fails closed PERMANENTLY -- the      *)
(*     pre-fix `LOGICAL_ERROR` abort during `Store::open`, exit 49)        *)
(*   SabAdoptIgnoresFence -> FenceCostsEpoch violated (the OLD adopt read  *)
(*     that skips the gc_fenced check: same-epoch resurrection of a        *)
(*     fenced incarnation)                                                 *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Actors,               \* server identities (distinct ServerUUIDs), e.g. {A, B}
    None,                 \* "no owner / no mount" sentinel (distinct from any Actor)
    MaxClock,             \* bound on abstract clock (TLC finiteness)
    MaxEpoch,             \* bound on the durable epoch counter
    MaxToken,             \* bound on the mount write-version counter (TLC finiteness)
    TTL,                  \* lease time-to-live added to clock at claim/renew
    SabForeignTakeover,   \* FALSE = honest; TRUE drops owner=a guard on an expired-mount claim
    SabEpochReset,        \* FALSE = honest; TRUE enables an action zeroing epoch when mount cleared
    SabSupersededWrites,  \* FALSE = honest; TRUE drops the epoch-match conjunct from Write
    SabAdoptWedgeOnTouch, \* FALSE = honest; TRUE makes an adopt token-mismatch a PERMANENT wedge
    SabAdoptIgnoresFence  \* FALSE = honest; TRUE lets AdoptRead accept a fenced same-epoch body

VARIABLES
    owner,        \* Actors \cup {None}; sticky once non-None
    epoch,        \* 0..MaxEpoch; durable monotone counter (its own object)
    mount,        \* None, or [uuid |-> Actor, epoch |-> Nat, deadline |-> Nat, fenced |-> BOOLEAN]
    mtoken,       \* 0..MaxToken; bumped by EVERY mount write (claim/renew/fence/adopt)
    clock,        \* 0..MaxClock; abstract time
    localEpoch,   \* [Actors -> 0..MaxEpoch]; the epoch an actor last allocated for itself
    localLost,    \* [Actors -> BOOLEAN]; TRUE once the actor learned it was superseded/fenced
    rejected,     \* [Actors -> BOOLEAN]; TRUE once a foreign actor was permanently rejected
    adoptObs,     \* [Actors -> (0..MaxToken) \cup {None}]; the token AdoptRead observed (in-flight adopt)
    fencedEpochs, \* SUBSET (Actors \X (0..MaxEpoch)); every (uuid, epoch) a GcFence ever stamped
    wedged,       \* [Actors -> BOOLEAN]; TRUE = permanent fail-closed abort (the S13 wedge state)
    wrote,        \* SUBSET (Actors \X (0..MaxEpoch)); mutations performed, tagged by live epoch
    rootEmpty,    \* BOOLEAN; FALSE once any Write happened
    firstOwner,   \* history: the first actor to ever own (sticky-owner witness)
    lostThenWrote,\* history: TRUE if a superseded actor ever entered a NEW write (must stay FALSE)
    reclaimed,    \* history: [Actors -> BOOLEAN]; TRUE once a reclaimed its OWN expired mount
    remountedAfterFence \* history: [Actors -> BOOLEAN]; TRUE once a re-mounted after being fenced

vars == << owner, epoch, mount, mtoken, clock, localEpoch, localLost, rejected,
           adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
           lostThenWrote, reclaimed, remountedAfterFence >>

Init ==
    /\ owner = None
    /\ epoch = 0
    /\ mount = None
    /\ mtoken = 0
    /\ clock = 0
    /\ localEpoch = [a \in Actors |-> 0]
    /\ localLost = [a \in Actors |-> FALSE]
    /\ rejected = [a \in Actors |-> FALSE]
    /\ adoptObs = [a \in Actors |-> None]
    /\ fencedEpochs = {}
    /\ wedged = [a \in Actors |-> FALSE]
    /\ wrote = {}
    /\ rootEmpty = TRUE
    /\ firstOwner = None
    /\ lostThenWrote = FALSE
    /\ reclaimed = [a \in Actors |-> FALSE]
    /\ remountedAfterFence = [a \in Actors |-> FALSE]

----------------------------------------------------------------------------
\* Claim an empty, unowned server_root: first-writer-wins, sticky from here on.
ClaimOwnerEmpty(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = None
    /\ rootEmpty
    /\ owner' = a
    /\ firstOwner' = IF firstOwner = None THEN a ELSE firstOwner
    /\ UNCHANGED << epoch, mount, mtoken, clock, localEpoch, localLost, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* A foreign uuid hits an already-owned root: fail closed, permanently rejected.
RejectForeignOwner(a) ==
    /\ owner # None
    /\ owner # a
    /\ ~rejected[a]
    /\ rejected' = [rejected EXCEPT ![a] = TRUE]
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, localLost,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* The owner bumps the durable epoch counter and records it as its live epoch.
AllocEpoch(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ localEpoch' = [localEpoch EXCEPT ![a] = epoch + 1]
    /\ UNCHANGED << owner, mount, mtoken, clock, localLost, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* Claim/reclaim the mount lease (the free-function `claimMount`, ONE atomic CAS in the
\* model: its real get+putOverwrite failure mode is a clean LiveDoubleStart retry, not
\* the wedge -- the wedge lives in the keeper's split adopt below).
\* Branch order mirrors the code:
\*   foreign uuid            -> not claimable here (fail closed; RejectForeignOwner covers identity)
\*   same uuid + SAME epoch  -> "refresh"; FIXED protocol refuses when fenced (a fence
\*                              costs an epoch -- no same-epoch resurrection)
\*   fenced (diff epoch)     -> reclaim IMMEDIATELY, expiry regardless (fence is terminal)
\*   expired                 -> reclaim
\*   mount = None            -> fresh mint
\* A same-uuid DIFFERENT-epoch LIVE unfenced mount is NOT claimable -> double-start blocked.
\* SabForeignTakeover drops the owner=a guard on the expired branch.
\* A successful (re)claim starts a FRESH incarnation: localLost resets, in-flight adopt
\* observation resets. History: reclaimed (own expired), remountedAfterFence (was fenced).
ClaimMount(a) ==
    LET expired    == (mount # None) /\ (mount.deadline <= clock)
        ownExpired == expired /\ (mount.uuid = a)
        ownerOK    == IF SabForeignTakeover /\ expired THEN TRUE ELSE owner = a
        sameEpoch  == (mount # None) /\ (mount.uuid = a) /\ (mount.epoch = localEpoch[a])
        refreshOK  == sameEpoch /\ (~mount.fenced \/ SabAdoptIgnoresFence)
        fencedReclaim == (mount # None) /\ (mount.uuid = a) /\ mount.fenced
                         /\ (mount.epoch # localEpoch[a])
        canClaim   == (mount = None) \/ refreshOK \/ fencedReclaim
                      \/ (expired /\ ~sameEpoch)
        wasFenced  == \E e \in 0..MaxEpoch : << a, e >> \in fencedEpochs
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ mtoken < MaxToken
    /\ ownerOK
    /\ canClaim
    /\ mount' = [uuid |-> a, epoch |-> localEpoch[a], deadline |-> clock + TTL, fenced |-> FALSE]
    /\ mtoken' = mtoken + 1
    /\ localLost' = [localLost EXCEPT ![a] = FALSE]
    /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
    /\ reclaimed' = IF ownExpired THEN [reclaimed EXCEPT ![a] = TRUE] ELSE reclaimed
    /\ remountedAfterFence' =
         IF wasFenced THEN [remountedAfterFence EXCEPT ![a] = TRUE] ELSE remountedAfterFence
    /\ UNCHANGED << owner, epoch, clock, localEpoch, rejected, fencedEpochs, wedged,
                    wrote, rootEmpty, firstOwner, lostThenWrote >>

\* The mount holder renews -- possibly LATE (the beat-blocked renewal: no expiry guard,
\* a renewal may fire after the deadline passed and after a GcFence landed).
\*   own live epoch, not fenced -> extend the deadline (token-guarded write; bumps mtoken)
\*   own live epoch, FENCED     -> the fence took the token; classify by BODY (fixed
\*                                 protocol): fenced-by-gc -> schedule remount (localLost;
\*                                 the actor re-allocates an epoch and reclaims). Never a wedge.
\*   newer epoch on the slot    -> learn we were superseded.
Renew(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ IF mount.epoch = localEpoch[a]
       THEN IF ~mount.fenced
            THEN /\ mtoken < MaxToken
                 /\ mount' = [mount EXCEPT !.deadline = clock + TTL]
                 /\ mtoken' = mtoken + 1
                 /\ UNCHANGED localLost
            ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
                 /\ UNCHANGED << mount, mtoken >>
       ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
            /\ UNCHANGED << mount, mtoken >>
    /\ UNCHANGED << owner, epoch, clock, localEpoch, rejected, adoptObs, fencedEpochs,
                    wedged, wrote, rootEmpty, firstOwner, lostThenWrote, reclaimed,
                    remountedAfterFence >>

\* The pool's GC fences an EXPIRED, unfenced mount (computeHeartbeatFloor's token-guarded
\* fence-out): gc_fenced = true, uuid/epoch/deadline preserved, token bumped. The fence is
\* an ENVIRONMENT action (any GC leader; identity irrelevant to this model).
GcFence ==
    /\ mount # None
    /\ ~mount.fenced
    /\ mount.deadline <= clock
    /\ mtoken < MaxToken
    /\ mount' = [mount EXCEPT !.fenced = TRUE]
    /\ mtoken' = mtoken + 1
    /\ fencedEpochs' = fencedEpochs \union { << mount.uuid, mount.epoch >> }
    /\ UNCHANGED << owner, epoch, clock, localEpoch, localLost, rejected, adoptObs,
                    wedged, wrote, rootEmpty, firstOwner, lostThenWrote, reclaimed,
                    remountedAfterFence >>

\* --- The keeper's NON-ATOMIC adopt (MountLeaseKeeper::claim: GET, decide, CAS) --------
\* AdoptRead: observe our own same-epoch slot and remember the token. The FIXED protocol
\* refuses a FENCED body at read (a fence costs an epoch): classify fenced-by-gc ->
\* schedule remount. SabAdoptIgnoresFence models the OLD read that skipped the check.
AdoptRead(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ adoptObs[a] = None
    /\ mount # None
    /\ mount.uuid = a
    /\ mount.epoch = localEpoch[a]
    /\ IF mount.fenced /\ ~SabAdoptIgnoresFence
       THEN /\ localLost' = [localLost EXCEPT ![a] = TRUE]   \* refuse: recover via new epoch
            /\ UNCHANGED adoptObs
       ELSE /\ adoptObs' = [adoptObs EXCEPT ![a] = mtoken]
            /\ UNCHANGED localLost
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, rejected,
                    fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* AdoptWrite: the CAS against the observed token. The GcFence may land between AdoptRead
\* and AdoptWrite -- THE S13 window.
\*   token unchanged -> adopt succeeds (fresh body, seq/expiry refreshed).
\*   token moved     -> FIXED protocol: re-read and classify by BODY -- our own uuid +
\*                      fenced = fenced-by-gc, recoverable (schedule remount, retry with a
\*                      new epoch). SabAdoptWedgeOnTouch models the OLD behavior: the
\*                      mismatch throws LOGICAL_ERROR out of Store::open -> PERMANENT wedge.
AdoptWrite(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ adoptObs[a] # None
    /\ IF mtoken = adoptObs[a]
       THEN /\ mtoken < MaxToken
            /\ mount' = [uuid |-> a, epoch |-> localEpoch[a],
                         deadline |-> clock + TTL, fenced |-> FALSE]
            /\ mtoken' = mtoken + 1
            /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
            /\ UNCHANGED << localLost, wedged >>
       ELSE /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
            /\ IF SabAdoptWedgeOnTouch
               THEN /\ wedged' = [wedged EXCEPT ![a] = TRUE]
                    /\ UNCHANGED localLost
               ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
                    /\ UNCHANGED wedged
            /\ UNCHANGED << mount, mtoken >>
    /\ UNCHANGED << owner, epoch, clock, localEpoch, rejected, fencedEpochs, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed, remountedAfterFence >>

\* Abstract time advances (so a deadline can pass -> fence + reclaim reachable).
Tick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED << owner, epoch, mount, mtoken, localEpoch, localLost, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* A server process crashes: later Ticks pass the deadline without the holder renewing.
\* An in-flight adopt observation dies with the process.
Die(a) ==
    /\ ~rejected[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ localLost' = [localLost EXCEPT ![a] = TRUE]
    /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, rejected,
                    fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* A mutation under the mount. Honest guard requires a LIVE, OWN, current-epoch, unfenced,
\* not-lost mount. SabSupersededWrites drops the epoch-match conjunct so a superseded
\* holder mutates.
Write(a) ==
    LET epochOK == IF SabSupersededWrites THEN TRUE ELSE mount.epoch = localEpoch[a]
        notLostOK == IF SabSupersededWrites THEN TRUE ELSE ~localLost[a]
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ mount.deadline > clock
    /\ ~mount.fenced
    /\ notLostOK
    /\ epochOK
    /\ wrote' = wrote \union {<< a, localEpoch[a] >>}
    /\ rootEmpty' = FALSE
    /\ lostThenWrote' = (lostThenWrote \/ localLost[a])
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, localLost, rejected,
                    adoptObs, fencedEpochs, wedged, firstOwner, reclaimed,
                    remountedAfterFence >>

\* SABOTAGE action (only enabled under SabEpochReset): zero the durable epoch
\* counter when the mount is cleared. Breaks epoch monotonicity.
SabResetEpoch ==
    /\ SabEpochReset
    /\ mount = None
    /\ epoch > 0
    /\ epoch' = 0
    /\ UNCHANGED << owner, mount, mtoken, clock, localEpoch, localLost, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

\* Clear an expired UNFENCED mount (lets SabResetEpoch fire; also a benign honest step).
\* A fenced slot is never cleared: in the implementation mount objects persist, and the
\* fenced body is exactly what lets a restart classify "my old incarnation was fenced".
ClearExpiredMount ==
    /\ mount # None
    /\ ~mount.fenced
    /\ mount.deadline <= clock
    /\ mount' = None
    /\ UNCHANGED << owner, epoch, mtoken, clock, localEpoch, localLost, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence >>

Next ==
    \/ Tick
    \/ ClearExpiredMount
    \/ SabResetEpoch
    \/ GcFence
    \/ \E a \in Actors : ClaimOwnerEmpty(a)
    \/ \E a \in Actors : RejectForeignOwner(a)
    \/ \E a \in Actors : AllocEpoch(a)
    \/ \E a \in Actors : ClaimMount(a)
    \/ \E a \in Actors : Renew(a)
    \/ \E a \in Actors : AdoptRead(a)
    \/ \E a \in Actors : AdoptWrite(a)
    \/ \E a \in Actors : Die(a)
    \/ \E a \in Actors : Write(a)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
TypeOK ==
    /\ owner \in Actors \union {None}
    /\ epoch \in 0..MaxEpoch
    /\ mtoken \in 0..MaxToken
    /\ clock \in 0..MaxClock
    /\ \/ mount = None
       \/ /\ mount.uuid \in Actors
          /\ mount.epoch \in 0..MaxEpoch
          /\ mount.deadline \in 0..(MaxClock + TTL)
          /\ mount.fenced \in BOOLEAN
    /\ localEpoch \in [Actors -> 0..MaxEpoch]
    /\ localLost \in [Actors -> BOOLEAN]
    /\ rejected \in [Actors -> BOOLEAN]
    /\ adoptObs \in [Actors -> (0..MaxToken) \union {None}]
    /\ fencedEpochs \subseteq (Actors \X (0..MaxEpoch))
    /\ wedged \in [Actors -> BOOLEAN]
    /\ wrote \subseteq (Actors \X (0..MaxEpoch))
    /\ rootEmpty \in BOOLEAN
    /\ firstOwner \in Actors \union {None}
    /\ lostThenWrote \in BOOLEAN
    /\ reclaimed \in [Actors -> BOOLEAN]
    /\ remountedAfterFence \in [Actors -> BOOLEAN]

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
WriterEpochMonotoneUnique ==
    /\ \A x \in wrote : \A y \in wrote : (x[2] = y[2]) => (x[1] = y[1])
    /\ \A x \in wrote : x[2] <= epoch

\* A superseded (lost) writer never enters a NEW mutation.
SupersededWriterMakesNoMutation ==
    ~lostThenWrote

\* P3.1: a fence costs an epoch -- no LIVE (unfenced) mount body ever exists under a
\* (uuid, epoch) that a GcFence stamped. Same-epoch resurrection violates this.
FenceCostsEpoch ==
    (mount # None /\ ~mount.fenced) => (<< mount.uuid, mount.epoch >> \notin fencedEpochs)

\* P3.1: no actor ever reaches the permanent fail-closed wedge state (the pre-fix
\* Store::open abort). Under the FIXED protocol `wedged` is unreachable; the
\* SabAdoptWedgeOnTouch cfg must violate this (reproducing the S13 wedge).
NoPermanentWedge ==
    \A a \in Actors : ~wedged[a]

----------------------------------------------------------------------------
\* LIVENESS WITNESSES (asserted as invariants in dedicated cfgs so TLC reports them
\* VIOLATED when the good state is REACHABLE).

\* Some actor reclaims a mount it had to expiry.
W_SameUuidReclaimsExpired ==
    ~(\E a \in Actors : reclaimed[a])

\* Some actor whose (uuid, epoch) was FENCED later holds a live mount again (with a new
\* epoch, by FenceCostsEpoch) -- the no-permanent-wedge recovery loop actually completes.
W_RemountAfterFence ==
    ~(\E a \in Actors : remountedAfterFence[a])
=============================================================================
