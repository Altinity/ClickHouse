# TLA+ model ↔ code currency review — 2026-06-22

Three parallel subagent audits of the CA-MergeTree TLA+ models vs the current C++ (branch
`cas-vfs-path-mapping`). Question asked: is each model current, does code↔model correspond, and is
there stale cruft from old designs? **No model edits were made** — this is the findings record;
dispositions below are recommendations pending decision.

## Bottom line
No *old-design* cruft (no EBR-epoch / `resurrect`-by-GET / `cas_owner` / `reuseBlob` / seal-TTL) in any
LIVE model — that design lives only in `CaGcCore.tla`, already quarantined HISTORICAL. The real drift is
around **B171** (precommit-reachability replaced per-candidate liveness/watermark guards) plus one
subsystem the core model fully specifies but the code defers (**M-F Full GC**).

## M-F = Milestone F = the deferred Full GC (spec `2026-06-10-...` §8)
GC is two-tier. **Regular GC** (shipped): O(delta) `fold→retire→fence→recheck→exact-token delete→cascade→trim`.
**Full GC (M-F)** (deferred, `CasGc.h:121` "API slot reserved"): rare checkpoint-diff full mark-sweep that
reclaims **debris** (objects from crashed/abandoned builds, keyed by `build_id`, heartbeat-gated) and
abandoned precommit manifests. The soak's `unreachable=N (M-F debris)` is exactly this: chaos kills builds
mid-upload → orphans that Regular GC only condemns (never deletes) → awaits the not-yet-built Full GC.
`dangling=0` holds; it is a space-reclaim deferral, not a safety gap (known B140 category).

## Canonical core — `CaIncarnationCore.tla`: MINOR-DRIFT (accurate, current)
Tight 1:1 mapping of the full pipeline, publish gate (`checkAndResolveDeps`), registry/fence-universe,
and INV-1 revival-from-source. Correctly does NOT model resurrect-by-GET (`WResurrect` = in-place
overwrite with fresh tag, "old resurrect minus the GET", `CasBuild.cpp:404`). Three discrepancies:
1. **Model-ahead:** `WHb*`/`GDebrisRetire`/`FGRead`/`FGCommit` (heartbeat/debris/full-GC-cut) are fully
   specified but DEFERRED in code (M-F). The shipped in-flight protection is B167 watermark + B171
   precommit reachability; `reclaimAbandonedPrecommit` liveness has no core-model counterpart.
2. **Precommit-first (B171/B188)** is the live safety mechanism but only obliquely modeled in the core
   (no first-class precommit-root edge); it IS modeled properly in `CaBuildRootPrecommit.tla`.
3. **Retained `EnableReval=FALSE` dead-token-oracle gate** (`deadTok`/`CondemnedAtView`) matches no code
   path (production is re-observation only). Sound but should be marked a superseded variant.

## Supporting models
| Model | Verdict | Reason |
|---|---|---|
| `CaBuildRootPrecommit.tla` | CURRENT | The real B171/B140 fix: precommit-first + reachability + fail-closed commit + `reclaimAbandonedPrecommit`. |
| `CaGcLeaseCore.tla` | CURRENT | Lease/steal/fence + heartbeat match `acquireOrRenewLease`/`Gc::heartbeat`; untouched by B171. |
| `CaResurrectLiveness.tla` | STALE vs shipped | Proves a condemn-time `HeartbeatGuard` never implemented (it's the deferred M-F subsystem). Not EBR cruft — models a fix the code doesn't contain. |
| `CaBuildWatermark.tla` | STALE vs shipped | Models the per-candidate `protectedByLiveBuild` *blob*-guard B171 deleted (`CasGc.cpp:979-982`). |
| `CaBuildWatermarkNum.tla` | STALE vs shipped | Same removed blob-guard subject; its monotone-`build_seq` floor lemma survives but now governs *precommit-ref reclaim* (`CasGc.cpp:1877`), not blob protection. |

**Conflict reconciled:** the cruft-sweep agent labeled the three above "CURRENT" (meaning: not EBR-era,
tied to a live bug-id). The deeper code-correspondence agent + the core audit are authoritative: they model
a per-candidate liveness/heartbeat guard the code either REMOVED (watermark → B171 reachability) or NEVER
shipped (heartbeat → deferred M-F). Their safety role fully migrated into `CaBuildRootPrecommit.tla`. So:
not old-design garbage, but stale against current code.

## B140 dangle models — keep all three (a deliberate progression)
- `CaB140Dangle.tla` — Phase-1 reproduction with UNFAITHFUL producers (marker-retaining strip,
  field-mixed generation adoption). Superseded-as-producer.
- `CaB140DangleFaithful.tla` — faithful refutation of the Phase-1 mechanism (clean 9.1M states); missing
  its own `_RESULTS.md` (recorded inside `CaB140DangleMerge_RESULTS.md`).
- `CaB140DangleMerge.tla` — first faithful reproduction + fix proof (trim-before-durable gap across lease
  handoff; `TrimGated`+`CursorInSnap` 2×2 matrix, clean 5.33M). The *current* B140 fix model is
  `CaBuildRootPrecommit.tla`.

## Proof core & housekeeping
- `CaIncarnationProofCore.tla` — consistent, intentionally-trimmed Apalache companion; induction covers
  only the pre-B91 W-REVALIDATE token-only fragment (self-flagged stale until re-derived).
- `CaGcCore.tla` — HISTORICAL banner present in `README.md` but MISSING in `RESULTS.md` and the module
  header (still self-labels "STABILIZED CORE").
- No real directory index — lease/watermark/resurrect/precommit/B140 models invisible from any README.

## Recommended actions
**Doc-only (low-risk):** add HISTORICAL banner to `RESULTS.md` + `CaGcCore.tla` header; add
"superseded-as-producer" banner to `CaB140Dangle.tla`; add "stale vs B171 — superseded by
`CaBuildRootPrecommit.tla`" banner atop the 3 watermark/resurrect models; build a real directory index.
**Needs decision:** (a) archive vs repurpose the 3 stale models (the watermark floor lemma → model
precommit-ref reclaim liveness instead); (b) re-derive `CaIncarnationProofCore` post-B91; (c) add a
first-class precommit-root edge to the core so its INV-NO-LOSS covers the shipped protection path.
