---
description: 'Entry point for the CAS (content-addressed) MergeTree documentation set: what this folder contains, a reading guide for docs 01–08, and a status dashboard across all major feature areas.'
sidebar_label: 'CAS docs entry point'
sidebar_position: 0
slug: /superpowers/cas/readme
title: 'CAS MergeTree — Documentation Index'
doc_type: 'guide'
---

# CAS MergeTree — Documentation Index {#cas-readme}

This folder holds the canonical, structured documentation for the **content-addressed (CAS)
MergeTree** feature of ClickHouse. It replaces the previous collection of ~150 dated spec, plan,
worklog, and report files in `docs/superpowers/` with nine focused, topic-structured documents
and two navigation aids (this file and `ROADMAP.md`).

**What CAS is in one sentence:** a new `metadata_type = content_addressed` disk back-end for
`MergeTree` and `ReplicatedMergeTree` that stores every part file once by content hash, enabling
multiple replicas to share a single object-storage pool without byte duplication and without
zero-copy replication.

---

## Reading guide {#reading-guide}

| Doc | What it covers |
|-----|----------------|
| [`01-architecture.md`](01-architecture.md) | The object model (blobs, refs, part-manifests, incarnation tokens), the pool layout (hot/cold split, `blobs/`, `cas/refs/`, `cas/manifests/`, `roots/`, `gc/`), and every major approach tested and **rejected** with reasons (Merkle tree layer, EBR GC core, integer refcount, namespace registry, zero-copy replication). |
| [`02-methodology.md`](02-methodology.md) | The disciplined method that drove decisions: TDD, subagent-driven development, TLA+ as a pre-implementation gate, the scenario/soak suite as an empirical oracle, and systematic debugging. Why each big architectural pivot happened. |
| [`03-writer-protocol.md`](03-writer-protocol.md) | The write path in full: mount startup and `server_root_id` owner claim; durable-monotone `writer_epoch` / `build_sequence`; build → precommit → upload → promote; mutable vs immutable files; renames; the fold barrier; memory and scratch requirements. |
| [`04-gc-protocol.md`](04-gc-protocol.md) | GC leader election and lease; the round (fold → retire → fence → recheck → trim/reclaim); orphan removal via the source-edge in-degree set; ref removal; shard-object reclaim and incarnation; attempt-scoped generations; snap prune; concurrent-leader safety. TLA+ references. |
| [`05-formats-and-backend.md`](05-formats-and-backend.md) | Object kinds, the one-header envelope format, codecs, deterministic-artifact upload (`putDeterministicArtifact`), layout keys, the `Cas::Backend` abstraction, exact-token deletes, the rustfs testbed, and AWS/GCS/Azure support status. |
| [`06-tla-models.md`](06-tla-models.md) | Index of every TLA+ model that survives: invariants proved, counterexamples that drove design changes, and code-currency notes. Points to `.tla`/`.cfg` sources (which remain untouched). |
| [`07-s3-budget.md`](07-s3-budget.md) | The consolidated, detailed S3 op-count breakdown per protocol part (write, read, GC) plus the full reduction history (dedup cache, adaptive HEAD-before-PUT, precommit-first, snap-prune, LIST-token skip). |
| [`08-testing-and-soak.md`](08-testing-and-soak.md) | The adversarial scenario suite (S01–S35: what each stresses, D2 triage status), the 24h soak harness, `clickhouse-disks ca-fsck` and `ca-gc-dryrun` introspection, the `system.content_addressed_log` and `system.content_addressed_garbage_collection_log` audit tables, and standing findings/backlog. |
| [`09-read-protocol.md`](09-read-protocol.md) | End-to-end read protocol: ref resolution (`resolveRef`), manifest fetch (`readManifest`), ranged blob reads, column pruning, shard and `(ManifestId, Token)` decode caches, `ReadBufferFromFileView`/`PackedFilesReader` and the B115 position-corruption fix, in-flight read-your-writes (B59, projections), mutable and verbatim file reads, and GC safety. |
| [`10-backups.md`](10-backups.md) | Backup and disaster recovery: why the cloud versioning stack is unavailable (and what versioned mode would re-enable), the threat model, a comparative survey of every backup option with RPO/RTO, and the chosen snapshot / mirror / fetch / restore design (in-pool snapshot objects, the pull daemon, selective fetch + relink `RESTORE`, the `BAK-RO` invariant). |
| [`how-we-got-here.md`](how-we-got-here.md) | The chronological trial-and-error history: every architectural turn (`2026-06-01 → 2026-07-22`), the counterexample / review / soak finding that forced it, and the approach it replaced. The narrative companion to `01`/`02`/`06`. |
| [`ROADMAP.md`](ROADMAP.md) | Consolidated cross-area DONE / TODO / REJECTED / DESIRABLE roll-up. Single place to see the whole feature state. |

