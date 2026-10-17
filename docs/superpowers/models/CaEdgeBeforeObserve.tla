---------------------------- MODULE CaEdgeBeforeObserve ----------------------------
(* Gate A of spec 2026-07-09-cas-writer-gc-simplification (Phase A).                *)
(* Focused model of the EDGE-BEFORE-OBSERVE claim: with the writer order            *)
(* precommit(closure durable) -> adopt/observe -> promote, and GC's                 *)
(* condemn -> graduate(floor) -> same-pass decided delete pipeline with per-pass    *)
(* d-recheck, the promote-time revalidation of TOKENED leaves is redundant, while   *)
(* the dedup-adoption check (K1), the tokenless presence HEAD (K3Head), the         *)
(* tokenless condemned check (K3AdoptCheck) and the ORDER itself stay load-bearing. *)
(*                                                                                  *)
(* Universe: ONE build; leaf ht = tokened dedup-adopted hash; leaf he = tokenless   *)
(* (adoptEvidence) hash. Both pre-exist unowned (their prior owners dropped), so    *)
(* GC condemns them when the build's closure is not yet durable/folded.             *)
(*                                                                                  *)
(* GC pipeline per pass (matches CasGc: fold -> settle -> pre-CAS deletes, then     *)
(* round CAS): GcSettle computes per-hash in-degree d from DURABLE state (the       *)
(* precommit closure once appended, or the committed ref), spares entries with      *)
(* d > 0 (including pending — the loud impossible-spare), condemns unowned          *)
(* present hashes, graduates cond -> pend when condemn_round < the writer's         *)
(* ADVERTISED round (the ack floor; advertised == installed view), and DECIDES      *)
(* deletes (doomed) for pending entries still at d = 0. GcExecute performs a        *)
(* decided delete — the seal->execute gap in which the adoption races live.         *)
(* Displacement (a writer re-uploading a fresh incarnation over a condemned one)    *)
(* drops the ledger entry AND revokes a decided delete: the exact-token delete      *)
(* misses the fresh incarnation.                                                    *)
EXTENDS Naturals, FiniteSets

CONSTANTS
  MaxRound,        \* GC round bound
  OrderSabotage,   \* TRUE: adoption allowed BEFORE the durable closure (pre-B188 order)
  AdoptCheck,      \* K1: adoption consults the installed view; condemned => displace
  K3Head,          \* promote HEADs the tokenless leaf; absent => abort
  K3AdoptCheck     \* promote consults the view for the tokenless leaf; condemned => copy-forward (displace)

HT == "ht"                 \* tokened leaf (dedup-adopted at putBlob)
HE == "he"                 \* tokenless leaf (adoptEvidence; observation-free before promote)
Leaves == {HT, HE}

NoEntry == [st |-> "none", r |-> 0]

VARIABLES
  present,       \* [Leaves -> BOOLEAN]           bodies
  entry,         \* [Leaves -> [st: {"none","cond","pend"}, r: Nat]]   GC retired ledger
  doomed,        \* SUBSET Leaves                 decided deletes awaiting execution (same pass)
  round,         \* GC round (bumped by settle)
  view,          \* writer's installed view round == its ADVERTISED ack (single writer)
  precommitted,  \* the closure (naming BOTH leaves) is durable in the shard journal
  adopted,       \* the tokened leaf ht was adopted by putBlob
  committed,     \* promote succeeded: the ref names BOTH leaves
  aborted        \* the build failed closed (terminal)

vars == <<present, entry, doomed, round, view, precommitted, adopted, committed, aborted>>

\* An entry visible in the writer's installed view (entries persist until spared/executed).
VisibleCond(h) == entry[h].st # "none" /\ entry[h].r <= view

\* Durable in-degree of a leaf: the precommit closure (until abort) or the committed ref.
Deg(h) == IF (precommitted /\ ~aborted) \/ committed THEN 1 ELSE 0

Init ==
  /\ present = [h \in Leaves |-> TRUE]
  /\ entry = [h \in Leaves |-> NoEntry]
  /\ doomed = {}
  /\ round = 1
  /\ view = 0
  /\ precommitted = FALSE /\ adopted = FALSE /\ committed = FALSE /\ aborted = FALSE

\* Writer installs the latest published round; its beat advertises it (view == advertised).
ViewAdvance ==
  /\ view < round
  /\ view' = round
  /\ UNCHANGED <<present, entry, doomed, round, precommitted, adopted, committed, aborted>>

\* stageManifest + precommitAdd: the closure naming BOTH leaves becomes durable.
Precommit ==
  /\ ~precommitted /\ ~aborted
  /\ precommitted' = TRUE
  /\ UNCHANGED <<present, entry, doomed, round, view, adopted, committed, aborted>>

\* putBlob dedup-adoption of ht. Enabled post-precommit (the order) unless sabotaged.
\* With AdoptCheck, a visible condemned entry triggers DISPLACEMENT (fresh incarnation,
\* INV-1 re-upload): the ledger entry is dropped and any decided delete is revoked
\* (exact-token miss). Without it, the adoption is blind (the K1 hole).
Adopt ==
  /\ ~adopted /\ ~aborted /\ present[HT]
  /\ (precommitted \/ OrderSabotage)
  /\ adopted' = TRUE
  /\ IF AdoptCheck /\ VisibleCond(HT)
     THEN /\ entry' = [entry EXCEPT ![HT] = NoEntry]
          /\ doomed' = doomed \ {HT}
     ELSE UNCHANGED <<entry, doomed>>
  /\ UNCHANGED <<present, round, view, precommitted, committed, aborted>>

\* The tokened leaf vanished before adoption (its delete executed): fail closed (putBlob
\* would fresh-upload in reality; aborting keeps the model tiny — both are non-dangles).
AdoptGone ==
  /\ ~adopted /\ ~aborted /\ ~committed /\ ~present[HT]
  /\ aborted' = TRUE
  /\ UNCHANGED <<present, entry, doomed, round, view, precommitted, adopted, committed>>

\* promote: NO revalidation of the tokened leaf (the Phase-A reduction under test).
\* The tokenless leaf gets: presence HEAD (K3Head; absent => abort) and the condemned
\* check (K3AdoptCheck; visible condemned => copy-forward == displacement).
Promote ==
  /\ precommitted /\ adopted /\ ~committed /\ ~aborted
  /\ IF K3Head /\ ~present[HE]
     THEN /\ aborted' = TRUE
          /\ UNCHANGED <<present, entry, doomed, round, view, precommitted, adopted, committed>>
     ELSE /\ committed' = TRUE
          /\ IF K3AdoptCheck /\ present[HE] /\ VisibleCond(HE)
             THEN /\ entry' = [entry EXCEPT ![HE] = NoEntry]
                  /\ doomed' = doomed \ {HE}
             ELSE UNCHANGED <<entry, doomed>>
          /\ UNCHANGED <<present, round, view, precommitted, adopted, aborted>>

\* One GC pass head: fold (Deg from durable state) + settle + DECIDE deletes. Enabled only
\* at a pass boundary (no undecided deletes pending — deletes execute within their pass).
SettleOne(h) ==
  IF present[h]
  THEN IF Deg(h) > 0
       THEN NoEntry                                        \* spared (incl. pending — loud in real code)
       ELSE IF entry[h].st = "none"
            THEN [st |-> "cond", r |-> round]              \* condemn: unowned present hash
            ELSE IF entry[h].st = "cond" /\ entry[h].r < view
                 THEN [st |-> "pend", r |-> entry[h].r]    \* graduate: floor passed (advertised > r)
                 ELSE entry[h]                             \* carried
  ELSE NoEntry                                             \* body gone: entry confirmed/dropped

GcSettle ==
  /\ round <= MaxRound
  /\ doomed = {}
  /\ entry' = [h \in Leaves |-> SettleOne(h)]
  /\ doomed' = {h \in Leaves: present[h] /\ Deg(h) = 0 /\ entry[h].st = "pend"}
  /\ round' = round + 1
  /\ UNCHANGED <<present, view, precommitted, adopted, committed, aborted>>

\* Execute one decided delete (the exact-token delete; a displacement has already
\* removed revoked targets from doomed).
GcExecute ==
  /\ \E h \in doomed:
       /\ present' = [present EXCEPT ![h] = FALSE]
       /\ entry' = [entry EXCEPT ![h] = NoEntry]
       /\ doomed' = doomed \ {h}
  /\ UNCHANGED <<round, view, precommitted, adopted, committed, aborted>>

Next ==
  \/ ViewAdvance \/ Precommit \/ Adopt \/ AdoptGone \/ Promote
  \/ GcSettle \/ GcExecute

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ present \in [Leaves -> BOOLEAN]
  /\ \A h \in Leaves: entry[h].st \in {"none", "cond", "pend"} /\ entry[h].r \in Nat
  /\ doomed \subseteq Leaves
  /\ round \in Nat /\ view \in Nat
  /\ precommitted \in BOOLEAN /\ adopted \in BOOLEAN
  /\ committed \in BOOLEAN /\ aborted \in BOOLEAN

\* THE invariant: a committed ref never names an absent body.
INV_NO_DANGLE == committed => \A h \in Leaves: present[h]

====================================================================================
