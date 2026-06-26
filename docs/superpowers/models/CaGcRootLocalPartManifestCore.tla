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
\* A RootOwnerEvent (journal record) names an OLD manifest binding and a NEW one for one ref. To keep
\* the journal UNIFORMLY TYPED for TLC fingerprinting (string-vs-tuple equality is rejected), the
\* old/new bindings are SUBSETs of ManifestIds: {} = no binding, a singleton = that ManifestId.
\* Owner-move dispatch (rev.15) compares the two: SAME ManifestId on both sides => pure owner
\* move (precommit->committed), NO blob deltas, NO mfCleanup. Different ids => a removal of old
\* (-1 + mfCleanup) and/or an activation of new (+1). Helper predicates over a journal record e:
Bind(m) == {m}                 \* a binding to manifest m
NoBind  == {}                  \* no binding
TheM(s) == CHOOSE m \in s : TRUE   \* extract the single ManifestId from a singleton binding
OwnerMoveSameManifest(e) == e.old # {} /\ e.old = e.new
IsRemoval(e)             == e.old # {} /\ e.old # e.new
IsActivation(e)          == e.new # {} /\ e.old # e.new

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
\* A blob's CURRENT token is condemned in the writer's publish view if GC has retired it or has a
\* delete in flight for that exact token (the W-PUBLISH-GATE / retire-view: the global fence guarantees
\* the writer sees the round's retired set before publishing). Committing over such a blob would race a
\* GC delete and dangle, so the fail-closed gate must reject it (the writer must re-upload from source).
BlobCondemnedInView(b) ==
    \/ CondemnedTok(b, tokOf[b])
    \/ \E e \in retired  : e.b = b /\ e.t = tokOf[b]
    \/ \E d \in inflight : d.b = b /\ d.t = tokOf[b]
\* A committed owner's fail-closed gate: body present+valid AND every named blob present and not
\* condemned-in-view. SabotageCommitSkipBlobReval drops the blob revalidation; SabotageMissingCommittedEmpty
\* belongs to the FOLD path (a missing committed body treated as empty), not here.
CommitGate(m) ==
    \/ SabotageCommitSkipBlobReval
    \/ ( mBody[m] /\ BodyValid(m)
         /\ \A b \in BlobsOf(mEntries[m]) : present[b] /\ ~BlobCondemnedInView(b) )
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
    /\ (m \notin mfDeleted \/ SabotageReuseManifestId)   \* a swept/deleted id is retired forever
    /\ (mBody[m] \/ EnableMissingBody)                  \* missing body only when allowed
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = bld]
    /\ journal' = AppendEvt(journal, m[1], bld, NoBind, Bind(m))
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

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
    /\ journal' = AppendEvt(journal, m[1], ref, Bind(m), Bind(m))
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Direct committed publish (no precommit). Same fail-closed body+blob gate as promote. Sets the
\* committed owner and activates the edges (committed manifests are always activated).
WPublishCommitted(m, ref) ==
    /\ owner[m] = None
    /\ (m \notin mfDeleted \/ SabotageReuseManifestId)
    /\ RefFreeFor(ref, m)
    /\ CommitGate(m)
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = ref]
    /\ journal' = AppendEvt(journal, m[1], ref, NoBind, Bind(m))
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Drop a committed ref: owner[m]: ref -> None; a true removal (-1 + mfCleanup queued at fold).
WDropRef(m) ==
    /\ owner[m] \in Refs
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = None]
    /\ journal' = AppendEvt(journal, m[1], owner[m], Bind(m), NoBind)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Abandon a precommit owner: owner[m]: bld -> None; a true removal.
