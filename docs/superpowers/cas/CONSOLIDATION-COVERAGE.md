---
description: 'Coverage matrix for the CAS documentation consolidation: maps every doc slated for deletion to the new consolidated doc(s) and section that capture its durable content (invariants, decisions, rejected paths, key technical detail). This is the audit artifact for the sanity gate that precedes irreversible deletion.'
sidebar_label: 'Consolidation coverage matrix'
sidebar_position: 10
slug: /superpowers/cas/consolidation-coverage
title: 'CAS Docs — Consolidation Coverage Matrix'
doc_type: 'reference'
---

# CAS Docs — Consolidation Coverage Matrix {#cas-consolidation-coverage}

This matrix is the mandatory sanity artifact for the CAS documentation consolidation
(spec `specs/2026-07-02-cas-docs-consolidation-design.md`, plan
`plans/2026-07-02-cas-docs-consolidation.md`). It maps **every** doc under
`docs/superpowers/` that is slated for deletion to the new consolidated doc(s) + section that
capture its durable content: invariants, architectural decisions, rejected/failed paths, and
load-bearing technical detail.

**Rule (from the sanity protocol):** no obsolete doc is deleted unless this matrix names where its
important content now lives. Ephemeral play-by-play (dated status, task checkboxes, night-log
narration) is intentionally dropped and is NOT a coverage gap.

The **new folder** `docs/superpowers/cas/` (README, ROADMAP, `01`–`08`, this matrix) is NOT a
deletion candidate.

---

## KEEP — not deleted (excluded from deletion) {#keep}

These are explicitly retained and are NOT deletion candidates:

- All TLA+ model **sources**: `docs/superpowers/models/*.tla` and `docs/superpowers/models/*.cfg`.
- Active D1 work: `specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`,
  `plans/2026-07-01-cas-shard-incarnation-and-registry-removal.md`,
  `models/CaGcShardIncarnationCore_RESULTS.md`.
- This consolidation's own `specs/2026-07-02-cas-docs-consolidation-design.md` +
  `plans/2026-07-02-cas-docs-consolidation.md`.
- `reports/2026-07-02-d2-scenario-triage.md` (current D2 triage).
- `deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md` (still-actionable backlog).
- The entire new `cas/` folder.

> Note: `models/CaGcShardIncarnationCore_RESULTS.md` is KEEP per the D1-active carve-out, even though
> the other `models/*_RESULTS.md` are delete-candidates. Its durable content is nonetheless also
> indexed in `06-tla-models.md §area-shard-incarnation`.

---

## DELETE-CANDIDATE coverage table {#delete-candidates}

Legend for target column: `01` = `01-architecture.md`, `02` = `02-methodology.md`,
`03` = `03-writer-protocol.md`, `04` = `04-gc-protocol.md`, `05` = `05-formats-and-backend.md`,
`06` = `06-tla-models.md`, `07` = `07-s3-budget.md`, `08` = `08-testing-and-soak.md`,
`RM` = `ROADMAP.md`.

### Top-level narrative / architecture docs {#top-level}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `content_addressed_mergetree.md` | "Git for MergeTree" framing; object model (blobs/refs/manifests); shared-nothing vs shared-state; what it buys / does not buy | `01 §what-cas-is`, `§object-model`, `§what-it-buys` |
| `incarnation-tagged-cas.md` | Incarnation-token identity; exact-token deletes; blob one-key-per-hash; body-not-key incarnation | `01 §incarnation-identity`, `§rejected-merkle-tree-layer`, `04 §recheck` |
| `CAS_MERGETREE_CODE_REVIEW_GUIDE.md` | Reviewer orientation (code map, invariants to check) | mined into `01 §key-invariants`, `02` (method); ephemeral guide dropped |
| `B140-dangle-HANDOFF.md` | B140 dangle root cause + fix handoff | `04 §trim` (B140 HISTORY), `06 §area-b140`, `02 §tla-b140` |
| `cas-gc-unattended-execution-log.md` | GC redesign method + TLA+-gate discipline + Phase-1a review blocker | mined into `02 §subagent-driven`, `§tla-gate`; log narration dropped |
| `cas-unattended-work-log-2026-06-24.md` | Format-framework work method | mined into `02`, `05`; log narration dropped |
| `NIGHT_LOG_2026-06-04.md` | TDD bugs found (cancelImpl, TOCTOU) | `02 §tdd`; log narration dropped |
| `NIGHT_LOG_2026-06-05.md` | Simplification passes; resurrect-cap-8 soak bug | `02 §subagent-driven`, `§soak-oracle`; narration dropped |
| `UNATTENDED-WORKLOG-2026-06-18-b140-dangle.md` | B140 fix T4→T10 sequence; test oracles | `02 §tdd`, `§tla-b140`, `04 §trim`; narration dropped |
| `UNATTENDED-WORKLOG-2026-06-19-ca-vfs.md` | VFS run regression-clean determination; harness fragility | `02 §systematic-debugging`, `08 §soak-limitations`; narration dropped |
| `worklogs/2026-06-28-cas-gc-attempt-scoped-generation-worklog.md` | Attempt-scoped generation implementation notes | `04 §attempt-scoped-generations`; narration dropped |
| `worklogs/2026-06-29-cas-layout-hot-cold-split-worklog.md` | Hot/cold split implementation notes | `01 §pool-layout`, `§why-the-split`; narration dropped |

