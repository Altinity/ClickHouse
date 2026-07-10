-------------------------- MODULE CaMetaDescriptorRaw --------------------------
(* Gate B (re-authored) for spec 2026-07-09-cas-writer-gc-simplification, Phase B FINAL design:      *)
(*   RAW bodies (no envelope/incarnation; etag = content, immutable, write-once) +                   *)
(*   a per-hash META with a THREE-state lifecycle {clean, condemned, tombstone} as the SOLE          *)
(*   linearization point (`gen` = the meta etag = the only conditional token; NO body token).        *)
(*                                                                                                    *)
(* Supersedes CaMetaDescriptor.tla (which kept a body token + exact-token body delete / C2). Here    *)
(* the delete is a TOMBSTONE handshake and resurrect never re-uploads a present body.                *)
(*                                                                                                    *)
(* Universe: ONE content hash, TWO writers (multi-writer races — the recorded caveat), one GC. The   *)
(* body is content-addressed: any upload writes the SAME bytes, so "present" is a single boolean.     *)
(* A pre-existing incarnation (Seed) is the condemnable prey; CrashedBirth models a body-without-meta *)
(* debris. Sabotages each drop one protocol rule and MUST break an invariant.                        *)
EXTENDS Naturals

CONSTANTS
  MaxGen,                    \* bound on meta generations (etags)
  Writers,                   \* set of writer ids (>=2 for the races)
  SabBlindAdopt,             \* adopt ignores condemned/tombstone (reference a doomed body)
  SabAdoptOverTombstone,     \* adopt a body whose meta is tombstone (being deleted) instead of re-establishing
  SabDeleteBodyNoTombstone,  \* GC deletes the body while meta is still condemned (no tombstone claim first)
  SabMetaBeforeBody          \* fresh upload writes meta before body (crash window -> meta w/o body)
\* NOTE: resurrect-skip-CAS and delete-meta-before-body are NOT modeled here — at this abstraction's
\* granularity (GC delete gated on NoRef) neither produces a safety violation (they are liveness/debris
\* concerns); exposing them needs a finer writer↔GC interleaving. Recorded as a Gate-B follow-up.

VARIABLES
  body,          \* BOOLEAN — the raw content body present?
  metaState,     \* "absent" | "clean" | "condemned" | "tombstone"
  metaGen,       \* Nat — the meta etag (0 when absent)
  nextGen,       \* fresh-gen allocator
  seeded, debrisUsed,
  refOf,         \* [Writers -> BOOLEAN] : this writer holds a committed ref naming the hash
  condemnRound   \* bookkeeping: the round the current condemnation used (model-only, for pacing realism)

vars == <<body, metaState, metaGen, nextGen, seeded, debrisUsed, refOf, condemnRound>>

Init ==
  /\ body = FALSE
  /\ metaState = "absent" /\ metaGen = 0
  /\ nextGen = 1 /\ seeded = FALSE /\ debrisUsed = FALSE
  /\ refOf = [w \in Writers |-> FALSE]
  /\ condemnRound = 0

Fresh == nextGen < MaxGen

------------------------------------------------------------------------------
(* Pre-existing clean incarnation: a prior committed part, its ref since dropped. *)
Seed ==
  /\ ~seeded /\ Fresh
  /\ body' = TRUE
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ seeded' = TRUE
  /\ UNCHANGED <<debrisUsed, refOf, condemnRound>>

(* A crashed pre-meta fresh upload: body landed, meta did not (the create-bottom-up crash window). *)
CrashedBirth ==
  /\ ~seeded /\ ~debrisUsed /\ Fresh
  /\ ~body /\ metaState = "absent"
  /\ body' = TRUE /\ debrisUsed' = TRUE /\ seeded' = TRUE
  /\ UNCHANGED <<metaState, metaGen, nextGen, refOf, condemnRound>>

------------------------------------------------------------------------------
(* Writer w acquires the hash and commits a ref. Multiple writers may do so (dedup). *)

\* Fresh upload: nothing exists. Bottom-up: body THEN meta (If-None-Match). Sabotage flips the order.
FreshUpload(w) ==
  /\ ~refOf[w] /\ Fresh
  /\ metaState = "absent" /\ ~body
  /\ IF SabMetaBeforeBody
     THEN /\ metaState' = "clean" /\ metaGen' = nextGen /\ body' = body   \* meta first; body write "lost"
     ELSE /\ body' = TRUE /\ metaState' = "clean" /\ metaGen' = nextGen
  /\ nextGen' = nextGen + 1
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<seeded, debrisUsed, condemnRound>>

\* Adopt: meta clean => reference the body directly (1 GET; body present by INV_META_BODY). Blind/
\* over-tombstone sabotages reference a doomed/being-deleted body.
Adopt(w) ==
  /\ ~refOf[w]
  /\ metaState = "clean" \/ (SabBlindAdopt /\ metaState \in {"condemned","tombstone"})
                         \/ (SabAdoptOverTombstone /\ metaState = "tombstone")
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<body, metaState, metaGen, nextGen, seeded, debrisUsed, condemnRound>>

