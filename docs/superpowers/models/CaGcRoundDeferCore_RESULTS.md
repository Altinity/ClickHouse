# CaGcRoundDeferCore — TLA+ gate results

Model: `CaGcRoundDeferCore.tla` (spec `2026-07-06-cas-gc-round-skip-unchanged-design.md`, Phase 4 Lever A).
Gate for the GC round DEFER (skip-unchanged) design. Constants across all cfgs:
`Writers = {w1, w2}`, `Blobs = {b1}`, `MaxRound = 4`, `MaxDefer = 3`.

TLC 2 (tla2tools, Java 21), `java -XX:+UseParallelGC -workers auto`. All runs finish in < 1 s.

| cfg | check | expected | TLC verdict | states (gen / distinct) |
|-----|-------|----------|-------------|--------------------------|
| `stage1` | `TypeOK` `NoOverDelete` `NoDangle` + PROPERTY `EventuallyFolded` | PASS | **PASS** — `Model checking completed. No error has been found.` | 31,041 / 8,445 |
| `witness_deferthenfold` | negated-reachability `W_DeferThenFold` | violation (reachable) | **VIOLATED (as required)** — `Invariant W_DeferThenFold is violated.` | 653 / 298 |
| `sab_graduate_on_stale` | `NoOverDelete` | counterexample | **VIOLATED (as required)** — `Invariant NoOverDelete is violated.` | 10,907 / 3,338 |
| `sab_unbounded_defer` | PROPERTY `EventuallyFolded` | counterexample | **VIOLATED (as required)** — `Temporal properties were violated.` | 402 / 102 |

## stage1 — GREEN

```
Model checking completed. No error has been found.
31041 states generated, 8445 distinct states found, 0 states left on queue.
```

Safety (`TypeOK`, `NoOverDelete`, `NoDangle`) holds on all 8,445 reachable states, and the temporal
property `EventuallyFolded` holds under the weak-fairness `Spec` (the `deferCount < MaxDefer` bound
forces `GBegin` once the budget is spent, and `WF` on the round machinery then drives `GFold`, which
drains `unfolded` back to `{}`).

## witness_deferthenfold — DEFER-then-FOLD is reachable (safety is not vacuous)

`W_DeferThenFold == ~(deferredWithUnfoldedEver /\ unfolded = {})` is violated, i.e. a state where a
DEFER round ran while `unfolded # {}` (so `deferCount > 0`) AND a later FOLD drained `unfolded` to
`{}` is reachable. Trace (abridged):

- State 2–3: two `WriterAddEdge` → `unfolded = {[b1,w1,add],[b1,w2,add]}`.
- State 4–6: three `DeferRound` while `unfolded # {}` → `deferredWithUnfoldedEver = TRUE`, `deferCount` 1→2→3.
- State 7: `GBegin` (bound hit, `deferCount = MaxDefer`).
- **State 8: `GFold` drains `unfolded = {}`** while `deferredWithUnfoldedEver = TRUE` → invariant violated.

`unfolded` only ever returns to `{}` via `GFold`, so this state is genuine proof that a real
DEFER-then-FOLD sequence occurs (the model is not vacuously safe by never deferring).

## sab_graduate_on_stale — NoOverDelete counterexample (force-fold-before-graduation is load-bearing)

With `SabotageGraduateOnStale = TRUE`, `GComplete` drops the "`unfolded` covers `b`" delete guard.
15-state counterexample (the mirror of the 2026-06-27 leak, on the +1 side):

- State 2–9: defer to the bound, `GBegin`/`GFold`/`GComplete` → **b1 condemned** (`retired = {[b1, condemn_round = 3]}`), unreferenced (`Indeg(b1) = 0`).
- State 11: `AckAdvance` → `minAck = 4 > 3` → graduation **due**.
- State 12–13: `GBegin` (latch `minAckL = 4`), `GFold` (drains nothing; `folded = {}`).
- **State 14: `WriterAddEdge(w1, b1)` — the protective +1 lands in the "folded" phase, AFTER the fold cut** → `unfolded = {[b1,w1,add]}`.
- **State 15: `GComplete` physically deletes b1** (`present[b1] = FALSE`, `deletedThisStep = {b1}`) while `unfolded` still holds the +1 → `NoOverDelete` violated (a live-referenced blob is deleted → dangling).

In the honest model (guard present) State 15 keeps the entry (blocked by the pending +1) instead of
deleting; the +1 folds on the next round and the graduation is cancelled (`Indeg` recovered).

## sab_unbounded_defer — EventuallyFolded counterexample (bounded deferral is load-bearing)

With `SabotageUnboundedDefer = TRUE`, the `deferCount < MaxDefer` guard is removed. 11-state
lasso + stuttering counter-example to the leads-to `(unfolded # {}) ~> (unfolded = {})`:

