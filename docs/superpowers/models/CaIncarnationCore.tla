------------------------------ MODULE CaIncarnationCore ------------------------------
(* Incarnation-token CA store core — spec: 2026-06-10-ca-incarnation-store-design.md §12 + Appendix A.
   One key per hash; tokens = naturals per key; deletes exact-token via in-flight messages that may
   land arbitrarily late; CAS root manifests with embedded journal; GC = fold -> retire -> fence ->
   recheck -> delete.  Sabotage* flags break one load-bearing rule each and MUST yield counterexamples. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Writers,            \* e.g. {w1, w2}
    Leaders,            \* e.g. {L1}; {L1, L2} only in stage 5
    Shards,             \* e.g. {s1}; {s1, s2} from stage 4
    Hashes,             \* content identities, e.g. {h1, h2}
    TreeHashes,         \* trees, subset of Hashes ({} until stage 3)
    MaxToken,           \* token allocator bound per key
    MaxRound,           \* GC round bound
    MaxLog,             \* journal length bound per shard (state constraint)
    EnableResurrect, EnableTrees, EnableDebris, EnableSplit, EnableOverwrite,
    SabotageNoFence, SabotageNoRecheckFold, SabotageNoRetireView,
    SabotageUncondDelete, SabotageReusedTag, SabotageCascadeRace, SabotageCutOverclaim

ASSUME TreeHashes \subseteq Hashes

NonTree  == Hashes \ TreeHashes
\* Children is DERIVED (TLC configs cannot express function/tuple constants). Asymmetric by design:
\* one tree (FullTree) references every non-tree hash; any further tree references exactly one.
\* With TreeHashes = {t1}: t1 -> NonTree (stages where one tree suffices).
\* With TreeHashes = {t1, t2}: full tree + single-child tree — covers BOTH shared-child survival
\* (a child outliving one of two referencing trees) AND selective cascade (deleting the full tree
\* must not touch the other tree's child).  Nested subtrees are NOT modeled (one-level closure).
\* FullTree/Children are evaluated lazily by TLC — only ever applied to members of TreeHashes,
\* so the CHOOSE is never evaluated when TreeHashes = {} (stages 1, 2, 5_small).
FullTree  == CHOOSE t \in TreeHashes : TRUE
OneChild  == CHOOSE h \in NonTree : TRUE
Children  == [t \in TreeHashes |-> IF t = FullTree THEN NonTree ELSE {OneChild}]
Ev       == MaxToken + 1                        \* dependency form: tokenless live-root evidence
Toks     == 1..MaxToken

AddRec   == [op: {"add", "rem"}, h: Hashes]
FenceRec == [op: {"fence"}]
Rec      == AddRec \cup FenceRec

VARIABLES
    \* ---- S3 durable: objects ----
    present,   \* [Hashes -> BOOLEAN]
    tokOf,     \* [Hashes -> 0..MaxToken]          current incarnation token (0 = never created)
    nextTok,   \* [Hashes -> 1..MaxToken+1]        fresh-token allocator (W-FRESH-TAG by construction)
    deadTok,   \* [Hashes -> SUBSET Toks]          tokens that have stopped being current (history; INV_NO_RETURN).
               \* MODEL RULE: any action that makes a token stop being current — in-place overwrite (WResurrect,
               \* and the future WOverwrite in stage 5) OR physical delete (Land) — MUST add that token here.
               \* CondemnedAtView consults deadTok, so this is what rejects a stale token-bearing dependency.
               \* Omitting the deadTok update in any such action silently reintroduces a dangling-ref bug.
    \* ---- S3 durable: roots + GC ----
    man,       \* [Shards -> [fence: 0..MaxRound, refs: SUBSET Hashes, log: Seq(Rec)]]
    retired,   \* SUBSET [h: Hashes, t: Toks, r: 1..MaxRound]
    inflight,  \* SUBSET [h: Hashes, t: Toks]      delete messages in flight; land ANY time later
    gcRound,   \* 0..MaxRound
    gcPhase,   \* [Leaders -> {"idle", "retiring", "fencing", "fenced"}]
    roundOf,   \* [Leaders -> 0..MaxRound]
    fencedSet, \* [Leaders -> SUBSET Shards]       shards this leader fenced this round
    fencePos,  \* [Shards -> Nat]                  log position of the latest fence record
    cursor,    \* [Shards -> Nat]                  folded prefix length (monotone)
    trimBase,  \* [Shards -> Nat]                  journal trimmed below this (INV_JOURNAL_COVERAGE)
    rootEdges, \* SUBSET (Shards \X Hashes)        snap: folded root edges
    treeEdges, \* SUBSET (TreeHashes \X Hashes)    snap: expanded tree edges
    marker,    \* SUBSET TreeHashes                expansion markers (marker(T) <=> T's edges present)
    everEdged, \* SUBSET Hashes                    journal-known
    pendCasc,  \* SUBSET TreeHashes                deferred cascades (SabotageCascadeRace only)
    \* ---- writer local ----
    wDeps,     \* [Writers -> SUBSET (Hashes \X (1..MaxToken+1))]   (h, tok) or (h, Ev)
    wView,     \* [Writers -> 0..MaxRound]         highest retire round refreshed
    \* ---- stage 4: heartbeats + debris + full-GC cut ----
    creator,   \* [Hashes -> Writers \cup {"none"}]  who last (re)created the object
    hbAlive,   \* [Writers -> BOOLEAN]               heartbeat object present
    hbSeq,     \* [Writers -> Nat]                   monotone renewal counter
    wedged,    \* [Writers -> BOOLEAN]               renewal thread stuck (writer still acts!)
    hbObs,     \* [Writers -> Nat \cup {-1}]         GC's first observation (-1 = none yet)
    fgPhase,   \* {"idle", "reading", "read"}        full-GC walk program counter
    fgCut,     \* [Shards -> Nat]                    log length actually read per shard
    fgRefs,    \* [Shards -> SUBSET Hashes]          refs snapshot actually read per shard
    fgSeen     \* SUBSET Shards                      shards read so far this walk

vars == << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase, roundOf,
           fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
           pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut,
           fgRefs, fgSeen >>

\* ---------------------------------------------------------------- helpers
\* A token-bearing dependency is condemned in a view if a retire entry for that exact token is
\* visible at the view, OR the token is already physically dead (INV_NO_RETURN: that exact token
\* can never again be a valid dependency, even after the retire entry was consumed by its landing).
CondemnedAtView(h, t, v) == \/ \E e \in retired : e.h = h /\ e.t = t /\ e.r <= v
                            \/ t \in deadTok[h]
HashHitAtView(h, v)      == \E e \in retired : e.h = h /\ e.r <= v
InDeg(h) == Cardinality({e \in rootEdges : e[2] = h}) + Cardinality({e \in treeEdges : e[2] = h})
Reach(r)      == {r} \cup (IF r \in TreeHashes THEN Children[r] ELSE {})
ReachableSet  == UNION { Reach(r) : r \in UNION { man[s].refs : s \in Shards } }
FoldedThroughFence == \A s \in Shards : cursor[s] >= fencePos[s]

Init ==
    /\ present  = [h \in Hashes |-> FALSE]
    /\ tokOf    = [h \in Hashes |-> 0]
    /\ nextTok  = [h \in Hashes |-> 1]
    /\ deadTok  = [h \in Hashes |-> {}]
    /\ man      = [s \in Shards |-> [fence |-> 0, refs |-> {}, log |-> <<>>]]
    /\ retired  = {} /\ inflight = {} /\ gcRound = 0
    /\ gcPhase  = [l \in Leaders |-> "idle"]
    /\ roundOf  = [l \in Leaders |-> 0]
    /\ fencedSet= [l \in Leaders |-> {}]
    /\ fencePos = [s \in Shards |-> 0]
    /\ cursor   = [s \in Shards |-> 0]
    /\ trimBase = [s \in Shards |-> 0]
    /\ rootEdges = {} /\ treeEdges = {} /\ marker = {} /\ everEdged = {} /\ pendCasc = {}
    /\ wDeps    = [w \in Writers |-> {}]
    /\ wView    = [w \in Writers |-> 0]
    /\ creator  = [h \in Hashes |-> "none"]
    /\ hbAlive  = [w \in Writers |-> FALSE]
    /\ hbSeq    = [w \in Writers |-> 0]
    /\ wedged   = [w \in Writers |-> FALSE]
    /\ hbObs    = [w \in Writers |-> -1]
    /\ fgPhase  = "idle"
    /\ fgCut    = [s \in Shards |-> 0]
    /\ fgRefs   = [s \in Shards |-> {}]
    /\ fgSeen   = {}

\* ---------------------------------------------------------------- writer actions
\* Create a missing object: fresh token from the allocator (W-SAME-CONTENT is by construction:
\* the model's key IS the content). Dependency recorded with the created token.
WCreate(w, h) ==
    /\ present[h] = FALSE /\ nextTok[h] <= MaxToken
    /\ (h \in TreeHashes => \A c \in Children[h] : present[c])
    /\ (~EnableDebris) \/ hbAlive[w]          \* W-HEARTBEAT: durable before the first object PUT
    /\ present' = [present EXCEPT ![h] = TRUE]
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ creator' = IF EnableDebris THEN [creator EXCEPT ![h] = w] ELSE creator
    /\ wDeps'   = [wDeps   EXCEPT ![w] = @ \cup {<<h, nextTok[h]>>}]
    /\ UNCHANGED << deadTok, man, retired, inflight, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wView, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs,
                    fgSeen >>

\* Cold reuse: the existence check observes the current token (token-bearing dependency).
WReuse(w, h) ==
    /\ present[h]
    /\ wDeps' = [wDeps EXCEPT ![w] = @ \cup {<<h, tokOf[h]>>}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* Anonymous environment churn: an unconditional same-content re-PUT by anyone, any time (spec §12:
\* safety must not rest on PUT conditions). No dependency recorded — writer-intent overwrites are
\* WResurrect (condemned) / WReuse (adopt). Per the deadTok MODEL RULE, the in-place overwrite makes
\* the OLD token stop being current, so it MUST join deadTok (else a stale token-bearing dep on it
\* dangles — the bug WResurrect fixed). creator gated on EnableDebris (keeps earlier stages inert).
WOverwrite(w, h) ==
    /\ EnableOverwrite /\ present[h] /\ nextTok[h] <= MaxToken
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ deadTok' = [deadTok EXCEPT ![h] = @ \cup {tokOf[h]}]
    /\ creator' = IF EnableDebris THEN [creator EXCEPT ![h] = w] ELSE creator
    /\ UNCHANGED << present, man, retired, inflight, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc,
                    wDeps, wView, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>

\* Resurrect a condemned current incarnation: overwrite in place with a FRESH token (W-FRESH-TAG).
\* The overwrite is an in-place replacement of the single physical slot, so the OLD token is gone for
\* good — record it in deadTok exactly as a physical delete would (INV-NO-RETURN: the overwritten token
\* can never again be a valid dependency, even though its retire entry is later consumed by a landing
\* or dropped as spared at the recheck).
\* Without this, a writer holding a stale token-bearing dependency on the overwritten incarnation could
\* pass the publish gate after that incarnation's retire entry drops, then a delete of the NEW token
\* leaves the logical hash absent under a live root (INV_NO_DANGLE / INV_NO_LOSS).
\* SabotageReusedTag re-issues the condemned token itself — token recurrence, must break INV_NO_RETURN.
WResurrect(w, h) ==
    /\ EnableResurrect
    /\ present[h] /\ CondemnedAtView(h, tokOf[h], wView[w])
    /\ nextTok[h] <= MaxToken
    /\ (~EnableDebris) \/ hbAlive[w]          \* W-HEARTBEAT: durable before the first object PUT
    /\ LET newt == IF SabotageReusedTag THEN tokOf[h] ELSE nextTok[h] IN
       /\ tokOf'   = [tokOf   EXCEPT ![h] = newt]
       /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
       /\ deadTok' = [deadTok EXCEPT ![h] = @ \cup {tokOf[h]}]
       /\ wDeps'   = [wDeps   EXCEPT ![w] = @ \cup {<<h, newt>>}]
    /\ creator' = IF EnableDebris THEN [creator EXCEPT ![h] = w] ELSE creator
    /\ UNCHANGED << present, man, retired, inflight, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc,
                    wView, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>

\* Carry-forward / fetch-by-reference: tokenless live-root-evidence dependency (no request made).
WEvidence(w, h) ==
    /\ EnableResurrect /\ present[h]
    /\ wDeps' = [wDeps EXCEPT ![w] = @ \cup {<<h, Ev>>}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* W-EVIDENCE escalation: a retire-view hit on the hash forces resolution to a token-bearing entry
\* (adopt the current token iff it is not condemned; a condemned current token goes via WResurrect).
WResolveEvidence(w, h) ==
    /\ EnableResurrect
    /\ <<h, Ev>> \in wDeps[w] /\ present[h]
    /\ ~CondemnedAtView(h, tokOf[h], wView[w])
    /\ wDeps' = [wDeps EXCEPT ![w] = (@ \ {<<h, Ev>>}) \cup {<<h, tokOf[h]>>}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

WRefreshView(w) ==
    /\ wView' = [wView EXCEPT ![w] = gcRound]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* Publish: one atomic successful CAS — guard reads the CURRENT manifest (CAS linearization).
\* W-PUBLISH-GATE + W-EVIDENCE.  SabotageNoRetireView removes the gate.
DepOK(w) ==
    \A d \in wDeps[w] :
        IF d[2] = Ev THEN ~HashHitAtView(d[1], wView[w])
                     ELSE ~CondemnedAtView(d[1], d[2], wView[w])
\* A published tree's children must be present AND live (not condemned in the writer's view) at
\* publish time. Models the bottom-up build (children uploaded before the tree ref is published).
\* This models the CHILD half of the dependency-set gate as a publish-time LIVE re-read of each
\* child's current token (it does NOT carry separate per-child wDeps entries) — sound because all
\* incarnations are payload-identical (W-SAME-CONTENT) and trees reference children by hash, so
\* validating the current token matches the reader contract. Without this a writer can publish a
\* tree over an absent/condemned child and dangle (INV_NO_LOSS). One-level: stage-3 trees are flat
\* (Children[t] subset of NonTree); nested subtrees are a documented residual.
TreeDepsOK(w, h) == h \in TreeHashes =>
                      \A c \in Children[h] : present[c] /\ ~CondemnedAtView(c, tokOf[c], wView[w])
WPublish(w, s, h) ==
    /\ \E t \in Toks : <<h, t>> \in wDeps[w]          \* the root itself was created/reused
    /\ h \notin man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ SabotageNoRetireView \/ (wView[w] >= man[s].fence /\ DepOK(w))
    /\ TreeDepsOK(w, h)
    /\ man'   = [man EXCEPT ![s].refs = @ \cup {h},
                            ![s].log  = Append(@, [op |-> "add", h |-> h])]
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, retired, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut,
                    fgRefs, fgSeen >>

WDrop(s, h) ==
    /\ h \in man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ man' = [man EXCEPT ![s].refs = @ \ {h},
                          ![s].log  = Append(@, [op |-> "rem", h |-> h])]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, retired, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

