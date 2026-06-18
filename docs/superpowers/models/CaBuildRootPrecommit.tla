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
*)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Builds,             \* in-flight builds, e.g. {bld1, bld2}
    Trees,              \* tree hashes, e.g. {t1, t2}
    Blobs,              \* blob hashes, e.g. {b1}
    UseBuildRoot,       \* FALSE = buggy (protection = revocable owner hint, no build root)
                        \* TRUE  = fix  (precommit edge protects reachable present objects)
    FailClosedCommit    \* FALSE = buggy (commit publishes table ref with no presence check)
                        \* TRUE  = fix  (commit verifies full closure present, else aborts)

ASSUME Trees \cap Blobs = {}
Hashes == Trees \cup Blobs
None    == "none"

\* Both trees reference the single shared blob (dedup); this is the load-bearing topology.
Children(t) == Blobs
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
    everDangle      \* BOOLEAN                         latch: a committed table ref ever dangled

vars == << present, tableRefs, owner, precommit, buildLive, judgedDead,
           committed, aborted, adopted, everDangle >>

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

\* FIXED: protected iff reachable from a LIVE build's precommit (build-root edge).
BuildRootProtected(h) ==
    \E bld \in Builds :
        /\ buildLive[bld]
        /\ precommit[bld] # None
        /\ h \in Reach(precommit[bld])

Protected(h) ==
    IF UseBuildRoot THEN BuildRootProtected(h) ELSE OwnerProtected(h)

