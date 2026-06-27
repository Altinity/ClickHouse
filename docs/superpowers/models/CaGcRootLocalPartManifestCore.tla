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
    SabotageAdvancePastMissingBodyPrecommit,
    \* ---- Phase 2: token-diff discovery (rev.15 token split) ----
    EnableTokenDiff,            \* TRUE -> discover MAY skip an unchanged shard's body read
    TokenObservable,            \* TRUE -> LIST surfaces a per-shard token (supportsListTokens); FALSE -> always read
    SabotageSkipChangedShard    \* skip a shard whose listed root token actually advanced past the folded token (must dangle)

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
    mfCleanup, mfDeleted, mPrefix, sweepEligible,  \* part-manifest cleanup work; deleted-bodies set; per-manifest build-prefix; per-prefix orphan-sweep eligibility
    extraShared,                                   \* #2: ManifestIds carrying a SECOND shared committed owner (SabotageTwoOwners only)
    \* ---- Phase 2: token-diff discovery (rev.15 token split) ----
    listedTok,   \* [Namespaces -> 0..MaxToken] live root-shard token discovery observes from LIST; any owner transition advances it (discovery MAY set it)
    foldedTok    \* [Namespaces -> 0..MaxToken] persisted ShardCoverage.folded_token; advanced ONLY by the fold-seal write, NEVER by discovery

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
           mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* ---- helpers (filled across tasks) ----
NoOp == UNCHANGED vars

\* Phase 2 (rev.15 token split): an owner transition in namespace n advances the LIST-observable live
\* root-shard token (capped at MaxToken so TypeOK holds). Only the fold-seal write (GDiscoverRead) ever
\* advances foldedTok. When EnableTokenDiff is FALSE the token machinery is inert (listedTok stays at its
\* zero init), so every pre-Phase-2 stage is unaffected and its state space is unchanged.
BumpListed(n) == listedTok' = IF EnableTokenDiff
                              THEN [listedTok EXCEPT ![n] = IF listedTok[n] < MaxToken THEN listedTok[n] + 1 ELSE listedTok[n]]
                              ELSE listedTok

\* A blob token stops being current when displaced or deleted (INV_NO_RETURN oracle).
CondemnedTok(b, t) == t \in deadTok[b]

