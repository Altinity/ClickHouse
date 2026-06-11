---------------------------- MODULE CaIncarnationProofCore ----------------------------
(* Incarnation-token CA store — TRIMMED PROOF CORE for Apalache one-step induction.
   Derived from CaIncarnationCore.tla by exact surgery (see the plan
   docs/superpowers/plans/2026-06-11-ca-apalache-inductive.md, Task 1):

     - SINGLE LEADER: gcPhase is a plain Str, gcRound IS the active round (no roundOf, no
       Leaders quantifier, no fencedSet-per-leader — fencedSet is a plain Set(SHARD)).
     - W-REVALIDATE GATE ONLY: the publish gate has NO dead/obsolete-token oracle. A stale
       dependency is rejected by RE-OBSERVATION (present[h] /\ tokOf[h] = t) plus a
       retire-view check (~RetiredHit). This is the faithful re-observation world.
     - TOKEN-ONLY DEPENDENCIES: wDeps : WRITER -> Set(<<HASH, Int>>). No tokenless evidence
       (no Ev, no WEvidence/WResolveEvidence).
     - NO TREES / DEBRIS / SPLIT / FULL-GC WALK / TRIM / SABOTAGE FLAGS. These are recorded
       residuals covered by the main model (CaIncarnationCore.tla).
     - UNIFORM JOURNAL RECORD: Apalache rejects heterogeneous record unions, so the main
       model's AddRec \cup FenceRec becomes one record type { op: Str, hs: Set(HASH) };
       hs is a singleton {h} for "add"/"rem" and {} for "fence". Semantics identical
       (asserted in the TLC cross-check, Task 2).

   RENAME (proof core only): the main model's deadTok is obsoleteTok here — "tokens that
   stopped being current as a dependency" (by delete, resurrect, OR overwrite), NOT
   "physically deleted". Semantics identical (MR-2).

   WHY THE GATE NEEDS NO OBSOLETE-TOKEN ORACLE (intentional, differs from the main model's
   stage-1 oracle fix): the re-observation conjunct present[h] /\ tokOf[h] = t blocks a stale
   dependency on a deleted token (present = FALSE) and on a displaced/overwritten token
   (tokOf # t) directly — obsoleteTok is needed only as the NoReturn history variable, never
   as a gate input. This is exactly the spec's W-REVALIDATE claim.

   MR-2 completeness: every action that changes tokOf[h] or clears present[h] adds the
   displaced/deleted token to obsoleteTok — WResurrect, WOverwrite, Land(real delete). WCreate
   has no prior token (0 -> t, nothing to add). No action ever adds 0 (every displacing/deleting
   action guards on present[h] => tokOf[h] >= 1, and TypeBounds pins obsoleteTok \subseteq
   1..(nextTok-1)). *)
EXTENDS Integers, Sequences, FiniteSets, Apalache

CONSTANTS
    \* @type: Set(WRITER);
    Writers,
    \* @type: Set(SHARD);
    Shards,
    \* @type: Set(HASH);
    Hashes,
    \* @type: Int;
    MaxToken,
    \* @type: Int;
    MaxRound,
    \* @type: Int;
    MaxLog

Toks == 1..MaxToken

VARIABLES
    \* @type: HASH -> Bool;
    present,
    \* @type: HASH -> Int;
    tokOf,
    \* @type: HASH -> Int;
    nextTok,
    \* @type: HASH -> Set(Int);
    obsoleteTok,   \* main model's deadTok: tokens that stopped being current (delete/resurrect/overwrite)
    \* @type: SHARD -> { fence: Int, refs: Set(HASH), log: Seq({ op: Str, hs: Set(HASH) }) };
    man,
    \* @type: Set({ h: HASH, t: Int, r: Int });
    retired,
    \* @type: Set({ h: HASH, t: Int });
    inflight,
    \* @type: Int;
    gcRound,
    \* @type: Str;
    gcPhase,
    \* @type: Set(SHARD);
    fencedSet,
    \* @type: SHARD -> Int;
    fencePos,
    \* @type: SHARD -> Int;
    cursor,
    \* @type: Set(<<SHARD, HASH>>);
    rootEdges,
    \* @type: WRITER -> Set(<<HASH, Int>>);
    wDeps,
    \* @type: WRITER -> Int;
    wView

vars == << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase,
           fencedSet, fencePos, cursor, rootEdges, wDeps, wView >>

\* ---------------------------------------------------------------- constant initializer (Apalache)
\* Uninterpreted-type literals are distinct by construction (declared uninterpreted CONSTANTS
\* are NOT automatically distinct in Apalache; the literal "x_OF_TYPE" syntax is).
CInit ==
    /\ Writers = { "w1_OF_WRITER", "w2_OF_WRITER" }
    /\ Shards  = { "s1_OF_SHARD" }
    /\ Hashes  = { "h1_OF_HASH", "h2_OF_HASH" }
    /\ MaxToken = 3 /\ MaxRound = 2 /\ MaxLog = 4

\* ---------------------------------------------------------------- fold operators (Apalache folds)
\* @type: (Set(HASH), { op: Str, hs: Set(HASH) }) => Set(HASH);
ApplyRec(acc, rec) == IF rec.op = "add" THEN acc \cup rec.hs
                      ELSE IF rec.op = "rem" THEN acc \ rec.hs ELSE acc
\* refs derived from a full log
\* @type: Seq({ op: Str, hs: Set(HASH) }) => Set(HASH);
FoldRefs(log) == ApaFoldSeqLeft(ApplyRec, {}, log)
\* prefix fold for the snap lemma
\* @type: (Seq({ op: Str, hs: Set(HASH) }), Int) => Set(HASH);
FoldPrefix(log, n) == ApaFoldSeqLeft(ApplyRec, {}, SubSeq(log, 1, n))

\* ---------------------------------------------------------------- helpers
\* A token-bearing dependency is condemned in a view iff a retire entry for that exact token
\* is visible at the view. The proof-core gate uses RetiredHit ONLY (no obsolete-token consult).
\* @type: (HASH, Int, Int) => Bool;
RetiredHit(h, t, v) == \E e \in retired : e.h = h /\ e.t = t /\ e.r <= v
\* in-degree over the root snap only (no trees in the proof core)
\* @type: HASH => Int;
InDeg(h) == Cardinality({ e \in rootEdges : e[2] = h })
FoldedThroughFence == \A s \in Shards : cursor[s] >= fencePos[s]

\* ---------------------------------------------------------------- init
Init ==
    /\ present  = [h \in Hashes |-> FALSE]
    /\ tokOf    = [h \in Hashes |-> 0]
    /\ nextTok  = [h \in Hashes |-> 1]
    /\ obsoleteTok = [h \in Hashes |-> {}]
    /\ man      = [s \in Shards |-> [fence |-> 0, refs |-> {}, log |-> <<>>]]
    /\ retired  = {} /\ inflight = {} /\ gcRound = 0
    /\ gcPhase  = "idle"
    /\ fencedSet = {}
    /\ fencePos = [s \in Shards |-> 0]
    /\ cursor   = [s \in Shards |-> 0]
    /\ rootEdges = {}
    /\ wDeps    = [w \in Writers |-> {}]
    /\ wView    = [w \in Writers |-> 0]

\* ---------------------------------------------------------------- writer actions
\* Create a missing object: fresh token from the allocator. Dependency recorded with the token.
WCreate(w, h) ==
    /\ present[h] = FALSE /\ nextTok[h] <= MaxToken
    /\ present' = [present EXCEPT ![h] = TRUE]
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ wDeps'   = [wDeps   EXCEPT ![w] = @ \cup {<<h, nextTok[h]>>}]
    /\ UNCHANGED << obsoleteTok, man, retired, inflight, gcRound, gcPhase, fencedSet,
                    fencePos, cursor, rootEdges, wView >>

\* Cold reuse: the existence check observes the current token (token-bearing dependency).
WReuse(w, h) ==
    /\ present[h]
    /\ wDeps' = [wDeps EXCEPT ![w] = @ \cup {<<h, tokOf[h]>>}]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wView >>

\* Anonymous environment churn: an unconditional same-content re-PUT, any time. No dependency
\* recorded. The in-place overwrite makes the OLD token stop being current, so it MUST join
\* obsoleteTok (the model rule; else a stale token-bearing dep on it dangles).
WOverwrite(w, h) ==
    /\ present[h] /\ nextTok[h] <= MaxToken
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ obsoleteTok' = [obsoleteTok EXCEPT ![h] = @ \cup {tokOf[h]}]
    /\ UNCHANGED << present, man, retired, inflight, gcRound, gcPhase, fencedSet,
                    fencePos, cursor, rootEdges, wDeps, wView >>

\* Resurrect a condemned current incarnation: overwrite in place with a FRESH token. The OLD
\* token is gone for good — record it in obsoleteTok (NoReturn). Condemned guard uses
\* RetiredHit only (faithful re-observation world).
WResurrect(w, h) ==
    /\ present[h] /\ RetiredHit(h, tokOf[h], wView[w])
    /\ nextTok[h] <= MaxToken
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ obsoleteTok' = [obsoleteTok EXCEPT ![h] = @ \cup {tokOf[h]}]
    /\ wDeps'   = [wDeps   EXCEPT ![w] = @ \cup {<<h, nextTok[h]>>}]
    /\ UNCHANGED << present, man, retired, inflight, gcRound, gcPhase, fencedSet,
                    fencePos, cursor, rootEdges, wView >>

\* W-REVALIDATE HEAD: on a retire-view refresh the writer re-observes every token-bearing
\* dependency on h — refresh to the CURRENT token if the key is present, or drop the dependency
\* if the key is absent.
WReobserve(w, h) ==
    /\ \E t \in Toks : <<h, t>> \in wDeps[w]
    /\ LET keep == { d \in wDeps[w] : d[1] # h }
       IN wDeps' = [wDeps EXCEPT ![w] = IF present[h] THEN keep \cup {<<h, tokOf[h]>>} ELSE keep]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wView >>

WRefreshView(w) ==
    /\ wView' = [wView EXCEPT ![w] = gcRound]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wDeps >>

\* Publish: one atomic successful CAS. W-REVALIDATE gate: the gate has NO obsolete-token oracle;
\* a stale observation is validated by RE-OBSERVATION (present /\ tokOf = t), plus ~RetiredHit
\* and the fence check wView >= man[s].fence.
\* @type: (WRITER) => Bool;
DepOK(w) == \A d \in wDeps[w] :
                /\ ~RetiredHit(d[1], d[2], wView[w])
                /\ present[d[1]] /\ tokOf[d[1]] = d[2]
WPublish(w, s, h) ==
    /\ \E t \in Toks : <<h, t>> \in wDeps[w]          \* the root itself was created/reused
    /\ h \notin man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ wView[w] >= man[s].fence /\ DepOK(w)
    /\ man'   = [man EXCEPT ![s].refs = @ \cup {h},
                            ![s].log  = Append(@, [op |-> "add", hs |-> {h}])]
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wView >>

WDrop(s, h) ==
    /\ h \in man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ man' = [man EXCEPT ![s].refs = @ \ {h},
                          ![s].log  = Append(@, [op |-> "rem", hs |-> {h}])]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wDeps, wView >>