WAbandon(w) ==      \* crash/abort before publish: deps lost; uploads remain (debris in stage 4)
    /\ wDeps[w] # {}
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* ---------------------------------------------------------------- GC actions (per leader)
GStartRound(l) ==
    /\ gcPhase[l] = "idle" /\ gcRound < MaxRound
    /\ gcRound'  = gcRound + 1
    /\ roundOf'  = [roundOf  EXCEPT ![l] = gcRound + 1]
    /\ gcPhase'  = [gcPhase  EXCEPT ![l] = "retiring"]
    /\ fencedSet'= [fencedSet EXCEPT ![l] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, fencePos, cursor,
                    trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc, wDeps, wView,
                    creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>

\* Fold one journal record into the snap (edge-set semantics; expansion marker rule).
GFold(s) ==
    /\ cursor[s] < Len(man[s].log)
    /\ LET rec == man[s].log[cursor[s] + 1] IN
       /\ rootEdges' = CASE rec.op = "add"   -> rootEdges \cup {<<s, rec.h>>}
                         [] rec.op = "rem"   -> rootEdges \ {<<s, rec.h>>}
                         [] rec.op = "fence" -> rootEdges
       /\ IF rec.op = "add" /\ rec.h \in TreeHashes /\ rec.h \notin marker
          THEN /\ treeEdges' = treeEdges \cup { <<rec.h, c>> : c \in Children[rec.h] }
               /\ marker'    = marker \cup {rec.h}
               /\ everEdged' = everEdged \cup {rec.h} \cup Children[rec.h]
          ELSE /\ treeEdges' = treeEdges /\ marker' = marker
               /\ everEdged' = IF rec.op = "add" THEN everEdged \cup {rec.h} ELSE everEdged
    /\ cursor' = [cursor EXCEPT ![s] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, trimBase, pendCasc, wDeps, wView, creator,
                    hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>

