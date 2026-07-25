-------------------- MODULE CaRelinkConfirmCore --------------------
(* Publish-then-confirm relink core -- spec
   `2026-07-23-cas-fetch-handoff-publish-confirm-design.md` (rev.5), Part B
   (§core-idea, §confirm-primitive, §correctness), Task 9 of the plan
   `2026-07-24-cas-publish-confirm-and-ref-lane-safety.md`.

   WHAT IS UNDER TEST.  Same-pool replication transfers only a part's MANIFEST; the receiver
   publishes its own ref over blobs the SENDER's ref still protects.  The receiver's own `+1`
   (`precommitAdd`, T1) is durable BEFORE it asks the sender to CONFIRM (T2) that the exact
   transferred `ManifestRef` is still the sender's committed binding.  Only a *yes* authorizes
   `promote`.  A durable `+1` alone proves nothing: it does not prove the blob was ALIVE when the
   `+1` landed.  The confirm at T2 proves liveness at T2, and T1 < T2, so protection was already
   in place at the instant liveness was proven -- that is the whole argument, and it is what this
   model checks.

   THE THREE PARTIES.
     * the SENDER's ref lane: a durable journal, an in-memory committed row that MAY LAG it
       (the post-durable-PUT window, §Problem 2), an admission queue (`pending`), a leader
       tenure (`leader_active`, one tenure spans several durable chunk transactions), an
       apply-pending POISON state (a durable transaction whose apply threw), and a mount fence.
       Gate 1 is an atomic predicate over exactly these (spec rules 1-6).
     * the RECEIVER: publish (durable `+1`) -> confirm -> promote, or abort (durable `-1`,
       releasing its protection).
     * GC: a per-namespace fold cursor over a journal it DISCOVERS BY LISTING, a source-edge
       presence set (`folded`), and three-phase graduation
       condemn -> delete_pending -> physical delete, with SPARING on positive in-degree.

   *** THE FOLD CURSOR IS MODELLED HONESTLY -- AND THAT IS A FINDING. ***
   The shipped GC discovers ref-log transactions with a paginated `LIST` that carries NO
   completeness proof, and advances the per-namespace cursor over WHATEVER THAT ROUND HAPPENED TO
   SEE (`CasGc.cpp:829,1033`, comment at `:1035-1037`; no gap/contiguity check exists) --
   BACKLOG `{#list-as-journal-dataloss-2026-07-25}`.  So `GFold` here folds an OBSERVATION SET
   `obs \subseteq Avail` and advances each namespace cursor to `Max(observed ids in that ns)`;
   a record omitted from `obs` while a LATER record of the same namespace is observed is skipped
   PERMANENTLY.  `MaxHoles` is the parameter -- the number of rounds in the whole behaviour that
   are allowed to return an INCOMPLETE page:
     * `MaxHoles = 0` -- ASSUMES every durable record above the cursor is returned by every LIST.
       This assumption is NOT established by the code.  It is the only setting under which the
       confirm protocol can be gated at all, so `_main` uses it.
     * `MaxHoles = 1` -- honest w.r.t. today's discovery mechanism, and deliberately the WEAKEST
       adversary that models it: ONE round, ONCE in the whole behaviour, returns a page that is
       missing a record; every other round is perfectly complete.  `_sab_holeylist` runs that with
       EVERY confirm rule intact and still violates `ConfirmedRelinkNeverDangles`.  The budget of
       one matters: it rules out the cheap traces where GC is merely lazy, so the counterexample
       MUST go through the permanent skip -- the omitted record sinks below its namespace cursor
       (advanced by a later record of the same namespace) and is never folded again, so the
       three-phase graduation's sparing can never recover it.  The confirm protocol cannot repair
       this; it is an independent release-blocker.

   Each Sabotage* flag removes exactly ONE load-bearing rule and MUST yield a counterexample:
     SabotageNoGate1            -> gate 1 rule 5 degenerates to ref-NAME match (drops exact
                                   ManifestRef equality) -> ABA: a repoint to another manifest
                                   answers *yes* over blobs the token's manifest no longer owns.
     SabotageStaleCache         -> gate 1 rule 3 (lane quiescence) dropped -> the confirm reads a
                                   committed row that lags a DURABLE removal.
     SabotageNoPoison           -> gate 1 rule 4 dropped -> a durable-but-unapplied removal leaves
                                   a permanently stale row on a QUIESCENT lane; only rule 4 sees it.
     SabotageNoFence            -> gate 1 rule 6 dropped -> a fence-less instance answers about a
                                   namespace whose current writer is somebody else.
     SabotagePublishAfterConfirm-> the design's ORDER is inverted (confirm, promote, then publish)
                                   -> violates `PromotedNeverDangles` (the antecedent-free form).

   OUT OF SCOPE (recorded as assumptions in `CaRelinkConfirmCore_RESULTS.md`): gate 0 (demoted in
   rev.5 to an availability filter -- it authorizes nothing, so it has no safety content to gate);
   gate 1 rule 2 (warm/recovered -- streaming recovery publishes atomically, `CasRefLedger.h:492`,
   so no half-recovered view is observable); the condemn-marker durability gate
   (`CaGcCondemnMarkerGate.tla`); multi-leader GC (`CaRetiredInRunFoldAbortWitness.tla`). *)