WAbandon(w) ==      \* crash/abort before publish: deps lost
    /\ wDeps[w] # {}
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wView >>

\* ---------------------------------------------------------------- GC actions (single leader)
GStartRound ==
    /\ gcPhase = "idle" /\ gcRound < MaxRound
    /\ gcRound'  = gcRound + 1
    /\ gcPhase'  = "retiring"
    /\ fencedSet' = {}
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, fencePos, cursor,
                    rootEdges, wDeps, wView >>

\* Fold one journal record into the snap (edge-set semantics; no tree expansion in the core).
GFold(s) ==
    /\ cursor[s] < Len(man[s].log)
    /\ LET rec == man[s].log[cursor[s] + 1] IN
       rootEdges' = CASE rec.op = "add"   -> rootEdges \cup { <<s, h>> : h \in rec.hs }
                      [] rec.op = "rem"   -> rootEdges \ { <<s, h>> : h \in rec.hs }
                      [] rec.op = "fence" -> rootEdges
    /\ cursor' = [cursor EXCEPT ![s] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, wDeps, wView >>

\* Retire a present, in-degree-0 candidate at its CURRENT token (the HEAD).
GRetire(h) ==
    /\ gcPhase = "retiring"
    /\ present[h] /\ InDeg(h) = 0
    /\ ~\E e \in retired : e.h = h /\ e.t = tokOf[h]
    /\ retired' = retired \cup { [h |-> h, t |-> tokOf[h], r |-> gcRound] }
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wDeps, wView >>

