---------------------------- MODULE CaRetiredInRunFoldAbortWitness ----------------------------
(* Add-only GC freshness-meta gate (spec 2026-07-11-cas-deposed-leader-clearsparedmeta-fix §5).

   One gc-shard, one implicit writer, blobs Blobs, TWO bounded in-flight GC leaders Leaders =
   {La, Lb}. Writers add/remove source edges through an append-only journal; a GC leader folds a
   journal prefix (the cut) into an attempt-scoped run artifact {edges, condemned rows} and adopts
   it via one gc/state CAS. Settlement follows the retired-in-run spec exactly (Settle below).

   ============================================================================================
   THE GATE (this file proves the deposed-leader `clearSparedMeta` fix — GC meta is ADD-ONLY):
   ============================================================================================
   GC freshness metadata is ADD-ONLY. A condemn/pending result may set meta[b]="cond"; a SPARE
   (in-degree recovered, d>0) leaves meta[b] UNCHANGED; a successful exact-token delete may model
   the meta clean (the matching body is already absent). ONLY a WRITER that displaced the body with
   a fresh incarnation token (BodyResolve's resurrect / fresh-upload) publishes "clean". GC never
   transitions a present-cond hash to clean on a spare — neither an adopting FoldRound nor a deposed
   FoldAbort. This restores the exact-token-delete argument: once a hash is Condemned, observing
   Clean ⟹ the body is absent OR a writer already bumped its token, so any stale pending
   deleteExact(t1) finds the body absent or TokenMismatch.

   Honest config = GREEN (INV_NO_LOSS/INV_NO_RETURN/INV_COVERAGE/INV_ONE_PASS). Sabotages RED:
   - inmem_token  : redelete keys off a re-observed in-memory token (deletes a resurrected live
                    incarnation)                                        -> INV_NO_RETURN.
   - attempt_reuse: a re-execution reuses the prior attempt's keys at an advanced cut and the stale
                    run bytes are silently adopted under the newer coverage -> INV_COVERAGE.
   - no_pacing    : graduate-at-birth (fresh condemn minted directly delete_pending) so the pend row
                    coincides with the one-round stale-reuse window        -> INV_NO_LOSS.
   - gc_clear_on_spare  : a SPARE sets meta[b]="clean" pre-CAS (the OLD clearSparedMeta behavior) —
                    even a deposed leader's stray clean lets a writer reuse the condemned token that
                    a pending redelete then kills                          -> INV_NO_LOSS.
   - post_adoption_clear: Fix 1 — the clear is moved AFTER the winning gc/state CAS (only the
                    adopting leader clears a spare to "clean"). This is the LOAD-BEARING sabotage:
                    it proves Fix 1 is INSUFFICIENT and add-only (Fix 4) is required. The final CAS
                    fences ADOPTION, not the pre-CAS deleteExact side effect issued by an older
                    leader from a stale snapshot. The split-action two-leader model below reaches
                    spec §2's interleaving: an older leader La captures a delete_pending(h,t1)
                    snapshot and plans deleteExact(h,t1); a newer leader Lb folds a later cut that
                    recovered h's in-degree, adopts the SPARE and (Fix 1) clears meta[h]->"clean";
                    the writer's deferred body-observe reads clean and REUSES t1; La then executes
                    its stale deleteExact(h,t1) against the live reuse  -> INV_NO_LOSS.

   ============================================================================================
   SPLIT-ACTION TWO-LEADER MODEL (defeats a false-green; spec §5 "strengthen the model"):
   ============================================================================================
   Each leader runs three separate actions so an older leader's pre-CAS side effects can interleave
   with a newer leader's adoption:
     LeaderCapture(L) : snapshot `adopted` at a chosen cut >= the coverage floor lenAtAdopt, and
                        claim an attempt key from the monotone lease.seq (nextAttempt).
     LeaderExecute(L) : execute the round's PRE-CAS side effects against the CURRENT physical state
                        — the exact-token redelete deleteExact of prior-adopted delete_pending rows
                        at d=0 (token from the captured snapshot), the badDelete ghost, and the
                        advisory add-only `.meta` writes (condemn -> "cond"; confirmed delete ->
                        "clean"; spare -> UNCHANGED unless a sabotage clears it). Seal the run
                        artifact.
     LeaderAdopt(L)   : the single gc/state CAS. WIN iff adopted.gen is still the captured gen (no
                        intervening adoption) and the putIfAbsent is byte-equal-or-fresh; then adopt
                        the sealed artifact and advance the coverage floor. Otherwise the leader is
                        DEPOSED (== FoldAbort, spec §4/§6): adopted/artifacts/lenAtAdopt do NOT move;
                        its execute-time deletes + advisory meta already persisted — the point is
                        they are HARMLESS under add-only. (post_adoption_clear's clear happens ONLY
                        on the WIN branch, faithfully modeling Fix 1's post-CAS clear.)

   The writer is likewise split into EDGE-BEFORE-OBSERVE halves so a recovery edge can be journalled
   (recovering in-degree, making Lb spare) with the body decision DEFERRED, and the later observe can
   read the post-spare meta:
     EdgeAdd(b)     : journal the edge and take a live ref. If the body is absent (phys=0) upload a
                      fresh incarnation immediately (a brand-new blob has no dedup target); if the
                      body is present (phys>0), mark it unresolved (dedup — the body observe is
                      deferred).
     BodyResolve(b) : the deferred point-read decides the body. absent-now (deleted mid-window) ->
                      fresh upload; meta "clean" -> dedup-REUSE the token; meta "cond" inside the
                      ONE-ROUND stale window (adopted.cond[b].round = adopted.round — the read raced
                      the current fold's meta write and returned the pre-fold clean) -> stale reuse;
                      otherwise (meta "cond", window closed) -> RESURRECT (fresh token, clean meta).
   The one-round stale window only fires while the row is at the current adopted round, i.e. BEFORE
   graduation to delete_pending (pending rows have cond.round < adopted.round); so the honest window
   and the deletable-pending window are DISJOINT — no writer reuses a token that has a pending
   redelete. post_adoption_clear breaks exactly this: a clean meta over a still-pending row lets
   BodyResolve reuse t1 regardless of the pacing window.

   INV_NO_LOSS excludes `unresolved[b]` (a deferred dedup body-observe): during that window the body
   may be transiently absent and the writer re-materializes from source on observe (the safe
   direction). The §2 losses are all detected at RESOLVED states (the writer reused t1, THEN the
   stale delete fired), so the exclusion masks no bug.

   ============================================================================================
   Retained retired-in-run rationale (spec 2026-07-10 §6; each was required to keep honest GREEN and
   every sabotage RED):
   - Coverage floor lenAtAdopt: each fold's cut must cover every journal entry landed before the
     previous adoption (round N+1 reads journals after round N's CAS). Entries landing between the
     previous adoption and this fold's read may still be missed (cut < Len(journal)) — the real
     racing window kept nondeterministic.
   - Incremental fold: newEdges = adopted.edges + delta over (adopted.cut, cut], the real 2-cursor
     merge; recomputing from the whole prefix would silently HEAL a stale adoption (attempt_reuse).
   - Monotone per-blob token mint nextTok (etags never repeat).
   - INV_COVERAGE: the adopted edge counts always equal the journal truth at the claimed cut.
   - INV_NO_RETURN ghost: a delete whose token /= the durably adopted condemn-time token trips it.
   - MaxJournal bound + NoOp self-loop (exhausted bounds are not a TLC deadlock).

   BOUNDS (recorded per spec §5's "bump-and-record", never weaken an invariant): the split-action
   two-leader interleaving needs up to nextAttempt = MaxRound + 1 concurrent attempt claims to reach
   spec §2 (two folds in flight at the racing round on top of the setup folds). LeaderCapture is
   guarded by `nextAttempt <= MaxRound + 1`. Honest config green and all six sabotage/honest results
   hold at Blobs = {b1, b2}, MaxRound = 4, MaxToken = 3, MaxJournal = 5.

   Sabotage \in {"none","inmem_token","attempt_reuse","no_pacing","gc_clear_on_spare",
                 "post_adoption_clear"}. *)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Blobs,          \* e.g. {b1, b2} (model values)
          MaxRound,       \* fold-round bound, e.g. 4
          MaxToken,       \* incarnation tokens per blob, e.g. 3
          MaxJournal,     \* journal length bound, e.g. 5
          Sabotage        \* see the set below

ASSUME Sabotage \in {"none", "inmem_token", "attempt_reuse", "no_pacing",
                     "gc_clear_on_spare", "post_adoption_clear"}

Leaders == {"La", "Lb"}   \* two bounded in-flight GC leaders (split-action)

VARIABLES
  journal,      \* Seq of <<blob, op>>, op \in {"add","rm"} — writer edge events (abstract sources)
  phys,         \* [Blobs -> 0..MaxToken] physical incarnation token; 0 = absent
  nextTok,      \* [Blobs -> 1..MaxToken+1] next fresh token to mint (etags never repeat)
  liveRef,      \* [Blobs -> BOOLEAN] writer holds a live reference (edge added, not removed)
  unresolved,   \* [Blobs -> BOOLEAN] a present-body dedup edge whose body observe is deferred
  adopted,      \* durable adopted artifact:
                \*   [gen, cut, edges: [Blobs -> Int],
                \*    cond: [Blobs -> [st: {"none","cond","pend"}, tok: Int, round: Int]], round]
  artifacts,    \* attempt-keyed store: attempt |-> artifact — the putIfAbsent byte-equal domain
  nextAttempt,  \* monotone attempt counter (lease.seq abstraction; bumped every capture)
  lenAtAdopt,   \* Len(journal) at the last adoption — the next fold's coverage floor
  meta,         \* [Blobs -> {"clean","cond"}] advisory freshness meta; no destructive read
  badDeleteEver,\* ghost: a delete executed with a token /= the durably adopted condemn-time token
  leaders       \* [Leaders -> [phase: {"idle","captured","executed"}, snap, cut, attempt, art]]

vars == <<journal, phys, nextTok, liveRef, unresolved, adopted, artifacts, nextAttempt,
          lenAtAdopt, meta, badDeleteEver, leaders>>

NoRow == [st |-> "none", tok |-> 0, round |-> 0]

InitAdopted == [gen |-> 0, cut |-> 0, edges |-> [b \in Blobs |-> 0],
                cond |-> [b \in Blobs |-> NoRow], round |-> 0]

\* Journal truth at a prefix (the oracle the invariants compare against).
EdgeCount(b, cut) ==
  Cardinality({i \in 1..cut : journal[i][1] = b /\ journal[i][2] = "add"})
    - Cardinality({i \in 1..cut : journal[i][1] = b /\ journal[i][2] = "rm"})

\* Incremental delta over the window (lo, hi] — what the real 2-cursor merge consumes.
Delta(b, lo, hi) ==
  Cardinality({i \in (lo + 1)..hi : journal[i][1] = b /\ journal[i][2] = "add"})
    - Cardinality({i \in (lo + 1)..hi : journal[i][1] = b /\ journal[i][2] = "rm"})

Init ==
  /\ journal = <<>>
  /\ phys = [b \in Blobs |-> 0]
  /\ nextTok = [b \in Blobs |-> 1]
  /\ liveRef = [b \in Blobs |-> FALSE]
  /\ unresolved = [b \in Blobs |-> FALSE]
  /\ adopted = InitAdopted
  /\ artifacts = <<>>
  /\ nextAttempt = 1
  /\ lenAtAdopt = 0
  /\ meta = [b \in Blobs |-> "clean"]
  /\ badDeleteEver = FALSE
  /\ leaders = [L \in Leaders |-> [phase |-> "idle", snap |-> InitAdopted, cut |-> 0,
                                   attempt |-> 0, art |-> InitAdopted]]

(* -------- Writer: EDGE-BEFORE-OBSERVE, split into edge-journal and body-observe. -------- *)

(* Journal an edge and take a live ref. A brand-new blob (phys=0) has no dedup target: upload a
   fresh incarnation immediately. A present body (phys>0) is a dedup: defer the body observe. *)
EdgeAdd(b) ==
  /\ Len(journal) < MaxJournal
  /\ ~liveRef[b]
  /\ IF phys[b] = 0
     THEN /\ nextTok[b] <= MaxToken
          /\ phys' = [phys EXCEPT ![b] = nextTok[b]]
          /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
          /\ meta' = [meta EXCEPT ![b] = "clean"]
          /\ unresolved' = unresolved
     ELSE /\ unresolved' = [unresolved EXCEPT ![b] = TRUE]
          /\ UNCHANGED <<phys, nextTok, meta>>
  /\ journal' = Append(journal, <<b, "add">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = TRUE]
  /\ UNCHANGED <<adopted, artifacts, nextAttempt, lenAtAdopt, badDeleteEver, leaders>>

(* The deferred body observe. absent-now (deleted mid-window) -> fresh upload; meta clean ->
   dedup-REUSE the current token; meta cond inside the one-round stale window -> stale reuse; else
   -> RESURRECT (fresh token, clean meta). Only this action (and EdgeAdd's fresh upload) publishes
   "clean" over a present body — GC never does. *)
BodyResolve(b) ==
  /\ unresolved[b]
  /\ LET window == adopted.cond[b].st \in {"cond", "pend"} /\ adopted.cond[b].round = adopted.round
     IN IF phys[b] = 0
        THEN /\ nextTok[b] <= MaxToken
             /\ phys' = [phys EXCEPT ![b] = nextTok[b]]
             /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
             /\ meta' = [meta EXCEPT ![b] = "clean"]
        ELSE IF meta[b] = "clean"
             THEN UNCHANGED <<phys, nextTok, meta>>
        ELSE IF window
             THEN UNCHANGED <<phys, nextTok, meta>>
        ELSE /\ nextTok[b] <= MaxToken
             /\ phys' = [phys EXCEPT ![b] = nextTok[b]]
             /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
             /\ meta' = [meta EXCEPT ![b] = "clean"]
  /\ unresolved' = [unresolved EXCEPT ![b] = FALSE]
  /\ UNCHANGED <<journal, liveRef, adopted, artifacts, nextAttempt, lenAtAdopt, badDeleteEver, leaders>>

WriterRemove(b) ==
  /\ Len(journal) < MaxJournal
  /\ liveRef[b]
  /\ ~unresolved[b]
  /\ journal' = Append(journal, <<b, "rm">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = FALSE]
  /\ UNCHANGED <<phys, nextTok, unresolved, adopted, artifacts, nextAttempt, lenAtAdopt, meta,
                 badDeleteEver, leaders>>

(* -------- Settlement per spec §3 (pc = prior condemned row, d = merged net in-degree at the new
   cut, tokNow = head-observed token for a fresh condemn, newRound = the folding round). -------- *)
Settle(pc, d, tokNow, newRound) ==
  IF pc.st = "pend"
    THEN NoRow                                     \* d=0: redelete; d>0: structurally-impossible spare
  ELSE IF d > 0
    THEN NoRow                                     \* spared (recovery wins even past the pacing gate)
  ELSE IF pc.st = "cond"
    THEN IF pc.round < newRound \/ Sabotage = "no_pacing"
           THEN [st |-> "pend", tok |-> pc.tok, round |-> pc.round]   \* graduated
           ELSE pc                                                    \* carried (rule 4)
  ELSE IF tokNow > 0                               \* fresh condemn (head observation, write-only)
    THEN [st |-> IF Sabotage = "no_pacing" THEN "pend" ELSE "cond",   \* graduate-at-birth = no pacing
          tok |-> tokNow, round |-> newRound]
  ELSE NoRow                                       \* absent-at-condemn: plain zero-marker, no row

(* -------- GC leader: split into capture / execute (pre-CAS side effects) / adopt (CAS). -------- *)

(* Snapshot `adopted` at a chosen cut >= the coverage floor, claim an attempt key from lease.seq.
   Bounded by nextAttempt <= MaxRound + 1 (see BOUNDS in the header). *)
LeaderCapture(L) ==
  /\ leaders[L].phase = "idle"
  /\ adopted.round < MaxRound
  /\ nextAttempt <= MaxRound + 1
  /\ \E cut \in lenAtAdopt..Len(journal) :
       LET key == IF Sabotage = "attempt_reuse" /\ nextAttempt > 1
                    THEN nextAttempt - 1 ELSE nextAttempt
       IN leaders' = [leaders EXCEPT ![L] = [phase |-> "captured", snap |-> adopted, cut |-> cut,
                                             attempt |-> key, art |-> InitAdopted]]
  /\ nextAttempt' = nextAttempt + 1
  /\ UNCHANGED <<journal, phys, nextTok, liveRef, unresolved, adopted, artifacts, lenAtAdopt, meta,
                 badDeleteEver>>

(* Execute the round's PRE-CAS side effects against the CURRENT physical state, then seal the run
   artifact. Redelete keys off the DURABLE captured condemn-time token (delTok); advisory meta is
   ADD-ONLY (condemn -> "cond"; confirmed delete -> "clean"; spare -> UNCHANGED) except the sabotage
   gc_clear_on_spare, which clears a spare to "clean" pre-CAS (the OLD behavior). *)
LeaderExecute(L) ==
  /\ leaders[L].phase = "captured"
  /\ LET Lr       == leaders[L]
         newEdges == [b \in Blobs |-> Lr.snap.edges[b] + Delta(b, Lr.snap.cut, Lr.cut)]
         newCond  == [b \in Blobs |-> Settle(Lr.snap.cond[b], newEdges[b], phys[b], Lr.snap.round + 1)]
         delTok(b)  == IF Sabotage = "inmem_token" THEN phys[b] ELSE Lr.snap.cond[b].tok
         delExec(b) == Lr.snap.cond[b].st = "pend" /\ newEdges[b] = 0 /\ phys[b] = delTok(b)
         isSpare(b) == Lr.snap.cond[b].st \in {"cond", "pend"} /\ newCond[b].st = "none"
                         /\ newEdges[b] > 0
         art == [gen |-> Lr.snap.gen + 1, cut |-> Lr.cut, edges |-> newEdges, cond |-> newCond,
                 round |-> Lr.snap.round + 1]
     IN /\ phys' = [b \in Blobs |-> IF delExec(b) THEN 0 ELSE phys[b]]
        /\ badDeleteEver' = (badDeleteEver
                             \/ \E b \in Blobs : delExec(b) /\ phys[b] /= Lr.snap.cond[b].tok)
        /\ meta' = [b \in Blobs |->
                      IF newCond[b].st \in {"cond", "pend"} THEN "cond"          \* condemn/pending
                      ELSE IF delExec(b) THEN "clean"                             \* body confirmed gone
                      ELSE IF Sabotage = "gc_clear_on_spare" /\ isSpare(b) THEN "clean"  \* OLD bug
                      ELSE meta[b]]                                               \* spare -> unchanged
        /\ leaders' = [leaders EXCEPT ![L] = [Lr EXCEPT !.phase = "executed", !.art = art]]
  /\ UNCHANGED <<journal, nextTok, liveRef, unresolved, adopted, artifacts, nextAttempt, lenAtAdopt>>

(* The single gc/state CAS. WIN iff no adoption intervened since capture (adopted.gen unchanged) and
   the putIfAbsent is byte-equal-or-fresh; then adopt the sealed artifact, advance the coverage
   floor, and (only under post_adoption_clear = Fix 1) clear the adopted spares' meta to "clean"
   POST-CAS. DEPOSED otherwise (== FoldAbort): adopted/artifacts/lenAtAdopt do NOT move — the
   execute-time deletes and add-only meta already persisted and are proven harmless. The
   attempt_reuse sabotage silently adopts the stale run bytes at the reused key. *)
LeaderAdopt(L) ==
  /\ leaders[L].phase = "executed"
  /\ LET Lr       == leaders[L]
         collide  == Lr.attempt \in DOMAIN artifacts
         byteBad  == collide /\ Sabotage /= "attempt_reuse" /\ artifacts[Lr.attempt] /= Lr.art
         adopt    == (adopted.gen = Lr.snap.gen) /\ ~byteBad
         durable  == IF collide /\ Sabotage = "attempt_reuse"
                       THEN [Lr.art EXCEPT !.edges = artifacts[Lr.attempt].edges,
                                           !.cond = artifacts[Lr.attempt].cond]
                       ELSE Lr.art
         isSpare(b) == Lr.snap.cond[b].st \in {"cond", "pend"} /\ Lr.art.cond[b].st = "none"
                         /\ Lr.art.edges[b] > 0
     IN /\ IF adopt
           THEN /\ artifacts' = IF collide THEN artifacts ELSE artifacts @@ (Lr.attempt :> Lr.art)
                /\ adopted' = durable
                /\ lenAtAdopt' = Len(journal)
                /\ meta' = [b \in Blobs |->
                              IF Sabotage = "post_adoption_clear" /\ isSpare(b)
                                THEN "clean" ELSE meta[b]]
           ELSE /\ UNCHANGED <<artifacts, adopted, lenAtAdopt, meta>>   \* DEPOSED (FoldAbort)
        /\ leaders' = [leaders EXCEPT ![L] = [Lr EXCEPT !.phase = "idle"]]
  /\ UNCHANGED <<journal, phys, nextTok, liveRef, unresolved, nextAttempt, badDeleteEver>>

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
  \/ \E b \in Blobs : EdgeAdd(b) \/ BodyResolve(b) \/ WriterRemove(b)
  \/ \E L \in Leaders : LeaderCapture(L) \/ LeaderExecute(L) \/ LeaderAdopt(L)
  \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ Len(journal) <= MaxJournal
  /\ \A i \in 1..Len(journal) : journal[i][1] \in Blobs /\ journal[i][2] \in {"add", "rm"}
  /\ \A b \in Blobs : phys[b] \in 0..MaxToken /\ nextTok[b] \in 1..(MaxToken + 1)
                      /\ phys[b] < nextTok[b]
  /\ \A b \in Blobs : liveRef[b] \in BOOLEAN /\ unresolved[b] \in BOOLEAN
                      /\ meta[b] \in {"clean", "cond"}
  /\ adopted.round \in 0..MaxRound
  /\ adopted.cut \in 0..MaxJournal /\ adopted.cut <= Len(journal)
  /\ \A b \in Blobs : adopted.edges[b] \in 0..MaxJournal
  /\ \A b \in Blobs : adopted.cond[b].st \in {"none", "cond", "pend"}
  /\ lenAtAdopt <= Len(journal)
  /\ badDeleteEver \in BOOLEAN
  /\ \A L \in Leaders : leaders[L].phase \in {"idle", "captured", "executed"}

(* INV_NO_LOSS: a blob the writer holds a RESOLVED live journalled reference to is never physically
   absent once its edge is inside the adopted coverage (folded live edge => present). Unresolved
   (deferred dedup) refs are excluded: the body may be transiently absent mid-observe and the writer
   re-materializes from source on resolve (the safe direction). *)
INV_NO_LOSS ==
  \A b \in Blobs :
    (liveRef[b] /\ ~unresolved[b] /\ EdgeCount(b, adopted.cut) > 0) => phys[b] > 0

(* INV_NO_RETURN: a delete's token always equals the durably adopted condemn-time token. *)
INV_NO_RETURN == ~badDeleteEver

(* INV_COVERAGE: coverage-coherence — the adopted edge state equals journal truth at the cut. *)
INV_COVERAGE ==
  \A b \in Blobs : adopted.edges[b] = EdgeCount(b, adopted.cut)

(* INV_ONE_PASS: one-pass adoption — the adopted artifact is always one stored attempt artifact. *)
INV_ONE_PASS ==
  adopted.round = 0 \/ \E a \in DOMAIN artifacts : artifacts[a] = adopted

THEOREM Spec => [](TypeOK /\ INV_NO_LOSS /\ INV_NO_RETURN /\ INV_COVERAGE /\ INV_ONE_PASS)
===============================================================================
