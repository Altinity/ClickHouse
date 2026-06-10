# CA Incarnation-Token TLA+ Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and model-check `CaIncarnationCore.tla` — the TLA+ model of the incarnation-token CA design (spec `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md` §12 + Appendix A), replacing the invalidated `CaGcCore.tla` (kept as historical record).

**Architecture:** One module with `Enable*` feature flags and `Sabotage*` negative-control flags, checked by TLC through five staged configs (core → resurrect/evidence → trees/cascade → debris/full-GC cut → split-leaders/overwrite), following the existing `docs/superpowers/models/` staged-config pattern. Every load-bearing rule gets a sabotage config that MUST produce a counterexample — the model-checking analog of a failing test.

**Tech Stack:** TLA+ / TLC (`tmp/tla2tools.jar`, v2.19; OpenJDK 21 at `/usr/bin/java`). All runs from `docs/superpowers/models/`, output redirected to `tmp/tlc_*.log`.

**Key modeling abstractions (state these in the README task; they are deliberate):**
- Tokens are naturals allocated per key by `nextTok` — `W-FRESH-TAG` and backend token-distinctness hold *by construction*; `SabotageReusedTag` breaks exactly that to prove `INV_NO_RETURN` has teeth.
- A successful publish CAS = one atomic action guarded on the *current* manifest (CAS linearization); a writer with a stale retire view simply has the guard false until `WRefreshView` — the retry loop needs no explicit encoding.
- `Children` is derived, not a constant — TLC configs cannot express tuple/function literals. With one tree it
  maps to all non-tree hashes; with two trees the derivation is asymmetric (one full tree, one single-child
  tree via `CHOOSE`), so stage 3 covers both shared-child survival and selective cascade. Nested subtrees are
  NOT modeled (one-level closure) — a stated residual.
- Retire entries are checked against the live `retired` set; dropped entries are safe to miss (deleted/absent ⇒ reuse becomes a fresh create; replaced/spared ⇒ current token is not condemned) — mirrors the spec's entry-lifecycle argument.
- The cascade is atomic with `Land` (the round-pipeline rule); `SabotageCascadeRace` turns it into a deferred record applied at an arbitrary later time — the exact race the spec's pipeline ordering closes.
- `INV-JOURNAL-COVERAGE` is modeled with an explicit `Trim` action gated on the folded cursor.
- Heartbeat staleness = GC reads `hbSeq` once (`GObserveHb`), classifies later only if still unchanged — two reads across an arbitrary interleaving, no clocks.

---

## Task 0: Workspace and runner sanity

**Files:**
- Create: `docs/superpowers/models/run_tlc.sh`

- [ ] **Step 1: Verify Java and the TLC jar**

Run: `cd docs/superpowers/models && /usr/bin/java -cp ../../../tmp/tla2tools.jar tlc2.TLC -h 2>&1 | head -3`
Expected: TLC version banner (`TLC2 Version 2.19` or similar), no exception.

- [ ] **Step 2: Create the runner script**

```bash
#!/usr/bin/env bash
# Run one TLC config against CaIncarnationCore.tla. Usage: ./run_tlc.sh <cfg-file> [extra TLC args]
# Output goes to ../../../tmp/tlc_<cfg-basename>.log; last lines + result echoed.
set -uo pipefail
if [[ $# -lt 1 ]]; then
  echo "usage: $0 <cfg-file> [extra TLC args]" >&2
  exit 2
fi
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
CFG="$1"; shift || true
LOG="../../../tmp/tlc_$(basename "$CFG" .cfg).log"
/usr/bin/java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto -config "$CFG" "$@" CaIncarnationCore.tla >"$LOG" 2>&1
RC=$?
grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in" "$LOG" | tail -8
echo "exit=$RC log=$LOG"
exit $RC
```

Run: `chmod +x docs/superpowers/models/run_tlc.sh`

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/models/run_tlc.sh
git commit -m "CA model: TLC runner script for the incarnation-token model"
```

---

## Task 1: Stage-1 module — state, writer core, GC tail, invariants

**Files:**
- Create: `docs/superpowers/models/CaIncarnationCore.tla`

This is the complete stage-1 module. Later tasks extend it; flags for later stages already exist and default to `FALSE` in configs.

- [ ] **Step 1: Write the module**

```tla
------------------------------ MODULE CaIncarnationCore ------------------------------
(* Incarnation-token CA store core — spec: 2026-06-10-ca-incarnation-store-design.md §12 + Appendix A.
   One key per hash; tokens = naturals per key; deletes exact-token via in-flight messages that may
   land arbitrarily late; CAS root manifests with embedded journal; GC = fold -> retire -> fence ->
   recheck -> delete.  Sabotage* flags break one load-bearing rule each and MUST yield counterexamples. *)
EXTENDS Naturals, Sequences, FiniteSets

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
    deadTok,   \* [Hashes -> SUBSET Toks]          physically deleted tokens (history; INV_NO_RETURN)
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
    wView      \* [Writers -> 0..MaxRound]         highest retire round refreshed

