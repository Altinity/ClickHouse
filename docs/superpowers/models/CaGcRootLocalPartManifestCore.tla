-------------------- MODULE CaGcRootLocalPartManifestCore --------------------
(* Root-local part-manifest GC core — spec: 2026-06-26-cas-gc-streaming-sharded-redesign-design.md.
   Branch of CaIncarnationCore.tla: blobs keep incarnation tokens + exact-token delete + fence/recheck;
   trees are REPLACED by unique single-owner ManifestIds with owner transitions and blob-only in-degree.
   Sabotage* flags each break exactly one load-bearing rule and MUST yield a counterexample. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Namespaces, Writers, Leaders, Blobs, ManifestInstances, Refs, Builds, Paths,
    BuildPrefixes,                                    \* sweep-eligibility grouping (writer_instance_id/build_sequence locator)
    MaxToken, MaxRound, MaxLog,
    EnablePrecommit, EnableMissingBody, EnableOrphanSweep, EnableMutablePayload,
    \* one per negative control (Task 8-10); all FALSE in positive stages:
    SabotageReuseManifestId, SabotageTwoOwners, SabotageSplitPromote,
    SabotageMissingBodyActivated, SabotageCommitSkipBlobReval, SabotagePrecommitlessProtect,
    SabotageNoOrphanSweep, SabotageWholesalePrefixDelete, SabotageFrozenSeqAuthority,
    SabotageMissingCommittedEmpty, SabotageDeleteBodyBeforeDecrements, SabotageCutOverclaim,
    SabotageRoundVisibilityEarly, SabotageNoFence, SabotageTrimUnincorporated,
    SabotageUncondDelete, SabotageReusedTag, SabotageBareNonce, SabotageKeyByRefNotId,
    SabotageAcceptNamespaceMismatch, SabotageAcceptRefMismatch,
    SabotageMutableAsReachability, SabotagePromoteAfterMissingBody,
    SabotageAdvancePastMissingBodyPrecommit

\* A ManifestId is (namespace, manifest_instance_id). manifest_instance_id is drawn from
\* ManifestInstances and is NEVER reused once visible (NoManifestIdReuse). Two namespaces may
\* hold the same instance id; they are DIFFERENT ManifestIds (the SabotageKeyByRefNotId hazard).
ManifestIds == Namespaces \X ManifestInstances
\* ---- abstraction note: two identities ----
\* The model's SAFETY identity is ManifestSafetyId = (namespace, manifest_instance_id) = the
\* ManifestIds tuple above; it is what owner/blobEdges/cleanup key on and what every safety
\* invariant ranges over. The PROTOCOL identity (on the wire) is ManifestId = (root_namespace,
\* ManifestRef); writer_instance_id / build_sequence are a LOCATOR + sweep-eligibility grouping,
\* modeled here as mPrefix \in [ManifestIds -> BuildPrefixes] with per-prefix sweepEligible. The
\* model collapses the protocol ManifestId onto ManifestSafetyId (single visible binding per id),
\* so "ManifestId" below names the safety identity unless the prose says protocol identity.
ManifestSafetyId == ManifestIds   \* alias: the safety identity the invariants range over
Toks == 1..MaxToken
None == "none"
NoBlob == "noblob"   \* per-path sentinel: that path has no blob entry (inline / absent)
\* Source edges are keyed by (ManifestId, path), NOT by a SUBSET Blobs. A manifest referencing the
\* same blob at two paths therefore contributes in-degree 2 — multiplicity that SUBSET Blobs cannot
\* express and that controls #18 (KeyByRefNotId), #21 (MutableAsReachability), and
\* MutablePayloadNotReachability require. mEntries[m] and mActiveEdges[m] are [Paths -> Blobs \cup {NoBlob}].
BlobsOf(g) == { g[p] : p \in {q \in Paths : g[q] \in Blobs} }   \* set of blobs a per-path map references

VARIABLES
    present, tokOf, nextTok, deadTok,         \* blob objects (as in CaIncarnationCore)
    mBody, mEntries, mRef, mNs,               \* manifest body: present?, PER-PATH blob refs [Paths->Blobs∪{NoBlob}], self-ref, self-ns
    owner, mActiveEdges,                       \* structural owner; per-path edges actually emitted at activation [Paths->Blobs∪{NoBlob}]
    journal,                                   \* [Namespaces -> Seq(OwnerTransition)]
    blobIndeg, blobEdges, everEdged,           \* folded blob in-degree; blobEdges ⊆ ManifestIds×Paths (folded active source edges); journal-known
    foldSeal,                                  \* per-(ns,shard) FOLD coverage: classification/folded_token/folded_cursor (SabotageCutOverclaim defense lives here)
    completionSeal,                            \* per-round COMPLETION fence: fence positions, recheck, delete outcomes, trim, adoptable
    gcRound, gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion,
    retired, inflight, wView,                  \* GC pipeline (carried from core)
    mfCleanup, mfDeleted, mPrefix, sweepEligible   \* part-manifest cleanup work; deleted-bodies set; per-manifest build-prefix; per-prefix orphan-sweep eligibility

\* SrcEdges(m): the active source edges currently emitted by m's activation map. Derived operator,
\* NOT a constant, so it follows mActiveEdges as the fold mutates it.
SrcEdges(m) == { <<m, p>> : p \in {q \in Paths : mActiveEdges[m][q] \in Blobs} }
\* A RootOwnerEvent (journal record) names an OLD manifest binding and a NEW one for one ref.
\* Owner-move dispatch (rev.15) compares the two: SAME ManifestId on both sides => pure owner
\* move (precommit->committed), NO blob deltas, NO mfCleanup. Different ids => a removal of old
\* (-1 + mfCleanup) and/or an activation of new (+1). Helper predicates over a journal record e:
OwnerMoveSameManifest(e) == e.old \in ManifestIds /\ e.new \in ManifestIds /\ e.old = e.new
IsRemoval(e)             == e.old \in ManifestIds /\ (e.new \notin ManifestIds \/ e.new # e.old)
IsActivation(e)          == e.new \in ManifestIds /\ (e.old \notin ManifestIds \/ e.old # e.new)

vars == << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
           journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
           roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
           mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* ---- helpers (filled across tasks) ----
NoOp == UNCHANGED vars

\* A blob token stops being current when displaced or deleted (INV_NO_RETURN oracle).
CondemnedTok(b, t) == t \in deadTok[b]

\* RefMatchesBody / ManifestNamespaceMatches: the body self-describes its ref + ns; a sabotage may
\* publish a manifest whose body disagrees. A read/fold that accepts a mismatch is unsafe.
BodyValid(m) == mRef[m] = m /\ mNs[m] = m[1]

\* WStageManifest: write a part-manifest body BEFORE any owner transition (the pre-precommit object).
\* everEdged tracks instance ids that have ever been bound to a body lineage; NoManifestIdReuse
\* forbids re-binding a visible instance id to a new body. SabotageReuseManifestId drops the
\* freshness guard (reuse a ManifestId for a byte-identical future manifest). SabotageAcceptRefMismatch
\* / SabotageAcceptNamespaceMismatch write a body whose self-ref / self-ns disagree with the id.
WStageManifest(m, f) ==
    /\ owner[m] = None /\ ~mBody[m]
    /\ (m[2] \notin everEdged \/ SabotageReuseManifestId)   \* fresh instance id (never-reused) unless sabotaged
    /\ mBody' = [mBody EXCEPT ![m] = TRUE]
    /\ mEntries' = [mEntries EXCEPT ![m] = f]
    /\ mRef' = [mRef EXCEPT ![m] = IF SabotageAcceptRefMismatch THEN (CHOOSE x \in ManifestIds : x # m) ELSE m]
    /\ mNs' = [mNs EXCEPT ![m] = IF SabotageAcceptNamespaceMismatch THEN (CHOOSE n \in Namespaces : n # m[1]) ELSE m[1]]
    /\ everEdged' = everEdged \cup {m[2]}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, owner, mActiveEdges, journal, blobIndeg,
                    blobEdges, foldSeal, completionSeal, gcRound, gcPhase, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible >>

\* ---- owner transitions (spec §Build And Precommit Protocol; §Fold Owner Transitions) ----
PresentBlobs == { b \in Blobs : present[b] }
\* A committed owner's fail-closed gate: body present+valid AND every named blob present (and not
\* condemned). SabotageCommitSkipBlobReval drops the blob revalidation; SabotageMissingCommittedEmpty
\* belongs to the FOLD path (a missing committed body treated as empty), not here.
CommitGate(m) ==
    \/ SabotageCommitSkipBlobReval
    \/ ( mBody[m] /\ BodyValid(m)
         /\ \A b \in BlobsOf(mEntries[m]) : present[b] /\ ~CondemnedTok(b, tokOf[b]) )
\* Append one OwnerTransition [ver, ref, old, new] to journal[ns]; version = new length (monotone).
AppendEvt(j, ns, rf, o, nw) ==
    [j EXCEPT ![ns] = Append(@, [ver |-> Len(@) + 1, ref |-> rf, old |-> o, new |-> nw])]
\* A committed ref names exactly one manifest (SingleManifestOwner): a ref may newly own m only when it
\* owns nothing else, except under SabotageTwoOwners (the sharing hazard #2). A repoint moves a ref off
\* its current manifest atomically, so the ref is considered free of everything other than mFrom.
RefFreeFor(ref, mFrom) ==
    SabotageTwoOwners \/ (\A x \in ManifestIds : owner[x] = ref => x = mFrom)

\* A blob upload: mint a fresh token (never the condemned one unless SabotageReusedTag); present.
WUploadBlob(b) ==
    /\ nextTok[b] <= MaxToken
    /\ LET newt == IF SabotageReusedTag /\ deadTok[b] # {} THEN (CHOOSE t \in deadTok[b] : TRUE) ELSE nextTok[b] IN
       /\ present' = [present EXCEPT ![b] = TRUE]
       /\ tokOf' = [tokOf EXCEPT ![b] = newt]
       /\ nextTok' = [nextTok EXCEPT ![b] = IF SabotageReusedTag /\ deadTok[b] # {} THEN @ ELSE @ + 1]
    /\ UNCHANGED << deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges, journal, blobIndeg,
                    blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible >>

\* PrecommitAdd: owner[m] := build bld. A precommit MAY have a missing body only when EnableMissingBody
\* (the missing-body fail-closed intent). A precommit ACTIVATES (emits edges) iff its body is present
\* and valid; SabotageMissingBodyActivated forces edges even with the body absent. The journal records
\* an activation event old=None,new=m for ref=bld.
WPrecommitAdd(m, bld) ==
    /\ EnablePrecommit
    /\ owner[m] = None
    /\ (mBody[m] \/ EnableMissingBody)                  \* missing body only when allowed
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = bld]
    /\ mActiveEdges' = [mActiveEdges EXCEPT ![m] =
            IF SabotageMissingBodyActivated \/ (mBody[m] /\ BodyValid(m))
            THEN mEntries[m] ELSE [p \in Paths |-> NoBlob]]
    /\ journal' = AppendEvt(journal, m[1], bld, None, m)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, blobIndeg,
                    blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible >>

\* Promote precommit -> committed: the atomic PURE owner move. Same ManifestId, blob Δ=0, NO new
\* edges (the activation +1 was emitted when the precommit's body arrived, or is still pending if the
\* precommit is non-activated -- in which case promote must FAIL CLOSED). Single transition
\* owner[m]: bld -> ref, journal old=m,new=m (a promotion event). Fail-closed gate ON ACTIVATION.
\* SabotagePromoteAfterMissingBody emits +edges after a missing-body precommit (treats the move as an
\* activation that adds reachability never folded). SabotageSplitPromote splits the move into
\* remove-then-add with an interleaving gap (here: drops the owner first, leaving a window).
\* Split-promote (#3): the move becomes two CAS with a gap and NO fail-closed retry on the add. The
\* gap lets the precommit be reclaimed / a blob lapse; the non-fail-closed add then publishes a
\* committed owner WITHOUT revalidating the blob bodies -> a committed ref over an absent blob.
WPromote(m, bld, ref) ==
    /\ EnablePrecommit
    /\ owner[m] = bld
    /\ RefFreeFor(ref, m)
    \* split-promote and promote-after-missing-body both drop the atomic fail-closed activation gate:
    /\ (SabotageSplitPromote \/ SabotagePromoteAfterMissingBody \/ CommitGate(m))
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = ref]
    /\ journal' = AppendEvt(journal, m[1], ref, m, m)
    /\ mActiveEdges' = IF SabotagePromoteAfterMissingBody \/ SabotageSplitPromote
                       THEN [mActiveEdges EXCEPT ![m] = mEntries[m]]  \* re-emit / publish edges unrevalidated
                       ELSE mActiveEdges
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, blobIndeg,
                    blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible >>

\* Direct committed publish (no precommit). Same fail-closed body+blob gate as promote. Sets the
\* committed owner and activates the edges (committed manifests are always activated).
WPublishCommitted(m, ref) ==
    /\ owner[m] = None
    /\ RefFreeFor(ref, m)
    /\ CommitGate(m)
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = ref]
    /\ mActiveEdges' = [mActiveEdges EXCEPT ![m] = mEntries[m]]   \* committed publishes the manifest's edges
    /\ journal' = AppendEvt(journal, m[1], ref, None, m)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, blobIndeg,
                    blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible >>

