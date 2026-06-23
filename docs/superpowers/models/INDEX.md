# CA TLA+ models — index

Directory index of the content-addressed (CA) MergeTree TLA+ models, with currency status as of the
2026-06-23 model-vs-code review (`MODEL_CURRENCY_REVIEW_2026-06-22.md`). "CURRENT" = accurately models
shipped code on branch `cas-vfs-path-mapping`; "HISTORICAL/SUPERSEDED" = kept as record only.

## Current

| Model | Role | Results |
|---|---|---|
| `CaIncarnationCore.tla` | **Canonical** CA-GC core (incarnation-token): fold→retire→fence→recheck→exact-token-delete→cascade→trim, publish gate, registry/fence-universe, INV-1 revival-from-source | `CaIncarnationCore_RESULTS.md`, README `CaIncarnationCore_README.md` |
| `CaIncarnationProofCore.tla` | Apalache inductive-invariant companion to the core (pre-B91 W-REVALIDATE token-only fragment) | — |
| `Apalache.tla` | Apalache stdlib shim for the proof core | — |
| `CaBuildRootPrecommit.tla` | **The current B140/B171 fix model**: precommit-first + build-root reachability + fail-closed commit + `reclaimAbandonedPrecommit` | `CaBuildRootPrecommit_RESULTS.md` |
| `CaGcLeaseCore.tla` | B160 GC leader-lease / heartbeat steal (renew/steal/fence) | `CaGcLeaseCore_RESULTS.md` |
| `CaB140DangleMerge.tla` | Faithful B140 reproduction + fix proof (trim-before-durable gap across lease handoff) | `CaB140DangleMerge_RESULTS.md` |
| `CaB140DangleFaithful.tla` | Faithful refutation of the Phase-1 mechanism (clean 9.1M states) | (recorded inside `CaB140DangleMerge_RESULTS.md`) |

## Historical / superseded (kept as record — banners in-file)

| Model | Why superseded |
|---|---|
| `CaGcCore.tla` | Abandoned EBR/epoch/generation GC design (2026-06-07 era). Replaced by `CaIncarnationCore.tla`. `README.md`/`RESULTS.md` are this model's docs. |
| `CaResurrectLiveness.tla` | Models a condemn-time HeartbeatGuard never implemented (heartbeat-gated condemn = deferred M-F Full GC). Protection role → `CaBuildRootPrecommit.tla`. |
| `CaBuildWatermark.tla` | Models the per-candidate watermark blob-guard B171 removed. Watermark now drives precommit-ref reclaim only. |
| `CaBuildWatermarkNum.tla` | Same removed blob-guard subject; its monotone-`build_seq` floor lemma survives for precommit-ref reclaim, not blob protection. |
| `CaB140Dangle.tla` | Phase-1 B140 repro with unfaithful producers; refuted by `CaB140DangleFaithful.tla`, superseded by `CaB140DangleMerge.tla`. |

See `MODEL_CURRENCY_REVIEW_2026-06-22.md` for the full model↔code correspondence audit and the open
follow-ups (re-derive `CaIncarnationProofCore` post-B91; optionally add a first-class precommit-root edge
to the core; archive-vs-repurpose the three superseded watermark/resurrect models).
