--------------------- MODULE CaBuildWatermarkNum ---------------------
\* =====================================================================================
\* STALE vs SHIPPED CODE (2026-06-23 currency review). Same removed blob-guard subject as
\* CaBuildWatermark (B171 removed per-candidate watermark blob-protection). The monotone
\* build_seq / min_active floor lemma it validates IS still load-bearing -- but for precommit-ref
\* reclaim (build_seq < min_active, CasGc.cpp ~1877), NOT blob protection. Superseded as a
\* blob-protection model; floor lemma retained. Details: MODEL_CURRENCY_REVIEW_2026-06-22.md.
\* =====================================================================================
(***************************************************************************)
(* CONCRETE, NUMERIC validation of the watermark oracle (B167) — the part  *)
(* CaBuildWatermark abstracts away. Here `min_active` is a real per-server  *)
(* scalar over numeric `build_seq`s, there are TWO servers each with its    *)
(* own epoch + watermark, and `epoch` is a real counter that a restart      *)
(* bumps. This exists to validate three things the boolean model asserts:   *)
(*                                                                         *)
(*  1. SCALAR FLOOR. A blob owned by build_seq k on server s is protected   *)
(*     iff k >= min(active build_seqs on s). An OLDER build keeping the     *)
(*     floor low must not cause a younger build's blob to be condemned, and *)
(*     a finished build's blob must lose protection only when the floor     *)
(*     actually passes it.                                                  *)
(*  2. PER-SERVER SCOPING. A blob owned by server s is governed by s's      *)
(*     watermark ALONE — never another server's. The `_confused` config    *)
(*     wires the guard to the wrong server and MUST violate safety.         *)
(*  3. EPOCH / RESTART. After a server restart (epoch bumped), an old-epoch  *)
(*     owner is unprotected (its incarnation is dead) and reclaimable.      *)
(*                                                                         *)
(* This model checks SAFETY only (the boolean model checked liveness):      *)
(*   Inv_ProtectedNeverCondemned == ProtectedCorrect => ~condemned          *)
(*       a GENUINELY protected blob (own server, epoch match, seq>=floor)    *)
(*       is never condemned -> never exact-token-deleted -> never lost.     *)
(*   Inv_NoDangle == referenced => present                                  *)
(*       a published reference never dangles.                               *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Servers,        \* {s1, s2}
    NoneServer,     \* model value: the blob carries no owner
    MaxSeq,         \* build_seq range 1..MaxSeq per (server, epoch)
    MaxEpoch,       \* a server may restart up to this epoch (0..MaxEpoch)
    ConfusedGuard,  \* SABOTAGE: TRUE = guard checks the WRONG server's watermark
    MonotonicSeq    \* TRUE = build_seq allocated strictly increasing (a counter); FALSE = SABOTAGE (out-of-order)

VARIABLES
    present, condemned, referenced,         \* the everEdged blob
    ownerServer, ownerEpoch, ownerSeq,      \* the triple stamped on the present incarnation
    epoch,                                   \* [Servers -> Nat] current epoch per server
    active,                                  \* [Servers -> SUBSET (1..MaxSeq)] in-flight build_seqs
    issued                                   \* [Servers -> SUBSET (1..MaxSeq)] build_seqs already used THIS epoch

vars == << present, condemned, referenced, ownerServer, ownerEpoch, ownerSeq, epoch, active, issued >>

Other(s) == CHOOSE t \in Servers : t # s
Min(S) == CHOOSE x \in S : \A y \in S : x <= y
\* The scalar floor: oldest in-flight build_seq, or MaxSeq+1 (nothing active => nothing protected).
MinActive(s) == IF active[s] = {} THEN MaxSeq + 1 ELSE Min(active[s])

\* Protection as judged against a GIVEN server's watermark.
ProtectedAt(s) ==
    /\ ownerServer # NoneServer
    /\ ownerEpoch = epoch[s]
    /\ ownerSeq >= MinActive(s)

\* The TRUTH: a blob is genuinely protected iff its OWN server's watermark protects it.
ProtectedCorrect == ownerServer # NoneServer /\ ProtectedAt(ownerServer)
\* What the GC guard actually evaluates: correct, unless ConfusedGuard wires it to the other server.
ProtectedGuard ==
    IF ownerServer = NoneServer THEN FALSE
    ELSE ProtectedAt(IF ConfusedGuard THEN Other(ownerServer) ELSE ownerServer)

Init ==
    /\ present = TRUE
    /\ condemned = TRUE          \* a stale condemned incarnation from a past build
    /\ referenced = FALSE
    /\ ownerServer = NoneServer
    /\ ownerEpoch = 0
    /\ ownerSeq = 0
    /\ epoch = [s \in Servers |-> 0]
    /\ active = [s \in Servers |-> {}]
    /\ issued = [s \in Servers |-> {}]

----------------------------------------------------------------------------
\* A build with build_seq k starts on server s (enters the in-memory active set). build_seq is monotone
\* within an epoch: a seq already ISSUED this epoch is never reused (so a finished build's seq cannot
\* re-enter the active set and falsely re-protect a stale owner).
StartBuild(s, k) ==
    /\ k \in (1..MaxSeq)
    /\ k \notin issued[s]
    /\ (MonotonicSeq => \A j \in issued[s] : k > j)   \* a counter: every new build_seq exceeds all prior
    /\ active' = [active EXCEPT ![s] = @ \cup {k}]
    /\ issued' = [issued EXCEPT ![s] = @ \cup {k}]
    /\ UNCHANGED << present, condemned, referenced, ownerServer, ownerEpoch, ownerSeq, epoch >>

\* That build re-streams a fresh incarnation stamped with its own (s, epoch[s], k) — body in hand.
Restamp(s, k) ==
    /\ k \in active[s]
    /\ (~present \/ condemned)
    /\ ownerServer' = s
    /\ ownerEpoch' = epoch[s]
    /\ ownerSeq' = k
    /\ present' = TRUE
    /\ condemned' = FALSE
    /\ UNCHANGED << referenced, epoch, active, issued >>

\* The build references the present, uncondemned incarnation (InDeg>=1) and finishes (leaves active).
PublishRef(s, k) ==
    /\ k \in active[s]
    /\ present
    /\ ~condemned
    /\ referenced' = TRUE
    /\ active' = [active EXCEPT ![s] = @ \ {k}]
    /\ UNCHANGED << present, condemned, ownerServer, ownerEpoch, ownerSeq, epoch, issued >>

\* A build finishes WITHOUT referencing the blob — exercises the floor advancing while the blob is
\* owned by some other (older/younger) build.
FinishNoRef(s, k) ==
    /\ k \in active[s]
    /\ active' = [active EXCEPT ![s] = @ \ {k}]
    /\ UNCHANGED << present, condemned, referenced, ownerServer, ownerEpoch, ownerSeq, epoch, issued >>

\* Server restart: bump epoch, drop the in-memory active set and the issued numbering (fresh epoch).
\* An old-epoch owner is now stale (epoch mismatch).
Restart(s) ==
    /\ epoch[s] < MaxEpoch
    /\ epoch' = [epoch EXCEPT ![s] = @ + 1]
    /\ active' = [active EXCEPT ![s] = {}]
    /\ issued' = [issued EXCEPT ![s] = {}]
    /\ UNCHANGED << present, condemned, referenced, ownerServer, ownerEpoch, ownerSeq >>

\* GC condemns an unreferenced, unprotected present incarnation (guard uses ProtectedGuard).
GcCondemn ==
    /\ present
    /\ ~condemned
    /\ ~referenced
    /\ ~ProtectedGuard
    /\ condemned' = TRUE
    /\ UNCHANGED << present, referenced, ownerServer, ownerEpoch, ownerSeq, epoch, active, issued >>

\* Exact-token delete of a condemned incarnation.
GcDelete ==
    /\ present
    /\ condemned
    /\ present' = FALSE
    /\ condemned' = FALSE
    /\ ownerServer' = NoneServer
    /\ ownerEpoch' = 0
    /\ ownerSeq' = 0
    /\ UNCHANGED << referenced, epoch, active, issued >>

Next ==
    \/ \E s \in Servers, k \in (1..MaxSeq) : StartBuild(s, k)
    \/ \E s \in Servers, k \in (1..MaxSeq) : Restamp(s, k)
    \/ \E s \in Servers, k \in (1..MaxSeq) : PublishRef(s, k)
    \/ \E s \in Servers, k \in (1..MaxSeq) : FinishNoRef(s, k)
    \/ \E s \in Servers : Restart(s)
    \/ GcCondemn
    \/ GcDelete

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* A genuinely protected incarnation (own server's watermark protects it) is NEVER condemned -> never
\* deleted -> never lost. With the correct per-server guard this holds; ConfusedGuard MUST break it.
Inv_ProtectedNeverCondemned == ProtectedCorrect => ~condemned
\* A published reference never dangles.
Inv_NoDangle == referenced => present

TypeOK ==
    /\ present \in BOOLEAN /\ condemned \in BOOLEAN /\ referenced \in BOOLEAN
    /\ ownerServer \in (Servers \cup {NoneServer})
    /\ ownerEpoch \in 0..MaxEpoch
    /\ ownerSeq \in 0..MaxSeq
    /\ epoch \in [Servers -> 0..MaxEpoch]
    /\ active \in [Servers -> SUBSET (1..MaxSeq)]
    /\ issued \in [Servers -> SUBSET (1..MaxSeq)]
=============================================================================
