------------------------- MODULE CaGcShardIncarnationCore -------------------------
(*****************************************************************************)
(* D1 phase-0 gate — CA GC shard incarnation + registry removal.            *)
(* Spec: docs/superpowers/specs/2026-07-01-cas-shard-incarnation-and-        *)
(*       registry-removal-design.md                                         *)
(*                                                                          *)
(* TWO coordinates replace the five overlapping "fence" counters + the      *)
(* namespace registry of the proven CaIncarnationCore.tla:                  *)
(*   (1) a durable never-reused per-(ns,shard) INCARNATION (sInc, from the  *)
(*       sIncMax allocator) — identity; the fold cursor is keyed by it, so  *)
(*       a delete+recreate at the same path can never be ABA-confused.      *)
(*   (2) the pool-global GC ROUND — a newborn shard is born fenced to the   *)
(*       current gcRound (self-floor: the writer reads gc/state.round), and *)
(*       GC discovers+fences EVERY present shard by LIST each round.         *)
(* There is NO registry variable. Discovery == the set of present shards.   *)
(*                                                                          *)
(* Theorems checked (INV_NO_DANGLING, INV_NO_ORPHAN_EDGE) and their         *)
(* load-bearing negative controls — each MUST break its invariant:          *)
(*   SabotageNewbornNoFloor  : newborn floor 0 (no round coord)  -> dangle  *)
(*                             (proves coordinate 2 is irreducible)         *)
(*   SabotagePathKeyedCursor : cursor keyed by path, not incarnation -> ABA *)
(*                             (proves coordinate 1 is irreducible)         *)
(*   SabotageDeleteBeforeFold: reclaim a shard object before its journal is *)
(*                             folded -> orphan edge -> blob leak           *)
(*   SabotageIncarnationReuse: a recreated (ns,shard) reuses a prior        *)
(*                             incarnation (INC-MONO broken) -> ABA dangle  *)
(*                             (proves per-shard incarnation monotonicity,  *)
(*                             not global uniqueness, is what matters — the *)
(*                             design config ALREADY lets incarnations      *)
(*                             collide across DIFFERENT shards and holds).   *)
(*                                                                          *)
(* Reuses the proven idioms of CaIncarnationCore.tla verbatim: incarnation  *)
(* tokens on blobs (tokOf/nextTok/deadTok), CondemnedAtView, ViewableRound, *)
(* fold->retire->fence->recheck->delete, exact-token late-landing deletes.  *)
(*****************************************************************************)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Blobs,                       \* shared content-addressed objects, e.g. {b1}
    Shards,                      \* namespaces (one root shard each), e.g. {n1, n2}
    Writers,                     \* e.g. {w1, w2} (single writer per namespace)
    Leaders,                     \* GC leaders, e.g. {L1}
    MaxTok,                      \* token allocator bound per blob
    MaxRound,                    \* GC round bound
    MaxLog,                      \* journal length bound per shard
    MaxInc,                      \* shard-incarnation allocator bound
    SabotageNewbornNoFloor,      \* TRUE: newborn born-floor 0 (registry removed WRONG) -> must dangle
    SabotagePathKeyedCursor,     \* TRUE: fold cursor keyed by path only (ignore incarnation) -> ABA -> must dangle
    SabotageDeleteBeforeFold,    \* TRUE: reclaim a shard object before its journal is folded -> orphan edge
    SabotageIncarnationReuse     \* TRUE: a recreated (ns,shard) may REUSE a prior incarnation (INC-MONO broken) -> ABA

Toks == 1..MaxTok

\* Journal record: an owner-edge add/rem, a fence marker, or a drop tombstone (dropNamespace).
Rec == [op: {"add", "rem", "fence", "tomb"}, b: Blobs \cup {"none"}]

