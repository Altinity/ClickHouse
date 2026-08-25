---
description: TLA+ gate results for Stage B Task 8A writer duties and orphan-manifest nomination.
sidebar_label: Stage B Task 8A TLA results
sidebar_position: 999
slug: /superpowers/models/stage-b-task-8a-results
title: Stage B Task 8A TLA+ results
doc_type: reference
---

# Stage B Task 8A TLA+ results {#stage-b-task-8a-tla-results}

This batch extends `CaRefWriterCleanupCore.tla` with the unresolved owner-grant duty lifecycle and
extends `CaRefFoldClampRecoveryCore.tla` with neutral orphan-manifest nomination. The latter is the
smallest semantically matching model: it already owns the candidate-round → durable `gc/state` CAS →
post-CAS exact-delete boundary. A separate model would duplicate that boundary without adding an
independent interleaving.

Both runners used the pinned official TLC `2026.07.18.145032`, SHA-256
`cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`, with one worker. The initial
sabotage-first run failed closed because the newly named properties were not yet present. The final
run checks each sabotage against its exact property name, reaches each negated witness, and exhausts
both honest configurations.

## Writer cleanup duties {#writer-cleanup-duties}

An unresolved every-attempt grant is modeled as a possible durable precommit. Failure queues an
in-memory duty while the mount lives. Resolution as `Adopted` requires exact owner removal before
retirement; resolution as `Rejected` proves no owner exists. `INV_NO_RETIRE_UNCERTAIN_GRANT` prevents
the duty from disappearing while the outcome is unknown. Existing successor cleanup remains the
seal-derived path for durable precommits left by a dead predecessor.

| config | expected result | observed result | distinct states | depth |
|---|---|---|---:|---:|
| `_sab_retirebeforeremoval` | `INV_RETIRE_AFTER_REMOVAL` red | exact red | 95 | 4 |
| `_sab_retireuncertain` | `INV_NO_RETIRE_UNCERTAIN_GRANT` red | exact red | 30 | 4 |
| `_sab_successorcurrentepoch` | `INV_NO_WRONGFUL_RECLAIM` red | exact red | 22 | 3 |
| `_sab_cancelbeforedurable` | `INV_NAMESPACE_REMOVAL_COMPLETE` red | exact red | 113 | 4 |
| `_witness_duty_adopt` | `WITNESS_DUTY_ADOPT_DRAIN` red | exact red | 76 | 6 |
| `_witness_duty_reject` | `WITNESS_DUTY_REJECT_DRAIN` red | exact red | 59 | 5 |
| `_safe` | green | green | 64,090 | 17 |

The model separates the caller-visible outcome from the hidden durable reality. The new sabotage
reaches `StartBuild` → `FailBuild` → `RetireFailedBuild` with outcome `Unresolved`, reality `Adopted`,
and the precommit still present: the only live duty is retired. The honest witnesses reach both drains:
`ResolveGrantAdopted` → `RemoveFailedPrecommit` → `RetireFailedBuild`, and
`ResolveGrantRejected` → `RetireFailedBuild`.

## Neutral orphan nomination {#neutral-orphan-nomination}

Exact GET/decode discovers an orphan manifest as a neutral candidate. It does not consume a B2
ordinal and does not enter unmatched-remove accounting. `CommitRound` adopts the candidate in the
round's `gc/state` CAS; only an adopted nomination may reach exact-token deletion. Process death
after adoption is therefore leak-safe and rediscoverable, not a promise that the dead process retries.

| config | expected result | observed result | distinct states | depth |
|---|---|---|---:|---:|
| `_sab_edgegranularity` | `NoDeleteBehindClamp` red | exact red | 21 | 6 |
| `_sab_deletebeforeadoption` | `NominationAdoptedBeforeManifestDelete` red | exact red | 6 | 3 |
| `_sab_nominationcontaminates` | `NeutralNominationPreservesRefAccounting` red | exact red | 2 | 2 |
| `_witness_nomination_adopted` | `WITNESS_NEUTRAL_NOMINATION_ADOPTED` red | exact red | 18 | 6 |
| `_witness_manifest_deleted` | `WITNESS_MANIFEST_DELETE_AFTER_ADOPTION` red | exact red | 25 | 7 |
| `_safe` | green | green | 58 | 12 |

The ordering sabotage reaches `DiscoverNeutralNomination` → `DeleteNominatedManifest` without a
round CAS. The accounting sabotage goes red at discovery. Honest reachability is
`DiscoverNeutralNomination` → fold round → `CommitRound`, followed optionally by
`DeleteNominatedManifest`; the manifest is still present at adoption in the first witness.

## Reproduction {#reproduction}

From `docs/superpowers/models`:

```bash
TLC_JAR=../../../tmp/tla2tools-official.jar ./run_refwcleanup.sh
TLC_JAR=../../../tmp/tla2tools-official.jar ./run_foldclamp.sh
```

Both commands finish with `ALL EXPECTATIONS MET`.