\* Retire a journal-known, present, in-degree-0 candidate at its CURRENT token (the HEAD).
GRetire(l, h) ==
    /\ gcPhase[l] = "retiring"
    /\ present[h] /\ h \in everEdged /\ InDeg(h) = 0
    /\ ~\E e \in retired : e.h = h /\ e.t = tokOf[h]
    /\ retired' = retired \cup { [h |-> h, t |-> tokOf[h], r |-> roundOf[l]] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* Fence one shard: bump fence_round in the manifest (a CAS — appends a fence record).
\* SabotageNoFence: the fence does NOT touch the manifest (writers never blocked, horn 2 open).
GFenceShard(l, s) ==
    /\ gcPhase[l] \in {"retiring", "fencing"}
    /\ s \notin fencedSet[l]
    /\ Len(man[s].log) < MaxLog
    /\ IF SabotageNoFence
       THEN man' = man /\ fencePos' = fencePos
            \* fence is monotone (CAS-modelled): a stale leader's lower round is absorbed, never lowers it (INV-MONOTONE-GC under split-brain).
       ELSE /\ man' = [man EXCEPT ![s].fence = IF roundOf[l] > man[s].fence THEN roundOf[l] ELSE man[s].fence,
                                  ![s].log   = Append(@, [op |-> "fence"])]
            /\ fencePos' = [fencePos EXCEPT ![s] = Len(man[s].log) + 1]
    /\ fencedSet' = [fencedSet EXCEPT ![l] = @ \cup {s}]
    /\ gcPhase'   = [gcPhase   EXCEPT ![l] = IF fencedSet[l] \cup {s} = Shards
                                             THEN "fenced" ELSE "fencing"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, retired, inflight, gcRound, roundOf, cursor,
                    trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc, wDeps, wView,
                    creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>