\* Fence one shard: bump fence_round in the manifest (a CAS — appends a fence record).
GFenceShard(s) ==
    /\ gcPhase \in {"retiring", "fencing"}
    /\ s \notin fencedSet
    /\ Len(man[s].log) < MaxLog
    /\ man' = [man EXCEPT ![s].fence = IF gcRound > man[s].fence THEN gcRound ELSE man[s].fence,
                          ![s].log   = Append(@, [op |-> "fence", hs |-> {}])]
    /\ fencePos' = [fencePos EXCEPT ![s] = Len(man[s].log) + 1]
    /\ fencedSet' = fencedSet \cup {s}
    /\ gcPhase'   = IF fencedSet \cup {s} = Shards THEN "fenced" ELSE "fencing"
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, retired, inflight, gcRound, cursor,
                    rootEdges, wDeps, wView >>

\* Recheck + issue delete. Spared entries drop; a condemned entry STAYS in retired (still
\* blocking reuse) and a delete MESSAGE is sent — the entry drops only when the message lands.
GRecheckDelete(e) ==
    /\ gcPhase = "fenced" /\ e \in retired /\ e.r = gcRound
    /\ FoldedThroughFence
    /\ IF InDeg(e.h) > 0
       THEN /\ retired'  = retired \ {e}                      \* outcome = spared
            /\ inflight' = inflight
       ELSE /\ [h |-> e.h, t |-> e.t] \notin inflight
            /\ retired'  = retired                            \* entry kept until landing confirms
            /\ inflight' = inflight \cup { [h |-> e.h, t |-> e.t] }
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, gcRound, gcPhase, fencedSet,
                    fencePos, cursor, rootEdges, wDeps, wView >>