### Architecture / core-model specs {#arch-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/content_addressed_shared_mergetree_design.md` | Full v3 design: object model, layout, zero-copy replacement rationale | `01` (whole doc), `RM §area-architecture` |
| `specs/2026-06-10-ca-incarnation-store-design.md` | Incarnation-token design that replaced EBR + Merkle-treeId | `01 §incarnation-identity`, `§rejected-ebr-gc`, `§rejected-merkle-tree-layer`, `04` |
| `specs/2026-06-11-ca-core-refactor-design.md` | Core refactor (treeId removal, object-model cleanup) | `01 §object-model`, `05 §rejected-tree-layer` |
| `specs/2026-06-28-cas-layout-hot-cold-split-design.md` | Hot/cold split; `cas/refs/` vs `cas/manifests/`; GC discovery cost fix | `01 §pool-layout`, `§why-the-split`, `05 §layout-keys` |
| `specs/2026-06-11-ca-apalache-inductive.md` (plan) | Apalache inductive-invariant approach | `06 §caincarnationproofcore`, `02 §tla-gate` |
| `plans/2026-06-10-ca-incarnation-model.md` | Incarnation TLA+ model plan | `06 §area-incarnation` |

### Writer-protocol specs / plans {#writer-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-18-ca-build-root-precommit-design.md` | Build-root reachability + fail-closed commit | `03 §write-path`, `§build-lifecycle`, `06 §cabuildRootPrecommit` |
| `specs/2026-06-18-ca-build-root-precommit-cpp-impl.md` | C++ impl of precommit-first | `03 §phase-precommit`, `§phase-promote` |
| `specs/2026-06-21-ca-precommit-first-design.md` | INV-2 precommit-first | `03 §renames` (INV-2), `§write-path` |
| `specs/2026-06-21-ca-adopt-evidence-defer-design.md` (B188) | Precommit-first: no pool op before precommit; adopt-evidence deferral | `03 §renames` (INV-2), `§build-lifecycle` |
| `specs/2026-06-23-ca-precommit-inline-closure-design.md` | Inline-closure on precommit Add (B199-S2) | `03 §fold-barrier`, `06 §cabuildRootPrecommit` |
| `specs/2026-06-16-ca-resurrect-reupload-design.md` | INV-1 revival-from-source; resurrect-by-GET rejected | `03 §phase-upload` (INV-1), `§status-rejected` |
| `specs/2026-06-21-ca-revival-consolidation-design.md` | Consolidated revival = uploadFromSource sole primitive | `03 §phase-upload`, `§status-rejected` |
| `specs/2026-06-16-ca-build-watermark-design.md` | `min_active` monotone-`build_seq` watermark; GC condemn guard | `03 §writer-identity`, `§build-lifecycle`, `06 §cabuildwatermark` |
| `specs/2026-06-14-ca-manifest-commit-lock-scope-design.md` | B151 lock-free publish via eager moveDirectory | `03 §mutable-vs-immutable` (rename/republishRef) |
| `specs/2026-06-16-ca-rootshard-protobuf-codec-design.md` | Root-shard protobuf codec | `05 §encoding-taxonomy`, `03 §writer-identity` |
| `specs/2026-06-26-cas-build-heartbeat-removal-design.md` | Per-build heartbeat removed; watermark replaces it | `03 §status-rejected` (per-build heartbeat), `07 §write-budget-watermark` |
| `plans/2026-06-26-cas-build-heartbeat-removal.md` | (same, plan) | `03 §status-rejected` |
| `specs/2026-06-05-ca-projection-dir-readback-design.md` | Read-your-writes overlay at directory granularity (projections) | `03 §write-path` (staging/overlay); minor detail folded |
| `specs/2026-06-06-ca-all-green-design.md` (B85/B87/B86) | 404→repair read fallback; moveFile rollback→moveDirectory | `02 §systematic-debugging` (H1–H5 same-class hunt); `03 §renames` |
| `specs/2026-06-26-cas-b92-adopt-tree-size-design.md` | Carry `tree_size` on adopt/relink wire | `03 §renames` (adopt), minor; `RM` |
| `specs/2026-06-26-cas-b194-striptree-reverse-index-design.md` | `GcSnap::stripTree` O(children) reverse index perf fix | `04 §snap-prune` / `RM §area-gc` (perf); minor |

### Milestone plans (M1–M9) — historical build sequence {#milestone-plans}

