------------------------------ MODULE CaB140Dangle ------------------------------
\* =====================================================================================
\* SUPERSEDED-AS-PRODUCER (2026-06-23 currency review). This Phase-1 repro uses UNFAITHFUL
\* producers (marker-retaining strip, field-mixed generation adoption) — refuted by
\* CaB140DangleFaithful.tla and superseded by the faithful reproduction+fix CaB140DangleMerge.tla.
\* The CURRENT B140 fix model is CaBuildRootPrecommit.tla. Kept as the reasoning-chain record.
\* Details: MODEL_CURRENCY_REVIEW_2026-06-22.md.
\* =====================================================================================
(* Phase 1 of the B140-dangle fix (spec 2026-06-17-ca-b140-dangle-fix-design.md).
   FOCUSED, SELF-CONTAINED model whose ONLY job is to EXPRESS and REPRODUCE the
   B140-dangle data-loss bug — it is NOT the full CaIncarnationCore. It abstracts
   away tokens / writers / heartbeats / registry / evidence and keeps exactly the
   machinery the producer needs: a LIVE root referencing a tree T whose child is a
   blob B, a PER-GENERATION GC snapshot store, NON-ATOMIC tree expansion, byte-equal
   generation adoption across leaders, and a lease-steal that orphans a generation.

   THE BUG (INV-MARKER-EDGES / INV-NO-LOSS):
     A markExpanded tree T that is still LIVE (in-degree >= 1) ends up WITHOUT its
     T->B child edge in the durable snap generation a GC round folds/retires against.
     Then B is computed in-degree 0, retire condemns it, and the content-delete
     deletes a blob the live ref still references — data loss.

   The whole point is cross-GENERATION incoherence: a generation that mixes T's
   markExpanded marker (a sticky bit) with a DIFFERENT generation's stripped edges,
   enabled by generations being byte-comparable without recording the fold position.

   FEATURE FLAG: EnableSplitExpand. With it FALSE the model expands a tree
   ATOMICALLY (edges+marker in one step, like the original GFold) and adoption is
   coherent — the existing safety invariants must HOLD. With it TRUE the producer
   actions are armed and the model REACHES the dangle. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Leaders,            \* GC leaders that compete for the lease, e.g. {L1, L2}
    Trees,              \* tree hashes, e.g. {t1}
    Blobs,              \* blob (non-tree) hashes, e.g. {b1}
    MaxGen,             \* generation-number bound (snap generations 0..MaxGen)
    MaxLog,             \* journal length bound
    EnableSplitExpand,  \* TRUE = arm the producer (non-atomic expansion: GAddTreeEdges/GMarkExpanded)
    EnableInBuildStrip, \* TRUE = allow GStripTree (cascade strip keeping the sticky marker)
    EnableLeaseSteal,   \* TRUE = allow the orphaned-generation lease-steal transition
    EnableAdopt         \* TRUE = allow byte-equal cross-leader generation adoption (field mix)

ASSUME Trees \cap Blobs = {}
Hashes == Trees \cup Blobs

\* Derived child map: every tree references every blob (one-level closure, like the core).
Children(t) == Blobs

AddRec == [op: {"add", "rem"}, h: Hashes]
Rec    == AddRec

\* A snap GENERATION is the durable folded view at some fold position:
\*   marker    : SUBSET Trees        expansion markers (sticky bit)
\*   treeEdges : SUBSET (Trees X Blobs)  recorded tree->child edges
\*   rootEdges : SUBSET Hashes        folded root refs (in-degree from roots)
\*   everEdged : SUBSET Hashes        journal-known
\*   cursor    : Nat                  fold watermark (#journal records folded)
NoGen == [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, everEdged |-> {}, cursor |-> 0]

VARIABLES
    present,    \* [Hashes -> BOOLEAN]   durable object exists
    tok,        \* [Hashes -> Nat]       current incarnation token (bumped on re-incarnation/relink)
    log,        \* Seq(Rec)              the single shard's manifest journal (append-only)
    refs,       \* SUBSET Hashes         current live root refs (the live manifest)
    snap,       \* [0..MaxGen -> genRec UNION {NoGen}]  per-generation snapshot store
    snapWritten,\* SUBSET (0..MaxGen)    which generations have been durably written
    gcState,    \* [snapGeneration: 0..MaxGen, cursor: Nat]  the durable GC pointer (snap+fold)
    lease,      \* Leaders UNION {"none"}   who currently holds the GC lease
    phase,      \* [Leaders -> {"idle","building","retiring"}]  per-leader round PC
    buildGen,   \* [Leaders -> 0..MaxGen]   the generation a leader is currently building
    retired,    \* SUBSET Hashes           condemned-this-round (in-degree-0 in the folded snap)
    inflight    \* SUBSET [h: Hashes, t: Nat]  content-delete messages in flight (EXACT token)

vars == << present, tok, log, refs, snap, snapWritten, gcState, lease, phase,
           buildGen, retired, inflight >>
InflightHashes == { d.h : d \in inflight }

\* ------------------------------------------------------------- helpers
\* In-degree of h in a GENERATION record g: roots referencing h + tree edges into h.
InDegGen(g, h) == Cardinality({r \in g.rootEdges : r = h})
                  + Cardinality({e \in g.treeEdges : e[2] = h})

\* The durable snap generation the GC currently reads (its fold target).
CurGen == snap[gcState.snapGeneration]

\* Reachable set from the LIVE manifest refs (ground truth — what must never be deleted).
Reach(r)     == {r} \cup (IF r \in Trees THEN Children(r) ELSE {})
ReachableSet == UNION { Reach(r) : r \in refs }

NextGen == gcState.snapGeneration + 1

Init ==
    /\ present     = [h \in Hashes |-> FALSE]
    /\ tok         = [h \in Hashes |-> 0]
    /\ log         = << >>
    /\ refs        = {}
    /\ snap        = [g \in 0..MaxGen |-> NoGen]
    /\ snapWritten = {0}                       \* generation 0 is the empty durable baseline
    /\ gcState     = [snapGeneration |-> 0, cursor |-> 0]
    /\ lease       = "none"
    /\ phase       = [l \in Leaders |-> "idle"]
    /\ buildGen    = [l \in Leaders |-> 0]
    /\ retired     = {}
    /\ inflight    = {}

\* ------------------------------------------------------------- writer / workload actions
\* Upload a blob (must exist before a tree referencing it is published).
WUploadBlob(b) ==
    /\ ~present[b]
    /\ present' = [present EXCEPT ![b] = TRUE]
    /\ UNCHANGED << tok, log, refs, snap, snapWritten, gcState, lease, phase, buildGen, retired, inflight >>

\* Publish a tree root: its children must be present (bottom-up build). Appends an "add".
WPublishTree(t) ==
    /\ ~present[t] /\ t \notin refs
    /\ \A c \in Children(t) : present[c]
    /\ Len(log) < MaxLog
    /\ present' = [present EXCEPT ![t] = TRUE]
    /\ refs'    = refs \cup {t}
    /\ log'     = Append(log, [op |-> "add", h |-> t])
    /\ UNCHANGED << tok, snap, snapWritten, gcState, lease, phase, buildGen, retired, inflight >>

\* Drop a tree root from the live manifest (in-degree of T from roots goes to 0). Appends a "rem".
WDropTree(t) ==
    /\ t \in refs
    /\ Len(log) < MaxLog
    /\ refs' = refs \ {t}
    /\ log'  = Append(log, [op |-> "rem", h |-> t])
    /\ UNCHANGED << tok, present, snap, snapWritten, gcState, lease, phase, buildGen, retired, inflight >>

\* Re-publish (relink) a tree root that is still present — the adoptFromTree tokenless re-pin.
\* Re-adds T to the LIVE refs and appends an "add". This is the relink that re-makes T live
\* after it was dropped, WITHOUT a fresh body upload (T's blobs are assumed still present).
WRelinkTree(t) ==
    /\ present[t] /\ t \notin refs
    /\ Len(log) < MaxLog
    /\ refs' = refs \cup {t}
    /\ log'  = Append(log, [op |-> "add", h |-> t])
    /\ tok'  = [tok EXCEPT ![t] = @ + 1]    \* re-incarnation: a stale in-flight delete now 412s
    /\ UNCHANGED << present, snap, snapWritten, gcState, lease, phase, buildGen, retired, inflight >>

\* ------------------------------------------------------------- GC lease
GAcquireLease(l) ==
    /\ lease # l
    /\ lease'   = l
    /\ phase'   = [phase EXCEPT ![l] = "building"]
    /\ buildGen'= [buildGen EXCEPT ![l] = NextGen]    \* a round builds the NEXT generation
    /\ UNCHANGED << tok, present, log, refs, snap, snapWritten, gcState, retired, inflight >>

\* LEASE-STEAL between retire/cascade and the closing gc/state CAS (the orphaned-generation
\* transition). Another leader m grabs the lease while l is mid-round; l's in-progress
\* generation (buildGen[l]) was persisted to snap but gc/state was NOT advanced to it — it is
\* ORPHANED. The stealer starts its own round from the still-durable gcState.snapGeneration.
GStealLease(m) ==
    /\ EnableLeaseSteal
    /\ lease # "none" /\ lease # m
    /\ LET victim == lease IN phase[victim] = "retiring"
    /\ lease'   = m
    /\ phase'   = [phase EXCEPT ![m] = "building", ![lease] = "idle"]
    /\ buildGen'= [buildGen EXCEPT ![m] = NextGen]
    /\ UNCHANGED << tok, present, log, refs, snap, snapWritten, gcState, retired, inflight >>

\* ------------------------------------------------------------- fold / expand (the core)
\* Fold the next journal record into the leader's build generation. Seeds the generation from
\* the leader's CURRENT durable snap base (gcState.snapGeneration) the first time it touches
\* its build generation, then applies the record. add => rootEdges += h (+ everEdged); for a
\* tree, expansion is EITHER atomic (flag off) OR split into GAddTreeEdges + GMarkExpanded.
\* rem => rootEdges -= h.
GFold(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET g    == buildGen[l]
           base == IF g \in snapWritten THEN snap[g] ELSE snap[gcState.snapGeneration]
           pos  == base.cursor
       IN
       /\ pos < Len(log)
       /\ LET rec == log[pos + 1]
              addEdges == IF rec.op = "add" THEN base.rootEdges \cup {rec.h}
                                            ELSE base.rootEdges \ {rec.h}
              \* atomic-expand path (flag OFF): a tree add records edges + marker together.
              doAtomic == rec.op = "add" /\ rec.h \in Trees /\ rec.h \notin base.marker
                          /\ ~EnableSplitExpand
              newMarker == IF doAtomic THEN base.marker \cup {rec.h} ELSE base.marker
              newTEdges == IF doAtomic
                           THEN base.treeEdges \cup { <<rec.h, c>> : c \in Children(rec.h) }
                           ELSE base.treeEdges
              newEver   == IF rec.op = "add"
                           THEN base.everEdged \cup {rec.h}
                                \cup (IF doAtomic THEN Children(rec.h) ELSE {})
                           ELSE base.everEdged
              g2 == [marker |-> newMarker, treeEdges |-> newTEdges, rootEdges |-> addEdges,
                     everEdged |-> newEver, cursor |-> pos + 1]
          IN /\ snap'        = [snap EXCEPT ![g] = g2]
             /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << tok, present, log, refs, gcState, lease, phase, buildGen, retired, inflight >>

\* SPLIT expansion sub-action 1 (flag ON): record T's child edges in the build generation,
\* WITHOUT setting the marker. Models foldShardRecords recording edges as a separate write.
GAddTreeEdges(l, t) ==
    /\ EnableSplitExpand
    /\ lease = l /\ phase[l] = "building"
    /\ LET g    == buildGen[l]
           base == IF g \in snapWritten THEN snap[g] ELSE snap[gcState.snapGeneration]
       IN
       /\ t \in base.everEdged             \* T has been folded (its add was seen)
       /\ t \notin base.marker             \* not already expanded
       /\ ~(\A c \in Children(t) : <<t, c>> \in base.treeEdges)   \* some edge still missing
       /\ LET g2 == [base EXCEPT !.treeEdges = @ \cup { <<t, c>> : c \in Children(t) },
                                 !.everEdged = @ \cup Children(t)]
          IN /\ snap'        = [snap EXCEPT ![g] = g2]
             /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << tok, present, log, refs, gcState, lease, phase, buildGen, retired, inflight >>

\* SPLIT expansion sub-action 2 (flag ON): set the markExpanded MARKER (the sticky bit),
\* INDEPENDENTLY of whether the edges were recorded. This is the non-atomic gap: an
\* interleaving (or an adopted base that already carries the marker) can mark T without edges.
GMarkExpanded(l, t) ==
    /\ EnableSplitExpand
    /\ lease = l /\ phase[l] = "building"
    /\ LET g    == buildGen[l]
           base == IF g \in snapWritten THEN snap[g] ELSE snap[gcState.snapGeneration]
       IN
       /\ t \in base.everEdged
       /\ t \notin base.marker
       /\ LET g2 == [base EXCEPT !.marker = @ \cup {t}]
          IN /\ snap'        = [snap EXCEPT ![g] = g2]
             /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << tok, present, log, refs, gcState, lease, phase, buildGen, retired, inflight >>

\* STRIP a tree's child edges in the leader's build generation, KEEPING the markExpanded marker
\* (the sticky bit) and the children in everEdged. Models GcSnap::stripTree / a cascade strip that
\* clears T->B edges but leaves the marker set in this generation (the marker is durable/sticky and
\* the codec does not re-derive it from the edge set). This is the generation that, mixed with a
\* marker-bearing generation, produces the incoherence.
GStripTree(l, t) ==
    /\ EnableInBuildStrip
    /\ lease = l /\ phase[l] = "building"
    /\ LET g    == buildGen[l]
           base == IF g \in snapWritten THEN snap[g] ELSE snap[gcState.snapGeneration]
       IN
       /\ t \in base.marker                                  \* T was expanded (marker set)...
       /\ \E c \in Children(t) : <<t, c>> \in base.treeEdges  \* ...and at least one edge present
       /\ LET g2 == [base EXCEPT !.treeEdges = { e \in @ : e[1] # t }]  \* edges gone, marker stays
          IN /\ snap'        = [snap EXCEPT ![g] = g2]
             /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << tok, present, log, refs, gcState, lease, phase, buildGen, retired, inflight >>

\* BYTE-EQUAL generation ADOPTION (the putIfAbsent byte-equal adoption / probe-upward). The snap
\* codec does NOT record the fold cursor as part of the generation's IDENTITY (the design's key
\* enabler), so two generations at the same NUMBER produced by divergent folds are byte-comparable.
\* A leader adopts the durably-written bytes at its build-generation number, MIXING fields: it takes
\* the OTHER generation's marker (the sticky bit) and everEdged but the (possibly stripped) edges of
\* whichever generation is durable. We model the concrete incoherence: the adopter constructs a
\* generation whose MARKER+everEdged come from a marker-bearing generation gm at a DIFFERENT number
\* while its treeEdges/rootEdges come from the stripped durable base — exactly a marker-gen mixed
\* with a stripped-gen.
GAdoptGeneration(l) ==
    /\ EnableAdopt
    /\ lease = l /\ phase[l] = "building"
    /\ LET g == buildGen[l] IN
       /\ \E gm \in snapWritten :
            /\ snap[gm].marker # {}                       \* gm carries a sticky marker
            /\ g \in snapWritten                          \* bytes already exist at this number
            /\ LET cur == snap[g]
                   g2  == [ marker    |-> cur.marker \cup snap[gm].marker,   \* sticky marker adopted
                            treeEdges |-> cur.treeEdges,                     \* stripped edges KEPT
                            rootEdges |-> cur.rootEdges,
                            everEdged |-> cur.everEdged \cup snap[gm].everEdged,
                            cursor    |-> cur.cursor ]                       \* cursor NOT part of identity
               IN /\ g2 # cur                              \* a real mix (not already coherent)
                  /\ snap'        = [snap EXCEPT ![g] = g2]
                  /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << tok, present, log, refs, gcState, lease, phase, buildGen, retired, inflight >>

\* ------------------------------------------------------------- retire / delete (against the snap)
\* RETIRE: condemn a present, journal-known, in-degree-0 (in the CURRENT durable snap generation)
\* blob. THIS IS THE FOLD-OUTPUT CONSUMER: it reads the generation gcState points at. If that
\* generation has T markExpanded but missing the T->B edge, B's in-degree is computed 0 here.
GRetire(l, h) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ CurGen.cursor = Len(log)            \* retire observes a snap folded through the whole journal
    /\ present[h] /\ h \in CurGen.everEdged
    /\ InDegGen(CurGen, h) = 0
    /\ h \notin retired
    /\ retired' = retired \cup {h}
    /\ UNCHANGED << tok, present, log, refs, snap, snapWritten, gcState, lease, phase, buildGen, inflight >>

\* Advance to retiring: the leader's build generation becomes the durable gc/state ONLY if it
\* did NOT lose the lease. (Commits the built generation as the new durable snap pointer.)
GCommitSnap(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ buildGen[l] \in snapWritten
    /\ gcState' = [snapGeneration |-> buildGen[l], cursor |-> snap[buildGen[l]].cursor]
    /\ phase'   = [phase EXCEPT ![l] = "retiring"]
    /\ UNCHANGED << tok, present, log, refs, snap, snapWritten, lease, buildGen, retired, inflight >>

\* Issue the content-delete for a condemned blob (the single delete site). In flight.
\* FENCE + RECHECK-FOLD barrier (modeled from the core's FoldedThroughFence + GRecheckDelete):
\* the durable snap must have folded through the ENTIRE current journal (fence then recheck-fold),
\* and the object's in-degree must STILL be 0 in that fully-folded snap. This blocks the simple
\* stale-cursor dangle (a relink/publish that the snap has not yet folded). It does NOT save the
\* B140 producer: there the snap IS fully folded (cursor = Len(log)) yet the generation is
\* INTERNALLY incoherent (marker set, child edge stripped), so the recheck still computes in-deg 0.
GIssueDelete(l, h) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ h \in retired /\ h \notin InflightHashes
    /\ CurGen.cursor = Len(log)               \* folded through the fence (whole journal)
    /\ InDegGen(CurGen, h) = 0                 \* recheck against the fully-folded snap
    /\ inflight' = inflight \cup { [h |-> h, t |-> tok[h]] }   \* capture the EXACT token now
    /\ UNCHANGED << tok, present, log, refs, snap, snapWritten, gcState, lease, phase, buildGen, retired >>

\* The content-delete lands: EXACT-token. If the object was re-incarnated since the delete was
\* issued (tok bumped by a relink), the delete is a 412 NO-OP — it removes nothing, just drops the
\* in-flight message. This is the incarnation-token safety that catches the benign relink-after-
\* issue TOCTOU. The B140 producer deletes a blob whose token was NEVER bumped (it was never
\* relinked — only its tree-parent edge was lost), so the exact-token delete SUCCEEDS and loses it.
GDeleteLand(d) ==
    /\ d \in inflight
    /\ inflight' = inflight \ {d}
    /\ retired'  = retired \ {d.h}
    /\ IF present[d.h] /\ tok[d.h] = d.t
       THEN present' = [present EXCEPT ![d.h] = FALSE]      \* exact-token hit: physical delete
       ELSE present' = present                              \* 412: stale token, no-op
    /\ UNCHANGED << tok, log, refs, snap, snapWritten, gcState, lease, phase, buildGen >>

\* End the round: leader goes idle, releases the lease.
GEndRound(l) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ phase' = [phase EXCEPT ![l] = "idle"]
    /\ lease' = "none"
    /\ UNCHANGED << tok, present, log, refs, snap, snapWritten, gcState, buildGen, retired, inflight >>

\* ------------------------------------------------------------- next / spec
Next ==
    \/ \E b \in Blobs : WUploadBlob(b)
    \/ \E t \in Trees : WPublishTree(t) \/ WDropTree(t) \/ WRelinkTree(t)
    \/ \E l \in Leaders : GAcquireLease(l) \/ GStealLease(l) \/ GFold(l)
                          \/ GCommitSnap(l) \/ GAdoptGeneration(l) \/ GEndRound(l)
    \/ \E l \in Leaders, t \in Trees : GAddTreeEdges(l, t) \/ GMarkExpanded(l, t) \/ GStripTree(l, t)
    \/ \E l \in Leaders, h \in Hashes : GRetire(l, h) \/ GIssueDelete(l, h)
    \/ \E d \in inflight : GDeleteLand(d)

Spec == Init /\ [][Next]_vars

\* ------------------------------------------------------------- invariants
TypeOK ==
    /\ present \in [Hashes -> BOOLEAN]
    /\ log \in Seq(Rec)
    /\ refs \subseteq Hashes
    /\ lease \in Leaders \cup {"none"}
    /\ phase \in [Leaders -> {"idle", "building", "retiring"}]
    /\ gcState.snapGeneration \in 0..MaxGen
    /\ tok \in [Hashes -> 0..MaxLog]
    /\ retired \subseteq Hashes
    /\ inflight \subseteq [h: Hashes, t: 0..MaxLog]

\* THE NEW INVARIANT. In the CURRENT durable snap generation, every markExpanded tree T that is
\* still live (in-degree > 0 from the LIVE manifest refs) has ALL its tree->blob child edges present.
INV_MARKER_EDGES ==
    \A t \in Trees :
        ( t \in CurGen.marker /\ t \in refs )
        => \A c \in Children(t) : <<t, c>> \in CurGen.treeEdges

\* No live reference resolves to an absent object (data loss). Ground truth vs durable presence.
INV_NO_LOSS == \A h \in ReachableSet : present[h]

\* An in-flight delete that would HIT (exact token still current) must not target a reachable
\* object. A STALE-token in-flight delete (token superseded by a relink/re-incarnation) is benign —
\* it 412s on landing. `retired` is also provisional (re-gated at GIssueDelete). The data-loss
\* property is INV_NO_LOSS; this is the sharper "an effective delete is never wrong" facet.
INV_NO_DANGLE == \A d \in inflight :
                    ( tok[d.h] = d.t /\ present[d.h] ) => d.h \notin ReachableSet

\* SHARPENED B140-dangle target: a BLOB child of a markExpanded LIVE tree must never be condemned.
\* This isolates the spec's exact mechanism (live markExpanded T, missing T->B edge => B in-deg 0
\* => B condemned/deleted) from the simpler stale-root-cursor dangle.
LiveMarkedTrees == { t \in Trees : t \in refs /\ t \in CurGen.marker }
EffectiveDelete(h) == \E d \in inflight : d.h = h /\ d.t = tok[h] /\ present[h]
INV_BLOB_NOT_CONDEMNED ==
    \A t \in LiveMarkedTrees :
        \A c \in Children(t) : ~EffectiveDelete(c) /\ present[c]

StateConstraint ==
    /\ Len(log) <= MaxLog
    /\ gcState.snapGeneration <= MaxGen
    /\ \A l \in Leaders : buildGen[l] <= MaxGen
    /\ \A h \in Hashes : tok[h] <= MaxLog
    /\ Cardinality(inflight) <= 2
=======================================================================================