GEndRound ==
    /\ gcPhase = "fenced"
    /\ ~\E e \in retired : e.r = gcRound      \* waits for landings
    /\ gcPhase' = "idle"
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, man, retired, inflight, gcRound,
                    fencedSet, fencePos, cursor, rootEdges, wDeps, wView >>

\* A delete message lands: exact-token (412 = no-op). The landing is the confirmed outcome:
\* the matching retired entry drops HERE. A real delete adds the deleted token to obsoleteTok.
Land(d) ==
    /\ d \in inflight
    /\ inflight' = inflight \ {d}
    /\ retired'  = { e \in retired : ~(e.h = d.h /\ e.t = d.t) }
    /\ IF present[d.h] /\ tokOf[d.h] = d.t
       THEN /\ present' = [present EXCEPT ![d.h] = FALSE]
            /\ obsoleteTok' = [obsoleteTok EXCEPT ![d.h] = @ \cup {tokOf[d.h]}]
       ELSE /\ UNCHANGED << present, obsoleteTok >>   \* 412 / absent
    /\ UNCHANGED << tokOf, nextTok, man, gcRound, gcPhase, fencedSet, fencePos,
                    cursor, rootEdges, wDeps, wView >>

\* ---------------------------------------------------------------- next / spec
Next ==
    \/ \E w \in Writers, h \in Hashes : WCreate(w, h) \/ WReuse(w, h)
    \/ \E w \in Writers, h \in Hashes : WOverwrite(w, h)
    \/ \E w \in Writers, h \in Hashes : WResurrect(w, h)
    \/ \E w \in Writers, h \in Hashes : WReobserve(w, h)
    \/ \E w \in Writers : WRefreshView(w) \/ WAbandon(w)
    \/ \E w \in Writers, s \in Shards, h \in Hashes : WPublish(w, s, h)
    \/ \E s \in Shards, h \in Hashes : WDrop(s, h)
    \/ GStartRound \/ GEndRound
    \/ \E s \in Shards : GFold(s)
    \/ \E h \in Hashes : GRetire(h)
    \/ \E s \in Shards : GFenceShard(s)
    \/ \E e \in retired : GRecheckDelete(e)
    \/ \E d \in inflight : Land(d)

Spec == Init /\ [][Next]_vars

\* ---------------------------------------------------------------- state constraint (TLC)
StateConstraint ==
    /\ \A s \in Shards : Len(man[s].log) <= MaxLog
    /\ Cardinality(inflight) <= 2
    /\ \A w \in Writers : Cardinality(wDeps[w]) <= 3

\* ---------------------------------------------------------------- invariants (Task 2 minimal)
NoDangle == \A s \in Shards : \A h \in man[s].refs : present[h]
NoReturn == \A h \in Hashes : present[h] => tokOf[h] \notin obsoleteTok[h]
=======================================================================================
