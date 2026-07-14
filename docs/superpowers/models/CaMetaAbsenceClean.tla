-------------------------- MODULE CaMetaAbsenceClean --------------------------
(* §5 gate of spec 2026-07-13-cas-memory-s3-budget-optimizations-design           *)
(* ("Blob meta: absence means Clean"). This is NOT the pre-§5 CaMetaDescriptor     *)
(* envelope (which wrote a Clean meta on create and CAS-ed a condemned meta back    *)
(* to Clean on resurrect) and NOT the rejected raw-body variant. Here `.meta` is a  *)
(* PURE TOMBSTONE:                                                                  *)
(*                                                                                  *)
(*   metaState = "absent"     -- NO meta object exists; the blob is Clean by        *)
(*                              definition (absence IS clean; there is no "clean"    *)
(*                              object to write or read).                           *)
(*   metaState = "condemned"  -- a round-stamped tombstone object exists.           *)
(*   metaEtag                 -- the backend etag of the tombstone (0 when absent); *)
(*                              every condemn writes a FRESH etag (next_gen).       *)
(*   body = <<present, tok>>  -- tok is the incarnation token (0 when absent);      *)
(*                              every body PUT gets a FRESH token (next_gen).       *)
(*                                                                                  *)
(* The FIVE transitions of the user-formulated model (spec §5, 2026-07-13),         *)
(* encoded EXACTLY:                                                                 *)
(*   1. Create    = body PUT only (fresh tok); NO meta write, NO meta read.         *)
(*   2. GcCondemn = write the tombstone (metaState -> condemned, fresh etag,        *)
(*                  round-stamped); body untouched.                                 *)
(*   3. GcDelete  = delete the body at the EXACT condemn-time token, THEN delete    *)
(*                  the meta at the EXACT etag; BOTH conditional; body-before-meta.  *)
(*   4. Resurrect = fresh body first (displace to a NEW tok), THEN delete the       *)
(*                  tombstone (If-Match the observed etag).                         *)
(*   5. Spare/Heal= a GC recheck meeting a tombstone with in-degree >= 1 CLEARS     *)
(*                  the tombstone (If-Match) -- spare = clear.                      *)
(*                                                                                  *)
(* CRASH WINDOWS: each ordered step-pair is TWO actions with the intermediate state *)
(* persisted, so TLC explores every interleaving (= every crash point) between the  *)
(* pair, and the invariants are checked in the intermediate states:                 *)
(*   - GcCondemn ; GcScheduleDelete       (tombstone written, delete not scheduled) *)
(*   - GcDeleteBody ; GcDeleteMeta        (body gone, tombstone still present)      *)
(*   - ResurrectBody ; ResurrectClear     (fresh body up, tombstone still present)  *)
(* Every window fails toward "tombstone present while alive" -- safe (at worst one   *)
(* extra resurrect cycle) and self-healing via rule 5. The ONE unsafe direction --  *)
(* "absence (=Clean) while the body is dying" -- is excluded BY CONSTRUCTION here    *)
(* and is exactly what each sabotage flag re-introduces.                            *)
(*                                                                                  *)
(* Universe: ONE hash, ONE writer build. A SEED action creates the pre-existing     *)
(* unowned incarnation (a prior part whose ref was dropped): body present, NO meta  *)
(* (a Clean blob carries no tombstone) -- the condemnable prey. GC DECISIONS         *)
(* (condemn, schedule-delete, spare) fire only at folded in-degree 0 (they model    *)
(* folds sealed BEFORE the writer's edge; Gate A owns fold timing). Delete           *)
(* EXECUTIONS are UNGUARDED: the deciding fold may predate the edge and the delete   *)
(* may land after the writer arrived -- the C1 window that makes a dangle reachable. *)
(* Single-writer-by-construction (rev.6 foundation): while the writer's edge is up   *)
(* (Deg >= 1) no fresh condemn can fire, so the etag observed at a resurrect's       *)
(* first step cannot change before its second -- the metaState="condemned" guard on  *)
(* the tombstone-clear IS the faithful If-Match here.                               *)
EXTENDS Naturals

CONSTANTS
  MaxGen,                  \* bound on generations (fresh etags / incarnation tokens / one allocator)
  SabResurrectMetaFirst,   \* THE excluded direction: resurrect clears the tombstone BEFORE the fresh body
  SabGcDeleteMetaFirst,    \* GC delete flips the order: meta deleted before the body
  SabAdoptOverTombstone,   \* the reuse/adopt point-read ignores the tombstone and adopts a condemned body
  SabCreateReadsMeta       \* the fresh-create path reads meta and clears a met tombstone (born under a stale one)

NoBody == [present |-> FALSE, tok |-> 0]

VARIABLES
  metaState,     \* "absent" | "condemned"  (absence == Clean; there is no "clean" object)
  metaEtag,      \* backend etag of the tombstone (0 when absent)
  body,          \* <<present, tok>>  (tok == incarnation token, 0 when absent)
  queuedDeletes, \* set of pending exact-token/exact-etag deletes: [tok, etag, phase]
  next_gen,      \* fresh-generation allocator (tokens and etags)
  seeded,        \* the pre-existing incarnation was created (once)
  precommitted,  \* the writer's durable closure names the hash (EDGE-BEFORE-OBSERVE: precedes acquisition)
  acquired,      \* the writer finished acquiring the hash (create / adopt / resurrect)
  committed,     \* promote succeeded -- a LIVE reference now stands over the body
  res_pending    \* resurrect first step done, second step not yet (the crash window between them)

vars == <<metaState, metaEtag, body, queuedDeletes, next_gen,
          seeded, precommitted, acquired, committed, res_pending>>

QDElem == [tok : 1..MaxGen, etag : 1..MaxGen, phase : {"pending", "body_done", "meta_done"}]

Init ==
  /\ metaState = "absent" /\ metaEtag = 0
  /\ body = NoBody
  /\ queuedDeletes = {}
  /\ next_gen = 1
  /\ seeded = FALSE
  /\ precommitted = FALSE /\ acquired = FALSE /\ committed = FALSE
  /\ res_pending = FALSE

Fresh == next_gen < MaxGen

(* Folded in-degree: the writer's durable closure (single writer). *)
Deg == IF precommitted \/ committed THEN 1 ELSE 0

(* Conditional-delete predicates (the If-Match guards on the backend). *)
BodyMatches(d) == body.present /\ body.tok = d.tok
MetaMatches(d) == metaState = "condemned" /\ metaEtag = d.etag

------------------------------------------------------------------------------
(* The pre-existing incarnation: a prior committed part published this body, its ref
   was dropped -- a CLEAN blob, so body present + NO meta (absence == clean). Unowned:
   the condemnable prey. *)
Seed ==
  /\ ~seeded /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1
  /\ seeded' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, queuedDeletes, precommitted, acquired, committed, res_pending>>

------------------------------------------------------------------------------
(* Writer. Precommit publishes the durable edge (raises Deg) BEFORE acquisition. *)

Precommit ==
  /\ ~precommitted
  /\ precommitted' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, body, queuedDeletes, next_gen, seeded, acquired, committed, res_pending>>

(* Transition 1 -- Create: fresh create path, body PUT ONLY. No meta write, no meta
   read. Fires when nothing is there to reuse (body absent). The CORRECT path is
   BLIND to the tombstone; SabCreateReadsMeta re-introduces a meta read below. *)
Create ==
  /\ ~SabCreateReadsMeta
  /\ precommitted /\ ~acquired /\ ~res_pending /\ Fresh
  /\ ~body.present
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1
  /\ acquired' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, queuedDeletes, seeded, precommitted, committed, res_pending>>

(* Reuse/adopt point-read on an occupied key: meta ABSENT (=Clean) => reference the
   body directly (1 GET, no body op). This is the adopt whose entire soundness rests
   on INV_ABSENCE_NO_QUEUED_DELETE. *)
AdoptClean ==
  /\ precommitted /\ ~acquired /\ ~res_pending
  /\ metaState = "absent" /\ body.present
  /\ acquired' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, body, queuedDeletes, next_gen, seeded, precommitted, committed, res_pending>>

(* Transition 4 -- Resurrect (CORRECT: fresh body FIRST). The occupied-key point-read
   met a CONDEMNED tombstone: displace the body from the writer's own bytes to a FRESH
   token, so any queued exact-token delete (on the OLD token) now MISSES. *)
ResurrectBody ==
  /\ ~SabResurrectMetaFirst
  /\ precommitted /\ ~acquired /\ ~res_pending /\ Fresh
  /\ metaState = "condemned" /\ body.present
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1
  /\ res_pending' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, queuedDeletes, seeded, precommitted, acquired, committed>>

(* Resurrect step 2: delete the tombstone, If-Match the observed etag (conditional,
   so GC's own re-condemn or a stale round cannot be stomped). Absence lands only
   AFTER the fresh body is up. *)
ResurrectClear ==
  /\ ~SabResurrectMetaFirst
  /\ res_pending
  /\ metaState' = IF metaState = "condemned" THEN "absent" ELSE metaState
  /\ metaEtag'  = IF metaState = "condemned" THEN 0 ELSE metaEtag
  /\ res_pending' = FALSE
  /\ acquired' = TRUE
  /\ UNCHANGED <<body, queuedDeletes, next_gen, seeded, precommitted, committed>>

(* Transition 2 -- GcCondemn: write the tombstone (metaState -> condemned, FRESH etag,
   round-stamped). Body untouched. A GC DECISION: folded in-degree 0. *)
GcCondemn ==
  /\ Deg = 0 /\ Fresh
  /\ metaState = "absent" /\ body.present
  /\ queuedDeletes = {}
  /\ metaState' = "condemned"
  /\ metaEtag' = next_gen
  /\ next_gen' = next_gen + 1
  /\ UNCHANGED <<body, queuedDeletes, seeded, precommitted, acquired, committed, res_pending>>

(* Schedule the delete of a tombstoned blob that stayed condemned across a round at
   in-degree 0 (a LATER round's decision). Capture the EXACT condemn-time body token
   and the EXACT tombstone etag. A GC DECISION: folded in-degree 0. *)
GcScheduleDelete ==
  /\ Deg = 0
  /\ metaState = "condemned" /\ body.present
  /\ queuedDeletes = {}
  /\ queuedDeletes' = {[tok |-> body.tok, etag |-> metaEtag, phase |-> "pending"]}
  /\ UNCHANGED <<metaState, metaEtag, body, next_gen, seeded, precommitted, acquired, committed, res_pending>>

(* Transition 5 -- Spare/Heal: a GC recheck meeting the tombstone with in-degree >= 1
   CLEARS it (If-Match). Drops any scheduled delete for it. *)
GcSpareHeal ==
  /\ Deg >= 1
  /\ metaState = "condemned"
  /\ metaState' = "absent" /\ metaEtag' = 0
  /\ queuedDeletes' = {}
  /\ UNCHANGED <<body, next_gen, seeded, precommitted, acquired, committed, res_pending>>

------------------------------------------------------------------------------
(* Transition 3 -- GcDelete. Delete EXECUTIONS are UNGUARDED (may land after the
   writer arrived). CORRECT order: body first (exact token), THEN meta (exact etag). *)

GcDeleteBody ==
  /\ ~SabGcDeleteMetaFirst
  /\ \E d \in queuedDeletes :
       /\ d.phase = "pending"
       /\ body' = IF BodyMatches(d) THEN NoBody ELSE body
       /\ queuedDeletes' = (queuedDeletes \ {d}) \cup {[d EXCEPT !.phase = "body_done"]}
  /\ UNCHANGED <<metaState, metaEtag, next_gen, seeded, precommitted, acquired, committed, res_pending>>

GcDeleteMeta ==
  /\ \E d \in queuedDeletes :
       /\ d.phase = "body_done"
       /\ metaState' = IF MetaMatches(d) THEN "absent" ELSE metaState
       /\ metaEtag'  = IF MetaMatches(d) THEN 0 ELSE metaEtag
       /\ queuedDeletes' = queuedDeletes \ {d}
  /\ UNCHANGED <<body, next_gen, seeded, precommitted, acquired, committed, res_pending>>

------------------------------------------------------------------------------
(* SABOTAGES. Each re-introduces the one unsafe direction and MUST break either
   INV_ABSENCE_NO_QUEUED_DELETE or INV_NO_DANGLE / INV_NO_LOSS. *)

(* SabResurrectMetaFirst: clear the tombstone BEFORE the fresh body (the excluded
   direction). Absence (=Clean) lands while the OLD body -- still carrying its queued
   exact-token delete -- is dying. *)
SabResurrectClearFirst ==
  /\ SabResurrectMetaFirst
  /\ precommitted /\ ~acquired /\ ~res_pending
  /\ metaState = "condemned" /\ body.present
  /\ metaState' = "absent" /\ metaEtag' = 0
  /\ res_pending' = TRUE
  /\ UNCHANGED <<body, queuedDeletes, next_gen, seeded, precommitted, acquired, committed>>

SabResurrectBodySecond ==
  /\ SabResurrectMetaFirst
  /\ res_pending /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1
  /\ res_pending' = FALSE
  /\ acquired' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, queuedDeletes, seeded, precommitted, committed>>

(* SabGcDeleteMetaFirst: delete the meta BEFORE the body. Absence lands while the body
   (still at the condemn-time token, still queued for exact-token delete) is dying. *)
SabGcDeleteMetaFirstAct ==
  /\ SabGcDeleteMetaFirst
  /\ \E d \in queuedDeletes :
       /\ d.phase = "pending"
       /\ metaState' = IF MetaMatches(d) THEN "absent" ELSE metaState
       /\ metaEtag'  = IF MetaMatches(d) THEN 0 ELSE metaEtag
       /\ queuedDeletes' = (queuedDeletes \ {d}) \cup {[d EXCEPT !.phase = "meta_done"]}
  /\ UNCHANGED <<body, next_gen, seeded, precommitted, acquired, committed, res_pending>>

SabGcDeleteBodyLast ==
  /\ SabGcDeleteMetaFirst
  /\ \E d \in queuedDeletes :
       /\ d.phase = "meta_done"
       /\ body' = IF BodyMatches(d) THEN NoBody ELSE body
       /\ queuedDeletes' = queuedDeletes \ {d}
  /\ UNCHANGED <<metaState, metaEtag, next_gen, seeded, precommitted, acquired, committed, res_pending>>

(* SabAdoptOverTombstone: the reuse point-read ignores the tombstone and adopts the
   condemned body as-is (references it) instead of resurrecting/displacing. A live ref
   now stands over a body with a queued exact-token delete. *)
SabAdoptOverTombstoneAct ==
  /\ SabAdoptOverTombstone
  /\ precommitted /\ ~acquired /\ ~res_pending
  /\ metaState = "condemned" /\ body.present
  /\ acquired' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, body, queuedDeletes, next_gen, seeded, precommitted, committed, res_pending>>

(* SabCreateReadsMeta: the fresh-create path reads meta and, meeting a tombstone,
   CLEARS it (meta-first) as "birth cleanup" -- born under a stale tombstone. Absence
   lands while the old body + its queued delete are still there. *)
SabCreateReadsMetaAct ==
  /\ SabCreateReadsMeta
  /\ precommitted /\ ~acquired /\ ~res_pending
  /\ metaState = "condemned" /\ body.present
  /\ metaState' = "absent" /\ metaEtag' = 0
  /\ UNCHANGED <<body, queuedDeletes, next_gen, seeded, precommitted, acquired, committed, res_pending>>

------------------------------------------------------------------------------
Promote ==
  /\ precommitted /\ acquired /\ ~committed
  /\ committed' = TRUE
  /\ UNCHANGED <<metaState, metaEtag, body, queuedDeletes, next_gen, seeded, precommitted, acquired, res_pending>>

Next ==
  \/ Seed \/ Precommit
  \/ Create \/ AdoptClean \/ ResurrectBody \/ ResurrectClear \/ Promote
  \/ GcCondemn \/ GcScheduleDelete \/ GcSpareHeal \/ GcDeleteBody \/ GcDeleteMeta
  \/ SabResurrectClearFirst \/ SabResurrectBodySecond
  \/ SabGcDeleteMetaFirstAct \/ SabGcDeleteBodyLast
  \/ SabAdoptOverTombstoneAct \/ SabCreateReadsMetaAct

Spec == Init /\ [][Next]_vars

------------------------------------------------------------------------------
TypeOK ==
  /\ metaState \in {"absent", "condemned"}
  /\ metaEtag \in Nat
  /\ body.present \in BOOLEAN /\ body.tok \in Nat
  /\ queuedDeletes \subseteq QDElem
  /\ next_gen \in Nat
  /\ (metaState = "absent") <=> (metaEtag = 0)

(* THE load-bearing invariant (spec §5): "meta absence implies no queued exact-token
   delete on a live token." Absence == Clean, so the adopt/reuse point-read will
   reference this body; there must be no exact-token delete queued against it. (When
   the body is absent, body.tok = 0 while every queued d.tok >= 1, so the RHS holds
   vacuously -- the property is about a LIVE token.) *)
INV_ABSENCE_NO_QUEUED_DELETE ==
  (metaState = "absent") => ~(\E d \in queuedDeletes : d.tok = body.tok)

(* A live/committed reference's body is never queued for an exact-token delete (direct
   dangling prevention -- the analogue named in the brief). *)
INV_NO_DANGLE ==
  committed => ~(\E d \in queuedDeletes : d.tok = body.tok)

(* A committed reference always stands over a present body -- we never lose a live
   body (the pre-§5 model's INV_NO_DANGLE, kept here as "no loss"). *)
INV_NO_LOSS ==
  committed => body.present

(* §5 replaces the pre-§5 "meta present => body present": that no longer holds because
   §5 deletes body-BEFORE-meta, so (tombstone present, body absent) is a real, intended
   crash window (heals via rule 5 at the hash's next birth). The meaningful §5 meta/body
   relationship is on the ABSENCE side: a live ref standing on Clean (absent) meta
   stands on a real, non-condemned, delete-free body -- the adopt-path's soundness. *)
INV_META_BODY ==
  (committed /\ metaState = "absent")
    => (body.present /\ ~(\E d \in queuedDeletes : d.tok = body.tok))

==============================================================================
