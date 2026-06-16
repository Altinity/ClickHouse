-------------------------- MODULE CaGcLeaseCore --------------------------
(***************************************************************************)
(* TLA+ model of the CA GC leader lease + advisory heartbeat (B160).       *)
(*                                                                         *)
(* Spec: docs/superpowers/specs/2026-06-16-ca-gc-lease-heartbeat-design.md *)
(*                                                                         *)
(* The lease is a clock-free observation-window steal on ONE CAS register  *)
(* `state` = [owner, seq, fence]:                                          *)
(*   - owner==me           => RENEW (seq++) and run a round.               *)
(*   - foreign, (owner,seq) CHANGED since my recorded observation => alive *)
(*     => back off.                                                        *)
(*   - foreign, (owner,seq) IDENTICAL to my recorded observation (frozen)  *)
(*     => the incumbent looks dead => STEAL (one atomic CAS: owner:=me,    *)
(*        seq++, fence++).                                                 *)
(* The leader bumps `seq` only ONCE PER ROUND, but a round spans many      *)
(* steps (fold/retire/...), so a slow-but-alive leader's seq looks frozen  *)
(* across a follower's window => the follower FALSELY steals (B160).       *)
(*                                                                         *)
(* The fix adds a second register `hb` = [owner, hbseq] that the leader    *)
(* bumps on a FAST cadence (H), independent of round progress. A follower  *)
(* steals only if BOTH (owner,seq) AND hb are frozen across its window.    *)
(*                                                                         *)
(* TUNING (H <= W) is encoded structurally: a global `clock` Tick advances *)
(* a window AND atomically bumps the heartbeat of every alive leader (when *)
(* EnableHeartbeat). So an alive leader's hb necessarily advances within a *)
(* follower's window; a dead leader's hb freezes.                          *)
(*                                                                         *)
(* RESULTS we check:                                                       *)
(*  - NoEpochCollision (SAFETY): no two distinct actors ever commit a      *)
(*    retire at the same fence epoch. Holds for ALL configs (heartbeat ON  *)
(*    or OFF) — the atomic single-CAS steal + fence epoch isolation make   *)
(*    every steal safe regardless of timing. This is the correctness       *)
(*    guarantee; the heartbeat must NOT break it.                          *)
(*  - NoFalseSteal (EFFICIENCY): no steal ever fires against an alive,     *)
(*    mid-round incumbent. Holds with EnableHeartbeat=TRUE; VIOLATED with  *)
(*    EnableHeartbeat=FALSE (the SabotageNoHeartbeat config) — the B160     *)
(*    false-steal, reproduced.                                            *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Actors,            \* GC leader identities, e.g. {L1, L2}
    None,              \* "no owner" sentinel (a model value, distinct from any Actor)
    EnableHeartbeat,   \* TRUE = the fix; FALSE = SabotageNoHeartbeat (reproduces B160)
    MaxClock,          \* bound on the global window counter (TLC finiteness)
    MaxSeq,            \* bound on lease/hb seq
    MaxFence           \* bound on fence_seq

NoObs == [seen |-> FALSE]             \* a follower with no recorded observation

VARIABLES
    clock,        \* global window counter; a Tick is one observation window
    stOwner,      \* state.owner   (the lease owner; None if never held)
    stSeq,        \* state.seq     (renewal counter — bumps once per round)
    stFence,      \* state.fence   (fence_seq — the leadership epoch; a steal bumps it)
    hbOwner,      \* hb.owner
    hbSeq,        \* hb.hbseq      (heartbeat pulse)
    inRound,      \* [Actors -> BOOLEAN]   is this actor executing a round
    roundFence,   \* [Actors -> Nat]       the fence its in-flight round committed to at start
    alive,        \* [Actors -> BOOLEAN]   process liveness (a dead leader stops heartbeating)
    obs,          \* [Actors -> record]    a follower's last observation of (owner,seq,hbOwner,hbSeq,clock)
    committed,    \* set of [actor |-> a, fence |-> f] retire commits (for the safety invariant)
    falseSteal    \* TRUE once a steal fired against an alive, mid-round incumbent (B160 witness)

vars == << clock, stOwner, stSeq, stFence, hbOwner, hbSeq, inRound, roundFence,
           alive, obs, committed, falseSteal >>

Init ==
    /\ clock = 0
    /\ stOwner = None
    /\ stSeq = 0
    /\ stFence = 0
    /\ hbOwner = None
    /\ hbSeq = 0
    /\ inRound = [a \in Actors |-> FALSE]
    /\ roundFence = [a \in Actors |-> 0]
    /\ alive = [a \in Actors |-> TRUE]
    /\ obs = [a \in Actors |-> NoObs]
    /\ committed = {}
    /\ falseSteal = FALSE

\* Record actor a's observation of the two registers at the current clock.
RecordObs(a) ==
    obs' = [obs EXCEPT ![a] = [ seen |-> TRUE, owner |-> stOwner, seq |-> stSeq,
                                hbOwner |-> hbOwner, hbSeq |-> hbSeq, clk |-> clock ]]

----------------------------------------------------------------------------
\* A window passes. For every alive leader, the heartbeat advances (when enabled).
\* This is the structural H <= W encoding: within ANY follower window, an alive
\* leader has heartbeated, while a dead leader's hb stays frozen.
Tick ==
    /\ clock < MaxClock
    /\ hbSeq < MaxSeq
    /\ clock' = clock + 1
    /\ IF EnableHeartbeat /\ stOwner # None /\ alive[stOwner]
       THEN /\ hbOwner' = stOwner
            /\ hbSeq' = hbSeq + 1
       ELSE /\ UNCHANGED hbOwner
            /\ UNCHANGED hbSeq
    /\ UNCHANGED << stOwner, stSeq, stFence, inRound, roundFence, alive, obs, committed, falseSteal >>

\* Fresh pool: create the lease if absent (create-if-absent CAS).
Create(a) ==
    /\ alive[a]
    /\ stOwner = None
    /\ stSeq < MaxSeq
    /\ stOwner' = a
    /\ stSeq' = stSeq + 1
    /\ inRound' = [inRound EXCEPT ![a] = TRUE]
    /\ roundFence' = [roundFence EXCEPT ![a] = stFence]
    /\ UNCHANGED << clock, stFence, hbOwner, hbSeq, alive, obs, committed, falseSteal >>

\* Owner renews and starts a NEW round (seq bumps once here, then is frozen for the round).
Renew(a) ==
    /\ alive[a]
    /\ stOwner = a
    /\ ~inRound[a]
    /\ stSeq < MaxSeq
    /\ stSeq' = stSeq + 1
    /\ inRound' = [inRound EXCEPT ![a] = TRUE]
    /\ roundFence' = [roundFence EXCEPT ![a] = stFence]
    /\ UNCHANGED << clock, stOwner, stFence, hbOwner, hbSeq, alive, obs, committed, falseSteal >>

\* The round's terminal retire CAS: it COMMITS iff this actor is still the owner at the SAME
\* fence epoch it began under (the atomic gc/state CAS). A displaced leader (its fence was bumped
\* by a steal, or owner changed) is BLOCKED — fail-closed, no commit (the zombie-steal protection).
Retire(a) ==
    /\ alive[a]
    /\ inRound[a]
    /\ inRound' = [inRound EXCEPT ![a] = FALSE]
    /\ IF stOwner = a /\ stFence = roundFence[a]
       THEN committed' = committed \union {[actor |-> a, fence |-> stFence]}
       ELSE committed' = committed   \* displaced -> wasted round, never commits
    /\ UNCHANGED << clock, stOwner, stSeq, stFence, hbOwner, hbSeq, roundFence, alive, obs, falseSteal >>

\* A follower observes / decides. Two phases, gated by a window having elapsed (clock advanced)
\* since the recorded observation — so a steal can only follow a full window, never the same instant.
ObserveOrSteal(a) ==
    /\ alive[a]
    /\ stOwner # a
    /\ stOwner # None
    /\ IF (~obs[a].seen) \/ (obs[a].clk = clock)
       THEN \* first sight, or no window elapsed yet: just (re)record and back off
            /\ RecordObs(a)
            /\ UNCHANGED << clock, stOwner, stSeq, stFence, hbOwner, hbSeq, inRound,
                            roundFence, alive, committed, falseSteal >>
       ELSE \* a full window elapsed since the recorded observation -> decide
            LET stateChanged == (obs[a].owner # stOwner) \/ (obs[a].seq # stSeq)
                hbAdvanced    == EnableHeartbeat /\ (hbOwner = stOwner) /\ (hbSeq > obs[a].hbSeq)
                incumbentAlive == stateChanged \/ hbAdvanced
            IN IF incumbentAlive
               THEN \* alive (renewed, or heartbeat advanced) -> re-record, back off
                    /\ RecordObs(a)
                    /\ UNCHANGED << clock, stOwner, stSeq, stFence, hbOwner, hbSeq, inRound,
                                    roundFence, alive, committed, falseSteal >>
               ELSE \* both frozen across the window -> STEAL (one atomic CAS on state)
                    /\ stFence < MaxFence /\ stSeq < MaxSeq
                    /\ stOwner' = a
                    /\ stSeq' = stSeq + 1
                    /\ stFence' = stFence + 1
                    /\ obs' = [obs EXCEPT ![a] = NoObs]
                    \* B160 witness: did we just steal from an alive, mid-round incumbent?
                    /\ falseSteal' = (falseSteal \/ (alive[stOwner] /\ inRound[stOwner]))
                    /\ UNCHANGED << clock, hbOwner, hbSeq, inRound, roundFence, alive, committed >>

\* A leader's process dies (stops heartbeating + can no longer retire) -> a LEGITIMATE steal target.
Die(a) ==
    /\ alive[a]
    /\ stOwner = a            \* only the leader dying is interesting (it must be failed over)
    /\ alive' = [alive EXCEPT ![a] = FALSE]
    /\ UNCHANGED << clock, stOwner, stSeq, stFence, hbOwner, hbSeq, inRound, roundFence, obs, committed, falseSteal >>

Next ==
    \/ Tick
    \/ \E a \in Actors : Create(a)
    \/ \E a \in Actors : Renew(a)
    \/ \E a \in Actors : Retire(a)
    \/ \E a \in Actors : ObserveOrSteal(a)
    \/ \E a \in Actors : Die(a)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* SAFETY: epoch isolation. No two DISTINCT actors ever commit a retire at the same fence epoch.
\* (A displaced leader's retire is blocked, so a stolen epoch is never double-committed.)
\* Must hold in EVERY config — the heartbeat must not weaken it.
NoEpochCollision ==
    \A c1 \in committed : \A c2 \in committed :
        (c1.fence = c2.fence) => (c1.actor = c2.actor)

\* EFFICIENCY (B160): no steal ever fires against an alive, mid-round incumbent.
\* Holds with EnableHeartbeat=TRUE; VIOLATED under SabotageNoHeartbeat (EnableHeartbeat=FALSE).
NoFalseSteal == ~falseSteal

\* Type/bounds sanity for TLC.
TypeOK ==
    /\ clock \in 0..MaxClock
    /\ stSeq \in 0..MaxSeq
    /\ stFence \in 0..MaxFence
    /\ hbSeq \in 0..MaxSeq
    /\ stOwner \in Actors \union {None}
    /\ hbOwner \in Actors \union {None}
=============================================================================