WAbandonPrecommit(m) ==
    /\ owner[m] \in Builds
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = None]
    /\ journal' = AppendEvt(journal, m[1], owner[m], Bind(m), NoBind)
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* A missing-body precommit's manifest body ARRIVES asynchronously (the upload completes after
\* PrecommitAdd). Only meaningful under EnableMissingBody. Sets the body + entries; if the precommit is
\* still build-owned, it ACTIVATES and its edges become emittable at the next fold of the parked event.
WManifestBodyArrives(m, f) ==
    /\ EnableMissingBody
    /\ ~mBody[m] /\ owner[m] \in Builds
    /\ mBody' = [mBody EXCEPT ![m] = TRUE]
    /\ mEntries' = [mEntries EXCEPT ![m] = f]
    /\ mRef' = [mRef EXCEPT ![m] = m]
    /\ mNs' = [mNs EXCEPT ![m] = m[1]]
    /\ everEdged' = everEdged \cup {m[2]}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, owner, mActiveEdges, journal, blobIndeg,
                    blobEdges, foldSeal, completionSeal, gcRound, gcPhase, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, mfDeleted,
                    mPrefix, sweepEligible >>

\* Repoint a committed ref from mOld to mNew (last-op-wins at the root source): removes mOld and
\* activates mNew under the SAME ref, in one event (old=mOld, new=mNew). Both must be in the same ns.
WRepoint(mOld, mNew, ref) ==
    /\ mOld # mNew /\ mOld[1] = mNew[1]
    /\ owner[mOld] = ref /\ ref \in Refs /\ owner[mNew] = None
    /\ (mNew \notin mfDeleted \/ SabotageReuseManifestId)
    /\ RefFreeFor(ref, mOld)
    /\ CommitGate(mNew)
    /\ Len(journal[mOld[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![mOld] = None, ![mNew] = ref]
    /\ journal' = AppendEvt(journal, mOld[1], ref, Bind(mOld), Bind(mNew))
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* ============================ GC pipeline (spec §Round Protocol) ============================
\* blobIndeg is ALWAYS the exact count of folded source edges by referenced blob (the
\* BlobInDegreeMatchesActiveManifests accounting). Recompute it from any (edges, activeMap) pair.
IndegFrom(edges, ae) == [b \in Blobs |-> Cardinality({ ed \in edges : ae[ed[1]][ed[2]] = b })]
EdgesFor(m) == { ed \in blobEdges : ed[1] = m }
\* m has an owner-removal recorded in the journal but not yet folded (the fold lags the owner change).
HasUnfoldedRemoval(m) ==
    \E i \in (cursor[m[1]] + 1)..Len(journal[m[1]]) :
        LET ev == journal[m[1]][i] IN ev.old = {m} /\ ev.new # {m}
\* A live precommit binding whose manifest body is not yet present+valid (the FIX-1 fold barrier).
LiveMissingBodyPrecommit(e) ==
    e.new # {} /\ owner[TheM(e.new)] \in Builds /\ ~(mBody[TheM(e.new)] /\ BodyValid(TheM(e.new)))

\* There is GC-collectable work pending: a present in-degree-0 blob not yet retired at its token, or
\* an unfolded journal event, or queued part-manifest cleanup. Used to let GC start another round even
\* at the round cap (re-using the top round number) so liveness is not an artifact of MaxRound — the
\* round COUNTER stays bounded (<= MaxRound) but GC may always make progress when work remains.
CollectableWorkPending ==
    \/ \E b \in Blobs : present[b] /\ blobIndeg[b] = 0 /\ ~(\E r \in retired : r.b = b /\ r.t = tokOf[b])
    \/ \E n \in Namespaces : cursor[n] < Len(journal[n])
    \/ mfCleanup # {}
GStartRound(l) ==
    /\ gcPhase[l] = "idle"
    /\ (gcRound < MaxRound \/ CollectableWorkPending)
    /\ gcRound' = IF gcRound < MaxRound THEN gcRound + 1 ELSE gcRound   \* counter capped at MaxRound
    /\ roundOf' = [roundOf EXCEPT ![l] = gcRound']
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "retiring"]
    \* Reset this round's completion-seal phase state (fresh fence/recheck/delete coverage). At a real
    \* round increment the slot is already fresh; at a cap re-use this clears the prior round's marks.
    /\ completionSeal' = [completionSeal EXCEPT ![gcRound'] = [fenced |-> {}, rechecked |-> {}, deleted |-> {}, adoptable |-> FALSE]]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, mfDeleted,
                    mPrefix, sweepEligible >>

