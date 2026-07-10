-------------------------- MODULE CaMetaIncarnationKey --------------------------
(* Option B (user 2026-07-10, "вернуть prefix"): PER-INCARNATION body keys.                              *)
(*   Body key = blobs/xx/<hash>.<incarnation> (raw, immutable, write-once per incarnation).              *)
(*   Meta     = blobs/xx/<hash>.meta = {incarnation (current), state ∈ {clean, condemned}} — NO tombstone.*)
(*   Manifest records (hash, incarnation) so reads locate the exact body key without touching the meta.  *)
(*                                                                                                        *)
(* The point of the model: prove Option B removes BOTH the terminal-tombstone AND the wait, with NO      *)
(* dangle. GC deletes the CONDEMNED incarnation's body by its exact key; a resurrect writes a DIFFERENT  *)
(* (fresh) incarnation key, so GC's body delete can never hit the writer's live body — no cross-object   *)
(* atomicity, no tombstone, no wait. The sabotage SabResurrectReuseIncarnation makes resurrect REUSE the *)
(* condemned incarnation (no fresh key) — that reintroduces the shared-key race and MUST dangle, proving *)
(* the fresh-incarnation-on-resurrect is load-bearing.                                                    *)
(*                                                                                                        *)
(* Universe: ONE content hash, TWO writers, one GC. `bodies` = the SET of incarnation ids whose body     *)
(* object currently exists. `metaInc` = the incarnation the meta currently points to (0 = meta absent).  *)
(* `refInc[w]` = the incarnation writer w committed a ref to (0 = no ref).                                *)
EXTENDS Naturals

CONSTANTS
  MaxInc,                        \* bound on incarnation ids
  Writers,
  SabResurrectReuseIncarnation   \* resurrect reuses the condemned incarnation instead of a fresh one

VARIABLES
  bodies,          \* SUBSET of incarnation ids: which body objects currently exist
  metaState,       \* "absent" | "clean" | "condemned"
  metaInc,         \* the incarnation the meta points to (0 = absent)
  metaGen,         \* meta etag (fresh per write; 0 = absent)
  nextId,          \* fresh id allocator (serves both incarnations and etags)
  refInc,          \* [Writers -> Nat] : incarnation each writer's committed ref names (0 = none)
  seeded

vars == <<bodies, metaState, metaInc, metaGen, nextId, refInc, seeded>>

Init ==
  /\ bodies = {}
  /\ metaState = "absent" /\ metaInc = 0 /\ metaGen = 0
  /\ nextId = 1
  /\ refInc = [w \in Writers |-> 0]
  /\ seeded = FALSE

Fresh == nextId < MaxInc

------------------------------------------------------------------------------
(* Pre-existing clean incarnation, ref since dropped (GC prey). Only from the empty initial state. *)
Seed ==
  /\ ~seeded /\ Fresh
  /\ metaState = "absent" /\ bodies = {}
  /\ bodies' = {nextId}
  /\ metaState' = "clean" /\ metaInc' = nextId /\ metaGen' = nextId + 1
  /\ nextId' = nextId + 2
  /\ seeded' = TRUE
  /\ UNCHANGED refInc

------------------------------------------------------------------------------
(* Fresh upload: nothing exists. Bottom-up: body (fresh incarnation) THEN meta If-None-Match. *)
FreshUpload(w) ==
  /\ refInc[w] = 0 /\ Fresh
  /\ metaState = "absent"
  /\ LET inc == nextId IN
       /\ bodies' = bodies \cup {inc}
       /\ metaState' = "clean" /\ metaInc' = inc /\ metaGen' = inc + 1
       /\ nextId' = nextId + 2
       /\ refInc' = [refInc EXCEPT ![w] = inc]
  /\ UNCHANGED seeded

(* Adopt: meta clean => reference the CURRENT incarnation directly (INV-META-BODY => its body present). *)
Adopt(w) ==
  /\ refInc[w] = 0 /\ metaState = "clean"
  /\ refInc' = [refInc EXCEPT ![w] = metaInc]
  /\ UNCHANGED <<bodies, metaState, metaInc, metaGen, nextId, seeded>>

(* Resurrect: meta condemned. CORRECT: write a FRESH incarnation body (from source), then CAS the meta
   condemned->clean pointing at the fresh incarnation. GC's delete of the condemned incarnation cannot
   touch this new key. SABOTAGE: reuse the condemned incarnation (no fresh body) -> shared-key race. *)
Resurrect(w) ==
  /\ refInc[w] = 0 /\ metaState = "condemned" /\ Fresh
  /\ IF SabResurrectReuseIncarnation
     THEN \* reuse the condemned incarnation (the bug): ref the SAME body GC is about to delete
          /\ metaState' = "clean" /\ metaInc' = metaInc /\ metaGen' = nextId
          /\ nextId' = nextId + 1
          /\ refInc' = [refInc EXCEPT ![w] = metaInc]
          /\ UNCHANGED bodies
     ELSE \* fresh incarnation: distinct key, safe
          /\ LET inc == nextId IN
               /\ bodies' = bodies \cup {inc}
               /\ metaState' = "clean" /\ metaInc' = inc /\ metaGen' = inc + 1
               /\ nextId' = nextId + 2
               /\ refInc' = [refInc EXCEPT ![w] = inc]
  /\ UNCHANGED seeded

(* Birth-completion: meta absent but a body exists that a writer can re-establish from source. Here the
   writer writes a FRESH incarnation and creates the meta (never adopts an orphan incarnation). *)
BirthCompletion(w) ==
  /\ refInc[w] = 0 /\ Fresh
  /\ metaState = "absent" /\ bodies # {}
  /\ LET inc == nextId IN
       /\ bodies' = bodies \cup {inc}
       /\ metaState' = "clean" /\ metaInc' = inc /\ metaGen' = inc + 1
       /\ nextId' = nextId + 2
       /\ refInc' = [refInc EXCEPT ![w] = inc]
  /\ UNCHANGED seeded

DropRef(w) ==
  /\ refInc[w] # 0
  /\ refInc' = [refInc EXCEPT ![w] = 0]
  /\ UNCHANGED <<bodies, metaState, metaInc, metaGen, nextId, seeded>>

------------------------------------------------------------------------------
NoRef == \A w \in Writers : refInc[w] = 0

GcCondemn ==
  /\ NoRef /\ metaState = "clean" /\ metaInc \in bodies /\ Fresh
  /\ metaState' = "condemned" /\ metaGen' = nextId /\ nextId' = nextId + 1
  /\ UNCHANGED <<bodies, metaInc, refInc, seeded>>

GcSpare ==
  /\ metaState = "condemned" /\ (\E w \in Writers : refInc[w] # 0) /\ Fresh
  /\ metaState' = "clean" /\ metaGen' = nextId /\ nextId' = nextId + 1
  /\ UNCHANGED <<bodies, metaInc, refInc, seeded>>

(* GC delete of a condemned incarnation: delete the CONDEMNED incarnation's body by its exact key, then
   clear the meta (deleteExact on the condemned etag). No tombstone. The body delete targets metaInc (the
   incarnation named by the condemned meta) — captured at condemn; here we model deleting exactly that id. *)
GcDeleteBody ==
  /\ NoRef /\ metaState = "condemned" /\ metaInc \in bodies
  /\ bodies' = bodies \ {metaInc}
  /\ UNCHANGED <<metaState, metaInc, metaGen, nextId, refInc, seeded>>

GcDeleteMeta ==
  /\ NoRef /\ metaState = "condemned" /\ metaInc \notin bodies
  /\ metaState' = "absent" /\ metaInc' = 0 /\ metaGen' = 0
  /\ UNCHANGED <<bodies, nextId, refInc, seeded>>

------------------------------------------------------------------------------
Next ==
  \/ Seed
  \/ \E w \in Writers : FreshUpload(w) \/ Adopt(w) \/ Resurrect(w) \/ BirthCompletion(w) \/ DropRef(w)
  \/ GcCondemn \/ GcSpare \/ GcDeleteBody \/ GcDeleteMeta

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ bodies \subseteq (0..MaxInc)
  /\ metaState \in {"absent","clean","condemned"}
  /\ metaInc \in Nat /\ metaGen \in Nat /\ nextId \in Nat
  /\ refInc \in [Writers -> Nat]

(* No dangling committed ref: every writer's referenced incarnation body still exists. *)
INV_NO_DANGLE == \A w \in Writers : (refInc[w] # 0) => (refInc[w] \in bodies)

(* A CLEAN meta points to a present body — the exact-incarnation 1-GET-adopt / read justification. The
   `condemned` state is the mid-delete window (GC may have already deleted the condemned incarnation's
   body); no writer adopts a condemned meta (adopt is clean-only) and resurrect writes a FRESH incarnation
   without reading the condemned body, so `condemned` need not imply body presence. *)
INV_META_BODY == (metaState = "clean") => (metaInc \in bodies)

==================================================================================
