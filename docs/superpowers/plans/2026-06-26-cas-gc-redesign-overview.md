---
description: "Index and shared-constraints anchor for the CAS GC root-local part-manifest redesign (phases 0-5)."
sidebar_label: "GC redesign — overview"
sidebar_position: 1
slug: /superpowers/plans/2026-06-26-cas-gc-redesign-overview
title: "CAS GC Root-Local Part Manifest Redesign — Master Plan & Index"
doc_type: reference
---

# CAS GC Root-Local Part Manifest Redesign — Master Plan & Index {#cas-gc-root-local-part-manifest-redesign-master-plan-index}

> **⚠️ IMPLEMENTATION STATUS / SOURCE OF TRUTH (2026-06-27, B15).** These plans were **executed** on
> branch `cas-gc-part-manifest-impl` (phases 0–4 code complete; phase-5 model gate done, its retire-token
> code intentionally not implemented — see B14). During implementation the design converged and some
> **task bodies below still contain pre-rev.15 protocol snippets that were superseded** (e.g. phase-1b
> Task 5 describes missing-body promotion folding "as committed" and appending old
> `PrecommitTransition`/`PromotePrecommit` vectors — the shipped protocol uses a **single ordered
> `RootOwnerEvent` journal** + a **pure owner-move promote** + the **missing-body fold barrier**, and
> `promote`/`abandon` were hardened post-review to match the model). **The authoritative record of what was
> actually built is the committed code, the TLA+ ledger `…/models/CaGcRootLocalPartManifestCore_RESULTS.md`,
> the execution log `…/cas-gc-unattended-execution-log.md`, and the backlog.** Read these plan bodies as the
> original intent, not as a description of the final code. (A full snippet-by-snippet rewrite was judged
> not worth the regression risk for executed-and-superseded plans.)

