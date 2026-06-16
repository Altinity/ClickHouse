# CaResurrectLiveness — TLC results (B167 resurrect convergence)

Model: `CaResurrectLiveness.tla`. Spec: `docs/superpowers/specs/2026-06-16-ca-resurrect-reupload-design.md`.
Checker: TLC (`tmp/tla2tools.jar`); a **liveness** check (temporal property under weak fairness), `CHECK_DEADLOCK FALSE` (the bounded spec terminates legitimately).
Run: `java -cp tmp/tla2tools.jar tlc2.TLC -config <cfg> CaResurrectLiveness.tla`.

This is the LIVENESS dimension the safety-only `CaIncarnationCore` does not check — and which therefore did not catch B167.

## What is modelled

A blob was referenced → dropped → CONDEMNED (zero in-degree). A build dedup-hits the same content and must re-reference (resurrect) it, then PUBLISH. Two modes:
- **Recreatable** (the fix): the build re-uploads a FRESH incarnation from its own bytes and references it in one step. A fresh incarnation is invisible to GC's manifest-fold until referenced, so GC cannot touch it in the upload→publish span.
- **Bodyless** (today's publish-gate path): the build has no bytes, so it must GET the existing condemned object (HEAD→GET→rewrite). GC's exact-token delete can land in the HEAD→GET window; the merge then re-creates the blob (which GC re-condemns) and re-observes — an adversarial GC that deletes in every window starves it.

Weak fairness is on the BUILD's actions only; GC is the unconstrained adversary.

## Property

`Liveness == <>published` — the build eventually publishes.

## Results

| config | `Recreatable` | distinct states | result |
|---|---|---|---|
| `CaResurrectLiveness_recreatable` | TRUE (fix) | 4 | **PASS** — `<>published` holds |
| `CaResurrectLiveness_sab_bodyless` | FALSE | 5 | **VIOLATED** — temporal counter-example (B167 livelock) |

Conclusions:
1. **The fix converges.** With `Recreatable=TRUE`, weak fairness forces the single atomic re-upload-and-reference and the build always publishes — regardless of what GC does to the old condemned incarnation.
2. **The bodyless gate is starvable.** With `Recreatable=FALSE`, a fair behaviour never publishes: GC keeps deleting the condemned incarnation and the merge keeps re-creating it (which GC re-condemns), so the build never holds a live incarnation long enough to reference it. This is B167.

## B167 livelock counterexample (`sab_bodyless`, lasso)

```
State 1  present=TRUE  condemned=TRUE  published=FALSE   (a dedup hit lands on the condemned-present blob)
State 2  GcDelete       -> present=FALSE                  (GC exact-token deletes the condemned incarnation)
Back to State 1  ReCreate -> present=TRUE, condemned=TRUE (merge re-uploads; GC re-condemns the unreferenced re-creation)
   ... GcDelete -> ReCreate -> GcDelete -> ...  published never becomes TRUE
```
This lasso faithfully matches soak #11: the merge re-uploaded the blob (412 dedup hit on a condemned incarnation), GC re-condemned+deleted it, the bodyless gate could never re-reference it before the next delete → the part never committed (broken). The `BodylessObserve`/`BodylessComplete` race (delete in the HEAD→GET window) is a longer fair behaviour with the same outcome; TLC reports the shortest witness.

## Reproduce

```bash
cd docs/superpowers/models
JAR=../../../tmp/tla2tools.jar
for c in CaResurrectLiveness_recreatable CaResurrectLiveness_sab_bodyless; do
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -config $c.cfg CaResurrectLiveness.tla
done
```
