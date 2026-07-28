# CaRefCatalogCore — TLA+ gate results (v9)

Model: `CaRefCatalogCore.tla`, a NEW module. Gates the namespace catalog of spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §2 **INV-3** (`cas/ref_catalog`,
ref-layer-scoped incarnations, structural inertness of surviving debris, the
O(`Creating` + `Live` + `Removing`) churn bound), §2 **INV-4** (`_ckpt`, and its removal ordering:
exact-token delete while `Removing`, catalog entry LAST) and §3 **Lifecycles** (creation's three
conditional writes, the admission fence generation required on each, reconciliation of a stale
`Creating`, removal and its terminal record). Task 3 of the plan
`2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0 gate — it blocks the C++ work.

Runner: `./run_refcatalog.sh` (runs every config and checks its expected verdict, including *which*
invariant a sabotage is required to break; sabotages run FIRST, because a green is only evidence
once the property it rests on has been seen red). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers 1`. Every number below is real TLC output from the run of
2026-07-28 (fix round 1), not an estimate.

**`-workers 1`, not `-workers auto`, and that is load-bearing for this document.** Parallel BFS
visits states in a nondeterministic order, so with `auto` the reported depth, the abort runs' state
counts, and — the part that matters — *which* shortest counterexample TLC prints all vary between
identical runs: three consecutive `auto` runs of `_safe` reported depth 19, 19, 20. Every trace
narrated below and in the cfg headers is a specific action sequence, so the run that produces them
has to be reproducible. Fix round 1 re-recorded every number from a single-worker run after the
first revision's `auto` numbers were found not to match a re-run.

Constants: `MaxInc = 3` in every config except `_safe` (`MaxInc = 2`, the lifecycle gate), so every
sabotage/control pair is like-for-like. `MaxInc` is the only constant besides the seven sabotage
toggles, and it is **not load-bearing**: `BOUND=5 ./run_refcatalog.sh` re-runs the entire suite at
`MaxInc = 5` and all thirteen expectations hold unchanged, still about a second per config. (At
`BOUND=5` `_safe` and `_churn` become byte-identical runs, so that sweep is 12 distinct checks
rather than 13.)

## Headline

1. **The user's churn objection is answered as an executable red/green pair, not as an argument.**
   §10 rejected `seq_floor` because "floors for dead names never retire → unbounded catalog".
   `_sab_floorretainsdeadname` is that design: removal keeps a per-dead-life record instead of
   deleting the entry, and `INV_BOUNDED_CATALOG` — which is INV-3's own sentence, *the catalog holds
   a record exactly while the name is* `Creating`/`Live`/`Removing` *and none after* — goes red at
   the first completed drop. `_churn` runs three full create → drop → recreate cycles of the same
   name with debris outstanding the whole time and is green.
2. **Incarnation-scoped inertness is what buys `EntryDelete` its missing physical-empty proof, and
   the model shows the exact price of losing it.** `_sab_sameincarnationrebirth` reuses a dead life's
   incarnation, and the very first thing the new life does — the recovery open of spec §4 — reads the
   dead life's bytes at its own prefix (`INV_NO_ALIAS` red). Removal is allowed to leave debris
   behind *only* because no future life can name it. Two independent routes reach the alias and both
   are committed: a completed removal's `_log`/`_snap` remnant (`_witness_aliasremnant`) and a
   reconciled stale creator's surviving `_ckpt` (the shorter route, which is what TLC reports).
3. **Reconciliation needs BOTH of spec §3's preconditions, and the token-exact one is the dangerous
   half.** The task brief modelled only the fence. `_sab_reconcilestaletoken` — reconciling on a
   stale sample, i.e. an unconditional delete rather than a token-exact CAS — destroys a `Live`
   successor's catalog entry, and then the janitor, *behaving entirely correctly* by its own rule
   ("delete every incarnation the catalog does not name"), deletes that running namespace's
   `_log`/`_snap` data (`_witness_orphaneaten`). Nothing else in v9 stands between a stale reconciler
   and a live namespace's bytes.
