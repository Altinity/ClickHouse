# CaRefNsCleanupStaleLeaderCore — TLA+ gate results (v9 rewrite)

Model: `CaRefNsCleanupStaleLeaderCore.tla`, rewritten (not new). Gates the GC stale-leader straggler
interleaving of spec `2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §2 **INV-3**
(`cas/ref_catalog`, ref-layer-scoped incarnations, structural inertness of surviving debris) and §3
**Lifecycles** (removal, recreation, and — added by this task's review round, commit `d1eae033122` —
the deposit-time-capture rule for cleanup items). Task 4 of the plan
`2026-07-28-cas-ref-chain-tla-phase.md`; this is a phase-0 gate — it blocks the C++ work. Supersedes
the module's 2026-07-11-design version, which modelled the `_cleanup`-marker / `Completed` recreation
gate v9 deletes (git history: commit `b0811c92382` and its ancestors).

Runner: `./run_nscleanup_staleleader.sh` (runs every config and checks its expected verdict, including
*which* invariant the sabotage is required to break; sabotages run FIRST, because a green is only
evidence once the property it rests on has been seen red). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers 1`. Every number below is real TLC output from the fix round 1 run of
2026-07-28, reproduced identically across two consecutive runs (see Reproduce).

**`-workers 1`, not `-workers auto`, for the same reason as `run_refcatalog.sh`:** parallel BFS visits
states in a nondeterministic order, so the reported depth and the trace TLC prints are not
reproducible run to run under `auto`. This module's state space is tiny (single digits to low tens of
distinct states), so the practical risk is low, but the convention is uniform across the phase.
Override with `TLC_WORKERS=auto` if you only want a verdict and not the numbers.

## Headline