These are the original per-milestone build plans. Their durable protocol content is fully subsumed
by `03`/`04`/`05`; the milestone sequencing itself is ephemeral.

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `plans/2026-06-02-cas-mergetree-m1.md` | Initial CA disk skeleton | `01`, `03 §mount-startup` |
| `plans/2026-06-02-cas-mergetree-m2.md` | Blob write path | `03 §write-path` |
| `plans/2026-06-02-cas-mergetree-m3.md` | Tree/manifest | `01 §part-manifests`, `05 §tree-codec` |
| `plans/2026-06-02-cas-mergetree-m4.md` | Read path | `07 §read-budget` |
| `plans/2026-06-02-cas-mergetree-m5-hardening.md` | Hardening | `02 §systematic-debugging` |
| `plans/2026-06-02-cas-mergetree-m5s3-mutable-state.md` | Mutable state (txn/metadata_version) | `03 §mutable-vs-immutable` |
| `plans/2026-06-02-cas-mergetree-m5s4-formats.md` | Formats | `05` |
| `plans/2026-06-03-cas-mergetree-m6-dropin.md` | Drop-in disk semantics | `01 §what-cas-is` |
| `plans/2026-06-03-cas-mergetree-m7-mutations.md` | Mutation carry-forward | `01 §what-it-buys`, `03` |
| `plans/2026-06-03-cas-mergetree-m8-shared-pool.md` | Shared pool | `01 §shared-blobs-per-server-trees` |
| `plans/2026-06-03-cas-mergetree-m9w2-partition-clone.md` | Partition clone | `01 §what-it-buys` (clone free), `03 §renames` |
| `specs/2026-06-02-cas-mergetree-integration-design.md` | Integration design (superseded by v3 shared design) | `01`, `RM` |
| `plans/2026-06-11-ca-core-m-c1.md` | Core-M incarnation phase C1 | `04`, `06 §area-incarnation` |
| `plans/2026-06-11-ca-core-m-c2.md` | Core-M phase C2 | `04`, `06 §area-incarnation` |
| `plans/2026-06-11-ca-core-m-c3.md` | Core-M phase C3 (large GC plan) | `04` (round), `06 §area-incarnation` |
| `plans/2026-06-12-ca-core-m-w.md` | Core-M watermark phase | `03 §build-lifecycle`, `06 §area-watermark-resurrect` |

### Feature specs / plans (mutations, projections, freeze, replication, backup, txns) {#feature-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-03-cas-mergetree-mutations-design.md` + `plans/...m7-mutations` | Carry-forward on mutation (Wide only) | `01 §what-it-buys`, `§what-it-does-not-buy` |
| `specs/2026-06-03-cas-mergetree-projections-design.md` + `plans/2026-06-03-cas-mergetree-projections.md` + `plans/2026-06-03-cas-overnight-projections-minio-replicated.md` + `plans/2026-06-04-cas-mergetree-projection-readback.md` | Projection overlay/readback | `03 §write-path` (staging overlay); `RM §area-writer` |
| `specs/2026-06-03-cas-mergetree-shared-pool-design.md` | Shared pool / per-server trees | `01 §shared-blobs-per-server-trees` |
| `specs/2026-06-04-cas-mergetree-fetch-partition-design.md` + `plans/...fetch-partition` | Fetch partition = relink refs | `01 §what-it-buys`, `RM §area-writer` (relink) |
| `specs/2026-06-04-cas-mergetree-freeze-design.md` + `plans/...freeze` | FREEZE = shadow-tree ref pins | `04 §d1-design` (listNamespaces→LIST), `08 §scenario-table` S18; `RM §area-operability` (B3/B186 freeze gtest) |
| `specs/2026-06-04-cas-mergetree-multipart-transaction-design.md` + `plans/...multipart-transaction` | Multipart upload transaction | `03 §phase-upload` (multipart), `RM` |
| `specs/2026-06-04-cas-mergetree-replication-design.md` + `plans/...replication` | Replication fetch-by-relink; zero-byte cross-replica | `01 §what-it-buys`, `07 §read-budget-replication`, `RM §area-writer` (manifest_hash TODO) |
| `specs/2026-06-04-cas-mergetree-transactions-design.md` + `plans/...transactions` | Transaction (txn_version) handling | `03 §mutable-vs-immutable` |
| `specs/2026-06-05-cas-mergetree-backup-restore-design.md` + `plans/...backup-restore` | Backup/restore runbook | `RM §area-operability` (B198 backup/restore TODO) |

