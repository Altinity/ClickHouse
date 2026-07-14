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
(*                                                                          *)
(* 2026-07-14 rev.6 extension (observation-based reclaim, spec              *)
(* 2026-07-13-cas-ref-lease-exclusivity-rev6-design.md, S:unclean-takeover, *)
(* S:gc-fence-out): a reclaimer never trusts a foreign wall clock. The      *)
(* holder's stamp (`mount.deadline`) and its TRUE local-fence expiry        *)
(* (`fenceUntil`) can differ by up to `Drift` ticks (bounded clock-RATE     *)
(* skew, not a clock-sync error). `SabWallClockReclaim` reproduces the OLD  *)
(* bug: a reclaimer that trusts the stamp directly (`clock > mount.deadline`)*)
(* instead of waiting out the full `TTL + Drift` on ITS OWN clock after     *)
(* observing the token hold stable (`ObservedReclaim`).                     *)
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
    SabAdoptIgnoresFence, \* FALSE = honest; TRUE lets AdoptRead accept a fenced same-epoch body
    Drift,                \* max extra ticks the holder's TRUE local-fence expiry outlives the stamp
    SabWallClockReclaim   \* FALSE = honest observation-based reclaim; TRUE = trust the stamp (rev.6 bug)

VARIABLES
    owner,        \* Actors \cup {None}; sticky once non-None
    epoch,        \* 0..MaxEpoch; durable monotone counter (its own object)
    mount,        \* None, or [uuid |-> Actor, epoch |-> Nat, deadline |-> Nat, fenced |-> BOOLEAN]
    mtoken,       \* 0..MaxToken; bumped by EVERY mount write (claim/renew/fence/adopt)
    clock,        \* 0..MaxClock; abstract time
    localEpoch,   \* [Actors -> 0..MaxEpoch]; the epoch an actor last allocated for itself
    localLost,    \* [Actors -> BOOLEAN]; TRUE once the actor learned it was superseded/fenced --
                  \* PURE KNOWLEDGE (round 3): appears in NO safety guard, only in the dedicated
                  \* knowledge-based witness (`lostThenWrote`/`SupersededWriterMakesNoMutation`)
    crashed,      \* rev.6 round 3: [Actors -> BOOLEAN]; TRUE = a MECHANICAL fact -- this actor's
                  \* process is genuinely dead (set by `Die`). A crashed process not writing is
                  \* PHYSICS, not politeness, so `Write` gates on `~crashed[a]` directly -- disjoint
                  \* from `localLost` (knowledge), which a dead process cannot even possess.
    rejected,     \* [Actors -> BOOLEAN]; TRUE once a foreign actor was permanently rejected
    adoptObs,     \* [Actors -> (0..MaxToken) \cup {None}]; the token AdoptRead observed (in-flight adopt)
    fencedEpochs, \* SUBSET (Actors \X (0..MaxEpoch)); every (uuid, epoch) a GcFence ever stamped
    wedged,       \* [Actors -> BOOLEAN]; TRUE = permanent fail-closed abort (the S13 wedge state)
    wrote,        \* SUBSET (Actors \X (0..MaxEpoch)); mutations performed, tagged by live epoch
    rootEmpty,    \* BOOLEAN; FALSE once any Write happened
    firstOwner,   \* history: the first actor to ever own (sticky-owner witness)
    lostThenWrote,\* history: TRUE if a superseded actor ever entered a NEW write (must stay FALSE)
    reclaimed,    \* history: [Actors -> BOOLEAN]; TRUE once a reclaimed its OWN expired mount
    remountedAfterFence, \* history: [Actors -> BOOLEAN]; TRUE once a re-mounted after being fenced
    fenceUntil,   \* rev.6: holder's TRUE local-fence expiry (stamp + nondet skew <= Drift)
    obsToken,     \* rev.6: reclaimer's observation -- mtoken value first seen (None = unarmed)
    obsSince,     \* rev.6: clock tick at which obsToken was first observed
    observedReclaimEver, \* rev.6 history: TRUE once ObservedReclaim has ever completed (witness)
    supersededThenWrote \* rev.6 history: TRUE if Write ever fired while `epoch` (GLOBAL truth,
                        \* the durable counter) had already advanced past the writer's OWN
                        \* `localEpoch[a]` -- i.e. a completed reclaim the writer did not know about

vars == << owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
           adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
           lostThenWrote, reclaimed, remountedAfterFence,
           fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

Init ==
    /\ owner = None
    /\ epoch = 0
    /\ mount = None
    /\ mtoken = 0
    /\ clock = 0
    /\ localEpoch = [a \in Actors |-> 0]
    /\ localLost = [a \in Actors |-> FALSE]
    /\ crashed = [a \in Actors |-> FALSE]
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
    /\ fenceUntil = 0
    /\ obsToken = None
    /\ obsSince = 0
    /\ observedReclaimEver = FALSE
    /\ supersededThenWrote = FALSE

----------------------------------------------------------------------------
\* Claim an empty, unowned server_root: first-writer-wins, sticky from here on.
ClaimOwnerEmpty(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = None
    /\ rootEmpty
    /\ owner' = a
    /\ firstOwner' = IF firstOwner = None THEN a ELSE firstOwner
    /\ UNCHANGED << epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* A foreign uuid hits an already-owned root: fail closed, permanently rejected.
RejectForeignOwner(a) ==
    /\ owner # None
    /\ owner # a
    /\ ~rejected[a]
    /\ rejected' = [rejected EXCEPT ![a] = TRUE]
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* The owner bumps the durable epoch counter and records it as its live epoch.
\* rev.6 round 5: guarded against firing while `a`'s own mount record is still UNFENCED --
\* the product's `allocateWriterEpoch` is invoked exactly once, strictly before any mount
\* exists for this open() attempt (`CasStore.cpp:312-316`'s own "STRICT ORDER: ... claim
\* owner (identity) -> allocate durable writer_epoch -> claim the mount lease (liveness) ..."
\* comment), or after the fence-recovery loop has ALREADY reset the previous (now-abandoned)
\* keeper (`CasStore.cpp:454-455`, `mount_keeper.reset()` before reallocating; the
\* `FencedSelf` retry at `:413` likewise only fires when our own fresh attempt was ALREADY
\* fenced). NOTE: this is NOT gated on wall-clock expiry (`mount.deadline`/`clock`) -- `Renew`
\* is explicitly allowed to fire on a wall-clock-expired-but-unfenced mount ("the beat-blocked
\* renewal", see its own comment), so mere expiry does not mean the actor has abandoned this
\* epoch; only a GENUINE FENCE does ("a fence costs an epoch", the model's own P3.1
\* philosophy). A guard keyed on `mount.deadline > clock` still let `AllocEpoch` fire right
\* at/after the wall-clock deadline while `mount.fenced` was still FALSE, reproducing the
\* SAME false alarm this round already fixed once (verified via TLC after the first,
\* deadline-keyed attempt at this guard came back RED with an identical trace).
AllocEpoch(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch < MaxEpoch
    /\ ~(mount # None /\ mount.uuid = a /\ ~mount.fenced)
    /\ epoch' = epoch + 1
    /\ localEpoch' = [localEpoch EXCEPT ![a] = epoch + 1]
    /\ UNCHANGED << owner, mount, mtoken, clock, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

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
\* rev.6 round 3: `crashed` ALSO resets here -- this model's only rebirth path for a
\* crashed actor IS a successful (re)claim (a process restart), so a fresh incarnation
\* must clear the mechanical "dead" fact along with the knowledge flag, or `witness_reclaim`
\* / `witness_remountafterfence` (which require the SAME uuid to write again after
\* recovering) would become permanently unable to `Write` post-crash.
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
    /\ \E d \in 0..Drift : fenceUntil' = clock + TTL + d  \* rev.6: TRUE local fence, stamp + skew
    /\ localLost' = [localLost EXCEPT ![a] = FALSE]
    /\ crashed' = [crashed EXCEPT ![a] = FALSE]
    /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
    /\ reclaimed' = IF ownExpired THEN [reclaimed EXCEPT ![a] = TRUE] ELSE reclaimed
    /\ remountedAfterFence' =
         IF wasFenced THEN [remountedAfterFence EXCEPT ![a] = TRUE] ELSE remountedAfterFence
    /\ UNCHANGED << owner, epoch, clock, localEpoch, rejected, fencedEpochs, wedged,
                    wrote, rootEmpty, firstOwner, lostThenWrote,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

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
                 /\ \E d \in 0..Drift : fenceUntil' = clock + TTL + d  \* rev.6
                 /\ UNCHANGED localLost
            ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
                 /\ UNCHANGED << mount, mtoken, fenceUntil >>
       ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
            /\ UNCHANGED << mount, mtoken, fenceUntil >>
    /\ UNCHANGED << owner, epoch, clock, localEpoch, crashed, rejected, adoptObs, fencedEpochs,
                    wedged, wrote, rootEmpty, firstOwner, lostThenWrote, reclaimed,
                    remountedAfterFence, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

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
    /\ UNCHANGED << owner, epoch, clock, localEpoch, localLost, crashed, rejected, adoptObs,
                    wedged, wrote, rootEmpty, firstOwner, lostThenWrote, reclaimed,
                    remountedAfterFence, fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

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
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, crashed, rejected,
                    fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* AdoptWrite: the CAS against the observed token. The GcFence may land between AdoptRead
\* and AdoptWrite -- THE S13 window.
\*   token unchanged -> adopt succeeds (fresh body, seq/expiry refreshed).
\*   token moved     -> FIXED protocol: re-read and classify by BODY -- our own uuid +
\*                      fenced = fenced-by-gc, recoverable (schedule remount, retry with a
\*                      new epoch). SabAdoptWedgeOnTouch models the OLD behavior: the
\*                      mismatch throws LOGICAL_ERROR out of Store::open -> PERMANENT wedge.
\* rev.6 round 4: the "classify by BODY" this comment has always claimed is now actually
\* implemented -- re-reading `mount` (already directly visible; no extra model step needed)
\* and checking uuid/epoch/fenced BEFORE concluding loss. If the body still shows
\* `mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced`, the token moved for a
\* provably benign, self-caused reason -- no loss, no wedge (matches the product's real
\* `MountLeaseKeeper::claim`, `CasServerRoot.cpp:711-737`, which performs exactly this
\* re-read-and-classify; see the task report's round-4 section for the product-vs-model
\* reachability caveat -- this exact interleaving is NOT reachable in the current product
\* because `state_mutex` serializes `claim()` against `renewOnce()` and the renewal thread
\* does not exist until AFTER `claim()` returns, `CasStore.cpp:444-473` -- this action still
\* encodes the target DESIGN semantics the comment above has always documented).
AdoptWrite(a) ==
    \* `mount # None` guards the record access below -- `ClearExpiredMount` can clear `mount`
    \* without touching `adoptObs` (a reachable state even pre-round-4), so a re-read CAN
    \* legitimately find nothing there; that is the product's "vanished while adopting"
    \* branch (`CasServerRoot.cpp:735-736`), which also fails closed (not self-caused).
    LET selfCaused == mount # None /\ mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ adoptObs[a] # None
    /\ IF mtoken = adoptObs[a]
       THEN /\ mtoken < MaxToken
            /\ mount' = [uuid |-> a, epoch |-> localEpoch[a],
                         deadline |-> clock + TTL, fenced |-> FALSE]
            /\ mtoken' = mtoken + 1
            /\ \E d \in 0..Drift : fenceUntil' = clock + TTL + d  \* rev.6
            /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
            /\ UNCHANGED << localLost, wedged >>
       ELSE /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
            /\ IF SabAdoptWedgeOnTouch
               THEN /\ wedged' = [wedged EXCEPT ![a] = TRUE]
                    /\ UNCHANGED localLost
               ELSE IF selfCaused
                    THEN UNCHANGED << localLost, wedged >>  \* rev.6: benign, self-caused -- no loss
                    ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
                         /\ UNCHANGED wedged
            /\ UNCHANGED << mount, mtoken, fenceUntil >>
    /\ UNCHANGED << owner, epoch, clock, localEpoch, crashed, rejected, fencedEpochs, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed, remountedAfterFence,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* Abstract time advances (so a deadline can pass -> fence + reclaim reachable).
Tick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED << owner, epoch, mount, mtoken, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* A server process crashes: later Ticks pass the deadline without the holder renewing.
\* An in-flight adopt observation dies with the process. rev.6 round 3: this sets
\* `crashed[a]`, a MECHANICAL fact -- NOT `localLost` (a dead process cannot "learn"
\* anything; knowledge and physical death are disjoint concepts kept in disjoint vars).
Die(a) ==
    /\ ~rejected[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ crashed' = [crashed EXCEPT ![a] = TRUE]
    /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, localLost, rejected,
                    fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* A mutation under the mount. Honest guard requires a LIVE, OWN, current-epoch, unfenced
\* mount. SabSupersededWrites drops the epoch-match conjunct so a superseded holder
\* mutates. rev.6 (2026-07-14): every liveness/authority conjunct here is a MECHANICAL
\* truth, never a knowledge/politeness one --
\*   epochOK          : the live epoch-conditional-write check (mount.epoch = localEpoch[a])
\*   clock < fenceUntil: the drift-aware TRUE local-fence-deadline check (never the stamp
\*                       `mount.deadline` -- see model header)
\*   ~crashed[a]       : the process is actually alive (a MECHANICAL fact set by `Die`)
\* `~localLost[a]` is DELIBERATELY ABSENT: `localLost` is PURE KNOWLEDGE (what `a` has
\* learned) and appears in NO safety guard anywhere in this model (round 3 audit, see
\* the classification table in the task report) -- a dangerous wall-clock-reclaim is
\* precisely a holder that does NOT know it was superseded (drifted clock still says the
\* lease is fine) while GLOBAL truth already moved on; baking politeness into a guard
\* would mask exactly that bug. `lostThenWrote`/`SupersededWriterMakesNoMutation` are
\* KEPT, unchanged in meaning, as the dedicated regression guard for the UNRELATED
\* `SabSupersededWrites` bug class (dropping the live epoch-match check) -- a distinct
\* axis from drift/reclaim-timing and crash-liveness, deliberately proxied by knowledge.
\* `trulySuperseded`/`supersededThenWrote` are the GLOBAL-truth witness: `epoch` (the
\* durable counter, a SEPARATE object from `mount`) has been advanced by a completed
\* reclaim (`ObservedReclaim`/`WallClockReclaim`) past what `a` itself last allocated --
\* independent of whether `a` ever learned about it.
Write(a) ==
    LET epochOK == IF SabSupersededWrites THEN TRUE ELSE mount.epoch = localEpoch[a]
        trulySuperseded == epoch > localEpoch[a]
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ ~crashed[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ clock < fenceUntil
    /\ ~mount.fenced
    /\ epochOK
    /\ wrote' = wrote \union {<< a, localEpoch[a] >>}
    /\ rootEmpty' = FALSE
    /\ lostThenWrote' = (lostThenWrote \/ localLost[a])
    /\ supersededThenWrote' = (supersededThenWrote \/ trulySuperseded)
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, firstOwner, reclaimed,
                    remountedAfterFence, fenceUntil, obsToken, obsSince, observedReclaimEver >>

\* SABOTAGE action (only enabled under SabEpochReset): zero the durable epoch
\* counter when the mount is cleared. Breaks epoch monotonicity.
SabResetEpoch ==
    /\ SabEpochReset
    /\ mount = None
    /\ epoch > 0
    /\ epoch' = 0
    /\ UNCHANGED << owner, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* Clear an expired UNFENCED mount (lets SabResetEpoch fire; also a benign honest step).
\* A fenced slot is never cleared: in the implementation mount objects persist, and the
\* fenced body is exactly what lets a restart classify "my old incarnation was fenced".
ClearExpiredMount ==
    /\ mount # None
    /\ ~mount.fenced
    /\ mount.deadline <= clock
    /\ mount' = None
    /\ UNCHANGED << owner, epoch, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* --- rev.6: observation-based reclaim vs wall-clock sabotage -------------------------
\* StartObservation: a reclaimer (GC leader, or a same-uuid successor probing an
\* apparently-dead mount) begins watching the write-token. Restarts implicitly: ANY
\* mount write bumps mtoken, so a stale obsToken simply stops matching mtoken and
\* ObservedReclaim below cannot fire until a fresh StartObservation re-arms it.
StartObservation ==
    /\ mount # None
    /\ obsToken = None
    /\ obsToken' = mtoken
    /\ obsSince' = clock
    /\ UNCHANGED << owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    observedReclaimEver, supersededThenWrote >>

\* ObservedReclaim: the FIXED protocol. Reclaim only once the token has held stable for
\* the FULL rate-bound wait (TTL + Drift) on the reclaimer's OWN clock -- this provably
\* guarantees clock >= fenceUntil (the holder's true fence already expired), because the
\* holder's last renewal/claim producing this token happened at or before obsSince, and
\* that renewal's fenceUntil is bounded by (that renewal's clock) + TTL + Drift <=
\* obsSince + TTL + Drift <= clock.
\*
\* rev.6 round 2: this bumps ONLY the durable `epoch` counter -- GLOBAL truth that a new
\* epoch is now authoritative, exactly like `AllocEpoch` but performed by the reclaimer
\* rather than the (unknowing) prior holder. It deliberately does NOT rewrite `mount`
\* (that would be a SEPARATE, later write -- the model header already documents `epoch`
\* as living in its own object, not the mount) and does NOT touch `localLost` -- setting
\* `localLost` here would be modeling "the reclaimer's belief becomes the holder's
\* knowledge" for free, which is exactly the politeness-implies-safety conflation this
\* round removes. Because a full stability wait was paid, `clock >= fenceUntil` already
\* holds by the time this can fire (see the proof above), so `Write` is independently,
\* mechanically blocked for the old holder from this point on -- no reliance on
\* `localLost`/`mount.epoch` needed for THIS path to stay safe.
ObservedReclaim ==
    /\ ~SabWallClockReclaim
    /\ mount # None
    /\ obsToken # None /\ obsToken = mtoken            \* token stable since obsSince
    /\ clock - obsSince >= TTL + Drift                 \* full rate-bound wait on OUR clock
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ obsToken' = None
    /\ observedReclaimEver' = TRUE
    /\ UNCHANGED << owner, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner, lostThenWrote,
                    reclaimed, remountedAfterFence, fenceUntil, obsSince, supersededThenWrote >>

\* WallClockReclaim: the SABOTAGE. Trusts the stamp directly (clock > mount.deadline)
\* with NO observation wait at all -- exactly the old cross-node wall-clock comparison
\* the rev.6 design replaces. When Drift > 0 the true holder may still be safely inside
\* its own fenceUntil at this clock value, so this can fire strictly earlier than
\* ObservedReclaim ever could -- and, just like ObservedReclaim, it only advances the
\* durable `epoch` (GLOBAL truth), never `localLost` (the old holder's KNOWLEDGE): the
\* whole point of the sabotage is a reclaimer that is wrong to believe the holder is
\* dead, so the old holder correctly does NOT learn anything here.
WallClockReclaim ==
    /\ SabWallClockReclaim
    /\ mount # None /\ clock > mount.deadline
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ UNCHANGED << owner, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner, lostThenWrote,
                    reclaimed, remountedAfterFence, fenceUntil, obsToken, obsSince,
                    observedReclaimEver, supersededThenWrote >>

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
    \/ StartObservation
    \/ ObservedReclaim
    \/ WallClockReclaim

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
    /\ crashed \in [Actors -> BOOLEAN]
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
    /\ fenceUntil \in 0..(MaxClock + TTL + Drift)
    /\ obsToken \in (0..MaxToken) \union {None}
    /\ obsSince \in 0..MaxClock
    /\ observedReclaimEver \in BOOLEAN
    /\ supersededThenWrote \in BOOLEAN

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

\* A writer that has locally LEARNED it was superseded/fenced (KNOWLEDGE, via
\* Renew/AdoptRead/Die noticing a mismatch or a crash) never enters a NEW mutation.
\* This is the regression guard for the UNRELATED `SabSupersededWrites` bug class
\* (dropping the live epoch-match check on `Write`) -- a distinct axis from
\* drift/reclaim-timing, deliberately left proxied by local knowledge. See
\* `GlobalSupersededWriterMakesNoMutation` for the rev.6 GLOBAL-truth counterpart.
SupersededWriterMakesNoMutation ==
    ~lostThenWrote

\* rev.6: a writer never mutates once GLOBAL truth (the durable `epoch` counter, a
\* completed `ObservedReclaim`/`WallClockReclaim`) has already moved past its own
\* `localEpoch[a]` -- regardless of whether the writer ever LEARNED this (`localLost`
\* is knowledge, not a safety fact; see `Write`'s header comment). `SabWallClockReclaim`
\* must violate this: a wall-clock-trusting reclaim can complete strictly before the
\* true holder's `fenceUntil` expires, so the holder's next mechanically-valid `Write`
\* lands after global truth already superseded it.
GlobalSupersededWriterMakesNoMutation ==
    ~supersededThenWrote

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

\* rev.6: the FIXED (observation-based) reclaim rule actually fires at least once --
\* the "wait for full token stability" recovery path is not vacuous.
W_ObservedReclaim ==
    ~observedReclaimEver
=============================================================================
