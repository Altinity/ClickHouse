# CaGcShardIncarnationCore — TLC results (D1 phase-0 gate)

**Model:** `CaGcShardIncarnationCore.tla` (focused sibling, in the style of `CaGcIndegRefoldCore`).
**Spec:** `docs/superpowers/specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`.
**Date:** 2026-07-01. **TLC:** 2.19, `tla2tools.jar`, java 21.

Run: `java -XX:+UseParallelGC -cp <tla2tools.jar> tlc2.TLC -workers auto -config <cfg> CaGcShardIncarnationCore.tla`

## What it decides

The proven `CaIncarnationCore.tla` shows the namespace **registry** is load-bearing for create-ordering (`SabotageNoRegistry` dangles: a newborn namespace's publish floor stays 0). D1 removes the registry and replaces its role with **two coordinates**:

1. a durable, never-reused per-`(ns,shard)` **incarnation** (`sInc`, from the `nextInc` allocator; reuses `(writer_epoch, build_sequence)` in the implementation) — identity; the fold cursor is keyed by it, so a delete+recreate at the same path cannot be ABA-confused;
2. the pool-global GC **round** — a newborn shard is born fenced to the current `gcRound` (self-floor: the writer reads `gc/state.round`), and GC discovers+fences every present shard by LIST each round.

The gate: with the registry gone, do `INV_NO_DANGLING` (no committed ref to an absent/dead-token blob — the no-return theorem) and `INV_NO_ORPHAN_EDGE` (no folded edge outlives its shard object — the blob-leak guard) still hold?

## Results

Bounds (all configs): `Blobs={b1}`, `Shards={n1,n2}`, `Writers={w1,w2}`, `Leaders={L1}`, `MaxTok=2`, `MaxRound=3`, `MaxLog=6`, `MaxInc=3`.

| Config | `SabotageNewbornNoFloor` | `SabotagePathKeyedCursor` | `SabotageDeleteBeforeFold` | Expected | Result |
|---|---|---|---|---|---|
| `_design` | F | F | F | hold | ✅ **No error** — 724,944 distinct states, depth 42 |
| `_sab_newbornnofloor` | T | F | F | dangle | ❌ `INV_NO_DANGLING` violated |
| `_sab_pathkeyedcursor` | F | T | F | dangle | ❌ `INV_NO_DANGLING` violated |
| `_sab_deletebeforefold` | F | F | T | leak | ❌ `INV_NO_ORPHAN_EDGE` violated |

`TypeOK` holds in every config.

## Conclusions

- **THM-NO-RETURN holds without the registry.** The `_design` config preserves `INV_NO_DANGLING`: the newborn self-floor (`fence := gcRound` at birth) + LIST-fence of every present shard close the shared-blob create race that the registry previously closed. **The registry can be deleted** — the spec's §risks fallback (an ephemeral `pending-newborns` object) is NOT needed.
- **Two coordinates, not one.** `_sab_newbornnofloor` (drop the round self-floor, keep only incarnation) dangles → the pool-global round is irreducible. `_sab_pathkeyedcursor` (drop the incarnation from the cursor, keep only the round) dangles via ABA → the incarnation is irreducible. Neither coordinate alone suffices; the design's two-coordinate model is minimal.
- **Reclaim ordering is load-bearing.** `_sab_deletebeforefold` orphans a folded edge → the target blob's in-degree can never drain (a leak). Reclaim must require the shard's journal fully folded (tombstone included) before the token-guarded `deleteExact`.

## Model teeth (why the ✅ is meaningful)

Each of the three deliberate breaks yields a counterexample against the exact invariant the design says it protects, so the model can detect the failure modes it claims to rule out — the `_design` pass is not vacuous. The model reuses `CaIncarnationCore`'s proven idioms verbatim: `CondemnedAtView`/`deadTok` (no-return oracle), `ViewableRound` (same-round retire visibility), and the `fold -> retire -> fence -> recheck -> delete` round with exact-token late-landing deletes.

## Residual / not modelled

- Concurrent GC leaders (`Leaders={L1}` here) — proven separately in `CaGcRootLocalPartManifestCore` (attempt-scoped generation) and orthogonal to the registry-removal question.
- The precommit fold-barrier (body-not-yet-present) — abstracted: `WPublish` is atomic (shard present + committed ref together), which is the stronger/worse case for the create race, so the safety result is conservative.
- Backend LIST consistency is assumed (discovery == present shards); the implementation must confirm strongly-consistent LIST per backend (spec §2, LIST-CONSISTENCY).