### GC specs / plans {#gc-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-04-ca-gc-convergence-design.md` | Foundational GC: fold/retire/fence/recheck; INV-NO-LOSS/DANGLE/RETURN; over-count bias | `04 §gc-round`, `§safety-invariants`, `01 §key-invariants` |
| `plans/2026-06-05-ca-gc-s1-reverse-index.md` | S1 reverse index | `04 §indegree-source-edge-set` |
| `plans/2026-06-05-ca-gc-s2-log-structured.md` | S2 log-structured journal | `04 §fold`, `01 §ref-shards` (journal) |
| `plans/2026-06-05-ca-gc-s3-generations-tombstones.md` | S3 generations + tombstones | `04 §attempt-scoped-generations`, `§d1-design` (tombstone) |
| `plans/2026-06-05-ca-gc-s4-lockless-handshake.md` | S4 lockless handshake | `04 §concurrent-leader-summary` (gc_lock REJECTED) |
| `plans/2026-06-05-ca-gc-s4-review-remediation.md` + `specs/2026-06-05-ca-gc-s4-review-remediation-design.md` | S4 review remediation (biggest GC plan) | `04 §gc-round`, `§concurrent-leader-summary`, `06 §area-incarnation` |
| `specs/2026-06-16-ca-gc-lease-heartbeat-design.md` + `plans/...gc-lease-heartbeat` | Lease + advisory heartbeat (B160); false-steal fix | `04 §leader-election`, `§advisory-heartbeat`, `06 §cagcleasecore` |
| `specs/2026-06-17-ca-gc-snap-prune-design.md` + `plans/...gc-snap-prune` | P9 node-forgetting; 46k HEAD storm | `04 §node-pruning`, `07 §gc-budget-retire` |
| `specs/2026-06-21-ca-gc-snap-generation-retention-design.md` + `plans/...gc-snap-generation-retention` | B174 generation retention; 82% pool storage | `04 §generation-retention` |
| `specs/2026-06-17-ca-b140-dangle-fix-design.md` | B140 dangle fix v1 | `04 §trim` (B140 HISTORY), `06 §area-b140` |
| `specs/2026-06-18-ca-b140-dangle-fix-v2-design.md` + `plans/2026-06-18-ca-b140-dangle-fix-v2.md` | B140 fix v2 (cursor-in-snap co-durable) | `04 §trim`, `06 §cab140danglemerge` |
| `specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md` | Root-local part-manifest GC redesign (rev.15); source-edge set; target-sharded reducers; lazy fence | `04` (whole doc), `06 §cagcrootlocalpartmanifestcore` |
| `specs/2026-06-28-cas-gc-attempt-scoped-generation-design.md` | Attempt-scoped generation (concurrent-leader safety) | `04 §attempt-scoped-generations`, `06 §cagcrootlocalpartmanifestcore` |
| `specs/2026-07-01-cas-gc-indegree-refold-undercount-design.md` | H1b integer-refcount underflow → source-edge set | `04 §indegree-source-edge-set`, `06 §cagcindegrefoldcore` |
| `specs/2026-06-17-ca-gc-introspection-design.md` + `plans/...gc-introspection` | GC introspection (dryrun/logs) | `08 §gc-dryrun`, `§gc-log` |
| `plans/2026-06-26-cas-gc-phase0-tla-model.md` | GC redesign phase-0 TLA+ | `06 §cagcrootlocalpartmanifestcore` |
| `plans/2026-06-26-cas-gc-phase1a-identity-and-codecs.md` | Phase 1a identity + codecs (RunFileReader CRC blocker) | `04`, `05 §encoding-taxonomy`, `02 §subagent-driven` |
| `plans/2026-06-26-cas-gc-phase1b-build-precommit-promote.md` | Phase 1b build/precommit/promote | `03 §write-path`, `04 §fold` |
| `plans/2026-06-26-cas-gc-phase1c-read-path.md` | Phase 1c read path | `07 §read-budget` |
| `plans/2026-06-26-cas-gc-phase1d-gc-fold-indegree-sweep.md` | Phase 1d fold/in-degree sweep (largest plan) | `04 §fold`, `§indegree-source-edge-set` |
| `plans/2026-06-26-cas-gc-phase2-token-diff-discovery.md` | Phase 2 token-diff discovery | `04 §discovery`, `07 §gc-budget-fold` (LIST-token skip) |
| `plans/2026-06-26-cas-gc-phase3-lazy-fence-trim.md` | Phase 3 lazy fence/trim | `04 §fence`, `06` (sab_lazyfenceunsafe) |
| `plans/2026-06-26-cas-gc-phase4-target-sharded-reducers.md` | Phase 4 target-sharded reducers | `04` , `06 §cagcrootlocalpartmanifestcore` |
| `plans/2026-06-26-cas-gc-phase5-retire-token-opt.md` | Phase 5 retire-token opt | `04 §retire`, `06` |
| `plans/2026-06-26-cas-gc-redesign-overview.md` | Redesign overview | `04 §overview` |
| `plans/2026-06-28-cas-gc-attempt-scoped-generation.md` | Attempt-scoped gen plan | `04 §attempt-scoped-generations` |
| `plans/2026-07-01-cas-gc-indegree-source-edge-set.md` | Source-edge-set plan | `04 §indegree-source-edge-set` |
| `plans/2026-06-29-cas-layout-phase0-mount-safety.md` | Layout phase 0 mount safety | `03 §mount-startup`, `06 §cacasmountcore` |
| `plans/2026-06-29-cas-layout-phase1-relocation.md` | Layout phase 1 relocation | `01 §pool-layout`, `05 §layout-keys` |
| `plans/2026-06-29-cas-layout-phase2-cursor-sweep.md` | Layout phase 2 cursor sweep | `04 §fold` (manifest sweep) |
| `plans/2026-06-29-cas-layout-phase3-manifest-identity.md` | Layout phase 3 manifest identity (ordinal) | `01 §part-manifests` (manifest_ordinal) |
| `specs/2026-06-30-cas-raw-audit-and-gc-rebuild-rfc.md` | RFC: raw audit + GC bookkeeping rebuild + repair commands | `08 §backlog`, `RM §area-operability` (fsck/repair); RFC status noted |

### Formats / envelope / codec specs & plans {#formats-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-07-ca-merkle-store-design.md` | Merkle store design (treeId rule; tree-layer REJECTED) | `05 §merkle-tree-id`, `§rejected-tree-layer`, `01 §rejected-merkle-tree-layer` |
| `specs/2026-06-08-ca-merkle-store-requirements.md` | Merkle store requirements | `05 §merkle-tree-id`, `§codecs-and-determinism` |
| `plans/2026-06-24-cas-2a-merkle-tree-id.md` | 2a merkle treeId (superseded tree-layer object) | `05 §rejected-tree-layer` |
| `plans/2026-06-24-cas-2b-envelope-one-header.md` | 2b one-header envelope (256-B, 94-B core) | `05 §envelope`, `§envelope-core` |
| `plans/2026-06-24-cas-2c-tree-catalog-layout.md` | 2c catalog-first/inline-last | `05 §tree-codec` |
| `plans/2026-06-24-cas-2d-part-writer-inlining.md` | 2d eager small-file inlining | `05 §tree-codec` (Placement::Inline) |
| `plans/2026-06-24-cas-3a-manifest-framing-published-at.md` | 3a manifest framing + published_at_ms | `05 §encoding-taxonomy`, `03 §phase-promote` (published_at_ms) |
| `plans/2026-06-24-cas-3c-json-objects-to-protobuf.md` + `plans/2026-06-25-cas-3c-tail-finish-json-abandonment.md` | JSON→protobuf; JSON codec deleted | `05 §object-kinds` (JSON abandoned), `§schema-evolution` |
| `plans/2026-06-24-cas-format-framework-foundation.md` | Format-framework foundation (writer/compat version) | `05 §encoding-taxonomy` |
| `plans/2026-06-25-cas-header-unification-rework.md` | Header unification (CHCA→CABL/CATR) | `05 §envelope-core` (prior format note) |
| `specs/2026-06-26-cas-proto-rename-design.md` | proto rename → cas_format.proto / clickhouse.cas.format | `05 §encoding-taxonomy` |
| `specs/2026-06-24-cas-schema-evolution-framework-design.md` | Schema-evolution stance; gen-1 freeze; write-down-to-floor; deferred rollout | `05 §schema-evolution`, `§gen1-freeze`, `§write-down-to-floor`, `§deferred-rollout` |
| `specs/2026-06-26-cas-b194-striptree-reverse-index-design.md` | (also formats-adjacent; see writer table) | `04 §snap-prune` |

