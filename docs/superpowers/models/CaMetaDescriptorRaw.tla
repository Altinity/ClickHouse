-------------------------- MODULE CaMetaDescriptorRaw --------------------------
(* Gate B (v2, 2026-07-10) for spec 2026-07-09-cas-writer-gc-simplification, Phase B FINAL design:      *)
(*   RAW bodies (no envelope; body write-once, deleted only by GC) + a per-hash META with a THREE-state  *)
(*   lifecycle {clean, condemned, tombstone} whose etag ("gen") is the SOLE linearization token.         *)
(*                                                                                                        *)
(* v2 CORRECTION (consult 2026-07-10, CRITICAL): TOMBSTONE IS TERMINAL. Under raw immutable bodies a      *)
(* resurrect-from-tombstone cannot protect the body (its token never changes), so GC's already-decided    *)
(* body delete would still hit it -> a committed ref dangles. The v1 model hid this by conjoining         *)
(* "meta==tombstone" with "delete body" in ONE atomic action (an atomicity S3 cannot provide).            *)
(*                                                                                                        *)
(* v2 fixes both: (a) resurrect is legal ONLY from `condemned` (its condemned->clean CAS races GC's       *)
(* condemned->tombstone on the shared etag; exactly one wins). A writer seeing `tombstone` WAITS for      *)
(* `absent` and re-creates fresh — it NEVER un-tombstones. (b) GC's body delete is modeled as a NON-ATOMIC*)
(* two-step: GcClaimTombstone sets `gcDeleteCommitted` (won the linearizer), then GcDeleteBody deletes the *)
(* body keyed on that COMMITMENT (not on the current metaState) — faithfully modeling "GC decided, gap,   *)
(* then deletes". The new sabotage SabResurrectFromTombstone re-enables the buggy tombstone->clean and    *)
(* MUST break INV_NO_LOSS / INV_NO_DANGLE, proving terminal-tombstone is load-bearing.                    *)
(*                                                                                                        *)
(* Universe: ONE content hash, TWO writers (multi-writer races), one GC. Body presence is a boolean       *)
(* (raw, immutable, write-once per lifecycle). A pre-existing incarnation (Seed) is condemnable prey;     *)
(* CrashedBirth models body-without-meta debris. Every meta write mints a fresh gen (= the incarnation    *)
(* nonce: on S3 the etag is content-derived, so the durable code carries a fresh u128 nonce making every  *)
(* meta write globally unique — modeled here as nextGen++).                                               *)
EXTENDS Naturals

CONSTANTS
  MaxGen,                     \* bound on meta generations (etags)
  Writers,                    \* set of writer ids (>=2 for the races)
  SabBlindAdopt,              \* adopt ignores condemned/tombstone (reference a doomed body)
  SabAdoptOverTombstone,      \* adopt a body whose meta is tombstone (being deleted)
  SabDeleteBodyNoTombstone,   \* GC deletes the body while meta is still condemned (no tombstone claim)
  SabMetaBeforeBody,          \* fresh upload writes meta before body (crash window -> meta w/o body)
  SabResurrectFromTombstone   \* v2: a writer un-tombstones (tombstone->clean) — the raw-body C2 bug

VARIABLES
  body,               \* BOOLEAN — the raw content body present?
  metaState,          \* "absent" | "clean" | "condemned" | "tombstone"
  metaGen,            \* Nat — the meta etag (0 when absent); fresh per write (incarnation nonce)
  nextGen,            \* fresh-gen allocator
  seeded, debrisUsed,
  refOf,              \* [Writers -> BOOLEAN] : this writer holds a committed ref naming the hash
  condemnRound,       \* the round the current condemnation used (model pacing realism)
  gcDeleteCommitted,  \* v2: GC won the tombstone claim and WILL delete the body (the non-atomic gap)
  deletedGens         \* set of gens whose meta was fully deleted (for INV_NO_RETURN)

vars == <<body, metaState, metaGen, nextGen, seeded, debrisUsed, refOf,
          condemnRound, gcDeleteCommitted, deletedGens>>

Init ==
  /\ body = FALSE
  /\ metaState = "absent" /\ metaGen = 0
  /\ nextGen = 1 /\ seeded = FALSE /\ debrisUsed = FALSE
  /\ refOf = [w \in Writers |-> FALSE]
  /\ condemnRound = 0
  /\ gcDeleteCommitted = FALSE
  /\ deletedGens = {}

Fresh == nextGen < MaxGen

------------------------------------------------------------------------------
(* Pre-existing clean incarnation: a prior committed part, its ref since dropped. Only from the initial
   empty state — Seed models an at-rest starting condition, not a re-establishment mid-lifecycle (that is
   FreshUpload/Resurrect/BirthCompletion). Without the empty guard, Seed could overwrite a tombstone. *)
Seed ==
  /\ ~seeded /\ Fresh
  /\ metaState = "absent" /\ ~body
  /\ body' = TRUE
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ seeded' = TRUE
  /\ UNCHANGED <<debrisUsed, refOf, condemnRound, gcDeleteCommitted, deletedGens>>

(* A crashed pre-meta fresh upload: body landed, meta did not (the create-bottom-up crash window). *)
CrashedBirth ==
  /\ ~seeded /\ ~debrisUsed /\ Fresh
  /\ ~body /\ metaState = "absent"
  /\ body' = TRUE /\ debrisUsed' = TRUE /\ seeded' = TRUE
  /\ UNCHANGED <<metaState, metaGen, nextGen, refOf, condemnRound, gcDeleteCommitted, deletedGens>>

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
  /\ UNCHANGED <<seeded, debrisUsed, condemnRound, gcDeleteCommitted, deletedGens>>

\* Adopt: meta clean => reference the body directly (1 GET; body present by INV_META_BODY). Blind/
\* over-tombstone sabotages reference a doomed/being-deleted body.
Adopt(w) ==
  /\ ~refOf[w]
  /\ \/ metaState = "clean"
     \/ (SabBlindAdopt /\ metaState \in {"condemned","tombstone"})
     \/ (SabAdoptOverTombstone /\ metaState = "tombstone")
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<body, metaState, metaGen, nextGen, seeded, debrisUsed,
                 condemnRound, gcDeleteCommitted, deletedGens>>

\* Resurrect: meta condemned => CAS clean(fresh gen). Body still present (condemn never deletes it),
\* so NO body re-upload (the raw-body simplification). Races GcClaimTombstone on the shared condemned etag.
Resurrect(w) ==
  /\ ~refOf[w] /\ metaState = "condemned" /\ Fresh
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<body, seeded, debrisUsed, condemnRound, gcDeleteCommitted, deletedGens>>

\* Birth-completion: meta ABSENT while body present (crashed pre-meta birth ONLY — NOT tombstone).
\* CORRECT: re-establish the meta (create If-None-Match) from the writer's own source; never adopt blind.
\* v2: tombstone is NOT completable here (terminal); a writer at tombstone must wait for absent.
BirthCompletion(w) ==
  /\ ~refOf[w] /\ Fresh
  /\ body /\ metaState = "absent"
  /\ body' = TRUE
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<seeded, debrisUsed, condemnRound, gcDeleteCommitted, deletedGens>>

\* SABOTAGE (v2): a writer un-tombstones (the buggy raw-body plan). Under raw bodies this cannot protect
\* the body, so GC's committed delete dangles the fresh ref. MUST break INV_NO_LOSS / INV_NO_DANGLE.
ResurrectFromTombstone(w) ==
  /\ SabResurrectFromTombstone
  /\ ~refOf[w] /\ metaState = "tombstone" /\ body /\ Fresh
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ refOf' = [refOf EXCEPT ![w] = TRUE]
  /\ UNCHANGED <<body, seeded, debrisUsed, condemnRound, gcDeleteCommitted, deletedGens>>

\* A committed ref is dropped (its owner no longer references the hash) — enables re-condemnation.
DropRef(w) ==
  /\ refOf[w]
  /\ refOf' = [refOf EXCEPT ![w] = FALSE]
  /\ UNCHANGED <<body, metaState, metaGen, nextGen, seeded, debrisUsed,
                 condemnRound, gcDeleteCommitted, deletedGens>>

------------------------------------------------------------------------------
(* GC. Acts only on a hash no live writer references (folded in-degree 0). *)
NoRef == \A w \in Writers : ~refOf[w]

GcCondemn ==
  /\ NoRef /\ metaState = "clean" /\ body /\ Fresh
  /\ metaState' = "condemned" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ condemnRound' = condemnRound + 1
  /\ UNCHANGED <<body, seeded, debrisUsed, refOf, gcDeleteCommitted, deletedGens>>

GcSpare ==
  /\ metaState = "condemned" /\ (\E w \in Writers : refOf[w]) /\ Fresh
  /\ metaState' = "clean" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1
  /\ UNCHANGED <<body, seeded, debrisUsed, refOf, condemnRound, gcDeleteCommitted, deletedGens>>

\* Delete phase A: CAS condemned -> tombstone (wins vs a racing resurrect on the shared etag) AND commit
\* to deleting the body (gcDeleteCommitted). SabDeleteBodyNoTombstone SKIPS the tombstone claim and deletes
\* the body directly under condemned.
GcClaimTombstone ==
  /\ NoRef /\ metaState = "condemned" /\ Fresh
  /\ IF SabDeleteBodyNoTombstone
     THEN /\ body' = FALSE /\ metaState' = metaState /\ metaGen' = metaGen /\ nextGen' = nextGen
          /\ gcDeleteCommitted' = gcDeleteCommitted
     ELSE /\ metaState' = "tombstone" /\ metaGen' = nextGen /\ nextGen' = nextGen + 1 /\ body' = body
          /\ gcDeleteCommitted' = TRUE
  /\ UNCHANGED <<seeded, debrisUsed, refOf, condemnRound, deletedGens>>

\* Delete phase B: delete the raw body. Keyed on the COMMITMENT (gcDeleteCommitted), NOT on the current
\* metaState — this is the crux: in reality GC decided at tombstone, then (after a gap) HEADs+deletes the
\* body unconditionally. In the correct protocol nothing un-tombstones, so metaState is still tombstone
\* here; under SabResurrectFromTombstone a writer moved it to clean and this delete dangles the ref.
GcDeleteBody ==
  /\ gcDeleteCommitted /\ body
  /\ body' = FALSE
  /\ UNCHANGED <<metaState, metaGen, nextGen, seeded, debrisUsed, refOf,
                 condemnRound, gcDeleteCommitted, deletedGens>>

\* Delete phase C: delete the tombstone meta (deleteExact on tombstone gen). Only after the body is gone.
\* Clears the commitment and records the retired gen (INV_NO_RETURN).
GcDeleteMeta ==
  /\ metaState = "tombstone" /\ ~body
  /\ metaState' = "absent" /\ deletedGens' = deletedGens \cup {metaGen} /\ metaGen' = 0
  /\ gcDeleteCommitted' = FALSE
  /\ UNCHANGED <<body, nextGen, seeded, debrisUsed, refOf, condemnRound>>

------------------------------------------------------------------------------
Next ==
  \/ Seed \/ CrashedBirth
  \/ \E w \in Writers :
       FreshUpload(w) \/ Adopt(w) \/ Resurrect(w) \/ BirthCompletion(w)
       \/ ResurrectFromTombstone(w) \/ DropRef(w)
  \/ GcCondemn \/ GcSpare \/ GcClaimTombstone \/ GcDeleteBody \/ GcDeleteMeta

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ body \in BOOLEAN
  /\ metaState \in {"absent","clean","condemned","tombstone"}
  /\ metaGen \in Nat /\ nextGen \in Nat
  /\ refOf \in [Writers -> BOOLEAN]
  /\ gcDeleteCommitted \in BOOLEAN

(* A writer that holds a committed ref must have a present body (no dangling committed ref). *)
INV_NO_DANGLE == \A w \in Writers : refOf[w] => body

(* A clean or condemned meta implies a present body — the 1-GET adopt's justification. A tombstone meta
   is the mid-delete state where the body may already be gone; an absent meta says nothing. *)
INV_META_BODY == (metaState \in {"clean","condemned"}) => body

(* The delete-safety property: once GC has committed to deleting the body, no writer holds (or acquires)
   a ref to it. This is what terminal-tombstone buys; SabResurrectFromTombstone breaks it directly. *)
INV_NO_LOSS == gcDeleteCommitted => NoRef

(* A fully-deleted meta generation never becomes current again (no ABA / resurrection of a dead gen). *)
INV_NO_RETURN == (metaState # "absent") => (metaGen \notin deletedGens)

====================================================================================
