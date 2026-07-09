---------------------------- MODULE CaMetaDescriptor ----------------------------
(* Gate B of spec 2026-07-09-cas-writer-gc-simplification (Phase B).              *)
(* The per-hash META-DESCRIPTOR as the lifecycle linearization point:             *)
(*   meta = absent | {gen, inc, condemned}     (gen models the backend etag; any  *)
(*                                              write bumps it via next_gen)      *)
(*   body = absent | {tok}                     (tok = the incarnation token)      *)
(* INV-META-BODY: meta present => body present. Create bottom-up (body, then meta *)
(* If-None-Match); delete top-down (deleteExact meta at the CAPTURED etag, then   *)
(* deleteExact body at the CONDEMN-TIME token — consult C2).                      *)
(*                                                                                *)
(* Universe: ONE hash, ONE writer build. A SEED action creates the pre-existing   *)
(* unowned incarnation (a prior part whose ref was dropped) — the condemnable     *)
(* prey. GC decisions (condemn/graduate) happen only at folded in-degree 0        *)
(* (pre-edge; Gate A owns fold timing); delete EXECUTIONS are unguarded — the     *)
(* adversary may run them after the writer arrived (the C1 window).               *)
(* Resurrect is TWO steps (meta CAS, then body re-upload) so the crash window is  *)
(* explorable. The no-claim sweep sabotage is TWO steps (observe, then blind      *)
(* delete) so the birth-completion race is explorable.                            *)
EXTENDS Naturals

CONSTANTS
  MaxGen,                 \* bound on generations (etags / incarnation tokens / one allocator)
  SabBirthAdopt,          \* (f) birth-completion ADOPTS the orphan body instead of resurrect-from-source
  SabFreshHeadDelete,     \* (g) GC deletes the body at whatever token it currently has, not condemn-time
  SabMetaBeforeBody,      \* (a) create order flipped: meta lands, body write is lost (crash window)
  SabBodyBeforeMeta,      \* (b) delete order flipped: the body is deleted before the meta
  SabBlindAdopt,          \* (c) adopt ignores the meta's condemned flag
  SabNoClaimSweep,        \* (e) debris sweep deletes the body without first claiming the meta
  SabUncondBodyDelete     \* (d) GC proceeds to the body delete even after LOSING the meta delete CAS

NoMeta == [present |-> FALSE, gen |-> 0, inc |-> 0, condemned |-> FALSE]
NoBody == [present |-> FALSE, tok |-> 0]
NoEntry == [present |-> FALSE, etag |-> 0, btok |-> 0, phase |-> "none"]

VARIABLES
  meta, body,
  next_gen,      \* fresh-generation allocator
  seeded,        \* the pre-existing incarnation was created (once)
  precommitted,  \* the writer's durable closure names the hash (EDGE-BEFORE-OBSERVE: precedes acquisition)
  res_pending,   \* resurrect step 1 (meta CAS) done, body re-upload not yet
  acquired,      \* the writer finished acquiring the hash
  committed,     \* promote succeeded
  gc_entry,      \* GC ledger: {etag (captured at condemn), btok (condemn-time body token), phase}
  sweep_armed,   \* (e) the blind sweep observed meta-absent and is about to delete the body
  debris_used    \* the one crashed pre-meta birth happened

vars == <<meta, body, next_gen, seeded, precommitted, res_pending, acquired, committed, gc_entry, sweep_armed, debris_used>>

Init ==
  /\ meta = NoMeta /\ body = NoBody
  /\ next_gen = 1 /\ seeded = FALSE
  /\ precommitted = FALSE /\ res_pending = FALSE /\ acquired = FALSE /\ committed = FALSE
  /\ gc_entry = NoEntry /\ sweep_armed = FALSE /\ debris_used = FALSE

Fresh == next_gen < MaxGen

(* Folded in-degree: the writer's durable closure (single writer). *)
Deg == IF precommitted \/ committed THEN 1 ELSE 0

------------------------------------------------------------------------------
(* The pre-existing incarnation: a prior committed part published it, its ref was dropped — body+meta
   exist, clean, unowned. *)
Seed ==
  /\ ~seeded /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> next_gen, condemned |-> FALSE]
  /\ next_gen' = next_gen + 1 /\ seeded' = TRUE
  /\ UNCHANGED <<precommitted, res_pending, acquired, committed, gc_entry, sweep_armed, debris_used>>

(* A crashed pre-meta birth (NOT a sabotage — the real crash window of create-bottom-up): some prior
   writer PUT the body and died before the meta. The debris the sweep and birth-completion contend for. *)
CrashedBirth ==
  /\ ~seeded /\ ~debris_used /\ Fresh
  /\ ~meta.present /\ ~body.present
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1 /\ debris_used' = TRUE /\ seeded' = TRUE
  /\ UNCHANGED <<meta, precommitted, res_pending, acquired, committed, gc_entry, sweep_armed>>

------------------------------------------------------------------------------
(* Writer *)