### S3 op-count specs / reports {#s3-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-08-s3-ops-cost-model.md` | Pricing tiers; op-count cost model | `07 §write-budget` (price ref), `§references` |
| `specs/2026-06-14-ca-reduce-s3-op-count-design.md` + `plans/2026-06-14-ca-reduce-s3-op-count.md` | Incremental GC + resolveRef decode cache; LIST-token skip | `07 §gc-budget-fold` (LIST-token skip), `§reduction-history`, `04 §discovery` |
| `specs/2026-06-15-ca-head-after-put-etag-design.md` | PUT-response ETag capture (no post-write HEAD); 73% HEAD elim | `07 §reduction-history` (ETag fix), `05 §etag-vs-generation` |
| `specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` + `plans/...dedup-cache-head-before-put` | P1 dedup cache + P2 adaptive HEAD-before-PUT | `07 §reduction-history`, `03 §phase-upload` (dedup) |
| `specs/2026-06-15-ca-root-shards-widen-design.md` | root_shards 8→64 widen; contention reduction | `07 §reduction-history` (root_shards widen) |
| `reports/2026-06-15-ca-soak-opcount-and-rustfs-findings.md` | Instrumented soak A1b op attribution | `07 §measured-vs-modeled`, `§references` |
| `reports/2026-06-15-unattended-night-opcount-fixes.md` | #1 ETag + #4 root_shards + soak #6/#7 | `07 §reduction-history`, `§references` |
| `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` | P0–P9 proposals; corrected cost table (~$571/day, 57% waste) | `07 §reduction-history`, `§cost-summary`, `§references` |

### Soak / fsck / vfs / introspection {#soak-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-13-ca-soak-test-design.md` + `plans/2026-06-13-ca-soak-test.md` | Soak topology, workload/oracle, quiescence, chaos, checkpoint assertions | `08 §soak-harness` (all subsections) |
| `specs/2026-06-13-ca-fsck-readonly-design.md` + `plans/2026-06-13-ca-fsck-readonly.md` | Read-only mode; fsck reachability classify; dedup_ratio | `08 §read-only-mode`, `§fsck` |
| `specs/2026-06-15-ca-fsck-timeout-progress-design.md` | fsck per-LIST timeout + progress; large-pool O(N²) | `08 §soak-limitations` (large-pool fsck timeout) |
| `specs/2026-06-19-ca-vfs-contract.md` | VFS contract (which files CA vs verbatim; path rules) | `05 §layout-keys`, `03 §mutable-vs-immutable` |
| `specs/2026-06-19-ca-vfs-path-mapping-design.md` + `plans/2026-06-19-ca-vfs-path-mapping.md` | VFS path mapping (store/<uuid> → pool namespaces) | `05 §layout-keys`, `03 §server-root-id`, `§mutable-vs-immutable` |
| `specs/2026-06-15-ca-rustfs-overwrite-leak-mitigation-design.md` | RustFS overwrite-leak; orphan-reaper workaround | `05 §rustfs` |
| `specs/2026-06-18-ca-event-log-design.md` + `plans/2026-06-18-ca-event-log.md` | `system.content_addressed_log` (B170) schema + event taxonomy | `08 §event-log` |

### Reports (incident forensics) {#reports}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `reports/2026-06-06-b90-unattended-followup.md` | B90 crash triage | `02 §systematic-debugging`; incident narration dropped |
| `reports/2026-06-06-threadgroup-uaf-dedup-log-s3.md` | ThreadGroup UAF (dedup-log + DROP) root cause + fix | `02 §systematic-debugging`; upstream-relevant bug, fix already in code |
| `reports/2026-06-12-readbufferfromfileview-position-corruption.md` | ReadBufferFromFileView position corruption (B115) fix | `02 §systematic-debugging`; fix already in code (commit `440871098a9`) |
| `reports/2026-06-13-unattended-progress.md` | Unattended progress narrative | `02`, `RM`; narration dropped |
| `reports/2026-06-07-ca-spec-review-milovidov.md` + `-v2.md` | Simplicity review: D6 cut; O(1) reconcile; Keeper cache rule | `02 §design-review`, `§tla-incarnation` |
| `reports/2026-06-07-ca-spec-review-distributed.md` + `-v2.md` | Distributed review: in-degree-alone over-stated → source-edge; intent-key collision | `02 §design-review`, `§tla-source-edge` |
| `reports/2026-06-18-ca-b140-dangle-soak-recurrence.md` | B140 dangle soak recurrence | `04 §trim`, `02 §soak-oracle` (B140) |
| `reports/2026-06-18-ca-b140-dangle-trigger-pinned.md` | B140 trigger pinned via event log | `02 §soak-oracle`, `04 §trim` |

