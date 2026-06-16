----------------------- MODULE CaBuildWatermark -----------------------
(***************************************************************************)
(* TLA+ LIVENESS model of the CA per-server build WATERMARK (B167).        *)
(*                                                                         *)
(* Spec: docs/superpowers/specs/2026-06-16-ca-build-watermark-design.md    *)
(*                                                                         *)
(* This replaces the abstract `HeartbeatGuard` boolean of                  *)
(* CaResurrectLiveness with the CONCRETE watermark oracle the design       *)
(* actually implements, and adds the cross-build (last-writer-wins) case.  *)
(*                                                                         *)
(* THE SETTING. One content hash that is `everEdged` and `InDeg=0` (it was *)
(* published once, then dropped -> condemned). One or more builds dedup-   *)
(* hit it and want to reference it in their OWN parts, then publish. A     *)
(* build has the body in hand, so on a CONDEMNED hit it re-streams a FRESH *)
(* incarnation stamped with its OWN triple (server,epoch,build_seq) — that *)
(* is `owner`. There is NO bodyless re-stamp and NO server-side copy.      *)
(*                                                                         *)
(* THE ORACLE UNDER TEST. GC condemns only                                 *)
(*     present /\ everEdged /\ InDeg=0 /\ ~Protected                       *)
(* where, concretely,                                                      *)
(*     Protected == WatermarkGuard /\ serverLive /\ ~gcDead                *)
(*                  /\ owner # NoOwner /\ owner \in activeSet.              *)
(*   - `owner \in activeSet`  models  `build_seq >= min_active` for a live  *)
(*     server: the owning build is still in-flight. (This is a CONSERVATIVE *)
(*     over-approximation of the scalar floor — it unprotects a build's     *)
(*     blobs as soon as THAT build finishes, even if an older build keeps   *)
(*     min_active low; that only makes GC MORE aggressive, so liveness      *)
(*     proven here implies liveness under the real, more-protective floor.) *)
(*   - `serverLive /\ ~gcDead` models the per-server liveness signal +      *)
(*     crashed-server detection. `epoch` (stale incarnation => unprotected) *)
(*     is folded into serverLive: an old epoch is exactly "old server not   *)
(*     live".                                                              *)
(*                                                                         *)
(* PUBLISH does NOT require ownership: a build references the present       *)
(* incarnation iff it is present /\ ~condemned (W-REVALIDATE adopt). Re-    *)
(* stamp-to-self is needed only to CLEAR a condemnation / regain           *)
(* protection. This is why two builds do not ping-pong: once any owner     *)
(* keeps it present+uncondemned, BOTH can publish.                         *)
(*                                                                         *)
(* PROPERTIES                                                              *)
(*   Liveness == <>(published = Builds)   -- every live build publishes.    *)
(*       HOLDS with the sound watermark; VIOLATED under each sabotage.      *)
(*   NoLeak   == (serverLive=FALSE /\ published={}) ~> <> ~present          *)
(*       -- a server that crashes before any publish has its in-flight      *)
(*       blob reclaimed (no leak), via the frozen-seq crash detection.      *)
(*                                                                         *)
(* SABOTAGE CONTROLS (each must produce a counterexample)                  *)
(*   WatermarkGuard=FALSE   today's GC: re-condemns the build's own fresh   *)
(*       incarnation -> the B167 starvation lasso. Liveness VIOLATED.       *)
(*   ActiveSetCorrect=FALSE min_active wrongly advances past a STILL-active *)
(*       build (MinActiveSkip) -> its fresh incarnation loses protection -> *)
(*       starve. Confirms the in-memory active-set discipline is load-      *)
(*       bearing. Liveness VIOLATED.                                        *)
(*   SoundDetection=FALSE   crash detection (K too small) declares a LIVE   *)
(*       server dead (GcDeclareDead while serverLive) -> unprotect ->       *)
(*       starve. Confirms K>=2 / B160-style sound detection. Liveness       *)
(*       VIOLATED.                                                          *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    Builds,            \* the contending builds on ONE server, e.g. {b1, b2}
    NoOwner,           \* model value: the present incarnation carries no live triple
    WatermarkGuard,    \* TRUE = GC honors the watermark (the fix); FALSE = today's GC
    SoundDetection,    \* TRUE = GC declares dead only a genuinely dead server (K>=2); FALSE = K too small
    ActiveSetCorrect,  \* TRUE = min_active never passes a still-active build; FALSE = MinActiveSkip enabled
    CanCrash           \* TRUE = the server may crash (for NoLeak); FALSE = server stays alive (for Liveness)

VARIABLES
    present,     \* an incarnation of the everEdged blob is present in the pool
    condemned,   \* GC has condemned the present incarnation (exact-token, in the retire pipeline)
    owner,       \* which build's triple the present incarnation carries (last-writer-wins), or NoOwner
    published,   \* set of builds that have referenced the present incarnation and published
    activeSet,   \* builds the server watermark counts as in-flight (min_active floor view)
    serverLive,  \* the server process is alive (epoch current)
    gcDead       \* GC believes the server is dead (frozen-seq detection verdict)

vars == << present, condemned, owner, published, activeSet, serverLive, gcDead >>

\* A dedup hit lands on a STALE condemned incarnation left by a past, finished build: present,
\* condemned, owned by nobody live. Both builds are in-flight (activeSet = Builds).
Init ==
    /\ present = TRUE
    /\ condemned = TRUE
    /\ owner = NoOwner
    /\ published = {}
    /\ activeSet = Builds
    /\ serverLive = TRUE
    /\ gcDead = FALSE

\* The present incarnation is protected from condemnation iff the guard is on AND its owning build is a
\* live, in-flight build on a live, not-presumed-dead server.
Protected ==
    /\ WatermarkGuard
    /\ serverLive
    /\ ~gcDead
    /\ owner # NoOwner
    /\ owner \in activeSet

----------------------------------------------------------------------------
\* WRITER: re-stream the body into a FRESH incarnation stamped with THIS build's triple. Body in hand,
\* so no GET. Needed only when the blob is absent or condemned (a present+uncondemned blob is just
\* adopted by BuildPublish). The fresh tag clears the condemnation (new ETag) and sets owner := b.
BuildRestamp(b) ==
    /\ b \notin published
    /\ serverLive
    /\ (~present \/ condemned)
    /\ present' = TRUE
    /\ condemned' = FALSE
    /\ owner' = b
    /\ UNCHANGED << published, activeSet, serverLive, gcDead >>

\* PUBLISH: reference the present, uncondemned incarnation (W-REVALIDATE adopt — no ownership required)
\* -> InDeg>=1 -> publish. The build finishes, so it leaves the active set (min_active advances).
BuildPublish(b) ==
    /\ b \notin published
    /\ serverLive
    /\ present
    /\ ~condemned
    /\ published' = published \cup {b}
    /\ activeSet' = activeSet \ {b}
    /\ UNCHANGED << present, condemned, owner, serverLive, gcDead >>

\* GC condemns the present incarnation. everEdged holds throughout; InDeg=0 holds while nobody has
\* published a reference (published = {}). The guard spares a Protected incarnation.
GcCondemn ==
    /\ present
    /\ ~condemned
    /\ published = {}
    /\ ~Protected
    /\ condemned' = TRUE
    /\ UNCHANGED << present, owner, published, activeSet, serverLive, gcDead >>

\* GC's exact-token delete of a CONDEMNED incarnation. A Protected incarnation never becomes condemned,
\* so it is never deleted.
GcDelete ==
    /\ present
    /\ condemned
    /\ present' = FALSE
    /\ condemned' = FALSE
    /\ owner' = NoOwner
    /\ UNCHANGED << published, activeSet, serverLive, gcDead >>

\* The server process crashes (only when CanCrash). Its builds can no longer act.
ServerCrash ==
    /\ CanCrash
    /\ serverLive
    /\ serverLive' = FALSE
    /\ UNCHANGED << present, condemned, owner, published, activeSet, gcDead >>

\* GC's crashed-server verdict (frozen seq across its window). SOUND detection fires only for a genuinely
\* dead server; UNSOUND detection (K too small) may fire while the server is still live.
GcDeclareDead ==
    /\ ~gcDead
    /\ (SoundDetection => ~serverLive)
    /\ gcDead' = TRUE
    /\ UNCHANGED << present, condemned, owner, published, activeSet, serverLive >>

\* SABOTAGE (b): the watermark floor wrongly advances past a still-in-flight build (a broken in-memory
\* active set). Only enabled when ActiveSetCorrect is FALSE.
MinActiveSkip(b) ==
    /\ ~ActiveSetCorrect
    /\ b \in activeSet
    /\ b \notin published
    /\ activeSet' = activeSet \ {b}
    /\ UNCHANGED << present, condemned, owner, published, serverLive, gcDead >>

Next ==
    \/ \E b \in Builds : BuildRestamp(b)
    \/ \E b \in Builds : BuildPublish(b)
    \/ \E b \in Builds : MinActiveSkip(b)
    \/ GcCondemn
    \/ GcDelete
    \/ ServerCrash
    \/ GcDeclareDead

\* Weak fairness: the build keeps trying (its own actions), AND GC eventually does its reclaim work
\* (so the NoLeak property has teeth). GC's condemn of a live build is never FORCED to harm liveness,
\* because the guard DISABLES it while Protected — WF only bites when it is continuously enabled.
Fairness ==
    /\ \A b \in Builds : WF_vars(BuildRestamp(b))
    /\ \A b \in Builds : WF_vars(BuildPublish(b))
    /\ WF_vars(GcDeclareDead)
    /\ WF_vars(GcCondemn)
    /\ WF_vars(GcDelete)

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ Fairness

----------------------------------------------------------------------------
\* Every live build eventually publishes. HOLDS with the sound watermark; VIOLATED under each sabotage.
Liveness == <>(published = Builds)

\* A server that crashes before any publish has its orphaned in-flight blob reclaimed (no leak).
NoLeak == (serverLive = FALSE /\ published = {}) ~> (<> (~present))

TypeOK ==
    /\ present \in BOOLEAN
    /\ condemned \in BOOLEAN
    /\ owner \in (Builds \cup {NoOwner})
    /\ published \subseteq Builds
    /\ activeSet \subseteq Builds
    /\ serverLive \in BOOLEAN
    /\ gcDead \in BOOLEAN

----------------------------------------------------------------------------
\* SAFETY — must hold in EVERY config, sabotage included. B167 is a LIVENESS bug, not a safety bug:
\* the sabotages must break only Liveness, never these. (If a sabotage ever violated one of these, the
\* model would be wrong about where the danger is.)
\*
\* The guard never leaves a Protected incarnation condemned (hence exact-token-deletable): a protected
\* blob is never lost.
Inv_ProtectedNeverCondemned == Protected => ~condemned
\* A published reference never dangles: once any build has published referencing the incarnation
\* (InDeg>=1), it stays present (GcCondemn is disabled, so GcDelete can never fire on it).
Inv_NoDangle == (published # {}) => present
=============================================================================