EXTENDS Integers, FiniteSets

CONSTANTS
    Receivers,                    \* relink receivers (one suffices for this safety class)
    MaxId,                        \* bound on the pool-wide ref-transaction id counter
    MaxRound,                     \* bound on the number of GC rounds
    MaxHoles,                     \* how many rounds in the whole behaviour may return an INCOMPLETE
                                  \* page (0 = the LIST is assumed complete; 1 = one holey page)
    SabotageNoGate1,              \* gate 1 rule 5: name-match instead of exact ManifestRef
    SabotageStaleCache,           \* gate 1 rule 3: ignore lane quiescence (pending / leader tenure)
    SabotageNoPoison,             \* gate 1 rule 4: ignore the apply-pending poison state
    SabotageNoFence,              \* gate 1 rule 6: ignore the mount fence / current-writer check
    SabotagePublishAfterConfirm   \* invert the design order: confirm+promote BEFORE the durable +1

(* ---- the universe ------------------------------------------------------------------------ *)

Blobs      == {"b1"}      \* ONE deduplicated blob: the sender's manifest and the receiver's
                          \* relinked manifest are two owners of the SAME content-addressed token.
Token      == "m1"        \* the ManifestRef minted by the sender at offer time (the relink token)
Other      == "m2"        \* a DIFFERENT manifest the sender may repoint the ref to; its own blobs
                          \* are outside this model's universe -- all that matters is that it does
                          \* NOT reference b1 (that is what makes the name-only confirm an ABA).
SenderEdge == "s_m1"      \* source-edge identity of the sender's committed binding of Token
NoiseSrc   == "noise"     \* a later, edge-neutral transaction in a namespace (an owner transition
                          \* on some other ref).  Its ONLY role: give the fold cursor something to
                          \* advance PAST, which is what turns a holey page into a permanent skip.
NsS        == "ns_s"      \* the sender's ref namespace
Namespaces == {NsS} \cup Receivers
BlobsOf(m) == IF m = Token THEN {"b1"} ELSE {}
Ids        == 0..MaxId
Rounds     == 0..MaxRound
Bindings   == {Token, Other, "none"}
Sources    == {SenderEdge, NoiseSrc} \cup Receivers
Records    == [id: Ids, ns: Namespaces, blob: Blobs, src: Sources, op: {"add", "del", "noop"}]

VARIABLES
    round,          \* published GC round
    present,        \* [Blobs -> BOOLEAN] -- the blob body physically exists
    condemned,      \* SUBSET Blobs -- phase 1 of graduation
    pendingDelete,  \* SUBSET Blobs -- phase 2 (delete_pending), published before any physical delete
    folded,         \* SUBSET [b: Blobs, src: Sources] -- the sealed source-edge PRESENCE set
    cursor,         \* [Namespaces -> Ids] -- the per-namespace fold cursor
    gcPhase,        \* {"idle","folded"}
    holes,          \* how many incomplete pages have been returned so far (bounded by MaxHoles)
    journal,        \* SUBSET Records -- ALL durable ref-log transactions (ground truth)
    nextId,         \* pool-wide monotone transaction id
    sDurableRef,    \* the sender's binding of the ref per the DURABLE journal (ground truth)
    sCacheRef,      \* the sender's in-memory committed row -- what the confirm actually reads
    sTarget,        \* the binding an admitted-but-not-yet-durable ref op will install
    sPending,       \* an item is admitted to the queue (`pending`), not yet durable
    sLeader,        \* a leader tenure is active (spans several durable chunk transactions)
    sPoison,        \* apply-pending POISON: a durable transaction whose in-memory apply threw
    sFence,         \* the mount fence is live / this instance is the namespace's current writer
    rState,         \* [Receivers -> {"init","published","confirmed","promoted","aborted"}]
    rAnswer,        \* [Receivers -> {"none","yes","no","unknown"}]
    rDurableBefore, \* [Receivers -> BOOLEAN] -- the +1 was durable BEFORE the confirm (T1 < T2)
    sawConfirmNo,   \* witness history: a confirm answered *no*
    sawConfirmUnk   \* witness history: a confirm answered *unknown*