> **For agentic workers:** This is the **index and shared-constraints anchor** for a multi-plan redesign. The bite-sized, checkbox (`- [ ]`) tasks live in the **per-phase plan files** listed under [Phase Index](#phase-index). REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to execute each phase plan task-by-task. Read this overview first; every phase plan repeats the [Global Constraints](#global-constraints) so an implementer who only sees one task still has them.

**Goal:** Replace CA's content-addressed *tree* model and resident-snapshot GC with **root-local immutable part manifests** plus a **streaming, target-shardable blob-in-degree GC**, with each behavior-changing phase gated on a green TLA+ model extension.

**Architecture:** Only **blobs** stay content-addressed. Each part becomes one immutable, single-owner, namespace-qualified **part manifest** whose body lists every path, inline payload, and blob reference. `ManifestId = (root_namespace_id, ManifestRef)` is the **protocol identity** (it keys edges, cleanup, and addressing); the distinct `ManifestSafetyId = (root_namespace_id, manifest_instance_id)` is a **TLA+-abstraction-only** term (see Phase 0). The root journal carries **one ordered `RootOwnerEvent` stream** (each event pairs an `old_binding`?/`new_binding`?, an `OwnerKind` of `Committed` or `Precommit`), not transitive closures. GC folds owner events into **write-once blob in-degree generations** sealed by two write-once phase seals, the `CasFoldSeal` and the `CasCompletionSeal`; there is no cascade, no tree expansion, no `children_by_tree`, no resident `GcSnap`. Default `gc_shards = 1`; an optional target-sharded reducer mode lets two replicas GC disjoint blob-hash shards concurrently.

**Tech stack:** C++ (ClickHouse coding standards, Allman braces); Protobuf for the control plane; dense block-framed sorted binary runs (`RunFile`/`DataBlock`/`RunFooter`) for the hot data plane; TLA+ (TLC) for safety gates; gtest for unit oracles; `ci.praktika` for integration and chaos-soak.

**Source spec:** `docs/superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md` (rev. 15). All section references below are to that spec.

---

## Global Constraints {#global-constraints}

*Every task in every phase plan implicitly includes this section. Phase plans repeat it verbatim.*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in a phase may begin until that phase's TLA+ gate is green (see [Execution Model & Gates](#execution-model--gates)).
- **R1 — bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 — target-shardable.** Default `gc_shards = 1`; sharded mode is optional (Phase 4).
- **R3 — simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, trimmed.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden (per `feedback_ca_no_compat_scaffolding_predev`).

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source — **never** `GET`s the condemned object (per `feedback_ca_resurrect_invariant`);
- GC must never **wedge** (throw out of the round) on a 404 during fold — record the anomaly and continue the round (per `feedback_ca_gc_never_throw_on_404`). This is **not** a license to treat a missing body as empty: a **committed or promoted** owner that resolves to a missing/invalid manifest body is **fail-closed for that object's fold/delete decision** (no decrement is guessed, no delete is issued, the condition is surfaced to fsck) and the fold cursor must **not** advance past an unresolved committed removal. Only a **precommit** owner may legally name a missing body (it contributes no blob edges and cannot promote).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into a `build_*` directory (e.g. `build`, `build_debug`, `build_asan`). Always redirect ninja output to `<build_dir>/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>[.sh]`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'` (exact target/filter confirmed per phase plan from the C++ ground-truth report).

**TLA+ run mechanics** (exact; from `docs/superpowers/models/`)
- Run one config:
  ```bash
  cd docs/superpowers/models
  java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config <Cfg>.cfg <Module>.tla
  ```
  (For a long run set `TLC_JAVA_OPTS=-Xmx48g`.) Add a `run_<module>.sh` wrapper following the existing `run_tlc.sh` pattern (that wrapper hardcodes `CaIncarnationCore.tla`, so a new module needs its own wrapper or a direct `for cfg in …` loop).
- **PASS** = exit 0 and the log contains `Model checking completed. No error has been found.`
- A **negative-control / `_sab_*` / `_buggy`** config is correct **only when it FAILS** with `Error: Invariant <NAME> is violated.` (or `Temporal properties were violated.`). A zero exit on a `_sab_*` config is a **suite failure** (`UNEXPECTED PASS`).
- Convention: `<Module>_stageN.cfg` / `_fixed` / `_safe` must HOLD; `<Module>_sab_<rule>.cfg` / `_buggy` must produce the expected counterexample; liveness configs use `SPECIFICATION FairSpec` + `PROPERTY` + `CHECK_DEADLOCK FALSE`; non-vacuity uses a negated `W_*` reachability probe that must report "violated".

---

## Resolved Open Questions {#resolved-open-questions}

The spec's §Open Questions are pinned here with fail-safe defaults so the unattended worker never stalls. Each phase plan restates the ones it depends on. **These are vetoable** — changing one is a plan edit, not a code rewrite.

1. **PartManifestProto debug fields (OQ1).** Body carries only: `header{magic="CAPT", format_version, writer_version}`, `ref` (`ManifestRef`), `root_namespace_id`, `payload_digest` (a `CityHash128` digest of the canonical body — the CAS content-hash primitive, integrity/debug only — never a key, never dedup, never in-degree), and `entries` (+ optional `directory_index`). The manifest magic is `"CAPT"`, not `"CAPM"` — `"CAPM"` is already `FormatId::PoolMeta`'s magic (see Phase 1a's Magic Collision note). No additional debug fields in Phase 1.
2. **`_manifests` placement (OQ2).** Directly under the root-namespace prefix: `roots/<root_namespace>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`. **Not** under a `root_shard` subprefix. The orphan sweep is scoped per namespace + per build prefix.
3. **Internal manifest indexing (OQ3).** Phase 1 stores `entries` in canonical path order as block-framed `RunFile` `DataBlock`s with a sparse footer index (per-block `min_key`/`max_key`). The optional `DirectoryIndex` is **deferred and off by default**; add it only when `listDirectory` profiles demand it.
4. **Promotion blob-summary (OQ4).** **No** compact blob summary inside the manifest in Phase 1. `PromotePrecommit` revalidates by streaming manifest entries (`O(manifest entries)`, one streaming read). Revisit only if profiles show it, as a later separately-modeled optimization.
5. **Block-run details (OQ5).** Defaults: `block_size` target **256 KiB**, hard cap **1 MiB**; per-`DataBlock` **CRC32C** checksum; sparse footer index = one `(min_key, max_key, block_offset)` per block; **compression off by default** (hashes are high-entropy); hashes stored fixed-width; key schemas fixed per `kind` (`blob_hash`; `(blob_hash, source_id)`; `(target_shard, blob_hash)`); encoding is deterministic (fixed block boundaries, no nondeterministic compression) so a write-once run is byte-reproducible for resume/adoption.
6. **Sweep-eligibility encoding (OQ6).** `writer_instance_id = "<stable_server_id>:<process_epoch>"`, where `process_epoch` is a durable monotone per-process-incarnation counter. A build prefix is sweep-eligible iff: an explicit retired-epoch sentinel is present, **or** the same epoch's durable watermark has `min_active > build_sequence`, **or** the writer incarnation has been replaced. **Never** a frozen-seq / judged-dead heuristic alone.
7. **Backpressure thresholds (OQ7).** Defaults (config-overridable, enforced fail-closed *before* publishing any owner transition): `manifest_entries ≤ 1048576`; `manifest_encoded_bytes ≤ 256 MiB`; `manifest_inline_bytes_total ≤ 16 MiB`; `largest_inline_entry_bytes ≤ 1 MiB`; `blob_delta_bytes_per_generation` soft-cap **1 GiB** then roll a new generation segment.
8. **fsck classification (OQ8).** Deferred to a Phase-1d follow-up task (not a phase blocker). Phase 1 ships the sweep; fsck then gains a read-only audit that flags an *owner-visible* missing manifest body as an **error** and a reclaimable *pre-precommit* manifest body as **info**, using the same sealed-owner-view + eligibility rule as the sweep.

---

## Execution Model & Gates {#execution-model-gates}

**Workflow.** Subagent-driven-development: a fresh implementer subagent per task, then spec-compliance review, then code-quality review, before the next task. The TLA+ gate tasks come first in each behavior-changing phase.

**Autonomy (user decision: *autonomous through green gates*).** The worker proceeds **task→task and phase→phase without stopping**, as long as:
1. the phase's **TLA+ gate is green** (defined below) before any code task in that phase, and
2. the **full `Cas*`/`Ca*` gtest sweep passes** at each phase exit, and
3. the build is clean.

It **stops and surfaces** only on: a TLA+ gate that cannot be made green, a failing/flaky test it cannot resolve, a build break it cannot fix, or genuine spec ambiguity. ("Should I continue?" check-ins are not wanted between green steps.)

**TLA+ gate (R0), per phase.** The phase's first task(s) author or extend the model + its `.cfg` suite. The gate is **green** iff:
- every `stage*` / `_fixed` / `_safe` / liveness-fix config reports `No error`, **and**
- every `_sab_*` / `_buggy` config reports its **expected** invariant violation (an `UNEXPECTED PASS` fails the gate).

No code task in a phase starts before that phase's gate is green.

**Phase exit (behavior-changing phases).** Full `Cas*`/`Ca*` gtest sweep green, then a chaos soak per the `cas-test-triage` / ca-soak procedure with periodic reports (see `reference_ca_soak_fresh_restart`). A phase that only adds an optimization still runs the gtest sweep; the soak is run at the end of Phase 1d (first behavior switch) and again after Phase 4 (sharding).

**Ordering & dependencies.**
```
Phase 0 ──gates──> 1a ──> 1b ┐
                      └──> 1c ├──(together = behavior switch)──> 1d ──> [soak] ──> 2 ──> 3 ──> 4 ──> [soak] ──> 5
                              ┘
```
- **Phase 0** gates everything (the model must hold + every negative control must break).
- **Phase 1a** (formats/identity/layout) gates 1b, 1c, 1d.
- **Phase 1b + 1c + 1d** are the non-behavior-preserving switch; 1d removes the old tree/snap/cascade machinery and is the first point the GC accounting changes.
- **Phases 2, 3** optimize discovery and trim (only discovery and trim are lazy; the fence stays global); each needs a Phase-0-model extension proving the laziness is safe before its code.
- **Phase 4** enables `gc_shards > 1`; needs the sharded-reducer model extension.
- **Phase 5** is optional and last; needs the retire-token model extension.

---

## Cross-Phase File Map {#cross-phase-file-map}

*New files are design proposals; modified files' exact current locations and signatures are confirmed inside each phase plan from the C++ ground-truth report. All paths are under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless noted — abbreviated `CA/` below.*

**TLA+ (Phase 0)**
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_*.cfg` (stage + `_sab_*` + `_fixed`/`_buggy` + liveness/witness configs)
- Create: `docs/superpowers/models/run_gc_partmanifest.sh` (wrapper)

**Phase 1a — identity, codecs, layout**
- Create: `CA/Core/CasManifestId.h` — `ManifestRef`, `ManifestId`, ordering/hash, `<aa>` derivation.
- Create: `CA/Core/CasManifestCodec.h` / `.cpp` — `PartManifestProto` + `ManifestEntry` encode/decode; block-framed `entries`.
- Create: `CA/Core/CasRunFile.h` / `.cpp` — `RunFile`/`DataBlock`/`RunFooter` writer, reader, range-seek, k-way streaming merge.
- Modify: `CA/Core/CasLayout.*` (or the file that owns `blobKey`/`treeKey`/`checkNamespace`) — add `manifestKey(ManifestId)`; reserve `_manifests` in `checkNamespace`.

**Phase 1b — build / precommit / promote**
- Modify: `CA/Core/CasBuild.*` — `stageTree`→mint `ManifestId` + stream-write body; `PrecommitAdd` in target root; atomic `PromotePrecommit`; writer best-effort `_manifests` debris cleanup; caps + fail-closed.
- Modify: `CA/Core/CasRootShardCodec.*` — `RootShard{shard_version, fence_round, refs, journal}` carrying `RootRef{ref_name, manifest_ref, mutable_files, published_at_ms}` plus **one ordered** `std::vector<RootOwnerEvent> journal` (folded in `transition_version` order). A `RootOwnerEvent{transition_version, old_binding?, new_binding?}` carries `OwnerBinding{OwnerKind owner_kind; String ref_name; UInt128 build_id; ManifestRef manifest_ref}` where `OwnerKind` is `Committed` (ref_name set, build_id = 0) or `Precommit` (ref_name = final_ref_name, build_id set). Every owner change — create/abandon precommit, publish/drop/repoint committed, **promote** — is one `RootOwnerEvent`; a promote is `old {Precommit,…,T}` / `new {Committed,…,T}` (same `manifest_ref` T, an owner move with blob Δ = 0). Remove `JournalRecord.closure`/`ClosureNodeProto`.
- Modify: `CA/ContentAddressedTransaction.cpp` — `republishRef`→publish a fresh destination manifest over the same blob hashes, then drop the source ref; `Atomic` rename stays a CA no-op.

**Phase 1c — read path**
- Modify: `CA/Core/CasStore.*` — `resolveRef`→`ManifestRef` + namespace + mutable payload; `readTree`→read part manifest via `CasLayout`, enforce `RefMatchesBody`/`ManifestNamespaceMatches`, serve path lookup + optional dir index; cache by `(ManifestId, token)`; fail-closed on a committed ref naming a missing manifest.

**Phase 1d — GC fold / in-degree / retire / seal / orphan sweep + removals**
- Create: `CA/Core/CasGenerationSeal.h` / `.cpp` — the two write-once phase seals, `CasFoldSeal` and `CasCompletionSeal`, encode/decode + coverage fields. `CasFoldSeal` (key `gc/gen/<gen>/fold_seal`, `FormatId::FoldSeal` magic "CAFS"): `{generation, parent_generation, per_ns_shard: map<String,ShardCoverage>, blob_target_runs: RunRef[], part_manifest_cleanup: RunRef[]}` where `ShardCoverage{uint8_t classification; Token folded_token; uint64_t folded_cursor}`. `CasCompletionSeal` (key `gc/gen/<gen>/completion_seal`, `FormatId::CompletionSeal` magic "CACS"): `{generation, fence_positions: map<String,uint64_t>, delete_outcomes: RunRef[], trim_cursors: map<String,uint64_t>, bool adoptable}`. Resume: completion_seal ⇒ done; else fold_seal ⇒ resume at recheck; else redo fold. (The old single `CasGenerationSeal` / "CAGN" is gone.)
- Create: `CA/Core/CasBlobInDegree.h` / `.cpp` — streaming in-degree run merge over `CasRunFile`.
- Create: `CA/Core/CasOrphanManifestSweep.h` / `.cpp` — per-namespace pre-precommit debris sweep.
- Modify: `CA/Core/CasGc.*` — `fold` owner transitions→blob deltas; remove cascade; build the streaming in-degree generation; retire; seal; wire the sweep. Keep `gc_shards = 1`, all-shard fence, per-candidate `HEAD`.
- Modify: `CA/Core/CasGcFormats.*` — generation/retired/outcome formats for the new model.
- Delete: `CA/Core/CasGcSnap.*`, `CA/Core/CasTreeCodec.*`, `CA/Core/CasClosureWalk.cpp` (+ header) — tree/snap/closure machinery gone.
- Modify (follow-up task): `CA/Core/CasFsck.*` — the OQ8 read-only manifest audit.

**Phase 2** — Modify `CA/Core/CasGc.*` (discovery), root-shard token persistence (likely `CasRootShardCodec`/`CasGcFormats`), LIST probes (backend interface `CasObjectStorageBackend`/`CasBackend`).

**Phase 3** — Modify `CA/Core/CasGc.*` (lazy trim; the fence stays global) + `CasFoldSeal`/`CasCompletionSeal` (coverage for skipped shards).

**Phase 4** — Modify `CA/Core/CasGc.*` + new `CA/Core/CasGcShardPlan.h`/`.cpp` (mapper scatter / reducer ownership / coordinator); `CasGcScheduler.*` for replica shard ownership; config `gc_shards`.

**Phase 5** — Modify `CA/Core/CasGc.*` retire (token-source) + `CasGcFormats`.

---

## Phase Index {#phase-index}

| Phase | Plan file (`docs/superpowers/plans/…`) | Scope | Gate before code | Depends on |
|---|---|---|---|---|
| 0 | `2026-06-26-cas-gc-phase0-tla-model.md` | `CaGcRootLocalPartManifestCore.tla` + cfg suite (stages hold, 22 negative controls break); format/identity/layout skeleton definitions | — (this **is** the gate) | — |
| 1a | `2026-06-26-cas-gc-phase1a-identity-and-codecs.md` | `ManifestRef`/`ManifestId`, `PartManifestProto` codec, `RunFile` dense runs, `CasLayout` manifest key + `_manifests` reservation | Phase 0 green | 0 |
| 1b | `2026-06-26-cas-gc-phase1b-build-precommit-promote.md` | mint `ManifestId`, write body, `PrecommitAdd` in target root, atomic `PromotePrecommit`, owner transitions, `republishRef`, writer debris cleanup | Phase 0 green | 1a |
| 1c | `2026-06-26-cas-gc-phase1c-read-path.md` | `resolveRef`/`readTree` over part manifests, `RefMatchesBody`/`ManifestNamespaceMatches`, path lookup, cache | Phase 0 green | 1a |
| 1d | `2026-06-26-cas-gc-phase1d-gc-fold-indegree-sweep.md` | fold→blob deltas, streaming in-degree generation, `CasFoldSeal`/`CasCompletionSeal`, retire, orphan sweep; delete snap/trees/closure/cascade | Phase 0 green | 1a, 1b, 1c |
| 2 | `2026-06-26-cas-gc-phase2-token-diff-discovery.md` | LIST token probes, persisted folded tokens+cursors, safe skip of unchanged shards | Phase-0 model ext. (skip-safety) green | 1d |
| 3 | `2026-06-26-cas-gc-phase3-lazy-fence-trim.md` | model-proven lazy trim below sealed coverage (the fence stays global) | Phase-0 model ext. (lazy-trim) green | 2 |
| 4 | `2026-06-26-cas-gc-phase4-target-sharded-reducers.md` | `gc_shards > 1`: mappers scatter by blob hash, disjoint reducers, coordinator, two-replica concurrency | Phase-0 model ext. (sharded reducers) green | 3 |
| 5 | `2026-06-26-cas-gc-phase5-retire-token-opt.md` | optional: drop per-candidate `HEAD` after token-source proof | Phase-0 model ext. (retire-token) green | 4 |

Each phase plan is a standalone document with its own Global Constraints, Goal/Architecture, File Structure, Interfaces, and bite-sized `- [ ]` tasks. Execute them in the order above.
