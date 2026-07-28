# CaRefCatalogCore — TLA+ gate results (v9)

Model: `CaRefCatalogCore.tla`, a NEW module. Gates the namespace catalog of spec
`2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §2 **INV-3** (`cas/ref_catalog`,
ref-layer-scoped incarnations, structural inertness of surviving debris, the
O(`Creating` + `Live` + `Removing`) churn bound), §2 **INV-4** (`_ckpt`, and its removal ordering:
exact-token delete while `Removing`, catalog entry LAST) and §3 **Lifecycles** (creation's three
conditional writes, reconciliation of a stale `Creating`, removal and its terminal record). Task 3
of the plan `2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0 gate — it blocks the C++ work.

Runner: `./run_refcatalog.sh` (runs every config and checks its expected verdict, including *which*
invariant a sabotage is required to break; sabotages run FIRST, because a green is only evidence
once the property it rests on has been seen red). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers auto`, 32 cores. Every number below is real TLC output from the
run of 2026-07-28, not an estimate.

Constants: `MaxInc = 3` in every config except `_safe` (`MaxInc = 2`, the lifecycle gate), so every
sabotage/control pair is like-for-like. `MaxInc` is the only constant besides the six sabotage
toggles, and it is **not load-bearing**: `BOUND=5 ./run_refcatalog.sh` re-runs the entire suite at
`MaxInc = 5` and all twelve expectations hold unchanged, still under a second per config.

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
   half.** The brief modelled only the fence. `_sab_reconcilestaletoken` — reconciling on a stale
   sample, i.e. an unconditional delete rather than a token-exact CAS — destroys a `Live`
   successor's catalog entry, and then the janitor, *behaving entirely correctly* by its own rule
   ("delete every incarnation the catalog does not name"), deletes that running namespace's whole
   ref layer (`_witness_orphaneaten`). Nothing else in v9 stands between a stale reconciler and a
   live namespace's data.
4. **The task brief's proposed `INV_RECONCILE_SAFE` is not a safety property, and the model says so
   with a counterexample rather than a paragraph.** `_finding_briefreconcileinv` runs the FULLY
   HONEST catalog against the brief's formula and it is red in three states: a creator whose fence
   goes terminal before its `_ckpt` create produces exactly that shape, and nothing is wrong — it is
   the state reconciliation exists to clean up. The formula is a liveness wish. The property is
   restated over the damage instead. See *Discrepancies*.
5. **`INV_CKPT_ORDER` is honest about its own limits.** INV-4's ordering rule is red under
   `_sab_entrybeforeckptdelete`, but the damage in a model with unique incarnations is a permanent
   *leak* — an object no actor can ever address by exact token again, reachable only through the
   zero-trust listing — not aliasing. The ordering is a second, independent line of defence behind
   incarnation freshness; it is not what stops rebirth aliasing. Recorded here so the implementation
   plan does not over-claim it.

## Summary table

Sabotages first. `distinct` is TLC's distinct-state count; sabotage runs stop at the first violation,
so only the two greens and the three witnesses explore their state space to exhaustion.

| config | expect | result | s | distinct | depth |
|---|---|---|---|---|---|
| `_sab_janitoreatsnewborn` | violation | `INV_NEWBORN_SAFE` | 1 | 285 | 15 |
| `_sab_reconcilelivecreator` | violation | `INV_RECONCILE_SAFE` | 1 | 214 | 12 |
| `_sab_reconcilestaletoken` | violation | `INV_RECONCILE_SAFE` | 0 | 243 | 13 |
| `_sab_entrybeforeckptdelete` | violation | `INV_CKPT_ORDER` | 1 | 308 | 16 |
| `_sab_sameincarnationrebirth` | violation | `INV_NO_ALIAS` | 1 | 352 | 27 |
| `_sab_floorretainsdeadname` | violation | `INV_BOUNDED_CATALOG` | 0 | 347 | 19 |
| `_finding_briefreconcileinv` | violation | `INV_RECONCILE_SAFE_BRIEF` | 1 | 95 | 13 |
| `_safe` | green | green | 0 | 278 | 20 |
| `_churn` | green | green | 1 | 1114 | 27 |
| `_witness_churn3` | violation | `WITNESS_CHURN` | 1 | 1114 | 28 |
| `_witness_aliasremnant` | violation | `WITNESS_ALIAS_REMNANT` | 0 | 427 | 27 |
| `_witness_orphaneaten` | violation | `WITNESS_ORPHAN_EATEN` | 1 | 387 | 14 |

`ALL EXPECTATIONS MET`, twelve of twelve, both at `MaxInc = 3` and under `BOUND=5`.

## The properties

`INV_NO_ALIAS == ~aliased` — **INV-3, incarnation inertness.** A life never touches bytes another
life wrote. This is the property that lets `EntryDelete` skip the physical-empty proof: debris is not
dangerous because it is not *reachable* from any future life, so there is no need to prove it gone.

`INV_NEWBORN_SAFE == (entry.state = "live") => (entry.inc \in ckptOf)` — **spec §3's creation
order.** `_ckpt` create precedes the catalog `Live` CAS, so a `Live` entry always has its `_ckpt`.
Stated over state rather than over the janitor's step deliberately: the damage of eating a newborn is
not the deletion (a `Creating` life that never goes `Live` loses nothing worth having) but the moment
its creator publishes `Live` on the strength of an ack for an object the pool no longer holds —
INV-4's "a cleaned prefix plus a hidden snapshot is indistinguishable from empty", entered through
the front door.

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
the entry is `Removing`, catalog entry LAST. The one invariant here that restates a precondition
rather than deriving a downstream consequence; see *Scoping* for why, stated as a limit rather than
hidden.

`TypeOK` additionally asserts `aliasedOnRemnant => aliased` and `orphanEaten => reconcileHarm`, so a
future edit cannot let a witness go red without its invariant going red too.

## The counterexamples, one per sabotage

**`_sab_janitoreatsnewborn` → `INV_NEWBORN_SAFE`, 5 states.** `Create` (inc 1, `Creating`) →
`CkptCreate` (the creator's conditional PUT acks; `ckptOf = {1}`) → the widened janitor deletes inc 1
while it is the entry's OWN incarnation → `GoLive`. The final state is a `Live` catalog entry whose
`_ckpt` object does not exist. The violation is the publish, not the delete: creation is three blind
conditional writes with no re-read (spec §3), so the creator goes `Live` on its own ack and has no
way to notice.

**`_sab_reconcilelivecreator` → `INV_RECONCILE_SAFE`, 5 states.** `Create` → `ReconcileObserve`
(a reconciler samples the `Creating` entry) → `ReconcileCreating` fires *without* the creator's fence
being terminal → the entry is gone while its creator is still running → `OrphanWrite`: that creator,
which captured `(namespace, incarnation)` at admission and writes blind conditional PUTs under it,
keeps writing into a prefix nothing in the catalog names.

**`_sab_reconcilestaletoken` → `INV_RECONCILE_SAFE`, 7 states.** The severe one. `Create` →
`ReconcileObserve` samples while the entry is `Creating` → the life proceeds normally all the way to
`Live` (`CkptCreate`, `GoLive`) → the reconciler acts on its now-stale sample and deletes the
**`Live`** entry → `OrphanWrite`. `_witness_orphaneaten` (below) shows the worse ending.

**`_sab_entrybeforeckptdelete` → `INV_CKPT_ORDER`, 7 states.** A complete honest life through
`Drop` and `TerminalFoldAndCleanup`, then `EntryDelete` with `ckptOf = {1}` still populated. The
catalog entry is the only record that names the incarnation and authorizes the exact-token delete, so
once it is gone the surviving `_ckpt` can never be addressed by token again — only the zero-trust
listing can find it, and the listing may omit it forever.

**`_sab_sameincarnationrebirth` → `INV_NO_ALIAS`, 8 states.** `Create` (inc 1) → `CreatorDies` →
`CkptCreate` → `ReconcileObserve` → `ReconcileCreating` (an honest, token-exact, fence-terminal
reconciliation — the creator's `_ckpt` is left behind as ordinary janitor food, which is correct) →
`Create` REUSES inc 1 →
`ReadOwn`, the new life's recovery open, reads the dead creator's `_ckpt` at its own prefix. Note
what is *not* sabotaged in this trace: the reconciliation and the leftover `_ckpt` are both honest.
Only the incarnation reuse is wrong, and that alone is enough.

**`_sab_floorretainsdeadname` → `INV_BOUNDED_CATALOG`, 9 states.** A complete honest life —
`Create`, `CkptCreate`, `GoLive`, `Drop`, `TerminalFoldAndCleanup`, `RemovalCkptDelete` — then
`EntryDelete` retains `floors = {1}`. The name is now absent and the catalog still holds a record for
it. Under churn the count grows with the cycles instead of staying at the live-name count.

**`_finding_briefreconcileinv` → `INV_RECONCILE_SAFE_BRIEF`, 3 states.** No sabotage is enabled.
`Create` → `CreatorDies`. The entry is `Creating`, the fence is terminal and no `_ckpt` exists yet —
which is a legitimate transient state and precisely the one reconciliation exists to clean up.

## Non-vacuity of the green runs

Both greens explore their state space to exhaustion (`0 states left on queue`): `_safe` 278 distinct
states at depth 20, `_churn` 1114 at depth 27. Action coverage for `_churn` (`-coverage 1`,
`distinct:generated`):

```
Create 29:30   ReadOwn 0:100   CkptCreate 42:100   GoLive 59:100   WriteObject 75:400
CreatorDies 109:500   ReconcileObserve 66:200   ReconcileCreating 34:40
OrphanDies 0:0   OrphanWrite 0:0
Drop 152:160   TerminalFoldAndCleanup 144:480   RemovalCkptDelete 277:320   EntryDelete 67:160
Janitor 59:1228   NoOp 0:1114
```

Every action fires except `OrphanDies` and `OrphanWrite`, and that is the correct reading: an honest
reconciliation never produces an orphan, so those two actions are unreachable in a green run by
construction. They are exercised in `_sab_reconcilelivecreator`, `_sab_reconcilestaletoken` and
`_witness_orphaneaten`. `ReadOwn 0:100` and `NoOp 0:1114` generate no distinct states because they
are self-loops when their ghost does not change — also correct.

Three witnesses carry the rest of the non-vacuity argument. All are negated, so a VIOLATION is the
evidence, and all exist for the same reason: breadth-first search reports the SHORTEST
counterexample, so an invariant reachable two ways only ever shows the near one and the far route
silently rots into a dead branch no run would notice.

**`_witness_churn3` → `WITNESS_CHURN`, 24 states.** Three complete create → drop → recreate cycles
of the one name, ending with `lives = 3`, `entry` absent and `objects = {1, 2}` — debris from two
earlier lives still sitting in the pool. This is the shape the churn green has to be about: a
lifecycle that quietly wedged at the first remnant (which is what a physical-empty-proof design does)
would also be green on every safety invariant and would be worthless.

**`_witness_aliasremnant` → `WITNESS_ALIAS_REMNANT`, 12 states.** The headline aliasing route, which
`INV_NO_ALIAS`'s own counterexample skips: a full life 1 including `WriteObject`, a
`TerminalFoldAndCleanup` that leaves `objects = {1}` behind, `RemovalCkptDelete`, `EntryDelete` — and
then a reborn inc 1 reading that remnant. This is the debris `EntryDelete` *knowingly* leaves; the
shorter route through a reconciled creator's `_ckpt` is real but is not what the inertness argument
is about.

**`_witness_orphaneaten` → `WITNESS_ORPHAN_EATEN`, 8 states.** The severe arm of
`INV_RECONCILE_SAFE`. A `Live` namespace with a real ref layer (`objects = {1}`, `ckptOf = {1}`) has
its catalog entry destroyed by the stale-token reconciler, and the janitor then deletes the running
life's `_log`/`_snap` *and* its `_ckpt` in one step. The janitor is not sabotaged in this trace and is
not misbehaving — its honest rule is "delete every incarnation the catalog does not name", and a
reconciliation that was neither token-exact nor fence-terminal is what handed it a live namespace to
apply that rule to.

## What the model is

ONE logical namespace name, lived over and over, with everything keyed by INCARNATION
(`<ns>/<inc>/{_log,_snap,_ckpt}`). The question v9 has to answer is not what one life does but what
SURVIVES a life and what the next life makes of it, so the oracle is not "is the pool tidy" — it
never is — but "did a new life ever touch an old life's bytes, and did the catalog ever keep a record
a name no longer has".

The catalog is `entry` (state + incarnation) plus the creator's fence (`creatorAlive`), its own
`_ckpt` ack (`ckptDone`, deliberately distinct from whether the object currently exists) and the
terminal record (`terminal`). The reconciler's token-CAS is modelled without a token counter:
`obsArmed` + `obsStale`, where every catalog write sets `obsStale' = (obsStale \/ obsArmed)`. That is
exactly as strong as a counter, because token equality *is* "unwritten since sampled", and it keeps
the state space at four figures.

