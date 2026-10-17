------------------------- MODULE CaMountObserveCore -------------------------
(***************************************************************************)
(* Observer timing of the CAS mount lease: one holder, one observer, one   *)
(* mount slot.                                                             *)
(*                                                                         *)
(* The holder renews by a conditional write. A renewal fixes its start on  *)
(* the holder's clock before the first request, and the holder may write   *)
(* while its clock is below `start + AuthoritySpan` of the last renewal it *)
(* saw succeed. The observer (a GC round, or a server re-mounting the      *)
(* slot) never reads the holder's clock. It watches the slot token on its  *)
(* own clock and fences by a write conditional on that token once the      *)
(* token stayed unchanged for `Threshold`.                                 *)
(*                                                                         *)
(* The two clocks are separate variables and nothing compares them. Their  *)
(* only coupling is a bounded rate difference (`GainEvery`).               *)
(*                                                                         *)
(* Code: `computeHeartbeatFloor`, `claimMountAwaitingExpiry` and           *)
(* `mountObservationThresholdMs` in `Pool/CasServerRoot.cpp`;              *)
(* `CasMountRuntime::admit` and `mayMutate` in `Pool/CasMountRuntime.cpp`. *)
(*                                                                         *)
(* `CaCasMountCore.GcFence` abstracts all of this into one guard,          *)
(* `mount.deadline + Drift <= clock`. `NoFenceInsideAuthority` here is     *)
(* what justifies that guard.                                              *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    None,
    TTL,            \* lease length on the holder's clock (`cas_mount_lease_ttl_ms`)
    Period,         \* renewal cadence on the holder's clock (`cas_mount_renew_period_ms`)
    Allowance,      \* drift term of the threshold (`TTL / 20` in `mountObservationThresholdMs`)
    Cadence,        \* cadence term of the threshold: `Period` for GC, `Period / 2` for a re-mount
    AuthoritySpan,  \* authority lasts this long after the start of the last acknowledged renewal.
                    \* `TTL` when the holder keeps the lease until it expires; smaller when a
                    \* holder with failing renewals fences itself early.
    GainEvery,      \* rate bound: per `GainEvery` joint ticks one clock may gain one tick on the
                    \* other, and one such tick may be banked. 0 = the clocks run at one rate.
    MaxClock,       \* horizon of both clocks
    MaxWalk,        \* a read happens at most this many observer ticks after the sample it is
                    \* decided with. `MaxClock` = no bound.
    Healthy,        \* TRUE: every renewal starts one `Period` after the previous start and is
                    \* acknowledged within `MaxLatency`. FALSE: renewals may stall or stop.
    MaxLatency,
    MinMargin,      \* `FenceMarginAtLeast`: holder ticks between the end of authority and the fence
    SabSightingBeforeRead,     \* a first sighting is dated with the sample taken before its read
    SabRedecisionStaleSample,  \* a sighting made after a refused fence is dated with that sample
    SabWrappingStability       \* the stability test wraps when its sample precedes the sighting

ASSUME
    /\ TTL \in Nat \ {0} /\ Period \in Nat \ {0}
    /\ Allowance \in Nat /\ Cadence \in Nat /\ GainEvery \in Nat
    /\ AuthoritySpan \in 1..TTL
    /\ MaxClock \in Nat /\ MaxWalk \in Nat /\ MaxLatency \in Nat /\ MinMargin \in Int
    /\ Healthy \in BOOLEAN
    /\ SabSightingBeforeRead \in BOOLEAN
    /\ SabRedecisionStaleSample \in BOOLEAN
    /\ SabWrappingStability \in BOOLEAN

VARIABLES
    hclock,          \* the holder's clock
    oclock,          \* the observer's clock
    credit,          \* joint ticks since the last solo tick, capped at `GainEvery`
    slotToken,       \* the store's token of the mount object; every landed write changes it
    slotFenced,      \* the observer's conditional write landed
    lastStart,       \* start of the last renewal the holder saw succeed
    rn,              \* the renewal in flight: None, or [start, landed]
    lost,            \* the holder read the fenced slot
    opc,             \* observer: "idle", "read", "fencing", "refused", "reread"
    roundSample,     \* the observer's clock sample taken before its reads; None before the first
    seenToken,       \* the token the last read returned; None when no decision is pending
    watchToken,      \* the token under observation; None when there is none
    watchSince,      \* the observer's date of the first sighting of `watchToken`
    usedLastInstant, \* history: the holder wrote at the last tick of its authority while its
                     \* renewal was still unresolved
    wroteAfterFence, \* history: the holder admitted a write after the fence landed
    redecided        \* history: a fence was refused and the slot was decided again

vars == << hclock, oclock, credit, slotToken, slotFenced, lastStart, rn, lost, opc, roundSample,
           seenToken, watchToken, watchSince, usedLastInstant, wroteAfterFence, redecided >>

holderVars   == << slotToken, slotFenced, lastStart, rn, lost, usedLastInstant, wroteAfterFence >>
observerVars == << opc, roundSample, seenToken, watchToken, watchSince, redecided >>
clockVars    == << hclock, oclock, credit >>

Min(a, b) == IF a < b THEN a ELSE b

Threshold      == TTL + Allowance + Cadence
AuthorityEnd   == lastStart + AuthoritySpan
RenewalFailing == rn # None /\ ~rn.landed

Init ==
    /\ hclock = 0 /\ oclock = 0
    /\ credit = GainEvery
    /\ slotToken = 1 /\ slotFenced = FALSE
    /\ lastStart = 0 /\ rn = None /\ lost = FALSE
    /\ opc = "idle" /\ roundSample = None /\ seenToken = None
    /\ watchToken = None /\ watchSince = 0
    /\ usedLastInstant = FALSE /\ wroteAfterFence = FALSE /\ redecided = FALSE

----------------------------------------------------------------------------
\* Clocks. They stop once the fence landed: nothing after that depends on time except `Write`,
\* whose guard only gets weaker as `hclock` grows.

\* A healthy holder cannot let its clock pass a renewal that is due or one that is overdue.
HolderMayAdvance ==
    Healthy =>
        /\ rn = None => hclock < lastStart + Period
        /\ rn # None => hclock < rn.start + MaxLatency

TickBoth ==
    /\ ~slotFenced
    /\ hclock < MaxClock /\ oclock < MaxClock
    /\ HolderMayAdvance
    /\ hclock' = hclock + 1
    /\ oclock' = oclock + 1
    /\ credit' = IF GainEvery = 0 THEN 0 ELSE Min(GainEvery, credit + 1)
    /\ UNCHANGED << holderVars, observerVars >>

\* The observer's clock runs fast: the direction that shortens the observer's wait.
TickObserverOnly ==
    /\ ~slotFenced
    /\ GainEvery > 0 /\ credit = GainEvery
    /\ oclock < MaxClock
    /\ oclock' = oclock + 1
    /\ credit' = 0
    /\ UNCHANGED << hclock, holderVars, observerVars >>

\* The holder's clock runs fast: renewals and the end of authority come early for the observer.
TickHolderOnly ==
    /\ ~slotFenced
    /\ GainEvery > 0 /\ credit = GainEvery
    /\ hclock < MaxClock
    /\ HolderMayAdvance
    /\ hclock' = hclock + 1
    /\ credit' = 0
    /\ UNCHANGED << oclock, holderVars, observerVars >>

----------------------------------------------------------------------------
\* Holder.

\* The start is sampled before the first request. One renewal at a time.
RenewStart ==
    /\ rn = None /\ ~lost
    /\ hclock >= lastStart + Period
    /\ rn' = [start |-> hclock, landed |-> FALSE]
    /\ UNCHANGED << clockVars, slotToken, slotFenced, lastStart, lost, usedLastInstant,
                    wroteAfterFence, observerVars >>

\* The conditional write lands, at any time after the start: the holder retries until the store
\* answers. A fenced slot carries another token, so the write cannot land on it.
RenewLand ==
    /\ RenewalFailing
    /\ ~slotFenced
    /\ slotToken' = slotToken + 1
    /\ rn' = [rn EXCEPT !.landed = TRUE]
    /\ UNCHANGED << clockVars, slotFenced, lastStart, lost, usedLastInstant, wroteAfterFence,
                    observerVars >>

\* The holder learns the write landed. Authority counts from the start, never from this moment.
\* Enabled after a fence too: the response of a write that landed earlier can still arrive.
RenewAck ==
    /\ rn # None /\ rn.landed
    /\ lastStart' = rn.start
    /\ rn' = None
    /\ usedLastInstant' = FALSE
    /\ UNCHANGED << clockVars, slotToken, slotFenced, lost, wroteAfterFence, observerVars >>

\* The definitive answer: the holder's request meets the fenced slot.
RenewLearnFenced ==
    /\ RenewalFailing
    /\ slotFenced
    /\ lost' = TRUE
    /\ rn' = None
    /\ UNCHANGED << clockVars, slotToken, slotFenced, lastStart, usedLastInstant,
                    wroteAfterFence, observerVars >>

\* Write admission: two local facts, no read of the slot.
Write ==
    /\ ~lost
    /\ hclock < AuthorityEnd
    /\ wroteAfterFence' = (wroteAfterFence \/ slotFenced)
    /\ usedLastInstant' = (usedLastInstant \/ (hclock = AuthorityEnd - 1 /\ RenewalFailing))
    /\ UNCHANGED << clockVars, slotToken, slotFenced, lastStart, rn, lost, observerVars >>

----------------------------------------------------------------------------
\* Observer.

\* A round, or one iteration of the re-mount wait, samples the observer's clock.
ObsSample ==
    /\ opc = "idle" /\ ~slotFenced
    /\ roundSample # oclock
    /\ roundSample' = oclock
    /\ UNCHANGED << clockVars, holderVars, opc, seenToken, watchToken, watchSince, redecided >>

\* The read of the slot. Up to `MaxWalk` ticks separate it from the sample, and a later read may
\* reuse an old sample.
ObsRead ==
    /\ opc = "idle" /\ ~slotFenced
    /\ roundSample # None
    /\ oclock - roundSample <= MaxWalk
    /\ seenToken' = slotToken
    /\ opc' = "read"
    /\ UNCHANGED << clockVars, holderVars, roundSample, watchToken, watchSince, redecided >>

\* Natural-number comparison: a sample that precedes the sighting is never stable.
StableFor(sample) ==
    IF SabWrappingStability /\ sample < watchSince
    THEN TRUE
    ELSE sample >= watchSince + Threshold

\* The decision on what the read returned. Ticks may separate it from the read, so `oclock`
\* here is a sample taken after the read. Stability is tested with `roundSample`, which was
\* taken before the read.
ObsDecide ==
    /\ opc \in {"read", "reread"}
    /\ LET stable     == watchToken = seenToken /\ StableFor(roundSample)
           staleStamp == \/ opc = "read" /\ SabSightingBeforeRead
                         \/ opc = "reread" /\ SabRedecisionStaleSample
       IN IF stable
          THEN /\ opc' = "fencing"
               /\ UNCHANGED << seenToken, watchToken, watchSince >>
          ELSE /\ opc' = "idle"
               /\ seenToken' = None
               /\ IF watchToken = seenToken
                  THEN UNCHANGED << watchToken, watchSince >>
                  ELSE /\ watchToken' = seenToken
                       /\ watchSince' = IF staleStamp THEN roundSample ELSE oclock
    /\ redecided' = (redecided \/ opc = "reread")
    /\ UNCHANGED << clockVars, holderVars, roundSample >>

\* The fence write, conditional on the token the decision saw. It lands at any later time.
ObsFenceWrite ==
    /\ opc = "fencing"
    /\ IF slotToken = seenToken /\ ~slotFenced
       THEN /\ slotFenced' = TRUE
            /\ slotToken' = slotToken + 1
            /\ opc' = "idle"
            /\ watchToken' = None /\ watchSince' = 0
       ELSE /\ opc' = "refused"
            /\ UNCHANGED << slotFenced, slotToken, watchToken, watchSince >>
    /\ seenToken' = None
    /\ UNCHANGED << clockVars, lastStart, rn, lost, usedLastInstant, wroteAfterFence,
                    roundSample, redecided >>

\* After a refused fence the slot is read again, under the same round sample.
ObsReRead ==
    /\ opc = "refused"
    /\ oclock - roundSample <= MaxWalk
    /\ seenToken' = slotToken
    /\ opc' = "reread"
    /\ UNCHANGED << clockVars, holderVars, roundSample, watchToken, watchSince, redecided >>

\* The round ends without that second read: the read failed, or the round ran out of time.
ObsGiveUp ==
    /\ opc = "refused"
    /\ opc' = "idle"
    /\ UNCHANGED << clockVars, holderVars, roundSample, seenToken, watchToken, watchSince,
                    redecided >>

Next ==
    \/ TickBoth \/ TickObserverOnly \/ TickHolderOnly
    \/ RenewStart \/ RenewLand \/ RenewAck \/ RenewLearnFenced \/ Write
    \/ ObsSample \/ ObsRead \/ ObsDecide \/ ObsFenceWrite \/ ObsReRead \/ ObsGiveUp

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
TypeOK ==
    /\ hclock \in 0..MaxClock /\ oclock \in 0..MaxClock
    /\ credit \in 0..GainEvery
    /\ slotToken \in 1..(MaxClock + 3)
    /\ slotFenced \in BOOLEAN
    /\ lastStart \in 0..MaxClock
    /\ \/ rn = None
       \/ rn.start \in 0..MaxClock /\ rn.landed \in BOOLEAN
    /\ lost \in BOOLEAN
    /\ opc \in {"idle", "read", "fencing", "refused", "reread"}
    /\ roundSample \in (0..MaxClock) \union {None}
    /\ seenToken \in (1..(MaxClock + 3)) \union {None}
    /\ watchToken \in (1..(MaxClock + 3)) \union {None}
    /\ watchSince \in 0..MaxClock
    /\ usedLastInstant \in BOOLEAN
    /\ wroteAfterFence \in BOOLEAN
    /\ redecided \in BOOLEAN

\* When the fence lands the holder has no authority, and it gains none afterwards.
NoFenceInsideAuthority ==
    slotFenced => hclock >= AuthorityEnd

\* The same with a margin: the fence lands at least `MinMargin` holder ticks after authority ended.
FenceMarginAtLeast ==
    slotFenced => hclock - AuthorityEnd >= MinMargin

NoWriteAfterFence ==
    ~wroteAfterFence

\* A holder whose renewals land on time is never fenced and never loses authority.
HealthyNeverFenced ==
    Healthy => ~slotFenced

HealthyKeepsAuthority ==
    Healthy => hclock < AuthorityEnd

----------------------------------------------------------------------------
\* Witnesses: negated reachability. A violation is the evidence.

\* The observer does fence.
W_ObserverFences ==
    ~slotFenced

\* A holder whose renewal never lands writes at the last tick of its lease and is fenced later.
W_FailingHolderKeepsLeaseToItsEnd ==
    ~(slotFenced /\ usedLastInstant /\ RenewalFailing)

\* A fence is refused because the holder renewed, the slot is decided again, and a later fence
\* lands.
W_RefusedFenceIsRedecided ==
    ~(slotFenced /\ redecided)

\* The observer waits out the whole threshold and then finds that a healthy holder moved the token.
W_HealthyHolderOutlivesTheThreshold ==
    ~(/\ Healthy
      /\ opc = "read"
      /\ watchToken # None /\ seenToken # watchToken
      /\ roundSample >= watchSince + Threshold)
=============================================================================