**Two independent facts carry rebirth safety for this hazard, each with its own counterexample, and
neither is "a live guard caught it."** The 2026-07-11 module's `StaleLeaderPass` had three live guards
(round re-read, marker-presence abort, epoch filter). None of the three has a v9 analogue: the
`_cleanup` marker and the GC round it was keyed to are gone from the ref layer's recreation gate (spec
§2 INV-3, §3). The rewritten `StaleLeaderPass` is **unconditional** on the catalog's current state — no
marker check, no round CAS — and `_safe` is green anyway, provided two things hold: (1) recreation
always mints a fresh incarnation (`_sab_noincarnation` falsifies this), and (2) the pass always targets
the incarnation captured when its item was deposited, never one re-read from the current catalog entry
(`_sab_rederive` falsifies this — spec §3's rule, added by this task's review round: "the
namespace-cleanup item carries the incarnation captured at deposition, and a resumed pass NEVER
re-derives it from the catalog"). The two configs violate `NoLiveDataDeleted` by independent, isolated
routes (see below) — the model-level proof that both facts are separately load-bearing, not that
either alone would suffice.

**This is not a claim that v9 has no live precondition on destructive cleanup at all.** Spec §2's
read-side contract still mandates that "destructive cleanup revalidates life and fence immediately
before every delete" — a real, separate check a real implementation keeps making on every delete. This
model deliberately omits that revalidation (`StaleLeaderPass` never re-validates anything) precisely to
prove the STRONGER claim that `NoLiveDataDeleted` does not depend on it: the two facts above are
sufficient on their own, over and above whatever the spec's revalidation additionally buys in
production as defense in depth.

## Summary table

| Config | Sabotage | Expected | Result |
|---|---|---|---|
| `_sab_noincarnation` | `SabotageNoIncarnation=TRUE` (recreation reuses `staleInc`) | violation | ❌ `NoLiveDataDeleted` violated, depth 4 (11 generated / 6 distinct states) |
| `_sab_rederive` | `SabotageRederive=TRUE` (pass re-derives its target from `entry.inc`) | violation | ❌ `NoLiveDataDeleted` violated, depth 5 (18 generated / 10 distinct states) |
| `_safe` | both FALSE | green | ✅ No error — depth 5 (21 generated / 8 distinct states) |

`TypeOK` holds in every config (asserted, not merely checked incidentally — TLC checks every listed
`INVARIANT` on every generated state regardless of order). `MaxInc = 2` in every config — tightened
from `MaxInc = 3` in this fix round; see the `_safe` section below for why 2 is the honest bound, not a
shortcut.

## The property

```
PassTarget == IF SabotageRederive THEN entry.inc ELSE staleInc
NoLiveDataDeleted == ~deletedLiveData
```

`deletedLiveData` is a sticky ghost, set the moment `StaleLeaderPass` fires while `entry.state = "live"`
**and** `entry.inc = PassTarget` **and** an object is actually present at that incarnation. All three
conjuncts matter: the first excludes the pass legitimately cleaning up its own dying life (`entry.inc`
also equals `staleInc` while `entry.state = "removing"`, but `"live"` is only ever reached through
`Recreate`, i.e. a genuinely new installation); the second is the mistargeting condition, reachable by
either of two independent routes (`PassTarget` aliasing onto a live `entry.inc` because `staleInc` was
reused, or `PassTarget` no longer tracking `staleInc` at all because it was re-derived); the third means
the property is about actual data loss, not merely the coincidence of numbers.

## `_sab_noincarnation` — RED (the counterexample)

```
State 1 <Init>            entry=[removing,1]  staleInc=1  objects={1}  passDone=F  deletedLiveData=F
State 2 <EntryDelete>     entry=[absent,0]    staleInc=1  objects={1}  passDone=F  deletedLiveData=F
State 3 <Recreate>        entry=[live,1]      staleInc=1  objects={1}  passDone=F  deletedLiveData=F
State 4 <StaleLeaderPass> entry=[live,1]      staleInc=1  objects={}   passDone=T  deletedLiveData=T
```

`SabotageRederive=FALSE` throughout this config, so `PassTarget = staleInc` at every step — this
config isolates the incarnation-reuse route from the deposit-vs-rederive route (`_sab_rederive`,
below).

This is the *shortest* counterexample, and it is worth noticing it does not even need the reborn life
to have written anything of its own (`WriteObject` never fires in this trace): `Recreate` under
`SabotageNoIncarnation` sets `entry.inc' = staleInc`, and the moment it does, the dying life's own
leftover object at incarnation 1 (present since `Init` — the pass's ordinary, legitimate cleanup
target) *is* the reborn life's data, because they now share one prefix. `StaleLeaderPass` then deletes
it, `entry.state = "live"` at the time, and the ghost fires. This is the aliasing hazard in its purest
form — a rebirth doesn't even have to write new bytes to have its identity stolen; it only has to
reuse the address.

A second, independent route exists and is *not* the one TLC reports (BFS reports the shortest): let
`WriteObject` fire after `Recreate` before the pass runs. `objects` is a union, so it does not matter
whether the old debris was already there — the new life's own write lands at the same aliased key and
is deleted just the same. Both routes are real; the model does not need a dedicated witness config to
separate them the way `CaRefCatalogCore_witness_aliasremnant` does, because here the *shorter* route
(reused debris) is the headline case, not a shorter decoy hiding a longer one — the module comment
calls this out explicitly rather than leaving it implicit.

## `_sab_rederive` — RED (the counterexample)

```
State 1 <Init>            entry=[removing,1]  staleInc=1  objects={1}    passDone=F  deletedLiveData=F
State 2 <EntryDelete>     entry=[absent,0]    staleInc=1  objects={1}    passDone=F  deletedLiveData=F
State 3 <Recreate>        entry=[live,2]      staleInc=1  objects={1}    passDone=F  deletedLiveData=F
State 4 <WriteObject>     entry=[live,2]      staleInc=1  objects={1,2}  passDone=F  deletedLiveData=F
State 5 <StaleLeaderPass> entry=[live,2]      staleInc=1  objects={1}    passDone=T  deletedLiveData=T
```

`SabotageNoIncarnation=FALSE` here: `Recreate` mints `entry.inc = 2`, a genuinely fresh incarnation, so
the `_sab_noincarnation` route is closed off by construction — this counterexample is not a repeat of
the other one wearing a different flag. The hazard instead comes entirely from `PassTarget`: under
`SabotageRederive`, `PassTarget = entry.inc`, i.e. the pass resolves its own target from whatever the
catalog currently says instead of the `staleInc = 1` its item was deposited with. Once the namespace is
reborn, "whatever the catalog currently says" simply IS the live incarnation — no aliasing needed, the
pass just targets the right thing (the namespace's current life) for the wrong reason (it should never
have looked).

Unlike `_sab_noincarnation`, this route needs `WriteObject` to fire first: at `State 3`,
`PassTarget = entry.inc = 2`, but `2 \notin objects` yet, so a pass firing there would be an observable
no-op. Only once the reborn life has written something of its own (`State 4`, `objects = {1,2}`) does
`StaleLeaderPass` have live data to reach. TLC's depth-5 trace is one step longer than
`_sab_noincarnation`'s depth-4 for exactly this reason, and — unlike the other config's reused-debris
shortcut — there is no shorter route available here at all.

## `_safe` — GREEN, and non-vacuously so

No error, 21 states generated / 8 distinct / depth 5, `TypeOK` and `NoLiveDataDeleted` both hold on
every reachable state — **identical counts to the pre-fix-round `MaxInc = 3` run.** That equality is
itself the evidence for tightening the bound: only ONE `Recreate` is ever reachable in this module
(`entry` never returns to `absent` after rebirth — there is no second removal in this story), so
`MaxInc = 3`'s third incarnation value was dead headroom from the start, never a load-bearing digit;
`MaxInc = 2` (the module's own `ASSUME` minimum) proves the identical property over the identical state
space. The 8 distinct states are exactly the reachable `(entry, objects, passDone)` combinations
(`staleInc` is fixed at 1 for the whole run; `nextInc` tracks `entry.state` one-for-one so it adds no
new dimension; `SabotageRederive = FALSE` here, so `PassTarget` tracks `staleInc` and is not a fourth
dimension either):

| entry | objects | passDone | reached via |
|---|---|---|---|
| `[removing,1]` | `{1}` | F | `Init` |
| `[removing,1]` | `{}` | T | `StaleLeaderPass` before `EntryDelete` |
| `[absent,0]` | `{1}` | F | `EntryDelete` |
| `[absent,0]` | `{}` | T | `EntryDelete` after the pass, or the pass after `EntryDelete` |
| `[live,2]` | `{1}` | F | `Recreate` |
| `[live,2]` | `{}` | T | `Recreate` after the pass, or the pass after `Recreate` |
| `[live,2]` | `{1,2}` | F | `WriteObject` after `Recreate`, before the pass |
| `[live,2]` | `{2}` | T | `WriteObject` then the pass, or the pass then `WriteObject` |

The last four rows are the ones that matter for non-vacuity by hand: `entry.state = "live"` **is**
reached (`nextInc` advances to 3, i.e. `Recreate` actually minted a fresh incarnation), and
`StaleLeaderPass` **does** run after rebirth in two of the eight states (rows 6 and 8) — the pass is not
vacuously safe because it never gets a chance to run once the namespace is live. It runs, deletes
whatever is at `staleInc = 1` (the stale debris, rows where `objects` loses its `1`), and never touches
incarnation 2.

**Machine-checked non-vacuity (M2): per-action invocation counts.**
`COVERAGE=1 ./run_nscleanup_staleleader.sh` re-runs `_safe` under TLC's `-coverage 1` and reports, per
action, `distinct-states-contributed:times-evaluated`:

```
EntryDelete 1:2   Recreate 1:2   WriteObject 1:4   StaleLeaderPass 4:4   NoOp 0:8
```

Every action in `Next` fires and contributes at least one distinct state — unlike `CaRefCatalogCore`'s
`_safe`, nothing here is sabotage-gated into permanent silence (`ZombieGoLive`, `OrphanDies`,
`OrphanWrite` never fire in that module's honest run; every action in THIS module is a plain,
always-enabled step, since both sabotages here are parametric switches inside otherwise-unconditional
actions rather than separate gated actions of their own). `StaleLeaderPass 4:4` is the number that
matters most: it fires in all 4 of the states where it is enabled and contributes a distinct state
every single time (`4:4`, not `k:4` for `k<4`) — i.e. every firing of the pass lands somewhere new,
including the two post-rebirth firings the hand-derived table above calls out as rows 6 and 8. This is
the same evidence as the hand enumeration, machine-checked rather than eyeballed.

## What changed, and the register-item boundary

**Note:** `Completed`-marker gate deleted for the REF layer per spec §3; the FILE layer keeps it —
register R1.

The 2026-07-11 module modelled both a `recreatedManifest` and a `recreatedFile` ghost,
because that design's single `_cleanup` marker gated recreation of *both* the ref-layer manifest and
the unqualified verbatim file at the same key. v9 splits that in two: the ref layer gets
incarnation-qualified keys (`<ns>/<inc>/{_log,_snap,_ckpt}`) and drops the marker gate entirely — this
module's whole subject. Verbatim files stay `{namespace, file_name}`-keyed with no incarnation
component and keep exactly the old `_cleanup`-marker gate, unchanged; their rebirth-aliasing hazard is
`docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md` register item R1 (pre-existing, not
this spec). This module therefore models **ref-layer objects only** — one generalised "object" kind,
since `_log`/`_snap`/`_ckpt` are indistinguishable at the granularity this hazard cares about — and
does not model files or a marker at all. A `CaRefNsCleanupStaleLeaderCore`-shaped model for the FILE
layer's still-live `_cleanup` hazard, if one is ever needed, is R1's problem, not a gap in this one.

**Fix round 1 addition.** Spec §3 gained a second, independent normative sentence during this task's
review (commit `d1eae033122`, prompted by review finding I2): "the namespace-cleanup item carries the
incarnation captured at deposition, and a resumed pass NEVER re-derives it from the catalog." This
module is the sentence's own citation (`d1eae033122`'s diff names
`CaRefNsCleanupStaleLeaderCore` directly) — `SabotageRederive` / `_sab_rederive.cfg` is the model of an
implementation that gets the rule backwards, and its counterexample above is the "phase-0 model" the
spec sentence points to.

## Scoping — what this rewrite deliberately does not cover

- **No `Creating` phase.** Recreation is one step, `absent -> live`, minting (or, sabotaged, reusing)
  the incarnation directly. CaRefCatalogCore (Task 3) is the proof that the real three-conditional-write
  creation sequence (`Creating` -> `_ckpt` create -> `Live` CAS, with the admission fence generation and
  catalog token-CAS on each) is itself safe; nothing about *this* hazard — a straggler racing a
  captured incarnation against rebirth — depends on which of those sub-steps rebirth is mid-way
  through, only on the fact that `live` is never reached with the old incarnation unless something
  reused it.
- **No `_ckpt` or terminal-record bookkeeping.** INV-4's ordering (`_ckpt` deleted by exact token while
  `Removing`, catalog entry last) is orthogonal to this module's property and is Task 3's proof
  (`INV_CKPT_ORDER`).