Precommit ==
  /\ ~precommitted
  /\ precommitted' = TRUE
  /\ UNCHANGED <<meta, body, next_gen, seeded, res_pending, acquired, committed, gc_entry, sweep_armed, debris_used>>

(* Fresh upload: nothing exists. Bottom-up: body THEN meta (one atomic step here — the crash-window
   sabotage (a) flips the order and loses the body write). *)
FreshUpload ==
  /\ precommitted /\ ~acquired /\ ~res_pending /\ Fresh
  /\ ~meta.present /\ ~body.present
  /\ IF SabMetaBeforeBody
     THEN /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> next_gen, condemned |-> FALSE]
          /\ body' = body
     ELSE /\ body' = [present |-> TRUE, tok |-> next_gen]
          /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> next_gen, condemned |-> FALSE]
  /\ next_gen' = next_gen + 1 /\ acquired' = TRUE
  /\ UNCHANGED <<seeded, precommitted, res_pending, committed, gc_entry, sweep_armed, debris_used>>

(* Adopt via the meta point-read: present + clean => reference the body directly (1 GET, no body op).
   (c) ignores the condemned flag. *)
AdoptClean ==
  /\ precommitted /\ ~acquired /\ ~res_pending
  /\ meta.present /\ (SabBlindAdopt \/ ~meta.condemned)
  /\ acquired' = TRUE
  /\ UNCHANGED <<meta, body, next_gen, seeded, precommitted, res_pending, committed, gc_entry, sweep_armed, debris_used>>

(* Resurrect step 1: CAS the CONDEMNED meta (If-Match its current gen) to clean + fresh incarnation.
   The single linearization point against GC's meta delete. *)
ResurrectCas ==
  /\ precommitted /\ ~acquired /\ ~res_pending /\ Fresh
  /\ meta.present /\ meta.condemned
  /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> next_gen, condemned |-> FALSE]
  /\ next_gen' = next_gen + 1 /\ res_pending' = TRUE
  /\ UNCHANGED <<body, seeded, precommitted, acquired, committed, gc_entry, sweep_armed, debris_used>>

(* Resurrect step 2: re-upload the body from the writer's OWN bytes (fresh token). The gap between the
   steps is the crash window: the OLD body (content-identical) still sits at its OLD token. *)
ResurrectBody ==
  /\ res_pending /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1
  /\ res_pending' = FALSE /\ acquired' = TRUE
  /\ UNCHANGED <<meta, seeded, precommitted, committed, gc_entry, sweep_armed, debris_used>>

(* Birth-completion (consult C1): meta ABSENT + body PRESENT — either a crashed pre-meta birth or GC's
   transient meta-deleted/body-pending window. CORRECT: displace the body from the writer's OWN bytes
   (putOverwrite at the observed token -> fresh token), THEN PUT meta If-None-Match; GC's pending body
   delete (condemn-time token) then MISSES. (f) adopts the orphan as-is instead. *)
BirthCompletion ==
  /\ precommitted /\ ~acquired /\ ~res_pending /\ Fresh
  /\ ~meta.present /\ body.present
  /\ IF SabBirthAdopt
     THEN /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> next_gen, condemned |-> FALSE]
          /\ body' = body
     ELSE /\ body' = [present |-> TRUE, tok |-> next_gen]
          /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> next_gen, condemned |-> FALSE]
  /\ next_gen' = next_gen + 1 /\ acquired' = TRUE
  /\ UNCHANGED <<seeded, precommitted, res_pending, committed, gc_entry, sweep_armed, debris_used>>

Promote ==
  /\ precommitted /\ acquired /\ ~committed
  /\ committed' = TRUE
  /\ UNCHANGED <<meta, body, next_gen, seeded, precommitted, res_pending, acquired, gc_entry, sweep_armed, debris_used>>

------------------------------------------------------------------------------
(* GC. Decisions (condemn, graduate) require folded in-degree 0 — they model folds sealed BEFORE the
   writer's edge (Gate A owns that timing). Delete EXECUTIONS are unguarded: the deciding fold may
   predate the edge, and the execution may land after the writer arrived — the C1 window. *)

(* Condemn: capture the meta etag (post-CAS gen) + the condemn-time body token into the ledger. *)
GcCondemn ==
  /\ Deg = 0 /\ Fresh
  /\ meta.present /\ ~meta.condemned /\ body.present
  /\ ~gc_entry.present
  /\ meta' = [present |-> TRUE, gen |-> next_gen, inc |-> meta.inc, condemned |-> TRUE]
  /\ gc_entry' = [present |-> TRUE, etag |-> next_gen, btok |-> body.tok, phase |-> "cond"]
  /\ next_gen' = next_gen + 1
  /\ UNCHANGED <<body, seeded, precommitted, res_pending, acquired, committed, sweep_armed, debris_used>>

