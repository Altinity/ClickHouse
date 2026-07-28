-------------------- MODULE CaRefCatalogCore --------------------
(* Namespace catalog core — spec 2026-07-27-cas-ref-chain-complete-cut-design.md (v9),
   §2 INV-3 (`cas/ref_catalog`, ref-layer-scoped incarnations, structural inertness, the
   O(`Creating` + `Live` + `Removing`) churn bound), §2 INV-4 (`_ckpt`; removal deletes it by exact
   token while the entry is `Removing`, catalog entry LAST), §3 Lifecycles (creation's three
   conditional writes, the admission fence generation required on each of them, reconciliation of a
   stale `Creating`, removal and its terminal record). TLA+ phase (gate), Task 3.

   ONE logical namespace name, lived over and over. That is the whole point of this module: the
   question v9 has to answer is not what one life does but what SURVIVES a life and what the next
   life makes of it. Everything here is therefore keyed by INCARNATION — `<ns>/<inc>/{_log,_snap,_ckpt}`
   — and the oracle is not "is the pool tidy" (it never is: cleanup is best-effort and the janitor is
   driven by the same zero-trust listing §1 distrusts) but "did a new life ever touch an old life's
   bytes, and did the catalog ever keep a record a name no longer has".

   THE CREATOR IS A SEPARATE ACTOR FROM ITS CATALOG ENTRY, and the two are deliberately not tied
   together. A creator captures `(incarnation, mount-fence generation, catalog token)` at admission
   and thereafter writes BLIND — three conditional writes with no re-read (spec §3). Deleting its
   catalog entry therefore does not delete IT: reconciliation, removal and a successor's `Create`
   FENCE the creator out, they do not erase it, and what stops a fenced-out creator from finishing
   its creation on top of whatever the catalog now holds is that its two captured credentials no
   longer validate. `GoLive` carries both checks and `ZombieGoLive` is what happens without them.
   An earlier revision of this module collapsed the creator into its entry, which made that whole
   failure mode unrepresentable and let a fence-terminal creator publish; the fix round that split
   them is the reason `creatorInc`, `creatorStale` and `installed` exist.

   What the incarnation buys, stated as the thing this module must prove:

   - `EntryDelete` runs with NO physical-empty proof. Surviving old-incarnation objects are
     structurally inert — a foreign prefix that the fold, which works only off catalog entries, will
     never open — so removal need not wait for the prefix to be provably empty. That is the entire
     reason the catalog stays bounded under create/drop churn, and it is what `_churn` (three full
     create -> drop -> recreate cycles, debris deliberately left behind by every one of them) and
     `_witness_churn3` (the same three cycles COMPLETE while debris survives) exist to show.
   - The alternative the user rejected (§10: a `seq_floor` per name instead of an incarnation) is
     executable here as `SabotageFloorRetainsDeadName`: removal keeps a per-dead-life record instead
     of deleting the entry. `INV_BOUNDED_CATALOG` is INV-3's own sentence — the catalog holds a
     record exactly for names that are `Creating`/`Live`/`Removing` — and that sabotage is what
     proves the sentence has teeth rather than being a restatement of the honest transitions.

   Adversaries, not sabotages, and enabled in GREEN configurations too: `TerminalFoldAndCleanup`
   leaves remnants nondeterministically (cleanup is best-effort), the janitor fires
   nondeterministically per object and may omit any object FOREVER (LIST is a zero-trust hint, §1;
   omission is deferred cleanup, the leak-only direction), and the owning writer's fence may go
   terminal at any moment (`CreatorDies` — which fences the writer, it does not stop it).

   Each Sabotage* toggle breaks exactly one load-bearing rule and must yield a counterexample:

     SabotageJanitorEatsNewborn      the janitor's incarnation scope (INV-3)     -> INV_NEWBORN_SAFE
     SabotageZombieGoLive            the install's fence generation AND catalog token-CAS (§3,
                                     INV-3 "token-CAS like `gc/state`")          -> INV_NEWBORN_SAFE
     SabotageReconcileLiveCreator    reconciliation's fence-terminal precondition (§3)
                                                                                 -> INV_RECONCILE_SAFE
     SabotageReconcileStaleToken     reconciliation's token-exact CAS (§3)       -> INV_RECONCILE_SAFE
     SabotageEntryBeforeCkptDelete   `_ckpt` before the catalog entry (INV-4)    -> INV_CKPT_ORDER
     SabotageSameIncarnationRebirth  incarnation freshness (INV-3)               -> INV_NO_ALIAS
     SabotageFloorRetainsDeadName    the rejected `seq_floor` catalog (§10)      -> INV_BOUNDED_CATALOG

   Two deliberate departures from the task brief's sketch, both because the sketched form cannot
   carry the property (details in CaRefCatalogCore_RESULTS.md):

   - the brief's `INV_RECONCILE_SAFE == (entry.state = "creating") => (ckptOf # {} \/ creatorAlive
     \/ entry.inc = 0)` is RED in the fully honest model — `CreatorDies` before `CkptCreate` is a
     legitimate transient state, not a violation. It is retained verbatim below as
     `INV_RECONCILE_SAFE_BRIEF` and `_finding_briefreconcileinv` is its committed counterexample, so
     the discrepancy is evidence rather than prose. The property it was reaching for is stated over
     the DAMAGE instead (`reconcileHarm`).
   - the brief's `newbornEaten` ghost is set by the sabotage's own step, so its counterexample would
     be tautological. `INV_NEWBORN_SAFE` is instead the state invariant the creation order buys
     (`_ckpt` create precedes catalog `Live`, so a `Live` entry always has its `_ckpt`), which the
     eaten newborn only violates once its creator goes on to publish `Live` — the consequence, not
     the act. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    MaxInc,                         \* incarnations mintable in one run; also the churn-cycle bound
    SabotageJanitorEatsNewborn,     \* the janitor also eats the CURRENT incarnation while `Creating`
    SabotageZombieGoLive,           \* a fenced-out / token-stale creator's install is not refused
    SabotageReconcileLiveCreator,   \* reconcile a stale `Creating` without its fence being terminal
    SabotageReconcileStaleToken,    \* reconcile on a STALE sample: an unconditional delete, not a CAS
    SabotageEntryBeforeCkptDelete,  \* delete the catalog entry while its `_ckpt` still exists
    SabotageSameIncarnationRebirth, \* a new life REUSES the dead life's incarnation instead of minting
    SabotageFloorRetainsDeadName    \* §10's rejected `seq_floor`: removal keeps a dead-name record

ASSUME MaxInc \in Nat /\ MaxInc >= 2

Incs == 1..MaxInc
States == {"absent", "creating", "live", "removing"}

VARIABLES
    (* ---- `cas/ref_catalog`: the record for THE one namespace name ---- *)
    entry,         \* [state: States, inc: 0..MaxInc]; "absent" <=> inc = 0
    terminal,      \* BOOLEAN: the `Removing` life's terminal record has folded
    nextInc,       \* 1..MaxInc+1: fresh-incarnation allocator. Random 128-bit in reality; a counter
                   \* here because the only property ever used is FRESHNESS, never ordering.
    lastInc,       \* 0..MaxInc: the most recent life's incarnation (the rebirth sabotage's source)
    lives,         \* 0..MaxInc: COMPLETED lives (the churn-cycle counter; a reconciled entry never
                   \* completed a life and does not count)
    floors,        \* SUBSET Incs: dead-name records the REJECTED `seq_floor` catalog retains. Empty
                   \* under every honest transition; the sabotage is what fills it.
    (* ---- the creator: captured at admission, and NOT erased when it is fenced out ---- *)
    creatorInc,    \* 0..MaxInc: the incarnation this creator was admitted for. Distinct from
                   \* `entry.inc` on purpose — they diverge exactly when the creator has been fenced
                   \* out, which is the case the install has to refuse.
    creatorAlive,  \* BOOLEAN: its mount-fence generation is still valid (spec §3: captured at
                   \* admission, required on every `slot-occupy`, `_ckpt` CAS and install). FALSE
                   \* means FENCED, not gone: the process may still issue every write it planned.
    creatorStale,  \* BOOLEAN: the catalog was written since admission, so the token it captured no
                   \* longer matches and its install's token-CAS must fail (INV-3: `ref_catalog` is
                   \* token-CAS "like `gc/state`").
    ckptDone,      \* BOOLEAN: its own `_ckpt` conditional create returned success. Distinct from
                   \* `creatorInc \in ckptOf` (does the object exist NOW) on purpose: creation is
                   \* three blind conditional writes with no re-read, so the creator goes `Live` on
                   \* its own PUT's ack, and that gap is exactly where an eaten newborn hides.
    installed,     \* BOOLEAN: this creator has made its ONE install attempt (successful or zombie).
                   \* Bounds the model — without it a zombie could resurrect the name indefinitely.
    (* ---- the ref layer, keyed by incarnation ---- *)
    objects,       \* SUBSET Incs: incarnations whose `_log`/`_snap` objects exist
    ckptOf,        \* SUBSET Incs: incarnations whose `_ckpt` object exists
    (* ---- the reconciler's sample (a token-CAS reader, modelled without a token counter) ---- *)
    obsArmed,      \* BOOLEAN: a reconciler holds a catalog sample it has not acted on
    obsStale,      \* BOOLEAN: the catalog was written since that sample -- i.e. its token no longer
                   \* matches, so a token-exact CAS would now FAIL. Cheaper than a token counter and
                   \* exactly as strong: token equality is precisely "unwritten since sampled".
    orphanInc,     \* 0..MaxInc: a still-running life whose catalog entry someone else destroyed
    orphanAlive,   \* BOOLEAN: that orphan's fence is still live
    (* ---- ghosts ---- *)
    aliased,          \* sticky: a new life read an old life's object at its own prefix
    aliasedOnRemnant, \* sticky, a STRICT SUBSET of `aliased`: the aliased object was a `_log`/`_snap`
                      \* remnant left by a COMPLETED removal — the headline case, since that is the
                      \* debris `EntryDelete`'s missing physical-empty proof knowingly leaves behind.
                      \* Witnessed separately (`_witness_aliasremnant`) because it is not the shortest
                      \* route to `aliased` and BFS therefore never reports it.
    reconcileHarm,    \* sticky: an orphaned running life's bytes were destroyed, or it wrote bytes
                      \* nothing in the catalog can ever name
    orphanDataEaten,  \* sticky, a STRICT SUBSET of `reconcileHarm`: the severe arm — the janitor
                      \* deleted a running life's `_log`/`_snap` DATA, not merely its `_ckpt`. Same
                      \* reason for the separate witness: `OrphanWrite` reaches `reconcileHarm` one
                      \* step sooner, and the `_ckpt`-only deletion one step sooner than that.
    ckptOrphaned      \* sticky: a REMOVAL deleted the catalog entry while its `_ckpt` survived
                      \* (INV-4's ordering). Scoped to the removal path deliberately: reconciliation
                      \* also destroys an entry with a `_ckpt` behind it, but that is not an ordering
                      \* violation — a reconciled `Creating` never reached `Removing`, so no
                      \* exact-token delete was ever owed. See Scoping in the RESULTS document.

entryVars   == << entry, terminal, nextInc, lastInc, lives, floors >>
creatorVars == << creatorInc, creatorAlive, creatorStale, ckptDone, installed >>
objVars     == << objects, ckptOf >>
recVars     == << obsArmed, obsStale, orphanInc, orphanAlive >>
ghostVars   == << aliased, aliasedOnRemnant, reconcileHarm, orphanDataEaten, ckptOrphaned >>

vars == << entryVars, creatorVars, objVars, recVars, ghostVars >>

(* Any write to `cas/ref_catalog` invalidates an outstanding sample's token. *)
StaleAfterWrite == (obsStale \/ obsArmed)

(* The incarnation a `Create` installs. Honest: the allocator's next value, never reused. The
   allocator advances either way — the sabotage reuses the KEY, it does not stop the counter, and
   letting it advance is also what keeps the sabotaged configurations bounded. *)
NewInc == IF SabotageSameIncarnationRebirth /\ lastInc # 0 THEN lastInc ELSE nextInc

(* INV-3's janitor scope: an incarnation the catalog entry does not currently name. With the entry
   absent, EVERY incarnation is foreign — which is the point, and why removal may delete the entry
   without proving the prefix empty. *)
Foreign(i) == (entry.state = "absent") \/ (i # entry.inc)

Init ==
    /\ entry = [state |-> "absent", inc |-> 0]
    /\ terminal = FALSE
    /\ nextInc = 1
    /\ lastInc = 0
    /\ lives = 0
    /\ floors = {}
    /\ creatorInc = 0
    /\ creatorAlive = FALSE
    /\ creatorStale = FALSE
    /\ ckptDone = FALSE
    /\ installed = FALSE
    /\ objects = {}
    /\ ckptOf = {}
    /\ obsArmed = FALSE
    /\ obsStale = FALSE
    /\ orphanInc = 0
    /\ orphanAlive = FALSE
    /\ aliased = FALSE
    /\ aliasedOnRemnant = FALSE
    /\ reconcileHarm = FALSE
    /\ orphanDataEaten = FALSE
    /\ ckptOrphaned = FALSE

(* ---- Creation: three conditional writes at DDL rate (spec §3) ---- *)

(* Write 1 of 3: the catalog CAS that mints the incarnation and reserves the name, and the admission
   at which the creator captures its incarnation, its mount-fence generation and its catalog token.
   `Creating` forbids publication, so nothing but `_ckpt` may be written under the new prefix yet.

   One creator slot: a new admission overwrites the previous creator's captured state. See Scoping. *)
Create ==
    /\ entry.state = "absent"
    /\ nextInc <= MaxInc
    /\ entry' = [state |-> "creating", inc |-> NewInc]
    /\ lastInc' = NewInc
    /\ nextInc' = nextInc + 1
    /\ terminal' = FALSE
    /\ creatorInc' = NewInc
    /\ creatorAlive' = TRUE
    /\ creatorStale' = FALSE
    /\ ckptDone' = FALSE
    /\ installed' = FALSE
    /\ obsStale' = StaleAfterWrite
    /\ UNCHANGED << lives, floors, objVars, obsArmed, orphanInc, orphanAlive, ghostVars >>

(* The recovery open of spec §4, done here as the new life's FIRST touch: catalog -> `_ckpt` ->
   exact-key snapshot. A freshly minted incarnation's prefix is empty BY CONSTRUCTION, so finding
   anything there means this life is standing in a dead life's footprint — the aliasing INV-3 exists
   to make impossible. Guarded by `~ckptDone` because after its own first write the life's prefix is
   legitimately non-empty and the two are no longer distinguishable.

   Both object kinds are checked, and they are reached by different routes — which is why they are
   ghosted separately. `objects` is the remnant of a COMPLETED removal, the debris `EntryDelete`
   knowingly leaves behind, and it is the headline case. `ckptOf` needs no sabotage at all: a stale
   `Creating` that gets RECONCILED leaves its `_ckpt` behind as ordinary janitor food (reconciliation
   deletes the catalog entry, not the object), and that is in fact the SHORTEST route to an alias, so
   it is the one TLC reports. Both are real; `_witness_aliasremnant` is what stops the shorter route
   from hiding the longer one. *)
ReadOwn ==
    /\ entry.state = "creating"
    /\ ~ckptDone
    /\ aliased' = (aliased \/ (entry.inc \in (objects \cup ckptOf)))
    /\ aliasedOnRemnant' = (aliasedOnRemnant \/ (entry.inc \in objects))
    /\ UNCHANGED << entryVars, creatorVars, objVars, recVars, reconcileHarm, orphanDataEaten,
                    ckptOrphaned >>

(* Write 2 of 3: `<ns>/<inc>/_ckpt`, a conditional create carrying the admission fence generation
   (spec §3). A fence-terminal creator is REFUSED here — that refusal is the reason a reconciled
   creator cannot go on to manufacture a `_ckpt` for a name someone else now owns. A key conflict is
   fail-closed too: the creator cannot ack a `_ckpt` it did not write, so it never reaches `GoLive`
   and the name stays reserved in `Creating` until a reconciler takes it. *)
CkptCreate ==
    /\ entry.state = "creating"
    /\ creatorAlive
    /\ ~ckptDone
    /\ creatorInc \notin ckptOf
    /\ ckptDone' = TRUE
    /\ ckptOf' = ckptOf \cup {creatorInc}
    /\ UNCHANGED << entryVars, creatorInc, creatorAlive, creatorStale, installed, objects,
                    recVars, ghostVars >>

(* Write 3 of 3: the catalog CAS to `Live`, carrying BOTH credentials — the admission fence
   generation (§3) and the catalog token (INV-3: `ref_catalog` is token-CAS "like `gc/state`").
   It consults `ckptDone`, this creator's own ack, and NOT the store, because spec §3 buys creation
   for three conditional writes with no re-read. `ZombieGoLive` below is the same install with those
   two conjuncts removed, and is the whole reason they are written out separately here. *)
GoLive ==
    /\ ckptDone
    /\ ~installed
    /\ creatorAlive
    /\ ~creatorStale
    /\ entry.state = "creating"
    /\ entry.inc = creatorInc
    /\ entry' = [state |-> "live", inc |-> creatorInc]
    /\ installed' = TRUE
    /\ creatorStale' = TRUE
    /\ obsStale' = StaleAfterWrite
    /\ UNCHANGED << terminal, nextInc, lastInc, lives, floors, creatorInc, creatorAlive, ckptDone,
                    objVars, obsArmed, orphanInc, orphanAlive, ghostVars >>

(* THE ZOMBIE INSTALL. A creator that holds a `_ckpt` ack but has been fenced out — its mount fence
   went terminal, or the catalog was written by someone it never saw (a reconciliation, a removal) —
   still has an install to make, and it makes it BLIND. v9 refuses it twice over and this action
   drops both refusals at once, because a design that relied on only one of them would be gated by
   neither config.

   The damage does not need incarnation reuse. The install republishes the name as `Live` pointing at
   an incarnation whose life is over, and the pool is entitled to have cleaned that prefix already —
   the janitor's honest rule licensed it the moment the catalog stopped naming it. So the catalog
   ends up advertising `Live` over a prefix that has been swept: `INV_NEWBORN_SAFE`, from the other
   direction. If the install lands on top of a DIFFERENT running life it also orphans it, which is
   the same `reconcileHarm` damage a bad reconciliation causes; with one creator slot that variant
   is not reachable here (Scoping). *)
ZombieGoLive ==
    /\ SabotageZombieGoLive
    /\ ckptDone
    /\ ~installed
    /\ creatorInc # 0
    /\ (~creatorAlive \/ creatorStale)        \* EXACTLY the case `GoLive` refuses
    /\ entry # [state |-> "live", inc |-> creatorInc]
    /\ LET victimRunning == entry.state \in {"live", "removing"} /\ entry.inc # creatorInc
       IN /\ orphanInc'   = (IF victimRunning THEN entry.inc ELSE orphanInc)
          /\ orphanAlive' = (victimRunning \/ orphanAlive)
    /\ entry' = [state |-> "live", inc |-> creatorInc]
    /\ terminal' = FALSE
    /\ installed' = TRUE
    /\ creatorStale' = TRUE
    /\ obsStale' = StaleAfterWrite
    /\ UNCHANGED << nextInc, lastInc, lives, floors, creatorInc, creatorAlive, ckptDone,
                    objVars, obsArmed, ghostVars >>

(* Ordinary ref-layer work: an appended `_log` or an installed `_snap`. `Creating` forbids
   publication (spec §3); `Removing` still appends (its terminal record is a `_log` object). *)
WriteObject ==
    /\ entry.state \in {"live", "removing"}
    /\ entry.inc \notin objects
    /\ objects' = objects \cup {entry.inc}
    /\ UNCHANGED << entryVars, creatorVars, ckptOf, recVars, ghostVars >>

(* The owning writer's mount fence goes terminal — a crash, an unmount, a lost lease. Free to happen
   at any point of any life; it is the precondition reconciliation must WAIT for, not an anomaly.

   It FENCES the writer, it does not stop it. Everything the writer captured at admission survives
   this step, and the writer may still attempt every write it had planned; what changes is that those
   writes must now be refused. Conflating the two is what makes zombie writers invisible to a model,
   so `CkptCreate` and `GoLive` carry the check explicitly and `ZombieGoLive` exists to drop it. *)
CreatorDies ==
    /\ creatorAlive
    /\ creatorAlive' = FALSE
    /\ UNCHANGED << entryVars, creatorInc, creatorStale, ckptDone, installed, objVars, recVars,
                    ghostVars >>

(* ---- Reconciliation of a stale `Creating` (spec §3) ---- *)

(* The reconciler samples the catalog: it sees a `Creating` entry and remembers the token it read.
   Re-sampling refreshes the token, which is the honest way out of a staleness it notices. *)
ReconcileObserve ==
    /\ entry.state = "creating"
    /\ obsArmed' = TRUE
    /\ obsStale' = FALSE
    /\ UNCHANGED << entryVars, creatorVars, objVars, orphanInc, orphanAlive, ghostVars >>

(* Spec §3, both preconditions: "a stale `Creating` is reconciled by TOKEN-EXACT CAS only AFTER its
   creator's fence is terminal".

   `~obsStale` is the token-exact CAS: the entry has not been written since the sample, so the
   reconciler is provably deleting the entry it looked at. `~creatorAlive` is the fence.
   `SabotageReconcileStaleToken` turns the CAS into an unconditional delete, which is the more
   dangerous of the two — the victim is then whatever the catalog holds NOW, up to and including a
   `Live` successor's entry.

   This deletes the catalog ENTRY and nothing else. The creator it reclaims the name from keeps its
   incarnation, its fence and its `_ckpt` ack; all it loses is its catalog token, and that is
   precisely the credential whose absence must make its later install fail (`ZombieGoLive`).

   The damage is not recorded here as "a precondition was violated". It is recorded as an ORPHAN: a
   life that is still running while nothing in the catalog names its incarnation. That state is
   consequential twice over — the janitor's rule is "delete every incarnation the catalog does not
   name", so a running life's bytes become legal prey (`Janitor`), and the orphan itself keeps
   writing under a prefix no reader can ever attribute (`OrphanWrite`). The ghost fires at those two
   steps, never at this one, so a sabotage must actually reach the damage to be caught. *)
ReconcileCreating ==
    /\ obsArmed
    /\ entry.state # "absent"
    /\ \/ (~obsStale /\ entry.state = "creating" /\ (~creatorAlive \/ SabotageReconcileLiveCreator))
       \/ (SabotageReconcileStaleToken /\ obsStale)
    /\ LET victimRunning == \/ (entry.state = "creating" /\ creatorAlive)
                            \/ entry.state \in {"live", "removing"}
       IN /\ orphanInc'   = (IF victimRunning THEN entry.inc ELSE orphanInc)
          /\ orphanAlive' = (victimRunning \/ orphanAlive)
    /\ entry' = [state |-> "absent", inc |-> 0]
    /\ terminal' = FALSE
    /\ creatorStale' = TRUE
    /\ obsArmed' = FALSE
    /\ obsStale' = FALSE
    /\ UNCHANGED << nextInc, lastInc, lives, floors, creatorInc, creatorAlive, ckptDone, installed,
                    objVars, ghostVars >>

(* The orphan's fence finally goes terminal. After this its debris is ordinary garbage and the
   janitor may take it without harm — which is what keeps `reconcileHarm` about DESTROYING RUNNING
   WORK rather than about tidiness. *)
OrphanDies ==
    /\ orphanAlive
    /\ orphanAlive' = FALSE
    /\ UNCHANGED << entryVars, creatorVars, objVars, obsArmed, obsStale, orphanInc, ghostVars >>

(* The orphan does not know its entry is gone: it captured `(namespace, incarnation)` at admission
   and its writes are blind conditional PUTs under that prefix. Every byte it writes now is
   unattributable — no catalog entry names its incarnation, so no fold will ever account it and no
   exact-token delete will ever address it; only the zero-trust listing can find it at all. *)
OrphanWrite ==
    /\ orphanAlive
    /\ orphanInc # 0
    /\ objects' = objects \cup {orphanInc}
    /\ reconcileHarm' = TRUE
    /\ UNCHANGED << entryVars, creatorVars, ckptOf, recVars, aliased, aliasedOnRemnant,
                    orphanDataEaten, ckptOrphaned >>

(* ---- Removal (spec §3 + INV-4's ordering) ---- *)

(* `Live -> Removing` is the admission bound: `Removing` forbids new positive ownership. *)
Drop ==
    /\ entry.state = "live"
    /\ entry' = [entry EXCEPT !.state = "removing"]
    /\ creatorStale' = TRUE
    /\ obsStale' = StaleAfterWrite
    /\ UNCHANGED << terminal, nextInc, lastInc, lives, floors, creatorInc, creatorAlive, ckptDone,
                    installed, objVars, obsArmed, orphanInc, orphanAlive, ghostVars >>

(* The terminal record folds, and best-effort cleanup runs.

   ACTOR (spec §3): the terminal record is appended ONLY by the owning mounted writer, or by a
   successor that has claimed and fenced that server root. GC surfaces stuck removals and NEVER
   appends one. This module has no GC actor at all, so that rule is enforced by construction rather
   than tested — which is a modelling artifact, not evidence, and is recorded as such in Scoping.
   No fence conjunct here for the same reason: the action abstracts over "the owner, or a fenced
   successor", and at least one of those is always available.

   Cleanup is best-effort by design: the remnant branch is not a fault, it is the ordinary case that
   INV-3's inertness argument has to survive, and `_churn`/`_witness_churn3` deliberately run three
   lives that all leave one. *)
TerminalFoldAndCleanup ==
    /\ entry.state = "removing"
    /\ ~terminal
    /\ terminal' = TRUE
    /\ objects' \in {objects, objects \ {entry.inc}}
    /\ UNCHANGED << entry, nextInc, lastInc, lives, floors, creatorVars, ckptOf, recVars,
                    ghostVars >>

(* INV-4: `_ckpt` is deleted BY EXACT TOKEN WHILE THE ENTRY IS `Removing`. The entry is what names
   the incarnation and authorizes the delete, which is precisely why it must outlive the object. *)
RemovalCkptDelete ==
    /\ entry.state = "removing"
    /\ entry.inc \in ckptOf
    /\ ckptOf' = ckptOf \ {entry.inc}
    /\ UNCHANGED << entryVars, creatorVars, objects, recVars, ghostVars >>

(* INV-3/INV-4: the catalog entry goes LAST, and it goes with NO PHYSICAL-EMPTY PROOF — note what is
   NOT in this guard. `objects` may still hold this life's remnants and the entry is deleted anyway;
   the remnants are inert because the fold works only off catalog entries and their prefix is now
   foreign to every future life. That single omission is what makes the catalog O(`Creating` +
   `Live` + `Removing`) under churn instead of O(all names ever created).

   What IS in the guard is the `_ckpt` ordering. `SabotageEntryBeforeCkptDelete` removes it: the
   surviving `_ckpt` then has no catalog entry naming its incarnation, so no actor can ever address
   it by exact token again and it is reachable only through the zero-trust listing (§1) — which may
   omit it forever. *)
EntryDelete ==
    /\ entry.state = "removing"
    /\ terminal
    /\ (SabotageEntryBeforeCkptDelete \/ entry.inc \notin ckptOf)
    /\ ckptOrphaned' = (ckptOrphaned \/ (entry.inc \in ckptOf))
    /\ entry' = [state |-> "absent", inc |-> 0]
    /\ terminal' = FALSE
    /\ lives' = lives + 1
    /\ floors' = (IF SabotageFloorRetainsDeadName THEN floors \cup {entry.inc} ELSE floors)
    /\ creatorStale' = TRUE
    /\ obsStale' = StaleAfterWrite
    /\ UNCHANGED << nextInc, lastInc, creatorInc, creatorAlive, ckptDone, installed, objVars,
                    obsArmed, orphanInc, orphanAlive, aliased, aliasedOnRemnant, reconcileHarm,
                    orphanDataEaten >>

(* ---- the lazy janitor (INV-3) ---- *)

(* "A lazy janitor deletes foreign-incarnation debris whenever listed." Enabled per object and never
   forced: LIST is a zero-trust hint, so omission is always possible and is deferred cleanup, the
   leak-only direction. Its scope is the load-bearing part — an incarnation the catalog does not
   currently name — and `SabotageJanitorEatsNewborn` widens it to the entry's OWN incarnation while
   the entry is `Creating`, the classic shape of GC eating a newborn between its `_ckpt` create and
   its `Live` CAS.

   `orphanDataEaten` requires `i \in objects` and not merely `i \in objects \cup ckptOf`: eating an
   orphan's `_ckpt` is bad, but eating its `_log`/`_snap` is the DATA loss, and the witness that
   claims data loss has to be the one that observed it. *)
Janitor(i) ==
    /\ i \in (objects \cup ckptOf)
    /\ \/ Foreign(i)
       \/ (SabotageJanitorEatsNewborn /\ entry.state = "creating" /\ i = entry.inc)
    /\ reconcileHarm'   = (reconcileHarm \/ (orphanAlive /\ i = orphanInc))
    /\ orphanDataEaten' = (orphanDataEaten \/ (orphanAlive /\ i = orphanInc /\ i \in objects))
    /\ objects' = objects \ {i}
    /\ ckptOf' = ckptOf \ {i}
    /\ UNCHANGED << entryVars, creatorVars, recVars, aliased, aliasedOnRemnant, ckptOrphaned >>

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
    \/ Create \/ ReadOwn \/ CkptCreate \/ GoLive \/ ZombieGoLive \/ WriteObject \/ CreatorDies
    \/ ReconcileObserve \/ ReconcileCreating \/ OrphanDies \/ OrphanWrite
    \/ Drop \/ TerminalFoldAndCleanup \/ RemovalCkptDelete \/ EntryDelete
    \/ \E i \in Incs : Janitor(i)
    \/ NoOp

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

TypeOK ==
    /\ entry \in [state : States, inc : 0..MaxInc]
    /\ ((entry.state = "absent") <=> (entry.inc = 0))
    /\ objects \subseteq Incs
    /\ ckptOf \subseteq Incs
    /\ floors \subseteq Incs
    /\ nextInc \in 1..(MaxInc + 1)
    /\ lastInc \in 0..MaxInc
    /\ lives \in 0..MaxInc
    /\ creatorInc \in 0..MaxInc
    /\ orphanInc \in 0..MaxInc
    /\ creatorAlive \in BOOLEAN
    /\ creatorStale \in BOOLEAN
    /\ ckptDone \in BOOLEAN
    /\ installed \in BOOLEAN
    /\ terminal \in BOOLEAN
    /\ obsArmed \in BOOLEAN
    /\ obsStale \in BOOLEAN
    /\ orphanAlive \in BOOLEAN
    /\ aliased \in BOOLEAN
    /\ aliasedOnRemnant \in BOOLEAN
    /\ reconcileHarm \in BOOLEAN
    /\ orphanDataEaten \in BOOLEAN
    /\ ckptOrphaned \in BOOLEAN
    /\ (orphanAlive => orphanInc # 0)
    (* Each life is installed at most once, so completed lives never outrun minted incarnations —
       this is what keeps `lives` bounded even when a zombie install resurrects a name. *)
    /\ (lives <= nextInc - 1)
    (* The two witness ghosts are strict subsets of the invariants they refine, asserted here so a
       future edit cannot let a witness go red without its invariant going red too. *)
    /\ (aliasedOnRemnant => aliased)
    /\ (orphanDataEaten => reconcileHarm)

(* INV-3, incarnation inertness. A life never touches bytes another life wrote. This is the property
   that lets `EntryDelete` skip the physical-empty proof: debris is not dangerous because it is not
   REACHABLE from any future life, so there is no need to prove it gone.

   In THIS model the only route to `aliased` is minting a used incarnation, and that is a statement
   about the model's reach, not a theorem: the adjacent way for a catalog to end up naming a dead
   life's prefix — a fenced-out creator's late install — is representable here too, and it is caught
   by `INV_NEWBORN_SAFE` rather than by this invariant. *)
INV_NO_ALIAS == ~aliased

(* Spec §3, the creation ORDER: `_ckpt` create precedes the catalog `Live` CAS, so a `Live` entry
   always has its `_ckpt`. Stated over state, not over the janitor's step, deliberately: the damage
   of eating a newborn is not the deletion (a `Creating` life that never goes `Live` loses nothing
   worth having) but the moment its creator publishes `Live` on the strength of an ack for an object
   the pool no longer holds — INV-4's "a cleaned prefix plus a hidden snapshot is indistinguishable
   from empty", entered through the front door. Two sabotages reach it from opposite ends: the
   janitor deleting the object under a live creator, and a fenced-out creator installing over a
   prefix the janitor had every right to have swept. *)
INV_NEWBORN_SAFE == (entry.state = "live") => (entry.inc \in ckptOf)

(* INV-3's own sentence: the catalog is O(`Creating` + `Live` + `Removing`) under any create/drop
   churn. With one name that reads as "the catalog holds a record exactly while the name is in one
   of those three states, and none after" — no record outlives its name's life, which is what §10
   rejected the `seq_floor` alternative for ("floors for dead names never retire -> unbounded
   catalog"). The multi-name bound follows by summation over independent names; this model checks
   the per-name term, which is the one the incarnation argument is responsible for. *)
CatalogSize == (IF entry.state = "absent" THEN 0 ELSE 1) + Cardinality(floors)
LiveNames   == IF entry.state = "absent" THEN 0 ELSE 1
INV_BOUNDED_CATALOG == CatalogSize = LiveNames

(* Spec §3's two reconciliation preconditions, stated over the damage they prevent: a life that is
   still running while the catalog no longer names its incarnation had its bytes destroyed by the
   janitor, or wrote bytes nothing can ever attribute. Both are steps a counterexample must actually
   REACH — neither is the sabotaged reconciliation itself. *)
INV_RECONCILE_SAFE == ~reconcileHarm

(* INV-4's removal ordering: exact-token `_ckpt` delete while the entry is `Removing`, catalog entry
   LAST. Restates the ordering as a precondition rather than deriving a downstream consequence, and
   that is the honest limit of this module: with incarnations unique, an orphaned `_ckpt` is inert,
   so its only consequence is a leak that the spec already tolerates in the janitor-omission
   direction. What the invariant does buy is the COMBINATION — an orphaned `_ckpt` is one of the two
   things a reused incarnation can alias onto (see `ReadOwn`), so the ordering is the second,
   independent line of defence and the model keeps it separable. *)
INV_CKPT_ORDER == ~ckptOrphaned

(* The task brief's proposed reconciliation invariant, kept verbatim as a FINDING, not as a
   property: `_finding_briefreconcileinv` is its counterexample in the fully honest model, where
   `CreatorDies` before `CkptCreate` produces exactly this shape and nothing is wrong. *)
INV_RECONCILE_SAFE_BRIEF ==
    (entry.state = "creating") => (ckptOf # {} \/ creatorAlive \/ entry.inc = 0)

(* ---- witnesses: negated, so a VIOLATION is the evidence ----

   All three exist for the same reason. Breadth-first search reports the SHORTEST counterexample, so
   an invariant reachable two ways only ever shows the near one, and the far one silently rots into a
   dead branch that no run would notice. Each witness pins one route that the corresponding
   invariant's own counterexample does not travel. *)

(* Non-vacuity for `_churn`. The user's create/drop-per-second scenario needs the cycles to actually
   complete, and to complete WITH debris outstanding — a green whose lifecycle silently wedged at the
   first remnant would be green on every safety invariant and would prove nothing. *)
WITNESS_CHURN == ~(lives = MaxInc /\ objects # {})

(* The headline aliasing route, which `INV_NO_ALIAS`'s own counterexample skips: a COMPLETED removal
   left a `_log`/`_snap` remnant and a reborn incarnation read it. TLC instead reports the shorter
   route through a reconciled creator's surviving `_ckpt`. Both are real; this pins the one the
   inertness argument is actually about. Checked in `_witness_aliasremnant`. *)
WITNESS_ALIAS_REMNANT == ~aliasedOnRemnant

(* The severe arm of `INV_RECONCILE_SAFE`, which its own counterexample skips: not "the orphan wrote
   unattributable bytes" (`OrphanWrite`, one step past the sabotage) and not the janitor taking only
   the orphan's `_ckpt`, but the janitor DELETING a running life's `_log`/`_snap` data, licensed by
   its honest rule the moment the catalog stopped naming that incarnation. Checked in
   `_witness_orphaneaten`. *)
WITNESS_ORPHAN_EATEN == ~orphanDataEaten

=============================================================================
