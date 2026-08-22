--------------------------- MODULE CaGcCondemnMarkerGate ---------------------------
(* Gate for triage 2026-07-17 §3.4 (codex review №4): the swallowed condemn-marker    *)
(* write vs same-token adopt. This focused GC gate models one fixed content-addressed *)
(* key, so every present body has that key's logical content; incarnation tokens      *)
(* authorize exact deletion but are not part of a committed reference's identity.     *)
(*                                                                                    *)
(* THE HAZARD (pre-fix). `Gc::scheduleMetaJob` swallows every exception from          *)
(* `writeCondemnedMeta`, while the round commits the retired (hash, token) entry      *)
(* regardless. The per-hash meta is the WRITER's adopt gate (absent/Clean => the      *)
(* writer may adopt the CURRENT token without re-upload), so a lost marker lets a     *)
(* writer adopt the exact token a graduated entry later deleteExact-s. EDGE-BEFORE-   *)
(* OBSERVE does not close it: it orders the writer's edge before the writer's OWN     *)
(* observation, not before GC's once-per-fold discovery cut — a writer landing in     *)
(* the [cut, deleteExact] window of the deleting round is invisible to that round's   *)
(* merge. Result: exact-token delete of a body under a live committed edge — a        *)
(* dangling manifest (INV_NO_LOSS).                                                   *)
(*                                                                                    *)
(* THE FIX (verifier option (a), GateOnMarker = TRUE). Graduation to delete_pending   *)
(* — the one edge that authorizes an irreversible delete — requires CONFIRMED         *)
(* durable Condemned evidence: the in-process confirmation recorded when the          *)
(* scheduled marker write succeeded (ret.conf), OR a synchronous loadMeta re-check    *)
(* observing meta = condemned. No evidence => the entry is CARRIED (fail-safe         *)
(* delay) and the marker write is RETRIED (liveness). Everything else stays           *)
(* async/advisory. The disaster-recovery rebuild publishes markers synchronously —    *)
(* in this model that is simply the condemn action's marker-ok branch.                *)
(*                                                                                    *)
(* WHY THE FIX HOLDS (the argument the model checks): a same-token adoption           *)
(* requires observing meta /= condemned, so the adopting writer's edge (durable       *)
(* BEFORE its observation) landed before the meta turned condemned; graduation        *)
(* confirms only at/after that write; and the redelete runs in a LATER round whose    *)
(* cut postdates the confirming round — that cut folds the edge and the entry is      *)
(* SPARED, never deleted.                                                             *)
(*                                                                                    *)
(* SHAPE. One hash, one writer build, single-leader GC. A GC round is TWO actions:    *)
(* GCut (the discovery LIST — folds the edge visibility snapshot) and GSettle (the    *)
(* merge settlement + pre-CAS delete + this round's marker writes, atomic). Writer    *)
(* actions interleave freely between them — the [cut, settle] window IS the C1        *)
(* window that makes the pre-fix dangle reachable. GRestart models a leader change:   *)
(* the in-process confirmation is lost, forcing graduation onto the loadMeta          *)
(* re-check branch. `meta` is {"clean", "condemned"} with absence == clean (the       *)
(* current production semantics: deleteConfirmedMeta drops the meta; no tombstone).   *)
EXTENDS Naturals

CONSTANTS
  GateOnMarker,   \* FALSE = pre-fix graduation (unconditional); TRUE = the fixed gate
  MaxGen,         \* bound on fresh incarnation tokens
  MaxRounds       \* bound on GC rounds

NoTok == 0

VARIABLES
  round,       \* GC round counter (bounds the run)
  cut_taken,   \* between GCut and GSettle of the current round
  folded,      \* the cut's edge-visibility snapshot: was the writer's edge folded at this round's cut
  body,        \* [present |-> BOOLEAN, tok |-> 0..MaxGen] — the one blob body (exact-token deletes)
  meta,        \* "clean" | "condemned" — the per-hash writer adopt gate (absence == clean)
  next_gen,    \* fresh incarnation-token allocator
  seeded,      \* the pre-existing unowned incarnation was created (once)
  edge,        \* "none" | "landed" — the writer's durable edge (EDGE-BEFORE-OBSERVE: precedes adoption)
  adopted,     \* the token the writer acquired (0 = none yet)
  committed,   \* promote succeeded — a LIVE committed reference stands over `adopted`
  ret          \* the retired entry: [st |-> "none"|"condemned"|"pending", tok, cr, conf]

vars == <<round, cut_taken, folded, body, meta, next_gen, seeded, edge, adopted, committed, ret>>

TypeOK ==
  /\ round \in 1..MaxRounds
  /\ cut_taken \in BOOLEAN /\ folded \in BOOLEAN
  /\ body \in [present : BOOLEAN, tok : 0..MaxGen]
  /\ meta \in {"clean", "condemned"}
  /\ next_gen \in 1..(MaxGen + 1)
  /\ seeded \in BOOLEAN
  /\ edge \in {"none", "landed"}
  /\ adopted \in 0..MaxGen
  /\ committed \in BOOLEAN
  /\ ret \in [st : {"none", "condemned", "pending"}, tok : 0..MaxGen, cr : 0..MaxRounds, conf : BOOLEAN]

Init ==
  /\ round = 1 /\ cut_taken = FALSE /\ folded = FALSE
  /\ body = [present |-> FALSE, tok |-> NoTok]
  /\ meta = "clean"
  /\ next_gen = 1
  /\ seeded = FALSE
  /\ edge = "none" /\ adopted = NoTok /\ committed = FALSE
  /\ ret = [st |-> "none", tok |-> NoTok, cr |-> 0, conf |-> FALSE]

Fresh == next_gen <= MaxGen

------------------------------------------------------------------------------------
(* The pre-existing unowned incarnation: a prior part published this body and its ref
   was dropped before the model starts — folded in-degree 0, the condemnable prey. A
   Clean blob carries a clean-reading meta. *)
Seed ==
  /\ ~seeded /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ next_gen' = next_gen + 1
  /\ seeded' = TRUE
  /\ UNCHANGED <<round, cut_taken, folded, meta, edge, adopted, committed, ret>>

------------------------------------------------------------------------------------
(* Writer (one build). The durable edge lands BEFORE any observation (EDGE-BEFORE-
   OBSERVE); acquisition then reads the meta: clean => adopt the CURRENT token
   without re-upload; condemned => resurrect (displace the body with a FRESH token
   and drive the meta back to clean — the sole condemned -> clean transition); body
   absent => create fresh. *)
WPrecommit ==
  /\ seeded /\ edge = "none"
  /\ edge' = "landed"
  /\ UNCHANGED <<round, cut_taken, folded, body, meta, next_gen, seeded, adopted, committed, ret>>

WAdopt ==
  /\ edge = "landed" /\ adopted = NoTok
  /\ body.present /\ meta = "clean"
  /\ adopted' = body.tok
  /\ UNCHANGED <<round, cut_taken, folded, body, meta, next_gen, seeded, edge, committed, ret>>

WResurrect ==
  /\ edge = "landed" /\ adopted = NoTok
  /\ body.present /\ meta = "condemned" /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ meta' = "clean"
  /\ adopted' = next_gen
  /\ next_gen' = next_gen + 1
  /\ UNCHANGED <<round, cut_taken, folded, seeded, edge, committed, ret>>

WCreate ==
  /\ edge = "landed" /\ adopted = NoTok
  /\ ~body.present /\ Fresh
  /\ body' = [present |-> TRUE, tok |-> next_gen]
  /\ meta' = "clean"
  /\ adopted' = next_gen
  /\ next_gen' = next_gen + 1
  /\ UNCHANGED <<round, cut_taken, folded, seeded, edge, committed, ret>>

WCommit ==
  /\ adopted /= NoTok /\ ~committed
  /\ committed' = TRUE
  /\ UNCHANGED <<round, cut_taken, folded, body, meta, next_gen, seeded, edge, adopted, ret>>

------------------------------------------------------------------------------------
(* GC. GCut snapshots the round's edge visibility (the discovery LIST / merge input);
   GSettle settles the retired entry against that snapshot and executes the round's
   pre-CAS delete + marker writes. Writer actions between the two ARE the C1 window. *)
GCut ==
  /\ ~cut_taken /\ round < MaxRounds
  /\ folded' = (edge = "landed")
  /\ cut_taken' = TRUE
  /\ UNCHANGED <<round, body, meta, next_gen, seeded, edge, adopted, committed, ret>>

EndRound == /\ cut_taken' = FALSE
            /\ round' = round + 1

(* Fresh condemn at folded in-degree 0: the entry enters the retired set and the
   marker write is scheduled — it either lands durable (conf) or is SWALLOWED (the
   §3.4 failure; the entry stays unconfirmed). The rebuild's synchronous marker
   publish is the marker-ok branch of this same action. *)
GSettleCondemn ==
  /\ cut_taken /\ ret.st = "none" /\ body.present /\ ~folded
  /\ \/ /\ meta' = "condemned"   \* marker write landed durable
        /\ ret' = [st |-> "condemned", tok |-> body.tok, cr |-> round, conf |-> TRUE]
     \/ /\ meta' = meta          \* marker write SWALLOWED (exception eaten by the pool wrapper)
        /\ ret' = [st |-> "condemned", tok |-> body.tok, cr |-> round, conf |-> FALSE]
  /\ EndRound
  /\ UNCHANGED <<folded, body, next_gen, seeded, edge, adopted, committed>>

GSettleIdle ==
  /\ cut_taken /\ ret.st = "none" /\ (~body.present \/ folded)
  /\ EndRound
  /\ UNCHANGED <<folded, body, meta, next_gen, seeded, edge, adopted, committed, ret>>

(* Folded in-degree recovered before graduation: spared (entry drops, body stays). *)
GSettleSpare ==
  /\ cut_taken /\ ret.st = "condemned" /\ folded
  /\ ret' = [st |-> "none", tok |-> NoTok, cr |-> 0, conf |-> FALSE]
  /\ EndRound
  /\ UNCHANGED <<folded, body, meta, next_gen, seeded, edge, adopted, committed>>

(* Floor-passed graduation attempt. The GATE: pre-fix (GateOnMarker = FALSE) it
   graduates unconditionally; post-fix it requires the in-process confirmation OR a
   loadMeta re-check observing condemned. Refused => CARRY + retry the marker write
   (which may land durable or be swallowed again). *)
GSettleGraduate ==
  /\ cut_taken /\ ret.st = "condemned" /\ ~folded /\ ret.cr < round
  /\ (~GateOnMarker \/ ret.conf \/ meta = "condemned")
  /\ ret' = [ret EXCEPT !.st = "pending", !.conf = TRUE]
  /\ EndRound
  /\ UNCHANGED <<folded, body, meta, next_gen, seeded, edge, adopted, committed>>

GSettleCarryRetry ==
  /\ cut_taken /\ ret.st = "condemned" /\ ~folded /\ ret.cr < round
  /\ GateOnMarker /\ ~ret.conf /\ meta /= "condemned"
  /\ \/ /\ meta' = "condemned"   \* the carry-time retry landed durable
        /\ ret' = [ret EXCEPT !.conf = TRUE]
     \/ /\ meta' = meta          \* the retry was swallowed again — still unconfirmed
        /\ ret' = ret
  /\ EndRound
  /\ UNCHANGED <<folded, body, next_gen, seeded, edge, adopted, committed>>

(* A delete_pending entry whose edge recovered by this round's cut: spared loudly. *)
GSettleSpareLoud ==
  /\ cut_taken /\ ret.st = "pending" /\ folded
  /\ ret' = [st |-> "none", tok |-> NoTok, cr |-> 0, conf |-> FALSE]
  /\ EndRound
  /\ UNCHANGED <<folded, body, meta, next_gen, seeded, edge, adopted, committed>>

(* The redelete: the pre-CAS exact-token delete of a previously-published pending
   entry. UNGUARDED against edges landing after this round's cut — the C1 window. A
   token mismatch (resurrected body) is a no-op; the entry drops either way. An
   actual delete drops the meta alongside (deleteConfirmedMeta; absence == clean). *)
GSettleRedelete ==
  /\ cut_taken /\ ret.st = "pending" /\ ~folded
  /\ IF body.present /\ body.tok = ret.tok
       THEN /\ body' = [present |-> FALSE, tok |-> NoTok]
            /\ meta' = "clean"
       ELSE /\ body' = body
            /\ meta' = meta
  /\ ret' = [st |-> "none", tok |-> NoTok, cr |-> 0, conf |-> FALSE]
  /\ EndRound
  /\ UNCHANGED <<folded, next_gen, seeded, edge, adopted, committed>>

(* Leader change / process restart: the in-process confirmation set is lost (the
   durable meta and the durable pending bit survive). Forces the graduation gate onto
   the loadMeta re-check branch. *)
GRestart ==
  /\ ret.st = "condemned" /\ ret.conf
  /\ ret' = [ret EXCEPT !.conf = FALSE]
  /\ UNCHANGED <<round, cut_taken, folded, body, meta, next_gen, seeded, edge, adopted, committed>>

------------------------------------------------------------------------------------
Next ==
  \/ Seed
  \/ WPrecommit \/ WAdopt \/ WResurrect \/ WCreate \/ WCommit
  \/ GCut
  \/ GSettleCondemn \/ GSettleIdle \/ GSettleSpare
  \/ GSettleGraduate \/ GSettleCarryRetry
  \/ GSettleSpareLoud \/ GSettleRedelete
  \/ GRestart

Spec == Init /\ [][Next]_vars

(* THE invariant: a live committed edge names the key's logical content, not the
   incarnation token observed by the writer. Because this model has one fixed content-addressed
   key, presence is also content identity; a safe equivalent replacement may change body.tok. *)
NoDangle == committed => body.present

=====================================================================================
