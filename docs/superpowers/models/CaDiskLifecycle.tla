--------------------------- MODULE CaDiskLifecycle ---------------------------
(***************************************************************************)
(* TLA+ model of the rev.8 (FORGET-only v1) pool lifecycle state machine   *)
(* + the as-built `SYSTEM CONTENT ADDRESSED FORGET` protocol (spec         *)
(* 2026-07-22 "throw-when-uncertain" SS1/SS3/SS5/SS6; Tasks 5/10/11 on     *)
(* `cas-gc-rebuild`), THE Task-15 gate.                                    *)
(*                                                                         *)
(* v1 scope (user decision, 2026-07-23): the natural `Vanished(erased)`    *)
(* proof stack is EXCISED from the code -- the observer never promotes to  *)
(* erased and that state does not exist. `IdentityLost` is entered         *)
(* directly on authoritative sentinel absence (no prefix-emptiness leg).   *)
(* Vanished = { Replaced (identity gate: Present + foreign pool_id),       *)
(* Forgotten (the FORGET verb) }. Two queued hardenings are modeled as     *)
(* landed: [C1] the GC scheduler loop/heartbeat check isVanished() at      *)
(* each wake and SELF-EXIT (no new round once Vanished; `GcSelfExit`,      *)
(* sabotage `_sab_nogcselfexit` shows the pre-fix bug as a liveness        *)
(* violation); [M1] the remount attempt's step-0 bail checks               *)
(* `vanished_intent` as well as `isVanished()` -- with the loop-condition  *)
(* check this means NO attempt begins once the intent is published         *)
(* (`RAttemptBegin`); an attempt already past its bail may still complete  *)
(* a reclaim or land a Replaced promotion mid-FORGET -- the honest race    *)
(* window trip#2 and the first-terminal-wins latch exist for.              *)
(*                                                                         *)
(* FORGET steps, exactly the as-built order of `Pool::forgetDisk` +        *)
(* `ContentAddressedMetadataStorage::forgetDisk`:                          *)
(*   mutex-acquire -> idempotent isVanished short-circuit (the detached    *)
(*   scheduler still stop()s via its local's dtor) -> publish intent ->    *)
(*   trip#1 -> GC stop (signal, then join waits out an in-flight round)    *)
(*   -> remount stop (shutdown latch, then join) -> trip#2 (re-latch the   *)
(*   fence against a reclaim that completed inside the join window) ->     *)
(*   drain (nondet outcome) -> teardown (farewell ONLY if drained) ->      *)
(*   keeperReset -> enterVanished(Forgotten) (idempotent: first terminal   *)
(*   transition wins) -> mutex-release.                                    *)
(*                                                                         *)
(* INVARIANTS                                                              *)
(*   I1  ForgetTerminal   : FORGET done => fence latched (mayMutate FALSE) *)
(*                          and the pool is in a fully-terminal Vanished   *)
(*                          state -- under ALL interleavings, including a  *)
(*                          reclaim completing inside the join window      *)
(*                          (the trip#2 sufficiency, Task 10 concern #1).  *)
(*   I1b ForgetWinsUnlessNaturalWon : if no natural Replaced transition    *)
(*                          raced FORGET, the final state is Forgotten     *)
(*                          (first-terminal-wins is BY DESIGN, so a raced  *)
(*                          Replaced outcome is legal).                    *)
(*   I2  EarnedFarewell   : a clean-release farewell is written ONLY       *)
(*                          after the ref lanes provably drained.          *)
(*   I3  OneWay           : IdentityLost/Vanished are never followed by    *)
(*                          Live or Transient (IdentityLost -> Vanished    *)
(*                          is allowed); Vanished is absorbing.            *)
(*   I4a/I4b              : Transient/IdentityLost and every Vanished      *)
(*                          state imply a latched fence -- the benign-     *)
(*                          answer gate (enabled exactly in the Vanished   *)
(*                          states) never coexists with write authority.   *)
(*   I6  GcStoppedAfterForget : FORGET done => the scheduler is destroyed  *)
(*                          and can never restart (START's Admin gate      *)
(*                          refuses on a non-Live pool).                   *)
(*   ForgetCompletes (liveness, FairSpec): a started FORGET always         *)
(*                          terminates -- no interleaving (in-flight       *)
(*                          attempt, in-flight round, racing natural       *)
(*                          promotion = I5) wedges it.                     *)
(*   GcExitsAfterVanished (liveness, FairSpec): once the pool is           *)
(*                          Vanished, the GC scheduler eventually stops    *)
(*                          ticking -- the [C1] fix; RED without it.       *)
(*                                                                         *)
(* SABOTAGES (must VIOLATE -- teeth checks)                                *)
(*   _sab_notrip2          : EnableTrip2=FALSE reproduces the Task-10      *)
(*                           review race (reclaim inside the join window   *)
(*                           leaves mayMutate TRUE after FORGET): I1/I4b.  *)
(*   _sab_unearnedfarewell : DrainGate=FALSE writes the farewell without   *)
(*                           a real drain: I2.                             *)
(*   _sab_nogcselfexit     : EnableGcSelfExit=FALSE is the pre-[C1] code   *)
(*                           (scheduler never exits on natural Vanished):  *)
(*                           GcExitsAfterVanished liveness RED.            *)
(* WITNESSES (violation = reachable = good)                                *)
(*   _witness_forgetdone, _witness_racedreplaced (first-terminal-wins      *)
(*   race is real), _witness_joinwindowreclaim (the trip#2 race is real).  *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    EnableTrip2,     \* the second tripMountLost after the remount join
    DrainGate,       \* farewell only when the ref lanes provably drained
    EnableGcSelfExit \* [C1] loop()/heartbeatLoop() self-exit on isVanished()

VanishedSet == {"VanishedReplaced", "VanishedForgotten"}
PStates == {"Live", "Transient", "IdentityLost"} \cup VanishedSet

VARIABLES
    pstate,         \* pool lifecycle condition (CasMountRuntime.pool_lifecycle)
    intent,         \* vanished_intent (terminal-intent latch)
    termPublished,  \* terminal_state_published (enterVanished idempotency)
    lost,           \* MountFence.lost; mayMutate == ~lost
    keeper,         \* "Running" | "StoppedClean" | "StoppedNoFarewell" | "None"
    farewell,       \* the clean-release terminal marker was written
    drained,        \* "Unset" | "Yes" | "No" (drainRefLanesForShutdown outcome)
    rthread,        \* logical remount work: "NotRunning" | "LoopTop" | "InAttempt"
    rshutdown,      \* runtime worker-stop request (`stopBackgroundWorkers` latch)
    gcsched,        \* "Running" | "Stopped" | "Destroyed"
    gcstopping,     \* CasGcScheduler.stopping (no new scheduled round)
    round,          \* a GC round is in flight
    mutex,          \* lifecycle_mutex (+ gc_scheduler_mutex): "None" | "Forget"
    fpc,            \* FORGET pc
    naturalWon,     \* a NATURAL terminal transition published the state
    everIL,         \* history: IdentityLost was entered
    everVanished    \* history: some Vanished state was entered

vars == <<pstate, intent, termPublished, lost, keeper, farewell, drained,
          rthread, rshutdown, gcsched, gcstopping, round, mutex, fpc,
          naturalWon, everIL, everVanished>>

FpcStates == {"NotStarted", "Check", "ShortJoin", "Intent", "Trip1",
              "GcStopSignal", "GcStopJoin", "JoinSignal", "JoinWait",
              "Trip2", "Drain", "Teardown", "KeeperReset", "Enter", "Done"}

TypeOK ==
    /\ pstate \in PStates
    /\ intent \in BOOLEAN /\ termPublished \in BOOLEAN /\ lost \in BOOLEAN
    /\ keeper \in {"Running", "StoppedClean", "StoppedNoFarewell", "None"}
    /\ farewell \in BOOLEAN
    /\ drained \in {"Unset", "Yes", "No"}
    /\ rthread \in {"NotRunning", "LoopTop", "InAttempt"}
    /\ rshutdown \in BOOLEAN
    /\ gcsched \in {"Running", "Stopped", "Destroyed"}
    /\ gcstopping \in BOOLEAN /\ round \in BOOLEAN
    /\ mutex \in {"None", "Forget"}
    /\ fpc \in FpcStates
    /\ naturalWon \in BOOLEAN /\ everIL \in BOOLEAN /\ everVanished \in BOOLEAN

Init ==
    /\ pstate = "Live" /\ intent = FALSE /\ termPublished = FALSE
    /\ lost = FALSE /\ keeper = "Running" /\ farewell = FALSE
    /\ drained = "Unset" /\ rthread = "NotRunning" /\ rshutdown = FALSE
    /\ gcsched = "Running" /\ gcstopping = FALSE /\ round = FALSE
    /\ mutex = "None" /\ fpc = "NotStarted"
    /\ naturalWon = FALSE /\ everIL = FALSE /\ everVanished = FALSE

(***************************************************************************)
(* RENEWAL: a terminal result trips the fence (tripMountLost ->            *)
(* noteLeaseLost Live->Transient) and may latch a remount generation.      *)
(* `rthread=NotRunning` abstracts a persistent worker with no active       *)
(* request, not the absence of a constructed thread.                      *)
(***************************************************************************)
KTrip ==
    /\ keeper = "Running"
    /\ lost' = TRUE
    /\ pstate' = IF pstate = "Live" THEN "Transient" ELSE pstate
    /\ UNCHANGED <<intent, termPublished, keeper, farewell, drained,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everIL, everVanished>>

RArm ==
    /\ keeper = "Running" /\ lost
    /\ rthread = "NotRunning" /\ ~rshutdown /\ ~intent
    /\ pstate \notin VanishedSet
    /\ rthread' = "LoopTop"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everIL, everVanished>>

(***************************************************************************)
(* PERSISTENT REMOUNT WORKER. The loop condition AND the [M1] step-0 bail  *)
(* intent/vanished, so no attempt BEGINS once the intent is published      *)
(* (they are adjacent with nothing observable between them -- modeled as   *)
(* one guard). An attempt already past its bail does NOT re-check intent   *)
(* mid-flight -- the honest race the trip#2 exists for.                    *)
(***************************************************************************)
RLoopExit ==
    /\ rthread = "LoopTop"
    /\ (rshutdown \/ intent \/ pstate \in VanishedSet)
    /\ rthread' = "NotRunning"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everIL, everVanished>>

RAttemptBegin ==
    /\ rthread = "LoopTop"
    /\ ~rshutdown /\ ~intent /\ pstate \notin VanishedSet
    /\ rthread' = "InAttempt"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everIL, everVanished>>

\* Verdict StayTransient / any failed attempt.
RAttemptFail ==
    /\ rthread = "InAttempt"
    /\ rthread' = "LoopTop"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everIL, everVanished>>

\* Verdict Recover -> full reclaim: re-arms the fence (armMountFence) and
\* noteRemounted (Transient -> Live only, never IdentityLost/terminal
\* [D3]); the loop breaks on success. NOTE: no intent check -- mid-attempt.
RReclaim ==
    /\ rthread = "InAttempt" /\ pstate = "Transient"
    /\ lost' = FALSE /\ pstate' = "Live" /\ rthread' = "NotRunning"
    /\ UNCHANGED <<intent, termPublished, keeper, farewell, drained,
                   rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everIL, everVanished>>

\* v1 identity gate: authoritative sentinel ABSENCE alone enters
\* IdentityLost (no prefix-emptiness leg -- the erasure proof is excised).
\* [C1] non-absorbing: no intent latch; the observer keeps probing, but in
\* v1 the only natural exit is Replaced; otherwise FORGET/restart.
REnterIL ==
    /\ rthread = "InAttempt" /\ pstate = "Transient"
    /\ pstate' = "IdentityLost" /\ everIL' = TRUE /\ rthread' = "LoopTop"
    /\ UNCHANGED <<intent, termPublished, lost, keeper, farewell, drained,
                   rshutdown, gcsched, gcstopping, round, mutex, fpc,
                   naturalWon, everVanished>>

\* The one natural terminal transition of v1: the identity gate sees a
\* Present `_pool_meta` with a FOREIGN pool_id (enterVanished(Replaced)
\* under remount_mutex; reachable from Transient AND IdentityLost -- the
\* Task-5 diagram note). First terminal transition wins.
RPromoteReplaced ==
    /\ rthread = "InAttempt"
    /\ pstate \in {"Transient", "IdentityLost"}
    /\ rthread' = "LoopTop"
    /\ intent' = TRUE
    /\ IF termPublished
       THEN UNCHANGED <<pstate, termPublished, naturalWon, everVanished>>
       ELSE /\ pstate' = "VanishedReplaced" /\ termPublished' = TRUE
            /\ naturalWon' = TRUE /\ everVanished' = TRUE
    /\ UNCHANGED <<lost, keeper, farewell, drained, rshutdown,
                   gcsched, gcstopping, round, mutex, fpc, everIL>>

(***************************************************************************)
(* GC SCHEDULER LOOP + ADMIN VERBS. [C1] fix: loop()/heartbeatLoop() check *)
(* isVanished() at each wake -- no new round once Vanished, and the        *)
(* scheduler SELF-EXITS (GcSelfExit; the sabotage disables it).            *)
(***************************************************************************)
GcRoundStart ==
    /\ gcsched = "Running" /\ ~gcstopping /\ ~round
    /\ pstate \notin VanishedSet          \* [C1] wake check
    /\ round' = TRUE
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, gcstopping, mutex,
                   fpc, naturalWon, everIL, everVanished>>

GcRoundEnd ==
    /\ round
    /\ round' = FALSE
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, gcstopping, mutex,
                   fpc, naturalWon, everIL, everVanished>>

\* [C1] self-exit at a wake that observes a Vanished pool (an in-flight
\* round finishes first; the exit joins nothing -- the threads return).
GcSelfExit ==
    /\ EnableGcSelfExit
    /\ gcsched = "Running" /\ pstate \in VanishedSet /\ ~round
    /\ gcsched' = "Stopped"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcstopping, round, mutex,
                   fpc, naturalWon, everIL, everVanished>>

\* SYSTEM CONTENT ADDRESSED GC STOP: lifecycle_mutex-serialized; stop-in-
\* place (scheduler retained, restartable); its stop() joins an in-flight
\* round (modeled as enabled-when-no-round). Works in ANY pool state.
VGcStop ==
    /\ mutex = "None" /\ gcsched = "Running" /\ ~round
    /\ gcsched' = "Stopped"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcstopping, round, mutex,
                   fpc, naturalWon, everIL, everVanished>>

\* SYSTEM CONTENT ADDRESSED GC START: goes through checkOpAdmitted(Admin)
\* -- refuses unless the pool is Live.
VGcStart ==
    /\ mutex = "None" /\ gcsched = "Stopped" /\ pstate = "Live"
    /\ gcsched' = "Running"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcstopping, round, mutex,
                   fpc, naturalWon, everIL, everVanished>>

(***************************************************************************)
(* FORGET (one instance; each step one atomic action, as-built order)      *)
(***************************************************************************)
FStart ==
    /\ fpc = "NotStarted" /\ mutex = "None"
    /\ mutex' = "Forget" /\ fpc' = "Check"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, gcstopping, round,
                   naturalWon, everIL, everVanished>>

\* Pool::forgetDisk's idempotent short-circuit on an already-terminal pool.
\* The metadata storage has already DETACHED the scheduler; the local's
\* dtor still stop()s it on return -- modeled as signal + join.
FCheckShort ==
    /\ fpc = "Check" /\ pstate \in VanishedSet
    /\ gcstopping' = TRUE /\ fpc' = "ShortJoin"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, round, mutex,
                   naturalWon, everIL, everVanished>>

FShortJoin ==
    /\ fpc = "ShortJoin" /\ ~round
    /\ gcsched' = "Destroyed" /\ fpc' = "Done" /\ mutex' = "None"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcstopping, round,
                   naturalWon, everIL, everVanished>>

FCheckProceed ==
    /\ fpc = "Check" /\ pstate \notin VanishedSet
    /\ fpc' = "Intent"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, gcstopping, round,
                   mutex, naturalWon, everIL, everVanished>>

\* (1) publish the terminal-intent latch FIRST (spec SS5).
FIntent ==
    /\ fpc = "Intent"
    /\ intent' = TRUE /\ fpc' = "Trip1"
    /\ UNCHANGED <<pstate, termPublished, lost, keeper, farewell, drained,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* (2) trip the fence -- the deliberate decommission act.
FTrip1 ==
    /\ fpc = "Trip1"
    /\ lost' = TRUE
    /\ pstate' = IF pstate = "Live" THEN "Transient" ELSE pstate
    /\ fpc' = "GcStopSignal"
    /\ UNCHANGED <<intent, termPublished, keeper, farewell, drained,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* (3+4) stop the GC scheduler: signal, then join (waits out a round).
FGcStopSignal ==
    /\ fpc = "GcStopSignal"
    /\ gcstopping' = TRUE /\ fpc' = "GcStopJoin"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, round, mutex,
                   naturalWon, everIL, everVanished>>

FGcStopJoin ==
    /\ fpc = "GcStopJoin" /\ ~round
    /\ gcsched' = "Destroyed" /\ fpc' = "JoinSignal"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* (5a) stopBackgroundWorkers: latch the shutdown gate, then join both workers.
FJoinSignal ==
    /\ fpc = "JoinSignal"
    /\ rshutdown' = TRUE /\ fpc' = "JoinWait"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

FJoinWait ==
    /\ fpc = "JoinWait" /\ rthread = "NotRunning"
    /\ fpc' = IF EnableTrip2 THEN "Trip2" ELSE "Drain"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   drained, rthread, rshutdown, gcsched, gcstopping, round,
                   mutex, naturalWon, everIL, everVanished>>

\* The SECOND tripMountLost: a reclaim that completed inside the join
\* window re-armed the fence; both runtime workers are now joined, so re-latch.
FTrip2 ==
    /\ fpc = "Trip2"
    /\ lost' = TRUE
    /\ pstate' = IF pstate = "Live" THEN "Transient" ELSE pstate
    /\ fpc' = "Drain"
    /\ UNCHANGED <<intent, termPublished, keeper, farewell, drained,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* (5b) drainRefLanesForShutdown: bounded, outcome nondeterministic.
FDrain ==
    /\ fpc = "Drain"
    /\ \E d \in {"Yes", "No"} : drained' = d
    /\ fpc' = "Teardown"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, keeper, farewell,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* (3+5c) finishTeardown: clean-release farewell ONLY if drained (fail-
\* closed otherwise: stop renewal with no terminal marker).
FTeardown ==
    /\ fpc = "Teardown"
    /\ IF keeper = "Running"
       THEN IF drained = "Yes" \/ ~DrainGate
            THEN keeper' = "StoppedClean" /\ farewell' = TRUE
            ELSE keeper' = "StoppedNoFarewell" /\ UNCHANGED farewell
       ELSE UNCHANGED <<keeper, farewell>>
    /\ fpc' = "KeeperReset"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, drained,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* Drop the keeper (single-shot terminal op; ~Pool must find none).
FKeeperReset ==
    /\ fpc = "KeeperReset"
    /\ keeper' = "None" /\ fpc' = "Enter"
    /\ UNCHANGED <<pstate, intent, termPublished, lost, farewell, drained,
                   rthread, rshutdown, gcsched, gcstopping, round, mutex,
                   naturalWon, everIL, everVanished>>

\* (6) enterVanished(Forgotten) under remount serialization; the FIRST
\* terminal transition wins (a raced natural promotion makes this a no-op).
FEnter ==
    /\ fpc = "Enter"
    /\ intent' = TRUE
    /\ IF termPublished
       THEN UNCHANGED <<pstate, termPublished, everVanished>>
       ELSE /\ pstate' = "VanishedForgotten" /\ termPublished' = TRUE
            /\ everVanished' = TRUE
    /\ fpc' = "Done" /\ mutex' = "None"
    /\ UNCHANGED <<lost, keeper, farewell, drained, rthread, rshutdown,
                   gcsched, gcstopping, round, naturalWon, everIL>>

(***************************************************************************)
Next ==
    \/ KTrip \/ RArm \/ RLoopExit \/ RAttemptBegin \/ RAttemptFail
    \/ RReclaim \/ REnterIL \/ RPromoteReplaced
    \/ GcRoundStart \/ GcRoundEnd \/ GcSelfExit \/ VGcStop \/ VGcStart
    \/ FStart \/ FCheckShort \/ FShortJoin \/ FCheckProceed \/ FIntent
    \/ FTrip1 \/ FGcStopSignal \/ FGcStopJoin \/ FJoinSignal \/ FJoinWait
    \/ FTrip2 \/ FDrain \/ FTeardown \/ FKeeperReset \/ FEnter

Spec == Init /\ [][Next]_vars

\* Fairness for the liveness checks: every FORGET step, the thread's exit
\* path, an attempt's resolution, round completion, and the [C1] self-exit
\* make progress. (FStart is deliberately NOT fair: FORGET is operator-
\* initiated.)
FairSpec ==
    /\ Spec
    /\ WF_vars(FCheckShort) /\ WF_vars(FShortJoin) /\ WF_vars(FCheckProceed)
    /\ WF_vars(FIntent) /\ WF_vars(FTrip1) /\ WF_vars(FGcStopSignal)
    /\ WF_vars(FGcStopJoin) /\ WF_vars(FJoinSignal) /\ WF_vars(FJoinWait)
    /\ WF_vars(FTrip2) /\ WF_vars(FDrain) /\ WF_vars(FTeardown)
    /\ WF_vars(FKeeperReset) /\ WF_vars(FEnter)
    /\ WF_vars(RLoopExit) /\ WF_vars(RAttemptFail) /\ WF_vars(GcRoundEnd)
    /\ WF_vars(GcSelfExit)

(***************************************************************************)
(* PROPERTIES                                                              *)
(***************************************************************************)
ForgetDone == fpc = "Done"

\* I1: FORGET-complete => fence latched + fully-terminal state.
I1ForgetTerminal == ForgetDone => (lost /\ pstate \in VanishedSet)

\* I1b: unless a natural Replaced transition won the race, Forgotten.
I1bForgetWinsUnlessNatural ==
    (ForgetDone /\ ~naturalWon) => pstate = "VanishedForgotten"

\* I2: no unearned clean farewell.
I2EarnedFarewell == farewell => drained = "Yes"

\* I3: one-way-ness.
I3OneWay ==
    /\ everIL => pstate \in ({"IdentityLost"} \cup VanishedSet)
    /\ everVanished => pstate \in VanishedSet

\* I4: the benign-answer gate (enabled exactly in the Vanished states)
\* never coexists with an armed write fence; transient/IL fail-loud too.
I4aTransientImpliesLost == pstate \in {"Transient", "IdentityLost"} => lost
I4bVanishedImpliesLost  == pstate \in VanishedSet => lost

\* I6: FORGET leaves GC destroyed; START's gate keeps it that way.
I6GcStoppedAfterForget == ForgetDone => gcsched = "Destroyed"

\* Liveness (FairSpec): a started FORGET always completes (I5: no stuck
\* half-terminal interleaving, including a racing natural promotion).
ForgetCompletes == (fpc = "Check") ~> ForgetDone

\* Liveness (FairSpec): the [C1] fix -- once Vanished, the GC scheduler
\* eventually stops ticking (self-exit, or FORGET's stop/destroy).
GcExitsAfterVanished == (pstate \in VanishedSet) ~> (gcsched # "Running")

\* Witnesses (expected VIOLATED in their configs = reachability).
WForgetNeverDone == ~ForgetDone
WNoRacedReplaced == ~(ForgetDone /\ pstate = "VanishedReplaced")
WNoJoinWindowReclaim == ~(fpc = "Trip2" /\ pstate = "Live" /\ ~lost)

=============================================================================