gcVars     == << round, present, condemned, pendingDelete, folded, cursor, gcPhase, holes >>
senderVars == << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sPoison, sFence >>
recvVars   == << rState, rAnswer, rDurableBefore >>
logVars    == << journal, nextId >>
histVars   == << sawConfirmNo, sawConfirmUnk >>
vars       == << gcVars, senderVars, recvVars, logVars, histVars >>

Max(S)  == CHOOSE x \in S : \A y \in S : y <= x
Indeg(b) == Cardinality({ e \in folded : e.b = b })

Init ==
    /\ round = 0
    /\ present = [b \in Blobs |-> TRUE]
    /\ condemned = {}
    /\ pendingDelete = {}
    (* History: the sender's binding of Token is already committed and already folded. *)
    /\ folded = { [b |-> "b1", src |-> SenderEdge] }
    /\ cursor = [ns \in Namespaces |-> 0]
    /\ gcPhase = "idle"
    /\ holes = 0
    /\ journal = {}
    /\ nextId = 1
    /\ sDurableRef = Token
    /\ sCacheRef = Token
    /\ sTarget = Token
    /\ sPending = FALSE
    /\ sLeader = FALSE
    /\ sPoison = FALSE
    /\ sFence = TRUE
    /\ rState = [r \in Receivers |-> "init"]
    /\ rAnswer = [r \in Receivers |-> "none"]
    /\ rDurableBefore = [r \in Receivers |-> FALSE]
    /\ sawConfirmNo = FALSE
    /\ sawConfirmUnk = FALSE

(* ---- the sender's ref lane ---------------------------------------------------------------- *)

(* Admission: the op enters the queue and a leader tenure opens.  `pending` and `leader_active`
   are exactly the two predicates gate 1 rule 3 reads under `ref_queue_mutex`. *)
SenderAdmit(nb) ==
    /\ sFence
    /\ ~sPending
    /\ ~sPoison
    /\ sDurableRef = Token
    /\ nextId <= MaxId
    /\ sPending' = TRUE
    /\ sLeader' = TRUE
    /\ sTarget' = nb
    /\ UNCHANGED << sDurableRef, sCacheRef, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* The conditional PUT is acked: the transaction is DURABLE and GC can fold it.  The in-memory
   committed row is NOT yet updated -- this is the post-durable-PUT window (§Problem 2). *)
SenderDurable ==
    /\ sPending
    /\ sDurableRef = Token
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsS, blob |-> "b1", src |-> SenderEdge, op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = sTarget
    /\ UNCHANGED << sCacheRef, sTarget, sPending, sLeader, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, histVars >>

(* The in-memory apply succeeds; the tenure closes and the lane goes quiescent. *)
SenderApply ==
    /\ sPending
    /\ sDurableRef # Token
    /\ sCacheRef' = sDurableRef
    /\ sPending' = FALSE
    /\ sLeader' = FALSE
    /\ UNCHANGED << sDurableRef, sTarget, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* The in-memory apply THREW although the object is durable (allocation failure on the COW apply).
   The tenure closes -- the lane looks perfectly quiescent -- but the committed row is now
   permanently stale.  Only gate 1 rule 4 (poison) can see this. *)