\* RefMatchesBody / ManifestNamespaceMatches: the body self-describes its ref + ns; a sabotage may
\* publish a manifest whose body disagrees. BodyValid is the TRUE predicate (the structural invariants
\* use it). BodyAccepted is what the gate/fold USE to decide acceptance: the honest path equals
\* BodyValid, but SabotageAcceptRefMismatch / SabotageAcceptNamespaceMismatch make the gate ACCEPT a
\* mismatched body (the unsafe "accept it" of controls #19/#20), so a committed binding becomes visible
\* over a body that names the wrong ref/ns.
BodyValid(m)    == mRef[m] = m /\ mNs[m] = m[1]
BodyAccepted(m) == \/ BodyValid(m)
                   \/ (SabotageAcceptRefMismatch /\ mNs[m] = m[1])        \* accept ref mismatch (ns still ok)
                   \/ (SabotageAcceptNamespaceMismatch /\ mRef[m] = m)    \* accept ns mismatch (ref still ok)

\* WStageManifest: write a part-manifest body BEFORE any owner transition (the pre-precommit object).
\* everEdged tracks ManifestIds (ns, instance) that have ever been bound to a body lineage;
\* NoManifestIdReuse forbids re-binding a visible ManifestId to a new body. Note it keys by the FULL
\* ManifestId, NOT the bare instance id: two namespaces may legitimately hold the same instance id as
\* DIFFERENT ManifestIds (the #18 KeyByRefNotId hazard). SabotageReuseManifestId drops the freshness
\* guard. SabotageAcceptRefMismatch / SabotageAcceptNamespaceMismatch write a mismatched body.
WStageManifest(m, f) ==
    /\ owner[m] = None /\ ~mBody[m]
    /\ (m \notin everEdged \/ SabotageReuseManifestId)   \* fresh ManifestId (never-reused) unless sabotaged
    /\ mBody' = [mBody EXCEPT ![m] = TRUE]
    /\ mEntries' = [mEntries EXCEPT ![m] = f]
    /\ mRef' = [mRef EXCEPT ![m] = IF SabotageAcceptRefMismatch THEN (CHOOSE x \in ManifestIds : x # m) ELSE m]
    /\ mNs' = [mNs EXCEPT ![m] = IF SabotageAcceptNamespaceMismatch THEN (CHOOSE n \in Namespaces : n # m[1]) ELSE m[1]]
    /\ everEdged' = everEdged \cup {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, owner, mActiveEdges, journal, blobIndeg,
                    blobEdges, foldSeal, completionSeal, gcRound, gcPhase, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup,
                    mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* ---- owner transitions (spec §Build And Precommit Protocol; §Fold Owner Transitions) ----
PresentBlobs == { b \in Blobs : present[b] }
\* The publish-gate FLOOR for namespace n: the highest round at which n was globally fenced. A writer
\* must have refreshed its retire-view to at least this round before it may publish into n (so it sees
\* the round's retired set). Derived from fenceVersion (the per-round fence record); 0 if never fenced.
PubFloor(n) == IF \E r \in 0..MaxRound : fenceVersion[r][n] > 0
               THEN CHOOSE r \in 0..MaxRound : fenceVersion[r][n] > 0 /\ \A r2 \in 0..MaxRound : fenceVersion[r2][n] > 0 => r2 <= r
               ELSE 0
\* A blob's CURRENT token is condemned in the WRITER'S retire-view: GC has a retired entry for that
\* exact token at a round the writer has refreshed to (e.r <= wView[w]), or the token is already
\* physically dead. This is the W-PUBLISH-GATE retire-view. The GLOBAL FENCE is load-bearing: a publish
\* requires wView[w] >= PubFloor(n), so a writer cannot publish without first observing the fence-round's
\* retired set — that is exactly what SabotageNoFence removes (PubFloor stays 0, the racing publish slips
\* through over a blob GC is about to delete). Inflight is GC-internal and NOT writer-visible here.
BlobCondemnedInView(b, w) ==
    \/ CondemnedTok(b, tokOf[b])
    \/ \E e \in retired : e.b = b /\ e.t = tokOf[b] /\ e.r <= wView[w]
\* A committed owner's fail-closed gate: body present+valid AND every named blob present and not
\* condemned-in-view. SabotageCommitSkipBlobReval drops the blob revalidation; SabotageMissingCommittedEmpty
\* belongs to the FOLD path (a missing committed body treated as empty), not here.
\* SabotagePrecommitlessProtect (#6): the writer assumes a speculatively-uploaded blob is protected by
\* the future build and skips revalidating that it is still live (present + not condemned). GC may have
\* deleted it before PrecommitAdd, so the committed publish dangles. (Distinct from #5: #5 skips body+blob
\* entirely; #6 keeps the present check but drops the condemned/retired-view recheck, modeling the
\* "assume protection" error.)
CommitGate(m, w) ==
    \/ SabotageCommitSkipBlobReval
    \/ ( mBody[m] /\ BodyAccepted(m)
         /\ wView[w] >= PubFloor(m[1])            \* must have observed the namespace's fence round
         /\ \A b \in BlobsOf(mEntries[m]) : present[b] /\ (SabotagePrecommitlessProtect \/ ~BlobCondemnedInView(b, w)) )
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
                    mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* The round a retire-view refresh may CLAIM COVERAGE of. The retired-token view is writer-visible only
\* AFTER the round's retire barrier (all retired sets durable). While a leader is still RETIRING at the
\* current round, a refresh can claim only gcRound-1 (the previous round's complete view). This is the
\* ViewableRound / retire-visibility-barrier (spec §Retire Visibility Barrier). SabotageRoundVisibilityEarly
\* (#13) makes the round visible after only partial retire work, so a refresh claims gcRound too early
\* and a later same-round retire+delete slips past the writer's gate -> a committed dangle.
ViewableRound == IF SabotageRoundVisibilityEarly THEN gcRound
                 ELSE IF \E l \in Leaders : gcPhase[l] = "retiring" /\ roundOf[l] = gcRound
                      THEN gcRound - 1 ELSE gcRound
WRefreshView(w) ==
    /\ wView' = [wView EXCEPT ![w] = ViewableRound]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* PrecommitAdd: owner[m] := build bld. A precommit MAY have a missing body only when EnableMissingBody
\* (the missing-body fail-closed intent). A precommit ACTIVATES (emits edges) iff its body is present
\* and valid; SabotageMissingBodyActivated forces edges even with the body absent. The intent record
\* (final_ref_name, manifest_ref, INTENDED entries) is known at PrecommitAdd even before the body is
\* uploaded, so a missing-body precommit may name intended blob entries f while mBody stays FALSE; GC
\* must NOT emit those as edges (non-activating intent) until the body is present.
WPrecommitAdd(m, bld, f) ==
    /\ EnablePrecommit
    /\ owner[m] = None
    /\ (m \notin mfDeleted \/ SabotageReuseManifestId)   \* a swept/deleted id is retired forever
    /\ (mBody[m] \/ EnableMissingBody)                  \* missing body only when allowed
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = bld]
    /\ mEntries' = IF mBody[m] THEN mEntries ELSE [mEntries EXCEPT ![m] = f]  \* record intended entries when no body yet
    /\ journal' = AppendEvt(journal, m[1], bld, NoBind, Bind(m))
    /\ BumpListed(m[1])
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, foldedTok >>

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
WPromote(m, bld, ref, w) ==
    /\ EnablePrecommit
    /\ owner[m] = bld
    /\ RefFreeFor(ref, m)
    \* split-promote and promote-after-missing-body both drop the atomic fail-closed activation gate:
    /\ (SabotageSplitPromote \/ SabotagePromoteAfterMissingBody \/ CommitGate(m, w))
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = ref]
    /\ journal' = AppendEvt(journal, m[1], ref, Bind(m), Bind(m))
    /\ BumpListed(m[1])
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, foldedTok >>

\* #2 (SabotageTwoOwners): a SECOND committed owner is attached to an already-committed ManifestId
\* (sharing a manifest across refs/namespaces). The single-owner rule (RefFreeFor / owner being a
\* function) forbids this on the honest path; the sabotage records the shared second owner in
\* extraShared. The manifest's blobs are now reachable from TWO owners, but GC tracks only one — so
\* dropping the tracked owner removes the manifest's blob edges still needed by the shared owner, and
\* the blob is over-deleted (INV_NO_LOSS). SingleManifestOwner also flags the shared state directly.
WShareOwner(m) ==
    /\ SabotageTwoOwners
    /\ owner[m] \in Refs /\ mBody[m]
    /\ m \notin extraShared
    /\ extraShared' = extraShared \cup {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight,
                    wView, mfCleanup, mfDeleted, mPrefix, sweepEligible, listedTok, foldedTok >>

\* Direct committed publish (no precommit). Same fail-closed body+blob gate as promote. Sets the
\* committed owner and activates the edges (committed manifests are always activated).
WPublishCommitted(m, ref, w) ==
    /\ owner[m] = None
    /\ (m \notin mfDeleted \/ SabotageReuseManifestId)
    /\ RefFreeFor(ref, m)
    /\ CommitGate(m, w)
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = ref]
    /\ journal' = AppendEvt(journal, m[1], ref, NoBind, Bind(m))
    /\ BumpListed(m[1])
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, foldedTok >>

\* Drop a committed ref: owner[m]: ref -> None; a true removal (-1 + mfCleanup queued at fold).
WDropRef(m) ==
    /\ owner[m] \in Refs
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = None]
    /\ journal' = AppendEvt(journal, m[1], owner[m], Bind(m), NoBind)
    /\ BumpListed(m[1])
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, foldedTok >>

\* Abandon a precommit owner: owner[m]: bld -> None; a true removal.
WAbandonPrecommit(m) ==
    /\ owner[m] \in Builds
    /\ Len(journal[m[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![m] = None]
    /\ journal' = AppendEvt(journal, m[1], owner[m], Bind(m), NoBind)
    /\ BumpListed(m[1])
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, foldedTok >>

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
    /\ everEdged' = everEdged \cup {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, owner, mActiveEdges, journal, blobIndeg,
                    blobEdges, foldSeal, completionSeal, gcRound, gcPhase, roundOf, fencePos,
                    cursor, trimBase, fenceVersion, retired, inflight, wView, mfCleanup, mfDeleted,
                    mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* Repoint a committed ref from mOld to mNew (last-op-wins at the root source): removes mOld and
\* activates mNew under the SAME ref, in one event (old=mOld, new=mNew). Both must be in the same ns.
WRepoint(mOld, mNew, ref, w) ==
    /\ mOld # mNew /\ mOld[1] = mNew[1]
    /\ owner[mOld] = ref /\ ref \in Refs /\ owner[mNew] = None
    /\ (mNew \notin mfDeleted \/ SabotageReuseManifestId)
    /\ RefFreeFor(ref, mOld)
    /\ CommitGate(mNew, w)
    /\ Len(journal[mOld[1]]) < MaxLog
    /\ owner' = [owner EXCEPT ![mOld] = None, ![mNew] = ref]
    /\ journal' = AppendEvt(journal, mOld[1], ref, Bind(mOld), Bind(mNew))
    /\ BumpListed(mOld[1])
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, mActiveEdges,
                    blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, foldedTok >>

\* ============================ GC pipeline (spec §Round Protocol) ============================
\* blobIndeg is ALWAYS the exact count of folded source edges by referenced blob (the
\* BlobInDegreeMatchesActiveManifests accounting). Recompute it from any (edges, activeMap) pair.
IndegFrom(edges, ae) == [b \in Blobs |-> Cardinality({ ed \in edges : ae[ed[1]][ed[2]] = b })]
\* The folded source edges of m. Control #18 (SabotageKeyByRefNotId) keys edges/cleanup by the bare
\* manifest_instance_id (the ManifestRef-without-namespace) instead of the full ManifestId, so a removal
\* in one namespace also strips an edge of a DIFFERENT namespace's manifest sharing the instance id —
\* merging unrelated blob edges and under-counting in-degree.
EdgesFor(m) == IF SabotageKeyByRefNotId
               THEN { ed \in blobEdges : ed[1][2] = m[2] }   \* match by instance id only (drops namespace)
               ELSE { ed \in blobEdges : ed[1] = m }
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
                    mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* Fold one RootOwnerEvent at cursor[n], dispatching by old-vs-new manifest comparison (rev.15).
GFoldTransition(n) ==
    /\ cursor[n] < Len(journal[n])
    /\ LET e == journal[n][cursor[n] + 1] IN
       \* FOLD BARRIER (control #23): do NOT advance past a live missing-body precommit. Bypassed by
       \* SabotageAdvancePastMissingBodyPrecommit (#23, fold-it-as-non-activated) and by
       \* SabotageMissingBodyActivated (#4, fold-it-AS-activated, emitting its intended edges).
       /\ (~LiveMissingBodyPrecommit(e) \/ SabotageAdvancePastMissingBodyPrecommit \/ SabotageMissingBodyActivated)
       /\ LET
            mo == IF e.old # {} THEN TheM(e.old) ELSE TheM(e.new)   \* old manifest (guarded by IsRemoval)
            mn == IF e.new # {} THEN TheM(e.new) ELSE TheM(e.old)   \* new manifest (guarded by IsActivation)
            \* The activation edge map of e.new, decided by body validity (committed fail-closed; a
            \* missing committed body fails closed unless SabotageMissingCommittedEmpty treats it empty;
            \* SabotageMissingBodyActivated forces edges for an absent precommit body). SabotageBareNonce
            \* (#17): the journal carried only a bare manifest_instance_id (no writer/build locator), so GC
            \* fold cannot build the manifest key — it emits NO edges for the committed activation, leaving
            \* the committed ref's blobs unprotected (over-delete -> INV_NO_LOSS).
            \* SabotageMissingCommittedEmpty (#10): a committed new-binding whose body the fold cannot
            \* read is treated as an EMPTY manifest (no edges) instead of failing closed — undercounts the
            \* committed ref's blob edges and permits over-delete (INV_NO_LOSS). Modeled by emitting empty
            \* edges for a committed activation.
            committedActivation == IsActivation(e) /\ owner[mn] \in Refs
            actMap == IF SabotageBareNonce THEN [p \in Paths |-> NoBlob]
                      ELSE IF SabotageMissingCommittedEmpty /\ committedActivation THEN [p \in Paths |-> NoBlob]
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
            \* #4 (SabotageMissingBodyActivated): a missing-body precommit was treated as ACTIVATED (folded
            \* past the barrier) but emitted NO edge (body absent, nothing to read). Its REMOVAL nonetheless
            \* emits -1 deltas from the intended entries — edges that were NEVER added — so a SHARED blob's
            \* in-degree is pushed below its true committed count and the blob is over-deleted (INV_NO_LOSS).
            sabUndercount == SabotageMissingBodyActivated /\ IsRemoval(e) /\ ~mBody[mo]
            ix0 == IndegFrom(be1, ae1)
            ix1 == IF sabUndercount
                   THEN [b \in Blobs |-> IF b \in BlobsOf(mEntries[mo]) /\ ix0[b] > 0 THEN ix0[b] - 1 ELSE ix0[b]]
                   ELSE ix0
          IN
            /\ mActiveEdges' = ae1
            /\ blobEdges' = be1
            /\ blobIndeg' = ix1
            \* a true removal queues part-manifest cleanup keyed by ManifestId; an owner move does not.
            /\ mfCleanup' = IF IsRemoval(e) THEN mfCleanup \cup {mo} ELSE mfCleanup
            /\ everEdged' = IF IsActivation(e) THEN everEdged \cup {mn} ELSE everEdged
    \* SabotageCutOverclaim (#12) advances the durable cursor PAST the unfolded suffix (to the journal
    \* end) while emitting deltas for only THIS one event — the cut outruns the deltas it claims to
    \* cover. The skipped activations' +1 source edges are never emitted, so their committed/precommit
    \* blobs look in-degree 0 and get over-deleted (a committed dangle). The honest cursor advances by 1.
    /\ cursor' = [cursor EXCEPT ![n] = IF SabotageCutOverclaim THEN Len(journal[n]) ELSE @ + 1]
    /\ foldSeal' = [foldSeal EXCEPT ![gcRound].foldedCursor[n] = cursor'[n]]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, journal,
                    completionSeal, gcRound, gcPhase, roundOf, fencePos, trimBase, fenceVersion,
                    retired, inflight, wView, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* Retire a folded, present, in-degree-0 blob candidate at its CURRENT token (the HEAD). The fold must
\* be CAUGHT UP first (cursor at journal end for every ns): otherwise a pending unfolded activation
\* (+1) referencing the blob has not been counted, and retiring it would race that activation — the
\* protocol order is discover -> fold -> retire. (Found by TLC: retiring an in-degree-0 blob whose
\* precommit/committed activation was still unfolded let a later promote keep a committed ref over a
\* blob GC then deleted -> CommittedNoMissingBlob.)
GRetireBlob(l, b) ==
    /\ gcPhase[l] = "retiring"
    /\ \A n \in Namespaces : cursor[n] = Len(journal[n])
    /\ present[b] /\ blobIndeg[b] = 0
    /\ ~\E r \in retired : r.b = b /\ r.t = tokOf[b]
    /\ retired' = retired \cup { [b |-> b, t |-> tokOf[b], r |-> roundOf[l]] }
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

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
                    mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

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
                    trimBase, retired, inflight, wView, mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* Recheck + issue exact-token delete. Requires the fold to have provably reached every fence
\* position (SabotageNoRecheckFold-equivalent: SabotageRoundVisibilityEarly opens an early-visibility
\* hole that lets recheck run before the fold caught up). Delete only a still-in-degree-0 candidate.
FoldedThroughFence == \A n \in Namespaces : cursor[n] >= fencePos[n]
GRecheckDelete(l, e) ==
    /\ gcPhase[l] = "fenced" /\ e \in retired /\ e.r = roundOf[l]
    /\ (SabotageRoundVisibilityEarly \/ FoldedThroughFence)
    \* Spare only if the in-degree recovered AND no delete is already in flight for this exact token.
    \* Once a delete is inflight, the entry is KEPT until the landing confirms the outcome (drop-on-
    \* confirmed-outcome): a spare that dropped an entry with a pending delete would make a writer stop
    \* seeing the blob as condemned while GC still deletes it — a committed dangle.
    /\ IF blobIndeg[e.b] > 0 /\ [b |-> e.b, t |-> e.t] \notin inflight
       THEN /\ retired' = retired \ {e}                       \* spared
            /\ inflight' = inflight
            /\ completionSeal' = completionSeal
       ELSE /\ [b |-> e.b, t |-> e.t] \notin inflight
            /\ blobIndeg[e.b] = 0                             \* only send a delete when still unreferenced
            /\ retired' = retired                             \* kept until the landing confirms
            /\ inflight' = inflight \cup { [b |-> e.b, t |-> e.t] }
            /\ completionSeal' = [completionSeal EXCEPT ![roundOf[l]].deleted = @ \cup {e.b}]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, gcRound, gcPhase, roundOf,
                    fencePos, cursor, trimBase, fenceVersion, wView, mfCleanup, mfDeleted, mPrefix,
                    sweepEligible, extraShared, listedTok, foldedTok >>

GEndRound(l) ==
    /\ gcPhase[l] = "fenced"
    /\ ~\E e \in retired : e.r = roundOf[l]
    /\ gcPhase' = [gcPhase EXCEPT ![l] = "idle"]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

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
                    mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* Journal trim: INV_JOURNAL_COVERAGE — only below the durable folded cursor. SabotageTrimUnincorporated
\* trims below an unfolded transition (cursor).
Trim(n) ==
    /\ IF SabotageTrimUnincorporated THEN trimBase[n] < Len(journal[n]) ELSE trimBase[n] < cursor[n]
    /\ trimBase' = [trimBase EXCEPT ![n] = @ + 1]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

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
                    wView, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* Stale part-manifest cleanup replay (control #1). When a ManifestId is REUSED (SabotageReuseManifestId),
\* a cleanup bundle queued for the OLD incarnation (m \in mfCleanup) cannot tell the id was rebound to a
\* NEW live owner. Replaying it strips the NEW incarnation's folded source edges — applying old blob
\* decrements to the new owner — so the still-referenced blob falls to in-degree 0 and is over-deleted
\* (INV_NO_LOSS). The unique-never-reused-id rule (NoManifestIdReuse) is exactly what forbids this.
GStaleReuseCleanup(m) ==
    /\ SabotageReuseManifestId
    /\ m \in mfCleanup /\ owner[m] # None /\ EdgesFor(m) # {}   \* reused: queued for cleanup yet live again
    /\ LET be1 == blobEdges \ EdgesFor(m)
           ae1 == [mActiveEdges EXCEPT ![m] = [p \in Paths |-> NoBlob]] IN
       /\ blobEdges' = be1
       /\ mActiveEdges' = ae1
       /\ blobIndeg' = IndegFrom(be1, ae1)
    /\ mfCleanup' = mfCleanup \ {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, journal,
                    everEdged, foldSeal, completionSeal, gcRound, gcPhase, roundOf, fencePos, cursor,
                    trimBase, fenceVersion, retired, inflight, wView, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

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
                    wView, mfCleanup, mfDeleted, mPrefix, extraShared, listedTok, foldedTok >>

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
              \* Honest sweep: per-object owner-view check (owner = None). SabotageFrozenSeqAuthority uses
              \* the frozen-seq prefix eligibility as the SOLE deletion authority, dropping the per-object
              \* owner check — so it deletes a still-live (committed) manifest body -> committed dangle.
              /\ m[1] = n /\ mBody[m] /\ sweepEligible[mPrefix[m]]
              /\ (SabotageFrozenSeqAuthority \/ owner[m] = None)
              /\ mBody' = [mBody EXCEPT ![m] = FALSE]
              /\ mfDeleted' = mfDeleted \cup {m}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound,
                    gcPhase, roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight,
                    wView, mfCleanup, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

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
                    mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* ---- Phase 2: token-diff discovery (spec §Discovery; the rev.15 token split) ----
\* The token-diff skip CLAIMS a shard's fold coverage WITHOUT reading its body: it advances the durable
\* fold cursor to the journal end (the shard is declared "covered this round") while emitting NO source
\* edges and NOT re-sealing foldedTok. This is exactly the "elide the body read / re-fold" of §Discovery.
\* A shard is skippable iff LIST surfaces a token (TokenObservable) AND the observed listed token equals
\* the persisted folded token (listedTok[n] = foldedTok[n]). When that guard holds the shard is UNCHANGED
\* since the last fold (no owner transition advanced listedTok, so the journal did not grow), hence the
\* cursor is already at the journal end and the skip is a true no-op — it folds nothing because there is
\* nothing left to fold. The cursor jump only ever has teeth on a CHANGED shard, which the equality guard
\* forbids on the honest path.
\*
\* The negative control SabotageSkipChangedShard DROPS the equality guard, so the skip may fire on a shard
\* whose listed token advanced past the folded token (listedTok[n] # foldedTok[n]) — i.e. with unfolded
\* owner transitions still in the journal. Claiming coverage then jumps the cursor PAST those unfolded
\* activations without ever emitting their +1 source edges (the SabotageCutOverclaim failure mode reached
\* through token-diff): the newly-committed manifest's blobs look in-degree 0 and GC over-deletes them ->
\* INV_NO_DANGLE. This is what proves the skip rule unsafe unless gated by listedTok = foldedTok.
\*
\* The skip governs ONLY the body read / re-fold cursor; it does NOT touch the fence (the all-shard fence
\* is orthogonal and still fences every shard every round, GFenceShard).
GDiscoverSkip(n) ==
    /\ EnableTokenDiff
    /\ TokenObservable
    /\ (listedTok[n] = foldedTok[n] \/ SabotageSkipChangedShard)
    \* Claim coverage to the journal end WITHOUT folding the body. On the honest path (listedTok =
    \* foldedTok) the shard is unchanged since the last fold, so cursor is already at Len(journal) and
    \* this is a harmless no-op (the skip elides only the I/O of re-reading already-folded bytes). Under
    \* SabotageSkipChangedShard it may jump the cursor PAST unfolded activations (cut-overclaim) -> dangle.
    /\ cursor' = [cursor EXCEPT ![n] = Len(journal[n])]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok, foldedTok >>

\* The body read + fold-seal write. Always legal (fail-closed to a read). The FOLD-SEAL WRITE is the
\* ONLY thing that advances the persisted folded token, bringing foldedTok[n] up to the listed token
\* observed at fold time so a later round may skip. Discovery itself never advances foldedTok — only
\* this seal write does. The fold of the journal records it covers is the existing GFoldTransition; this
\* action models ONLY the fold-seal folded-token write that follows a caught-up read, leaving the edge
\* fold (cursor/edges/blobIndeg) to GFoldTransition. Sealing only when the fold has actually caught up
\* (cursor at the journal end) keeps foldedTok an HONEST record of what was folded — never a claim that
\* outruns the deltas, which is what the skip guard then relies on.
GDiscoverRead(n) ==
    /\ EnableTokenDiff
    /\ cursor[n] = Len(journal[n])                       \* the body read folded through the journal end (GFoldTransition caught up)
    /\ foldedTok' = [foldedTok EXCEPT ![n] = listedTok[n]]
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, mBody, mEntries, mRef, mNs, owner, mActiveEdges,
                    journal, blobIndeg, blobEdges, everEdged, foldSeal, completionSeal, gcRound, gcPhase,
                    roundOf, fencePos, cursor, trimBase, fenceVersion, retired, inflight, wView,
                    mfCleanup, mfDeleted, mPrefix, sweepEligible, extraShared, listedTok >>

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
    /\ extraShared = {}
    \* Phase 2: a fresh pool has a zero live listed token and no folded token (the rev.15 token split):
    /\ listedTok = [n \in Namespaces |-> 0]
    /\ foldedTok = [n \in Namespaces |-> 0]

TypeOK ==
    /\ extraShared \in SUBSET ManifestIds
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
    /\ listedTok \in [Namespaces -> 0..MaxToken]
    /\ foldedTok \in [Namespaces -> 0..MaxToken]

INV_JOURNAL_COVERAGE == \A n \in Namespaces : trimBase[n] <= cursor[n]

\* once a body is staged for an instance id, no DIFFERENT body lineage rebinds it (self-ref stays = id):
NoManifestIdReuse ==
    \A m \in ManifestIds : mBody[m] => (mRef[m] = m \/ SabotageAcceptRefMismatch)
RefMatchesBody == \A m \in ManifestIds : (mBody[m] /\ owner[m] # None) => mRef[m] = m
ManifestNamespaceMatches == \A m \in ManifestIds : (mBody[m] /\ owner[m] # None) => mNs[m] = m[1]
INV_NO_RETURN == \A b \in Blobs : present[b] => tokOf[b] \notin deadTok[b]

\* ---- ownership / dangle invariants (spec §Safety Invariants) ----
\* Each visible ManifestId has at most one structural committed owner: no two committed refs share a
\* manifest (extraShared empty), and no ref names two manifests. (Builds may legitimately appear on
\* multiple precommits.) SabotageTwoOwners populates extraShared and is flagged directly.
SingleManifestOwner ==
    /\ extraShared = {}
    /\ \A m1, m2 \in ManifestIds :
        (owner[m1] # None /\ owner[m1] = owner[m2] /\ m1 # m2) => owner[m1] \in Builds
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
\* Blobs reachable from a committed ref OR from a SHARED second owner (#2 extraShared) — both must
\* keep their blobs present; a manifest shared across owners but tracked by only one loses blobs when
\* the tracked owner is dropped.
ReachableBlobs == UNION { BlobsOf(mEntries[m]) :
                           m \in {x \in ManifestIds : (owner[x] \in Refs \/ x \in extraShared) /\ mBody[x]} }
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

\* ---- non-vacuity witnesses (Task 9). Each W_x == ~(interesting reachable state); TLC must report it
\* VIOLATED, proving the dangerous-but-safe interleaving is actually reached (the positive stages are
\* not vacuously true). An UNEXPECTED PASS means the state is unreachable — a modeling bug.
W_PrecommitMissingBodyReached == ~(\E m \in ManifestIds : owner[m] \in Builds /\ ~mBody[m])
W_CommittedOverFoldedBlob == ~(\E m \in ManifestIds : owner[m] \in Refs /\ mBody[m]
                                  /\ \E p \in Paths : mActiveEdges[m][p] \in Blobs)
W_OrphanDeleted == ~(\E m \in ManifestIds : ~mBody[m] /\ owner[m] = None /\ m \in everEdged /\ m \in mfDeleted)

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
    \/ \E w \in Writers : WRefreshView(w)
    \/ \E m \in ManifestIds, bld \in Builds, f \in [Paths -> Blobs \cup {NoBlob}] : WPrecommitAdd(m, bld, f)
    \/ \E m \in ManifestIds, bld \in Builds, ref \in Refs, w \in Writers : WPromote(m, bld, ref, w)
    \/ \E m \in ManifestIds, ref \in Refs, w \in Writers : WPublishCommitted(m, ref, w)
    \/ \E m \in ManifestIds : WShareOwner(m)
    \/ \E m \in ManifestIds : WDropRef(m) \/ WAbandonPrecommit(m)
    \/ \E m \in ManifestIds, f \in [Paths -> Blobs \cup {NoBlob}] : WManifestBodyArrives(m, f)
    \/ \E mOld, mNew \in ManifestIds, ref \in Refs, w \in Writers : WRepoint(mOld, mNew, ref, w)
    \/ \E l \in Leaders : GStartRound(l) \/ GFenceRegistry(l) \/ GEndRound(l)
    \/ \E n \in Namespaces : GFoldTransition(n) \/ Trim(n)
    \/ \E l \in Leaders, b \in Blobs : GRetireBlob(l, b)
    \/ \E l \in Leaders, n \in Namespaces : GFenceShard(l, n)
    \/ \E l \in Leaders, e \in retired : GRecheckDelete(l, e)
    \/ \E d \in inflight : Land(d)
    \/ \E m \in ManifestIds : GDeleteManifest(m)
    \/ \E m \in ManifestIds : GStaleReuseCleanup(m)
    \/ \E p \in BuildPrefixes : GMarkSweepEligible(p)
    \/ \E n \in Namespaces : GOrphanSweep(n)
    \/ \E m \in ManifestIds : WMutableUpdate(m)
    \* Phase 2: token-diff discovery (the rev.15 token split):
    \/ \E n \in Namespaces : GDiscoverSkip(n)
    \/ \E n \in Namespaces : GDiscoverRead(n)

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