vars == << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase, roundOf,
           fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
           pendCasc, wDeps, wView >>

\* ---------------------------------------------------------------- helpers
CondemnedAtView(h, t, v) == \E e \in retired : e.h = h /\ e.t = t /\ e.r <= v
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

\* ---------------------------------------------------------------- writer actions
\* Create a missing object: fresh token from the allocator (W-SAME-CONTENT is by construction:
\* the model's key IS the content). Dependency recorded with the created token.
WCreate(w, h) ==
    /\ present[h] = FALSE /\ nextTok[h] <= MaxToken
    /\ present' = [present EXCEPT ![h] = TRUE]
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ wDeps'   = [wDeps   EXCEPT ![w] = @ \cup {<<h, nextTok[h]>>}]
    /\ UNCHANGED << deadTok, man, retired, inflight, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wView >>

\* Cold reuse: the existence check observes the current token (token-bearing dependency).
WReuse(w, h) ==
    /\ present[h]
    /\ wDeps' = [wDeps EXCEPT ![w] = @ \cup {<<h, tokOf[h]>>}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView >>

WRefreshView(w) ==
    /\ wView' = [wView EXCEPT ![w] = gcRound]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wDeps >>

\* Publish: one atomic successful CAS — guard reads the CURRENT manifest (CAS linearization).
\* W-PUBLISH-GATE + W-EVIDENCE.  SabotageNoRetireView removes the gate.
DepOK(w) ==
    \A d \in wDeps[w] :
        IF d[2] = Ev THEN ~HashHitAtView(d[1], wView[w])
                     ELSE ~CondemnedAtView(d[1], d[2], wView[w])
WPublish(w, s, h) ==
    /\ \E t \in Toks : <<h, t>> \in wDeps[w]          \* the root itself was created/reused
    /\ h \notin man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ SabotageNoRetireView \/ (wView[w] >= man[s].fence /\ DepOK(w))
    /\ man'   = [man EXCEPT ![s].refs = @ \cup {h},
                            ![s].log  = Append(@, [op |-> "add", h |-> h])]
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, retired, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wView >>

WDrop(s, h) ==
    /\ h \in man[s].refs
    /\ Len(man[s].log) < MaxLog
    /\ man' = [man EXCEPT ![s].refs = @ \ {h},
                          ![s].log  = Append(@, [op |-> "rem", h |-> h])]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, retired, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView >>

WAbandon(w) ==      \* crash/abort before publish: deps lost; uploads remain (debris in stage 4)
    /\ wDeps[w] # {}
    /\ wDeps' = [wDeps EXCEPT ![w] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView >>

\* ---------------------------------------------------------------- GC actions (per leader)
GStartRound(l) ==
    /\ gcPhase[l] = "idle" /\ gcRound < MaxRound
    /\ gcRound'  = gcRound + 1
    /\ roundOf'  = [roundOf  EXCEPT ![l] = gcRound + 1]
    /\ gcPhase'  = [gcPhase  EXCEPT ![l] = "retiring"]
    /\ fencedSet'= [fencedSet EXCEPT ![l] = {}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, fencePos, cursor,
                    trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc, wDeps, wView >>

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
                    roundOf, fencedSet, fencePos, trimBase, pendCasc, wDeps, wView >>

\* Retire a journal-known, present, in-degree-0 candidate at its CURRENT token (the HEAD).
GRetire(l, h) ==
    /\ gcPhase[l] = "retiring"
    /\ present[h] /\ h \in everEdged /\ InDeg(h) = 0
    /\ ~\E e \in retired : e.h = h /\ e.t = tokOf[h]
    /\ retired' = retired \cup { [h |-> h, t |-> tokOf[h], r |-> roundOf[l]] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, inflight, gcRound, gcPhase, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView >>

\* Fence one shard: bump fence_round in the manifest (a CAS — appends a fence record).
\* SabotageNoFence: the fence does NOT touch the manifest (writers never blocked, horn 2 open).
GFenceShard(l, s) ==
    /\ gcPhase[l] \in {"retiring", "fencing"}
    /\ s \notin fencedSet[l]
    /\ Len(man[s].log) < MaxLog
    /\ IF SabotageNoFence
       THEN man' = man /\ fencePos' = fencePos
       ELSE /\ man' = [man EXCEPT ![s].fence = roundOf[l],
                                  ![s].log   = Append(@, [op |-> "fence"])]
            /\ fencePos' = [fencePos EXCEPT ![s] = Len(man[s].log) + 1]
    /\ fencedSet' = [fencedSet EXCEPT ![l] = @ \cup {s}]
    /\ gcPhase'   = [gcPhase   EXCEPT ![l] = IF fencedSet[l] \cup {s} = Shards
                                             THEN "fenced" ELSE "fencing"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, retired, inflight, gcRound, roundOf, cursor,
                    trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc, wDeps, wView >>

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
                    wDeps, wView >>