SenderPoison ==
    /\ sPending
    /\ sDurableRef # Token
    /\ sPoison' = TRUE
    /\ sPending' = FALSE
    /\ sLeader' = FALSE
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sFence >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* The mount fence is lost: this instance is no longer the namespace's single writer. *)
FenceLoss ==
    /\ sFence
    /\ ~sPending
    /\ sFence' = FALSE
    /\ UNCHANGED << sDurableRef, sCacheRef, sTarget, sPending, sLeader, sPoison >>
    /\ UNCHANGED << gcVars, recvVars, logVars, histVars >>

(* The namespace's NEW writer removes the binding.  Durable, folded by GC -- and completely
   invisible to the deposed instance's committed rows. *)
ForeignRemove ==
    /\ ~sFence
    /\ sDurableRef = Token
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> NsS, blob |-> "b1", src |-> SenderEdge, op |-> "del"] }
    /\ nextId' = nextId + 1
    /\ sDurableRef' = "none"
    /\ UNCHANGED << sCacheRef, sTarget, sPending, sLeader, sPoison, sFence >>
    /\ UNCHANGED << gcVars, recvVars, histVars >>

(* ---- the receiver ------------------------------------------------------------------------- *)

(* T1: stage the manifest body and `precommitAdd`.  On return the +1 is DURABLE in the receiver's
   own journal.  NOTE: no presence probe -- promotion does not probe tokenless adopted leaves
   (`CasPartWriteTxn.cpp:1072-1106`), which is exactly the hole the confirm is placed to close. *)
RPublish(r) ==
    /\ \/ rState[r] = "init"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "promoted")
    /\ ~(\E rec \in journal : rec.ns = r /\ rec.op = "add")
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> r, blob |-> "b1", src |-> r, op |-> "add"] }
    /\ nextId' = nextId + 1
    /\ rState' = [rState EXCEPT ![r] = IF rState[r] = "init" THEN "published" ELSE "promoted"]
    /\ UNCHANGED << rAnswer, rDurableBefore >>
    /\ UNCHANGED << gcVars, senderVars, histVars >>

(* A later, edge-neutral transaction in the same namespace (an owner transition on another ref).
   Edge-neutral, so it changes NOTHING about reachability -- its only effect is to give the fold
   cursor a higher id to advance to.  That is precisely what converts an omitted page entry into a
   PERMANENT skip, and it is why the mirror-safety reproduction in the BACKLOG seeds a record `H`
   after the one it filters out. *)
NsNoise(r) ==
    /\ rState[r] # "init"
    /\ ~(\E rec \in journal : rec.ns = r /\ rec.op = "noop")
    /\ nextId <= MaxId
    /\ journal' = journal \cup
         { [id |-> nextId, ns |-> r, blob |-> "b1", src |-> NoiseSrc, op |-> "noop"] }
    /\ nextId' = nextId + 1
    /\ UNCHANGED << gcVars, senderVars, recvVars, histVars >>

(* T2: the confirm -- ONE atomic predicate over the sender's lane state (spec §confirm-primitive
   gate 1).  Zero object-store I/O by contract, so it reads the in-memory committed row
   `sCacheRef`; rules 3, 4 and 6 are exactly what make that row trustworthy at this instant. *)
Gate1Answer ==
    LET quiescent == SabotageStaleCache \/ (~sPending /\ ~sLeader)   \* rule 3
        clean     == SabotageNoPoison   \/ ~sPoison                  \* rule 4
        fenced    == SabotageNoFence    \/ sFence                    \* rule 6
        bound     == IF SabotageNoGate1                              \* rule 5
                       THEN sCacheRef # "none"                       \*   name-match only (sabotage)
                       ELSE sCacheRef = Token                        \*   exact ManifestRef equality
    IN IF ~quiescent \/ ~clean \/ ~fenced THEN "unknown"
       ELSE IF bound THEN "yes" ELSE "no"

RConfirm(r) ==
    /\ \/ rState[r] = "published"
       \/ (SabotagePublishAfterConfirm /\ rState[r] = "init")
    /\ LET ans == Gate1Answer IN
         /\ rAnswer' = [rAnswer EXCEPT ![r] = ans]
         /\ sawConfirmNo'  = (sawConfirmNo  \/ ans = "no")
         /\ sawConfirmUnk' = (sawConfirmUnk \/ ans = "unknown")
    /\ rDurableBefore' = [rDurableBefore EXCEPT ![r] = (rState[r] = "published")]
    /\ rState' = [rState EXCEPT ![r] = "confirmed"]
    /\ UNCHANGED << gcVars, senderVars, logVars >>