4. **A fenced-out creator is FENCED, not erased — and the two credentials that refuse its late
   install are both load-bearing.** Deleting a creator's catalog entry does not delete the creator: it
   keeps its incarnation, its mount fence and its `_ckpt` ack, and it will finish its creation blind
   unless refused. `_sab_zombiegolive` drops spec §3's admission fence check *and* INV-3's catalog
   token-CAS together, and a stalled creator republishes the name as `Live` over a prefix the janitor
   had every right to have already swept — `INV_NEWBORN_SAFE` red, reached from the opposite end to
   `_sab_janitoreatsnewborn`, and **needing no incarnation reuse at all**. This is the aliasing route
   the incarnation argument does *not* close. Added in fix round 1; the first revision collapsed the
   creator into its catalog entry, which made the whole failure mode unrepresentable.
5. **The task brief's proposed `INV_RECONCILE_SAFE` is not a safety property, and the model says so
   with a counterexample rather than a paragraph.** `_finding_briefreconcileinv` runs the FULLY
   HONEST catalog against the brief's formula and it is red in three states: a creator whose fence
   goes terminal before its `_ckpt` create produces exactly that shape, and nothing is wrong — it is
   the state reconciliation exists to clean up. The formula is a liveness wish. The property is
   restated over the damage instead. See *Discrepancies*.
6. **`INV_CKPT_ORDER` is honest about its own limits.** INV-4's ordering rule is red under
   `_sab_entrybeforeckptdelete`, but the damage in a model with unique incarnations is a permanent
   *leak* — an object no actor can ever address by exact token again, reachable only through the
   zero-trust listing — not aliasing. The ordering is a second, independent line of defence behind
   incarnation freshness; it is not what stops rebirth aliasing. Recorded here so the implementation
   plan does not over-claim it.

## Summary table

Sabotages first. For a **violation** run, `states` is how many distinct states TLC explored before
aborting and `trace` is the counterexample's length in states; both are deterministic under
`-workers 1`. For a **green** run, `states` is the exhaustive distinct-state count (`0 states left
on queue`) and `trace` is the depth of the complete state graph.

| config | expect | result | s | states | trace |
|---|---|---|---|---|---|
| `_sab_janitoreatsnewborn` | violation | `INV_NEWBORN_SAFE` | 1 | 18 | 5 |
| `_sab_zombiegolive` | violation | `INV_NEWBORN_SAFE` | 0 | 53 | 8 |
| `_sab_reconcilelivecreator` | violation | `INV_RECONCILE_SAFE` | 1 | 21 | 5 |
| `_sab_reconcilestaletoken` | violation | `INV_RECONCILE_SAFE` | 0 | 42 | 7 |
| `_sab_entrybeforeckptdelete` | violation | `INV_CKPT_ORDER` | 1 | 32 | 7 |
| `_sab_sameincarnationrebirth` | violation | `INV_NO_ALIAS` | 1 | 47 | 8 |
| `_sab_floorretainsdeadname` | violation | `INV_BOUNDED_CATALOG` | 0 | 46 | 8 |
| `_finding_briefreconcileinv` | violation | `INV_RECONCILE_SAFE_BRIEF` | 1 | 4 | 3 |
| `_safe` (`MaxInc = 2`) | green | green | 0 | 304 | 19 |
| `_churn` | green | green | 1 | 1221 | 27 |
| `_witness_churn3` | violation | `WITNESS_CHURN` | 0 | 1127 | 23 |
| `_witness_aliasremnant` | violation | `WITNESS_ALIAS_REMNANT` | 1 | 99 | 11 |
| `_witness_orphaneaten` | violation | `WITNESS_ORPHAN_EATEN` | 1 | 61 | 8 |

`ALL EXPECTATIONS MET`, thirteen of thirteen, both at `MaxInc = 3` and under `BOUND=5`.

## The properties