### Simplification / adopt / cleanup misc {#misc-specs}

| Old doc | Durable content captured | New target |
|---------|--------------------------|------------|
| `specs/2026-06-23-cas-cleanup-simplification-design.md` + `plans/2026-06-23-cas-cleanup-simplification.md` | Cleanup/simplification pass | `02 §subagent-driven` (simplification passes) |
| `specs/2026-06-16-ca-resurrect-reupload-design.md` | (writer table) | `03 §phase-upload` |
| `specs/2026-06-26-cas-b92-adopt-tree-size-design.md` | (writer table) | `03 §renames` |

### TLA+ model prose (superseded by `06-tla-models.md`) {#model-prose}

All `models/*_RESULTS.md`, `*_README.md`, `INDEX.md`, `README.md`, `RESULTS.md`,
`MODEL_CURRENCY_REVIEW*` are superseded by `06-tla-models.md`, which indexes every model's
invariants, sabotages/counterexamples, state counts, and code-currency.

| Old doc | New target |
|---------|-----------|
| `models/CaB140DangleMerge_RESULTS.md` | `06 §cab140danglemerge`, `§cab140danglefaithful` |
| `models/CaB140Dangle_RESULTS.md` | `06 §cab140dangle` |
| `models/CaBuildRootPrecommit_RESULTS.md` | `06 §cabuildRootPrecommit` |
| `models/CaBuildWatermark_RESULTS.md` | `06 §cabuildwatermark`, `§cabuildwatermarknum` |
| `models/CaCasMountCore_RESULTS.md` | `06 §cacasmountcore` |
| `models/CaGcLeaseCore_RESULTS.md` | `06 §cagcleasecore` |
| `models/CaGcRootLocalPartManifestCore_RESULTS.md` | `06 §cagcrootlocalpartmanifestcore` |
| `models/CaIncarnationCore_RESULTS.md` | `06 §caincarnationcore` |
| `models/CaIncarnationCore_README.md` | `06 §caincarnationcore`, `§caincarnationproofcore` |
| `models/CaResurrectLiveness_RESULTS.md` | `06 §caresurrectliveness` |
| `models/INDEX.md` | `06` (whole index) |
| `models/README.md` | `06 §running-models`, `§cagccore` |
| `models/RESULTS.md` | `06 §cagccore` (EBR core) |
| `models/MODEL_CURRENCY_REVIEW_2026-06-22.md` | `06` (per-model code-currency notes), `02 §design-review` |

### Backlogs {#backlogs}

| Old doc | New target |
|---------|-----------|
| `deferred_backlog/cas-mergetree-integration.md` | `RM` (whole roadmap is the structured view; RM still links this file as the "living backlog") |
| `deferred_backlog/cas-mergetree-integration-archive.md` | `RM §deferred-backlog-summary`; archived items folded |
| `deferred_backlog/cas-gc-redesign-backlog.md` | `RM §area-gc`, `04` findings |

> NOTE: `ROADMAP.md` currently references
> `deferred_backlog/cas-mergetree-integration.md` as the "living backlog" in two places
> (`§intro` and `§deferred-backlog-summary`). If that file is deleted, those references dangle.
> Resolve before deletion: either keep `cas-mergetree-integration.md` as the living backlog, or
> fold its still-actionable items fully into `ROADMAP.md` and drop the references. See the audit
> findings section.

---

## Audit result {#audit-result}

**Verdict: GAPS — deletion is BLOCKED for a subset of docs until the target new docs are amended.**

A fresh sanity-audit (four parallel auditors, each reading the largest/most-important old docs in
full against the mapped new sections) found the great majority of load-bearing content is present.
The following REAL losses of load-bearing content block the listed old doc(s) from deletion until
the named target new doc carries the missing item. (Stylistic differences, dropped narrative, and
superseded-with-reason mechanisms are NOT gaps and are excluded.)

### Blocking gaps — must be filled before deleting the source doc {#blocking-gaps}

**Group A — VFS path-mapping (largest gap; content is LIVE in code, not superseded):**
The CH-path → CAS-namespace mapping rules survive only in `PartPathParser.{h,cpp}` /
`ContentAddressedMetadataStorage.cpp` code comments, not in any new doc. Blocks
`specs/2026-06-19-ca-vfs-contract.md`, `specs/2026-06-19-ca-vfs-path-mapping-design.md`,
`plans/2026-06-19-ca-vfs-path-mapping.md`. Missing items → target `01`/`03`/`05`:
- The `@cas@` archive-suffix marker semantics (suffix-on-table-dir, never a standalone segment;
  `@` is S3-safe and never occurs in uuids/part-names/detached/projection/column names); the
  mutability invariant "a node is immutable **iff** it is content-addressed; `@cas@` is exactly the
  content/verbatim boundary".
- The Atomic-vs-non-Atomic mirroring rule (`store/<u3>/<uuid>@cas@` vs `data/<db>/<tbl>@cas@`).
- The `@cas@`-scoped shard-classification invariant (a numeric-tailed loose file with no `@cas@`
  ancestor is an opaque ordinary file, never mis-parsed as a shard).
