--------------------------- MODULE CaB140DangleFaithful ---------------------------
(* FAITHFUL B140-dangle model (Job B, 2026-06-18). The Phase-1 CaB140Dangle.tla
   reproduced the marker-without-edges STATE via UNFAITHFUL operations: its
   GStripTree KEPT the marker (real GcSnap::stripTree clears it — expanded.erase,
   CasGcSnap.cpp:218) and its GAdoptGeneration FIELD-MIXED two generations (real
   Gc::fold adoption is whole-generation byte-equal-or-diverge, CasGc.cpp:1086-1115).
   So the producer was not faithfully pinned.

   GROUND TRUTH (Job A, decoded the real dangling=25 soak snaps with the real codec):
     - markers_with_ZERO_child_edges = 0 in ALL three saved generations: the
       "marker-without-edges" signature is ABSENT in reality.
     - The 25 dangling blobs are known=Y, inDeg=0 (gen 671/672), then forgotten
       (known=n) by gen 673; all 25 appear in outcomes_340 with outcome=deleted.
     - known=Y is reachable ONLY via addEdge's known.insert => an edge to each blob
       WAS added once, then its in-degree decremented to 0 (stripTree of a parent).
   So the real mechanism is NOT "marker set, edge never recorded". It is a SHARED
   (deduplicated) blob: blob B is a child of two trees Tdel and Tlive. Tdel's edge is
   folded (B known, in-deg 1), Tdel is cascade-stripped (B -> in-deg 0, marker of Tdel
   cleared), and Tlive's edge to B is NOT present in the folded snap, so the recheck
   computes in-deg 0 and deletes B though Tlive still references it.

   This faithful model therefore uses TWO trees sharing ONE blob and faithful
   edge/in-degree/strip/adopt semantics. The question Job B asks: with the unfaithful
   producers removed and the genuinely-faithful operations of the real fold added, is
   INV_NO_LOSS still violated?

   FAITHFUL operations modeled (cited to CasGc.cpp):
     (i)   displaced_later FILE_DOESNT_EXIST expansion-skip (foldShardRecords ~830):
           a tree object gone at fold time + a later same-ref record => skip expansion,
           marker UNSET, NO edges added.   [GFoldAdd, branch tree_gone]
     (ii)  resident-snap reuse across rounds keyed on generation (CasGc.cpp:1021).
           [modeled: a build seeds from the durable generation; identical bytes reused]
     (iii) cascade probe-upward persist + byte-equal adoption (CasGc.cpp:1086-1115).
           [GAdoptGeneration: whole-generation byte-equal, else probe to NextGen]
     (iv)  relink (adoptTree-style) re-points a ref to an already-folded tree
           (WRelinkTree: last-op-wins republish, no body upload).                       *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Leaders,            \* GC leaders, e.g. {L1, L2}
    Trees,              \* tree hashes, e.g. {t1, t2}
    Blobs,              \* blob hashes, e.g. {b1}
    MaxGen,             \* generation-number bound
    MaxLog,             \* journal length bound
    EnableTreeGone,     \* TRUE = allow a tree object to be gone at fold time (displaced_later skip)
    EnableRelink        \* TRUE = allow WRelinkTree (last-op-wins republish)

ASSUME Trees \cap Blobs = {}
Hashes == Trees \cup Blobs
\* ASYMMETRIC, SELECTIVELY-SHARED children (same pattern as CaIncarnationCore so TLC configs need
\* no function constant). FullTree references EVERY blob; every other tree references exactly ONE
\* blob (OneChild). With Trees={t1,t2}, Blobs={b1,b2}: FullTree(say t2)->{b1,b2}, t1->{b1} — so b1
\* is a SHARED (deduplicated) blob (in both trees) while b2 is private to FullTree. This is exactly
\* Job A's real producer shape: a shared blob whose in-degree counts distinct present parent edges.
\* Evaluated lazily: only ever applied to members of Trees.
FullTree  == CHOOSE t \in Trees : TRUE
OneChild  == CHOOSE b \in Blobs : TRUE
Children(t) == IF t = FullTree THEN Blobs ELSE {OneChild}

\* A snap GENERATION — faithful field set (CasGcSnap.h):
\*   marker    : SUBSET Trees                expanded (markExpanded) trees
\*   treeEdges : SUBSET (Trees X Blobs)       present tree->child edges (set semantics)
\*   rootEdges : SUBSET Trees                 present root edges (a live ref to a tree)
\*   known     : SUBSET Hashes                the `known` set (addEdge/addRootEdge insert)
\*   cursor    : Nat                          fold watermark
\* In-degree of h = (#root edges to h) + (#distinct tree edges into h). With a shared blob,
\* a blob's in-degree is the number of DISTINCT parent trees whose edge is present.
NoGen == [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]

VARIABLES
    present,    \* [Hashes -> BOOLEAN]   durable object exists (for a TREE this IS its object's existence)
    log,        \* Seq(Rec)             the single shard's manifest journal (append-only)
    refs,       \* SUBSET Trees          current live root refs (the live manifest)
    snap,       \* [0..MaxGen -> genRec] per-generation snapshot store
    snapWritten,\* SUBSET (0..MaxGen)
    gcState,    \* [snapGeneration: 0..MaxGen, cursor: Nat]  durable GC pointer
    lease,      \* Leaders UNION {"none"}
    phase,      \* [Leaders -> {"idle","building","retiring"}]
    buildGen,   \* [Leaders -> 0..MaxGen]   generation a leader is building
    retired,    \* SUBSET Hashes
    everLost    \* SUBSET Hashes  (objects GC deleted WHILE reachable — a true-loss witness latch)

vars == << present, log, refs, snap, snapWritten, gcState, lease, phase,
           buildGen, retired, everLost >>

Rec == [op: {"add", "rem"}, h: Trees]

\* ------------------------------------------------------------- helpers
\* A tree's OBJECT existence is exactly present[t] (content-addressed: the object IS the tree).
treeObj(t) == present[t]
InDegGen(g, h) == Cardinality({r \in g.rootEdges : r = h})
                  + Cardinality({e \in g.treeEdges : e[2] = h})
CurGen   == snap[gcState.snapGeneration]
NextGen  == gcState.snapGeneration + 1
Reach(t) == {t} \cup Children(t)
ReachableSet == UNION { Reach(r) : r \in refs }

\* The set of distinct (root_shard, part_name)=ref entries in the journal up to position pos.
\* A "displacement proof" for record at index i (1-based) is a LATER record (op add or rem) for
\* the SAME ref (here ref == the tree's slot; we model one slot per tree id) at a higher position.
DisplacedLater(pos) ==
    LET rec == log[pos] IN
    \E j \in (pos+1)..Len(log) : log[j].h = rec.h

Init ==
    /\ present     = [h \in Hashes |-> FALSE]
    /\ log         = << >>
    /\ refs        = {}
    /\ snap        = [g \in 0..MaxGen |-> NoGen]
    /\ snapWritten = {0}
    /\ gcState     = [snapGeneration |-> 0, cursor |-> 0]
    /\ lease       = "none"
    /\ phase       = [l \in Leaders |-> "idle"]
    /\ buildGen    = [l \in Leaders |-> 0]
    /\ retired     = {}
    /\ everLost    = {}

\* ------------------------------------------------------------- writer / workload
WUploadBlob(b) ==
    /\ ~present[b]
    /\ present' = [present EXCEPT ![b] = TRUE]
    /\ UNCHANGED << log, refs, snap, snapWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* Publish a tree root. Children must be present (bottom-up). Creates the tree OBJECT and a live ref,
\* appends an "add". A blob shared by an already-published tree is simply reused (dedup).
WPublishTree(t) ==
    /\ ~treeObj(t) /\ t \notin refs
    /\ \A c \in Children(t) : present[c]
    /\ Len(log) < MaxLog
    /\ present' = [present EXCEPT ![t] = TRUE]
    /\ refs'    = refs \cup {t}
    /\ log'     = Append(log, [op |-> "add", h |-> t])
    /\ UNCHANGED << snap, snapWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* Drop a tree root (in-degree of the tree from roots goes to 0). Appends a "rem".
WDropTree(t) ==
    /\ t \in refs
    /\ Len(log) < MaxLog
    /\ refs' = refs \ {t}
    /\ log'  = Append(log, [op |-> "rem", h |-> t])
    /\ UNCHANGED << present, snap, snapWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* Relink (last-op-wins republish): re-add a still-present tree object to live refs, append "add".
\* FAITHFUL bottom-up build precondition: a republished manifest's CHILDREN must be present (you
\* cannot publish a tree pointing at a reclaimed blob — Build verifies/uploads children first). This
\* is the same `present[c]` guard WPublishTree carries; without it the relink manufactures a
\* reference to an already-deleted blob, which is a DIFFERENT (relink-validates-children) bug, not
\* the B140 in-degree-fold producer.
WRelinkTree(t) ==
    /\ EnableRelink
    /\ treeObj(t) /\ t \notin refs
    /\ \A c \in Children(t) : present[c]
    /\ Len(log) < MaxLog
    /\ refs' = refs \cup {t}
    /\ log'  = Append(log, [op |-> "add", h |-> t])
    /\ UNCHANGED << present, snap, snapWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* ------------------------------------------------------------- GC lease
GAcquireLease(l) ==
    /\ lease # l
    /\ lease'    = l
    /\ phase'    = [phase EXCEPT ![l] = "building"]
    /\ buildGen' = [buildGen EXCEPT ![l] = NextGen]
    /\ UNCHANGED << present, log, refs, snap, snapWritten, gcState, retired, everLost >>

\* ------------------------------------------------------------- fold (faithful foldShardRecords)
\* Base for the build generation: resident reuse keyed on generation (CasGc.cpp:1021) — if this
\* number is already written, reuse it; else seed from the durable snapGeneration.
BaseOf(l) == LET g == buildGen[l] IN IF g \in snapWritten THEN snap[g] ELSE snap[gcState.snapGeneration]

\* GFold processes the record at the base cursor. For an "add" of tree t with no marker:
\*   - if treeObj[t] is present: expand — add ALL t->child edges (each into `known`), set marker.
\*   - if treeObj[t] is GONE and DisplacedLater holds (a later record for the same slot exists):
\*       displaced_later skip (CasGc.cpp:830-843): NO edges added, marker UNSET. known unchanged.
\*       (foldShardRecords still routes the root edge add via addRootEdge below.)
\*   - if treeObj[t] is GONE and NO later record: fail closed (modeled as no transition; the real
\*       code throws — INV-NO-DANGLE surfaced at fold, never deletes). We simply disallow it.
\* For a "rem": removeRootEdge (root edge gone; in-degree of t from roots drops). The cascade strip
\* of t's child edges is NOT done here (it is the recheck/cascade pipeline step) — faithful.
GFold(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET g    == buildGen[l]
           base == BaseOf(l)
           pos  == base.cursor
       IN
       /\ pos < Len(log)
       /\ LET rec == log[pos + 1]
              t   == rec.h
              isAdd    == rec.op = "add"
              firstExp == isAdd /\ t \notin base.marker
              \* expansion only when the tree object is present at fold time:
              canExpand   == firstExp /\ treeObj(t)
              \* displaced_later skip: object gone but a later same-slot record exists:
              goneSkip    == firstExp /\ ~treeObj(t) /\ DisplacedLater(pos + 1)
              \* fail-closed case (gone, no later record): no enabled transition.
              wellFormed  == isAdd => (treeObj(t) \/ goneSkip)
              newRoot  == IF isAdd THEN base.rootEdges \cup {t} ELSE base.rootEdges \ {t}
              newMark  == IF canExpand THEN base.marker \cup {t} ELSE base.marker
              newTE    == IF canExpand
                          THEN base.treeEdges \cup { <<t, c>> : c \in Children(t) }
                          ELSE base.treeEdges
              newKnown == IF isAdd
                          THEN base.known \cup {t} \cup (IF canExpand THEN Children(t) ELSE {})
                          ELSE base.known
              g2 == [marker |-> newMark, treeEdges |-> newTE, rootEdges |-> newRoot,
                     known |-> newKnown, cursor |-> pos + 1]
          IN /\ wellFormed
             /\ snap'        = [snap EXCEPT ![g] = g2]
             /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << present, log, refs, gcState, lease, phase, buildGen, retired, everLost >>

\* CASCADE STRIP (the recheck/cascade pipeline step, CasGc.cpp:373-383 / CasGcSnap.cpp:204-220):
\* when a tree t is being deleted (it has rootEdge in-degree 0 in this generation, i.e. dropped and
\* not re-added), strip ALL of t's child edges and CLEAR ITS MARKER (expanded.erase). Decrements the
\* in-degree of each shared child. FAITHFUL: marker is cleared, unlike Phase-1 GStripTree.
GCascadeStrip(l, t) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET g    == buildGen[l]
           base == BaseOf(l)
       IN
       /\ base.cursor = Len(log)                  \* cascade runs on a fully-folded generation
       /\ t \in base.marker                       \* t was expanded
       /\ t \notin base.rootEdges                 \* t's root edge is gone (a delete candidate)
       /\ \E c \in Children(t) : <<t, c>> \in base.treeEdges
       /\ LET g2 == [base EXCEPT !.treeEdges = { e \in @ : e[1] # t },   \* edges gone
                                 !.marker    = @ \ {t}]                   \* marker CLEARED (faithful)
          IN /\ snap'        = [snap EXCEPT ![g] = g2]
             /\ snapWritten' = snapWritten \cup {g}
    /\ UNCHANGED << present, log, refs, gcState, lease, phase, buildGen, retired, everLost >>

\* BYTE-EQUAL whole-generation ADOPTION + probe-upward (CasGc.cpp:1086-1115). A leader trying to
\* persist its build generation at number g: if number g already holds bytes EQUAL to what the leader
\* would write, adopt it (no-op, write-once replay); if bytes DIFFER, the generation diverges — the
\* leader must probe to a HIGHER number (buildGen += 1). NO field mixing: the whole record is the
\* unit. Here we model only the consequence relevant to safety: a leader can never overwrite an
\* occupied generation with different bytes; it relocates upward.
GProbeUpward(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET g == buildGen[l] IN
       /\ g \in snapWritten
       /\ snap[g] # BaseOf(l)        \* would-be bytes differ from the occupant => diverged
       /\ g + 1 <= MaxGen
       /\ buildGen' = [buildGen EXCEPT ![l] = g + 1]
       /\ UNCHANGED << present, log, refs, snap, snapWritten, gcState, lease, phase, retired, everLost >>

\* ------------------------------------------------------------- commit / retire / delete
GCommitSnap(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ buildGen[l] \in snapWritten
    /\ gcState' = [snapGeneration |-> buildGen[l], cursor |-> snap[buildGen[l]].cursor]
    /\ phase'   = [phase EXCEPT ![l] = "retiring"]
    /\ UNCHANGED << present, log, refs, snap, snapWritten, lease, buildGen, retired, everLost >>

\* RETIRE: condemn a present, known, in-degree-0 object in the CURRENT durable generation, folded
\* through the whole journal. (zeroInDegreeKnown, CasGcSnap.cpp:250.)
GRetire(l, h) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ CurGen.cursor = Len(log)
    /\ present[h] /\ h \in CurGen.known
    /\ InDegGen(CurGen, h) = 0
    /\ h \notin retired
    /\ retired' = retired \cup {h}
    /\ UNCHANGED << present, log, refs, snap, snapWritten, gcState, lease, phase, buildGen, everLost >>

\* RECHECK + the single content-delete site (CasGc.cpp:226-286). Re-derive in-degree through the
\* fully-folded generation; if still 0 => physical delete. (exact-token elided: the real producer's
\* blob is never re-incarnated, so the exact-token check always passes — Job A: token never bumped.)
GDelete(l, h) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ h \in retired
    /\ CurGen.cursor = Len(log)
    /\ InDegGen(CurGen, h) = 0
    /\ present[h]
    /\ present' = [present EXCEPT ![h] = FALSE]
    \* TRUE-LOSS latch: a delete that hits an object STILL REACHABLE from the live manifest is the
    \* data-loss witness (B140). A delete of an unreachable object is legitimate reclamation.
    /\ everLost' = everLost \cup (IF h \in ReachableSet THEN {h} ELSE {})
    /\ retired' = retired \ {h}
    /\ UNCHANGED << log, refs, snap, snapWritten, gcState, lease, phase, buildGen >>

GEndRound(l) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ phase' = [phase EXCEPT ![l] = "idle"]
    /\ lease' = "none"
    /\ UNCHANGED << present, log, refs, snap, snapWritten, gcState, buildGen, retired, everLost >>

\* Make a tree object disappear from the store WITHOUT a journal record (a competing completed GC
\* round deleted the displaced tree — the precondition for the displaced_later skip). Only a tree
\* that is NOT in live refs and whose root has been dropped in the journal may vanish.
GTreeObjGone(t) ==
    /\ EnableTreeGone
    /\ treeObj(t)
    /\ t \notin refs                          \* unreachable: a competing GC may legitimately reclaim it
    /\ \E i \in 1..Len(log) : log[i].h = t   \* it appeared in the journal (was published)
    /\ present' = [present EXCEPT ![t] = FALSE]   \* the tree OBJECT physically vanishes from the store
    /\ UNCHANGED << log, refs, snap, snapWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* ------------------------------------------------------------- next / spec
Next ==
    \/ \E b \in Blobs : WUploadBlob(b)
    \/ \E t \in Trees : WPublishTree(t) \/ WDropTree(t) \/ WRelinkTree(t) \/ GTreeObjGone(t)
    \/ \E l \in Leaders : GAcquireLease(l) \/ GFold(l) \/ GProbeUpward(l)
                          \/ GCommitSnap(l) \/ GEndRound(l)
    \/ \E l \in Leaders, t \in Trees : GCascadeStrip(l, t)
    \/ \E l \in Leaders, h \in Hashes : GRetire(l, h) \/ GDelete(l, h)

Spec == Init /\ [][Next]_vars

\* ------------------------------------------------------------- invariants
TypeOK ==
    /\ present \in [Hashes -> BOOLEAN]
    /\ log \in Seq(Rec)
    /\ refs \subseteq Trees
    /\ lease \in Leaders \cup {"none"}
    /\ phase \in [Leaders -> {"idle", "building", "retiring"}]
    /\ gcState.snapGeneration \in 0..MaxGen
    /\ retired \subseteq Hashes
    /\ everLost \subseteq Hashes

\* No live reference resolves to a deleted/absent object (the data-loss property — INV-NO-LOSS).
INV_NO_LOSS == \A h \in ReachableSet : present[h]

\* In the CURRENT durable generation, every markExpanded LIVE tree has ALL its child edges.
INV_MARKER_EDGES ==
    \A t \in Trees :
        ( t \in CurGen.marker /\ t \in refs )
        => \A c \in Children(t) : <<t, c>> \in CurGen.treeEdges

\* No object was ever physically deleted by GC WHILE reachable from the live manifest (the
\* true-loss latch). This is the sharpest B140 witness: even if a later re-publish heals `present`,
\* a delete that struck a then-reachable object is a data-loss event.
INV_NO_GC_LOSS == everLost = {}

StateConstraint ==
    /\ Len(log) <= MaxLog
    /\ gcState.snapGeneration <= MaxGen
    /\ \A l \in Leaders : buildGen[l] <= MaxGen
=======================================================================================
