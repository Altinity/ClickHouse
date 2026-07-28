-------------------- MODULE CaRefNsCleanupStaleLeaderCore --------------------
(* GC namespace-cleanup stale-leader core, rewritten for v9 — spec
   2026-07-27-cas-ref-chain-complete-cut-design.md §2 INV-3 (ref-layer-scoped incarnations,
   structural inertness) and §3 Lifecycles (removal, recreation). TLA+ phase (gate), Task 4.

   SUPERSEDES the 2026-07-11 design's `_cleanup`-marker / `Completed` recreation gate, which this
   module used to model directly (git history: the removal -> Completed -> marker -> recreation ->
   straggler-delete interleaving, guarded by a live round re-read + a marker check + an epoch filter).
   v9 DELETES that gate for the ref layer outright — no marker, no round to re-read, no epoch to
   filter — and this module now proves what carries the same safety property in its place.

   SCOPE — REF LAYER ONLY. §2 INV-3: "the incarnation ... qualifies the ref layer only:
   `<ns>/<inc>/{_log, _snap, _ckpt}` ... verbatim FILES stay unqualified and keep today's `_cleanup`
   gate — their pre-existing rebirth-aliasing hazard is register item R1, not this spec." This module
   therefore models ref-layer objects only, generalised (no `_log`/`_snap`/`_ckpt` distinction — none
   of the three is reachable except through the incarnation-qualified prefix all three share, so
   nothing is lost by treating them as one kind) and does NOT model files at all: the FILE layer's
   `_cleanup` marker gate is untouched by v9 and stays exactly as load-bearing as before — that is
   register R1's open problem, not this module's.

   WHY A DEDICATED MODULE, STILL. CaRefCatalogCore (Task 3) is the deep model of catalog lifecycles —
   creation's three conditional writes, reconciliation of a stale `Creating`, the lazy janitor's
   per-object incarnation scope, the churn bound — and its `Janitor` action already proves
   foreign-incarnation debris is safe to delete IN GENERAL. What it does not construct is the ONE
   interleaving this module exists for: a GC leader that captured an incarnation BEFORE deposition,
   stalled, and resumes its physical-delete pass only after that SAME NAME has been reborn. That is a
   straggling actor racing its own stale, already-captured state against catalog transitions it never
   observed — exactly the shape `CaRefWriterCleanupCore` explicitly declines to model ("no recreation
   in this small model") and `CaRefDeltaIntakeCore` abstracts past (its `Cleanup` is covered-LOG
   cleanup, not this physical `@cas@`-prefix pass). The catalog machinery below is deliberately thin —
   just enough state to make "recreation only starts once the old entry is gone" true — because the
   full lifecycle is Task 3's proof, not this one's; see the Scoping section of
   CaRefNsCleanupStaleLeaderCore_RESULTS.md for the exact simplifications and why each is safe to make
   here.

   MODELED INTERLEAVING. A GC leader A deposits a Pending namespace-cleanup item for a namespace being
   removed, capturing its incarnation `staleInc` — and STALLS before running its physical-delete pass
   (a VM pause; the P3.1 fence-out class). Meanwhile the removal completes (`EntryDelete`: the catalog
   entry goes `absent`, with NO physical-empty proof owed — INV-3's whole point) and the SAME name is
   reborn (`Recreate`: a fresh incarnation is minted; `Creating` + `_ckpt` create + the `Live` CAS are
   collapsed into one step, see Scoping) which then does ordinary ref-layer work (`WriteObject`).
   Leader A resumes and runs its stale pass exactly once, at whatever point in this sequence TLC's
   interleaving happens to schedule it — before the entry-delete, after it, before or after rebirth,
   before or after the new life's own write. `Next` explores all of them.

   THE RULE UNDER TEST used to be Step 6 straggler safety: a live re-read of the round, an abort on the
   `_cleanup` marker's presence, and an epoch filter on manifest deletes — three independent guards,
   each re-checked against fresh state at delete time. v9 deletes all three from the ref layer's
   recreation gate, and nothing replaces THEM as a live check. That is not the same claim as
   "destructive cleanup has no live precondition at all" — spec §2's read-side contract still mandates
   that "destructive cleanup revalidates life and fence immediately before every delete", and a real
   implementation keeps doing exactly that. This model does not need that revalidation to prove
   `NoLiveDataDeleted` and deliberately does not model it: `StaleLeaderPass` below is UNCONDITIONAL, an
   adversarial OVER-approximation of what the spec actually allows a delete to attempt, and the
   property survives anyway. That is a STRONGER result than "the revalidation catches it" would be —
   not a claim that the revalidation is unnecessary, absent, or this model's business.

   What IS gone, structurally, is any need for a live re-check of the DELETED incarnation's identity —
   and that only holds if the pass's target is itself trustworthy. A namespace-cleanup item names
   exactly one incarnation, `staleInc`, captured at deposition; spec §3 now states as a normative rule
   what this model exists to prove as an obligation: "the namespace-cleanup item carries the
   incarnation captured at deposition, and a resumed pass NEVER re-derives it from the catalog" (§3,
   added by this task's review — commit `d1eae033122`). `PassTarget` below is the switch between
   "capture" and "re-derive"; `SabotageRederive` is what an implementation that gets this backwards
   looks like: it resolves its target from the CURRENT `entry.inc` at resume time instead of the value
   deposited with the item, and the moment the namespace has been reborn that current value simply IS
   the live incarnation — no aliasing needed, the pass just targets the right thing for the wrong
   reason.

   Safety is therefore a structural consequence of TWO independent facts, each with its own lever:
   (1) `Recreate` never reusing `staleInc` (`SabotageNoIncarnation` breaks this — the same hazard as
   `SabotageSameIncarnationRebirth` in CaRefCatalogCore, replayed here in the stale-leader shape), and
   (2) the pass always using the incarnation captured at deposition, never one re-read at resume
   (`SabotageRederive` breaks this). Either alone reaches live data, by a different route: the first
   makes the CAPTURED value alias onto a live one; the second makes the pass stop using the captured
   value at all. That is the model-level proof the main plan cites: incarnation freshness AND
   deposit-time capture — not physical-empty polling, not a marker, and not the spec's separate
   revalidation requirement — are what carry rebirth safety for this specific hazard. *)
EXTENDS Integers

CONSTANTS
    MaxInc,                \* incarnations mintable in one run (TLC bound, as CaRefCatalogCore)
    SabotageNoIncarnation, \* recreation reuses `staleInc` instead of minting fresh (INV-3)
    SabotageRederive \* the resumed pass resolves its target from `entry.inc` (the CURRENT
                            \* catalog entry) instead of the incarnation captured at deposition
                            \* (spec §3: "a resumed pass NEVER re-derives it from the catalog")

ASSUME MaxInc \in Nat /\ MaxInc >= 2

Incs == 1..MaxInc

VARIABLES
    entry,           \* [state: {"removing","absent","live"}, inc: 0..MaxInc] — THE one namespace
                      \* name's catalog record, thinned to what this module's interleaving needs
                      \* (Scoping: no `Creating` phase, no `_ckpt`/terminal-record bookkeeping — that
                      \* is CaRefCatalogCore's proof)
    staleInc,         \* 1..MaxInc: the incarnation leader A's Pending item captured BEFORE
                      \* deposition — fixed for the run, exactly like the old model's `staleRound`
    nextInc,          \* 1..MaxInc+1: fresh-incarnation allocator (as CaRefCatalogCore's `nextInc`) —
                      \* a counter here because the only property ever used is freshness, not order
    objects,          \* SUBSET Incs: incarnations whose ref-layer object(s) are physically present
    passDone,         \* BOOLEAN: leader A's stale Pending pass has executed (once)
    deletedLiveData   \* ghost, sticky: the stale pass deleted an object belonging to the namespace's
                      \* CURRENT ("live") incarnation — the hazard under test

vars == << entry, staleInc, nextInc, objects, passDone, deletedLiveData >>

Init ==
    /\ entry = [state |-> "removing", inc |-> 1]  \* A's item was deposited while this life was being
                                                    \* removed; the story starts right there
    /\ staleInc = 1
    /\ nextInc = 2                                 \* 1 is already spoken for by the dying life
    /\ objects = {1}                               \* the dying life's own leftover data — the pass's
                                                    \* legitimate cleanup target
    /\ passDone = FALSE
    /\ deletedLiveData = FALSE

(* The removal's terminal record folds, best-effort cleanup runs, and the catalog entry is deleted —
   collapsed into one step because none of that ordering (INV-4's `_ckpt`-before-entry rule, the
   terminal record's actor identity) is this module's concern; CaRefCatalogCore is where that
   machinery is modelled and proved. What this module needs from it is only the GATE `Recreate` reads
   below: `entry.state = "absent"`, reached with NO physical-empty proof (INV-3). *)
EntryDelete ==
    /\ entry.state = "removing"
    /\ entry' = [state |-> "absent", inc |-> 0]
    /\ UNCHANGED << staleInc, nextInc, objects, passDone, deletedLiveData >>

(* THE REBIRTH. Spec §3: recreation begins only once the old entry is `absent` — no marker, no
   physical-empty proof, just a fresh incarnation (spec §2 INV-3: "random 128-bit, minted at
   `Creating`"; modelled, as in CaRefCatalogCore, as a monotonic allocator because the only property
   ever used is freshness, never ordering). Collapsed straight to `live` — the `Creating` phase's two
   extra conditional writes (the `_ckpt` create, the `Live` CAS) are Task 3's proof, not this one's;
   nothing about the leader-straggler hazard depends on the intermediate state.

   `SabotageNoIncarnation` is the one and only lever: it reuses `staleInc` instead of minting, which
   is precisely the alternative INV-3's inertness argument depends on NOT happening ("surviving
   old-incarnation objects are structurally inert" is true only because the incarnation is not
   reused). *)
NewInc == IF SabotageNoIncarnation THEN staleInc ELSE nextInc

Recreate ==
    /\ entry.state = "absent"
    /\ nextInc <= MaxInc
    /\ entry' = [state |-> "live", inc |-> NewInc]
    /\ nextInc' = nextInc + 1
    /\ UNCHANGED << staleInc, objects, passDone, deletedLiveData >>

(* Ordinary ref-layer work under the reborn life: an appended `_log`, an installed `_snap`, a `_ckpt`
   write — all at `<ns>/<inc>/...`, so all indistinguishable at this model's granularity. Idempotent
   on purpose (a union, not a guarded first-write): this module does not care how many times the new
   life writes, only whether ITS incarnation's objects survive the straggler. *)
WriteObject ==
    /\ entry.state = "live"
    /\ objects' = objects \cup {entry.inc}
    /\ UNCHANGED << entry, staleInc, nextInc, passDone, deletedLiveData >>

(* Spec §3's normative rule, added by this task's review (commit `d1eae033122`): "the
   namespace-cleanup item carries the incarnation captured at deposition, and a resumed pass NEVER
   re-derives it from the catalog". Honestly, always `staleInc` — the value fixed at `Init`, never
   `entry.inc` as it stands right now. `SabotageRederive` is the one way to break the rule: the pass
   resolves its OWN target from whatever the catalog currently says, which is indistinguishable from
   "discovering current lives" (spec §2's ordinary, legitimate use of `entry.inc`) right up to the
   moment the name has been reborn — at which point it is simply the live incarnation, not a captured
   cleanup scope at all. *)
PassTarget == IF SabotageRederive THEN entry.inc ELSE staleInc

(* Leader A resumes its stale Pending pass (once), at ANY point after depositing it — `Next`'s
   interleaving explores every placement relative to `EntryDelete`, `Recreate` and `WriteObject`.
   The `_cleanup`-marker / round-recheck gate spec §3 deletes for the ref layer has no replacement
   live check IN THIS ACTION — spec §2's mandated destructive-cleanup revalidation (life + fence) is
   a real, separate precondition a real implementation still owes on every delete; this model
   deliberately omits it to prove the STRONGER claim that `NoLiveDataDeleted` does not depend on it.
   What the action does still have to get right is `PassTarget`: there is no marker check, no round
   CAS, and (honestly) no re-derivation — the action is unconditional on the catalog's current state
   on purpose. The honest config's green is not "a guard held", it is "there was nothing to reach":
   `PassTarget` cannot coincide with the live incarnation unless something upstream — an incarnation
   reuse, or a re-derivation — made it so. *)
StaleLeaderPass ==
    /\ ~passDone
    /\ passDone' = TRUE
    /\ deletedLiveData' = (deletedLiveData \/
                            (entry.state = "live" /\ entry.inc = PassTarget /\ PassTarget \in objects))
    /\ objects' = objects \ {PassTarget}
    /\ UNCHANGED << entry, staleInc, nextInc >>

NoOp == UNCHANGED vars

Next ==
    \/ EntryDelete
    \/ Recreate
    \/ WriteObject
    \/ StaleLeaderPass
    \/ NoOp

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* ---- invariants ---- *)

TypeOK ==
    /\ entry \in [state : {"removing", "absent", "live"}, inc : 0..MaxInc]
    /\ (entry.state = "absent" <=> entry.inc = 0)
    /\ staleInc \in Incs
    /\ nextInc \in 1..(MaxInc + 1)
    /\ objects \subseteq Incs
    /\ passDone \in BOOLEAN
    /\ deletedLiveData \in BOOLEAN

(* (SAFETY, spec §2 INV-3: "surviving old-incarnation objects are structurally inert"; spec §3: a
   cleanup item's incarnation is fixed at deposition, never re-derived.) The stale Pending pass —
   entirely unguarded, unconditional, running at any point in the interleaving — never deletes the
   reborn life's own data, PROVIDED both of its inputs are honest: `Recreate` mints a fresh
   incarnation (`~SabotageNoIncarnation`) and the pass targets what it captured at deposition, not a
   value re-read from the current catalog (`~SabotageRederive`). Either sabotage alone is enough to
   make `PassTarget = entry.inc` while `entry.state = "live"`, by a different route, and either alone
   is therefore enough to violate this. *)
NoLiveDataDeleted == ~deletedLiveData

=============================================================================
