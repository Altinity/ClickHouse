---
description: 'TLC evidence for the focused unconditional CAS blob-publication protocol, exact sabotages, and non-vacuity witnesses'
sidebar_label: 'CAS blob publication TLC results'
sidebar_position: 4
slug: /superpowers/models/CaBlobPublishCore-results
title: 'CAS blob publication TLC results'
doc_type: 'reference'
---

# `CaBlobPublishCore` results {#cablobpublishcore-results}

## Verdict {#verdict}

The focused safety gate passed twice from identical final inputs. Both complete runs printed
`ALL EXPECTATIONS MET`: every sabotage violated its exact mapped invariant, the safe configuration
exhausted its state graph without an error, and all three negated-reachability witnesses fired while
the safety invariants remained enabled.

## Checker and finite scope {#checker-and-finite-scope}

- TLC: `2026.07.18.145032`, revision `30cc360`.
- Pinned `tla2tools.jar` SHA-256:
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Workers: `1`.
- Writers: `w1` and `w2`; one GC actor.
- Bounds: two publication attempts per writer, two total landed writes, four metadata versions, and
  two fence generations. Two landed writes are sufficient for every required race, replacement,
  exact-delete retry, and late-landing trace.
- The safe configuration explores both deterministic ETag tokens and allocation-based generation
  tokens. ETag-specific sabotage and witness configurations use `ETag` so byte-identical envelope
  reproduction is explicit.
- TLC metadata, traces, and row logs were stored below the repository `tmp` directory.
- The runner passes `-noGenerateSpecTE`, so TLC does not create source-adjacent trace modules.

The model separates durable precommit, `HEAD`, metadata observation, publication selection, backend
landing, response loss, recovery, metadata reconciliation, readiness, commit, fence loss, and exact
deletion. Each writer has a fixed staged envelope and monotonic `publicationAttempted` history.
ETags are a deterministic function of envelope plus payload; generation tokens consume `nextToken`
on every landed write. A single queued exact-delete record remains available for retries, which is
the authorization needed by all three staged-copy regressions.

## Safety invariants {#safety-invariants}

The safe run checks these stable names:

- `CommittedRefHasContent`
- `ReadyRequiresObservedMaterialization`
- `CondemnedNeedsFreshPublication`
- `FreshAfterCondemned`
- `PublicationAttemptIsMonotonic`
- `VerbatimCopyOnlyFirstAbsent`
- `ExactDeleteCannotRemoveFreshIncarnation`
- `PublicationRequiresDurablePrecommit`
- `ReadyRequiresCleanMeta`
- `FencedWriterCannotCommit`
- `KeyNamesPayload`

`CommittedRefHasContent` is intentionally logical: a committed reference requires a present body
with the key's payload, but does not require the current incarnation token to equal a writer's prior
observation.

## First complete run {#first-complete-run}

Command output: `build/task1_blobpublish_tla.log`. TLC row-log run ID:
`CaBlobPublishCore-11-1787382593629836842`.

| Configuration | Expected | Exact observed result | Generated | Distinct | Seconds |
|---|---|---|---:|---:|---:|
| `CaBlobPublishCore_sab_adopt_condemned.cfg` | violation | `CondemnedNeedsFreshPublication` | 83 | 49 | 0 |
| `CaBlobPublishCore_sab_reuse_condemned_envelope.cfg` | violation | `FreshAfterCondemned` | 303 | 159 | 1 |
| `CaBlobPublishCore_sab_recopy_after_condemned.cfg` | violation | `ExactDeleteCannotRemoveFreshIncarnation` | 28,567 | 10,778 | 1 |
| `CaBlobPublishCore_sab_recopy_after_absent.cfg` | violation | `ExactDeleteCannotRemoveFreshIncarnation` | 17,311 | 6,691 | 0 |
| `CaBlobPublishCore_sab_first_condemned_then_copy.cfg` | violation | `VerbatimCopyOnlyFirstAbsent` | 4,182 | 1,547 | 0 |
| `CaBlobPublishCore_sab_unconditional_delete.cfg` | violation | `ExactDeleteCannotRemoveFreshIncarnation` | 608 | 284 | 1 |
| `CaBlobPublishCore_sab_ready_without_reobserve.cfg` | violation | `ReadyRequiresObservedMaterialization` | 118 | 65 | 0 |
| `CaBlobPublishCore_sab_publish_before_precommit.cfg` | violation | `PublicationRequiresDurablePrecommit` | 58 | 39 | 0 |
| `CaBlobPublishCore_sab_skip_meta_clean.cfg` | violation | `ReadyRequiresCleanMeta` | 212 | 107 | 1 |
| `CaBlobPublishCore_sab_commit_after_fence.cfg` | violation | `FencedWriterCannotCommit` | 146 | 81 | 0 |
| `CaBlobPublishCore_sab_wrong_payload.cfg` | violation | `KeyNamesPayload` | 56 | 32 | 0 |
| `CaBlobPublishCore_safe.cfg` | green | `green` | 11,023,983 | 1,696,943 | 67 |
| `CaBlobPublishCore_witness_racing_publishers.cfg` | witness | `WitnessRacingPublishers` reached | 888 | 380 | 1 |
| `CaBlobPublishCore_witness_staged_retag.cfg` | witness | `WitnessStagedRetag` reached | 5,087 | 1,783 | 0 |
| `CaBlobPublishCore_witness_late_landing.cfg` | witness | `WitnessLateLanding` reached | 441 | 203 | 0 |

