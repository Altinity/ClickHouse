# CaRefNsCleanupStaleLeaderCore — TLA+ gate results (v9 rewrite)

Model: `CaRefNsCleanupStaleLeaderCore.tla`, rewritten (not new). Gates the GC stale-leader straggler
interleaving of spec `2026-07-27-cas-ref-chain-complete-cut-design.md` (v9) — §2 **INV-3**
(`cas/ref_catalog`, ref-layer-scoped incarnations, structural inertness of surviving debris) and §3
**Lifecycles** (removal, recreation). Task 4 of the plan `2026-07-28-cas-ref-chain-tla-phase.md`; this
is a phase-0 gate — it blocks the C++ work. Supersedes the module's 2026-07-11-design version, which
modelled the `_cleanup`-marker / `Completed` recreation gate v9 deletes (git history: commit
`b0811c92382` and its ancestors).

Runner: `./run_nscleanup_staleleader.sh` (runs every config and checks its expected verdict, including
*which* invariant the sabotage is required to break; the sabotage runs FIRST, because a green is only
evidence once the property it rests on has been seen red). TLC 2.19 (tla2tools, Java 21),
`java -XX:+UseParallelGC -workers 1`. Every number below is real TLC output, reproduced identically
across two consecutive runs (see Reproduce).

**`-workers 1`, not `-workers auto`, for the same reason as `run_refcatalog.sh`:** parallel BFS visits
states in a nondeterministic order, so the reported depth and the trace TLC prints are not
reproducible run to run under `auto`. This module's state space is tiny (single digits of distinct
states either way), so the practical risk is low, but the convention is uniform across the phase.
Override with `TLC_WORKERS=auto` if you only want a verdict and not the numbers.

## Headline

**The old model's hazard invariant is now structural, and the model proves exactly that — not by
asserting it, but by deleting the guard code and showing the property survives anyway.** The
2026-07-11 module's `StaleLeaderPass` had three live guards (round re-read, marker-presence abort,
epoch filter) wired behind `SabotageNoStragglerGuard`. None of the three has a v9 analogue: the
`_cleanup` marker and the GC round it was keyed to are gone from the ref layer's recreation gate
(spec §2 INV-3, §3). The rewritten `StaleLeaderPass` is **unconditional** — it has no sabotage toggle
of its own at all — and `_safe` is green anyway, because the delete it issues can only ever name the
incarnation it captured before deposition (`staleInc`), and an honestly reborn namespace never reuses
that value. `_sab_noincarnation` is the one way to falsify the premise: make recreation reuse
`staleInc`, and the *identical, unmodified* `StaleLeaderPass` code now deletes the reborn life's own
data. That is the model-level proof the main plan cites for deleting the marker gate on the ref layer:
**incarnation freshness carries rebirth safety, not physical-empty polling and not a marker.**

## Summary table

| Config | Sabotage | Expected | Result |
|---|---|---|---|
| `_sab_noincarnation` | `SabotageNoIncarnation = TRUE` (recreation reuses `staleInc`) | violation | ❌ `NoLiveDataDeleted` violated, depth 4 (11 generated / 6 distinct states) |
| `_safe` | `SabotageNoIncarnation = FALSE` (recreation always mints fresh) | green | ✅ No error — depth 5 (21 generated / 8 distinct states) |

`TypeOK` holds in both configs (asserted, not merely checked incidentally — TLC checks every listed
`INVARIANT` on every generated state regardless of order).

## The property

```
NoLiveDataDeleted == ~deletedLiveData
```

`deletedLiveData` is a sticky ghost, set the moment `StaleLeaderPass` fires while `entry.state = "live"`
**and** `entry.inc = staleInc` **and** an object is actually present at that incarnation. All three
conjuncts matter: the first excludes the pass legitimately cleaning up its own dying life (which is
also `entry.inc = staleInc`, just with `entry.state = "removing"`, never `"live"` — `"live"` is only
ever reached through `Recreate`, i.e. a genuinely new installation); the second is the aliasing
condition; the third means the property is about actual data loss, not merely about the coincidence of
numbers.

## `_sab_noincarnation` — RED (the counterexample)

```
State 1 <Init>            entry=[removing,1]  staleInc=1  objects={1}  passDone=F  deletedLiveData=F
State 2 <EntryDelete>     entry=[absent,0]    staleInc=1  objects={1}  passDone=F  deletedLiveData=F
State 3 <Recreate>        entry=[live,1]      staleInc=1  objects={1}  passDone=F  deletedLiveData=F
State 4 <StaleLeaderPass> entry=[live,1]      staleInc=1  objects={}   passDone=T  deletedLiveData=T
```

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

## `_safe` — GREEN, and non-vacuously so

No error, 21 states generated / 8 distinct / depth 5, `TypeOK` and `NoLiveDataDeleted` both hold on
every reachable state. The 8 distinct states are exactly the reachable
`(entry, objects, passDone)` combinations (`staleInc` is fixed at 1 for the whole run; `nextInc`
tracks `entry.state` one-for-one so it adds no new dimension):

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

The last four rows are the ones that matter for non-vacuity: `entry.state = "live"` **is** reached
(`nextInc` advances to 3, i.e. `Recreate` actually minted a fresh incarnation), and
`StaleLeaderPass` **does** run after rebirth in two of the eight states (rows 6 and 8) — the pass is
not vacuously safe because it never gets a chance to run once the namespace is live. It runs, deletes
whatever is at `staleInc = 1` (the stale debris, rows where `objects` loses its `1`), and never touches
incarnation 2.

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
  alive would misrepresent what the file tests. The one remaining lever — whether recreation reuses the
  incarnation — is `SabotageNoIncarnation` / `_sab_noincarnation.cfg`, and it is new, not a renaming of
  the old one; `_safe.cfg` keeps its name and role (the honest, all-adversaries-off baseline) unchanged.
- **Files are entirely absent** — see "What changed" above.

## Reproduce

```bash
cd docs/superpowers/models
./run_nscleanup_staleleader.sh
# TLC_WORKERS=auto ./run_nscleanup_staleleader.sh   # verdict only, numbers not reproducible run-to-run
```

Confirmed byte-identical (state counts, depth, and the counterexample trace) across two consecutive
`-workers 1` runs on 2026-07-28.