GEndRound(l) ==
    /\ gcPhase[l] = "fenced"
    /\ ~\E e \in retired : e.r = roundOf[l]      \* waits for landings: entries drop only on outcomes
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "idle"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, roundOf,
                    fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView >>

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
                    cursor, trimBase, rootEdges, everEdged, wDeps, wView >>

\* Deferred cascade application (exists ONLY under SabotageCascadeRace; this is the race).
ApplyPendCascade(t) ==
    /\ SabotageCascadeRace /\ t \in pendCasc
    /\ treeEdges' = { e \in treeEdges : e[1] # t }
    /\ marker'    = marker \ {t}
    /\ pendCasc'  = pendCasc \ {t}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, everEdged,
                    wDeps, wView >>

\* Journal trim: INV_JOURNAL_COVERAGE — only below the durable folded cursor.
Trim(s) ==
    /\ trimBase[s] < cursor[s]
    /\ trimBase' = [trimBase EXCEPT ![s] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, rootEdges, treeEdges, marker, everEdged,
                    pendCasc, wDeps, wView >>

\* ---------------------------------------------------------------- next / spec
Next ==
    \/ \E w \in Writers, h \in Hashes : WCreate(w, h) \/ WReuse(w, h)
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
```

- [ ] **Step 2: Parse-check (no config yet — expect a config error AFTER a clean parse)**

Run: `cd docs/superpowers/models && /usr/bin/java -cp ../../../tmp/tla2tools.jar tlc2.TLC CaIncarnationCore.tla 2>&1 | head -15`
Expected: parsing succeeds (`Semantic processing of module CaIncarnationCore` with no `***Parse Error***`); TLC then complains about a missing config/constants — that is fine. If there is a parse error, fix the reported line before proceeding (common issues: `CASE` needs `[]` separators as written; every action must list all unchanged variables).

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore.tla
git commit -m "CA model: stage-1 module — incarnation tokens, CAS manifests w/ journal, fold/retire/fence/recheck/delete"
```

---

## Task 2: Stage-1 config — PASS run

**Files:**
- Create: `docs/superpowers/models/CaIncarnationCore_stage1.cfg`

- [ ] **Step 1: Write the config**

```text
\* Stage 1: core — publish/drop, fold/retire/fence/recheck, in-flight deletes. No failures.
SPECIFICATION Spec
CONSTANTS
    Writers = {w1, w2}
    Leaders = {L1}
    Shards = {s1}
    Hashes = {h1, h2}
    TreeHashes = {}
    MaxToken = 3
    MaxRound = 2
    MaxLog = 6
    EnableResurrect = FALSE
    EnableTrees = FALSE
    EnableDebris = FALSE
    EnableSplit = FALSE
    EnableOverwrite = FALSE
    SabotageNoFence = FALSE
    SabotageNoRecheckFold = FALSE
    SabotageNoRetireView = FALSE
    SabotageUncondDelete = FALSE
    SabotageReusedTag = FALSE
    SabotageCascadeRace = FALSE
    SabotageCutOverclaim = FALSE
CONSTRAINT StateConstraint
INVARIANT TypeOK
INVARIANT INV_NO_DANGLE
INVARIANT INV_NO_LOSS
INVARIANT INV_NO_RETURN
INVARIANT INV_JOURNAL_COVERAGE
PROPERTY MonotoneGC
```

- [ ] **Step 2: Run TLC, expect PASS**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_stage1.cfg`
Expected: `Model checking completed. No error has been found.` and `exit=0`. Record the distinct-state count and wall time (needed for the RESULTS file).
If TLC reports an invariant violation here, the encoding (not the design) has a bug — read the trace in `tmp/tlc_CaIncarnationCore_stage1.log`, fix the module, re-run. Do not weaken an invariant to get green.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore_stage1.cfg
git commit -m "CA model: stage-1 config — core PASS"
```

---

## Task 3: Stage-1 sabotage configs — every gate must bleed when removed

Each config flips exactly one `Sabotage*` flag on the stage-1 base and MUST produce a counterexample. A sabotage run that passes means the invariant has no teeth for that rule — treat it as a bug in the model (or, if analysis shows the rule is genuinely not load-bearing, record that finding for the spec).

**Files:**
- Create: `docs/superpowers/models/CaIncarnationCore_sab_nofence.cfg`
- Create: `docs/superpowers/models/CaIncarnationCore_sab_norecheckfold.cfg`
- Create: `docs/superpowers/models/CaIncarnationCore_sab_noretireview.cfg`