VARIABLES
    \* ---- durable: shared blobs (incarnation tokens) ----
    present,    \* [Blobs -> BOOLEAN]
    tokOf,      \* [Blobs -> 0..MaxTok]        current incarnation token (0 = never created)
    nextTok,    \* [Blobs -> 1..MaxTok+1]      fresh-token allocator
    deadTok,    \* [Blobs -> SUBSET Toks]      tokens that stopped being current (INV_NO_RETURN history)
    \* ---- durable: root shards (NO registry) ----
    sPresent,   \* [Shards -> BOOLEAN]         the ref-shard object exists (=> LIST-discoverable)
    sInc,       \* [Shards -> 0..MaxInc]       the current object's incarnation (0 = absent; coordinate 1)
    sIncMax,    \* [Shards -> 0..MaxInc]        per-shard incarnation high-water: the highest incarnation
                \*                              ever assigned to this path. A fresh birth draws > sIncMax[s]
                \*                              (strictly per-shard monotone => INC-MONO; incs MAY collide
                \*                              across DIFFERENT shards, which is safe — the cursor keys by
                \*                              (shard, inc)). SabotageIncarnationReuse lets a recreate draw
                \*                              <= sIncMax[s], reusing a prior incarnation -> ABA.
    refs,       \* [Shards -> SUBSET (Blobs \X Toks)]  committed refs (blob + bound token)
    fence,      \* [Shards -> 0..MaxRound]     the shard's fence_round (writer floor)
    log,        \* [Shards -> Seq(Rec)]        append-only owner journal (fresh per incarnation)
    tomb,       \* [Shards -> BOOLEAN]         a drop tombstone is the last journal event
    \* ---- GC snap + round state ----
    gcRound,    \* 0..MaxRound
    gcPhase,    \* [Leaders -> {"idle","retiring","fencing","fenced"}]
    roundOf,    \* [Leaders -> 0..MaxRound]
    fencedSet,  \* [Leaders -> SUBSET Shards]
    fencePos,   \* [Shards -> Nat]             log position of the latest fence record (this incarnation)
    cursor,     \* [Shards -> [inc: 0..MaxInc, pos: Nat]]  folded prefix, keyed by incarnation
    rootEdges,  \* SUBSET (Shards \X Blobs)    folded owner edges (edge-set semantics)
    everEdged,  \* SUBSET Blobs                journal-known
    retired,    \* SUBSET [b: Blobs, t: Toks, r: 1..MaxRound]
    inflight,   \* SUBSET [b: Blobs, t: Toks]  delete messages (land arbitrarily late)
    \* ---- writer local ----
    wView,      \* [Writers -> 0..MaxRound]    highest retire round refreshed
    wHave       \* [Writers -> SUBSET (Blobs \X Toks)]  blobs created/held, ready to publish

vars == << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log, tomb,
           gcRound, gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges,
           everEdged, retired, inflight, wView, wHave >>

-----------------------------------------------------------------------------
\* ---- helpers (reused verbatim from CaIncarnationCore) ----
RetiredHit(b, t, v)      == \E e \in retired : e.b = b /\ e.t = t /\ e.r <= v
CondemnedAtView(b, t, v) == RetiredHit(b, t, v) \/ t \in deadTok[b]
InDeg(b)                 == Cardinality({e \in rootEdges : e[2] = b})

\* During a leader's RETIRING phase the round is not yet fully visible, so a view can only claim
\* gcRound-1 (the CaIncarnationCore ViewableRound rule — load-bearing for same-round retires).
ViewableRound == IF \E l \in Leaders : gcPhase[l] = "retiring" /\ roundOf[l] = gcRound
                 THEN gcRound - 1 ELSE gcRound

\* A shard is folded-through-fence iff the cursor's INCARNATION matches the live object AND its
\* position reached the fence. SabotagePathKeyedCursor drops the incarnation check (the ABA bug):
\* a stale cursor position from a prior incarnation then satisfies the guard falsely.
ShardFolded(s) == /\ SabotagePathKeyedCursor \/ cursor[s].inc = sInc[s]
                  /\ cursor[s].pos >= fencePos[s]
FoldedThroughFence == \A s \in Shards : sPresent[s] => ShardFolded(s)