RPromote(r) ==
    /\ rState[r] = "confirmed"
    /\ rAnswer[r] = "yes"
    /\ rState' = [rState EXCEPT ![r] = "promoted"]
    /\ UNCHANGED << rAnswer, rDurableBefore >>
    /\ UNCHANGED << gcVars, senderVars, logVars, histVars >>

(* Anything but *yes* aborts, and the abort RELEASES the receiver's protection (a durable -1). *)
RAbort(r) ==
    /\ rState[r] = "confirmed"
    /\ rAnswer[r] # "yes"
    /\ rState' = [rState EXCEPT ![r] = "aborted"]
    /\ IF \E rec \in journal : rec.ns = r /\ rec.op = "add"
         THEN /\ nextId <= MaxId
              /\ journal' = journal \cup
                   { [id |-> nextId, ns |-> r, blob |-> "b1", src |-> r, op |-> "del"] }
              /\ nextId' = nextId + 1
         ELSE UNCHANGED logVars
    /\ UNCHANGED << rAnswer, rDurableBefore >>
    /\ UNCHANGED << gcVars, senderVars, histVars >>

(* ---- GC: discovery, fold cursor, three-phase graduation ------------------------------------ *)

ApplyOne(F, rec) ==
    IF rec.op = "add" THEN F \cup { [b |-> rec.blob, src |-> rec.src] }
    ELSE IF rec.op = "del" THEN F \ { [b |-> rec.blob, src |-> rec.src] }
    ELSE F

(* The shipped merge is a set-presence merge keyed by (blob_ref, source_id) -- last-wins per key,
   a remove whose key is absent is a silent no-op (`CasBlobInDegree.cpp:585-597`).  Applying in
   strict transaction-id order reproduces that. *)
RECURSIVE ApplyOrdered(_, _)
ApplyOrdered(F, S) ==
    IF S = {} THEN F
    ELSE LET m == CHOOSE x \in S : \A y \in S : x.id <= y.id
         IN ApplyOrdered(ApplyOne(F, m), S \ {m})

Avail == { rec \in journal : rec.id > cursor[rec.ns] }

(* THE HONEST FOLD.  `obs` is what THIS round's paginated LIST actually returned.  The cursor is
   advanced to the highest id OBSERVED per namespace -- not to the highest id that EXISTS.  With
   `ObserveComplete = FALSE` a record can be omitted while a later same-namespace record is
   returned, and it is then below the cursor forever: skipped permanently, with ref-log cleanup
   free to delete it (`CasGc.cpp:1502` requires cursor coverage, not proof of application). *)
GFold ==
    /\ gcPhase = "idle"
    /\ round < MaxRound
    /\ \E obs \in SUBSET Avail :
         /\ obs = Avail \/ holes < MaxHoles
         /\ holes' = IF obs = Avail THEN holes ELSE holes + 1
         /\ folded' = ApplyOrdered(folded, obs)
         /\ cursor' = [ ns \in Namespaces |->
                          LET seen == { rec.id : rec \in { x \in obs : x.ns = ns } }
                          IN IF seen = {} THEN cursor[ns] ELSE Max(seen) ]
    /\ gcPhase' = "folded"
    /\ UNCHANGED << round, present, condemned, pendingDelete >>
    /\ UNCHANGED << senderVars, recvVars, logVars, histVars >>

(* One-pass round commit over the folded cut: SPARE on positive in-degree, otherwise advance one
   phase -- condemn, then delete_pending, then the physical exact-token delete. *)
GSettle ==
    /\ gcPhase = "folded"
    /\ LET live  == { b \in Blobs : Indeg(b) > 0 }
           kills == { b \in pendingDelete : present[b] /\ Indeg(b) = 0 }
           grads == { b \in (condemned \ pendingDelete) : present[b] /\ Indeg(b) = 0 }
           newly == { b \in Blobs : present[b] /\ Indeg(b) = 0 /\ b \notin condemned }
       IN /\ present'       = [ b \in Blobs |-> IF b \in kills THEN FALSE ELSE present[b] ]
          /\ condemned'     = ((condemned \ live) \ kills) \cup newly
          /\ pendingDelete' = ((pendingDelete \ live) \ kills) \cup grads
    /\ round' = round + 1
    /\ gcPhase' = "idle"
    /\ UNCHANGED << folded, cursor, holes >>
    /\ UNCHANGED << senderVars, recvVars, logVars, histVars >>