- **No concurrent GC leaders, no janitor.** This module is deliberately about the ONE interleaving of
  ONE stale leader against ONE rebirth; `CaRefCatalogCore`'s `Janitor` action already covers the
  general "delete any foreign-incarnation debris, from any actor, at any time" case
  (`INV_NEWBORN_SAFE`, `INV_NO_ALIAS`). Modelling a second leader here would duplicate that proof
  without adding anything specific to the straggler shape.
- **`SabotageNoStragglerGuard` has no successor and the old `_sab_noguard.cfg` is retired (not kept
  under a new name).** The old sabotage disabled three live re-checks (round, marker, epoch) that
  simply do not exist in v9's design — there is no guard left in `StaleLeaderPass` to disable, because
  the action never had one to begin with (see the module comment). A cfg toggling a constant that no
  longer exists would not parse; inventing a *new* meaning for the same constant name to keep the file
  alive would misrepresent what the file tests. The two remaining levers — whether recreation reuses
  the incarnation, and whether the pass re-derives its target — are `SabotageNoIncarnation` and
  `SabotageRederive` / `_sab_noincarnation.cfg` and `_sab_rederive.cfg`, both new, neither a renaming
  of the old one; `_safe.cfg` keeps its name and role (the honest, all-adversaries-off baseline)
  unchanged.
- **How the deposited value ITSELF gets captured is not modelled.** `staleInc` is a given at `Init` —
  this module proves that an honestly-captured value is later honored (`_safe`) and shows what happens
  if it is dishonestly overridden at resume time (`_sab_rederive`), but it says nothing about a
  possible race at the OTHER end: whether the cleanup item's deposition itself could observe a wrong
  incarnation in the first place (e.g. a race between the removal's own commit and the item's
  creation). That is a deposit-correctness question, not a resume-honesty one, and is out of this
  module's scope on the same grounds as the rest of this list — flagged as a main-plan test obligation
  in `task-4-report.md`, not modelled here.
- **Files are entirely absent** — see "What changed" above.

## Reproduce

```bash
cd docs/superpowers/models
./run_nscleanup_staleleader.sh
# COVERAGE=1 ./run_nscleanup_staleleader.sh          # also re-runs `safe` under -coverage 1 (M2)
# TLC_WORKERS=auto ./run_nscleanup_staleleader.sh    # verdict only, numbers not reproducible run-to-run
```

Confirmed byte-identical (state counts, depth, and every counterexample trace, all three configs)
across two consecutive `-workers 1` runs on 2026-07-28 (fix round 1).
