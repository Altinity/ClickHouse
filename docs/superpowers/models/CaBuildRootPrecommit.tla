-------------------------- MODULE CaBuildRootPrecommit --------------------------
(* CA build-root / precommit redesign — the B140-dangle REAL fix (B171, 2026-06-18).
   Spec: ../specs/2026-06-18-ca-build-root-precommit-design.md
   Root cause: ../reports/2026-06-18-ca-b140-dangle-trigger-pinned.md

   ------------------------------------------------------------------------------
   WHAT THIS MODELS
   ------------------------------------------------------------------------------
   Two root kinds over a small object space:
     - a TABLE root (reader-facing): tableRefs \subseteq Trees. INV_NO_DANGLE_COMMITTED
       is enforced HERE, and only here.
     - a BUILD root (precommit intent): precommit[bld] \in Trees \cup {None}. An intent
       structure; it may legitimately reference absent objects.

   Object space (shared blob is essential to the dangle):
     - Trees = {t1, t2}, Blobs = {b1}; BOTH t1 and t2 reference b1 (dedup).
     - t1 = the "independent" pin (a pre-existing committed ref over b1).
     - t2 = the tree a slow, in-flight build wants to publish; it ADOPTS the shared b1.

   Two builds (Builds = {bld1, bld2}) so one build's protection lapse races another's
   reference: bld1 writes/owns b1; bld2 adopts b1 and tries to publish t2.

   ------------------------------------------------------------------------------
   THE BUGGY MODE  (UseBuildRoot=FALSE, FailClosedCommit=FALSE)
   ------------------------------------------------------------------------------
   Protection is today's per-object revocable hint (cas_owner / protectedByLiveBuild):
     - WriteBlob stamps owner[b] := the writing build.
     - AdoptBlob moves no bytes and re-stamps NOTHING (the spec's structural flaw #1):
       owner stays whoever WROTE the blob.
     - GcDelete refuses to delete a blob whose owner build is still live
       (protectedByLiveBuild). But once the OWNER build dies/retires, protection lapses
       even though a DIFFERENT, still-live adopting build holds the blob.
     - Commit publishes the table ref with NO final presence re-check (publishes blindly).
   => adopt -> drop independent pin -> owner dies -> GcDelete -> Commit  ==> dangle.

   ------------------------------------------------------------------------------
   THE FIXED MODE  (UseBuildRoot=TRUE, FailClosedCommit=TRUE)
   ------------------------------------------------------------------------------
     - Precommit publishes a build-root ref -> tree edge. GcFold walks the build root, so
       every object reachable from a LIVE build's precommit has in-degree >= 1 and
       structurally cannot be GcDelete'd (INV_BUILDROOT_PROTECTS).
     - Commit is fail-closed: it publishes a table ref ONLY if the full closure of the
       tree is present; otherwise it ABORTS (and may retry). Even after a premature
       GcReclaimPrecommit, the commit re-checks presence and refuses to publish a dangle
       (INV_COMMIT_FAILCLOSED).
     - GcReclaimPrecommit may drop a precommit whose owning build is judged dead (clean
       retire or crash). A FALSE-positive (BuildDie heartbeat freeze) can drop a live
       build's precommit; fail-closed commit still saves correctness.

   The two flags are independent so the 2x2 table isolates which mechanism is load-bearing.

   ------------------------------------------------------------------------------
   B199-S2 EXTENSION (2026-06-23): INLINE CLOSURE — the never-expanded-tree leak
   ------------------------------------------------------------------------------
   Spec: ../specs/2026-06-23-ca-precommit-inline-closure-design.md (§3/§4).

   THE S2 LEAK (space-only; dangling=0, not data loss). GC learns a precommit's
   closure by EXPANDING the tree object (reading trees/<hash> once, recording
   tree->child edges). If the tree object is GONE before that read (a stale/competing
   leader delete; lease is work-dedup, not safety), the expansion 404s, the closure is
   never recorded, and on reclaim the build's unique blobs are never released ->
   they leak as unreachable debris forever.

   THE FIX. The precommit records its closure INLINE in the ref payload at precommit
   time (the writer holds the full staged structure in hand — the only safe capture
   point; a condemned/deleted object must never be GET-ed to recover it). GC seeds the
   protection edges from that RECORDED closure (no tree read, never 404s -> S2 closed by
   construction). On reclaim, GC mirror-drops the closure edges; the children fall to
   in-degree 0 and go through the existing retire->delete tail. A closure member that
   was never uploaded (partial build) is simply already-absent: its delete is an
   idempotent no-op (deleteExact NotFound).

   MODELED HERE.
     - `closure[bld]`  : the recorded inline closure (set of object ids), seeded at
                          Precommit. Protection + reclaim both source edges from THIS,
                          not from reading the (possibly gone) tree object.
     - `uploaded[h]`   : the object's bytes were actually uploaded. A closure member can
                          be recorded /\ ~uploaded (the partial-build / never-uploaded
                          case); present[h] => uploaded[h].
     - `InlineClosure` : TRUE  = fix  (precommit records the full closure inline; GC
                          seeds from it; never depends on the tree object existing).
                          FALSE = old lazy path (closure is recorded only if the tree
                          object is present to be expanded at fold time; a gone tree =>
                          empty closure => the S2 leak).
   INV_NO_LEAK (liveness): for an abandoned build, every closure member with no other
   live reference is eventually NOT present (reclaimed). Holds with InlineClosure=TRUE;
   the leak (FALSE) starves it.
*)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Builds,             \* in-flight builds, e.g. {bld1, bld2}
    Trees,              \* tree hashes, e.g. {t1, t2}
    Blobs,              \* blob hashes, e.g. {b1}
    UseBuildRoot,       \* FALSE = buggy (protection = revocable owner hint, no build root)
                        \* TRUE  = fix  (precommit edge protects reachable present objects)
    FailClosedCommit,   \* FALSE = buggy (commit publishes table ref with no presence check)
                        \* TRUE  = fix  (commit verifies full closure present, else aborts)
    InlineClosure,      \* FALSE = old lazy path (closure recorded only if tree readable -> S2 leak)
                        \* TRUE  = fix  (precommit records closure inline; GC seeds + reclaims from it)
    BuildTree,          \* \in Trees        the abandoned-precommit's manifest tree; it references ALL
                        \*                  of Blobs (the shared blob(s) PLUS any unique-to-this-build).
    UniqueToBuildTree   \* SUBSET Blobs     blobs referenced ONLY by BuildTree (unique to that build);
                        \*                  other trees reference Blobs \ UniqueToBuildTree (the shared
                        \*                  part). {} reproduces the single-shared-blob topology.

ASSUME Trees \cap Blobs = {}
ASSUME BuildTree \in Trees
ASSUME UniqueToBuildTree \subseteq Blobs
Hashes == Trees \cup Blobs
None    == "none"

\* A tree references its subset of blobs (dedup: distinct trees may share a blob). The BuildTree
\* references everything (shared + its unique blobs); every other tree references only the shared part.
\* This makes "unique vs shared" expressible without inline cfg function literals (the TLC cfg parser
\* rejects @@/:> as a constant value): the per-tree map is derived from two simple model-value constants.
Children(t) == IF t = BuildTree THEN Blobs ELSE (Blobs \ UniqueToBuildTree)
Reach(t)    == {t} \cup Children(t)

VARIABLES
    present,        \* [Hashes -> BOOLEAN]            durable object exists in the pool
    tableRefs,      \* SUBSET Trees                   reader-facing table-namespace refs (TRUTH)
    owner,          \* [Blobs -> Builds \cup {None}]  buggy-mode protection hint: writer build
    precommit,      \* [Builds -> Trees \cup {None}]  build-root ref -> tree (precommit intent)
    buildLive,      \* [Builds -> BOOLEAN]            TRUE = build truly running (can still act)
    judgedDead,     \* SUBSET Builds                  GC's liveness VERDICT: seq < min_active or
                    \*                                epoch mismatch. May be a FALSE POSITIVE (a
                    \*                                frozen-but-live build whose heartbeat lapsed).
    committed,      \* SUBSET Trees                   trees a build successfully committed (table)
    aborted,        \* SUBSET Builds                  builds whose Commit aborted (fail-closed)
    adopted,        \* [Builds -> SUBSET Blobs]       blobs a build has adopted (intends to ref)
    everDangle,     \* BOOLEAN                         latch: a committed table ref ever dangled
    uploaded,       \* [Hashes -> BOOLEAN]             bytes were uploaded (present => uploaded);
                    \*                                 recorded /\ ~uploaded = never-uploaded member
    closure,        \* [Builds -> SUBSET Hashes]       RECORDED inline closure seeded at Precommit:
                    \*                                 GC seeds protection edges from this, not from
                    \*                                 reading the (possibly gone) tree object
    everSnapped,    \* SUBSET Hashes                   monotone: objects GC ever enumerated into the
                    \*                                 in-degree snap (table-reachable, or recorded in
                    \*                                 a precommit closure). The S2 leak surface: an
                    \*                                 object NEVER snapped is invisible debris GC
                    \*                                 cannot reclaim, even at in-degree 0.
    staged          \* SUBSET Hashes                   monotone: the TRUE staged structure of every
                    \*                                 precommit ({t} \cup Children(t)), independent of
                    \*                                 whether the closure was recorded inline. This is
                    \*                                 the SHOULD-be-reclaimable set INV_NO_LEAK ranges
                    \*                                 over -- the fix records it inline (-> snapped ->
                    \*                                 reclaimed); the lazy path drops it (-> leak).

vars == << present, tableRefs, owner, precommit, buildLive, judgedDead,
           committed, aborted, adopted, everDangle, uploaded, closure, everSnapped, staged >>

\* ----------------------------------------------------------------- helpers
TableClosure == UNION { Reach(t) : t \in tableRefs }

\* An object is protected from GcDelete.
\* BUGGY: protected iff some still-live build OWNS it (the cas_owner hint). Adoption never
\* re-stamps owner, so this tracks the BYTE-WRITER, not the referencing build.
OwnerProtected(h) ==
    /\ h \in Blobs
    /\ owner[h] # None
    /\ buildLive[owner[h]]
    /\ owner[h] \notin judgedDead

\* The edges a precommit contributes to the snap. B199-S2: these are seeded from the RECORDED
\* inline closure (closure[bld] -> the root precommit[bld] plus its recorded children), NOT by
\* reading the tree object. So an in-flight build pins exactly {precommit[bld]} \cup closure[bld].
\* (With InlineClosure=FALSE the closure may be empty even though the tree exists -> the S2 leak.)
BuildRootEdges(bld) ==
    IF precommit[bld] = None THEN {} ELSE {precommit[bld]} \cup closure[bld]

\* FIXED: protected iff reachable from a LIVE build's precommit, sourced from the inline closure.
BuildRootProtected(h) ==
    \E bld \in Builds :
        /\ buildLive[bld]
        /\ h \in BuildRootEdges(bld)

Protected(h) ==
    IF UseBuildRoot THEN BuildRootProtected(h) ELSE OwnerProtected(h)

\* In-degree from durable references that actually pin (table refs always; build-root
\* precommit edges pin too in the fixed model, seeded from the recorded inline closure).
PinnedByTable(h)     == \E t \in tableRefs : h \in Reach(t)
PinnedByBuildRoot(h) == UseBuildRoot /\ (\E bld \in Builds : h \in BuildRootEdges(bld))
InDegZero(h)         == ~PinnedByTable(h) /\ ~PinnedByBuildRoot(h)

\* B199-S2 GC VISIBILITY (the in-degree snap). GC reclaims an object only if it appears in the
\* snap -- which is built by expanding the things GC knows about: table refs (always expanded) and,
\* in the fixed model, the precommit's RECORDED inline closure (BuildRootEdges). An object that was
\* never recorded in any closure and is not table-reachable is invisible "unreachable debris" GC
\* never enumerates -> it cannot be reclaimed. This is precisely the S2 leak surface: when the lazy
\* path (InlineClosure=FALSE) records an empty closure for a gone tree, the children are never
\* snapped and leak forever. Membership is MONOTONE (everSnapped) -- once enumerated, a later
\* in-degree drop (e.g. reclaim mirror-dropping the closure edges) does not un-snap the object, so
\* GC can still delete it. What it never re-derives is an object it never enumerated in the first place.
NewlySnapped(h) == PinnedByTable(h) \/ (\E bld \in Builds : h \in BuildRootEdges(bld))

Init ==
    /\ present       = [h \in Hashes |-> FALSE]
    /\ tableRefs     = {}
    /\ owner         = [b \in Blobs  |-> None]
    /\ precommit     = [bld \in Builds |-> None]
    /\ buildLive     = [bld \in Builds |-> TRUE]
    /\ judgedDead = {}
    /\ committed     = {}
    /\ aborted       = {}
    /\ adopted       = [bld \in Builds |-> {}]
    /\ everDangle    = FALSE
    /\ uploaded      = [h \in Hashes |-> FALSE]
    /\ closure       = [bld \in Builds |-> {}]
    /\ everSnapped   = {}
    /\ staged        = {}

\* ----------------------------------------------------------------- actions

\* A build writes a fresh blob to the pool. Stamps owner = this build (the cas_owner hint).
WriteBlob(bld, b) ==
    /\ buildLive[bld]
    /\ ~present[b]
    /\ present'  = [present  EXCEPT ![b] = TRUE]
    /\ uploaded' = [uploaded EXCEPT ![b] = TRUE]          \* bytes landed in the pool
    /\ owner'    = [owner    EXCEPT ![b] = bld]
    /\ adopted'  = [adopted  EXCEPT ![bld] = @ \cup {b}]  \* writer trivially "holds" its blob
    /\ UNCHANGED << tableRefs, precommit, buildLive, judgedDead, committed, aborted, everDangle, closure, everSnapped, staged >>

\* A build ADOPTS an already-present blob (dedup): no bytes moved, owner UNCHANGED.
\* It records the blob in its dep set (adopted[bld]). In the buggy model this confers NO
\* protection (owner still points at the original writer, structural flaw #1). A build cannot
\* adopt an absent blob (it must be present at adopt time -- the soak's adopt-the-live-
\* incarnation step). The blob may later be deleted out from under the adopter.
AdoptBlob(bld, b) ==
    /\ buildLive[bld]
    /\ present[b]
    /\ adopted' = [adopted EXCEPT ![bld] = @ \cup {b}]
    /\ UNCHANGED << present, tableRefs, owner, precommit, buildLive, judgedDead, committed, aborted, everDangle, uploaded, closure, everSnapped, staged >>

\* A build publishes a TABLE ref directly (the pre-existing independent pin, e.g. t1 over b1).
\* Requires the closure present. Models content already committed by some prior, now-retired
\* path; this is the "independent reference" whose drop can take in-degree to 0.
PublishTableRef(t) ==
    /\ \A c \in Children(t) : present[c]
    /\ t \notin tableRefs
    /\ present'   = [present  EXCEPT ![t] = TRUE]
    /\ uploaded'  = [uploaded EXCEPT ![t] = TRUE]
    /\ tableRefs' = tableRefs \cup {t}
    /\ committed' = committed \cup {t}
    /\ everSnapped' = everSnapped \cup Reach(t)   \* table ref is expanded -> its closure is snapped
    /\ UNCHANGED << owner, precommit, buildLive, judgedDead, aborted, adopted, everDangle, closure, staged >>

\* FIXED ONLY: a build publishes its precommit (build-root ref -> tree). Tiny write of hashes;
\* may be published before the bytes land (build root tolerates absent objects). B199-S2: the
\* precommit RECORDS its closure INLINE here (the writer holds the staged structure in hand).
\*   InlineClosure=TRUE  : record the full structural closure (the staged children), sourced from
\*                         the payload -- NEVER depends on the tree object existing. S2 closed by
\*                         construction: the children are recorded even for a never-uploaded tree.
\*   InlineClosure=FALSE : the OLD lazy path -- GC would learn the closure only by reading the tree
\*                         OBJECT at fold time, so the closure is recorded ONLY if the tree is
\*                         present to be expanded; a gone/un-uploaded tree => EMPTY closure (the
\*                         S2 leak: the children are never recorded, never protected, never reclaimed).
Precommit(bld, t) ==
    /\ UseBuildRoot
    /\ buildLive[bld]
    /\ precommit[bld] = None
    /\ precommit' = [precommit EXCEPT ![bld] = t]
    /\ closure'   = [closure EXCEPT ![bld] =
                       IF InlineClosure \/ present[t] THEN Children(t) ELSE {}]
    \* The recorded closure (root + children) enters the snap. With InlineClosure=FALSE and a gone
    \* tree, closure'[bld] is empty, so the children are NOT snapped here -> the S2 leak surface.
    /\ everSnapped' = everSnapped \cup {t} \cup closure'[bld]
    \* The TRUE staged structure ({t} \cup its real children) -- recorded regardless of InlineClosure.
    \* INV_NO_LEAK ranges over this: with the fix it equals closure' (-> snapped); the lazy path
    \* records a SUBSET into the snap yet the full structure was still staged -> the leaked remainder.
    /\ staged'      = staged \cup {t} \cup Children(t)
    /\ UNCHANGED << present, tableRefs, owner, buildLive, judgedDead, committed, aborted, adopted, everDangle, uploaded >>

\* An INDEPENDENT table ref is dropped (its in-degree contribution disappears). This is the
\* DropTableRef that can take the shared blob to in-degree 0 while another build still wants it.
DropTableRef(t) ==
    /\ t \in tableRefs
    /\ tableRefs' = tableRefs \ {t}
    /\ UNCHANGED << present, owner, precommit, buildLive, judgedDead, committed, aborted, adopted, everDangle, uploaded, closure, everSnapped, staged >>

\* The real COMMIT: build bld publishes a table ref -> its manifest tree t. The build must
\* have adopted/written every blob in t's closure (it assembled the manifest). The manifest
\* tree itself is a tiny write done here (present[t] := TRUE), referencing the blobs by hash.
\*   FailClosedCommit=TRUE  : publish IFF the whole closure is present, else take CommitAbort.
\*   FailClosedCommit=FALSE : publish blindly (no presence re-check on the adopted blobs) --
\*                            the spec's structural flaw #2. If a blob was deleted after adopt,
\*                            the published table ref dangles.
Commit(bld, t) ==
    /\ buildLive[bld]
    /\ t \notin tableRefs
    /\ Children(t) \subseteq adopted[bld]        \* the build assembled this tree's closure
    /\ (UseBuildRoot => precommit[bld] = t)      \* fixed: must have precommitted this tree
    /\ FailClosedCommit => (\A c \in Children(t) : present[c])  \* fail-closed presence gate
    /\ present'   = [present  EXCEPT ![t] = TRUE] \* write the manifest tree
    /\ uploaded'  = [uploaded EXCEPT ![t] = TRUE]
    /\ tableRefs' = tableRefs \cup {t}
    /\ committed' = committed \cup {t}
    (* The parentheses are load-bearing: `x' = a \/ cond` parses as `(x' = a) \/ cond`, so with
       FailClosedCommit = FALSE (the only setting under which the right disjunct can be true here)
       TLC takes the right disjunct and leaves everDangle' unspecified -- "successor state not
       completely specified". Every ghost latch in this model set wraps its disjunction. *)
    /\ everDangle' = (everDangle \/ (\E c \in Children(t) : ~present[c]))
    /\ everSnapped' = everSnapped \cup Reach(t)   \* committed table ref is expanded -> snapped
    /\ UNCHANGED << owner, precommit, buildLive, judgedDead, aborted, adopted, closure, staged >>

\* Commit ABORT path (fail-closed): closure incomplete => do NOT publish; mark aborted (retry).
\* This is the path that saves correctness after a premature GcReclaimPrecommit + GcDelete.
CommitAbort(bld, t) ==
    /\ FailClosedCommit
    /\ buildLive[bld]
    /\ t \notin tableRefs
    /\ Children(t) \subseteq adopted[bld]
    /\ (UseBuildRoot => precommit[bld] = t)
    /\ \E c \in Children(t) : ~present[c]         \* a blob in the closure is missing
    /\ aborted' = aborted \cup {bld}
    /\ UNCHANGED << present, tableRefs, owner, precommit, buildLive, judgedDead, committed, adopted, everDangle, uploaded, closure, everSnapped, staged >>

\* FIXED ONLY: remove a precommit after a successful commit (normal happy-path teardown).
RemovePrecommit(bld) ==
    /\ UseBuildRoot
    /\ precommit[bld] # None
    /\ precommit[bld] \in tableRefs               \* its tree is now a real table ref
    /\ precommit' = [precommit EXCEPT ![bld] = None]
    /\ closure'   = [closure   EXCEPT ![bld] = {}] \* drop the inline-closure edges (committed tree pins now)
    /\ UNCHANGED << present, tableRefs, owner, buildLive, judgedDead, committed, aborted, adopted, everDangle, uploaded, everSnapped, staged >>

\* A build truly dies (clean retire or hard crash): it stops acting AND GC judges it dead.
\* In the buggy model this is what makes a blob's owner-protection lapse (min_active rises).
BuildDie(bld) ==
    /\ buildLive[bld]
    /\ buildLive'  = [buildLive EXCEPT ![bld] = FALSE]
    /\ judgedDead' = judgedDead \cup {bld}        \* seq < min_active / epoch mismatch
    /\ UNCHANGED << present, tableRefs, owner, precommit, committed, aborted, adopted, everDangle, uploaded, closure, everSnapped, staged >>

\* FALSE-POSITIVE freeze (§4.6 residual fragility): the build's background heartbeat renewer
\* stalls (e.g. an S3 retry storm), so GC JUDGES it dead -- but it is still RUNNING (buildLive
\* stays TRUE) and will later commit. This is the input that lets GcReclaimPrecommit fire on a
\* still-live build's precommit; INV-COMMIT-FAILCLOSED must still hold.
BuildFreeze(bld) ==
    /\ buildLive[bld]
    /\ bld \notin judgedDead
    /\ judgedDead' = judgedDead \cup {bld}
    /\ UNCHANGED << present, tableRefs, owner, precommit, buildLive, committed, aborted, adopted, everDangle, uploaded, closure, everSnapped, staged >>

\* GC fold of the build root: structural, no state change beyond what InDeg helpers already
\* derive from precommit. Kept as a named (stuttering) step so the trace shows the fold point.
GcFold ==
    /\ UNCHANGED vars

\* FIXED ONLY: GC reclaims a precommit whose owning build is judged dead (retire/crash) OR a
\* FALSE-positive on a frozen-but-live build. Removing the edge can drop protection. B199-S2:
\* the reclaim MIRROR-DROPS the recorded inline-closure edges (closure[bld] := {}) -- exactly the
\* edges Precommit seeded -- so the members fall to in-degree 0 and are released by GcDelete. A
\* never-uploaded member is already absent, so its eventual delete is an idempotent no-op.
GcReclaimPrecommit(bld) ==
    /\ UseBuildRoot
    /\ precommit[bld] # None
    /\ precommit[bld] \notin tableRefs            \* not yet committed (else RemovePrecommit)
    /\ (~buildLive[bld] \/ bld \in judgedDead) \* judged dead -- may be a false positive
    /\ precommit' = [precommit EXCEPT ![bld] = None]
    /\ closure'   = [closure   EXCEPT ![bld] = {}] \* mirror-drop the inline-closure edges
    /\ UNCHANGED << present, tableRefs, owner, buildLive, judgedDead, committed, aborted, adopted, everDangle, uploaded, everSnapped, staged >>

\* GC condemns + deletes an in-degree-0, unprotected, present object. B199-S2: GC can only act on
\* an object it has ENUMERATED into the snap (everSnapped) -- an object never recorded in any closure
\* and never table-reachable is invisible debris GC cannot reclaim. (h \in everSnapped) is the
\* enabling guard that distinguishes a reclaimable in-degree-0 object from a leaked one. The guard
\* is the EXPANSION-snap model (UseBuildRoot); the legacy owner-hint model (UseBuildRoot=FALSE) keeps
\* its original enabling condition so the buggy/buildrootonly counterexamples reproduce unchanged.
GcDelete(h) ==
    /\ present[h]
    /\ (UseBuildRoot => h \in everSnapped)
    /\ InDegZero(h)
    /\ ~Protected(h)
    /\ present' = [present EXCEPT ![h] = FALSE]
    /\ UNCHANGED << tableRefs, owner, precommit, buildLive, judgedDead, committed, aborted, adopted, everDangle, uploaded, closure, everSnapped, staged >>

\* ----------------------------------------------------------------- next / spec
Next ==
    \/ \E bld \in Builds, b \in Blobs : WriteBlob(bld, b) \/ AdoptBlob(bld, b)
    \/ \E t \in Trees : PublishTableRef(t) \/ DropTableRef(t)
    \/ \E bld \in Builds, t \in Trees : Precommit(bld, t) \/ Commit(bld, t) \/ CommitAbort(bld, t)
    \/ \E bld \in Builds : RemovePrecommit(bld) \/ BuildDie(bld) \/ BuildFreeze(bld) \/ GcReclaimPrecommit(bld)
    \/ GcFold
    \/ \E h \in Hashes : GcDelete(h)

Spec == Init /\ [][Next]_vars

\* ----------------------------------------------------------------- invariants
TypeOK ==
    /\ present       \in [Hashes -> BOOLEAN]
    /\ tableRefs     \subseteq Trees
    /\ owner         \in [Blobs  -> Builds \cup {None}]
    /\ precommit     \in [Builds -> Trees \cup {None}]
    /\ buildLive     \in [Builds -> BOOLEAN]
    /\ judgedDead \subseteq Builds
    /\ committed     \subseteq Trees
    /\ aborted       \subseteq Builds
    /\ adopted       \in [Builds -> SUBSET Blobs]
    /\ everDangle    \in BOOLEAN
    /\ uploaded      \in [Hashes -> BOOLEAN]
    /\ closure       \in [Builds -> SUBSET Hashes]
    /\ everSnapped   \subseteq Hashes
    /\ staged        \subseteq Hashes

\* INV-NO-DANGLE-COMMITTED (strict, reader-facing): every object in the closure of any
\* TABLE ref is present, and no committed table ref ever dangled.
INV_NO_DANGLE_COMMITTED ==
    /\ \A h \in TableClosure : present[h]
    /\ ~everDangle

\* INV-BUILDROOT-PROTECTS: a present object reachable from a LIVE build's precommit is never
\* condemned by GcDelete. The substantive content is that GcDelete's enabling guard can never
\* fire on such an object: a present, build-root-reachable object is always Protected and
\* never in-degree 0 (the build-root edge itself pins it). If this holds in every reachable
\* state, GcDelete (which requires InDegZero /\ ~Protected /\ present) can never delete it.
INV_BUILDROOT_PROTECTS ==
    UseBuildRoot =>
        \A h \in Hashes :
            (present[h] /\ BuildRootProtected(h)) => (Protected(h) /\ ~InDegZero(h))

\* INV-COMMIT-FAILCLOSED: any committed table ref's closure is present (no commit-creates-
\* dangle), even after a premature reclaim. With FailClosedCommit this is structural.
\* (This is the NO-LOSS guarantee for this model: a committed reader-facing ref never loses
\* a closure member -- the reader never observes a dangle.)
INV_COMMIT_FAILCLOSED ==
    FailClosedCommit =>
        (\A t \in committed : \A h \in Reach(t) : (t \in tableRefs) => present[h])

\* An object has a LEGITIMATE LIVE reference: it is pinned by a published table ref (reader-facing
\* truth) OR by the build-root edges of a STILL-LIVE build (an in-flight build legitimately holding
\* it). Anything else that is present is garbage that GC should reclaim.
OtherLiveRef(h) ==
    \/ PinnedByTable(h)
    \/ (\E bld \in Builds : buildLive[bld] /\ h \in BuildRootEdges(bld))

\* INV-NO-RETURN (safety): a closure member that GC reclaimed (deleted) is never resurrected by a
\* read/GET of the condemned object -- it can only come back as a FRESH re-upload from source. In
\* this model the only way present[h] turns back TRUE is WriteBlob/PublishTableRef/Commit, all of
\* which are fresh writes (WriteBlob requires ~present and re-stamps owner; the trees are written by
\* their own committing build). No action GETs a condemned object to revive it. We assert the
\* structural consequence: a present object's bytes were produced by a fresh upload (uploaded[h]),
\* never conjured from a deleted incarnation. (Honors [[feedback-ca-resurrect-invariant]].)
INV_NO_RETURN ==
    \A h \in Hashes : present[h] => uploaded[h]

\* INV-NO-LEAK (liveness): garbage is eventually reclaimed OR legitimately re-referenced -- it never
\* stays present-and-unreferenced forever. This is the S2 property: for an ABANDONED build (dead, its
\* precommit reclaimed -> closure dropped -> no OtherLiveRef), every recorded closure member with no
\* other live reference is eventually NOT present. With InlineClosure=TRUE the member was snapped at
\* Precommit, so after reclaim it is a visible in-degree-0 object that GcDelete reclaims; the
\* never-uploaded member is already absent (its delete is an idempotent no-op). With InlineClosure
\* =FALSE and a gone tree, the member was NEVER snapped -> GcDelete can never fire on it -> it stays
\* present-and-unreferenced forever (the S2 leak) and this property is VIOLATED.
\* Scope: STAGED members only (objects some precommit actually staged). A blob never staged by any
\* build is outside the precommit-closure model and not what inline-closure governs.
INV_NO_LEAK ==
    \A h \in Hashes :
        (h \in staged /\ present[h] /\ ~OtherLiveRef(h)) ~> (~present[h] \/ OtherLiveRef(h))

\* StateConstraint: the model is finite already, but bound the explorable space explicitly.
StateConstraint == TRUE

\* ----------------------------------------------------------------- fair spec (for INV-NO-LEAK)
\* INV-NO-LEAK is a liveness property, so it needs fairness. GC must eventually act on a reclaimable
\* object and the abandoning build must eventually be reclaimed -- otherwise any safety-only Spec can
\* "leak" simply by stuttering. We put WEAK FAIRNESS on the GC reclaim/delete actions and on the
\* teardown that abandons a build (BuildDie + GcReclaimPrecommit). The WRITER side gets NO fairness
\* (a build may abandon at any point; we do not force it to commit). This isolates "does GC drain the
\* garbage of an abandoned build" from "does the build make progress".
FairSpec ==
    /\ Init
    /\ [][Next]_vars
    /\ \A bld \in Builds : WF_vars(BuildDie(bld))
    /\ \A bld \in Builds : WF_vars(GcReclaimPrecommit(bld))
    /\ \A h \in Hashes   : WF_vars(GcDelete(h))

\* ----------------------------------------------------------------- reachability WITNESSES
\* These are NOT safety invariants -- they are NEGATED reachability probes. Each asserts that a
\* dangerous configuration is UNreachable; TLC reports a "violation" exactly when the dangerous
\* interleaving IS reachable. Used to prove the fixed config is NON-VACUOUS (it really exercises
\* and survives the dangerous interleavings, rather than being green because it never gets there).
\* Run ad hoc with witness.cfg; not part of the committed buggy/fixed cfgs.

\* W1: the premature-reclaim interleaving is reachable AND the fail-closed save fires -- a
\* still-RUNNING build was frozen, its precommit reclaimed, a blob it adopted was then deleted,
\* and its real commit ABORTED (never published the dangle). buildLive distinguishes this from
\* a genuine crash; the build is alive and would have published without the fail-closed gate.
W_PrematureReclaimAbortReached ==
    ~(\E bld \in Builds :
        /\ buildLive[bld]
        /\ bld \in judgedDead
        /\ bld \in aborted
        /\ precommit[bld] = None
        /\ \E b \in adopted[bld] : ~present[b])

\* W2: a GcDelete actually happened in the fixed model (some blob was adopted yet is absent and
\* unpinned) -- proves GcDelete is enabled, i.e. the model is not frozen before deletion.
W_GcDeleteReached ==
    ~(\E b \in Blobs : ~present[b] /\ (\E bld \in Builds : b \in adopted[bld]))

\* W3: the build-root actually protected a present blob from an enabled GcDelete -- a present,
\* in-degree-0-but-for-the-precommit blob that only the build root pins.
W_BuildRootProtectReached ==
    ~(\E b \in Blobs :
        /\ present[b]
        /\ ~PinnedByTable(b)
        /\ PinnedByBuildRoot(b))

\* W4: the premature-reclaim-of-a-LIVE-build interleaving (§4.6) is reached: a still-running
\* build (buildLive) was frozen (judgedDead), its precommit reclaimed (precommit=None), and a
\* blob it adopted was subsequently deleted -- the exact input INV-COMMIT-FAILCLOSED must absorb.
W_LiveFrozenReclaimDeleteReached ==
    ~(\E bld \in Builds :
        /\ buildLive[bld]
        /\ bld \in judgedDead
        /\ precommit[bld] = None
        /\ \E b \in adopted[bld] : ~present[b])

\* W5 (b2 config only -- requires Blobs={b1,b2}, TreeBlobs=(t1:>{b1} @@ t2:>{b1,b2})): the
\* SHARED-vs-UNIQUE outcome is reached. A blob b is "shared" if it is in a committed table ref's
\* closure; the abandoned precommit also referenced it. After abandonment + reclaim, the SHARED blob
\* (b1, pinned by committed t1) stays PRESENT, while the abandoned build's UNIQUE blob (b2, referenced
\* ONLY by the reclaimed precommit t2) has been RECLAIMED. NEGATED probe: TLC reports it violated
\* exactly when this exact "shared spared AND unique reclaimed" state is reachable.
W_SharedSparedUniqueReclaimed ==
    ~(\E sh \in Blobs, uq \in Blobs :
        /\ sh # uq
        /\ PinnedByTable(sh)            \* shared blob: held by a committed table ref
        /\ present[sh]                  \*   -> spared
        /\ ~PinnedByTable(uq)           \* unique blob: NOT held by any committed ref
        /\ uq \in staged                \*   -> it was a staged member of some (now abandoned) precommit
        /\ ~present[uq]                 \*   -> reclaimed
        /\ (\A bld \in Builds : ~buildLive[bld]))  \* all builds abandoned
=================================================================================