(* Self-loop so exhausting the bounded counters is not reported as a TLC deadlock (house pattern;
   every cfg also sets CHECK_DEADLOCK FALSE). *)
NoOp == UNCHANGED vars

Next ==
    \/ \E nb \in {Other, "none"} : SenderAdmit(nb)
    \/ SenderDurable \/ SenderApply \/ SenderPoison
    \/ FenceLoss \/ ForeignRemove
    \/ \E r \in Receivers : RPublish(r) \/ NsNoise(r) \/ RConfirm(r) \/ RPromote(r) \/ RAbort(r)
    \/ GFold \/ GSettle
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---------------------------------------------------------------------------- *)

TypeOK ==
    /\ round \in Rounds
    /\ present \in [Blobs -> BOOLEAN]
    /\ condemned \subseteq Blobs
    /\ pendingDelete \subseteq condemned
    /\ \A e \in folded : e.b \in Blobs /\ e.src \in Sources
    /\ cursor \in [Namespaces -> Ids]
    /\ gcPhase \in {"idle", "folded"}
    /\ holes \in 0..MaxHoles
    /\ journal \subseteq Records
    /\ nextId \in 1..(MaxId + 1)
    /\ sDurableRef \in Bindings /\ sCacheRef \in Bindings /\ sTarget \in Bindings
    /\ sPending \in BOOLEAN /\ sLeader \in BOOLEAN /\ sPoison \in BOOLEAN /\ sFence \in BOOLEAN
    /\ rState \in [Receivers -> {"init", "published", "confirmed", "promoted", "aborted"}]
    /\ rAnswer \in [Receivers -> {"none", "yes", "no", "unknown"}]
    /\ rDurableBefore \in [Receivers -> BOOLEAN]
    /\ sawConfirmNo \in BOOLEAN /\ sawConfirmUnk \in BOOLEAN

LiveBlobs == { b \in Blobs : present[b] }
Promoted(r) == rState[r] = "promoted"

(* THEOREM ConfirmedRelinkNeverDangles (plan Task 9, step 3).  A relink that was promoted on a
   confirm *yes* whose activation (+1) was durable BEFORE the confirm never references a blob that
   has been physically deleted. *)
ConfirmedRelinkNeverDangles ==
    \A r \in Receivers :
        (Promoted(r) /\ rAnswer[r] = "yes" /\ rDurableBefore[r])
            => BlobsOf(Token) \subseteq LiveBlobs

(* The antecedent-free form.  In the honest model it coincides with the theorem (a promote is only
   reachable through a *yes*, and the design's order makes the +1 durable first).  Inverting the
   order -- confirm/promote before the durable +1 -- breaks THIS one while leaving the guarded
   theorem vacuously satisfied, which is exactly how `_sab_publishafterconfirm` shows that
   publish-BEFORE-confirm is load-bearing rather than incidental. *)
PromotedNeverDangles ==
    \A r \in Receivers : Promoted(r) => BlobsOf(Token) \subseteq LiveBlobs

(* Sanity: graduation is three-phase and physical deletion never happens straight from condemn. *)
GraduationIsPhased == pendingDelete \subseteq condemned

(* ---- witnesses (negated reachability; a TLC "violation" means the state IS reachable) ------- *)

(* The confirm's *no* branch actually fires -- the guard is not dead code. *)
W_ConfirmNo == ~sawConfirmNo

(* The confirm's *unknown* branch actually fires. *)
W_ConfirmUnknown == ~sawConfirmUnk

(* NON-VACUITY OF THE THEOREM: the antecedent (promoted + yes + activation durable first) is
   reachable.  Without this, `_main` passing would prove nothing. *)
W_ConfirmYesPromoted ==
    ~(\E r \in Receivers : Promoted(r) /\ rAnswer[r] = "yes" /\ rDurableBefore[r])

(* NON-VACUITY OF THE CONSEQUENT: GC in this model really does delete blobs, so
   `BlobsOf(Token) \subseteq LiveBlobs` is not trivially true everywhere. *)
W_BlobDeleted == \A b \in Blobs : present[b]

=============================================================================