\* Recheck + issue delete. The recheck requires the fold to have provably reached every fence
\* position (SabotageNoRecheckFold drops that — horn 1 open).  Spared entries drop at the recheck;
\* a condemned entry STAYS in `retired` (still blocking reuse) and a delete MESSAGE is sent —
\* the entry drops only when the message lands (spec §7: entries drop on confirmed outcomes only).
GRecheckDelete(l, e) ==
    /\ gcPhase[l] = "fenced" /\ e \in retired /\ e.r = roundOf[l]
    /\ SabotageNoRecheckFold \/ FoldedThroughFence
    /\ IF InDeg(e.h) > 0
       THEN /\ retired'  = retired \ {e}                      \* outcome = spared
            /\ inflight' = inflight
       ELSE /\ [h |-> e.h, t |-> e.t] \notin inflight         \* no duplicate sends (idempotent anyway)
            /\ retired'  = retired                            \* entry kept until the landing confirms
            /\ inflight' = inflight \cup { [h |-> e.h, t |-> e.t] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc,
                    wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs,
                    fgSeen >>

GEndRound(l) ==
    /\ gcPhase[l] = "fenced"
    /\ ~\E e \in retired : e.r = roundOf[l]      \* waits for landings: entries drop only on outcomes
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "idle"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* A delete message lands: exact-token (412 = no-op).  SabotageUncondDelete ignores the token.
\* The landing is the confirmed outcome: the matching retired entry drops HERE (deleted/absent/
\* replaced), never at send time.  Trees: cascade (strip child edges + marker) is ATOMIC with the
\* landing — the pipeline rule.  SabotageCascadeRace defers the strip to an arbitrary later time.
Land(d) ==
    /\ d \in inflight
    /\ inflight' = inflight \ {d}
    /\ retired'  = { e \in retired : ~(e.h = d.h /\ e.t = d.t) }
    /\ IF present[d.h] /\ (SabotageUncondDelete \/ tokOf[d.h] = d.t)
       THEN /\ present' = [present EXCEPT ![d.h] = FALSE]
            /\ deadTok' = [deadTok EXCEPT ![d.h] = @ \cup {tokOf[d.h]}]
            /\ IF d.h \in TreeHashes /\ d.h \in marker
               THEN IF SabotageCascadeRace
                    THEN /\ pendCasc'  = pendCasc \cup {d.h}
                         /\ treeEdges' = treeEdges /\ marker' = marker
                    ELSE /\ treeEdges' = { e \in treeEdges : e[1] # d.h }
                         /\ marker'    = marker \ {d.h}
                         /\ pendCasc'  = pendCasc
               ELSE UNCHANGED << treeEdges, marker, pendCasc >>
       ELSE /\ UNCHANGED << present, deadTok, treeEdges, marker, pendCasc >>   \* 412 / absent
    /\ UNCHANGED << tokOf, nextTok, man, gcRound, gcPhase, roundOf, fencedSet, fencePos,
                    cursor, trimBase, rootEdges, everEdged, wDeps, wView, creator, hbAlive,
                    hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>

