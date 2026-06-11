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

\* ---------------------------------------------------------------- inductive-step initializer
\* Apalache's --init must ASSIGN every variable. IndInv (a conjunction of element-wise bounds and
\* facts) carries no top-level assignment, so --init=IndInv fails with "v' is used before it is
\* assigned". The documented idiom (EWD840's InvAndTypeOK) is to combine a most-permissive shape
\* (every variable a bounded nondeterministic value of its type, via Apalache's Gen) with the
\* invariant: StateShape ASSIGNS, IndInv CONSTRAINS. StateShape weakens nothing — Gen ranges over
\* all values of the type; TypeBounds does the pinning. Gen widths cover MaxLog=4 and the small
\* identity sets with room to spare. Used only for the step check (--init=IndInvInit); the base
\* case uses --init=Init.
\* Function-typed variables get an explicit DOMAIN (the index set) so reads under \A h \in Hashes
\* etc. land on defined entries — a bare Gen produces a function over an arbitrary (FRESH) domain,
\* making TypeBounds reads junk. Codomains are wide (TypeBounds pins the tight bounds). The Seq-
\* bearing man and the set-typed variables use Gen (Seq is not finitely enumerable); man's domain
\* is pinned to Shards by building the function explicitly and Gen-ing only each record value.
StateShape ==
    /\ present     \in [Hashes -> BOOLEAN]
    /\ tokOf       \in [Hashes -> 0..(MaxToken+1)]
    /\ nextTok     \in [Hashes -> 0..(MaxToken+1)]
    /\ obsoleteTok \in [Hashes -> SUBSET (0..(MaxToken+1))]
    /\ man         = [s \in Shards |-> Gen(6)]
    /\ retired     = Gen(6)
    /\ inflight    = Gen(6)
    /\ gcRound     = Gen(1)
    /\ gcPhase     = Gen(1)
    /\ fencedSet   \in SUBSET Shards
    /\ fencePos    \in [Shards -> 0..(MaxLog+2)]
    /\ cursor      \in [Shards -> 0..(MaxLog+2)]
    /\ rootEdges   = Gen(6)
    /\ wDeps       \in [Writers -> SUBSET (Hashes \X (0..(MaxToken+1)))]
    /\ wView       \in [Writers -> 0..(MaxRound+1)]

\* ---------------------------------------------------------------- invariants / IndInv v1 (Task 4)
\* The candidate inductive invariant. Each named conjunct is a fact argued informally in the
\* spec/model work; the CTI loop (Task 5) strengthens. Every conjunct is TLC-pre-filtered against
\* reachable states before any SMT effort.
\*
\* PRE-FILTER POLICY: per-hash facts are pre-filtered EXHAUSTIVELY at the single-hash TLC bounds
\* (694,265 distinct states, depth 30, complete). INTER-HASH facts (anything coupling inflight/
\* retired on one hash with edges/journal records of another) cannot be witnessed non-vacuously at
\* Hashes={h1} — a candidate fact `tokOf[d.h]=d.t => InDeg(d.h)=0` passed single-hash TLC vacuously
\* yet was FALSE at two hashes, broken by GFold of a stale add (CTI #6). For those, the pre-filter
\* is (a) the exhaustive single-hash run (must stay green) PLUS (b) a BOUNDED two-hash TLC run
\* matching CInit — no violation required, exhaustion NOT required. This is sound because induction
\* soundness does not rest on the pre-filter: a false lemma necessarily fails the Apalache base or
\* step check (Init => IndInv, plus the step, pin IndInv on all reachable states) — the pre-filter
\* is an efficiency/diagnosis device, not a soundness gate. The CTI #6 trap wasted SMT cycles; it
\* could not have certified a false invariant.
\*
\* CTI-loop history (Task 5): see the per-conjunct comments below; the journal lives in the commit
\* messages and CaIncarnationCore_RESULTS.md (Task 7).
TypeBounds ==   \* domains the types don't carry (Apalache types are unbounded Int/Seq)
    /\ \A h \in Hashes : /\ nextTok[h] \in 1..(MaxToken+1)
                         /\ tokOf[h] \in 0..MaxToken /\ tokOf[h] < nextTok[h]
                         /\ obsoleteTok[h] \subseteq 1..(nextTok[h]-1)
                         /\ (present[h] => tokOf[h] >= 1)
    /\ gcRound \in 0..MaxRound /\ gcPhase \in {"idle","retiring","fencing","fenced"}
    /\ fencedSet \subseteq Shards
    /\ \A s \in Shards : /\ Len(man[s].log) <= MaxLog
                         /\ cursor[s] \in 0..Len(man[s].log)
                         /\ fencePos[s] \in 0..(Len(man[s].log) + 1)
                         /\ man[s].fence \in 0..MaxRound /\ man[s].refs \subseteq Hashes
                         /\ \A i \in DOMAIN man[s].log : /\ man[s].log[i].op \in {"add","rem","fence"}
                                                         /\ man[s].log[i].hs \subseteq Hashes
    /\ \A e \in retired : e.h \in Hashes /\ e.t \in 1..MaxToken /\ e.r \in 1..MaxRound
                          /\ e.t < nextTok[e.h]      \* a retired token was allocated
    /\ \A d \in inflight : d.h \in Hashes /\ d.t \in 1..MaxToken
    /\ \A w \in Writers : wView[w] \in 0..MaxRound
                          /\ \A p \in wDeps[w] : p[1] \in Hashes /\ p[2] \in 1..MaxToken

NoDangle  == \A s \in Shards : \A h \in man[s].refs : present[h]              \* TARGET
NoReturn  == \A h \in Hashes : present[h] => tokOf[h] \notin obsoleteTok[h]   \* TARGET

FenceCoverage ==       \* CTI #7 (Apalache step, 2026-06-11): a fenced shard's manifest fence covers
                       \* the active round, and the "fenced" phase means every shard is fenced.
                       \* Without it the step admits gcPhase="fenced" with man.fence=0 < gcRound: a
                       \* low-view writer's post-fence add then escapes the gate's RetiredHit (view
                       \* >= man.fence is vacuous), the snap lags it, and GRecheckDelete condemns a
                       \* still-referenced current token off the stale snap, violating
                       \* InflightVsRefs. Inductive: GFenceShard adds s to fencedSet and bumps its
                       \* fence to >= gcRound in the same step, and flips to "fenced" exactly when
                       \* fencedSet covers Shards; GStartRound resets fencedSet to {} as it bumps
                       \* gcRound; fences never decrease. HASH-FREE fact: the exhaustive single-hash
                       \* TLC pre-filter is complete for it (no two-hash vacuity concern).
    /\ \A s \in fencedSet : man[s].fence >= gcRound
    /\ (gcPhase = "fenced" => fencedSet = Shards)

RetiredCoveredNoPostFenceAdd ==
                       \* CTI #10 (Apalache step, 2026-06-11): the retired-entry analogue of
                       \* InflightCurrentUnreferenced — for a retired entry e whose token is still
                       \* current and whose round is covered by a shard's fence, that shard's log
                       \* has NO add of e.h after its latest fence record. Records after fencePos
                       \* were published with the CURRENT man.fence (a newer fence would have
                       \* advanced fencePos), so the gate had wView >= man.fence >= e.r and
                       \* re-observation forced the dep token to tokOf[e.h] = e.t — RetiredHit(e)
                       \* fires and blocks the publish; a resurrect/overwrite instead falsifies
                       \* tokOf = e.t (tokens never return). Without this fact the step parks
                       \* unfolded post-fence adds of e.h, the snap stays stale through
                       \* FoldedThroughFence, and GRecheckDelete condemns a still-referenced
                       \* current token (CTI #7/#8/#9's pattern, final layer). Preserved:
                       \* GFenceShard advances fencePos past all old records (the post-fence set
                       \* empties); WPublish of e.h is gate-blocked as above; GRetire's new entry
                       \* has e.r = gcRound and fences lag the round during "retiring".
    \A e \in retired : \A s \in Shards :
        (tokOf[e.h] = e.t /\ man[s].fence >= e.r) =>
            \A i \in DOMAIN man[s].log :
                (i > fencePos[s]) => ~(man[s].log[i].op = "add" /\ e.h \in man[s].log[i].hs)

RetiringFenceBelow ==  \* CTI #11 (Apalache step, 2026-06-11): during "retiring" every shard's
                       \* fence is strictly below the active round. GStartRound enters "retiring"
                       \* bumping gcRound past every fence (FenceLeRound bounds them by the OLD
                       \* round), and GFenceShard — the only fence writer — exits "retiring" in the
                       \* same step. Without it the step lets GRetire stamp a new retired entry
                       \* whose round is already "covered" (fence = gcRound) while an old unfolded
                       \* add of the retiree sits past fencePos, breaking
                       \* RetiredCoveredNoPostFenceAdd at its creation point.
    gcPhase = "retiring" => \A s \in Shards : man[s].fence < gcRound

FenceLeRound ==        \* CTI #9 (Apalache step, 2026-06-11): no shard's fence is ahead of the
                       \* active round (man[s].fence <= gcRound). GFenceShard sets the fence to at
                       \* most gcRound and nothing else writes it; gcRound only increases. Without
                       \* it the step admits a fence "from the future" (fence=2 at gcRound=1) that
                       \* satisfies FenceCoverage while an ungated unfolded post-fence add keeps the
                       \* snap stale, and GRecheckDelete condemns a still-referenced current token
                       \* (CTI #7's pattern again). Inductive: GFenceShard writes max(gcRound,fence);
                       \* GStartRound increases gcRound.
    \A s \in Shards : man[s].fence <= gcRound

FencePosRecord ==      \* CTI #8 (Apalache step, 2026-06-11): a fenced shard's fencePos points at an
                       \* actual fence record in its log. Without it the step admits fencedSet={s}
                       \* with fencePos=0 and NO fence record: FoldedThroughFence is trivially true
                       \* at cursor=0, every add is "post-fence" yet ungated, and GRecheckDelete
                       \* condemns a still-referenced current token off the empty snap (CTI #7's
                       \* pattern one layer deeper). Inductive: GFenceShard appends the fence record
                       \* and sets fencePos to exactly its position (Len(old log)+1) in the same
                       \* step; the journal is append-only (no trim in the proof core), so the
                       \* record at fencePos stays a fence; GStartRound resets fencedSet to {}.
    \A s \in fencedSet : /\ fencePos[s] >= 1
                         /\ fencePos[s] <= Len(man[s].log)
                         /\ man[s].log[fencePos[s]].op = "fence"

InflightCurrentUnreferenced ==
                       \* CTI #5/#7 (Apalache step, 2026-06-11): an in-flight delete on a
                       \* STILL-CURRENT token has no reference anywhere: no folded root edge, and no
                       \* unfolded add in any journal suffix beyond the cursor. This is what makes
                       \* the spared-branch orphan impossible while the token is current (CTI #5's
                       \* state). Established at send (GRecheckDelete condemned branch): InDeg=0
                       \* covers the folded half; for the unfolded half, any add beyond the cursor
                       \* sits beyond the fence record (FoldedThroughFence), its publish was
                       \* post-fence, and a post-fence publish of token d.t is blocked by the gate's
                       \* RetiredHit (entry held at send; SendImpliesFenced gives man.fence >= e.r;
                       \* gate needs view >= man.fence), while a post-fence publish of t' # d.t
                       \* implies an intervening resurrect, i.e. tokOf # d.t (antecedent false).
                       \* Preserved: WPublish of d.h@d.t is blocked the same way, of t' requires
                       \* tokOf = t' (re-observation); WResurrect/WOverwrite falsify the antecedent;
                       \* GFold folds an add at a position <= the new cursor, which the unfolded
                       \* half says is not d.h; Land removes d; the spared drop touches neither half.
    \A d \in inflight : tokOf[d.h] = d.t =>
        /\ \A s \in Shards : <<s, d.h>> \notin rootEdges
        /\ \A s \in Shards : \A i \in DOMAIN man[s].log :   \* i > cursor[s]: the unfolded suffix
               \* (Apalache rejects the non-constant range (cursor[s]+1)..Len(log) — known issue)
               (i > cursor[s]) => ~(man[s].log[i].op = "add" /\ d.h \in man[s].log[i].hs)

RootEdgesTyped ==      \* CTI #4 (Apalache step, 2026-06-11): every root edge is over a real shard
                       \* and hash (rootEdges \subseteq Shards \X Hashes). The type Set(<<SHARD,HASH>>)
                       \* does NOT restrict the shard to Shards, so the step admits a junk edge
                       \* <<fresh_shard, h>> that inflates InDeg(h); GRecheckDelete then takes the
                       \* SPARED branch (InDeg>0) and drops a retired entry while leaving its delete
                       \* in flight on a still-current token, violating InflightHeld. Inductive:
                       \* GFold is the only action adding to rootEdges and it adds <<s,h>> with
                       \* s \in Shards (the GFold binder) and h \in rec.hs \subseteq Hashes.
    rootEdges \subseteq (Shards \X Hashes)

AbsentObsolete ==      \* CTI #3 (Apalache step, 2026-06-11): an ABSENT object's last current token
                       \* is obsolete (or it was never created, tokOf=0). The only action clearing
                       \* present[h] is Land (real delete), which adds tokOf[h] to obsoleteTok[h] in
                       \* the same step. Without it the step allows present[h]=FALSE with a stale
                       \* nonzero tokOf[h] not in obsoleteTok, plus retired entries at that token;
                       \* WCreate then advances tokOf, orphaning those retired entries and violating
                       \* RetiredCurrentOrDead. Inductive: Land is the only present->FALSE step and it
                       \* obsoletes tokOf; tokOf changes only while present=TRUE; obsoleteTok grows.
    \A h \in Hashes : present[h] \/ tokOf[h] = 0 \/ tokOf[h] \in obsoleteTok[h]

PhaseRoundActive ==    \* CTI #2 (Apalache step, 2026-06-11): an active GC phase implies a started
                       \* round (gcRound >= 1). GStartRound is the only action entering "retiring",
                       \* and it sets gcRound := gcRound+1 (>= 1) in the same step; the later phases
                       \* keep the round. Without it the step lets gcPhase="retiring" sit at
                       \* gcRound=0, then GRetire stamps a retired entry with r=0, violating
                       \* TypeBounds' e.r \in 1..MaxRound. Inductive: only GStartRound sets an active
                       \* phase and it forces gcRound >= 1; no action decreases gcRound.
    gcPhase \in {"retiring", "fencing", "fenced"} => gcRound >= 1

InflightAllocated ==   \* CTI #1 (Apalache step, 2026-06-11): an in-flight delete's token was
                       \* actually allocated (d.t < nextTok[d.h]). TypeBounds carries the analogous
                       \* fact for retired entries but NOT for inflight; without it the step lets a
                       \* phantom inflight [h, nextTok[h]] sit with no retired entry, then WOverwrite
                       \* bumps tokOf to that very (unallocated) value, making it current and
                       \* falsifying InflightHeld. Inductive: inflight grows only via GRecheckDelete
                       \* from a retired entry e (TypeBounds: e.t < nextTok[e.h]), and nextTok never
                       \* decreases.
    \A d \in inflight : d.t < nextTok[d.h]

InflightHeld ==        \* THE lemma (CORRECTED by TLC pre-filter — see below): a delete can be in
                       \* flight only for an entry that is EITHER still held in retired OR whose
                       \* token has already stopped being current (tokOf[h] # t), making the
                       \* eventual Land a harmless 412 no-op.
                       \*
                       \* CORRECTION (TLC trace, 2026-06-11): the original form
                       \*   \A d \in inflight : \E e \in retired : e.h = d.h /\ e.t = d.t
                       \* is FALSE in this model. GRecheckDelete may put [h,t] in flight while
                       \* InDeg(h)=0; a later GFold raises InDeg(h)>0; GRecheckDelete on the SAME
                       \* retired entry then takes the SPARED branch and drops the retired entry,
                       \* leaving [h,t] in flight with no matching retired entry. In that orphan
                       \* state the token is no longer current (it was resurrected/overwritten),
                       \* so tokOf[h] # t holds — the corrected disjunct. The actions are frozen;
                       \* the lemma is weakened to match the model's actual truth.
                       \* NOTE: still depends on outcome consumption being ATOMIC with the
                       \* delete-message landing (Land drops the matching retired entry in the
                       \* same step). A later refactor to separate outcome logs revisits this.
    \A d \in inflight : (\E e \in retired : e.h = d.h /\ e.t = d.t) \/ tokOf[d.h] # d.t

RetiredCurrentOrDead ==   \* a retired token is still current, or it STOPPED BEING CURRENT and is
                          \* therefore in obsoleteTok (displaced-or-deleted history — the name says
                          \* what it means: obsolete as a dependency, not necessarily deleted).
                          \* WResurrect/WOverwrite add the displaced old token atomically (MR-2) —
                          \* exactly why the resurrect-displacement state does not falsify this.
    \A e \in retired : e.t = tokOf[e.h] \/ e.t \in obsoleteTok[e.h]

InflightVsRefs ==      \* the heart: an in-flight-deletable token is never a referenced current token
    \A d \in inflight : \A s \in Shards : d.h \in man[s].refs => tokOf[d.h] # d.t

SendImpliesFenced ==   \* a delete is sent only after every shard's fence covers its entry's round.
                       \* CORRECTED in parallel with InflightHeld (TLC pre-filter, 2026-06-11): a
                       \* delete may outlive its retired entry (the orphan case, see InflightHeld),
                       \* at which point there is no entry to read e.r from — but then the token is
                       \* no longer current (tokOf[h] # t). So: EITHER a matching retired entry
                       \* exists and every shard's fence covers its round, OR the token has stopped
                       \* being current.
    \A d \in inflight : (\E e \in retired :
                            e.h = d.h /\ e.t = d.t /\ \A s \in Shards : man[s].fence >= e.r)
                        \/ tokOf[d.h] # d.t

RefsFromLog ==         \* manifest CAS keeps refs and journal atomic — refs IS the full-log fold.
                       \* SCOPE: this is the LOGICAL / pre-compaction manifest (the proof core has
                       \* no Trim). Production manifests trim folded tails: there, refs = fold of
                       \* (checkpoint base + tail). Physical trimming is covered by the TLC model
                       \* (INV_JOURNAL_COVERAGE), not by this proof core — a recorded residual.
    \A s \in Shards : man[s].refs = FoldRefs(man[s].log)

SnapFromPrefix ==      \* the snap is exactly the cursor-prefix fold
    \A s \in Shards : { h \in Hashes : <<s, h>> \in rootEdges } = FoldPrefix(man[s].log, cursor[s])

IndInv == TypeBounds /\ NoDangle /\ NoReturn /\ InflightHeld /\ RetiredCurrentOrDead
          /\ InflightVsRefs /\ SendImpliesFenced /\ RefsFromLog /\ SnapFromPrefix
          /\ InflightAllocated /\ PhaseRoundActive /\ AbsentObsolete /\ RootEdgesTyped
          /\ InflightCurrentUnreferenced /\ FenceCoverage /\ FencePosRecord /\ FenceLeRound
          /\ RetiredCoveredNoPostFenceAdd /\ RetiringFenceBelow

\* Inductive-step init: assign (StateShape) then constrain (IndInv). See StateShape comment.
IndInvInit == StateShape /\ IndInv

\* ============================================================ NEGATIVE CONTROLS (Task 6) ==========
\* The definitions below are CONTROLS for the induction proof, NOT part of the protocol. They are
\* inert for TLC (not referenced from the .cfg) and are exercised only by explicit Apalache runs.
\* Their job is to prove IndInv "has teeth": that load-bearing conjuncts are load-bearing (drop-a-
\* lemma), that the W-REVALIDATE re-observation conjunct is what carries F1 (gate control), and that
\* IndInv is satisfiable (not vacuously true). See the plan, Task 6.

\* --------------------------------------------- drop-a-lemma controls (Step 2, extended per Task 5)
\* Each is IndInv with EXACTLY ONE conjunct removed, plus its own StateShape-based --init. The
\* step check on each MUST FAIL with a CTI — i.e. the removed conjunct is load-bearing for the
\* induction (not implied by the rest). If one PASSES, the remaining conjuncts imply the dropped
\* one (an acceptable, documentable implication) or IndInv is over-strong somewhere.

\* minus InflightHeld
IndInv_NoHeld == TypeBounds /\ NoDangle /\ NoReturn /\ RetiredCurrentOrDead
          /\ InflightVsRefs /\ SendImpliesFenced /\ RefsFromLog /\ SnapFromPrefix
          /\ InflightAllocated /\ PhaseRoundActive /\ AbsentObsolete /\ RootEdgesTyped
          /\ InflightCurrentUnreferenced /\ FenceCoverage /\ FencePosRecord /\ FenceLeRound
          /\ RetiredCoveredNoPostFenceAdd /\ RetiringFenceBelow
IndInv_NoHeldInit == StateShape /\ IndInv_NoHeld

\* minus InflightVsRefs
IndInv_NoRefs == TypeBounds /\ NoDangle /\ NoReturn /\ InflightHeld /\ RetiredCurrentOrDead
          /\ SendImpliesFenced /\ RefsFromLog /\ SnapFromPrefix
          /\ InflightAllocated /\ PhaseRoundActive /\ AbsentObsolete /\ RootEdgesTyped
          /\ InflightCurrentUnreferenced /\ FenceCoverage /\ FencePosRecord /\ FenceLeRound
          /\ RetiredCoveredNoPostFenceAdd /\ RetiringFenceBelow
IndInv_NoRefsInit == StateShape /\ IndInv_NoRefs

\* minus InflightCurrentUnreferenced (the most load-bearing Task-5 addition — CTI #5/#7)
IndInv_NoICU == TypeBounds /\ NoDangle /\ NoReturn /\ InflightHeld /\ RetiredCurrentOrDead
          /\ InflightVsRefs /\ SendImpliesFenced /\ RefsFromLog /\ SnapFromPrefix
          /\ InflightAllocated /\ PhaseRoundActive /\ AbsentObsolete /\ RootEdgesTyped
          /\ FenceCoverage /\ FencePosRecord /\ FenceLeRound
          /\ RetiredCoveredNoPostFenceAdd /\ RetiringFenceBelow
IndInv_NoICUInit == StateShape /\ IndInv_NoICU

\* --------------------------------------------- gate negative control (Step 2b — the F1 witness)
\* WPublishNoReval is a mechanical copy of WPublish with the per-dep RE-OBSERVATION conjunct
\* (present[d[1]] /\ tokOf[d[1]] = d[2]) REMOVED — only ~RetiredHit and the fence check remain.
\* This is the F1 bug class: a stale token-bearing dependency (object deleted or displaced after
\* observation) passes the weakened gate and publishes. NextNoReval swaps WPublish -> WPublishNoReval.
\* NEGATIVE CONTROL ONLY — NOT the protocol. Selected via Apalache's --next=NextNoReval.
\* @type: (WRITER) => Bool;
DepOKNoReval(w) == \A d \in wDeps[w] : ~RetiredHit(d[1], d[2], wView[w])
WPublishNoReval(w, s, h) ==
    /\ \E t \in Toks : <<h, t>> \in wDeps[w]
    /\ h \notin man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ wView[w] >= man[s].fence /\ DepOKNoReval(w)
    /\ man'   = [man EXCEPT ![s].refs = @ \cup {h},
                            ![s].log  = Append(@, [op |-> "add", hs |-> {h}])]
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, obsoleteTok, retired, inflight, gcRound, gcPhase,
                    fencedSet, fencePos, cursor, rootEdges, wView >>

\* NEGATIVE CONTROL ONLY: Next with WPublish replaced by WPublishNoReval (everything else identical).
NextNoReval ==
    \/ \E w \in Writers, h \in Hashes : WCreate(w, h) \/ WReuse(w, h)
    \/ \E w \in Writers, h \in Hashes : WOverwrite(w, h)
    \/ \E w \in Writers, h \in Hashes : WResurrect(w, h)
    \/ \E w \in Writers, h \in Hashes : WReobserve(w, h)
    \/ \E w \in Writers : WRefreshView(w) \/ WAbandon(w)
    \/ \E w \in Writers, s \in Shards, h \in Hashes : WPublishNoReval(w, s, h)
    \/ \E s \in Shards, h \in Hashes : WDrop(s, h)
    \/ GStartRound \/ GEndRound
    \/ \E s \in Shards : GFold(s)
    \/ \E h \in Hashes : GRetire(h)
    \/ \E s \in Shards : GFenceShard(s)
    \/ \E e \in retired : GRecheckDelete(e)
    \/ \E d \in inflight : Land(d)

\* --------------------------------------------- satisfiability control (Step 3)
\* FalseInv == FALSE: --init=IndInvInit --inv=FalseInv --length=0 MUST produce a counterexample,
\* i.e. an IndInv state exists (guards against a contradictory IndInv that would pass everything
\* vacuously). The base case (Init => IndInv, already green) also implies satisfiability.
FalseInv == FALSE
=======================================================================================