### The three adversaries, and why they are not sabotages

Enabled in GREEN configurations too:

- **`TerminalFoldAndCleanup` leaves remnants nondeterministically.** Best-effort cleanup is the
  ordinary case that INV-3's inertness argument has to survive, not a fault. `_churn` and
  `_witness_churn3` deliberately run three lives that all leave one.
- **The janitor is lazy and may omit any object forever.** LIST is a zero-trust hint (§1), so
  omission is always possible; INV-3 calls it deferred cleanup, the leak-only direction.
- **The owning fence may go terminal at any point of any life** (`CreatorDies`). It is the
  precondition reconciliation must WAIT for, not an anomaly.

### One rule enforced by construction rather than tested

Spec §3: the terminal record is appended ONLY by the owning mounted writer or by a successor that has
claimed and fenced that server root — "GC surfaces stuck removals, never appends". GC therefore has
no appending action anywhere in this module, so a `Removing` entry whose owner never returns simply
stays `Removing`. That is the intended outcome and it is bounded at one entry, which is why it does
not threaten `INV_BOUNDED_CATALOG`. There is no sabotage for this rule because there is nothing in
the model that could break it; a reviewer wanting it tested would need a GC actor, which this module
does not have (`CaRefDeltaIntakeCore` does).

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
occur — a permanently unaddressable object. Scope stated in headline 5.