\* Resurrect: meta condemned => CAS clean(fresh gen). Body is still present (condemn never deletes it),
\* so NO body re-upload (the raw-body simplification). Sabotage references without the CAS.
Resurrect(w) ==
  /\ ~refOf[w] /\ metaState = "condemned" /\ Fresh
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1   \* CAS condemned->clean; body already present
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<body, seeded, debrisUsed, condemnRound>>

\* Birth-completion: meta tombstone or absent while body present (GC's mid-delete window or crashed
\* birth). CORRECT: re-establish — ensure body (re-upload; idempotent, content-addressed) + CAS meta
\* to clean (from tombstone gen) or PUT If-None-Match (from absent). Never adopt the doomed body as-is.
BirthCompletion(w) ==
  /\ ~refOf[w] /\ Fresh
  /\ body /\ metaState \in {"tombstone","absent"}
  /\ body' = TRUE
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<seeded, debrisUsed, condemnRound>>

\* A committed ref is dropped (its owner no longer references the hash) — enables re-condemnation.
DropRef(w) ==
  /\ refOf[w]
  /\ refOf' = [refOf EXCEPT ![w] = FALSE]
  /\ UNCHANGED <<body, metaState, metaGen, nextGen, seeded, debrisUsed, condemnRound>>

------------------------------------------------------------------------------
(* GC. Acts only on a hash no live writer references (folded in-degree 0). *)
NoRef == \A w \in Writers : ~refOf[w]

GcCondemn ==
  /\ NoRef /\ metaState = "clean" /\ body /\ Fresh
  /\ metaState' = "condemned" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ condemnRound' = condemnRound + 1
  /\ UNCHANGED <<body, seeded, debrisUsed, refOf>>

GcSpare ==
  /\ metaState = "condemned" /\ (\E w \in Writers : refOf[w]) /\ Fresh
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ UNCHANGED <<body, seeded, debrisUsed, refOf, condemnRound>>

\* Delete phase A: CAS condemned -> tombstone (wins the race vs resurrect on the shared gen).
\* SabDeleteBodyNoTombstone SKIPS the tombstone claim and deletes the body directly under condemned.
GcDeletePhaseA ==
  /\ NoRef /\ metaState = "condemned" /\ Fresh
  /\ IF SabDeleteBodyNoTombstone
     THEN /\ body' = FALSE /\ metaState' = metaState /\ metaGen' = metaGen /\ nextGen' = nextGen   \* body gone, meta still condemned
     ELSE /\ metaState' = "tombstone" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1 /\ body' = body
  /\ UNCHANGED <<seeded, debrisUsed, refOf, condemnRound>>

\* Delete phase B: delete the raw body (only under a tombstone claim). SabDeleteMetaBeforeBody instead
\* deletes the META first (leaving the body), which a later adopt would see as clean-over... modeled as
\* dropping to absent while body present without deleting body -> a subsequent stale clean.
GcDeletePhaseB ==
  /\ metaState = "tombstone" /\ body
  /\ body' = FALSE /\ UNCHANGED <<metaState, metaGen>>
  /\ UNCHANGED <<nextGen, seeded, debrisUsed, refOf, condemnRound>>

\* Delete phase C: delete the tombstone meta (deleteExact on tombstone gen). Only after the body is gone.
GcDeletePhaseC ==
  /\ metaState = "tombstone" /\ ~body
  /\ metaState' = "absent" /\ metaGen' = 0
  /\ UNCHANGED <<body, nextGen, seeded, debrisUsed, refOf, condemnRound>>

------------------------------------------------------------------------------
Next ==
  \/ Seed \/ CrashedBirth
  \/ \E w \in Writers : FreshUpload(w) \/ Adopt(w) \/ Resurrect(w) \/ BirthCompletion(w) \/ DropRef(w)
  \/ GcCondemn \/ GcSpare \/ GcDeletePhaseA \/ GcDeletePhaseB \/ GcDeletePhaseC

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ body \in BOOLEAN
  /\ metaState \in {"absent","clean","condemned","tombstone"}
  /\ metaGen \in Nat /\ nextGen \in Nat
  /\ refOf \in [Writers -> BOOLEAN]

(* A writer that holds a committed ref must have a present body (no dangling committed ref). *)
INV_NO_DANGLE == \A w \in Writers : refOf[w] => body

(* A clean or condemned meta implies a present body — the 1-GET adopt's justification (adopt trusts a
   clean meta without a body HEAD; condemn leaves the body for a possible resurrect). A tombstone meta is
   the mid-delete state where the body may already be gone; an absent meta says nothing. *)
INV_META_BODY == (metaState \in {"clean","condemned"}) => body

====================================================================================