GcGraduate ==
  /\ gc_entry.present /\ gc_entry.phase = "cond" /\ Deg = 0
  /\ gc_entry' = [gc_entry EXCEPT !.phase = "pend"]
  /\ UNCHANGED <<meta, body, next_gen, seeded, precommitted, res_pending, acquired, committed, sweep_armed, debris_used>>

(* Spare: the fold sees d > 0 — drop the entry; CAS the meta clean iff it is still the condemned
   generation the entry named (a resurrect already displaced it otherwise). *)
GcSpare ==
  /\ gc_entry.present /\ Deg > 0
  /\ meta' = IF meta.present /\ meta.condemned /\ meta.gen = gc_entry.etag
             THEN [meta EXCEPT !.condemned = FALSE]
             ELSE meta
  /\ gc_entry' = NoEntry
  /\ UNCHANGED <<body, next_gen, seeded, precommitted, res_pending, acquired, committed, sweep_armed, debris_used>>

(* Delete phase 1: deleteExact(meta, captured etag). Wins iff the meta is unchanged since condemn.
   (b) deletes the body first instead. (d) proceeds to the body even after losing. *)
GcDeleteMeta ==
  /\ gc_entry.present /\ gc_entry.phase = "pend"
  /\ IF SabBodyBeforeMeta
     THEN /\ body' = IF body.present /\ body.tok = gc_entry.btok THEN NoBody ELSE body
          /\ meta' = meta
          /\ gc_entry' = [gc_entry EXCEPT !.phase = "metadel"]
     ELSE IF meta.present /\ meta.gen = gc_entry.etag
          THEN /\ meta' = NoMeta
               /\ body' = body
               /\ gc_entry' = [gc_entry EXCEPT !.phase = "metadel"]
          ELSE /\ meta' = meta
               /\ body' = body
               /\ gc_entry' = IF SabUncondBodyDelete
                              THEN [gc_entry EXCEPT !.phase = "metadel"]
                              ELSE NoEntry                       \* lost the CAS: abort the delete
  /\ UNCHANGED <<next_gen, seeded, precommitted, res_pending, acquired, committed, sweep_armed, debris_used>>

(* Delete phase 2: deleteExact(body, condemn-time token). (g) deletes at whatever token is current. *)
GcDeleteBody ==
  /\ gc_entry.present /\ gc_entry.phase = "metadel"
  /\ body' = IF body.present /\ (SabFreshHeadDelete \/ body.tok = gc_entry.btok)
             THEN NoBody ELSE body
  /\ gc_entry' = NoEntry
  /\ UNCHANGED <<meta, next_gen, seeded, precommitted, res_pending, acquired, committed, sweep_armed, debris_used>>

(* Debris sweep, CORRECT: one atomic claim — enabled only while the meta is ABSENT (the tombstone
   If-None-Match claim), removes the unaccounted body. A meta landing first disables it. Guarded to
   unowned state (a debris body has no edge). *)
SweepClaimed ==
  /\ ~SabNoClaimSweep
  /\ body.present /\ ~meta.present /\ Deg = 0 /\ ~gc_entry.present /\ ~res_pending
  /\ body' = NoBody
  /\ UNCHANGED <<meta, next_gen, seeded, precommitted, res_pending, acquired, committed, gc_entry, sweep_armed, debris_used>>

(* Debris sweep, sabotage (e): observe meta-absent (arm), then blind-delete the body EVEN IF a
   birth-completion landed a meta in between. *)
SweepArm ==
  /\ SabNoClaimSweep /\ ~sweep_armed
  /\ body.present /\ ~meta.present /\ Deg = 0 /\ ~gc_entry.present
  /\ sweep_armed' = TRUE
  /\ UNCHANGED <<meta, body, next_gen, seeded, precommitted, res_pending, acquired, committed, gc_entry, debris_used>>

SweepFire ==
  /\ sweep_armed
  /\ body' = NoBody /\ sweep_armed' = FALSE
  /\ UNCHANGED <<meta, next_gen, seeded, precommitted, res_pending, acquired, committed, gc_entry, debris_used>>

------------------------------------------------------------------------------
Next ==
  \/ Seed \/ CrashedBirth \/ Precommit
  \/ FreshUpload \/ AdoptClean \/ ResurrectCas \/ ResurrectBody \/ BirthCompletion \/ Promote
  \/ GcCondemn \/ GcGraduate \/ GcSpare \/ GcDeleteMeta \/ GcDeleteBody
  \/ SweepClaimed \/ SweepArm \/ SweepFire

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ meta.present \in BOOLEAN /\ body.present \in BOOLEAN
  /\ next_gen \in Nat
  /\ gc_entry.phase \in {"none", "cond", "pend", "metadel"}

(* A committed ref must stand over a present body. *)
INV_NO_DANGLE == committed => body.present

(* INV-META-BODY: a present meta implies a present body — the 1-GET adopt's entire justification.
   The resurrect crash window keeps the OLD body present, so it does not violate this. *)
INV_META_BODY == meta.present => body.present

====================================================================================