- The non-nesting / sibling-detached namespace invariant (no namespace may be a path-prefix of
  another, because GC enumerates by prefix-LIST — detached parts must be a sibling namespace).
- The two verbatim-file locations (loose in the mountpoint `roots/<server>/<path>`, never GC-scanned;
  and `_files/` inside a `@cas@` archive) and the eliminated `_disk` magic namespace.
- The logical-vs-physical view contract (`clickhouse-disks` presents the `@cas@`-stripped logical
  view; raw `aws s3 ls` shows the physical archive; raw subtree `rm` is destructive maintenance, NOT
  `dropNamespace`).

**Group B — CaIncarnationCore model-derived spec findings (MR/F rules):** block
`models/CaIncarnationCore_RESULTS.md` → target `06` (and `03`):
- `MR-1`: the publish gate must consult the durable deleted-token history `deadTok[h]`, not only the
  in-flight `retired` set.
- `MR-2` / `F1`: any action that makes a token stop being current (delete-land / `WResurrect` /
  `WOverwrite`) must push the displaced token into `deadTok[h]`; the publish gate must re-validate a
  dependency's CURRENT physical state, not just the originally-observed token.
- `MR-3` / `F2` (`TreeDepsOK`): a tree ref may be published only when all direct children are present
  and non-condemned at publish time (bottom-up build discipline).

**Group C — GC safety rules absent from `04`:** block
`plans/2026-06-11-ca-core-m-c3.md` and `specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md`
→ target `04 §3.4/§3.6`:
- Retire fail-closed: a candidate absent at retire (HEAD 404) → skip, **never fabricate a token**.
- Recheck 404 policy: a missing/invalid committed-or-promoted new-binding manifest body in the fence
  window **clamps/aborts the affected delete (fail-closed, not spare-by-default)** and surfaces to
  `fsck`; an old-binding removal uses the blob edges already sealed at fold; recheck must never read a
  deleted manifest body.
- `created_delete_marker → LOGICAL_ERROR` per-delete versioning guard (partially mitigated by the
  startup probe in `01 §backend-contract`).
- `ViewableRound` operational statement: `gc/state.round` advances only after **every** blob-target
  shard's retired set and part-manifest cleanup bundle are durable (subset-safety for sharded mode).
  The invariant name is in `06`; its operational rule is missing from `04`.

**Group D — architecture / integration decisions (only carrier is the v3 design + incarnation docs):**
block `specs/content_addressed_shared_mergetree_design.md` and `incarnation-tagged-cas.md`
→ target `01` (+ `04`/`05`):
- DROP primitive: how a SQL DROP PART/PARTITION supersedes parts in the CAS model (covering tombstone
  / promote-empty-tombstone-part); distinct from the D1 shard-object tombstone.
- DROP-PART mid-range race reconciliation with a concurrent merge (the documented weak guarantee).
- FREEZE MUST materialize real bytes into `shadow/` (cannot be reference-only) and why.
- Patch-part / lightweight-delete reachability: a patch manifest is a first-class reachability root
  keeping both its delta blobs and the referenced base blobs alive → `04`.
- Benign cross-replica divergence rationale (single-producer is an optimization, not correctness;
  tolerant commit-time `checkEqual`) — governs the `manifest_hash`-on-znode TODO.
- Stateless/ref-less reader GC fence (ephemeral reader pin + lost-replica-timeout) — OR an explicit
  "superseded by the per-server-owned-namespace model" note if that is the intent.
- (`incarnation-tagged-cas.md`) the "incarnation in body vs provider metadata" decision + its
  S3-grounded rationale (metadata write-once/needs-copy, 8 KB cap) → `05`; and the writer-side
  logical-hash-collision → quarantine/fail-closed rule → `01`.

**Group E — writer/backend + incremental-GC details:**
- `specs/2026-06-21-ca-revival-consolidation-design.md`: `ObjectStorageBackend::get` NoSuchKey/404 →
  `std::nullopt` contract (legitimate live read in a HEAD→GET delete window surfaces cleanly, not raw
  Code 499) → `03`/`05`.
- `plans/2026-06-14-ca-reduce-s3-op-count.md`: the resident-snap incremental-GC checkpoint design
  (`gc_checkpoint_records=4096`, `gc_checkpoint_rounds=64`) and the durable-vs-resident cursor trim
  invariant (`trim` only at/below the **durable** cursor) → `04`; `shard_decode_cache_ttl_ms=200`
  default + "absence is never TTL-cached" → `07 §2.1`.

### Minor / optional (not blocking) {#minor-gaps}

- S3 per-prefix req/s scaling ceilings (5,500 read / 3,500 write per prefix; motivates root-shard
  fanout) — `07`.
- The `sab_staletokenoverdelete` oracle *reasoning* (conclusion kept in `06`, rationale dropped).
- The quantified "~90% steady-state op reduction" projection (measured tables in `07` supersede it).
- The definitive "all HEADs funnel through `tryGetObjectMetadata`, `exists()` never called" forensic
  attribution — `07 §5`.
- Packing/pack-object recorded as deferred future work with the "refcount never materialized, not on
  critical path" rationale — `01` (currently only "`pack_slice` reserved").

### Safe to delete now (audited, COVERED) {#covered}