\* The self-floor a newborn shard is stamped with at birth: the CURRENT gcRound (the writer reads
\* gc/state.round). Coordinate 2. Registry removed WRONG => 0.
BornFloor == IF SabotageNewbornNoFloor THEN 0 ELSE gcRound

-----------------------------------------------------------------------------
Init ==
    /\ present  = [b \in Blobs |-> FALSE]
    /\ tokOf    = [b \in Blobs |-> 0]
    /\ nextTok  = [b \in Blobs |-> 1]
    /\ deadTok  = [b \in Blobs |-> {}]
    /\ sPresent = [s \in Shards |-> FALSE]
    /\ sInc     = [s \in Shards |-> 0]
    /\ sIncMax  = [s \in Shards |-> 0]
    /\ refs     = [s \in Shards |-> {}]
    /\ fence    = [s \in Shards |-> 0]
    /\ log      = [s \in Shards |-> << >>]
    /\ tomb     = [s \in Shards |-> FALSE]
    /\ gcRound  = 0
    /\ gcPhase  = [l \in Leaders |-> "idle"]
    /\ roundOf  = [l \in Leaders |-> 0]
    /\ fencedSet= [l \in Leaders |-> {}]
    /\ fencePos = [s \in Shards |-> 0]
    /\ cursor   = [s \in Shards |-> [inc |-> 0, pos |-> 0]]
    /\ rootEdges= {}
    /\ everEdged= {}
    /\ retired  = {}
    /\ inflight = {}
    /\ wView    = [w \in Writers |-> 0]
    /\ wHave    = [w \in Writers |-> {}]

-----------------------------------------------------------------------------
\* ---- writer actions ----

\* Create a missing blob: fresh token from the allocator. Recorded in wHave for a later publish.
WCreateBlob(w, b) ==
    /\ ~present[b] /\ nextTok[b] <= MaxTok
    /\ present' = [present EXCEPT ![b] = TRUE]
    /\ tokOf'   = [tokOf   EXCEPT ![b] = nextTok[b]]
    /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
    /\ wHave'   = [wHave   EXCEPT ![w] = @ \cup {<<b, nextTok[b]>>}]
    /\ UNCHANGED << deadTok, sPresent, sInc, sIncMax, refs, fence, log, tomb, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged, retired, inflight, wView >>

\* Refresh the writer's retire view to the currently viewable round.
WRefreshView(w) ==
    /\ wView' = [wView EXCEPT ![w] = ViewableRound]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges,
                    everEdged, retired, inflight, wHave >>

\* Publish blob b into shard s (one atomic CAS). If s is a NEWBORN (absent — brand-new OR a path
\* reclaimed earlier), it is (re)created here: a FRESH incarnation from sIncMax (never reused), a
\* FRESH empty log seeded with this "add", fencePos reset, and the self-floor BornFloor — all made
\* present (LIST-discoverable) atomically with the committed ref. The publish gate: the writer's
\* view must reach the shard's floor AND the dependency must not be condemned at that view.
WPublish(w, s, b) ==
    /\ present[b]
    /\ \E t \in Toks : <<b, t>> \in wHave[w]
    /\ ~(\E t \in Toks : <<b, t>> \in refs[s])
    /\ IF sPresent[s]
       THEN /\ Len(log[s]) < MaxLog
            /\ wView[w] >= fence[s] /\ ~CondemnedAtView(b, tokOf[b], wView[w])
            /\ refs'    = [refs    EXCEPT ![s] = @ \cup {<<b, tokOf[b]>>}]
            /\ log'     = [log     EXCEPT ![s] = Append(@, [op |-> "add", b |-> b])]
            /\ UNCHANGED << sPresent, sInc, sIncMax, fence, fencePos, tomb >>
       ELSE /\ wView[w] >= BornFloor /\ ~CondemnedAtView(b, tokOf[b], wView[w])
            \* Choose this incarnation. Design: strictly per-shard monotone (> sIncMax[s]) — the fresh
            \* birth; incs may collide with OTHER shards (safe). SabotageIncarnationReuse on a RECREATE
            \* (sIncMax[s] >= 1) may pick <= sIncMax[s], reusing a prior incarnation -> the cursor keyed
            \* by (shard, inc) can then match a stale prior fold -> ABA.
            /\ \E newInc \in 1..MaxInc :
                 /\ IF SabotageIncarnationReuse /\ sIncMax[s] >= 1
                    THEN newInc <= sIncMax[s]
                    ELSE newInc > sIncMax[s]
                 /\ sInc'    = [sInc    EXCEPT ![s] = newInc]
                 /\ sIncMax' = [sIncMax EXCEPT ![s] = IF newInc > @ THEN newInc ELSE @]
            /\ sPresent' = [sPresent EXCEPT ![s] = TRUE]
            /\ fence'    = [fence    EXCEPT ![s] = BornFloor]
            /\ fencePos' = [fencePos EXCEPT ![s] = 0]
            /\ refs'     = [refs     EXCEPT ![s] = {<<b, tokOf[b]>>}]
            /\ log'      = [log      EXCEPT ![s] = << [op |-> "add", b |-> b] >>]
            /\ tomb'     = [tomb     EXCEPT ![s] = FALSE]
    /\ wHave' = [wHave EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, gcRound, gcPhase, roundOf, fencedSet,
                    cursor, rootEdges, everEdged, retired, inflight, wView >>

