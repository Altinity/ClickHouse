--------------------------- MODULE CaErasureProof ---------------------------
(***************************************************************************)
(* TLA+ model of the rev.7 `Vanished(erased)` erasure-proof soundness      *)
(* (spec 2026-07-22 "throw-when-uncertain" SS2 [C2][C3][D1], as built by   *)
(* Tasks 4/5/6/8 on `cas-gc-rebuild`).                                     *)
(*                                                                         *)
(* CROWN PROPERTY (TruthEmpty): when the observer promotes the pool to    *)
(* `VanishedErased` ("verified: pool prefix empty"), the pool prefix IS    *)
(* empty and STAYS empty -- no admitted durable write, no zombie request,  *)
(* and no GC control write ever lands under the prefix at or after the     *)
(* promotion. A violation is a false benign-truth conversion: a disk that  *)
(* answers truth-empty while objects exist under its prefix.               *)
(*                                                                         *)
(* Actors (each TLA action = one atomic step; interleaving is free):       *)
(*  - WRITERS (2): admit (op-gate: `Live` only; capture fence generation;  *)
(*    `DurableRequestGuard`++), recheck (`checkFenceOrThrow`: ~lost AND    *)
(*    generation unchanged) -> issue, then EITHER land+resolve OR abandon  *)
(*    (guard released on a timeout while the issued request is still in    *)
(*    flight = a ZOMBIE that may land arbitrarily later). The TOCTOU is    *)
(*    honest: the recheck precedes the land; an issued write lands at any  *)
(*    later step. The guard is op-scoped (admission..resolution).          *)
(*  - KEEPER: trips the fence (lost:=TRUE, generation++,                   *)
(*    Live->TransientNotLive).                                             *)
(*  - RECLAIM (the remount tick, verdict `Recover`): needs `_pool_meta`    *)
(*    present, `TransientNotLive`, no terminal intent; re-arms the fence   *)
(*    (lost:=FALSE, generation++) and returns to `Live`; resets the        *)
(*    erasure streak (`resetErasureProof` on verdict `Recover`). Atomic    *)
(*    here because the arm..noteRemounted window is unobservable: the op   *)
(*    gate refuses admission while `TransientNotLive` and every older      *)
(*    admitted generation fails its recheck (see fidelity notes).          *)
(*  - GC: rounds set `round_in_flight` for the whole body; a round's       *)
(*    acquire either RENEWS a present `gc/state`, or -- when absent --     *)
(*    THROWS if `has_observation` is latched (failed round, leadership     *)
(*    hint cleared by the loop catch) or CREATES a fresh `gc/state` (a     *)
(*    durable write INTO the prefix) if never latched (a fresh scheduler:  *)
(*    the normal first-round path of a new pool). The heartbeat loop       *)
(*    pulses `gc/hb` (create-if-absent!) whenever `i_am_leader`, OUTSIDE   *)
(*    any round (EnableHbOutsideRound models the real `heartbeatLoop`).    *)
(*  - ERASER (environment): deletes objects in ANY order (progressive      *)
(*    `rm -rf`; may delete sentinels first). RESTORE (backup) may bring    *)
(*    `_pool_meta` back between observer ticks (mid-sample restores are    *)
(*    [A1] out-of-contract erase-and-recreate; see fidelity notes).        *)
(*  - OBSERVER (the demoted remount tick): a sample is SPLIT into its two  *)
(*    non-atomic halves exactly as the code orders them under              *)
(*    `remount_mutex`: (1) the gate's authoritative empty-LIST             *)
(*    (`SentinelsGoneEmptyPrefix`), then (2) the qualification reads of    *)
(*    `evaluateErasureProofEmptySample` (guard counter == 0, GC quiescent  *)
(*    = no round in flight, grace) and the streak/promote. Writers, the    *)
(*    keeper, GC and the eraser interleave BETWEEN the two halves -- the   *)
(*    load-bearing non-atomicity this model exists to probe. Two counted   *)
(*    samples must be spaced by a Tick (>= renewal period); any failed     *)
(*    qualification or non-empty observation RESETS the streak; a         *)
(*    too-soon qualifying sample neither advances nor resets.              *)
(*                                                                         *)
(* GRACE ([D1]) is modeled per its intent, not as wall time: the           *)
(* qualification requires that no ZOMBIE (a request issued under the       *)
(* dying incarnation whose guard was already released) is still in flight  *)
(* -- precisely what "elapsed-since-trip >= the backend's total            *)
(* per-operation budget" buys. All zombies are issued strictly before the  *)
(* last trip (issues need ~lost, admits need Live), so "no zombie in       *)
(* flight" == "no write admitted before the last trip remains              *)
(* unresolved". GraceOn=FALSE removes the condition -- the sabotage that   *)
(* answers whether grace is load-bearing.                                  *)
(*                                                                         *)
(* CONFIGS                                                                 *)
(*  - _nogc_grace       : writers only, grace ON            -> MUST HOLD   *)
(*  - _sab_nograce      : writers only, grace OFF           -> VIOLATES    *)
(*    (zombie lands after promotion => grace IS load-bearing).             *)
(*  - _gc_promptliteral : GC per the T8 prose (hb only inside rounds,      *)
(*    no new round after terminal intent), grace ON         -> VIOLATES:   *)
(*    a fresh (never-observed) scheduler's round CREATES `gc/state`        *)
(*    between the observer's final LIST and its round_in_flight read,      *)
(*    and COMPLETES before that read -- `round_in_flight`-at-sample does   *)
(*    NOT close the completed-round window mechanically.                   *)
(*  - _gc_asbuilt       : + the real out-of-round heartbeat pulses and no  *)
(*    scheduler exit on the terminal transition             -> VIOLATES    *)
(*    (more paths: hb pulse in the LIST..qual window; post-promotion       *)
(*    round/hb writes recreate control keys under a VanishedErased pool).  *)
(*  - _fix_gclivegate   : candidate fix -- scheduler rounds AND heartbeat  *)
(*    pulses refuse unless the pool lifecycle is `Live` (the same refusal  *)
(*    the manual GC entry points already make via checkOpAdmitted(Admin))  *)
(*                                                          -> MUST HOLD.  *)
(*  - _witness_promote  : non-vacuity -- promotion is reachable.           *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    EnableGc,               \* model the GC scheduler at all
    GraceOn,                \* [D1] grace precondition at each qualification
    GcStopsOnIntent,        \* no NEW round once terminal intent is published (spec SS3)
    EnableHbOutsideRound,   \* the real heartbeatLoop: leader pulses outside rounds
    GcLiveGate,             \* candidate fix: rounds + hb pulses only while Live
    MaxGen                  \* fence-generation bound (trips + re-arms)

Writers == {"w1", "w2"}
Meta    == "meta"           \* `_pool_meta` + owner anchor, collapsed to one sentinel
GState  == "gstate"         \* the `gc/state` control object
GHb     == "ghb"            \* the `gc/hb` heartbeat object
Objects == Writers \cup {Meta, GState, GHb}

States == {"Live", "Transient", "IdentityLost", "VanishedErased"}

VARIABLES
    prefix,     \* SUBSET Objects: what a full-prefix LIST returns
    mstate,     \* pool lifecycle condition
    lost,       \* MountFence.lost (mayMutate == ~lost; deadline abstracted)
    gen,        \* fence_generation (bumped by trip AND re-arm)
    intent,     \* vanished_intent (terminal-intent latch)
    wpc,        \* [Writers -> {"Idle","Admitted","Issued"}]
    wgen,       \* [Writers -> 0..MaxGen]: generation captured at admission
    zombie,     \* [Writers -> BOOLEAN]: guard released, request still in flight
    gcRound,    \* CasGcScheduler.round_in_flight
    gcHasObs,   \* Gc.has_observation (latched by any successful acquire/renew)
    gcLeader,   \* CasGcScheduler.i_am_leader (in-process hint)
    opc,        \* observer tick pc: "Idle" | "Listed" (LIST taken, quals pending)
    streak,     \* erasure_empty_samples
    spaced      \* >= one renewal period elapsed since the last counted sample

vars == <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
          gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

Pending(w)  == wpc[w] \in {"Admitted", "Issued"}
CounterVal  == Cardinality({w \in Writers : Pending(w)})   \* outstanding_durable_requests
NoZombies   == \A w \in Writers : ~zombie[w]
Promoted    == mstate = "VanishedErased"
Observing   == mstate \in {"Transient", "IdentityLost"}

TypeOK ==
    /\ prefix \subseteq Objects
    /\ mstate \in States
    /\ lost \in BOOLEAN
    /\ gen \in 0..MaxGen
    /\ intent \in BOOLEAN
    /\ wpc \in [Writers -> {"Idle", "Admitted", "Issued"}]
    /\ wgen \in [Writers -> 0..MaxGen]
    /\ zombie \in [Writers -> BOOLEAN]
    /\ gcRound \in BOOLEAN
    /\ gcHasObs \in BOOLEAN
    /\ gcLeader \in BOOLEAN
    /\ opc \in {"Idle", "Listed"}
    /\ streak \in 0..2
    /\ spaced \in BOOLEAN

Init ==
    \* Two starts: a pool whose GC never ran (no gc/state -- the fresh-scheduler
    \* create path is live) and one with a pre-existing gc/state.
    /\ prefix \in { {Meta}, {Meta, GState} }
    /\ mstate = "Live" /\ lost = FALSE /\ gen = 0 /\ intent = FALSE
    /\ wpc = [w \in Writers |-> "Idle"]
    /\ wgen = [w \in Writers |-> 0]
    /\ zombie = [w \in Writers |-> FALSE]
    /\ gcRound = FALSE /\ gcHasObs = FALSE /\ gcLeader = FALSE
    /\ opc = "Idle" /\ streak = 0 /\ spaced = FALSE

(***************************************************************************)
(* KEEPER                                                                  *)
(***************************************************************************)
\* tripMountLost: latch the fence, bump the generation, Live->Transient.
KeeperTrip ==
    /\ gen < MaxGen
    /\ lost' = TRUE
    /\ gen' = gen + 1
    /\ mstate' = IF mstate = "Live" THEN "Transient" ELSE mstate
    /\ UNCHANGED <<prefix, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

(***************************************************************************)
(* REMOUNT TICKS (one thread; each tick is one gate probe + dispatch,      *)
(* serialized under remount_mutex -- hence opc = "Idle" guards: no tick    *)
(* interleaves inside another tick's LIST..qual span)                      *)
(***************************************************************************)
\* Verdict Recover -> full reclaim: arm the fence under a fresh generation,
\* back to Live; the verdict resets any accrued erasure streak.
Reclaim ==
    /\ opc = "Idle" /\ mstate = "Transient" /\ ~intent
    /\ Meta \in prefix /\ gen < MaxGen
    /\ lost' = FALSE /\ gen' = gen + 1 /\ mstate' = "Live" /\ streak' = 0
    /\ UNCHANGED <<prefix, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, spaced>>

\* Verdict IdentityLost: sentinels gone, prefix non-empty; streak reset.
EnterIL ==
    /\ opc = "Idle" /\ mstate = "Transient"
    /\ Meta \notin prefix /\ prefix # {}
    /\ mstate' = "IdentityLost" /\ streak' = 0
    /\ UNCHANGED <<prefix, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, spaced>>

\* Any tick whose gate sees a NON-empty prefix (verdict Recover /
\* IdentityLost-repeat / StayTransient) resets the streak.
ObsReset ==
    /\ opc = "Idle" /\ Observing /\ prefix # {} /\ streak > 0
    /\ streak' = 0
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, spaced>>

\* Sample half 1: the gate's authoritative empty-LIST (verdict
\* SentinelsGoneEmptyPrefix). The qualification reads follow NON-atomically.
ObsList ==
    /\ opc = "Idle" /\ Observing /\ ~intent /\ prefix = {}
    /\ opc' = "Listed"
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, streak, spaced>>

\* [D1] qualification at THIS sample, in code order after the LIST:
\* durable-guard counter == 0, GC quiescent (no round in flight), grace.
\* (Capability [C3] is assumed granted; ref-lanes-settled is subsumed under
\* the counter + grace abstraction -- see the fidelity notes.)
QualOK == CounterVal = 0 /\ ~gcRound /\ (~GraceOn \/ NoZombies)

ObsQualFail ==
    /\ opc = "Listed" /\ ~QualOK
    /\ opc' = "Idle" /\ streak' = 0
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, spaced>>

\* A qualifying sample inside one renewal period of the last counted one:
\* ignored -- the streak neither advances nor resets.
ObsQualTooSoon ==
    /\ opc = "Listed" /\ QualOK /\ streak >= 1 /\ ~spaced
    /\ opc' = "Idle"
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, streak, spaced>>

\* A counted sample; the second one promotes (enterVanished(VanishedErased)
\* publishes the terminal-intent latch with the state).
ObsQualCount ==
    /\ opc = "Listed" /\ QualOK /\ (streak = 0 \/ spaced)
    /\ streak' = streak + 1 /\ spaced' = FALSE /\ opc' = "Idle"
    /\ IF streak + 1 >= 2
       THEN mstate' = "VanishedErased" /\ intent' = TRUE
       ELSE UNCHANGED <<mstate, intent>>
    /\ UNCHANGED <<prefix, lost, gen, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader>>

\* One renewal period elapses.
Tick ==
    /\ ~spaced
    /\ spaced' = TRUE
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak>>

(***************************************************************************)
(* WRITERS (durable-effect operations, rev.7 [C2]/[D1])                    *)
(***************************************************************************)
\* Admission: op gate admits Write class only while Live; the guard
\* (counter) is held from here to resolution; the fence generation is
\* captured once.
Admit(w) ==
    /\ wpc[w] = "Idle" /\ mstate = "Live"
    /\ wpc' = [wpc EXCEPT ![w] = "Admitted"]
    /\ wgen' = [wgen EXCEPT ![w] = gen]
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

\* checkFenceOrThrow immediately before the durable call: pass -> issued.
RecheckIssue(w) ==
    /\ wpc[w] = "Admitted" /\ ~lost /\ wgen[w] = gen
    /\ wpc' = [wpc EXCEPT ![w] = "Issued"]
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

\* Recheck fails: typed abort; the guard resolves without a durable write.
RecheckAbort(w) ==
    /\ wpc[w] = "Admitted" /\ (lost \/ wgen[w] # gen)
    /\ wpc' = [wpc EXCEPT ![w] = "Idle"]
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

\* The issued request lands (object durable under the prefix) and the op
\* resolves. Arbitrarily later than the recheck -- the honest TOCTOU.
LandResolve(w) ==
    /\ wpc[w] = "Issued"
    /\ prefix' = prefix \cup {w}
    /\ wpc' = [wpc EXCEPT ![w] = "Idle"]
    /\ UNCHANGED <<mstate, lost, gen, intent, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

\* The op gives up (timeout/deadline): the guard resolves, but the issued
\* request is STILL in flight -- a zombie the counter can no longer see.
\* This is the residual window [D1]'s grace exists to cover.
AbandonZombie(w) ==
    /\ wpc[w] = "Issued"
    /\ wpc' = [wpc EXCEPT ![w] = "Idle"]
    /\ zombie' = [zombie EXCEPT ![w] = TRUE]
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wgen,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

ZombieLand(w) ==
    /\ zombie[w]
    /\ prefix' = prefix \cup {w}
    /\ zombie' = [zombie EXCEPT ![w] = FALSE]
    /\ UNCHANGED <<mstate, lost, gen, intent, wpc, wgen,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

ZombieExpire(w) ==
    /\ zombie[w]
    /\ zombie' = [zombie EXCEPT ![w] = FALSE]
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

(***************************************************************************)
(* GC (own lease -- NOT fence-generation-checked, NOT guard-counted)       *)
(***************************************************************************)
\* A scheduler tick starts a round (round_in_flight for the whole body).
GcRoundStart ==
    /\ EnableGc /\ ~gcRound
    /\ (GcLiveGate => mstate = "Live")
    /\ (GcStopsOnIntent => ~intent)
    /\ gcRound' = TRUE
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcHasObs, gcLeader, opc, streak, spaced>>

\* acquireOrRenewLease, gc/state ABSENT, has_observation never latched: the
\* fresh-scheduler first-acquire path CREATES gc/state -- a durable write
\* into the prefix (CasGc.cpp:2436).
GcAcquireCreate ==
    /\ gcRound /\ GState \notin prefix /\ ~gcHasObs
    /\ prefix' = prefix \cup {GState}
    /\ gcHasObs' = TRUE /\ gcLeader' = TRUE
    /\ UNCHANGED <<mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, opc, streak, spaced>>

\* acquireOrRenewLease, gc/state present: renew in place (rewrite; the key
\* set is unchanged), latch the observation, leadership hint on.
GcAcquireRenew ==
    /\ gcRound /\ GState \in prefix
    /\ gcHasObs' = TRUE /\ gcLeader' = TRUE
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, opc, streak, spaced>>

\* gc/state absent AFTER an observation: the has_observation guard throws
\* CORRUPTED_DATA (CasGc.cpp:2432) -> failed round; the scheduler loop's
\* catch clears i_am_leader.
GcRoundFailObs ==
    /\ gcRound /\ GState \notin prefix /\ gcHasObs
    /\ gcRound' = FALSE /\ gcLeader' = FALSE
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcHasObs, opc, streak, spaced>>

\* The round body completes (empty pool => short round) or ends NotALeader.
GcRoundEnd ==
    /\ gcRound
    /\ gcRound' = FALSE
    /\ UNCHANGED <<prefix, mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcHasObs, gcLeader, opc, streak, spaced>>

\* onLeaseAcquired's advisory pulse inside the round: gc/hb create-or-bump.
GcHbInRound ==
    /\ gcRound /\ gcLeader
    /\ prefix' = prefix \cup {GHb}
    /\ UNCHANGED <<mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

\* The REAL heartbeatLoop: pulses whenever i_am_leader, OUTSIDE any round
\* (pulseHeartbeat creates gc/hb if absent -- expected token nullopt).
GcHbPulse ==
    /\ EnableGc /\ EnableHbOutsideRound /\ gcLeader
    /\ (GcLiveGate => mstate = "Live")
    /\ prefix' = prefix \cup {GHb}
    /\ UNCHANGED <<mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

(***************************************************************************)
(* ENVIRONMENT                                                             *)
(***************************************************************************)
\* Progressive rm -rf: any object, any order (sentinels first included).
Erase ==
    /\ \E o \in prefix : prefix' = prefix \ {o}
    /\ UNCHANGED <<mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

\* A backup restore brings the sentinel back BETWEEN observer ticks (a
\* restore racing the inside of one sample, or landing after the terminal
\* transition, is [A1] out-of-contract erase-and-recreate -- scoped out).
Restore ==
    /\ Meta \notin prefix /\ ~Promoted /\ opc = "Idle"
    /\ prefix' = prefix \cup {Meta}
    /\ UNCHANGED <<mstate, lost, gen, intent, wpc, wgen, zombie,
                   gcRound, gcHasObs, gcLeader, opc, streak, spaced>>

(***************************************************************************)
Next ==
    \/ KeeperTrip \/ Reclaim \/ EnterIL
    \/ ObsReset \/ ObsList \/ ObsQualFail \/ ObsQualTooSoon \/ ObsQualCount
    \/ Tick \/ Erase \/ Restore
    \/ \E w \in Writers :
         Admit(w) \/ RecheckIssue(w) \/ RecheckAbort(w)
         \/ LandResolve(w) \/ AbandonZombie(w) \/ ZombieExpire(w)
         \/ ZombieLand(w)
    \/ GcRoundStart \/ GcAcquireCreate \/ GcAcquireRenew
    \/ GcRoundFailObs \/ GcRoundEnd \/ GcHbInRound \/ GcHbPulse

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* PROPERTIES                                                              *)
(***************************************************************************)
\* The crown: VanishedErased means the prefix is empty -- at the promotion
\* and forever after (a later land while Promoted violates it too).
TruthEmpty == Promoted => prefix = {}

\* Witness (non-vacuity): promotion must be reachable at all; a config
\* asserting this as an invariant must report a violation.
NeverPromoted == ~Promoted

=============================================================================
