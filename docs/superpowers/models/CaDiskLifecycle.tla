--------------------------- MODULE CaDiskLifecycle ---------------------------
(***************************************************************************)
(* Pool lifecycle of a writable CAS mount, its ONE lease thread, the GC    *)
(* scheduler, and the `SYSTEM CONTENT ADDRESSED FORGET` protocol           *)
(* (`Pool::forgetDisk`).                                                   *)
(*                                                                         *)
(* Lifecycle: Live, Transient, IdentityLost, Vanished = {Replaced,         *)
(* Forgotten}. `IdentityLost` is entered on authoritative sentinel         *)
(* absence; `Replaced` on a foreign `pool_id`; `Forgotten` by FORGET.      *)
(*                                                                         *)
(* The lease thread renews and reclaims in sequence (design 2026-10-01,    *)
(* "One loop"):                                                            *)
(*   until stopped or `LeaseTerminal`:                                     *)
(*     a remount is pending (reqGen > handledGen): reclaim for the         *)
(*       generation it snapshots, or fail and retry;                       *)
(*     otherwise renew: success sets the deadline; any other end trips    *)
(*       the fence and requests a remount unless one is pending.           *)
(* `deadlineOk` abstracts "the fence deadline admits a write". One arming  *)
(* rule (`CanArm`): the fence is armed and Live reported only with no      *)
(* remount request pending, no stop and no terminal lifecycle. A reclaim   *)
(* arms when its claim admits a write; otherwise the next successful       *)
(* renewal arms in the same step that reports Live.                        *)
(*                                                                         *)
(* FORGET steps in `Pool::forgetDisk` order: mutex -> idempotent           *)
(* `isVanished` short-circuit -> publish intent -> trip#1 -> GC stop       *)
(* (signal, join) -> lease thread stop (signal, join) -> trip#2 -> drain   *)
(* -> teardown (farewell only if drained and the renewer is Active) ->     *)
(* renewer reset -> enterVanished(Forgotten), first terminal wins.         *)
(*                                                                         *)
(* Not modelled: time and retry spacing, the open's own readiness renewal, *)
(* the content of a claim or a renewal.                                    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    EnableTrip2,       \* FORGET re-trips the fence after joining the lease thread
    DrainGate,         \* farewell only when the ref lanes provably drained
    EnableGcSelfExit,  \* the GC scheduler exits at a wake that sees a Vanished pool
    RenewChecksIntent, \* a renewal never starts once the FORGET intent is published
    ReadinessGate,     \* a reclaim with a too-old claim reports Live only after a renewal
    LiveChecksPending, \* a renewal arms nothing while a remount request is pending
    ReclaimChecksPending, \* a reclaim arms nothing while a newer request is pending
    ArmChecksTerminal, \* nothing arms after a stop or with a terminal lifecycle
    AckSnapshot,       \* a reclaim acknowledges only the generation it snapshotted
    ReclaimTrips,      \* a reclaim's step 0 trips the fence, not only the lifecycle
    MaxGen             \* bound on remount requests (state-space bound only)

VanishedSet == {"VanishedReplaced", "VanishedForgotten"}
PStates == {"Live", "Transient", "IdentityLost"} \cup VanishedSet

VARIABLES
    pstate,         \* CasMountRuntime.pool_lifecycle
    intent,         \* vanished_intent
    termPublished,  \* terminal_state_published
    lost,           \* MountFence.lost
    deadlineOk,     \* the fence deadline still admits a write
    keeper,         \* renewer: "Active" | "Terminal" | "StoppedClean" | "StoppedNoFarewell" | "None"
    farewell,       \* the clean-release marker was written
    drained,        \* "Unset" | "Yes" | "No" (drainRefLanesForShutdown)
    lpc,            \* lease thread: "Top" | "Renewing" | "InAttempt" | "Exited"
    stop,           \* workers_stop_requested
    reqGen,         \* remount_requested_generation
    handledGen,     \* remount_handled_generation
    snapGen,        \* the generation the latest reclaim snapshotted
    gcsched,        \* "Running" | "Stopped" | "Destroyed"
    gcstopping,     \* CasGcScheduler.stopping
    round,          \* a GC round is in flight
    mutex,          \* lifecycle_mutex: "None" | "Forget"
    fpc,            \* FORGET pc
    naturalWon,     \* history: a natural Replaced transition published the state
    everIL,         \* history: IdentityLost was entered
    everVanished,   \* history: a Vanished state was entered
    liveByRenewal   \* history: a renewal armed the fence and reported Live

vars == <<pstate, intent, termPublished, lost, deadlineOk, keeper, farewell,
          drained, lpc, stop, reqGen, handledGen, snapGen,
          gcsched, gcstopping, round, mutex, fpc, naturalWon, everIL,
          everVanished, liveByRenewal>>

leaseVars == <<lpc, reqGen, handledGen, snapGen, liveByRenewal>>
gcVars == <<gcsched, gcstopping, round>>
forgetVars == <<mutex, fpc, drained, farewell, stop>>
historyVars == <<naturalWon, everIL, everVanished>>

FpcStates == {"NotStarted", "Check", "ShortJoin", "Intent", "Trip1",
              "GcStopSignal", "GcStopJoin", "JoinSignal", "JoinWait",
              "Trip2", "Drain", "Teardown", "KeeperReset", "Enter", "Done"}

TypeOK ==
    /\ pstate \in PStates
    /\ intent \in BOOLEAN /\ termPublished \in BOOLEAN /\ lost \in BOOLEAN
    /\ deadlineOk \in BOOLEAN
    /\ keeper \in {"Active", "Terminal", "StoppedClean", "StoppedNoFarewell", "None"}
    /\ farewell \in BOOLEAN
    /\ drained \in {"Unset", "Yes", "No"}
    /\ lpc \in {"Top", "Renewing", "InAttempt", "Exited"}
    /\ stop \in BOOLEAN
    /\ reqGen \in 0..MaxGen /\ handledGen \in 0..MaxGen /\ snapGen \in 0..MaxGen
    /\ gcsched \in {"Running", "Stopped", "Destroyed"}
    /\ gcstopping \in BOOLEAN /\ round \in BOOLEAN
    /\ mutex \in {"None", "Forget"}
    /\ fpc \in FpcStates
    /\ naturalWon \in BOOLEAN /\ everIL \in BOOLEAN /\ everVanished \in BOOLEAN
    /\ liveByRenewal \in BOOLEAN

Init ==
    /\ pstate = "Live" /\ intent = FALSE /\ termPublished = FALSE
    /\ lost = FALSE /\ deadlineOk = TRUE /\ keeper = "Active" /\ farewell = FALSE
    /\ drained = "Unset" /\ lpc = "Top" /\ stop = FALSE
    /\ reqGen = 0 /\ handledGen = 0 /\ snapGen = 0
    /\ gcsched = "Running" /\ gcstopping = FALSE /\ round = FALSE
    /\ mutex = "None" /\ fpc = "NotStarted"
    /\ naturalWon = FALSE /\ everIL = FALSE /\ everVanished = FALSE
    /\ liveByRenewal = FALSE

\* `remountTerminal`: the published FORGET intent or IdentityLost. Every
\* Vanished state also publishes the intent.
LeaseTerminal == intent \/ pstate = "IdentityLost"

Pending == reqGen > handledGen

\* `tripMountLost`: latch the fence; count a lease loss (Live -> Transient)
\* only when no FORGET intent is published.
TripMountLost ==
    /\ lost' = TRUE
    /\ pstate' = IF pstate = "Live" /\ ~intent THEN "Transient" ELSE pstate

Max(a, b) == IF a > b THEN a ELSE b

\* The one arming rule; `pendingAfter` is whether a request stays pending
\* once this step's acknowledgement is applied.
CanArm(pendingAfter, checkPending) ==
    /\ (~checkPending \/ ~pendingAfter)
    /\ (~ArmChecksTerminal \/ (~stop /\ ~LeaseTerminal))

(***************************************************************************)
(* THE LEASE THREAD                                                        *)
(***************************************************************************)
LExit ==
    /\ lpc = "Top"
    /\ (stop \/ LeaseTerminal)
    /\ lpc' = "Exited"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   reqGen, handledGen, snapGen, liveByRenewal,
                   gcVars, forgetVars, historyVars>>

\* Step 0 of `Pool::tryRemountOnce` presumes the lease lost. A reclaim can
\* begin on an armed fence when a request was raised during the previous
\* reclaim, whose arm then overrode that request's trip.
LAttemptBegin ==
    /\ lpc = "Top"
    /\ ~stop /\ ~LeaseTerminal /\ Pending
    /\ lpc' = "InAttempt" /\ snapGen' = reqGen
    /\ IF ReclaimTrips
       THEN TripMountLost
       ELSE /\ pstate' = IF pstate = "Live" THEN "Transient" ELSE pstate
            /\ UNCHANGED lost
    /\ UNCHANGED <<intent, termPublished, deadlineOk, keeper, reqGen,
                   handledGen, liveByRenewal, gcVars, forgetVars,
                   historyVars>>

\* Verdict StayTransient, a probe that could not reach the store, an
\* unclaimable mount, or any failed later step.
LAttemptFail ==
    /\ lpc = "InAttempt"
    /\ lpc' = "Top"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   reqGen, handledGen, snapGen, liveByRenewal,
                   gcVars, forgetVars, historyVars>>

\* Verdict Recover: install and start a fresh renewer, acknowledge the
\* snapshot. `ready` = the claim still admits a write. Arm and report Live
\* under `CanArm`; otherwise leave the fence latched and the pool Transient
\* for the loop's next renewal, or its next reclaim if a request is pending.
LReclaim ==
    /\ lpc = "InAttempt" /\ pstate = "Transient"
    /\ \E ready \in BOOLEAN :
         /\ deadlineOk' = ready
         /\ IF (ready \/ ~ReadinessGate)
               /\ CanArm(reqGen > snapGen, ReclaimChecksPending)
            THEN pstate' = "Live" /\ lost' = FALSE
            ELSE UNCHANGED <<pstate, lost>>
    /\ keeper' = "Active" /\ lpc' = "Top"
    /\ handledGen' = IF AckSnapshot THEN Max(handledGen, snapGen) ELSE reqGen
    /\ UNCHANGED <<intent, termPublished, reqGen, snapGen, liveByRenewal,
                   gcVars, forgetVars, historyVars>>

\* Both sentinels authoritatively absent.
LEnterIL ==
    /\ lpc = "InAttempt" /\ pstate = "Transient"
    /\ pstate' = "IdentityLost" /\ everIL' = TRUE /\ lpc' = "Top"
    /\ UNCHANGED <<intent, termPublished, lost, deadlineOk, keeper, reqGen,
                   handledGen, snapGen, liveByRenewal, gcVars, forgetVars,
                   naturalWon, everVanished>>

\* A Present `_pool_meta` with a foreign `pool_id`: enterVanished(Replaced).
\* An attempt past its step-0 bail can land it mid-FORGET; the first
\* terminal transition wins.
LPromoteReplaced ==
    /\ lpc = "InAttempt"
    /\ pstate \in {"Transient", "IdentityLost"}
    /\ lpc' = "Top" /\ intent' = TRUE
    /\ IF termPublished
       THEN UNCHANGED <<pstate, termPublished, naturalWon, everVanished>>
       ELSE /\ pstate' = "VanishedReplaced" /\ termPublished' = TRUE
            /\ naturalWon' = TRUE /\ everVanished' = TRUE
    /\ UNCHANGED <<lost, deadlineOk, keeper, reqGen, handledGen, snapGen,
                   liveByRenewal, gcVars, forgetVars, everIL>>

\* The period wait is abstracted: a renewal may start whenever the loop is
\* idle with no pending request.
LRenewBegin ==
    /\ lpc = "Top"
    /\ ~stop /\ ~Pending
    /\ pstate \notin ({"IdentityLost"} \cup VanishedSet)
    /\ (~RenewChecksIntent \/ ~intent)
    /\ lpc' = "Renewing"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   reqGen, handledGen, snapGen, liveByRenewal,
                   gcVars, forgetVars, historyVars>>

\* Success. A request already under way can succeed after a stop, a trip
\* or a remount request; `ready` = start + TTL still admits a write. On a
\* pool a reclaim left Transient with a too-old claim, arm the fence and
\* report Live in the same step.
LRenewOk ==
    /\ lpc = "Renewing"
    /\ lpc' = "Top"
    /\ \E ready \in BOOLEAN :
         LET report == ready /\ pstate = "Transient"
                       /\ CanArm(Pending, LiveChecksPending)
         IN /\ deadlineOk' = ready
            /\ pstate' = IF report THEN "Live" ELSE pstate
            /\ lost' = IF report THEN FALSE ELSE lost
            /\ liveByRenewal' = (liveByRenewal \/ report)
    /\ UNCHANGED <<intent, termPublished, keeper, reqGen, handledGen,
                   snapGen, gcVars, forgetVars, historyVars>>

\* Any other end: a definitive answer, or an early end on a stop, a pending
\* remount request or a terminal lifecycle. A lost fence alone does not end
\* a renewal: the renewal that arms a too-old reclaim runs under it. The renewer becomes
\* RenewalTerminal, which forbids the farewell. A stop trips the fence
\* without counting a lease loss; otherwise trip and request a remount
\* unless one is pending.
LRenewEnd ==
    /\ lpc = "Renewing"
    /\ (stop \/ LeaseTerminal \/ Pending \/ reqGen < MaxGen)
    /\ lpc' = "Top" /\ keeper' = "Terminal"
    /\ IF stop
       THEN lost' = TRUE /\ UNCHANGED pstate
       ELSE TripMountLost
    /\ reqGen' = IF stop \/ LeaseTerminal \/ Pending THEN reqGen ELSE reqGen + 1
    /\ UNCHANGED <<intent, termPublished, deadlineOk, handledGen, snapGen,
                   liveByRenewal, gcVars, forgetVars, historyVars>>

LRenewEndEarly == (stop \/ LeaseTerminal \/ Pending) /\ LRenewEnd

(***************************************************************************)
(* OUTSIDE THE THREAD                                                      *)
(***************************************************************************)
\* `reportImpossibleInterference`: tripMountLost + scheduleRemount, from any
\* thread, while a renewer exists.
XInterfere ==
    /\ keeper \in {"Active", "Terminal"}
    /\ (stop \/ LeaseTerminal \/ reqGen < MaxGen)
    /\ TripMountLost
    /\ reqGen' = IF stop \/ LeaseTerminal THEN reqGen ELSE reqGen + 1
    /\ UNCHANGED <<intent, termPublished, deadlineOk, keeper, lpc, handledGen,
                   snapGen, liveByRenewal, gcVars, forgetVars,
                   historyVars>>

\* The lease expires while no renewal succeeds. Neither the fence latch nor
\* the lifecycle changes.
XExpire ==
    /\ deadlineOk
    /\ deadlineOk' = FALSE
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, leaseVars,
                   gcVars, forgetVars, historyVars>>

(***************************************************************************)
(* GC SCHEDULER LOOP + ADMIN VERBS                                         *)
(***************************************************************************)
GcRoundStart ==
    /\ gcsched = "Running" /\ ~gcstopping /\ ~round
    /\ pstate \notin VanishedSet
    /\ round' = TRUE
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   leaseVars, gcsched, gcstopping, forgetVars, historyVars>>

GcRoundEnd ==
    /\ round
    /\ round' = FALSE
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   leaseVars, gcsched, gcstopping, forgetVars, historyVars>>

\* Self-exit at a wake that observes a Vanished pool, after the round ends.
GcSelfExit ==
    /\ EnableGcSelfExit
    /\ gcsched = "Running" /\ pstate \in VanishedSet /\ ~round
    /\ gcsched' = "Stopped"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   leaseVars, gcstopping, round, forgetVars, historyVars>>

\* `GC STOP`: under lifecycle_mutex; stop() joins an in-flight round.
VGcStop ==
    /\ mutex = "None" /\ gcsched = "Running" /\ ~round
    /\ gcsched' = "Stopped"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   leaseVars, gcstopping, round, forgetVars, historyVars>>

\* `GC START`: checkOpAdmitted(Admin) refuses unless the pool is Live.
VGcStart ==
    /\ mutex = "None" /\ gcsched = "Stopped" /\ pstate = "Live"
    /\ gcsched' = "Running"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   leaseVars, gcstopping, round, forgetVars, historyVars>>

(***************************************************************************)
(* FORGET (one instance; each step one atomic action)                      *)
(***************************************************************************)
FStart ==
    /\ fpc = "NotStarted" /\ mutex = "None"
    /\ mutex' = "Forget" /\ fpc' = "Check"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcVars, historyVars>>

\* Already Vanished: return. The storage-side scheduler local still
\* stop()s on return (signal + join).
FCheckShort ==
    /\ fpc = "Check" /\ pstate \in VanishedSet
    /\ gcstopping' = TRUE /\ fpc' = "ShortJoin"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcsched, round, mutex,
                   historyVars>>

FShortJoin ==
    /\ fpc = "ShortJoin" /\ ~round
    /\ gcsched' = "Destroyed" /\ fpc' = "Done" /\ mutex' = "None"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcstopping, round,
                   historyVars>>

FCheckProceed ==
    /\ fpc = "Check" /\ pstate \notin VanishedSet
    /\ fpc' = "Intent"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcVars, mutex,
                   historyVars>>

FIntent ==
    /\ fpc = "Intent"
    /\ intent' = TRUE /\ fpc' = "Trip1"
    /\ UNCHANGED <<pstate, termPublished, lost, deadlineOk, keeper, farewell,
                   drained, leaseVars, stop, gcVars, mutex, historyVars>>

FTrip1 ==
    /\ fpc = "Trip1"
    /\ TripMountLost
    /\ fpc' = "GcStopSignal"
    /\ UNCHANGED <<intent, termPublished, deadlineOk, keeper, farewell,
                   drained, leaseVars, stop, gcVars, mutex, historyVars>>

FGcStopSignal ==
    /\ fpc = "GcStopSignal"
    /\ gcstopping' = TRUE /\ fpc' = "GcStopJoin"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcsched, round, mutex,
                   historyVars>>

FGcStopJoin ==
    /\ fpc = "GcStopJoin" /\ ~round
    /\ gcsched' = "Destroyed" /\ fpc' = "JoinSignal"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcstopping, round,
                   mutex, historyVars>>

\* stopBackgroundWorkers: latch the stop, then join the lease thread.
FJoinSignal ==
    /\ fpc = "JoinSignal"
    /\ stop' = TRUE /\ fpc' = "JoinWait"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, gcVars, mutex, historyVars>>

FJoinWait ==
    /\ fpc = "JoinWait" /\ lpc = "Exited"
    /\ fpc' = IF EnableTrip2 THEN "Trip2" ELSE "Drain"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, drained, leaseVars, stop, gcVars, mutex,
                   historyVars>>

\* Re-latch after the join. Under `CanArm` nothing arms once the intent is
\* published; this covers an arm that ignores the intent.
FTrip2 ==
    /\ fpc = "Trip2"
    /\ TripMountLost
    /\ fpc' = "Drain"
    /\ UNCHANGED <<intent, termPublished, deadlineOk, keeper, farewell,
                   drained, leaseVars, stop, gcVars, mutex, historyVars>>

FDrain ==
    /\ fpc = "Drain"
    /\ \E d \in {"Yes", "No"} : drained' = d
    /\ fpc' = "Teardown"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, keeper,
                   farewell, leaseVars, stop, gcVars, mutex, historyVars>>

\* finishTeardown: `release` (the farewell) only from an Active renewer and
\* only if drained.
FTeardown ==
    /\ fpc = "Teardown"
    /\ IF keeper \in {"Active", "Terminal"}
       THEN IF keeper = "Active" /\ (drained = "Yes" \/ ~DrainGate)
            THEN keeper' = "StoppedClean" /\ farewell' = TRUE
            ELSE keeper' = "StoppedNoFarewell" /\ UNCHANGED farewell
       ELSE UNCHANGED <<keeper, farewell>>
    /\ fpc' = "KeeperReset"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, drained,
                   leaseVars, stop, gcVars, mutex, historyVars>>

FKeeperReset ==
    /\ fpc = "KeeperReset"
    /\ keeper' = "None" /\ fpc' = "Enter"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, deadlineOk, farewell,
                   drained, leaseVars, stop, gcVars, mutex, historyVars>>

\* enterVanished(Forgotten); a raced natural promotion makes it a no-op.
FEnter ==
    /\ fpc = "Enter"
    /\ intent' = TRUE
    /\ IF termPublished
       THEN UNCHANGED <<pstate, termPublished, everVanished>>
       ELSE /\ pstate' = "VanishedForgotten" /\ termPublished' = TRUE
            /\ everVanished' = TRUE
    /\ fpc' = "Done" /\ mutex' = "None"
    /\ UNCHANGED <<lost, deadlineOk, keeper, farewell, drained, leaseVars,
                   stop, gcVars, naturalWon, everIL>>

(***************************************************************************)
Next ==
    \/ LExit \/ LAttemptBegin \/ LAttemptFail \/ LReclaim \/ LEnterIL
    \/ LPromoteReplaced \/ LRenewBegin \/ LRenewOk \/ LRenewEnd
    \/ XInterfere \/ XExpire
    \/ GcRoundStart \/ GcRoundEnd \/ GcSelfExit \/ VGcStop \/ VGcStart
    \/ FStart \/ FCheckShort \/ FShortJoin \/ FCheckProceed \/ FIntent
    \/ FTrip1 \/ FGcStopSignal \/ FGcStopJoin \/ FJoinSignal \/ FJoinWait
    \/ FTrip2 \/ FDrain \/ FTeardown \/ FKeeperReset \/ FEnter

Spec == Init /\ [][Next]_vars

\* Every FORGET step, the lease thread's exit and the ends of its attempt
\* and of a renewal that must end early, a GC round, and the GC self-exit
\* make progress. FStart is not fair: FORGET is operator-initiated.
FairSpec ==
    /\ Spec
    /\ WF_vars(FCheckShort) /\ WF_vars(FShortJoin) /\ WF_vars(FCheckProceed)
    /\ WF_vars(FIntent) /\ WF_vars(FTrip1) /\ WF_vars(FGcStopSignal)
    /\ WF_vars(FGcStopJoin) /\ WF_vars(FJoinSignal) /\ WF_vars(FJoinWait)
    /\ WF_vars(FTrip2) /\ WF_vars(FDrain) /\ WF_vars(FTeardown)
    /\ WF_vars(FKeeperReset) /\ WF_vars(FEnter)
    /\ WF_vars(LExit) /\ WF_vars(LAttemptFail) /\ WF_vars(LRenewEndEarly)
    /\ WF_vars(GcRoundEnd) /\ WF_vars(GcSelfExit)

(***************************************************************************)
(* PROPERTIES                                                              *)
(***************************************************************************)
ForgetDone == fpc = "Done"

\* FORGET-complete => fence latched + fully-terminal state.
I1ForgetTerminal == ForgetDone => (lost /\ pstate \in VanishedSet)

\* Unless a natural Replaced transition won the race, Forgotten.
I1bForgetWinsUnlessNatural ==
    (ForgetDone /\ ~naturalWon) => pstate = "VanishedForgotten"

\* No unearned farewell: the lanes drained and the lease thread was joined.
I2EarnedFarewell == farewell => (drained = "Yes" /\ lpc = "Exited")

I3OneWay ==
    /\ everIL => pstate \in ({"IdentityLost"} \cup VanishedSet)
    /\ everVanished => pstate \in VanishedSet

I4aTransientImpliesLost == pstate \in {"Transient", "IdentityLost"} => lost

\* The benign-answer gate (Vanished) never coexists with an armed fence.
I4bVanishedImpliesLost == pstate \in VanishedSet => lost

I6GcStoppedAfterForget == ForgetDone => gcsched = "Destroyed"

\* A reclaim acknowledges no request raised after its snapshot.
I7RequestNotLost == handledGen <= snapGen

\* The thread renews only a renewer that is not RenewalTerminal.
I8RenewsActiveRenewer == lpc = "Renewing" => keeper = "Active"

\* No renewal starts once the FORGET intent is published.
RenewNotAfterIntent ==
    [][(lpc # "Renewing" /\ lpc' = "Renewing") => ~intent]_vars

\* No renewal starts while a remount request is pending.
RenewNotWhilePending ==
    [][(lpc # "Renewing" /\ lpc' = "Renewing") => ~Pending]_vars

\* The fence is never armed while a remount request stays pending.
NoArmWhileRequestPending == [][(lost /\ ~lost') => ~Pending']_vars

\* Live is reported only when a write can be admitted.
LiveOnlyWhenAdmissible ==
    [][(pstate # "Live" /\ pstate' = "Live") => (~lost' /\ deadlineOk')]_vars

\* Liveness (FairSpec): a started FORGET always completes.
ForgetCompletes == (fpc = "Check") ~> ForgetDone

\* Liveness (FairSpec): once Vanished, the GC scheduler stops ticking.
GcExitsAfterVanished == (pstate \in VanishedSet) ~> (gcsched # "Running")

\* Liveness (FairSpec): the lease thread exits on a stop, a published FORGET
\* intent or IdentityLost.
LeaseThreadExits == (stop \/ LeaseTerminal) ~> (lpc = "Exited")

\* Witnesses (expected VIOLATED in their configs = reachability).
WForgetNeverDone == ~ForgetDone
WNoRacedReplaced == ~(ForgetDone /\ pstate = "VanishedReplaced")
\* No reclaim arms the fence inside FORGET's join window.
NoJoinWindowReclaim == ~(fpc = "Trip2" /\ pstate = "Live" /\ ~lost)
WNoLiveByRenewal == ~liveByRenewal
WNoRequestDuringReclaim == ~(lpc = "InAttempt" /\ reqGen > snapGen)
WNoForgetFarewell == ~(ForgetDone /\ farewell)

=============================================================================
