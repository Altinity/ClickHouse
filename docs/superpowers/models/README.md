---
description: 'Directory index of the CAS MergeTree TLA+ models: per-model purpose, status, configs, runner scripts, and pointers to the deep-dive index and design specs.'
sidebar_label: 'CAS TLA+ models directory'
sidebar_position: 1
slug: /superpowers/models
title: 'CAS MergeTree — TLA+ models directory index'
doc_type: 'guide'
---

# CAS MergeTree — TLA+ models directory {#cas-tla-models-directory}

This directory holds the TLA+ formal models for the content-addressed (CAS) MergeTree feature.
This README is the **complete directory index**: one entry per model, its status against the shipped
code, its config files, and its runner script. The deep-dive companion —
per-model invariants, counterexample traces, state counts, and the design decisions each model
forced — is `docs/superpowers/cas/06-tla-models.md`. Models added after that doc's last full
refresh (the ref snapshot+log family, `CaRetiredInRun*`, `CaGcCondemnMarkerGate`) are documented
only here and in their design specs.

Audit date for every status below: **2026-07-21**, verified against branch `cas-gc-rebuild`
(CAS code under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`).

## Conventions {#conventions}

- `<Model>.tla` — the module; `<Model>_*.cfg` — TLC configs for that module (prefix-matched, no
  sharing across modules; exception: `m_*.cfg` belong to `CaB140DangleMerge.tla`).
- `_stage*` / `_safe` / `_reduced` / `_fix` — positive gates: must pass.
- `_sab_*` / `_bug` — sabotages (negative controls): each removes one load-bearing rule and MUST
  produce a counterexample; an unexpected pass means the model lost its teeth.
- `_witness_*` — negated reachability: TLC reporting a "violation" means the state IS reachable
  (non-vacuity check).
- `run_*.sh` — thin TLC/Apalache wrappers; TLC jar expected at `../../../tmp/tla2tools.jar`
  (v2.19), Apalache at `../../../tmp/apalache/bin/apalache-mc` (0.58.0+).
- `states/`, `tmp/`, `_apalache-out/` — tool scratch output, gitignored / untracked.
- `*_RESULTS.md` — supplementary raw TLC run evidence for the matching model.

Status legend:

- **CURRENT** — gates shipped code; the mechanism it proves is what the code does.
- **CURRENT (partial)** — the model is the live gate for part of what it proves; the superseded
  part is called out in the entry.
- **STALE** — proves the current design family but predates later amendments; kept deliberately.
- **HISTORICAL** — record of a real production bug and its fix proof; mechanism has since evolved.

## Summary table {#summary-table}

| Model | Proves / gates | Status | Runner |
|---|---|---|---|
| `CaIncarnationCore.tla` | canonical incarnation-token GC core (fold → retire → fence → recheck → exact-token delete → cascade → trim) | CURRENT | `run_tlc.sh` |
| `CaIncarnationProofCore.tla` (+ `Apalache.tla`) | Apalache inductive invariant for the pre-B91 core fragment | STALE (kept) | `run_apalache.sh` |
| `CaBuildRootPrecommit.tla` | B140/B171 fix: build-root reachability + fail-closed commit + B199-S2 inline closure | CURRENT | (inline TLC, see 06 doc) |
| `CaGcLeaseCore.tla` | GC leader lease, advisory heartbeat (B160) | CURRENT | (inline TLC) |
| `CaCasMountCore.tla` | mount ownership + rev.6 observation-based / body-faithful reclaim | CURRENT | `run_mount.sh` |
| `CaGcRootLocalPartManifestCore.tla` | root-local part-manifest GC: fold, manifest cleanup, orphan sweep, attempt scoping | CURRENT (partial: fence/recheck phases superseded by ack-floor) | `run_gc_partmanifest.sh` |
| `CaGcAckFloorCore.tla` | one-pass GC round, clamp suppression, disaster-recovery rebuild | CURRENT (partial: writer-heartbeat floor superseded by v3) | `run_ackfloor.sh` |
| `CaGcAckFloorZombie.tla` | two-leader `delete_pending` two-phase graduation | CURRENT (partial: same caveat) | `run_ackfloor_zombie.sh` |
| `CaGcIndegRefoldCore.tla` | completion-seal cursor at `max(foldCursor, fenceVersion)` (in-degree re-fold undercount) | CURRENT | (inline TLC) |
| `CaGcShardIncarnationCore.tla` | D1 registry removal: per-shard incarnation + newborn round self-floor | CURRENT | (inline TLC) |
| `CaGcRoundDeferCore.tla` | GC round DEFER (skip-unchanged): force-fold before a due graduation, bounded deferral | CURRENT | (inline TLC) |
| `CaGcResurrectReuploadOrphan.tla` | re-condemn the current token when settling a replaced retired entry (S30 orphan) | CURRENT | (inline TLC) |
| `CaGcCondemnMarkerGate.tla` | graduation gated on confirmed durable condemn marker (codex triage №4) | CURRENT | `run_condemnmarker.sh` |
| `CaEdgeBeforeObserve.tla` | writer/GC simplification Gate A: promote-time tokened revalidation redundant given EDGE-BEFORE-OBSERVE | CURRENT | `run_ebo.sh` |
| `CaMetaDescriptor.tla` | writer/GC simplification Gate B v1: per-hash freshness meta, create bottom-up / delete top-down | CURRENT (predates the v3 2-state codec trim) | `run_meta.sh` |
| `CaManifestSweepWindow.tla` | orphan sweep must skip a committed body with a pending unsealed removal | CURRENT | `run_sweepwindow.sh` |
| `CaRetiredInRun.tla` | retired-list folded into the snapshot run (2-cursor merge, coverage coherence) | CURRENT | `run_retiredinrun.sh` |
| `CaRetiredInRunFoldAbortWitness.tla` | ADD-ONLY GC freshness meta: spare never clears a condemned marker (deposed-leader fix) | CURRENT | `run_foldabort_witness.sh` |
| `CaRefTableSnapshotLogCore.tla` | ref-table snapshot + append-only log protocol + rev.6 coverage-at-birth mount seal | CURRENT | `run_refsnaplog.sh` |
| `CaRefDeltaIntakeCore.tla` | GC ref-intake pagination (three-premise proof; cursor adoption atomic with fold commit) | CURRENT | `run_refintake.sh` |
| `CaRefFoldClampRecoveryCore.tla` | fold clamp always recoverable: per-log cleanup staging (transaction atomicity) | CURRENT | `run_foldclamp.sh` |
| `CaRefNsCleanupStaleLeaderCore.tla` | stale-leader namespace-cleanup pass aborts on completed-marker observation | CURRENT | `run_nscleanup_staleleader.sh` |
| `CaRefWriterCleanupCore.tla` | ref-table writer ownership lifecycle: precommit/promote/fence/successor cleanup | CURRENT | `run_refwcleanup.sh` |
| `CaB140DangleMerge.tla` (+ `m_*.cfg`) | B140 fix proof: trim-gate + cursor-in-snap jointly necessary | HISTORICAL | (inline TLC) |
| `CaB140DangleFaithful.tla` | B140 Phase-1 mechanism clean with faithful producers | HISTORICAL | (inline TLC) |

## Model groups {#model-groups}

### GC core and proofs {#group-gc-core}

- **`CaIncarnationCore.tla`** — the canonical adversarial GC core: concurrent writers and GC
  leaders, split-brain, debris, full-GC cut, resurrect/overwrite, trees and atomic cascade,
  namespace registry and evidence staleness (B91). Invariants `INV_NO_DANGLE`, `INV_NO_LOSS`,
  `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`; 11 sabotages. Configs: `_stage1..6*`, `_hunt_*`,
  `_reval_stage2`, `_sab_*`. Details: 06 doc §Area 1.
- **`CaIncarnationProofCore.tla`** + **`Apalache.tla`** — Apalache inductive invariant (`IndInv`,
  19 conjuncts) for the pre-B91 token-only fragment; stronger than bounded checking at its fixed
  bounds. STALE: predates the B91 amendments; re-derivation is an open follow-up; kept as the
  groundwork (CTI journal) for a future parametric proof. Configs: `_tlc.cfg`, `_tlc2h.cfg`.
- **`CaGcAckFloorCore.tla`** / **`CaGcAckFloorZombie.tla`** — the one-pass ack-floor round and the
  two-leader `delete_pending` gate. The GC-side round pipeline (`GBegin`/`GFold`/`GComplete`,
  `graduationDue`, `GRebuild`, clamp suppression, `gc/state` CAS guard) is CURRENT; the
  writer-heartbeat `min_ack` half was removed by the v3 freshness-meta redesign
  (`docs/superpowers/plans/2026-07-10-cas-freshness-meta-v3.md`) — graduation now paces on
  `condemn_round < current_round` alone. Details: 06 doc §Area 11.
- **`CaGcRootLocalPartManifestCore.tla`** — the largest model in the corpus (28 sabotages):
  precommit/missing-body states, owner transitions, orphan sweep, token-diff discovery, lazy trim,
  sharded reducers, attempt-scoped visibility, plus the `SkipParksDeadPrecommit` and
  promote-over-committed regression gates. The fence-era controls (`sab_nofence`,
  `sab_lazyfenceunsafe`, `sab_reducerownsfence`) document the mechanism the ack-floor redesign
  replaced and are kept as evidence. Details: 06 doc §Area 7.

### Focused GC bug gates {#group-focused-gates}

- **`CaGcLeaseCore.tla`** — B160 lease steal: epoch-fence CAS alone is safe; the advisory
  heartbeat eliminates false steals of an alive mid-round leader. 06 doc §Area 3.
- **`CaGcIndegRefoldCore.tla`** — the seal cursor must persist at `max(foldCursor, fenceVersion)`,
  else the next round re-folds a fence-window removal and drives the integer in-degree counter
  negative (`CORRUPTED_DATA` exception in C++). 06 doc §Area 8.
- **`CaGcShardIncarnationCore.tla`** — D1 registry removal: durable per-`(ns,shard)` incarnation +
  newborn shard born fenced to the current round; neither coordinate alone suffices. Raw TLC
  evidence: `CaGcShardIncarnationCore_RESULTS.md`. 06 doc §Area 9.
- **`CaGcRoundDeferCore.tla`** — a round may DEFER (re-adopt the sealed in-degree snapshot) only
  if no destructive decision is due; deferral is bounded. Raw TLC evidence:
  `CaGcRoundDeferCore_RESULTS.md`. 06 doc §Area 13.
- **`CaGcResurrectReuploadOrphan.tla`** — the S30 `RESURRECT-REUPLOAD-ORPHAN` leak: `closeBlob`
  must re-condemn the CURRENT token when settling a prior retired entry whose token differs
  (faithful to the touch-gated C++ fold that the canonical model abstracts away). 06 doc §Area 12.
- **`CaGcCondemnMarkerGate.tla`** — codex-review triage №4 (2026-07-17): `writeCondemnedMeta`
  failures are swallowed, so graduation to `delete_pending` must require CONFIRMED durable
  Condemned evidence (marker-write completion or a synchronous `loadMeta` re-check); otherwise the
  entry is carried fail-safe. Landed as `Gc::scheduleCondemnMarkerWrite` /
  `noteCondemnMarkerDurable` / the `condemn_markers_confirmed` set. Spec:
  `docs/superpowers/reports/2026-07-17-codex-review-triage.md` §3.4,
  `docs/superpowers/plans/2026-07-17-codex-triage-fix-wave.md` Task 1.

### Writer protection and freshness meta {#group-writer-meta}

- **`CaBuildRootPrecommit.tla`** — the live B140/B171 protection: build-root structural
  reachability and fail-closed commit are each necessary and jointly sufficient; the precommit
  closure must be recorded inline at precommit time (B199-S2 leak otherwise). 06 doc §Area 2.
- **`CaEdgeBeforeObserve.tla`** — Gate A of the writer/GC simplification
  (`docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md`): with
  `precommit → adopt/observe → promote` and same-pass decided deletes, promote-time revalidation
  of tokened leaves is redundant; K1/K3Head/K3AdoptCheck and the order itself stay load-bearing.
  06 doc §Area 13.
- **`CaMetaDescriptor.tla`** — Gate B v1: per-hash freshness meta, create bottom-up (body then
  meta), delete top-down (meta at captured etag, then body at condemn-time token); 7 sabotages.
  This is the variant that shipped (as the 2-state `{clean, condemned}` + `condemn_round` codec of
  the v3 plan); the model predates the 2-state trim and has not been re-run against it. 06 doc
  §Area 13.
- **`CaManifestSweepWindow.tla`** — the orphan sweep must not delete a committed body whose `-1`
  removal is appended but not yet sealed by the fold (`INV_FOLD_PROGRESS`). 06 doc §Area 13.

### Retired-in-run family {#group-retired-in-run}

Spec `docs/superpowers/specs/2026-07-10-cas-retired-in-snapshot-design.md`; landed 2026-07-11
(the separate retired list is gone — condemned state rides the source-edge run).

- **`CaRetiredInRun.tla`** — the 3-cursor → 2-cursor merge: settlement inside the snapshot run,
  fold-read coverage floor, monotone token mint, EDGE-BEFORE-OBSERVE staleness window. Sabotages:
  `inmem_token`, `attempt_reuse`, `no_pacing`.
- **`CaRetiredInRunFoldAbortWitness.tla`** — two bounded in-flight GC leaders; proves GC freshness
  meta is ADD-ONLY (a spare leaves the meta unchanged; only a token-displacing writer publishes
  clean). The `post_adoption_clear` sabotage proves the weaker "clear after the winning CAS" fix
  insufficient. Spec `docs/superpowers/specs/2026-07-11-cas-deposed-leader-clearsparedmeta-fix-design.md`.
  This witness also refuted the later §5 "absence means Clean" revival (see removed
  `CaMetaAbsenceClean` below).

### Ref-table snapshot + log family {#group-ref-table}

Spec `docs/superpowers/specs/2026-07-11-cas-ref-table-snapshot-log-design.md` (rev.4), extended by
`docs/superpowers/specs/2026-07-13-cas-ref-lease-exclusivity-rev6-design.md` (rev.6). The
migration landed (`Pool/CasRefProtocol.*`, `Formats/CasRefLogFormat.*`, `CasRefSnapshotFormat.*`,
`CasRefLedger.*`).

- **`CaRefTableSnapshotLogCore.tla`** — one table's snapshot + append-only log: strictly
  increasing txn ids, one-scan recovery, observed-durable-only cleanup, Completed-marker rebirth
  gate; rev.6 addendum (`Rev6MountRule`) seals a successor's mount coverage-at-birth
  (`NoDivergentFold`, `INV_FRESH_READER`, `INV_SNAP_DETERMINISTIC`). The `_latepred` configs
  document the pre-rev.6 cross-epoch expected-fail limitation, closed by the rev.6 grace removal;
  kept as expected-fail documentation.
- **`CaRefDeltaIntakeCore.tla`** — GC ref-intake pagination: resume-after-returned-key, cursor
  adoption atomic with the fold commit, cleanup requires BOTH cursor and snapshot coverage.
- **`CaRefFoldClampRecoveryCore.tla`** — per-log transaction atomicity for cleanup staging: a
  `-1` body token joins the round cleanup set only once the whole log folds; edge-granularity
  commit creates a permanent destructive freeze.
- **`CaRefNsCleanupStaleLeaderCore.tla`** — a stale leader's pending namespace-cleanup pass must
  re-read `gc/state`, abort on the `_cleanup` completed marker, and epoch-filter deletes, else it
  reclaims a recreated namespace.
- **`CaRefWriterCleanupCore.tla`** — writer-side ownership lifecycle: atomic promote, epoch-gated
  successor cleanup of stale precommits, retire-after-removal ordering.

### Mount ownership {#group-mount}

- **`CaCasMountCore.tla`** — sticky owner, durable monotone epoch, TTL lease; extended 2026-07-14
  for rev.6: observation-based reclaim (the reclaimer waits out `TTL + Drift` on its OWN clock
  after a stable token hold — `_sab_wallclockreclaim` reproduces the trust-the-foreign-wall-clock
  bug) and body-faithful reclaim (installs the successor's body, matching `CasServerRoot.cpp`).
  Current configs: `_stage1`, `_rev6_observe`, `_sab_adoptwedge`, `_sab_epochreset`,
  `_sab_fenceresurrect`, `_sab_foreigntakeover`, `_sab_wallclockreclaim`, `_witness_reclaim`,
  `_witness_observedreclaim`, `_witness_recoveryafterobservedreclaim`,
  `_witness_remountafterfence`. (The 06 doc's §Area 4 predates rev.6: `_sab_supersededwrites` and
  the `SupersededWriterMakesNoMutation` invariant were retired in the rev.6 task-2 commit.)

### Historical records (kept) {#group-historical}

- **`CaB140DangleMerge.tla`** (+ `m_both_buggy.cfg`, `m_cursorskip.cfg`, `m_trimonly.cfg`,
  `m_merged.cfg`) — the B140 trim-before-durable dangle across a lease handoff and the 2×2 proof
  that trim-gate + cursor-in-snap are jointly necessary. The cursor-in-snapshot principle carries
  forward into the ref snapshot+log design. 06 doc §Area 5.
- **`CaB140DangleFaithful.tla`** — the faithful-producer refutation of the Phase-1 mechanism.
  06 doc §Area 5.

## Removed models {#removed-models}

The following models gated superseded or rejected designs and were removed on 2026-07-21 (one
commit per model, motivation in each commit message; full text remains in git history and their
prose records stay in `docs/superpowers/cas/06-tla-models.md`):

| Model | Why removed |
|---|---|
| `CaGcCore.tla` | the original EBR epoch/generation GC design, superseded by the incarnation-token design (`CaIncarnationCore.tla`); no EBR machinery exists in code |
| `CaB140Dangle.tla` | Phase-1 B140 reproduction with unfaithful producers; superseded by `CaB140DangleFaithful.tla` |
| `CaResurrectLiveness.tla` | modeled the condemn-time `HeartbeatGuard`, never implemented; shipped protection is precommit-first reachability (`CaBuildRootPrecommit.tla`) |
| `CaBuildWatermark.tla` | modeled the per-candidate blob-guard `protectedByLiveBuild`, removed by B171; the surviving monotone `build_seq` floor lemma lives in precommit-ref reclaim |
| `CaBuildWatermarkNum.tla` | numeric companion of `CaBuildWatermark.tla`, same supersession |
| `CaMetaDescriptorRaw.tla` (+ `run_metaraw.sh`) | Gate B raw-body/terminal-tombstone variant, REJECTED by the v3 freshness-meta design |
| `CaMetaIncarnationKey.tla` (+ `run_inckey.sh`) | Gate B per-incarnation body-key variant, REJECTED (generation-in-key design, rejected twice) |
| `CaMetaAbsenceClean.tla` (+ `run_metaabsence.sh`) | gated §5 "absence means Clean" of the S3-budget spec, BLOCKED 2026-07-14 — its green was refuted (`Build::adoptEvidence` token-preserving relink reopens the deposed-leader data loss); no §5 code landed |
| `CaIncarnationCore.pdf`, `CaIncarnationCore.toolbox/` | generated TLA+ Toolbox pretty-print artifacts (regenerable from the module) |