\* Fold one RootOwnerEvent at cursor[n], dispatching by old-vs-new manifest comparison (rev.15).
GFoldTransition(n) ==
    /\ cursor[n] < Len(journal[n])
    /\ LET e == journal[n][cursor[n] + 1] IN
       \* FOLD BARRIER (control #23): do NOT advance past a live missing-body precommit, unless sabotaged.
       /\ (~LiveMissingBodyPrecommit(e) \/ SabotageAdvancePastMissingBodyPrecommit)
       /\ LET
            mo == IF e.old # {} THEN TheM(e.old) ELSE TheM(e.new)   \* old manifest (guarded by IsRemoval)
            mn == IF e.new # {} THEN TheM(e.new) ELSE TheM(e.old)   \* new manifest (guarded by IsActivation)
            \* The activation edge map of e.new, decided by body validity (committed fail-closed; a
            \* missing committed body fails closed unless SabotageMissingCommittedEmpty treats it empty;
            \* SabotageMissingBodyActivated forces edges for an absent precommit body).
            actMap == IF SabotageMissingBodyActivated THEN mEntries[mn]
                      ELSE IF mBody[mn] /\ BodyValid(mn) THEN mEntries[mn]
                      ELSE [p \in Paths |-> NoBlob]
            \* promote-of-never-activated: this owner-move event re-emits edges (the #22 fold hazard).
            promoteReEmit == SabotagePromoteAfterMissingBody /\ OwnerMoveSameManifest(e)
                             /\ (\A p \in Paths : mActiveEdges[mn][p] = NoBlob)
            \* Apply the REMOVAL of e.old first (clears its edges), THEN the ACTIVATION of e.new. A
            \* repoint event is BOTH a removal and an activation, so the two steps compose; a pure
            \* owner move (equal refs) is neither. (Using CASE here would wrongly do only one.)
            \* A removal seals its -1 decrements from the body, which MUST still be readable at fold
            \* (the spec's ordering: read the body / sealed edges while present, THEN allow body delete).
            \* If the body was deleted before this fold (only reachable under
            \* SabotageDeleteBodyBeforeDecrements), the decrement is LOST — the edges stay, indeg stays
            \* elevated, and the blobs leak forever (control #11, NoLeakForever).
            canDecrement == mBody[mo] \/ ~SabotageDeleteBodyBeforeDecrements
            aeRem == IF IsRemoval(e) /\ canDecrement THEN [mActiveEdges EXCEPT ![mo] = [p \in Paths |-> NoBlob]] ELSE mActiveEdges
            beRem == IF IsRemoval(e) /\ canDecrement THEN blobEdges \ EdgesFor(mo) ELSE blobEdges
            ae1 == IF IsActivation(e)    THEN [aeRem EXCEPT ![mn] = actMap]
                   ELSE IF promoteReEmit THEN [aeRem EXCEPT ![mn] = mEntries[mn]]
                   ELSE aeRem
            be1 == IF IsActivation(e)    THEN beRem \cup { <<mn, p>> : p \in {q \in Paths : actMap[q] \in Blobs} }
                   ELSE IF promoteReEmit THEN beRem \cup { <<mn, p>> : p \in {q \in Paths : mEntries[mn][q] \in Blobs} }
                   ELSE beRem
          IN
            /\ mActiveEdges' = ae1
            /\ blobEdges' = be1
            /\ blobIndeg' = IndegFrom(be1, ae1)
            \* a true removal queues part-manifest cleanup keyed by ManifestId; an owner move does not.
            /\ mfCleanup' = IF IsRemoval(e) THEN mfCleanup \cup {mo} ELSE mfCleanup
            /\ everEdged' = IF IsActivation(e) THEN everEdged \cup {mn[2]} ELSE everEdged
    /\ cursor' = [cursor EXCEPT ![n] = @ + 1]
    \* SabotageCutOverclaim jumps the sealed folded cursor to the journal end (the cut outruns the
    \* deltas it claims to cover) — recorded into the foldSeal coverage field.
    /\ foldSeal' = [foldSeal EXCEPT ![gcRound].foldedCursor[n] =
                       IF SabotageCutOverclaim THEN Len(journal[n]) ELSE cursor[n] + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, journal,
                    completionSeal, gcRound, gcPhase, roundOf, fencePos, trimBase, fenceVersion,
                    retired, inflight, wView, mfDeleted, mPrefix, sweepEligible >>

\* Retire a folded, present, in-degree-0 blob candidate at its CURRENT token (the HEAD).
GRetireBlob(l, b) ==
    /\ gcPhase[l] = "retiring"
    /\ present[b] /\ blobIndeg[b] = 0
    /\ ~\E r \in retired : r.b = b /\ r.t = tokOf[b]
    /\ retired' = retired \cup { [b |-> b, t |-> tokOf[b], r |-> roundOf[l]] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Global fence: fence every namespace this round. SabotageNoFence skips the fence write (a racing
\* publish is never blocked). SabotageRoundVisibilityEarly marks the round adoptable before the fence.
\* The round order is discover -> fold -> retire -> fence. The fence begins only after the fold has
\* caught up (cursor at journal end for every ns) AND every present in-degree-0 blob has been retired
\* this round (the retire phase completed). This ordering is what gives retire fairness teeth: a leak
\* candidate is always retired before the round can move past retiring. (A missing-body precommit can
\* park the fold; the watermark reclaim fairness unblocks it.)
RetirePhaseComplete ==
    /\ \A n \in Namespaces : cursor[n] = Len(journal[n])
    /\ \A b \in Blobs : (present[b] /\ blobIndeg[b] = 0) => (\E r \in retired : r.b = b /\ r.t = tokOf[b])
GFenceRegistry(l) ==
    /\ gcPhase[l] = "retiring"
    /\ RetirePhaseComplete
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "fencing"]
    /\ completionSeal' = [completionSeal EXCEPT ![roundOf[l]].adoptable =
                            IF SabotageRoundVisibilityEarly THEN TRUE ELSE @]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, gcRound, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, mfDeleted,
                    mPrefix, sweepEligible >>

GFenceShard(l, n) ==
    /\ gcPhase[l] \in {"fencing"}
    /\ n \notin completionSeal[roundOf[l]].fenced
    /\ IF SabotageNoFence
       THEN /\ fencePos' = fencePos
            /\ fenceVersion' = fenceVersion
       ELSE /\ fencePos' = [fencePos EXCEPT ![n] = Len(journal[n])]
            /\ fenceVersion' = [fenceVersion EXCEPT ![roundOf[l]][n] = roundOf[l]]
    /\ completionSeal' = [completionSeal EXCEPT ![roundOf[l]].fenced = @ \cup {n}]
    /\ gcPhase' = [gcPhase EXCEPT ![l] = IF Namespaces \subseteq (completionSeal[roundOf[l]].fenced \cup {n})
                                         THEN "fenced" ELSE "fencing"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, gcRound, roundOf, cursor,
                    trimBase, retired, inflight, wView, mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* Recheck + issue exact-token delete. Requires the fold to have provably reached every fence
\* position (SabotageNoRecheckFold-equivalent: SabotageRoundVisibilityEarly opens an early-visibility
\* hole that lets recheck run before the fold caught up). Delete only a still-in-degree-0 candidate.
FoldedThroughFence == \A n \in Namespaces : cursor[n] >= fencePos[n]
GRecheckDelete(l, e) ==
    /\ gcPhase[l] = "fenced" /\ e \in retired /\ e.r = roundOf[l]
    /\ (SabotageRoundVisibilityEarly \/ FoldedThroughFence)
    /\ IF blobIndeg[e.b] > 0
       THEN /\ retired' = retired \ {e}                       \* spared
            /\ inflight' = inflight
            /\ completionSeal' = completionSeal
       ELSE /\ [b |-> e.b, t |-> e.t] \notin inflight
            /\ retired' = retired                             \* kept until the landing confirms
            /\ inflight' = inflight \cup { [b |-> e.b, t |-> e.t] }
            /\ completionSeal' = [completionSeal EXCEPT ![roundOf[l]].deleted = @ \cup {e.b}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, wView, mfCleanup, mfDeleted, mPrefix,
                    sweepEligible >>

GEndRound(l) ==
    /\ gcPhase[l] = "fenced"
    /\ ~\E e \in retired : e.r = roundOf[l]
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "idle"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* A delete message lands: exact-token (412 = no-op). SabotageUncondDelete ignores the token match.
\* The landing is the confirmed outcome: the matching retired entry drops HERE. Any token stopping
\* being current joins deadTok (INV_NO_RETURN).
Land(d) ==
    /\ d \in inflight
    /\ inflight' = inflight \ {d}
    /\ retired' = { e \in retired : ~(e.b = d.b /\ e.t = d.t) }
    /\ IF present[d.b] /\ (SabotageUncondDelete \/ tokOf[d.b] = d.t)
       THEN /\ present' = [present EXCEPT ![d.b] = FALSE]
            /\ deadTok' = [deadTok EXCEPT ![d.b] = @ \cup {tokOf[d.b]}]
       ELSE /\ UNCHANGED << present, deadTok >>
    /\ UNCHANGED << tokOf, nextTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges, journal,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, wView, mfCleanup, mfDeleted,
                    mPrefix, sweepEligible >>

\* Journal trim: INV_JOURNAL_COVERAGE — only below the durable folded cursor. SabotageTrimUnincorporated
\* trims below an unfolded transition (cursor).
Trim(n) ==
    /\ IF SabotageTrimUnincorporated THEN trimBase[n] < Len(journal[n]) ELSE trimBase[n] < cursor[n]
    /\ trimBase' = [trimBase EXCEPT ![n] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible >>

\* ---- part-manifest cleanup ordering + orphan sweep + mutable update (Task 5) ----
\* Delete an unowned manifest body (exact-token abstracted). NORMAL: only after its owner-removal
\* blob decrements are sealed into the generation (its folded edges are gone: EdgesFor(m) = {}) AND it
\* was queued for cleanup. SabotageDeleteBodyBeforeDecrements deletes the body BEFORE the removal is
\* folded (edges still present) — so the later removal fold can no longer read the body to decrement.
GDeleteManifest(m) ==
    /\ mBody[m] /\ owner[m] = None
    /\ IF SabotageDeleteBodyBeforeDecrements
       THEN m \in mfCleanup \/ HasUnfoldedRemoval(m)   \* delete eagerly, possibly before the decrement
       ELSE m \in mfCleanup /\ EdgesFor(m) = {}        \* only after decrements are sealed
    /\ mBody' = [mBody EXCEPT ![m] = FALSE]
    /\ mfDeleted' = mfDeleted \cup {m}
    /\ mfCleanup' = mfCleanup \ {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight,
                    wView, mPrefix, sweepEligible >>

\* A durable watermark fact makes a build prefix conservatively sweep-eligible (the writer incarnation
\* can no longer publish from it). SabotageFrozenSeqAuthority sets it from a frozen-seq heuristic
\* (modeled by allowing eligibility for a prefix that still has a LIVE owner in it — judged-dead while
\* still active), which the orphan sweep then uses as deletion authority.
GMarkSweepEligible(p) ==
    /\ EnableOrphanSweep
    /\ ~sweepEligible[p]
    /\ ( SabotageFrozenSeqAuthority \/ (\A m \in ManifestIds : mPrefix[m] = p => owner[m] = None) )
    /\ sweepEligible' = [sweepEligible EXCEPT ![p] = TRUE]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight,
                    wView, mfCleanup, mfDeleted, mPrefix >>

\* Orphan pre-precommit sweep: deletes a staged-but-unowned manifest body whose build prefix is
\* sweep-eligible AND whose id is absent from the sealed owner view (owner = None). Emits NO blob
\* deltas (a pre-precommit manifest never contributed any). SabotageNoOrphanSweep disables the sweep
\* (debris leaks). SabotageWholesalePrefixDelete deletes the WHOLE eligible prefix regardless of the
\* owner view (so a live committed ref's manifest body is dropped -> committed dangle).
GOrphanSweep(n) ==
    /\ EnableOrphanSweep
    /\ ~SabotageNoOrphanSweep
    /\ IF SabotageWholesalePrefixDelete
       THEN \E m \in ManifestIds :
              /\ m[1] = n /\ mBody[m] /\ sweepEligible[mPrefix[m]]
              /\ LET grp == { x \in ManifestIds : mPrefix[x] = mPrefix[m] } IN
                 /\ mBody' = [x \in ManifestIds |-> IF x \in grp THEN FALSE ELSE mBody[x]]
                 /\ mfDeleted' = mfDeleted \cup grp
       ELSE \E m \in ManifestIds :
              /\ m[1] = n /\ mBody[m] /\ owner[m] = None /\ sweepEligible[mPrefix[m]]
              /\ mBody' = [mBody EXCEPT ![m] = FALSE]
              /\ mfDeleted' = mfDeleted \cup {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight,
                    wView, mfCleanup, mPrefix, sweepEligible >>

\* Mutable per-ref payload update: changes only mutable payload — NO owner transition, NO blob delta,
\* NO id change (MutablePayloadNotReachability). Modeled as a no-op stutter on every reachability var,
\* touching only an unmodeled payload. SabotageMutableAsReachability mints reachability: it emits a
\* spurious blob decrement (drops a folded edge) for a committed manifest, hiding a real transition.
WMutableUpdate(m) ==
    /\ EnableMutablePayload
    /\ owner[m] \in Refs
    /\ IF SabotageMutableAsReachability
       THEN /\ EdgesFor(m) # {}
            /\ LET be1 == blobEdges \ EdgesFor(m)
                   ae1 == [mActiveEdges EXCEPT ![m] = [p \in Paths |-> NoBlob]] IN
               /\ blobEdges' = be1
               /\ mActiveEdges' = ae1
               /\ blobIndeg' = IndegFrom(be1, ae1)
       ELSE /\ blobEdges' = blobEdges /\ mActiveEdges' = mActiveEdges /\ blobIndeg' = blobIndeg
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, journal,
                    everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, mfDeleted,
                    mPrefix, sweepEligible >>

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

\* ---- GC accounting (spec §GC Authority Model) ----
\* Durable blob in-degree equals the multiset of folded source edges by referenced blob.
BlobInDegreeMatchesActiveManifests ==
    \A b \in Blobs :
        blobIndeg[b] = Cardinality({ e \in blobEdges : mActiveEdges[e[1]][e[2]] = b })
\* Every folded source edge belongs to a manifest that is still owned, OR whose owner-removal is
\* recorded in the journal but not yet folded (the fold lags the owner change by design — the
\* removal -1 is pending). Without the pending-removal disjunct this would falsely flag the legitimate
\* drop-then-fold window. Fence+recheck protect the blob across exactly this window.
FoldedEdgesAreActive == \A e \in blobEdges : owner[e[1]] # None \/ HasUnfoldedRemoval(e[1])
MonotoneGC == [][ /\ gcRound' >= gcRound
                  /\ \A n \in Namespaces : /\ cursor'[n]   >= cursor[n]
                                           /\ trimBase'[n] >= trimBase[n] ]_vars

\* ---- part-manifest cleanup / sweep / mutable invariants ----
MutablePayloadNotReachability == TRUE   \* enforced by WMutableUpdate touching no reachability var on
                                        \* the honest path; SabotageMutableAsReachability violates INV_NO_LOSS
                                        \* by a spurious decrement that over-deletes a committed blob.
\* A manifest's emitted active edges are a sub-map of its body entries (or its body is gone/missing).
ManifestActivationMatchesEdges ==
    \A m \in ManifestIds : (\A p \in Paths : mActiveEdges[m][p] \in Blobs => mActiveEdges[m][p] = mEntries[m][p]) \/ ~mBody[m]

\* ---- liveness (under FairSpec) ----
\* A staged-unowned body in a sweep-eligible prefix eventually either DRAINS (deleted) or is ADOPTED
\* (gets an owner). The adoption disjunct is essential: a staged body a writer later promotes to a
\* committed ref must NOT be swept — the sweep only reclaims bodies that stay unowned.
OrphanManifestDebrisDrains ==
    \A m \in ManifestIds :
        (mBody[m] /\ owner[m] = None /\ sweepEligible[mPrefix[m]]) ~> (~mBody[m] \/ owner[m] # None)
\* A blob is referenced by some manifest that still has an owner (committed or precommit) — i.e. it is
\* reachable / protected and must NOT be reclaimed. The owner-transition fold may lag this, so the
\* protection is via the owner's body entries, not the (possibly not-yet-folded) blobEdges.
OwnedRefBlobs == UNION { BlobsOf(mEntries[m]) : m \in {x \in ManifestIds : owner[x] # None /\ mBody[x]} }
\* No present, unreferenced blob lingers forever: a blob that is present and not referenced by any
\* owned manifest is eventually deleted (or becomes referenced again). Unreferenced speculative debris
\* and decremented-to-zero blobs both drain.
NoLeakForever ==
    \A b \in Blobs :
        [](( present[b] /\ b \notin OwnedRefBlobs ) => <>( ~present[b] \/ b \in OwnedRefBlobs ))

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
    \/ \E m \in ManifestIds, f \in [Paths -> Blobs \cup {NoBlob}] : WManifestBodyArrives(m, f)
    \/ \E mOld, mNew \in ManifestIds, ref \in Refs : WRepoint(mOld, mNew, ref)
    \/ \E l \in Leaders : GStartRound(l) \/ GFenceRegistry(l) \/ GEndRound(l)
    \/ \E n \in Namespaces : GFoldTransition(n) \/ Trim(n)
    \/ \E l \in Leaders, b \in Blobs : GRetireBlob(l, b)
    \/ \E l \in Leaders, n \in Namespaces : GFenceShard(l, n)
    \/ \E l \in Leaders, e \in retired : GRecheckDelete(l, e)
    \/ \E d \in inflight : Land(d)
    \/ \E m \in ManifestIds : GDeleteManifest(m)
    \/ \E p \in BuildPrefixes : GMarkSweepEligible(p)
    \/ \E n \in Namespaces : GOrphanSweep(n)
    \/ \E m \in ManifestIds : WMutableUpdate(m)

Spec == Init /\ [][Next]_vars

FairSpec == Spec
    /\ WF_vars(\E l \in Leaders : GStartRound(l))
    /\ WF_vars(\E n \in Namespaces : GFoldTransition(n))
    /\ WF_vars(\E l \in Leaders, b \in Blobs : GRetireBlob(l, b))
    /\ WF_vars(\E l \in Leaders : GFenceRegistry(l))
    /\ WF_vars(\E l \in Leaders, n \in Namespaces : GFenceShard(l, n))
    /\ WF_vars(\E l \in Leaders, e \in retired : GRecheckDelete(l, e))
    /\ WF_vars(\E l \in Leaders : GEndRound(l))
    /\ WF_vars(\E d \in inflight : Land(d))
    /\ WF_vars(\E p \in BuildPrefixes : GMarkSweepEligible(p))
    /\ WF_vars(\E n \in Namespaces : GOrphanSweep(n))
    \* A stuck missing-body precommit is bounded by the watermark-based precommit reclaim (spec
    \* §Fold Owner Transitions liveness): reclaiming it removes the binding and unblocks the fold
    \* cursor. Modeled as fairness on abandoning a live precommit whose body never arrived.
    /\ WF_vars(\E m \in ManifestIds : owner[m] \in Builds /\ ~mBody[m] /\ WAbandonPrecommit(m))
=============================================================================
