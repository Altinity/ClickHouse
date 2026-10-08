--------------------------- MODULE CaB140DangleMerge ---------------------------
(* B140-dangle: the THREE-separate-durable-structures producer + the MERGE fix
   (2026-06-18). Builds on CaB140DangleFaithful.tla, which exhausted clean because it
   (a) kept the fold cursor INSIDE each generation record (so byte-equal adoption is
   cursor-coherent) and (b) never TRIMMED the journal. Reality keeps three SEPARATELY
   durable pieces: snap EDGES (gc/snap/<gen>), the folded CURSOR (gc/state), and the
   untrimmed JOURNAL tail (in the root manifests, trimmed for size — B164/B160). The
   dangle lives in the gap between them across leaders.

   PRODUCER modeled here (trim-before-durable; faithful to the real fold/persist split):
     A GC leader folds the journal into an IN-MEMORY work-in-progress generation (wip),
     persists it to durable storage (durSnap) only at a checkpoint, and commits via
     gcState. The journal is TRIMMED based on the leader's IN-MEMORY fold cursor — before
     that fold is durable. If the leader then loses the lease (its wip is discarded), the
     edges it folded are GONE (never persisted) AND the journal records are trimmed. A
     fresh leader rebuilds from the COMMITTED durable snap (which is behind the trim) and
     hits a GAP over the trimmed records: it resumes from the journal head without their
     edges. A live ref's tree->blob edge lost in that gap is never in any durable snap.
     When the counted parent is stripped, the shared blob hits in-degree 0 and is deleted
     while a live ref still references it => INV_NO_LOSS.

   FIX modeled (the user's "merge {edges,cursor} + trim-gate"):
     TrimGated = TRUE => the journal may be trimmed only up to gcState.cursor (the
     COMMITTED snap's cursor). Then any leader seeding from the committed snap has
     base.cursor = gcState.cursor >= logBase => no gap => the replay is always faithful,
     because every trimmed record's edges are durable in the committed snap.

   The shared blob is essential: Trees={t1,t2}, Blobs={b1}; BOTH t1 and t2 reference b1
   (dedup). t1 is the "counted" parent that gets dropped+stripped; t2 is the live ref
   whose edge is lost in the trim gap. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Leaders,            \* GC leaders, e.g. {L1, L2}
    Trees,              \* tree hashes, e.g. {t1, t2}
    Blobs,              \* blob hashes, e.g. {b1}
    MaxGen,             \* generation-number bound
    MaxLog,             \* journal length bound (absolute positions)
    TrimGated,          \* FALSE = buggy (trim up to any leader's IN-MEMORY fold); TRUE = fix (trim up to committed cursor)
    CursorInSnap        \* FALSE = buggy (cursor is durable SEPARATELY from edges; split commit can run ahead);
                        \* TRUE  = fix (cursor is part of the committed snap; commit is atomic edges+own-extent)

ASSUME Trees \cap Blobs = {}
Hashes == Trees \cup Blobs
FullTree  == CHOOSE t \in Trees : TRUE
OneChild  == CHOOSE b \in Blobs : TRUE
Children(t) == IF t = FullTree THEN Blobs ELSE {OneChild}

\* A snap GENERATION (faithful field set; cursor is ABSOLUTE over the never-renumbered journal).
NoGen == [marker |-> {}, treeEdges |-> {}, rootEdges |-> {}, known |-> {}, cursor |-> 0]

VARIABLES
    present,    \* [Hashes -> BOOLEAN]   durable object exists
    log,        \* Seq(Rec)             the LIVE (untrimmed) journal tail
    logBase,    \* Nat                  count of trimmed records; abs position of log[i] is logBase+i
    refs,       \* SUBSET Trees          current live root refs (the live manifest = TRUTH)
    durSnap,    \* [0..MaxGen -> genRec]  DURABLE persisted snaps (write-once)
    durWritten, \* SUBSET (0..MaxGen)
    gcState,    \* [snapGeneration: 0..MaxGen, cursor: Nat]  durable committed GC pointer
    lease,      \* Leaders UNION {"none"}
    phase,      \* [Leaders -> {"idle","building","retiring"}]
    wip,        \* [Leaders -> genRec]    IN-MEMORY work-in-progress fold (lost on EndRound)
    buildGen,   \* [Leaders -> 0..MaxGen]
    retired,    \* SUBSET Hashes
    everLost    \* SUBSET Hashes  (objects GC deleted WHILE reachable — a true-loss witness latch)

vars == << present, log, logBase, refs, durSnap, durWritten, gcState, lease, phase,
           wip, buildGen, retired, everLost >>

Rec == [op: {"add", "rem"}, h: Trees]

\* ------------------------------------------------------------- helpers
treeObj(t) == present[t]
AbsLen     == logBase + Len(log)
InDegGen(g, h) == Cardinality({r \in g.rootEdges : r = h})
                  + Cardinality({e \in g.treeEdges : e[2] = h})
CurGen   == durSnap[gcState.snapGeneration]
NextGen  == gcState.snapGeneration + 1
Reach(t) == {t} \cup Children(t)
ReachableSet == UNION { Reach(r) : r \in refs }

\* The trim bound. BUGGY: the max fold cursor across durable snaps AND the in-memory wip of
\* any building leader (a leader trims by its own in-flight fold, before persisting). FIX: only
\* the committed durable cursor.
BuildingCursors == { wip[l].cursor : l \in { ll \in Leaders : phase[ll] = "building" } }
DurCursors      == { durSnap[g].cursor : g \in durWritten }
MaxBuggyCursor  == LET cs == DurCursors \cup BuildingCursors \cup {gcState.cursor} IN
                   CHOOSE m \in cs : \A c \in cs : c <= m

Init ==
    /\ present     = [h \in Hashes |-> FALSE]
    /\ log         = << >>
    /\ logBase     = 0
    /\ refs        = {}
    /\ durSnap     = [g \in 0..MaxGen |-> NoGen]
    /\ durWritten  = {0}
    /\ gcState     = [snapGeneration |-> 0, cursor |-> 0]
    /\ lease       = "none"
    /\ phase       = [l \in Leaders |-> "idle"]
    /\ wip         = [l \in Leaders |-> NoGen]
    /\ buildGen    = [l \in Leaders |-> 0]
    /\ retired     = {}
    /\ everLost    = {}

\* ------------------------------------------------------------- writer / workload
WUploadBlob(b) ==
    /\ ~present[b]
    /\ present' = [present EXCEPT ![b] = TRUE]
    /\ UNCHANGED << log, logBase, refs, durSnap, durWritten, gcState, lease, phase, wip, buildGen, retired, everLost >>

WPublishTree(t) ==
    /\ ~treeObj(t) /\ t \notin refs
    /\ \A c \in Children(t) : present[c]
    /\ AbsLen < MaxLog
    /\ present' = [present EXCEPT ![t] = TRUE]
    /\ refs'    = refs \cup {t}
    /\ log'     = Append(log, [op |-> "add", h |-> t])
    /\ UNCHANGED << logBase, durSnap, durWritten, gcState, lease, phase, wip, buildGen, retired, everLost >>

WDropTree(t) ==
    /\ t \in refs
    /\ AbsLen < MaxLog
    /\ refs' = refs \ {t}
    /\ log'  = Append(log, [op |-> "rem", h |-> t])
    /\ UNCHANGED << present, logBase, durSnap, durWritten, gcState, lease, phase, wip, buildGen, retired, everLost >>

\* ------------------------------------------------------------- journal TRIM
GTrim ==
    /\ LET bound == IF TrimGated THEN gcState.cursor ELSE MaxBuggyCursor IN
       \E nb \in (logBase+1)..bound :
          /\ logBase' = nb
          /\ log'     = SubSeq(log, (nb - logBase) + 1, Len(log))
    /\ UNCHANGED << present, refs, durSnap, durWritten, gcState, lease, phase, wip, buildGen, retired, everLost >>

\* ------------------------------------------------------------- GC lease
\* A fresh leader seeds its IN-MEMORY wip from the COMMITTED durable snap (it does NOT inherit
\* another node's in-memory build). buildGen starts at NextGen.
GAcquireLease(l) ==
    /\ lease # l
    /\ lease'    = l
    /\ phase'    = [phase EXCEPT ![l] = "building"]
    /\ buildGen' = [buildGen EXCEPT ![l] = NextGen]
    /\ wip'      = [wip EXCEPT ![l] = CurGen]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, gcState, retired, everLost >>

\* ------------------------------------------------------------- fold (faithful + trim gap), in-memory
GFold(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET base == wip[l]
           pos  == wip[l].cursor
       IN
       \/ /\ pos < logBase                                  \* ---- trim GAP: trimmed records lost
          /\ wip' = [wip EXCEPT ![l] = [base EXCEPT !.cursor = logBase]]
       \/ /\ pos >= logBase /\ pos < AbsLen                 \* ---- normal fold of abs record pos+1
          /\ LET rec      == log[(pos + 1) - logBase]
                 t        == rec.h
                 isAdd    == rec.op = "add"
                 firstExp == isAdd /\ t \notin base.marker
                 canExpand== firstExp /\ treeObj(t)
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
             IN /\ isAdd => treeObj(t)
                /\ wip' = [wip EXCEPT ![l] = g2]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* CASCADE STRIP on the in-memory wip (faithful — clears marker too).
GCascadeStrip(l, t) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET base == wip[l] IN
       /\ base.cursor = AbsLen                              \* cascade runs on a fully-folded generation
       /\ t \in base.marker
       /\ t \notin base.rootEdges
       /\ \E c \in Children(t) : <<t, c>> \in base.treeEdges
       /\ wip' = [wip EXCEPT ![l] = [base EXCEPT !.treeEdges = { e \in @ : e[1] # t },
                                                 !.marker    = @ \ {t}]]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, gcState, lease, phase, buildGen, retired, everLost >>

\* PERSIST the in-memory wip to durable storage at buildGen (write-once putIfAbsent). If the
\* number is free, write. If occupied by EQUAL bytes, adopt (no-op). Occupied by DIFFERENT bytes
\* is handled by GProbeUpward (relocate to a higher number).
GPersist(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET g == buildGen[l] IN
       /\ g \notin durWritten
       /\ durSnap'    = [durSnap EXCEPT ![g] = wip[l]]
       /\ durWritten' = durWritten \cup {g}
    /\ UNCHANGED << present, log, logBase, refs, gcState, lease, phase, wip, buildGen, retired, everLost >>

GProbeUpward(l) ==
    /\ lease = l /\ phase[l] = "building"
    /\ LET g == buildGen[l] IN
       /\ g \in durWritten
       /\ durSnap[g] # wip[l]
       /\ g + 1 <= MaxGen
       /\ buildGen' = [buildGen EXCEPT ![l] = g + 1]
       /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, gcState, lease, phase, wip, retired, everLost >>

\* ------------------------------------------------------------- commit / retire / delete
\* MERGE (CursorInSnap=TRUE): commit is ONE atomic step that sets the edge-pointer AND the
\* cursor to the committed generation's OWN fold extent (durSnap[g].cursor). The committed
\* cursor can never diverge from the committed edges.
GCommit(l) ==
    /\ CursorInSnap
    /\ lease = l /\ phase[l] = "building"
    /\ buildGen[l] \in durWritten
    /\ durSnap[buildGen[l]] = wip[l]
    /\ gcState' = [snapGeneration |-> buildGen[l], cursor |-> durSnap[buildGen[l]].cursor]
    /\ phase'   = [phase EXCEPT ![l] = "retiring"]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, lease, wip, buildGen, retired, everLost >>

\* SPLIT COMMIT (CursorInSnap=FALSE): the durable cursor (gc/state.cursor) is a SEPARATE
\* object from the snap edges (the snap codec does NOT store the cursor — handoff KEY FACT).
\* So a leader can publish the edge-pointer and the watermark INDEPENDENTLY, and (across
\* leaders) the committed cursor can run AHEAD of the committed edges' actual fold extent.
GCommitEdges(l) ==
    /\ ~CursorInSnap
    /\ lease = l /\ phase[l] = "building"
    /\ buildGen[l] \in durWritten
    /\ durSnap[buildGen[l]] = wip[l]
    /\ gcState' = [gcState EXCEPT !.snapGeneration = buildGen[l]]
    /\ phase'   = [phase EXCEPT ![l] = "retiring"]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, lease, wip, buildGen, retired, everLost >>

GCommitCursor(l) ==
    /\ ~CursorInSnap
    /\ lease = l /\ phase[l] \in {"building", "retiring"}
    /\ gcState' = [gcState EXCEPT !.cursor = wip[l].cursor]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, lease, phase, wip, buildGen, retired, everLost >>

GRetire(l, h) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ CurGen.cursor = AbsLen
    /\ present[h] /\ h \in CurGen.known
    /\ InDegGen(CurGen, h) = 0
    /\ h \notin retired
    /\ retired' = retired \cup {h}
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, gcState, lease, phase, wip, buildGen, everLost >>

GDelete(l, h) ==
    /\ lease = l /\ phase[l] = "retiring"
    /\ h \in retired
    /\ CurGen.cursor = AbsLen
    /\ InDegGen(CurGen, h) = 0
    /\ present[h]
    /\ present' = [present EXCEPT ![h] = FALSE]
    /\ everLost' = everLost \cup (IF h \in ReachableSet THEN {h} ELSE {})
    /\ retired' = retired \ {h}
    /\ UNCHANGED << log, logBase, refs, durSnap, durWritten, gcState, lease, phase, wip, buildGen >>

\* END ROUND: release lease AND DISCARD the in-memory wip (a leader that loses the lease loses
\* its un-persisted fold). This is the crux: edges folded only in wip are gone.
GEndRound(l) ==
    /\ lease = l /\ phase[l] \in {"building", "retiring"}
    /\ phase' = [phase EXCEPT ![l] = "idle"]
    /\ lease' = "none"
    /\ wip'   = [wip EXCEPT ![l] = NoGen]
    /\ UNCHANGED << present, log, logBase, refs, durSnap, durWritten, gcState, buildGen, retired, everLost >>

\* ------------------------------------------------------------- next / spec
Next ==
    \/ \E b \in Blobs : WUploadBlob(b)
    \/ \E t \in Trees : WPublishTree(t) \/ WDropTree(t)
    \/ GTrim
    \/ \E l \in Leaders : GAcquireLease(l) \/ GFold(l) \/ GPersist(l) \/ GProbeUpward(l)
                          \/ GCommit(l) \/ GCommitEdges(l) \/ GCommitCursor(l) \/ GEndRound(l)
    \/ \E l \in Leaders, t \in Trees : GCascadeStrip(l, t)
    \/ \E l \in Leaders, h \in Hashes : GRetire(l, h) \/ GDelete(l, h)

Spec == Init /\ [][Next]_vars

\* ------------------------------------------------------------- invariants
TypeOK ==
    /\ present \in [Hashes -> BOOLEAN]
    /\ log \in Seq(Rec)
    /\ logBase \in 0..MaxLog
    /\ refs \subseteq Trees
    /\ lease \in Leaders \cup {"none"}
    /\ phase \in [Leaders -> {"idle", "building", "retiring"}]
    /\ gcState.snapGeneration \in 0..MaxGen
    /\ retired \subseteq Hashes
    /\ everLost \subseteq Hashes

\* No live reference resolves to a deleted/absent object (INV-NO-LOSS).
INV_NO_LOSS == \A h \in ReachableSet : present[h]

\* No object was ever physically deleted by GC WHILE reachable from the live manifest.
INV_NO_GC_LOSS == everLost = {}

StateConstraint ==
    /\ AbsLen <= MaxLog
    /\ gcState.snapGeneration <= MaxGen
    /\ \A l \in Leaders : buildGen[l] <= MaxGen
=======================================================================================