\* Deferred cascade application (exists ONLY under SabotageCascadeRace; this is the race).
ApplyPendCascade(t) ==
    /\ SabotageCascadeRace /\ t \in pendCasc
    /\ treeEdges' = { e \in treeEdges : e[1] # t }
    /\ marker'    = marker \ {t}
    /\ pendCasc'  = pendCasc \ {t}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, everEdged,
                    wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut,
                    fgRefs, fgSeen >>

\* Journal trim: INV_JOURNAL_COVERAGE — only below the durable folded cursor.
Trim(s) ==
    /\ trimBase[s] < cursor[s]
    /\ trimBase' = [trimBase EXCEPT ![s] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* ---------------------------------------------------------------- stage 4: heartbeats + debris + full-GC cut
WHbStart(w) ==
    /\ EnableDebris /\ ~hbAlive[w]
    /\ hbAlive' = [hbAlive EXCEPT ![w] = TRUE]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps, wView, creator, hbSeq, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

WHbRenew(w) ==
    /\ EnableDebris /\ hbAlive[w] /\ ~wedged[w] /\ hbSeq[w] < MaxRound + 2
    /\ hbSeq' = [hbSeq EXCEPT ![w] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps, wView, creator, hbAlive, wedged, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

Wedge(w) ==        \* environment: renewal thread stuck; the writer can still publish
    /\ EnableDebris /\ ~wedged[w]
    /\ wedged' = [wedged EXCEPT ![w] = TRUE]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps, wView, creator, hbAlive, hbSeq, hbObs, fgPhase,
                    fgCut, fgRefs, fgSeen >>