**6. Three witness configs added** beyond the brief's six, plus `BOUND`. Cost: about four seconds
total.

## Scoping — what this model deliberately does not cover

- **`INV_CKPT_ORDER` restates its precondition.** With incarnations unique, an orphaned `_ckpt` is
  inert, so its only consequence is a leak that the spec already tolerates in the janitor-omission
  direction. There is no downstream state in this model that the leak corrupts, and inventing one
  would be modelling fiction. The invariant is therefore weaker evidence than the other five, and it
  is marked as such rather than dressed up.
- **INV-3's additive capacity predicate is not modelled.** The byte bound on namespace names, the
  encoded-catalog-plus-worst-case-reservation admission check and the `encodeFoldSeal(...).size()`
  cap are size arithmetic, not concurrency; a TLA+ model of them would be a spreadsheet. §9 lists
  them under the plan's boundary and boundary-plus-one tests, which is the right instrument. What
  this module does cover is the other half of INV-3's capacity story — that the *number* of entries
  stays bounded under churn.
- **One name.** The catalog is a map and the per-name terms are independent, so the O(C+L+R) bound
  sums; but a genuinely cross-name hazard (a reconciler or janitor confusing two names) is outside
  this model. The typed `RefNamespaceId` of INV-3's r9-3 closure is the mechanism that makes such a
  confusion unrepresentable in code, and it is an API-shape obligation, not a protocol one.
- **No GC actor**, per the section above.
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
./run_refcatalog.sh            # 12 configs, ~8s total
BOUND=5 ./run_refcatalog.sh    # same 12 expectations at MaxInc = 5
```

Logs land in `tmp/tlc_CaRefCatalogCore_<config>.log`.
