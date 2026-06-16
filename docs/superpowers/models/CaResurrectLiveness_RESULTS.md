# CaResurrectLiveness — TLC results (B167 resurrect convergence)

Model: `CaResurrectLiveness.tla`. Spec: `docs/superpowers/specs/2026-06-16-ca-resurrect-reupload-design.md`.
Checker: TLC (`tmp/tla2tools.jar`); a **liveness** check (temporal property under weak fairness of the
build's own actions), `CHECK_DEADLOCK FALSE` (the bounded spec terminates legitimately).
Run: `java -cp tmp/tla2tools.jar tlc2.TLC -config <cfg> CaResurrectLiveness.tla`.

This is the LIVENESS dimension the safety-only `CaIncarnationCore` does not check — and which therefore
did not catch B167.

## Why this model was REWRITTEN (the false-confidence trap)

The first version of this model collapsed the writer-side fix into ONE atomic step
(`BuildRecreatable: published' = TRUE`), justified by the comment *"a fresh incarnation is not in any
manifest fold until referenced, so GC cannot touch it in the span."* **That assumption is false.** The
condemned blob is `everEdged` (it was published once, then dropped — that is *why* it is condemned) and
stays `InDeg=0` until this build publishes. So GC's condemn guard `present ∧ everEdged ∧ InDeg=0` is
satisfied for the build's OWN fresh incarnation too — GC can re-condemn and exact-token delete it in the
upload→publish span. Collapsing that span to an atomic step "proved" convergence that does not exist.

The rewritten model makes upload→publish NON-atomic and lets GC act in the gap, with the **build
heartbeat guard** as the checked variable. The guard is what the old model silently assumed.

## What is modelled

A dedup hit lands on a stale condemned incarnation. The build has the body in hand (re-invokable
`BlobSource`), so it never GETs — it re-streams a FRESH incarnation stamped with its OWN live `build_id`
(`BuildUpload`), then references it and publishes (`BuildPublish`). GC condemns (`GcCondemn`) and
exact-token deletes (`GcDelete`) as an unconstrained adversary. The fix:

- **`HeartbeatGuard = TRUE`**: incremental GC reads the envelope `build_id` and refuses to CONDEMN a
  blob owned by a live build. Condemn guard becomes `present ∧ everEdged ∧ InDeg=0 ∧ ¬liveBuild(build_id)`.
- **`HeartbeatGuard = FALSE`**: today's incremental GC — condemns on zero-in-degree alone.

Weak fairness is on the BUILD's actions only; GC is unconstrained.

## Property

`Liveness == <>published` — the build eventually publishes.

## Results

| config | `HeartbeatGuard` | distinct states | result |
|---|---|---|---|
| `CaResurrectLiveness_guard`   | TRUE (fix) | 4 | **PASS** — `<>published` holds |
| `CaResurrectLiveness_noguard` | FALSE      | 7 | **VIOLATED** — temporal counter-example (B167 livelock) |

Conclusions:
1. **The guard converges.** With the heartbeat guard, once `BuildUpload` mints the build's fresh
   incarnation, GC can neither condemn nor delete it (`freshOwned` ⇒ guard blocks `GcCondemn`, and
   `GcDelete` needs `condemned`). The state `present ∧ freshOwned ∧ ¬condemned` is stable, so weak
   fairness forces `BuildPublish`.
2. **Writer-side re-upload ALONE is starvable.** With the guard off, a fair behaviour never publishes —
   crucially this is the BODY-IN-HAND writer, not the old bodyless GET path. This is the sharper B167.

## B167 livelock counterexample (`noguard`, lasso)

```
State 1  present=T condemned=T freshOwned=F published=F   (dedup hit on a stale past-build condemned incarnation)
State 2  GcDelete    -> present=F condemned=F freshOwned=F (GC exact-token deletes the stale incarnation)
State 3  BuildUpload -> present=T condemned=F freshOwned=T (the build re-streams its OWN fresh incarnation)
State 4  GcCondemn   -> present=T condemned=T freshOwned=T (guard OFF: GC re-condemns the build's OWN fresh incarnation)
Back to State 2  GcDelete -> ...                            (GC deletes it before the build references it)
   ... GcDelete -> BuildUpload -> GcCondemn -> ...  published never becomes TRUE
```
State 3→4 is the smoking gun: GC condemning a `freshOwned=TRUE` incarnation. The old atomic model could
not express this transition, which is why it missed the livelock. This lasso matches soak #11: the merge
re-uploaded the blob, GC re-condemned + deleted it, and the build never held a live incarnation long
enough to reference it → the part never committed (broken).

## Reproduce

```bash
cd docs/superpowers/models
JAR=../../../tmp/tla2tools.jar
for c in CaResurrectLiveness_guard CaResurrectLiveness_noguard; do
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -config $c.cfg CaResurrectLiveness.tla
done
```