The safe graph completed at depth `34` with zero states left on the queue. The TLC log reports
`01min 06s`; the runner's whole-second measurement reports `67` seconds.

## Second complete run {#second-complete-run}

Command output: `build/task1_blobpublish_tla_rerun.log`. TLC row-log run ID:
`CaBlobPublishCore-11-1787382699119372279`.

| Configuration | Expected | Exact observed result | Generated | Distinct | Seconds |
|---|---|---|---:|---:|---:|
| `CaBlobPublishCore_sab_adopt_condemned.cfg` | violation | `CondemnedNeedsFreshPublication` | 83 | 49 | 0 |
| `CaBlobPublishCore_sab_reuse_condemned_envelope.cfg` | violation | `FreshAfterCondemned` | 303 | 159 | 0 |
| `CaBlobPublishCore_sab_recopy_after_condemned.cfg` | violation | `ExactDeleteCannotRemoveFreshIncarnation` | 28,567 | 10,778 | 1 |
| `CaBlobPublishCore_sab_recopy_after_absent.cfg` | violation | `ExactDeleteCannotRemoveFreshIncarnation` | 17,311 | 6,691 | 1 |
| `CaBlobPublishCore_sab_first_condemned_then_copy.cfg` | violation | `VerbatimCopyOnlyFirstAbsent` | 4,182 | 1,547 | 0 |
| `CaBlobPublishCore_sab_unconditional_delete.cfg` | violation | `ExactDeleteCannotRemoveFreshIncarnation` | 608 | 284 | 0 |
| `CaBlobPublishCore_sab_ready_without_reobserve.cfg` | violation | `ReadyRequiresObservedMaterialization` | 118 | 65 | 1 |
| `CaBlobPublishCore_sab_publish_before_precommit.cfg` | violation | `PublicationRequiresDurablePrecommit` | 58 | 39 | 0 |
| `CaBlobPublishCore_sab_skip_meta_clean.cfg` | violation | `ReadyRequiresCleanMeta` | 212 | 107 | 0 |
| `CaBlobPublishCore_sab_commit_after_fence.cfg` | violation | `FencedWriterCannotCommit` | 146 | 81 | 1 |
| `CaBlobPublishCore_sab_wrong_payload.cfg` | violation | `KeyNamesPayload` | 56 | 32 | 0 |
| `CaBlobPublishCore_safe.cfg` | green | `green` | 11,023,983 | 1,696,943 | 66 |
| `CaBlobPublishCore_witness_racing_publishers.cfg` | witness | `WitnessRacingPublishers` reached | 888 | 380 | 1 |
| `CaBlobPublishCore_witness_staged_retag.cfg` | witness | `WitnessStagedRetag` reached | 5,087 | 1,783 | 0 |
| `CaBlobPublishCore_witness_late_landing.cfg` | witness | `WitnessLateLanding` reached | 441 | 203 | 0 |

The second safe graph also completed at depth `34` with zero states left on the queue. The TLC log
reports `01min 05s`; the runner reports `66` seconds.

## Counterexample audit {#counterexample-audit}

The underlying TLC traces were inspected, not only the runner summary:

- `sab_recopy_after_condemned` performs an absent verbatim staged copy, backend landing, response
  loss, recovery `HEAD`, a `Condemned` metadata observation with its exact token queued, the forbidden
  identical copy, `Clean` reconciliation, and the old exact-delete attempt that removes the newer
  write serial.
- `sab_recopy_after_absent` performs the same ambiguous first copy, condemnation, an exact deletion
  that leaves its authorization queued, recovery `HEAD` of absence, identical copy, `Clean`
  reconciliation and readiness, then the queued exact-delete retry removes the newer write serial.
- `sab_first_condemned_then_copy` starts from a present body, observes `Condemned`, begins a retagged
  stream, loses the response before landing, enters recovery, lets the queued exact delete remove the
  old body, observes absence, and then selects the forbidden original staged copy.

Every other sabotage trace ended at the mapped invariant shown in the tables. No timeout, parse
error, deadlock, wrong invariant, or unrelated TLC error was classified as a pass.

## Witness audit {#witness-audit}

- `WitnessRacingPublishers` records both writers observing the same absent write serial and then
  landing equivalent publications.
- `WitnessStagedRetag` records an absent verbatim staged copy, backend landing, response loss,
  recovery `HEAD`, `Condemned` re-observation, and a fresh retagged stream landing.
- `WitnessLateLanding` records response loss before backend landing, recovery `HEAD`, and then
  `LateBackendLand` from the old attempt.

Each witness config lists all eleven safety invariants before its negated reachability invariant.
The safe config explores the same honest behavior with both token families to completion.

## Commands {#commands}

```bash
mkdir -p build tmp
docs/superpowers/models/run_blobpublish.sh > build/task1_blobpublish_tla.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task1_blobpublish_tla.log
docs/superpowers/models/run_blobpublish.sh > build/task1_blobpublish_tla_rerun.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task1_blobpublish_tla_rerun.log
```

Both `grep` commands exited `0`. The complete runner output for each run ended with
`ALL EXPECTATIONS MET`.

## Scope note {#scope-note}

This is a bounded safety proof, not a liveness proof or a provider wire-format test. The two-write
bound covers every named release-gate counterexample and witness. Multipart behavior, S3 copy bytes,
and GCS request semantics remain live-test obligations outside this focused model.