\* Drop a committed ref (append a removal to the journal). The journal carries the -1 GC folds.
WDrop(s, b) ==
    /\ \E t \in Toks : <<b, t>> \in refs[s]
    /\ Len(log[s]) < MaxLog
    /\ refs' = [refs EXCEPT ![s] = { e \in @ : e[1] # b }]
    /\ log'  = [log  EXCEPT ![s] = Append(@, [op |-> "rem", b |-> b])]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, fence, tomb, gcRound,
                    gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged, retired,
                    inflight, wView, wHave >>

\* dropNamespace's final act: with no refs left, append the drop tombstone as the last journal event.
WTombstone(s) ==
    /\ sPresent[s] /\ refs[s] = {} /\ ~tomb[s]
    /\ Len(log[s]) < MaxLog
    /\ tomb' = [tomb EXCEPT ![s] = TRUE]
    /\ log'  = [log  EXCEPT ![s] = Append(@, [op |-> "tomb", b |-> "none"])]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, gcRound,
                    gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged, retired,
                    inflight, wView, wHave >>

\* Abandon held (uncommitted) uploads (crash before publish).
WAbandon(w) ==
    /\ wHave[w] # {}
    /\ wHave' = [wHave EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges,
                    everEdged, retired, inflight, wView >>

-----------------------------------------------------------------------------
\* ---- GC actions (per leader) ----

GStartRound(l) ==
    /\ gcPhase[l] = "idle" /\ gcRound < MaxRound
    /\ gcRound'   = gcRound + 1
    /\ roundOf'   = [roundOf   EXCEPT ![l] = gcRound + 1]
    /\ gcPhase'   = [gcPhase   EXCEPT ![l] = "retiring"]
    /\ fencedSet' = [fencedSet EXCEPT ![l] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, fencePos, cursor, rootEdges, everEdged, retired, inflight, wView, wHave >>