\* Drop a committed ref: owner[m]: ref -> None; a true removal (-1 + mfCleanup queued at fold).
WDropRef(m) ==
    /\ owner[m] \in Refs
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = None]
    /\ journal' = AppendEvt(journal, m[1], owner[m], m, None)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Abandon a precommit owner: owner[m]: bld -> None; a true removal.
WAbandonPrecommit(m) ==
    /\ owner[m] \in Builds
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = None]
    /\ journal' = AppendEvt(journal, m[1], owner[m], m, None)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Repoint a committed ref from mOld to mNew (last-op-wins at the root source): removes mOld and
\* activates mNew under the SAME ref, in one event (old=mOld, new=mNew). Both must be in the same ns.
WRepoint(mOld, mNew, ref) ==
    /\ mOld # mNew /\ mOld[1] = mNew[1]
    /\ owner[mOld] = ref /\ ref \in Refs /\ owner[mNew] = None
    /\ RefFreeFor(ref, mOld)
    /\ CommitGate(mNew)
    /\ Len(journal[mOld[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![mOld] = None, ![mNew] = ref]
    /\ mActiveEdges' = [mActiveEdges EXCEPT ![mNew] = mEntries[mNew]]
    /\ journal' = AppendEvt(journal, mOld[1], ref, mOld, mNew)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, blobIndeg,
                    blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible >>

Init ==
    /\ present = [b \in Blobs |-> FALSE]
    /\ tokOf = [b \in Blobs |-> 0]
    /\ nextTok = [b \in Blobs |-> 1]
    /\ deadTok = [b \in Blobs |-> {}]
    /\ mBody = [m \in ManifestIds |-> FALSE]
    /\ mEntries = [m \in ManifestIds |-> [p \in Paths |-> NoBlob]]
    /\ mRef = [m \in ManifestIds |-> m]      \* body self-ref equals id until sabotaged
    /\ mNs = [m \in ManifestIds |-> m[1]]    \* body self-ns equals owning ns until sabotaged
    /\ owner = [m \in ManifestIds |-> None]
    /\ mActiveEdges = [m \in ManifestIds |-> [p \in Paths |-> NoBlob]]
    /\ journal = [n \in Namespaces |-> << >>]
    /\ blobIndeg = [b \in Blobs |-> 0]
    /\ blobEdges = {}
    /\ everEdged = {}
    \* foldSeal: per-round fold classification + folded_cursor (the SabotageCutOverclaim defense — a
    \* cut may not outrun the deltas it claims to cover):
    /\ foldSeal = [r \in 0..MaxRound |-> [classified |-> {}, foldedCursor |-> [n \in Namespaces |-> 0]]]
    \* completionSeal: per-round fence positions, recheck status, delete outcomes, trim base, adoptable:
    /\ completionSeal = [r \in 0..MaxRound |-> [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]]
    /\ gcRound = 0
    /\ gcPhase = [l \in Leaders |-> "idle"]
    /\ roundOf = [l \in Leaders |-> 0]
    /\ fencePos = [n \in Namespaces |-> 0]
    /\ cursor = [n \in Namespaces |-> 0]
    /\ trimBase = [n \in Namespaces |-> 0]
    /\ fenceVersion = [r \in 0..MaxRound |-> [n \in Namespaces |-> 0]]
    /\ retired = {}
    /\ inflight = {}
    /\ wView = [w \in Writers |-> 0]
    /\ mfCleanup = {}
    /\ mfDeleted = {}
    /\ mPrefix = [m \in ManifestIds |-> CHOOSE p \in BuildPrefixes : TRUE]   \* each manifest belongs to one build-prefix
    /\ sweepEligible = [p \in BuildPrefixes |-> FALSE]   \* per-prefix eligibility from a durable watermark fact

TypeOK ==
    /\ present \in [Blobs -> BOOLEAN]
    /\ tokOf \in [Blobs -> 0..MaxToken]
    /\ owner \in [ManifestIds -> Refs \cup Builds \cup {None}]
    /\ mBody \in [ManifestIds -> BOOLEAN]
    /\ mEntries \in [ManifestIds -> [Paths -> Blobs \cup {NoBlob}]]
    /\ blobEdges \in SUBSET (ManifestIds \X Paths)
    /\ blobIndeg \in [Blobs -> 0..(Cardinality(ManifestIds) * Cardinality(Paths))]
    /\ cursor \in [Namespaces -> 0..MaxLog]
    /\ trimBase \in [Namespaces -> 0..MaxLog]
    /\ foldSeal \in [0..MaxRound -> [classified : SUBSET ManifestIds, foldedCursor : [Namespaces -> 0..MaxLog]]]
    /\ completionSeal \in [0..MaxRound -> [fenced : SUBSET Namespaces, rechecked : SUBSET Blobs, deleted : SUBSET Blobs, adoptable : BOOLEAN]]
    /\ mPrefix \in [ManifestIds -> BuildPrefixes]
    /\ sweepEligible \in [BuildPrefixes -> BOOLEAN]

INV_JOURNAL_COVERAGE == \A n \in Namespaces : trimBase[n] <= cursor[n]

\* once a body is staged for an instance id, no DIFFERENT body lineage rebinds it (self-ref stays = id):
NoManifestIdReuse ==
    \A m \in ManifestIds : mBody[m] => (mRef[m] = m \/ SabotageAcceptRefMismatch)
RefMatchesBody == \A m \in ManifestIds : (mBody[m] /\ owner[m] # None) => mRef[m] = m
ManifestNamespaceMatches == \A m \in ManifestIds : (mBody[m] /\ owner[m] # None) => mNs[m] = m[1]
INV_NO_RETURN == \A b \in Blobs : present[b] => tokOf[b] \notin deadTok[b]

\* ---- ownership / dangle invariants (spec §Safety Invariants) ----
SingleManifestOwner ==
    SabotageTwoOwners \/ (\A m1, m2 \in ManifestIds :
        (owner[m1] # None /\ owner[m1] = owner[m2] /\ m1 # m2) => owner[m1] \in Builds)
CommittedManifestBodyRequired ==
    \A m \in ManifestIds : (owner[m] \in Refs) => (mBody[m] /\ BodyValid(m))
PrecommitMayReferenceMissingManifest == TRUE   \* witnessed reachable, not an invariant to hold
\* Every blob actually emitted as an active edge of a committed manifest is present and live.
CommittedNoMissingBlob ==
    \A m \in ManifestIds : (owner[m] \in Refs) =>
        (\A b \in BlobsOf(mActiveEdges[m]) : present[b] /\ ~CondemnedTok(b, tokOf[b]))
NoCommittedDangle ==
    \A m \in ManifestIds : (owner[m] \in Refs) => (mBody[m] /\ \A b \in BlobsOf(mEntries[m]) : present[b])
INV_NO_DANGLE == NoCommittedDangle
ReachableBlobs == UNION { BlobsOf(mEntries[m]) : m \in {x \in ManifestIds : owner[x] \in Refs /\ mBody[x]} }
INV_NO_LOSS == \A b \in ReachableBlobs : present[b]

StateConstraint ==
    /\ \A n \in Namespaces : Len(journal[n]) <= MaxLog
    /\ Cardinality(inflight) <= 2

Next ==
    \/ \E m \in ManifestIds, f \in [Paths -> Blobs \cup {NoBlob}] : WStageManifest(m, f)
    \/ \E b \in Blobs : WUploadBlob(b)
    \/ \E m \in ManifestIds, bld \in Builds : WPrecommitAdd(m, bld)
    \/ \E m \in ManifestIds, bld \in Builds, ref \in Refs : WPromote(m, bld, ref)
    \/ \E m \in ManifestIds, ref \in Refs : WPublishCommitted(m, ref)
    \/ \E m \in ManifestIds : WDropRef(m) \/ WAbandonPrecommit(m)
    \/ \E mOld, mNew \in ManifestIds, ref \in Refs : WRepoint(mOld, mNew, ref)

Spec == Init /\ [][Next]_vars
=============================================================================