`INV_NO_ALIAS == ~aliased` — **INV-3, incarnation inertness.** A life never touches bytes another
life wrote. This is the property that lets `EntryDelete` skip the physical-empty proof: debris is not
dangerous because it is not *reachable* from any future life, so there is no need to prove it gone.
In this model the only route to `aliased` is minting a used incarnation — a statement about the
model's reach, not a theorem; the adjacent way for a catalog to end up naming a dead life's prefix
(a fenced-out creator's late install) is representable too and is caught by `INV_NEWBORN_SAFE`.

`INV_NEWBORN_SAFE == (entry.state = "live") => (entry.inc \in ckptOf)` — **spec §3's creation
order.** `_ckpt` create precedes the catalog `Live` CAS, so a `Live` entry always has its `_ckpt`.
Stated over state rather than over the janitor's step deliberately: the damage of eating a newborn is
not the deletion (a `Creating` life that never goes `Live` loses nothing worth having) but the moment
its creator publishes `Live` on the strength of an ack for an object the pool no longer holds —
INV-4's "a cleaned prefix plus a hidden snapshot is indistinguishable from empty", entered through
the front door. Two sabotages reach it from opposite ends: the janitor deleting the object under a
live creator, and a fenced-out creator installing over a prefix the janitor had every right to sweep.

`INV_BOUNDED_CATALOG == CatalogSize = LiveNames` — **INV-3's O(`Creating` + `Live` + `Removing`)
churn bound.** With one name that reads as "no record outlives its name's life". The multi-name bound
follows by summation over independent names; this model checks the per-name term, which is the one
the incarnation argument is responsible for.

`INV_RECONCILE_SAFE == ~reconcileHarm` — **spec §3's two reconciliation preconditions, stated over
the damage they prevent.** A life that is still running while the catalog no longer names its
incarnation has had its bytes destroyed by the janitor, or has written bytes nothing can ever
attribute. Both are steps a counterexample must actually REACH; neither is the sabotaged
reconciliation itself.

`INV_CKPT_ORDER == ~ckptOrphaned` — **INV-4's removal ordering.** Exact-token `_ckpt` delete while
the entry is `Removing`, catalog entry LAST. Scoped to the removal path: reconciliation also destroys
an entry with a `_ckpt` behind it, but that is not an ordering violation, because a reconciled
`Creating` never reached `Removing` and so no exact-token delete was ever owed (see *Scoping*). This
is the one invariant here that restates a precondition rather than deriving a downstream consequence.

`TypeOK` additionally asserts `aliasedOnRemnant => aliased` and `orphanDataEaten => reconcileHarm`,
so a future edit cannot let a witness go red without its invariant going red too, and
`lives <= nextInc - 1`, which is what keeps the churn counter bounded once a zombie install can
resurrect a name.

## The counterexamples, one per sabotage

Action sequences are verbatim from the `-workers 1` logs.

**`_sab_janitoreatsnewborn` → `INV_NEWBORN_SAFE`, 5 states.** `Create` (inc 1, `Creating`) →
`CkptCreate` (the creator's conditional PUT acks; `ckptOf = {1}`) → `Janitor` deletes inc 1 while it
is the entry's OWN incarnation → `GoLive`. The final state is a `Live` catalog entry whose `_ckpt`
object does not exist. The violation is the publish, not the delete: creation is three blind
conditional writes with no re-read (spec §3), so the creator goes `Live` on its own ack and has no
way to notice.

**`_sab_zombiegolive` → `INV_NEWBORN_SAFE`, 8 states.** `Create` (inc 1) → `CkptCreate` →
`CreatorDies` (the fence goes terminal — the creator is fenced, not stopped) → `ReconcileObserve` →
`ReconcileCreating` → `Janitor` → `ZombieGoLive`. Everything except the last step is honest: the
reconciliation is token-exact and fence-terminal, and the janitor is collecting a `_ckpt` that became
foreign debris the moment the catalog stopped naming it. The stalled creator then wakes up and
installs `entry = (live, 1)` — a `Live` namespace pointing at a swept prefix. v9 refuses this twice
(the admission fence generation, §3; the catalog token-CAS, INV-3) and this config drops both,
because a design relying on only one of them would be gated by neither.

**`_sab_reconcilelivecreator` → `INV_RECONCILE_SAFE`, 5 states.** `Create` → `ReconcileObserve`
(a reconciler samples the `Creating` entry) → `ReconcileCreating` fires *without* the creator's fence
being terminal → `OrphanWrite`: that creator, which captured `(namespace, incarnation)` at admission
and writes blind conditional PUTs under it, keeps writing into a prefix nothing in the catalog names.

**`_sab_reconcilestaletoken` → `INV_RECONCILE_SAFE`, 7 states.** The severe one. `Create` →
`CkptCreate` → `ReconcileObserve` samples while the entry is `Creating` → `GoLive` (the life proceeds
normally and the sample goes stale) → `ReconcileCreating` acts on that stale sample and deletes the
**`Live`** entry → `OrphanWrite`. `_witness_orphaneaten` shows the worse ending.

**`_sab_entrybeforeckptdelete` → `INV_CKPT_ORDER`, 7 states.** A complete honest life — `Create`,
`CkptCreate`, `GoLive`, `Drop`, `TerminalFoldAndCleanup` — then `EntryDelete` with `ckptOf = {1}`
still populated. The catalog entry is the only record that names the incarnation and authorizes the
exact-token delete, so once it is gone the surviving `_ckpt` can never be addressed by token again —
only the zero-trust listing can find it, and the listing may omit it forever.

**`_sab_sameincarnationrebirth` → `INV_NO_ALIAS`, 8 states.** `Create` (inc 1) → `CkptCreate` →
`CreatorDies` → `ReconcileObserve` → `ReconcileCreating` (an honest, token-exact, fence-terminal
reconciliation; the creator's `_ckpt` is left behind as ordinary janitor food, which is correct) →
`Create` REUSES inc 1 → `ReadOwn`, the new life's recovery open, reads the dead creator's `_ckpt` at
its own prefix. Note what is *not* sabotaged: the reconciliation and the leftover `_ckpt` are both
honest. Only the incarnation reuse is wrong, and that alone is enough.

**`_sab_floorretainsdeadname` → `INV_BOUNDED_CATALOG`, 8 states.** A complete honest life —
`Create`, `CkptCreate`, `GoLive`, `Drop`, `TerminalFoldAndCleanup`, `RemovalCkptDelete` — then
`EntryDelete` retains `floors = {1}`. The name is now absent and the catalog still holds a record for
it. Under churn the count grows with the cycles instead of staying at the live-name count.

**`_finding_briefreconcileinv` → `INV_RECONCILE_SAFE_BRIEF`, 3 states.** No sabotage is enabled.
`Create` → `CreatorDies`. The entry is `Creating`, the fence is terminal and no `_ckpt` exists yet —
a legitimate transient state, and precisely the one reconciliation exists to clean up.

## Non-vacuity of the green runs

Both greens explore their state space to exhaustion (`0 states left on queue`): `_safe` 304 distinct
states at depth 19, `_churn` 1221 at depth 27. Action coverage for `_churn` (`-coverage 1`,
`distinct:generated`):

```
Create 30:56   ReadOwn 0:100   CkptCreate 30:50   GoLive 40:50   ZombieGoLive 0:0
WriteObject 40:400   CreatorDies 140:580   ReconcileObserve 80:200   ReconcileCreating 40:40
OrphanDies 0:0   OrphanWrite 0:0
Drop 160:160   TerminalFoldAndCleanup 160:480   RemovalCkptDelete 320:320   EntryDelete 160:160
Janitor 20:1496   NoOp 0:1221
```

Three actions do not fire, and each is correct: `ZombieGoLive` is sabotage-guarded and so contributes
nothing to a green run's state space at all; `OrphanDies` and `OrphanWrite` are unreachable because an
honest reconciliation never produces an orphan. They are exercised in `_sab_zombiegolive`,
`_sab_reconcilelivecreator`, `_sab_reconcilestaletoken` and `_witness_orphaneaten`. `ReadOwn 0:100`
and `NoOp 0:1221` generate no distinct states because they are self-loops when their ghost does not
change — also correct.

Three witnesses carry the rest of the non-vacuity argument. All are negated, so a VIOLATION is the
evidence, and all exist for the same reason: breadth-first search reports the SHORTEST
counterexample, so an invariant reachable two ways only ever shows the near one and the far route
silently rots into a dead branch no run would notice.

**`_witness_churn3` → `WITNESS_CHURN`, 23 states.** Three complete create → drop → recreate cycles
of the one name, ending with `lives = 3`, `entry` absent and `objects = {1}` — debris from the first
life still sitting in the pool through both later ones. This is the shape the churn green has to be
about: a lifecycle that quietly wedged at the first remnant (which is what a physical-empty-proof
design does) would also be green on every safety invariant and would be worthless.

**`_witness_aliasremnant` → `WITNESS_ALIAS_REMNANT`, 11 states.** The headline aliasing route, which
`INV_NO_ALIAS`'s own counterexample skips: a full life 1 including `WriteObject`, a
`TerminalFoldAndCleanup` that leaves `objects = {1}` behind, `RemovalCkptDelete`, `EntryDelete` — and
then a reborn inc 1 reading that remnant. This is the debris `EntryDelete` *knowingly* leaves; the
shorter route through a reconciled creator's `_ckpt` is real but is not what the inertness argument
is about.

**`_witness_orphaneaten` → `WITNESS_ORPHAN_EATEN`, 8 states.** The severe arm of
`INV_RECONCILE_SAFE`. `Create`, `CkptCreate`, `ReconcileObserve`, `GoLive`, `WriteObject` — so the
namespace is `Live` with a real ref layer (`objects = {1}`, `ckptOf = {1}`) — then
`ReconcileCreating` on the stale sample destroys its catalog entry, and `Janitor` deletes the running
life's `_log`/`_snap` **data** along with its `_ckpt`. The ghost requires `i \in objects`
specifically: eating an orphan's `_ckpt` is bad, but eating its data is the loss this witness claims,
and the witness that claims data loss has to be the one that observed it. (Fix round 1: before the
strengthening the ghost also fired on a `_ckpt`-only deletion, and the committed trace did not show
the data loss the narrative described.) The janitor is not sabotaged here and is not misbehaving —
its rule is "delete every incarnation the catalog does not name", and a reconciliation that was
neither token-exact nor fence-terminal is what handed it a live namespace to apply that rule to.

## What the model is

ONE logical namespace name, lived over and over, with everything keyed by INCARNATION
(`<ns>/<inc>/{_log,_snap,_ckpt}`). The question v9 has to answer is not what one life does but what
SURVIVES a life and what the next life makes of it, so the oracle is not "is the pool tidy" — it
never is — but "did a new life ever touch an old life's bytes, and did the catalog ever keep a record
a name no longer has".

**The creator is a separate actor from its catalog entry.** A creator captures
`(incarnation, mount-fence generation, catalog token)` at admission (`creatorInc`, `creatorAlive`,
`creatorStale`) and thereafter writes blind. Reconciliation, removal and a successor's `Create`
FENCE it out; they do not erase it. What stops a fenced-out creator from finishing its creation on
top of whatever the catalog now holds is that its two captured credentials no longer validate —
modelled explicitly as conjuncts of `CkptCreate` and `GoLive`, with `ZombieGoLive` as the action that
drops them. `ckptDone` (its own PUT's ack) is deliberately distinct from `creatorInc \in ckptOf` (does
the object exist now), and that gap is where the newborn bug lives.

Both token-CAS readers are modelled without token counters: a boolean that every catalog write sets
(`creatorStale` for the creator, `obsStale` for the reconciler's sample). That is exactly as strong
as a counter, because token equality *is* "unwritten since captured", and it keeps the state space at
four figures.

### The three adversaries, and why they are not sabotages

Enabled in GREEN configurations too:

- **`TerminalFoldAndCleanup` leaves remnants nondeterministically.** Best-effort cleanup is the
  ordinary case that INV-3's inertness argument has to survive, not a fault. `_churn` and
  `_witness_churn3` deliberately run three lives that all leave one.
- **The janitor is lazy and may omit any object forever.** LIST is a zero-trust hint (§1), so
  omission is always possible; INV-3 calls it deferred cleanup, the leak-only direction.
- **The owning fence may go terminal at any point of any life** (`CreatorDies`). It is the
  precondition reconciliation must WAIT for, not an anomaly — and it fences the writer rather than
  stopping it, which is what makes the zombie install representable.

## Discrepancies with the task brief, and why the spec won

**1. `INV_RECONCILE_SAFE`.** The brief proposed

```tla
INV_RECONCILE_SAFE == (entry.state = "creating") => (ckptOf # {} \/ creatorAlive \/ entry.inc = 0)
```

which is red in the honest model in three states (`_finding_briefreconcileinv`). It is a liveness
wish — "a stalled `Creating` should not linger" — dressed as an invariant, and the state it forbids
is the legitimate input to reconciliation. The formula is retained verbatim in the module as
`INV_RECONCILE_SAFE_BRIEF` with its counterexample committed as a configuration, so the discrepancy
is evidence rather than prose. The name `INV_RECONCILE_SAFE` is kept for the main plan's tests, with
the damage-based content.

**2. `newbornEaten`.** The brief's ghost is set by the sabotage's own step, so its counterexample
would be tautological — the Task-2 lesson (`holdDebt`) applied. `INV_NEWBORN_SAFE` is instead the
state invariant the creation order buys, which the eaten newborn violates only once its creator goes
on to publish `Live`: the consequence, not the act. The ghost variable is gone; the invariant name is
unchanged.

**3. `INV_BOUNDED_CATALOG == TRUE`.** The brief made it structurally vacuous ("the churn cfg's bound
is the check"). A `TRUE` invariant can never be observed red, which this phase's convention forbids.
It is now INV-3's own sentence with `_sab_floorretainsdeadname` as its red witness — the executable
form of the alternative §10 records the user rejecting.

**4. `SabotageReconcileStaleToken` added.** Spec §3 states two preconditions ("by token-exact CAS
only after its creator's fence is terminal") and the brief modelled one. The unmodelled one turned
out to be the dangerous half (headline 3).

**5. `INV_CKPT_ORDER` and `_sab_entrybeforeckptdelete`'s expectation.** The brief expected the
ordering sabotage to be red on `INV_NO_ALIAS` via a `_ckpt` collision, and correctly noted that this
requires same-incarnation rebirth. With incarnations unique it cannot happen, so the sabotage would
have been a green config named `_sab_*`. It is red on a new invariant that names the damage that does
occur — a permanently unaddressable object. Scope stated in headline 6.

**6. `SabotageZombieGoLive` added (fix round 1).** Neither the brief nor the first revision modelled
the admission fence generation §3 requires on the `_ckpt` CAS and the install, which let a
fence-terminal creator publish and made the zombie-install failure mode unrepresentable. Headline 4.

**7. Four extra configs** beyond the brief's six — three witnesses and the zombie sabotage — plus
`BOUND`. Cost: about five seconds total.

## Scoping — what this model deliberately does not cover

- **`INV_CKPT_ORDER` restates its precondition.** With incarnations unique, an orphaned `_ckpt` is
  inert, so its only consequence is a leak that the spec already tolerates in the janitor-omission
  direction. There is no downstream state in this model that the leak corrupts, and inventing one
  would be modelling fiction. The invariant is therefore weaker evidence than the other five, and it
  is marked as such rather than dressed up.
- **The reconciliation path also strands a `_ckpt`, and that is not counted as an ordering
  violation.** When a stale `Creating` is reconciled, its `_ckpt` survives with no catalog entry
  naming it — the same physical residue `_sab_entrybeforeckptdelete` produces, and the residue the
  rebirth counterexample aliases onto. It is not an INV-4 violation because a reconciled `Creating`
  never reached `Removing`, so no exact-token delete was ever owed; the object is ordinary
  foreign-incarnation debris on the leak-only path. `ckptOrphaned` is scoped to the removal path for
  that reason. Whether the *implementation* should nevertheless best-effort delete a reconciled
  creator's `_ckpt` at reconciliation time is a plan question this model does not answer.
- **One creator slot, one reconciler sample slot, one orphan slot, one name.** A new admission
  overwrites the previous creator's captured state, so two creators never coexist. The direct
  consequence: the zombie install can be shown landing on an ABSENT entry (`_sab_zombiegolive`) but
  not landing on top of a *different* running life's `Live` entry — the strictly worse instance of
  the same ungated write, which would need a second creator slot. It is the same guard either way, so
  the gate is not weakened; only the worst-case severity is argued rather than executed. Likewise a
  genuinely cross-name hazard (a reconciler or janitor confusing two names) is out of scope; INV-3's
  r9-3 closure — the typed `RefNamespaceId` — is the mechanism that makes such a confusion
  unrepresentable in code, and it is an API-shape obligation, not a protocol one.
- **§3's fence generation is modelled only on the two writes this module owns.** `CkptCreate` and
  `GoLive` carry it. The third site §3 names, `slot-occupy`, belongs to `CaRefTableSnapshotLogCore`
  (Task 1), and the generation's own lifecycle — how it is issued, observed to be terminal, and
  rechecked under the install lock — belongs to `CaCasMountCore` (Task 5, the recovery-generation
  work). This module treats `creatorAlive` as an oracle for "the generation still validates" and
  proves only what depends on the check being *performed*.
- **INV-3's additive capacity predicate is not modelled.** The byte bound on namespace names, the
  encoded-catalog-plus-worst-case-reservation admission check and the `encodeFoldSeal(...).size()`
  cap are size arithmetic, not concurrency; a TLA+ model of them would be a spreadsheet. §9 lists
  them under the plan's boundary and boundary-plus-one tests, which is the right instrument. What
  this module does cover is the other half of INV-3's capacity story — that the *number* of entries
  stays bounded under churn.
- **No GC actor.** Spec §3's "GC surfaces stuck removals, never appends" is enforced here by
  construction — the module simply has no appending GC action — which is a modelling artifact, not
  evidence. A reviewer wanting that rule tested would need a GC actor this module does not have
  (`CaRefDeltaIntakeCore` has one).
- **`WriteObject` stays enabled while `Removing`, including after the terminal record has folded.**
  Real code stops appending at the terminal record; here the only effect of a late write is one more
  inert object at the dying life's prefix, which is debris the inertness argument already has to
  survive. Nothing in the model distinguishes debris created before the terminal record from debris
  created after it, so the extra freedom costs nothing and removes a guard that would need its own
  justification.
- **Recovery is abstracted to `ReadOwn`.** Spec §4's full sequence (catalog → `_ckpt` → exact-key
  snapshot → arithmetic tail → CAS-walk + seal) is `CaRefTableSnapshotLogCore`'s subject (Task 1).
  Here it appears only as "the new life's first touch reads its own prefix", which is all the
  aliasing question needs.
- **Incarnations are a counter, not 128 random bits.** The only property used anywhere is FRESHNESS;
  nothing in the module depends on ordering. The real generator's collision probability is a
  different (and non-temporal) argument.

## Reproduce

```bash
cd docs/superpowers/models
./run_refcatalog.sh            # 13 configs, ~8s total, deterministic under -workers 1
BOUND=5 ./run_refcatalog.sh    # same 13 expectations at MaxInc = 5
```

Logs land in `tmp/tlc_CaRefCatalogCore_<config>.log`.