WCrash(w) ==       \* process death: heartbeat gone, build state lost; uploads stay as debris
    /\ EnableDebris /\ hbAlive[w]
    /\ hbAlive' = [hbAlive EXCEPT ![w] = FALSE]
    /\ wDeps'   = [wDeps   EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView, creator, hbSeq, wedged, hbObs, fgPhase, fgCut,
                    fgRefs, fgSeen >>

GObserveHb(w) ==   \* first read of the GC observation window
    /\ EnableDebris
    /\ hbObs' = [hbObs EXCEPT ![w] = IF hbAlive[w] THEN hbSeq[w] ELSE -1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, fgPhase,
                    fgCut, fgRefs, fgSeen >>

\* Debris classification: present, never journal-known, creator's heartbeat absent OR unchanged
\* since the earlier observation (the second read happens in this guard). Same retire tail.
GDebrisRetire(l, h) ==
    /\ EnableDebris /\ gcPhase[l] = "retiring"
    /\ present[h] /\ h \notin everEdged /\ creator[h] \in Writers
    /\ LET c == creator[h] IN
       \/ ~hbAlive[c]
       \/ (hbObs[c] >= 0 /\ hbObs[c] = hbSeq[c])
    /\ ~\E e \in retired : e.h = h /\ e.t = tokOf[h]
    /\ retired' = retired \cup { [h |-> h, t |-> tokOf[h], r |-> roundOf[l]] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut,
                    fgRefs, fgSeen >>