- State 2–3: two `WriterAddEdge` → `unfolded = {[b1,w1,add],[b1,w2,add]}` (never empty again).
- State 4–11: `DeferRound` fires forever (interleaved with `AckAdvance`); `deferCount` caps at 3 and
  `round` caps at 4, but `DeferRound` stays **enabled** because the bound is gone and `~GraduationDue`
  holds (`retired = {}`, so no fold is ever forced).
- **State 12: Stuttering** — `unfolded` remains `{[b1,w1,add],[b1,w2,add]}` forever, never folded.

The behavior is fair (`WF(DeferRound)` fires infinitely; `GBegin`/`GFold`/`GComplete` are never
enabled → their `WF` is vacuous), so it is a legitimate liveness violation: an unfolded delta is
permanently skipped — the exact leak class the bound exists to prevent.

## Modeling decisions / assumptions

- **`unfolded` carries both signs** (`op ∈ {"add","del"}`); `GFold` applies adds then dels. The
  delete guard force-folds on ANY pending delta touching `b`, so the safety rule covers the +1 and
  the −1 directions uniformly and matches the brief's `NoOverDelete == … \A e \in unfolded : e.b # b`.
- **In-degree is read from the sealed snapshot `folded` only** (`Indeg`); a +1 added after the fold
  cut lives in `unfolded` and is invisible to `Indeg` — this is exactly what makes the guard, not the
  in-degree check, the thing that protects the blob (mirrors the ack-floor "landed after cut" window).
- **FOLD and DEFER are mutually exclusive per idle round and their disjunction is total**:
  `DeferRound` is enabled iff `~GraduationDue /\ (Sab \/ deferCount < MaxDefer)`; `GBegin` iff
  `FoldEnabled == GraduationDue \/ (~Sab /\ deferCount >= MaxDefer)`. This is what lets
  `SabotageUnboundedDefer` starve the fold: if fold were also voluntarily enabled during defer,
  `WF` on it would force draining and the liveness sabotage could not bite.
- **Weak fairness only on the round machinery** (`DeferRound`, `GBegin`, `GFold`, `GComplete`);
  writers and `AckAdvance` are the unconstrained environment. `WF(DeferRound)` is required so the
  honest model cannot stutter forever with budget remaining; the bound then hands off to `WF(GBegin)`.
- **`minAck` is a monotone abstraction** of the heartbeat floor (`AckAdvance`, `minAck < round`),
  latched by `GBegin` into `minAckL`; graduation reads the latched floor (ack-floor discipline).
- **`round`/`deferCount` cap** at `MaxRound`/`MaxDefer` so the sabotage state space stays finite and
  TLC can exhibit the liveness lasso.
- **Ref removal via `unfolded` (not immediate)** unlike `CaGcAckFloorCore`; here the deferred-delta
  timing is the subject under test, so removals are carried through the same defer/fold pipeline.

## 2026-08-03 — runner-suite run {#2026-08-03-runner-suite-run}

`run_gcrounddefer.sh` re-run whole-suite, checker: TLC `2026.07.18.145032` (rev `30cc360`), SHA-256
`cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`, `-workers 1`.

| Config | Expect | Result | Seconds | States generated / distinct |
| --- | --- | --- | ---: | ---: |
| `sab_graduate_on_stale` | violation | `NoOverDelete` violated | 0 | 6,394 / 2,102 |
| `sab_unbounded_defer` | temporal | `EventuallyFolded` violated | 1 | 402 / 102 |
| `stage1` | green | green | 0 | 31,041 / 8,445 |
| `witness_deferthenfold` | violation | `W_DeferThenFold` violated | 1 | 159 / 65 |

`stage1`'s green count (31,041 / 8,445) is identical to the original run above — BFS explores the
full reachable space regardless of search order. The two sabotage counterexamples' counts differ
from the original run because each seeds a fresh random exploration order and BFS reports the
first-found (not the shortest overall) counterexample; the violated property/invariant name is the
stable fact.

This run surfaced a genuine classifier bug: the pinned official jar prints
`Temporal property EventuallyFolded was violated.` (naming the one declared `PROPERTY`) where the
runner's classifier only recognized the generic `Temporal properties were violated.` — the form
every other recorded `tmp/tlc_*.log` in this tree used, because those were produced by the older,
no-longer-accepted TLC 2.19 jar. Under the pinned jar the row misclassified as `error` on first
run; widening the classifier's pattern to accept both the generic and the named-property form
(`run_gcrounddefer.sh`) fixed it, and the row now resolves to its declared `temporal` expectation.
Reproduction: `docs/superpowers/models/run_gcrounddefer.sh`.