`specs/2026-06-10-ca-incarnation-store-design.md`, `specs/2026-06-11-ca-core-refactor-design.md`,
`specs/2026-06-28-cas-layout-hot-cold-split-design.md`, `content_addressed_mergetree.md`, the
build-root/precommit/inline-closure/watermark/manifest-commit-lock writer specs, the GC
lease-heartbeat / snap-prune / generation-retention / attempt-scoped-generation / b140-v2 specs, the
soak/fsck specs, the rustfs-leak spec, the s3-cost-model / dedup-cache / opcount-proposals reports,
and all `models/*_RESULTS.md` / `*_README.md` / `INDEX.md` / `README.md` / `RESULTS.md` /
`MODEL_CURRENCY_REVIEW*` (superseded by `06`) — subject to the Group B/C/E items above being folded
in first where they name those source docs.

The controller should amend the target new docs for Groups A–E, then re-run this gate (or spot-check
the amended sections), before `git rm` of the affected sources.

---

## Gap-fill (2026-07-02) {#gap-fill}

All blocking gaps (Groups A–E) and the `ROADMAP.md` backlog-reference item were filled with additive,
source- and code-grounded edits. Per group:

**Group A — VFS path-mapping.**
- `05-formats-and-backend.md` — new `§path-mapping`: `@cas@` suffix semantics + mutability boundary,
  Atomic-vs-non-Atomic mirroring table (`store/<u3>/<uuid>@cas@` vs `data/<db>/<tbl>@cas@`),
  `@cas@`-scoped shard classification, non-nesting invariant, reserved segments, logical-vs-physical
  view + raw-`rm`-is-maintenance.
- `03-writer-protocol.md` — new `§verbatim-files`: the two verbatim locations (mountpoint
  `roots/<server>/<path>`, `@cas@/_files/`) + eliminated `_disk` namespace.
- `01-architecture.md` — new `§namespace-mirroring` in pool layout: the `@cas@` boundary + mirroring
  summary, cross-linking `05`/`03`.
- NOTE (grounded against `PartPathParser.{h,cpp}`, `CasLayout.h`, `ContentAddressedMetadataStorage.cpp`):
  the spec's sibling-detached-namespace proposal is **superseded in code by B181** (detached parts
  fold into the table namespace as `detached/`-prefixed refs). The docs describe the live B181
  mechanism and note it preserves the non-nesting property via ref-name prefixing.

**Group B — CaIncarnationCore MR/F.**
- `06-tla-models.md §caincarnationcore` — added MR-1 (`deadTok[h]` in the gate), MR-2/F1
  (displaced-token push + current-state re-validation), MR-3/F2 (`TreeDepsOK` bottom-up) to the
  design-decisions block with literal model names.
- `03-writer-protocol.md` — new `§publish-gate` under Phase 4 promote restating the three obligations
  in protocol terms.

**Group C — GC safety rules.**
- `04-gc-protocol.md §3.4` — retire fail-closed (never fabricate a token on 404).
- `04-gc-protocol.md §3.6` — recheck 404 policy (committed/promoted new-binding clamps the delete,
  precommit non-activating, old-binding uses fold-sealed edges, never read a deleted body) +
  `created_delete_marker → LOGICAL_ERROR` guard.
- `04-gc-protocol.md §5` (attempt-scoped) — `ViewableRound` round-advance operational rule
  (subset-safety for sharded mode).

**Group D — architecture/integration decisions.**
- `01-architecture.md` — new `§integration-decisions`: DROP-supersession (empty covering tombstone
  ref; distinct from D1 shard tombstone), DROP-PART/merge race weak guarantee, FREEZE materializes
  real bytes into `shadow/`, benign cross-replica divergence (tolerant `checkEqual`; governs
  `manifest_hash` TODO), logical-hash-collision → quarantine.
- `04-gc-protocol.md §6.4` — patch-part first-class reachability root (transitive base-blob
  reachability) + stateless/ref-less reader GC fence (ephemeral pin + lost-replica timeout).
- `05-formats-and-backend.md §incarnation-in-body` — incarnation-in-body-vs-metadata S3-grounded
  rationale (metadata write-once/needs-copy, 8 KB cap, ETag = content not metadata).

**Group E — writer/backend + incremental GC.**
- `05-formats-and-backend.md §get-nullopt` — `get` NoSuchKey/404 → `std::nullopt` read-window
  contract (also referenced from `03` via INV-1).
- `04-gc-protocol.md §3.9` — resident-snap incremental-GC checkpoint (`gc_checkpoint_records=4096`,
  `gc_checkpoint_rounds=64`) + durable-vs-resident cursor trim invariant.
- `07-s3-budget.md §2.1` — `shard_decode_cache_ttl_ms=200` default + "absence is never TTL-cached".
- NOTE: Group E's LIST-token-skip item is not in the named source docs; it is already documented in
  `07 §reduction-history` and `04 §discovery`, so no gap.

**ROADMAP.md backlog references.** The two "living backlog" references to
`deferred_backlog/cas-mergetree-integration.md` (`§intro`, `§deferred-backlog-summary`) and the
in-table mention (§area-gc) were replaced with the statement that the roadmap **is** the living
backlog (the file's content is folded in); B168 remainder now points to `07 §reduction-history`.

**Still-open / caveats (not invented):**
- Group A sibling-detached vs B181 folding divergence documented as live-in-code (above).
- Group D patch-part reachability is flagged in the v3 design's residual-risk list as not yet fully
  model-checked; the doc carries that caveat.
- Group D stateless-reader fence: the doc notes the per-server-owned-namespace model narrows but does
  not eliminate the window; the ephemeral-pin mechanism remains the documented answer.