\* ---- full-GC walk: per-shard atomic reads recording the EXACT cut; commit CAS-guarded ----
FGRead(s) ==
    /\ EnableDebris /\ fgPhase \in {"idle", "reading"} /\ s \notin fgSeen
    /\ fgCut'   = [fgCut  EXCEPT ![s] = Len(man[s].log)]
    /\ fgRefs'  = [fgRefs EXCEPT ![s] = man[s].refs]
    /\ fgSeen'  = fgSeen \cup {s}
    /\ fgPhase' = IF fgSeen \cup {s} = Shards THEN "read" ELSE "reading"
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs >>

\* Rebuild authority EXACTLY through the cut: refs snapshot -> root edges; reachable trees ->
\* expansion; cursors := the versions actually read.  Claimed authority == incorporated state.
\* The commit FAILS (walk aborts to idle) if any cursor already advanced past its cut.
\* SabotageCutOverclaim: cursors jump to CURRENT log length while edges come from the cut.
FGCommit ==
    /\ EnableDebris /\ fgPhase = "read"
    \* Under the sabotage, only commit when a publish actually landed in the read-commit gap —
    \* focuses TLC on the intended counterexample instead of vacuous overclaims.
    /\ (~SabotageCutOverclaim) \/ (\E s \in Shards : Len(man[s].log) > fgCut[s])
    /\ IF \A s \in Shards : cursor[s] <= fgCut[s]
       THEN /\ rootEdges' = UNION { { <<s, h>> : h \in fgRefs[s] } : s \in Shards }
            /\ LET trees == { h \in UNION {fgRefs[s] : s \in Shards} : h \in TreeHashes } IN
               /\ treeEdges' = UNION { { <<t, c>> : c \in Children[t] } : t \in trees }
               /\ marker'    = trees
               /\ everEdged' = UNION { Reach(r) : r \in UNION {fgRefs[s] : s \in Shards} }
            /\ cursor' = IF SabotageCutOverclaim
                         THEN [s \in Shards |-> Len(man[s].log)]
                         ELSE [s \in Shards |-> fgCut[s]]
       ELSE UNCHANGED << rootEdges, treeEdges, marker, everEdged, cursor >>
    /\ fgPhase' = "idle" /\ fgSeen' = {}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, trimBase, pendCasc, wDeps, wView, creator,
                    hbAlive, hbSeq, wedged, hbObs, fgCut, fgRefs >>

\* ---------------------------------------------------------------- next / spec
Next ==
    \/ \E w \in Writers, h \in Hashes : WCreate(w, h) \/ WReuse(w, h)
    \/ \E w \in Writers, h \in Hashes : WOverwrite(w, h)
    \/ \E w \in Writers, h \in Hashes : WResurrect(w, h) \/ WEvidence(w, h) \/ WResolveEvidence(w, h)
    \/ \E w \in Writers : WRefreshView(w) \/ WAbandon(w)
    \/ \E w \in Writers, s \in Shards, h \in Hashes : WPublish(w, s, h)
    \/ \E s \in Shards, h \in Hashes : WDrop(s, h)
    \/ \E l \in Leaders : GStartRound(l) \/ GEndRound(l)
    \/ \E s \in Shards : GFold(s) \/ Trim(s)
    \/ \E l \in Leaders, h \in Hashes : GRetire(l, h)
    \/ \E l \in Leaders, s \in Shards : GFenceShard(l, s)
    \/ \E l \in Leaders, e \in retired : GRecheckDelete(l, e)
    \/ \E d \in inflight : Land(d)
    \/ \E t \in TreeHashes : ApplyPendCascade(t)
    \/ \E w \in Writers : WHbStart(w) \/ WHbRenew(w) \/ Wedge(w) \/ WCrash(w) \/ GObserveHb(w)
    \/ \E l \in Leaders, h \in Hashes : GDebrisRetire(l, h)
    \/ \E s \in Shards : FGRead(s)
    \/ FGCommit