(`sab_unconddelete` needs Task 4's `WResurrect` action — it is created AND run in Task 4, not here.)

- [ ] **Step 1: Write the three configs.** Each is a copy of `CaIncarnationCore_stage1.cfg` with exactly one line changed and the header comment replaced:

| File | Changed line | Header comment |
|---|---|---|
| `..._sab_nofence.cfg` | `SabotageNoFence = TRUE` | `\* SABOTAGE: fence does not touch manifests — post-fence writers never blocked. EXPECT violation.` |
| `..._sab_norecheckfold.cfg` | `SabotageNoRecheckFold = TRUE` | `\* SABOTAGE: recheck does not require fold-through-fence — pre-fence publishes missed. EXPECT violation.` |
| `..._sab_noretireview.cfg` | `SabotageNoRetireView = TRUE` | `\* SABOTAGE: publish gate removed — writers reuse condemned tokens. EXPECT violation.` |

- [ ] **Step 2: Run all three, expect counterexamples**

Run (each): `docs/superpowers/models/run_tlc.sh CaIncarnationCore_sab_nofence.cfg` (and `_sab_norecheckfold`, `_sab_noretireview`)
Expected: `Error: Invariant INV_NO_LOSS is violated.` (or `INV_NO_DANGLE`) and nonzero exit. **Validity rule:** a negative control is valid only if the log shows an invariant violation with a state trace — a parse or type error is NOT a counterexample; a sabotage run that PASSES is a model failure to be analyzed, never shipped. Save each trace's action sequence (TLC prints the state trace) — they go into the RESULTS file as CE-1..CE-3 analogs proving horn 1, horn 2, and the retire-view gate are each load-bearing.
If one PASSES: the corresponding rule did not bite within bounds — first try raising `MaxRound = 3` or `MaxLog = 8` in that sabotage config only; if it still passes, stop and analyze before continuing.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore_sab_*.cfg
git commit -m "CA model: stage-1 sabotage configs — fence, recheck-fold, retire-view, uncond-delete negative controls"
```

---

## Task 4: Stage 2 — resurrect, tokenless evidence, retired-old-vs-newer-current

**Files:**
- Modify: `docs/superpowers/models/CaIncarnationCore.tla` (add three actions; extend `Next`)
- Create: `docs/superpowers/models/CaIncarnationCore_stage2.cfg`

- [ ] **Step 1: Add the actions** (insert after `WReuse`, before `WRefreshView`):

```tla
\* Resurrect a condemned current incarnation: overwrite with a FRESH token (W-FRESH-TAG).
\* SabotageReusedTag re-issues the condemned token itself — token recurrence, must break INV_NO_RETURN.
WResurrect(w, h) ==
    /\ EnableResurrect
    /\ present[h] /\ CondemnedAtView(h, tokOf[h], wView[w])
    /\ nextTok[h] <= MaxToken
    /\ LET newt == IF SabotageReusedTag THEN tokOf[h] ELSE nextTok[h] IN
       /\ tokOf'   = [tokOf   EXCEPT ![h] = newt]
       /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
       /\ wDeps'   = [wDeps   EXCEPT ![w] = @ \cup {<<h, newt>>}]
    /\ UNCHANGED << present, deadTok, man, retired, inflight, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc,
                    wView >>

\* Carry-forward / fetch-by-reference: tokenless live-root-evidence dependency (no request made).
WEvidence(w, h) ==
    /\ EnableResurrect /\ present[h]
    /\ wDeps' = [wDeps EXCEPT ![w] = @ \cup {<<h, Ev>>}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView >>

\* W-EVIDENCE escalation: a retire-view hit on the hash forces resolution to a token-bearing entry
\* (adopt the current token iff it is not condemned; a condemned current token goes via WResurrect).
WResolveEvidence(w, h) ==
    /\ EnableResurrect
    /\ <<h, Ev>> \in wDeps[w] /\ present[h]
    /\ ~CondemnedAtView(h, tokOf[h], wView[w])
    /\ wDeps' = [wDeps EXCEPT ![w] = (@ \ {<<h, Ev>>}) \cup {<<h, tokOf[h]>>}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    everEdged, pendCasc, wView >>
```

- [ ] **Step 2: Extend `Next`** — add inside the writer block:

```tla
    \/ \E w \in Writers, h \in Hashes : WResurrect(w, h) \/ WEvidence(w, h) \/ WResolveEvidence(w, h)
```

- [ ] **Step 3: Write `CaIncarnationCore_stage2.cfg`** — copy of stage 1 with `EnableResurrect = TRUE` and header `\* Stage 2: + resurrect, tokenless evidence, retired-old-token-vs-newer-current-token.`

- [ ] **Step 4: Run stage 2, expect PASS**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_stage2.cfg`
Expected: `No error has been found.` This stage's state space covers the design's heart: `(h, tok_old)` retired while `h` is current at `tok_new` — the publish gate admits `tok_new` dependencies, rejects `tok_old`, and a landing zombie delete for `tok_old` no-ops.

- [ ] **Step 5: Run the two stage-2 sabotages, expect counterexamples**

Create `CaIncarnationCore_sab_reusedtag.cfg` (stage-2 base + `SabotageReusedTag = TRUE`, header `\* SABOTAGE: resurrect reuses the condemned token — token recurrence. EXPECT INV_NO_RETURN violation.`).
Create `CaIncarnationCore_sab_unconddelete.cfg` (stage-2 base + `SabotageUncondDelete = TRUE`, header `\* SABOTAGE: delete ignores the token — zombie delete kills the resurrected incarnation. EXPECT violation.`).
Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_sab_reusedtag.cfg`
Expected: `Error: Invariant INV_NO_RETURN is violated.` (or `INV_NO_LOSS` — record which).
Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_sab_unconddelete.cfg`
Expected: `Error: Invariant INV_NO_LOSS is violated.` — the zombie-delete-kills-resurrected trace.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore.tla docs/superpowers/models/CaIncarnationCore_stage2.cfg docs/superpowers/models/CaIncarnationCore_sab_reusedtag.cfg docs/superpowers/models/CaIncarnationCore_sab_unconddelete.cfg
git commit -m "CA model: stage 2 — resurrect/evidence/resolve actions, retired-old-vs-newer-current PASS, reused-tag + uncond-delete counterexamples"
```

---

## Task 5: Stage 3 — trees, expansion markers, cascade pipeline

No new actions needed — `GFold` expansion, `Land` cascade, and `ApplyPendCascade` were written tree-aware in Task 1. This task only adds the config and validates the two tree scenarios.

**Files:**
- Create: `docs/superpowers/models/CaIncarnationCore_stage3.cfg`
- Create: `docs/superpowers/models/CaIncarnationCore_sab_cascade.cfg`

- [ ] **Step 1: Write `CaIncarnationCore_stage3.cfg`** — copy of stage 2 with:

```text
    Hashes = {t1, t2, h1, h2}
    TreeHashes = {t1, t2}
    EnableTrees = TRUE
    MaxLog = 7
```

Header: `\* Stage 3: + trees — one full tree, one single-child tree (asymmetric derived Children): expansion markers, atomic cascade, shared-child survival, selective cascade, drop/re-attach replay.`
If the two-tree state space exceeds ~30 minutes, fall back to `Hashes = {t1, h1, h2}` / `TreeHashes = {t1}` / `MaxLog = 8` (the single-tree variant) and record in RESULTS that selective-cascade coverage was bound-limited.
(`EnableTrees` gates nothing in the module — trees activate via non-empty `TreeHashes`; the flag is kept for config self-documentation.)

- [ ] **Step 2: Run stage 3, expect PASS**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_stage3.cfg`
Expected: `No error has been found.` Covers: publish ref→tree (expansion adds child edges exactly once), drop + re-publish same tree (edge-set replay), tree retire/delete with atomic cascade, children surviving while any referencing tree is present (shared-child survival: the single-child tree keeps its child alive when the full tree dies), and selective cascade (deleting the full tree never strips the other tree's edges).

- [ ] **Step 3: Write `CaIncarnationCore_sab_cascade.cfg`** — stage-3 base + `SabotageCascadeRace = TRUE`, header `\* SABOTAGE: cascade deferred as a free-floating record applied at an arbitrary later time — the re-create/re-expand interleave strips a LIVE tree's child edges. EXPECT violation.`

- [ ] **Step 4: Run it, expect a counterexample**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_sab_cascade.cfg`
Expected: `Error: Invariant INV_NO_LOSS is violated.` The trace should show: tree deleted → cascade pending → identical tree re-created and re-published → fold re-expands → stale `ApplyPendCascade` strips the live tree's edges → children retire/delete while reachable. This is the spec §7 pipeline-rule justification, machine-checked.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore_stage3.cfg docs/superpowers/models/CaIncarnationCore_sab_cascade.cfg
git commit -m "CA model: stage 3 — trees + cascade pipeline PASS; cascade-as-record counterexample"
```

---

## Task 6: Stage 4 — debris, heartbeats, wedged writer, full-GC cut

**Files:**
- Modify: `docs/superpowers/models/CaIncarnationCore.tla`
- Create: `docs/superpowers/models/CaIncarnationCore_stage4.cfg`
- Create: `docs/superpowers/models/CaIncarnationCore_sab_cutoverclaim.cfg`

- [ ] **Step 1: Add stage-4 variables.** Extend `VARIABLES`, `vars`, `Init`, and `TypeOK`:

```tla
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
```

`Init` additions: `creator = [h \in Hashes |-> "none"]`, `hbAlive = [w \in Writers |-> FALSE]`, `hbSeq = [w \in Writers |-> 0]`, `wedged = [w \in Writers |-> FALSE]`, `hbObs = [w \in Writers |-> -1]`, `fgPhase = "idle"`, `fgCut = [s \in Shards |-> 0]`, `fgRefs = [s \in Shards |-> {}]`, `fgSeen = {}`. Add all nine names to `vars` and to every existing action's `UNCHANGED` list (mechanical; TLC's "variable not assigned" errors will point at any one missed).

- [ ] **Step 2: Wire creator/heartbeat into the writer actions.** In `WCreate` and `WResurrect`, replace the `UNCHANGED` of `creator` with:

```tla
    /\ creator' = [creator EXCEPT ![h] = w]
```

and add the heartbeat-before-PUT guard to `WCreate` (and `WResurrect`):

```tla
    /\ (~EnableDebris) \/ hbAlive[w]          \* W-HEARTBEAT: durable before the first object PUT
```

- [ ] **Step 3: Add the heartbeat + debris + full-GC actions** (after `Trim`):

```tla
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
       THEN /\ rootEdges' = { <<s, h>> : s \in Shards, h \in fgRefs[s] }
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
```

- [ ] **Step 4: Extend `Next`:**

```tla
    \/ \E w \in Writers : WHbStart(w) \/ WHbRenew(w) \/ Wedge(w) \/ WCrash(w) \/ GObserveHb(w)
    \/ \E l \in Leaders, h \in Hashes : GDebrisRetire(l, h)
    \/ \E s \in Shards : FGRead(s)
    \/ FGCommit
```

Also extend `TypeOK` with the new variables' domains, mirroring the declarations in Step 1.

- [ ] **Step 5: Write `CaIncarnationCore_stage4.cfg`** — copy of stage 3 with:

```text
    Shards = {s1, s2}
    EnableDebris = TRUE
    MaxLog = 6
```

Header: `\* Stage 4: + debris/heartbeats (wedged-writer publish) + full-GC exact-cut walk over two shards.`

Also write the fallback `CaIncarnationCore_stage4_small.cfg` now (same flags, smaller bounds — used only if the full stage-4 run exceeds ~45 minutes; record in RESULTS which one ran):

```text
    Writers = {w1}
    Leaders = {L1}
    Shards = {s1, s2}
    Hashes = {t1, h1}
    TreeHashes = {t1}
    MaxToken = 3
    MaxRound = 2
    MaxLog = 5
```

- [ ] **Step 6: Run stage 4, expect PASS**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_stage4.cfg`
Expected: `No error has been found.` This is the largest state space — if it exceeds ~30 minutes, reduce `MaxLog` to 5 in the stage-4 config and note the reduced bound in RESULTS. Headlines covered: wedged-heartbeat writer publishing after full GC condemned its uploads (must resurrect via the gate, never dangle); debris of a crashed writer reclaimed; live writer's edge-less uploads never deleted; torn two-shard walk with exact cuts.

- [ ] **Step 7: Write and run the cut sabotage**

`CaIncarnationCore_sab_cutoverclaim.cfg` = stage-4 base + `SabotageCutOverclaim = TRUE`, header `\* SABOTAGE: full-GC commit claims cursors at CURRENT length while edges come from the older cut — a publish in the gap is never folded. EXPECT violation.`
Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_sab_cutoverclaim.cfg`
Expected: `Error: Invariant INV_NO_LOSS is violated.` — trace: publish lands between `FGRead(s)` and `FGCommit`; overclaimed cursor skips its journal record; recheck believes fold-through-fence; referenced object deleted.

- [ ] **Step 8: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore.tla docs/superpowers/models/CaIncarnationCore_stage4.cfg docs/superpowers/models/CaIncarnationCore_sab_cutoverclaim.cfg
git commit -m "CA model: stage 4 — heartbeat-gated debris, wedged-writer publish, full-GC exact-cut walk; cut-overclaim counterexample"
```

---

## Task 7: Stage 5 — split leaders + unconditional overwrite

**Files:**
- Modify: `docs/superpowers/models/CaIncarnationCore.tla` (one action)
- Create: `docs/superpowers/models/CaIncarnationCore_stage5.cfg`

- [ ] **Step 1: Add the overwrite action** (after `WReuse`). This is **anonymous environment churn** — a raced, duplicated, or retried unconditional PUT landing at any time; same logical content by construction, fresh token; spec §12 requires safety not to rest on PUT conditions. It deliberately adds **no dependency**: a writer that wants to depend on the fresh incarnation performs `WReuse` afterward (observing the new token), and the writer-overwrite-then-publish path is already `WResurrect`'s job:

```tla
\* Anonymous environment churn: an unconditional same-content re-PUT by anyone, any time.
\* No dependency recorded — writer-intent overwrites are WResurrect (condemned) / WReuse (adopt).
WOverwrite(w, h) ==
    /\ EnableOverwrite /\ present[h] /\ nextTok[h] <= MaxToken
    /\ tokOf'   = [tokOf   EXCEPT ![h] = nextTok[h]]
    /\ nextTok' = [nextTok EXCEPT ![h] = @ + 1]
    /\ creator' = [creator EXCEPT ![h] = w]
    /\ UNCHANGED << present, deadTok, man, retired, inflight, gcRound, gcPhase, roundOf, fencedSet,
                    fencePos, cursor, trimBase, rootEdges, treeEdges, marker, everEdged, pendCasc,
                    wDeps, wView, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut, fgRefs, fgSeen >>
```

Extend `Next`: `\/ \E w \in Writers, h \in Hashes : WOverwrite(w, h)`.

- [ ] **Step 2: Write `CaIncarnationCore_stage5.cfg`** — copy of stage 4 with:

```text
    Leaders = {L1, L2}
    EnableSplit = TRUE
    EnableOverwrite = TRUE
    MaxLog = 5
```

Header: `\* Stage 5: + split-brain leaders (two concurrent GC round machines) + unconditional overwrite.`
(`EnableSplit` gates nothing — split-brain is simply `|Leaders| = 2`, since every GC action is already guard-based on shared durable state with per-leader phase machines; the flag documents intent.)

Also write the fallback `CaIncarnationCore_stage5_small.cfg` (used only if the full run exceeds ~45 minutes; record which ran):

```text
    Writers = {w1}
    Leaders = {L1, L2}
    Shards = {s1}
    Hashes = {h1, h2}
    TreeHashes = {}
    MaxToken = 3
    MaxRound = 2
    MaxLog = 5
```

- [ ] **Step 3: Run stage 5, expect PASS**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_stage5.cfg`
Expected: `No error has been found.` Two leaders interleave retire/fence/recheck/delete on shared state; deletes stay token-exact; `MonotoneGC` holds. If state-space explodes (> ~45 min), drop `Hashes` to `{t1, h1}` for this stage and record the bound.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore.tla docs/superpowers/models/CaIncarnationCore_stage5.cfg
git commit -m "CA model: stage 5 — split leaders + unconditional overwrite PASS"
```

---

## Task 8: Liveness — no-leak-forever (exploratory)

**Files:**
- Modify: `docs/superpowers/models/CaIncarnationCore.tla` (fair spec + property)
- Create: `docs/superpowers/models/CaIncarnationCore_stage2_live.cfg`

- [ ] **Step 1: Add the fair spec and the property** (at the end of the module, before the close):

```tla
\* Liveness (checked only on the small stage-2 bounds): a permanently-unreachable journal-known
\* object is eventually deleted.  Fairness on the GC pipeline + delete landing.
FairSpec == Spec /\ WF_vars(\E s \in Shards : GFold(s))
                 /\ WF_vars(\E l \in Leaders : GStartRound(l))
                 /\ WF_vars(\E l \in Leaders, h \in Hashes : GRetire(l, h))
                 /\ WF_vars(\E l \in Leaders, s \in Shards : GFenceShard(l, s))
                 /\ WF_vars(\E l \in Leaders, e \in retired : GRecheckDelete(l, e))
                 /\ WF_vars(\E l \in Leaders : GEndRound(l))
                 /\ WF_vars(\E d \in inflight : Land(d))

NoLeakForever ==
    \A h \in Hashes :
        [](( present[h] /\ h \in everEdged /\ h \notin ReachableSet )
           => <>( ~present[h] \/ h \in ReachableSet ))
```

- [ ] **Step 2: Write `CaIncarnationCore_stage2_live.cfg`** — copy of stage 2 with `SPECIFICATION FairSpec`, all `INVARIANT` lines kept, `PROPERTY MonotoneGC` replaced by `PROPERTY NoLeakForever`, and `MaxLog = 4` (liveness checking is expensive). Header: `\* Liveness: no-leak-forever under fairness; exploratory bounds.`

- [ ] **Step 3: Run, interpret honestly**

Run: `docs/superpowers/models/run_tlc.sh CaIncarnationCore_stage2_live.cfg`
Expected: PASS, **or** a liveness counterexample showing a leak-only lasso (e.g., a round that never retires a particular candidate because `gcRound` hit `MaxRound`). A bound-artifact lasso (rounds exhausted) is not a design bug — raise `MaxRound` to 3 and re-run; if it persists as a genuine non-bound lasso, record it as a finding for the spec's liveness section. Do not silently drop the property.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore.tla docs/superpowers/models/CaIncarnationCore_stage2_live.cfg
git commit -m "CA model: liveness — no-leak-forever under fairness (exploratory bounds)"
```

---

## Task 9: README, RESULTS, and the historical banner

**Files:**
- Create: `docs/superpowers/models/CaIncarnationCore_README.md`
- Create: `docs/superpowers/models/CaIncarnationCore_RESULTS.md`
- Modify: `docs/superpowers/models/README.md` (top banner only)

- [ ] **Step 1: Banner the old README.** Insert as the new first paragraph of `docs/superpowers/models/README.md`:

```markdown
> **HISTORICAL.** This model (`CaGcCore.tla`) checks the superseded EBR/epoch/generation design
> (`2026-06-07-ca-merkle-store-design.md`). The current model is `CaIncarnationCore.tla` — see
> `CaIncarnationCore_README.md`. Kept, with `RESULTS.md`, as the record of the EBR-era checking.
```

- [ ] **Step 2: Write `CaIncarnationCore_README.md`** — same shape as the old README: source spec pointer (`2026-06-10-ca-incarnation-store-design.md` §12 + Appendix A), what is modeled (the bullet list from this plan's "Key modeling abstractions" plus the variables/actions overview), what is deliberately NOT modeled (pack byte ranges; manifest size bounds; the `O(delta)` fold data plane internals; GCS/Azure token bindings — token distinctness is a parameter; reader path; provenance; **nested subtrees — tree closure is one-level**), a flags table making the documentation-only constants explicit:

```markdown
| Constant | Effect |
|---|---|
| EnableResurrect, EnableDebris, EnableOverwrite | gate actions |
| EnableTrees  | documentation only — non-empty TreeHashes activates trees |
| EnableSplit  | documentation only — |Leaders| = 2 activates split-brain |
| Sabotage*    | negative controls; exactly one TRUE per sabotage config |
```

then the run table:

```markdown
## How to run

```bash
# from docs/superpowers/models; jar at ../../../tmp/tla2tools.jar (v2.19, OpenJDK 21)
./run_tlc.sh CaIncarnationCore_stage1.cfg
./run_tlc.sh CaIncarnationCore_stage2.cfg
./run_tlc.sh CaIncarnationCore_stage3.cfg
./run_tlc.sh CaIncarnationCore_stage4.cfg
./run_tlc.sh CaIncarnationCore_stage5.cfg
./run_tlc.sh CaIncarnationCore_stage2_live.cfg
# negative controls — these MUST fail:
for c in nofence norecheckfold noretireview unconddelete reusedtag cascade cutoverclaim; do
  ./run_tlc.sh CaIncarnationCore_sab_$c.cfg && echo "UNEXPECTED PASS: $c"
done
```
```

plus a stage table (stage | config | adds | expected) mirroring this plan's tasks.

- [ ] **Step 3: Write `CaIncarnationCore_RESULTS.md`** from the recorded runs. Required structure (fill every cell from the actual TLC logs in `tmp/tlc_*.log` — no invented numbers):

```markdown
# CA incarnation-token core — TLC model-checking results

TLC (v2.19, OpenJDK 21) on `CaIncarnationCore.tla`. Bounded model checking, not a proof.

## PASS stages
| Stage | Adds | Distinct states | Wall time | Result |
|---|---|---|---|---|
| 1 core | … | … | … | PASS/FAIL |
| 2 resurrect/evidence | … | … | … | … |
| 3 trees/cascade | … | … | … | … |
| 4 debris/full-GC cut | … | … | … | … |
| 5 split/overwrite | … | … | … | … |
| liveness (stage-2 bounds) | … | … | … | … |

## Negative controls (each MUST produce a counterexample)
A negative-control PASS is a model failure — analyze before shipping. A run failing on a
parse/type error is NOT a valid counterexample; only an invariant violation with a trace counts.
| Sabotage | Rule it removes | Violated invariant | Trace summary (3-6 lines) |
|---|---|---|---|
| nofence | fence blocks post-fence writers (horn 2) | … | … |
| norecheckfold | recheck folds through fence versions (horn 1) | … | … |
| noretireview | W-PUBLISH-GATE retire-view check | … | … |
| unconddelete | exact-token delete | … | … |
| reusedtag | W-FRESH-TAG / token distinctness | … | … |
| cascade | cascade-as-pipeline-step ordering | … | … |
| cutoverclaim | full-GC claimed-authority = incorporated-state | … | … |

## Counterexamples found during development (if any)
Same format as the old RESULTS.md: CE-n, what happened, design constraint vs modeling artifact.

## Residual untested surface
(enumerate honestly — at minimum: pack range addressing, manifest bounds/trim data plane,
token-distinctness assumed as parameter, one-level tree closure (no nested subtrees), single pool,
bounds themselves, and which fallback small configs were used instead of the full ones)
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaIncarnationCore_README.md docs/superpowers/models/CaIncarnationCore_RESULTS.md docs/superpowers/models/README.md
git commit -m "CA model: README + RESULTS for the incarnation-token model; banner CaGcCore as historical"
```

---

## Task 10: Spec cross-check and closeout

- [ ] **Step 1: Check the spec's must-check list against the model.** Open `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md` §12. For each must-check scenario, name the stage/sabotage that covers it:
fence horns → stage 1 + `sab_nofence`/`sab_norecheckfold`; retired-old-vs-newer-current → stage 2; zombie-after-resurrect → stage 2 + `sab_unconddelete`; spared-entry-then-stale-delete → stage 1 (Land of a spared entry's message); cascade-vs-recreate → stage 3 + `sab_cascade`; full-GC cut-vs-cursor → stage 4 + `sab_cutoverclaim`; wedged-heartbeat publish → stage 4; drop/re-attach replay → stage 3; `INV_JOURNAL_COVERAGE` → `Trim` in all stages; `MonotoneGC` → all stages; **debris retire fences ALL root shards, never only the creator's** → by construction in stage 4 (`GRecheckDelete` requires the round's `fencedSet` to cover `Shards` via the `"fenced"` phase) — verify this stays true if the phase machine is ever refactored. If anything in §12 has no covering stage, add a scenario or record it in RESULTS' residual list — do not skip silently.

- [ ] **Step 2: Update the spec's §12 with a pointer.** Add one line at the top of §12: `The model lives in docs/superpowers/models/CaIncarnationCore.tla (see CaIncarnationCore_README.md; results in CaIncarnationCore_RESULTS.md).` — keeping the anchor and formatting rules of the file.

- [ ] **Step 3: Final commit**

```bash
git add docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md
git commit -m "CA spec: link the verification section to CaIncarnationCore model + results"
```