\* Fold one journal record into the snap (edge-set semantics). INCARNATION-KEYED: if the cursor was
\* sealed against a PRIOR incarnation of this path, the fold restarts at 0 and drops this path's
\* stale folded edges first (a delete+recreate is a clean new object). SabotagePathKeyedCursor
\* ignores the incarnation — the ABA bug: a stale cursor position skips the new incarnation's events.
GFold(s) ==
    /\ LET stale == ~SabotagePathKeyedCursor /\ cursor[s].inc # sInc[s]
           base  == IF stale THEN 0 ELSE cursor[s].pos
           prior == IF stale THEN { e \in rootEdges : e[1] # s } ELSE rootEdges
       IN /\ base < Len(log[s])
          /\ LET rec == log[s][base + 1] IN
             /\ rootEdges' = CASE rec.op = "add"   -> prior \cup {<<s, rec.b>>}
                               [] rec.op = "rem"   -> prior \ {<<s, rec.b>>}
                               [] rec.op = "fence" -> prior
                               [] rec.op = "tomb"  -> prior
             /\ everEdged' = IF rec.op = "add" THEN everEdged \cup {rec.b} ELSE everEdged
          /\ cursor' = [cursor EXCEPT ![s] = [inc |-> sInc[s], pos |-> base + 1]]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, gcPhase, roundOf, fencedSet, fencePos, retired, inflight, wView, wHave >>

\* Retire a journal-known, present, in-degree-0 blob at its current token (the HEAD).
GRetire(l, b) ==
    /\ gcPhase[l] = "retiring"
    /\ present[b] /\ b \in everEdged /\ InDeg(b) = 0
    /\ ~\E e \in retired : e.b = b /\ e.t = tokOf[b]
    /\ retired' = retired \cup { [b |-> b, t |-> tokOf[b], r |-> roundOf[l]] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges,
                    everEdged, inflight, wView, wHave >>

\* Fence one PRESENT shard (LIST discovery — no registry universe). Absent shards do not exist to
\* fence. Fence is monotone (a stale leader never lowers it).
GFenceShard(l, s) ==
    /\ gcPhase[l] \in {"retiring", "fencing"}
    /\ sPresent[s] /\ s \notin fencedSet[l]
    /\ Len(log[s]) < MaxLog
    /\ fence'    = [fence    EXCEPT ![s] = IF roundOf[l] > @ THEN roundOf[l] ELSE @]
    /\ log'      = [log      EXCEPT ![s] = Append(@, [op |-> "fence", b |-> "none"])]
    /\ fencePos' = [fencePos EXCEPT ![s] = Len(log[s]) + 1]
    /\ fencedSet'= [fencedSet EXCEPT ![l] = @ \cup {s}]
    /\ gcPhase'  = [gcPhase  EXCEPT ![l] =
                      IF { x \in Shards : sPresent[x] } \subseteq (fencedSet[l] \cup {s})
                      THEN "fenced" ELSE "fencing"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, tomb, gcRound,
                    roundOf, cursor, rootEdges, everEdged, retired, inflight, wView, wHave >>

\* All present shards fenced => fence phase complete (also covers "no present shard").
GFenceDone(l) ==
    /\ gcPhase[l] \in {"retiring", "fencing"}
    /\ { x \in Shards : sPresent[x] } \subseteq fencedSet[l]
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "fenced"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged,
                    retired, inflight, wView, wHave >>

\* Recheck + issue delete. Requires the fold to have reached every present shard's fence position.
GRecheckDelete(l, e) ==
    /\ gcPhase[l] = "fenced" /\ e \in retired /\ e.r = roundOf[l]
    /\ FoldedThroughFence
    /\ IF InDeg(e.b) > 0
       THEN /\ retired'  = retired \ {e}
            /\ inflight' = inflight
       ELSE /\ [b |-> e.b, t |-> e.t] \notin inflight
            /\ retired'  = retired
            /\ inflight' = inflight \cup { [b |-> e.b, t |-> e.t] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges,
                    everEdged, wView, wHave >>

GEndRound(l) ==
    /\ gcPhase[l] = "fenced"
    /\ ~\E e \in retired : e.r = roundOf[l]
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "idle"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sPresent, sInc, sIncMax, refs, fence, log,
                    tomb, gcRound, roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged,
                    retired, inflight, wView, wHave >>

\* Reclaim a ref-shard object like a blob: it must be empty (no committed refs), carry a drop
\* tombstone, AND have its journal FULLY folded under the CURRENT incarnation (so every removal edge
\* is applied before the object is deleted). SabotageDeleteBeforeFold drops the folded precondition
\* -> a still-folded "add" edge orphans (the blob it points at can never reach in-degree 0 -> leak).
GReclaim(s) ==
    /\ sPresent[s] /\ tomb[s] /\ refs[s] = {}
    /\ SabotageDeleteBeforeFold
       \/ (cursor[s].inc = sInc[s] /\ cursor[s].pos >= Len(log[s]))
    /\ sPresent' = [sPresent EXCEPT ![s] = FALSE]
    /\ tomb'     = [tomb     EXCEPT ![s] = FALSE]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, sInc, sIncMax, refs, fence, log, gcRound,
                    gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged, retired,
                    inflight, wView, wHave >>

\* A delete message lands: physically remove the blob at that exact token; the token joins deadTok
\* (INV_NO_RETURN) and the retire entry drops (confirmed outcome).
Land(m) ==
    /\ m \in inflight
    /\ present' = [present EXCEPT ![m.b] = IF tokOf[m.b] = m.t THEN FALSE ELSE @]
    /\ deadTok' = [deadTok EXCEPT ![m.b] = @ \cup {m.t}]
    /\ retired' = { e \in retired : ~(e.b = m.b /\ e.t = m.t) }
    /\ inflight'= inflight \ {m}
    /\ UNCHANGED << tokOf, nextTok, sPresent, sInc, sIncMax, refs, fence, log, tomb, gcRound,
                    gcPhase, roundOf, fencedSet, fencePos, cursor, rootEdges, everEdged, wView, wHave >>

-----------------------------------------------------------------------------
Next ==
    \/ \E w \in Writers, b \in Blobs : WCreateBlob(w, b)
    \/ \E w \in Writers : WRefreshView(w) \/ WAbandon(w)
    \/ \E w \in Writers, s \in Shards, b \in Blobs : WPublish(w, s, b)
    \/ \E s \in Shards, b \in Blobs : WDrop(s, b)
    \/ \E s \in Shards : WTombstone(s)
    \/ \E l \in Leaders : GStartRound(l) \/ GFenceDone(l) \/ GEndRound(l)
    \/ \E l \in Leaders, s \in Shards : GFenceShard(l, s)
    \/ \E s \in Shards : GFold(s) \/ GReclaim(s)
    \/ \E l \in Leaders, b \in Blobs : GRetire(l, b)
    \/ \E l \in Leaders, e \in retired : GRecheckDelete(l, e)
    \/ \E m \in inflight : Land(m)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
StateConstraint ==
    /\ gcRound <= MaxRound
    /\ \A s \in Shards : sIncMax[s] <= MaxInc
    /\ \A s \in Shards : Len(log[s]) <= MaxLog

TypeOK ==
    /\ present  \in [Blobs -> BOOLEAN]
    /\ tokOf    \in [Blobs -> 0..MaxTok]
    /\ deadTok  \in [Blobs -> SUBSET Toks]
    /\ sPresent \in [Shards -> BOOLEAN]
    /\ sInc     \in [Shards -> 0..MaxInc]
    /\ sIncMax  \in [Shards -> 0..MaxInc]
    /\ fence    \in [Shards -> 0..MaxRound]
    /\ tomb     \in [Shards -> BOOLEAN]
    /\ gcRound  \in 0..MaxRound
    /\ gcPhase  \in [Leaders -> {"idle","retiring","fencing","fenced"}]

\* ---- central safety: no-dangling / no-return ----
\* No committed ref points to an absent blob, or to a token that stopped being current (deleted or
\* resurrected). Broken by SabotageNewbornNoFloor (create-race) and by SabotagePathKeyedCursor (ABA
\* on recreate: the fresh incarnation's edge is never folded, GC wrongly retires a live blob).
INV_NO_DANGLING ==
    \A s \in Shards :
        \A e \in refs[s] :
            /\ present[e[1]]
            /\ tokOf[e[1]] = e[2]
            /\ e[2] \notin deadTok[e[1]]

\* ---- blob-leak: no folded edge outlives its shard object ----
\* If GC's snap holds an edge from a shard whose object no longer exists, the target blob's
\* in-degree can never drain -> the blob leaks. Broken by SabotageDeleteBeforeFold.
INV_NO_ORPHAN_EDGE == \A e \in rootEdges : sPresent[e[1]]

=============================================================================