Spec == Init /\ [][Next]_vars

\* ---------------------------------------------------------------- invariants
TypeOK ==
    /\ present \in [Hashes -> BOOLEAN]
    /\ tokOf   \in [Hashes -> 0..MaxToken]
    /\ nextTok \in [Hashes -> 1..MaxToken+1]
    /\ deadTok \in [Hashes -> SUBSET Toks]
    /\ \A s \in Shards : man[s].fence \in 0..MaxRound /\ man[s].refs \subseteq Hashes
                         /\ man[s].log \in Seq(Rec)
    /\ gcRound \in 0..MaxRound
    /\ creator \in [Hashes -> Writers \cup {"none"}]
    /\ hbAlive \in [Writers -> BOOLEAN]
    /\ hbSeq   \in [Writers -> 0..(MaxRound + 2)]
    /\ wedged  \in [Writers -> BOOLEAN]
    /\ hbObs   \in [Writers -> {-1} \cup (0..(MaxRound + 2))]
    /\ fgPhase \in {"idle", "reading", "read"}
    /\ fgCut   \in [Shards -> 0..MaxLog]
    /\ fgRefs  \in [Shards -> SUBSET Hashes]
    /\ fgSeen  \subseteq Shards

\* Roots hold LOGICAL hashes only; the publish-time token dependency is transient by design (the
\* real publish_dependency_set is writer-local).  Hence NO_DANGLE/NO_LOSS are about logical
\* presence — any current incarnation satisfies a reader — exactly the spec's reader contract.
INV_NO_DANGLE == \A s \in Shards : \A h \in man[s].refs : present[h]
INV_NO_LOSS   == \A h \in ReachableSet : present[h]
\* INV_NO_RETURN models the LATE-DELETE-SAFETY facet of the spec's invariant (a deleted token is
\* never current again).  The publish-gate facet ("a condemned token is never a valid dependency
\* of a publish") is enforced by the W-PUBLISH-GATE guard and proven load-bearing by the
\* sab_noretireview counterexample — it is NOT a state invariant (an unpublished writer may hold
\* a dependency on a token that GC legitimately deletes; the gate catches it at publish).
INV_NO_RETURN == \A h \in Hashes : present[h] => tokOf[h] \notin deadTok[h]
\* This is the ORDERING half only of the spec's INV-JOURNAL-COVERAGE: trim never advances past the
\* durable folded cursor.  The coverage half (the journal retains every folded-but-untrimmed record)
\* holds BY CONSTRUCTION in this model — Trim only advances a watermark below cursor and never pops
\* journal records — so this invariant cannot, by itself, catch a coverage bug.
INV_JOURNAL_COVERAGE == \A s \in Shards : trimBase[s] <= cursor[s]

\* Monotonicity of GC state — checked as an action property (PROPERTY in configs).
\* NOTE on form: [][A]_vars is the standard TLC action property; the _vars subscript exempts
\* stuttering steps, so this does NOT over-constrain.  The Len(log)-monotone clause is valid
\* only because Trim advances trimBase without physically shrinking the log — if real log
\* compaction is ever modeled, revise this clause together with the Trim action.
MonotoneGC == [][ /\ gcRound' >= gcRound
                  /\ \A s \in Shards : /\ cursor'[s]   >= cursor[s]
                                       /\ trimBase'[s] >= trimBase[s]
                                       /\ man'[s].fence >= man[s].fence
                                       /\ Len(man'[s].log) >= Len(man[s].log) ]_vars

StateConstraint ==
    /\ \A s \in Shards : Len(man[s].log) <= MaxLog
    /\ Cardinality(inflight) <= 2
    /\ \A w \in Writers : Cardinality(wDeps[w]) <= 3
=======================================================================================