\* In-degree from durable references that actually pin (table refs always; build-root
\* precommit edges pin too in the fixed model, via the GC fold).
PinnedByTable(h)     == \E t \in tableRefs : h \in Reach(t)
PinnedByBuildRoot(h) == UseBuildRoot /\ (\E bld \in Builds : precommit[bld] # None /\ h \in Reach(precommit[bld]))
InDegZero(h)         == ~PinnedByTable(h) /\ ~PinnedByBuildRoot(h)

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

\* ----------------------------------------------------------------- actions

\* A build writes a fresh blob to the pool. Stamps owner = this build (the cas_owner hint).
WriteBlob(bld, b) ==
    /\ buildLive[bld]
    /\ ~present[b]
    /\ present'  = [present EXCEPT ![b] = TRUE]
    /\ owner'    = [owner   EXCEPT ![b] = bld]
    /\ adopted'  = [adopted EXCEPT ![bld] = @ \cup {b}]   \* writer trivially "holds" its blob
    /\ UNCHANGED << tableRefs, precommit, buildLive, judgedDead, committed, aborted, everDangle >>

\* A build ADOPTS an already-present blob (dedup): no bytes moved, owner UNCHANGED.
\* It records the blob in its dep set (adopted[bld]). In the buggy model this confers NO
\* protection (owner still points at the original writer, structural flaw #1). A build cannot
\* adopt an absent blob (it must be present at adopt time -- the soak's adopt-the-live-
\* incarnation step). The blob may later be deleted out from under the adopter.
AdoptBlob(bld, b) ==
    /\ buildLive[bld]
    /\ present[b]
    /\ adopted' = [adopted EXCEPT ![bld] = @ \cup {b}]
    /\ UNCHANGED << present, tableRefs, owner, precommit, buildLive, judgedDead, committed, aborted, everDangle >>

\* A build publishes a TABLE ref directly (the pre-existing independent pin, e.g. t1 over b1).
\* Requires the closure present. Models content already committed by some prior, now-retired
\* path; this is the "independent reference" whose drop can take in-degree to 0.
PublishTableRef(t) ==
    /\ \A c \in Children(t) : present[c]
    /\ t \notin tableRefs
    /\ present'   = [present EXCEPT ![t] = TRUE]
    /\ tableRefs' = tableRefs \cup {t}
    /\ committed' = committed \cup {t}
    /\ UNCHANGED << owner, precommit, buildLive, judgedDead, aborted, adopted, everDangle >>

\* FIXED ONLY: a build publishes its precommit (build-root ref -> tree). Tiny write of hashes;
\* may be published before the bytes land (build root tolerates absent objects).
Precommit(bld, t) ==
    /\ UseBuildRoot
    /\ buildLive[bld]
    /\ precommit[bld] = None
    /\ precommit' = [precommit EXCEPT ![bld] = t]
    /\ UNCHANGED << present, tableRefs, owner, buildLive, judgedDead, committed, aborted, adopted, everDangle >>

\* An INDEPENDENT table ref is dropped (its in-degree contribution disappears). This is the
\* DropTableRef that can take the shared blob to in-degree 0 while another build still wants it.
DropTableRef(t) ==
    /\ t \in tableRefs
    /\ tableRefs' = tableRefs \ {t}
    /\ UNCHANGED << present, owner, precommit, buildLive, judgedDead, committed, aborted, adopted, everDangle >>

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
    /\ present'   = [present EXCEPT ![t] = TRUE] \* write the manifest tree
    /\ tableRefs' = tableRefs \cup {t}
    /\ committed' = committed \cup {t}
    /\ everDangle' = everDangle \/ (\E c \in Children(t) : ~present[c])
    /\ UNCHANGED << owner, precommit, buildLive, judgedDead, aborted, adopted >>

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
    /\ UNCHANGED << present, tableRefs, owner, precommit, buildLive, judgedDead, committed, adopted, everDangle >>

\* FIXED ONLY: remove a precommit after a successful commit (normal happy-path teardown).
RemovePrecommit(bld) ==
    /\ UseBuildRoot
    /\ precommit[bld] # None
    /\ precommit[bld] \in tableRefs               \* its tree is now a real table ref
    /\ precommit' = [precommit EXCEPT ![bld] = None]
    /\ UNCHANGED << present, tableRefs, owner, buildLive, judgedDead, committed, aborted, adopted, everDangle >>

\* A build truly dies (clean retire or hard crash): it stops acting AND GC judges it dead.
\* In the buggy model this is what makes a blob's owner-protection lapse (min_active rises).
BuildDie(bld) ==
    /\ buildLive[bld]
    /\ buildLive'  = [buildLive EXCEPT ![bld] = FALSE]
    /\ judgedDead' = judgedDead \cup {bld}        \* seq < min_active / epoch mismatch
    /\ UNCHANGED << present, tableRefs, owner, precommit, committed, aborted, adopted, everDangle >>

\* FALSE-POSITIVE freeze (§4.6 residual fragility): the build's background heartbeat renewer
\* stalls (e.g. an S3 retry storm), so GC JUDGES it dead -- but it is still RUNNING (buildLive
\* stays TRUE) and will later commit. This is the input that lets GcReclaimPrecommit fire on a
\* still-live build's precommit; INV-COMMIT-FAILCLOSED must still hold.
BuildFreeze(bld) ==
    /\ buildLive[bld]
    /\ bld \notin judgedDead
    /\ judgedDead' = judgedDead \cup {bld}
    /\ UNCHANGED << present, tableRefs, owner, precommit, buildLive, committed, aborted, adopted, everDangle >>

\* GC fold of the build root: structural, no state change beyond what InDeg helpers already
\* derive from precommit. Kept as a named (stuttering) step so the trace shows the fold point.
GcFold ==
    /\ UNCHANGED vars

\* FIXED ONLY: GC reclaims a precommit whose owning build is judged dead (retire/crash) OR a
\* FALSE-positive on a frozen-but-live build. Removing the edge can drop protection.
GcReclaimPrecommit(bld) ==
    /\ UseBuildRoot
    /\ precommit[bld] # None
    /\ precommit[bld] \notin tableRefs            \* not yet committed (else RemovePrecommit)
    /\ (~buildLive[bld] \/ bld \in judgedDead) \* judged dead -- may be a false positive
    /\ precommit' = [precommit EXCEPT ![bld] = None]
    /\ UNCHANGED << present, tableRefs, owner, buildLive, judgedDead, committed, aborted, adopted, everDangle >>

\* GC condemns + deletes an in-degree-0, unprotected, present object.
GcDelete(h) ==
    /\ present[h]
    /\ InDegZero(h)
    /\ ~Protected(h)
    /\ present' = [present EXCEPT ![h] = FALSE]
    /\ UNCHANGED << tableRefs, owner, precommit, buildLive, judgedDead, committed, aborted, adopted, everDangle >>

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
INV_COMMIT_FAILCLOSED ==
    FailClosedCommit =>
        (\A t \in committed : \A h \in Reach(t) : (t \in tableRefs) => present[h])

\* StateConstraint: the model is finite already, but bound the explorable space explicitly.
StateConstraint == TRUE

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
=================================================================================