---

## Status dashboard {#status-dashboard}

| Area | Status | Canonical doc |
|------|--------|---------------|
| Architecture and object model | **DONE** — pool layout, hot/cold split (D0), format freeze | [`01-architecture.md`](01-architecture.md) |
| Methodology (TDD, TLA+, soak-driven) | **DONE** — established and applied throughout | [`02-methodology.md`](02-methodology.md) |
| Writer protocol (build → precommit → promote) | **DONE** — including precommit-first, manifest soft/hard limits (B164b), streaming `putBlob` | [`03-writer-protocol.md`](03-writer-protocol.md) |
| GC protocol (fold/retire/fence/recheck) | **DONE (core)** — attempt-scoped generations, snap prune, D1 shard-incarnation; remaining: run-file streaming, common-shard-prefix discovery | [`04-gc-protocol.md`](04-gc-protocol.md) |
| Replication (fetch-by-relink) | **DONE** — multi-replica shared pool works; manifest id travels in-band (interserver handshake); the `manifest_hash` Keeper-header field (B1) was REJECTED — replication stays disk-agnostic | [`03-writer-protocol.md`](03-writer-protocol.md) |
| Formats and backend abstraction | **DONE** — one-header envelope, protobuf codecs, schema-evolution stance, `putDeterministicArtifact` | [`05-formats-and-backend.md`](05-formats-and-backend.md) |
| TLA+ models | **DONE** — multiple models; GC safety + liveness + attempt-scoped generation proved | [`06-tla-models.md`](06-tla-models.md) |
| S3 op-count reduction | **Partial** — dedup cache, adaptive HEAD, precommit-first, snap-prune done; HEAD-storm (B148), B168 program ongoing | [`07-s3-budget.md`](07-s3-budget.md) |
| Testing — scenario suite (S01–S35) | **Partial** — 2026-07-03 re-triage: 8 PASS, ZERO real fails (all old D2 FAILs resolved/superseded/card bugs); remaining inconclusives are ci/full scale gates + infra gates (S12/S22/S27) | [`08-testing-and-soak.md`](08-testing-and-soak.md) |
| Testing — soak harness | **Partial** — green-path soak works; chaos soak limited by fsck timeout at large pool and TTL-band oracle; 4h continuous run needs compacting object store | [`08-testing-and-soak.md`](08-testing-and-soak.md) |
| Introspection (`ca-fsck`, `ca-gc-dryrun`, audit logs) | **DONE** — `clickhouse-disks ca-fsck`, `ca-gc-dryrun`, `system.content_addressed_log`, `system.content_addressed_garbage_collection_log` all implemented | [`08-testing-and-soak.md`](08-testing-and-soak.md) |
| Read protocol (ref resolution, manifest fetch, ranged blobs, decode caches, read-your-writes) | **DONE** — B59 in-flight overlay, B115 position-corruption fix, shard and manifest decode caches operational | [`09-read-protocol.md`](09-read-protocol.md) |
| Backups and disaster recovery | **DESIGN** — option survey + chosen snapshot/mirror/fetch/restore direction written 2026-07-14; implementation not started (feeds B198) | [`10-backups.md`](10-backups.md) |
| Release readiness | **TODO** — capability gate (B31), real-S3 GC validation, backup/restore runbook, `SYSTEM` control commands (B197), integration tests on RustFS (B125), repo hygiene (B131) | [`ROADMAP.md`](ROADMAP.md) |

---

## TLA+ model sources {#tla-models}

All TLA+ model sources live in `docs/superpowers/models/` and are **not rewritten** by this
consolidation. The new doc `06-tla-models.md` provides a navigable index and replaces the
per-model `*_RESULTS.md` and `*_README.md` prose files.

## Active in-flight work {#active-work}

The D1 shard-incarnation and registry-removal work is fully landed (2026-07-02/03: `gc/registry`
deleted, `ShardIncarnation` stamping in `CasRootShardCodec.h`, LIST-based `discoverUniverse`,
regression guards S30/S34/S35 PASS). Its spec
(`specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`) and plan
(`plans/2026-07-01-cas-shard-incarnation-and-registry-removal.md`) are kept as the historical record.
