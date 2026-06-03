---
description: Design spec for supporting MergeTree projections on a content-addressed disk (the projection half of backlog B5).
sidebar_label: 'CAS MergeTree projections'
sidebar_position: 4
slug: /superpowers/specs/cas-mergetree-projections
title: 'Content-Addressed MergeTree — Projections Design'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Projections Design {#cas-mergetree-projections}

**Status:** design spec, awaiting review.
**Date:** 2026-06-03.
**Sources:** the M1 integration design (`2026-06-02-cas-mergetree-integration-design.md`), the running backlog (`docs/superpowers/deferred_backlog/cas-mergetree-integration.md`, item **B5**), and a read-only investigation of how MergeTree projections touch the per-part storage layer (`IMergeTreeDataPart::loadProjections`, `DataPartStorageOnDiskFull::getProjection`, `MergeTreeDataWriter::writeProjectionPart`, `MergeTask`/`MutateTask` projection handling).

## 1. Goal and scope {#goal-and-scope}

Support **MergeTree projections** on a `content_addressed` disk — the projection half of backlog **B5** (the patch-parts/lightweight-delete half is already done, M7). Projections are currently rejected at `CREATE`/`ATTACH` by `MergeTreeData::checkContentAddressedDiskRestrictions`.

**Scope (agreed: Full):** `CREATE TABLE … (PROJECTION …)`, `INSERT`, projection-optimized `SELECT`, background **merge** (projections rebuilt), and `ALTER TABLE … ADD`/`DROP`/`MATERIALIZE PROJECTION` (plus mutation carry-forward of unaffected projection files). The success criterion is the ~40 projection stateless tests currently tagged `no-content-addressed-storage` passing under the content-addressed default config, with no S3/pool leftovers.

**Out of scope:** the original B5 framing of "nested sub-manifests / a second active-set namespace" (see §3 — not needed); the FREEZE/BACKUP interactions of projections (those ride on B4/B34, still gated); replication (B1).

## 2. Key finding (why this is small) {#key-finding}

A projection is **not** a separate part with its own identity. On disk it lives at `<part>/<projection_name>.proj/<files>`, and `DataPartStorageOnDiskFull::getProjection(name)` hands the projection a **child** `IDataPartStorage` whose `root_path` is the parent part dir and whose `part_dir` is `<name>.proj`, **sharing the parent part's transaction** (for non-temporary projections). Therefore every projection file write/read resolves to the path `<uuid>/<part>/<proj>.proj/<file>`, which `ContentAddressed::parsePartFilePath` already parses to `part_name = <part>`, `file = <proj>.proj/<file>` — a **nested key in the parent part's single `PartManifest`**.

This is the **same flat-manifest-with-nested-keys** generalization already shipped twice: table-level subdirectories (`deduplication_logs/…`, B37) and the detached-part namespace (`detached/<part>/<file>`, B36). Projections need the same treatment, not a new structure.

Two consequences fall out for free:
- **Merges rebuild projections** (no hardlink carry-forward), so the merged part's manifest simply gains fresh projection blobs — no special carry-forward logic for the merge path.
- The content-addressed **`part_id` (a hash over the part's file→checksum set, excluding the mutable per-part files) naturally includes the projection blobs**, so two parts with identical base data but different projections get distinct `part_id`s (no false dedup), and ADD/DROP/MATERIALIZE yields a new part version. This is the correct semantics, and it is automatic.

## 3. Approach decision {#approach-decision}

**Chosen: A — flat manifest with nested projection keys.** Store projection files as nested keys (`<proj>.proj/<file>`) in the parent part's existing `PartManifest`. No manifest format change, no version bump, no new namespace, no per-projection `part_id`.

Rejected alternatives:
- **B — nested sub-manifests** (each projection its own sub-`PartManifest` + `part_id`, referenced from a `projections` footer section): a format/version change and a recursive resolver, for identity projections never use (they are always read as children and rebuilt on merge). YAGNI.
- **C — projections as separate refs** (a `projections/` active-set namespace lifecycle-linked to the parent): a second namespace + GC coupling, for no benefit — projections are cloned/detached/dropped with their parent.

## 4. Architecture and components {#architecture}

### 4.1 The model {#model}

One part ⇒ one `PartManifest` whose `blobs` map holds **both** top-level files (`columns.txt`, `data.bin`, `checksums.txt`, …) and nested projection files (`p_sum.proj/data.bin`, `p_sum.proj/columns.txt`, `p_sum.proj/checksums.txt`, …). The mutable per-part files (`uuid.txt`, `txn_version.txt`, `metadata_version.txt`) remain top-level and excluded from the `part_id` exactly as today; a projection never carries those, so the existing `isMutablePerPartFile` basename check is unaffected.

### 4.2 Metadata-storage projection-subdirectory awareness (the core change) {#metadata-storage}

`ContentAddressedMetadataStorage` gains a projection-subdirectory branch in two methods, mirroring the existing single-detached-part-dir branches:

- **`existsDirectory("<uuid>/<part>/<proj>.proj")`** → `true` iff the part's manifest (or per-ref sidecar) carries any key with prefix `<proj>.proj/`. Today this returns `false`, so `IMergeTreeDataPart::loadProjections` (which calls `existsDirectory(<name>.proj)` per metadata projection) never discovers a projection. This is the single change that makes projections visible.
- **`listDirectory("<uuid>/<part>/<proj>.proj")`** → the projection's inner file names, stripping the `<proj>.proj/` prefix (e.g. `data.bin`, `columns.txt`, `checksums.txt`). This lets the projection's child storage enumerate and read its files.
- **`listDirectory("<uuid>/<part>")`** (the parent part dir) → collapse every `<proj>.proj/*` key to a **single `<proj>.proj` directory entry** (first-component collapse) listed alongside the top-level files. This keeps `iterate()`-based discovery (e.g. `checkDataPart`) seeing the `.proj` subdir as a directory rather than a flood of nested files, and keeps the top-level column listing clean.

No change is required to `writeFile`, `getStorageObjects`, `existsFile`, `getFileSize`, or `createHardLink`: each keys the manifest by the full post-part-dir path string, which is already the nested key for a projection file.

### 4.3 Write path {#write-path}

- **INSERT:** `MergeTreeDataWriter::writeProjectionPart` builds the projection via the parent's `getProjectionPartBuilder`, whose child storage shares the parent transaction (non-temp). Each projection file write is content-addressed and recorded in the parent manifest; the parent part's single commit publishes the whole part (top-level + projection blobs). Works once §4.2 is in place.
- **Merge:** `MergeTask` rebuilds projections into the merged part; the merged part's child storage writes fresh projection files into the merged manifest. No carry-forward.
- **Temp projections (`.tmp_proj`):** used by projection MATERIALIZE-merges, written under `<part>/<proj>.tmp_proj/` and then renamed to `<proj>.proj`. This is the **primary implementation risk** — a temp projection built in a *separate* transaction would otherwise publish a colliding ref for the parent part name. The plan must ensure projection files land in the parent's transaction/manifest and handle the `.tmp_proj`→`.proj` re-key on CA (analogous to the detached-staging→active `moveDirectory` branch, da/6e0…). Phase 3 nails this.

### 4.4 ALTER (ADD / DROP / MATERIALIZE PROJECTION) {#alter}

`supportsHardLinks()` already returns `true` for CA (M7), so once the CREATE/ATTACH gate is lifted (§4.5) these run as mutations through `MutateTask`'s whole-part transaction:
- **ADD / MATERIALIZE PROJECTION:** carry forward unchanged top-level columns and unaffected projection files via `createHardLink("<proj>.proj/<file>")` (re-ref by checksum — the new manifest entry points at the same blob; no re-upload), build the target projection fresh, commit a new part version. The `part_id` changes (the manifest gained keys) ⇒ a new ref.
- **DROP PROJECTION:** the new part version's manifest simply omits the `<proj>.proj/*` keys.

`createHardLink` with a nested key already works (it parses the source/destination, looks up the source blob entry by the nested file name, and re-records it) — the carry-forward path is unchanged from top-level files.

### 4.5 Gate lift {#gate-lift}

Remove the projection rejection in `MergeTreeData::checkContentAddressedDiskRestrictions`. With `supportsHardLinks` already `true`, no other gate blocks ADD/DROP/MATERIALIZE. The method's remaining responsibilities (it currently gates only projections) shrink to a no-op for CA, or the method is removed if nothing else uses it — to be decided in the plan after confirming no other caller relies on it.

## 5. Error handling and fail-close {#error-handling}

- A missing ref or blob fail-closes exactly as today (`FILE_DOESNT_EXIST` / `CORRUPTED_DATA`), never a silent empty result.
- A **broken projection** is a MergeTree-level concept: MergeTree marks the projection broken and the query falls back to the base table. CA introduces no special handling — it resolves what the manifest holds; a projection whose files are not in the manifest is simply "not discovered" (`existsDirectory` returns `false`), which MergeTree already tolerates as "this part has no such projection". The content-addressed `part_id` only ever covers blobs that were actually committed, so there is no skew between "what the manifest claims" and "what exists".

## 6. Testing {#testing}

- **Unit (gtest, `gtest_content_addressed_metadata.cpp`):** projection-subdirectory `existsDirectory`/`listDirectory`; parent-part listing collapses `<proj>.proj/*` to one `<proj>.proj` entry; a nested-key round-trip — write a part whose manifest holds projection files as nested keys, read them back via the projection child path, list the projection dir (inner files), list the parent (shows `<proj>.proj` + top-level), and confirm the `part_id` differs from the same base data without the projection.
- **Stateless (new, inline CA disk so it exercises CA on any config):** `CREATE` a table with a projection on an inline `content_addressed` disk; `INSERT`; a projection-optimized `SELECT` whose result matches a base-table oracle; `OPTIMIZE … FINAL` (merge) and re-check; `ALTER … ADD PROJECTION`, `MATERIALIZE PROJECTION`, `DROP PROJECTION`, each re-checked; `DETACH`/`ATTACH` to prove the projection persists; and a no-leftovers assertion (DROP TABLE reclaims the projection blobs).
- **Regression suite:** un-gate the ~40 projection stateless tests tagged `no-content-addressed-storage`, run under the content-addressed default config, and triage; re-gate only tests that fail for orthogonal reasons (with a backlog note), exactly as in the dedup-window and partition-clone milestones.

## 7. Plan phasing {#plan-phasing}

One cohesive subsystem ⇒ one spec/plan, internally staged so each stage is independently testable:

1. **Metadata-storage subdir branches + gtests** — `existsDirectory`/`listDirectory` projection-subdirectory awareness and parent-listing collapse, proven by unit tests over a hand-seeded nested-key manifest. No server needed.
2. **INSERT + SELECT + merge on an inline CA disk** — lift the gate, verify a projection round-trips through write/read and survives `OPTIMIZE`/merge; the new stateless test's first half.
3. **ALTER ADD/DROP/MATERIALIZE + mutation carry-forward + `.tmp_proj` handling** — the temp-projection re-key is the risk to nail here; the new stateless test's second half.
4. **Un-gate the projection suite** — run the ~40 tests under CA-default, triage, un-tag passers, re-tag/backlog orthogonal failures.

## 8. Backlog interaction {#backlog}

This closes the **projection half of B5**. The "nested sub-manifests / second active-set namespace" wording in B5 is explicitly superseded by Approach A (flat nested keys); the patch-parts half of B5 is already done (M7). FREEZE/BACKUP of projection-bearing parts remains gated under B4/B34. Any newly-surfaced deferrals (e.g. a temp-projection edge case) are appended to the backlog with a plug-in point, per the project convention.
